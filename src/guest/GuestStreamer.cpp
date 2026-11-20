#include "veyra/guest/GuestStreamer.h"

#include <winsock2.h>
#include <iphlpapi.h>
#include <ws2tcpip.h>
#include <bcrypt.h>

#include <rtc/rtc.hpp>
#include <nlohmann/json.hpp>

#include "GuestAudio.h"
#include "GuestCloudflare.h"
#include "GuestHttp.h"
#include "GuestTunnel.h"
#include "GuestVideo.h"
#include "veyra/guest/GuestAccess.h"
#include "veyra/Log.h"
#include "veyra/sink/RecordingAudioTap.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <ctime>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "bcrypt.lib")

namespace veyra::guest {
using json = nlohmann::json;
namespace {

constexpr int kVideoPt = 102;        // Constrained High, what NVENC sends
constexpr int kVideoPtBaseline = 104; // offered too, for a browser that only takes baseline
constexpr int kAudioPt = 111;
constexpr int kPlayoutDelayId = 6;
constexpr uint32_t kVideoSsrc = 0x42006F01, kAudioSsrc = 0x42006F02;
constexpr size_t kMaxFragment = 1100;  // fits Tailscale's 1280-byte MTU with SRTP and IP headers
constexpr int64_t kConnectTimeoutUs = 20000000;
constexpr int64_t kSilentUs = 3000000;
constexpr int64_t kPadSilentUs = 15000000;  // a pad that has sent no button this long is flagged to the host
constexpr int kTurnTtlSeconds = 4 * 3600;   // TURN credentials last this long; past it, restart the share for a new relay
const char* kStunUrl = "stun:stun.l.google.com:19302";
const char* kPlayoutDelayUri = "http://www.webrtc.org/experiments/rtp-hdrext/playout-delay";

int64_t nowUs() { return sink::recordingClockUs(); }

// Tests: a bottleneck on the way to the guest (VEYRA_TEST_GUEST_LIMIT=<kbps>[@<seconds after the
// guest connects>]). RTP and link-test bytes past it are dropped, as a full router queue would.
struct TestLimit {
    std::mutex m;
    uint32_t kbps = 0;
    int64_t afterUs = 0, fromUs = INT64_MAX, atUs = 0;
    double tokens = 0;
    static std::shared_ptr<TestLimit> fromEnvironment() {
        const char* v = std::getenv("VEYRA_TEST_GUEST_LIMIT");
        if (!v || !*v) return nullptr;
        auto out = std::make_shared<TestLimit>();
        out->kbps = uint32_t(std::strtoul(v, nullptr, 10));
        if (const char* at = std::strchr(v, '@')) out->afterUs = int64_t(std::atof(at + 1) * 1e6);
        return out->kbps ? out : nullptr;
    }
    void armed(int64_t now) { std::lock_guard lock(m); fromUs = now + afterUs; atUs = now; tokens = 0; }
    bool active() { std::lock_guard lock(m); return nowUs() >= fromUs; }
    bool pass(size_t bytes) {
        std::lock_guard lock(m);
        const int64_t now = nowUs();
        if (now < fromUs) return true;
        const double burst = kbps * 1000.0 / 8 * 0.03;   // 30 ms of queue
        tokens = std::min(burst, tokens + double(now - atUs) * kbps / 8000.0);
        atUs = now;
        if (tokens < double(bytes)) return false;
        tokens -= double(bytes);
        return true;
    }
};

class TestLimitHandler final : public rtc::MediaHandler {
public:
    explicit TestLimitHandler(std::shared_ptr<TestLimit> limit) : limit_(std::move(limit)) {}
    void outgoing(rtc::message_vector& messages, const rtc::message_callback&) override {
        std::erase_if(messages, [&](const rtc::message_ptr& m) { return m && !limit_->pass(m->size()); });
    }
    // Retransmissions would leave outside the bottleneck and hide its losses: past it, the
    // guest's generic NACKs are turned into an unknown feedback type (PLIs still pass).
    void incoming(rtc::message_vector& messages, const rtc::message_callback&) override {
        if (!limit_->active()) return;
        for (auto& m : messages) {
            if (!m || m->type != rtc::Message::Control) continue;
            auto* b = reinterpret_cast<uint8_t*>(m->data());
            for (size_t p = 0, n = m->size(); p + 4 <= n; p += ((size_t(b[p + 2]) << 8) | b[p + 3]) * 4 + 4)
                if (b[p + 1] == 205 && (b[p] & 0x1F) == 1) b[p] &= 0xE0;
        }
    }
private:
    std::shared_ptr<TestLimit> limit_;
};

std::string randomToken() {
    uint8_t bytes[18]{};
    BCryptGenRandom(nullptr, bytes, sizeof(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    static const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    for (size_t i = 0; i < sizeof(bytes); i += 3) {
        const uint32_t v = (uint32_t(bytes[i]) << 16) | (uint32_t(bytes[i + 1]) << 8) | bytes[i + 2];
        for (int k = 3; k >= 0; --k) out.push_back(alphabet[(v >> (6 * k)) & 63]);
    }
    return out;
}

bool sameToken(const std::string& a, const std::string& b) {
    if (a.size() != b.size() || a.empty()) return false;
    unsigned diff = 0;
    for (size_t i = 0; i < a.size(); ++i) diff |= unsigned(uint8_t(a[i]) ^ uint8_t(b[i]));
    return diff == 0;
}

// The PC's Tailscale IPv4 address (100.64.0.0/10 on the Tailscale adapter), empty when down.
std::string tailscaleAddress() {
    ULONG size = 32 * 1024;
    std::vector<uint8_t> buffer(size);
    auto* list = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, list, &size) == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(size); list = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
        if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, list, &size) != NO_ERROR) return {};
    }
    for (auto* a = list; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp) continue;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            const auto* in = reinterpret_cast<const sockaddr_in*>(u->Address.lpSockaddr);
            const uint32_t ip = ntohl(in->sin_addr.s_addr);
            if ((ip & 0xFFC00000u) != 0x64400000u) continue;   // 100.64.0.0/10
            char text[INET_ADDRSTRLEN]{};
            inet_ntop(AF_INET, &in->sin_addr, text, sizeof(text));
            return text;
        }
    }
    return {};
}

std::string readFile(const std::string& path) {
    std::ifstream in(std::filesystem::u8path(path), std::ios::binary);
    if (!in) return {};
    std::ostringstream out; out << in.rdbuf();
    return out.str();
}

double median(std::vector<double> v) {
    if (v.empty()) return -1;
    std::nth_element(v.begin(), v.begin() + ptrdiff_t(v.size() / 2), v.end());
    return v[v.size() / 2];
}

const char* modeName(ControlMode m) {
    return m == ControlMode::GuestPlays ? "guest" : m == ControlMode::Together ? "together" : "watch";
}

const char* candidateName(rtc::Candidate::Type t) {
    switch (t) {
    case rtc::Candidate::Type::Host: return "host";
    case rtc::Candidate::Type::ServerReflexive: return "srflx";
    case rtc::Candidate::Type::PeerReflexive: return "prflx";
    case rtc::Candidate::Type::Relayed: return "relay";
    default: return "unknown";
    }
}

const char* reachName(Reach r) { return r == Reach::Tunnel ? "tunnel" : r == Reach::Quick ? "quick" : "tailnet"; }

// The host's own ICE servers: STUN first, then Cloudflare's TURN over UDP (what libjuice relays).
std::vector<rtc::IceServer> hostIceServers(const json& cloudflare) {
    std::vector<rtc::IceServer> out{rtc::IceServer(kStunUrl)};
    for (const auto& server : cloudflare) {
        if (!server.is_object() || !server.contains("username") || !server["username"].is_string() || !server.contains("credential") || !server["credential"].is_string()) continue;
        const auto urls = server.contains("urls") ? (server["urls"].is_array() ? server["urls"] : json::array({server["urls"]})) : json::array();
        for (const auto& url : urls) {
            if (!url.is_string()) continue;
            const std::string u = url.get<std::string>();
            if (!u.starts_with("turn:") || u.find("transport=udp") == std::string::npos) continue;
            const std::string hostPort = u.substr(5, u.find('?') - 5);
            const size_t colon = hostPort.rfind(':');
            if (colon == std::string::npos) continue;
            out.emplace_back(hostPort.substr(0, colon), uint16_t(std::atoi(hostPort.c_str() + colon + 1)),
                             server["username"].get<std::string>(), server["credential"].get<std::string>(), rtc::IceServer::RelayType::TurnUdp);
            return out;
        }
    }
    return out;
}

void initRtcLogOnce() {
    static std::once_flag once;
    std::call_once(once, [] {
        rtc::InitLogger(rtc::LogLevel::Warning, [](rtc::LogLevel level, std::string text) {
            if (level <= rtc::LogLevel::Error) veyra::log::warn("guest-rtc", text);
            else veyra::log::info("guest-rtc", text);
        });
    });
}

} // namespace

// One guest connection. Callbacks hold it weakly: the peer connection owns them.
struct GuestPeer {
    std::string id;
    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::Track> video, audio;
    std::shared_ptr<rtc::RtpPacketizationConfig> videoConfig, audioConfig;
    std::shared_ptr<rtc::DataChannel> input, feedback, stats, probe;
    std::atomic<int> state{int(rtc::PeerConnection::State::New)};
    std::atomic<bool> gathered{false};
    std::atomic<bool> connectedHandled{false};   // set by the control thread once media flow
    std::atomic<bool> retired{false};
    std::atomic<int64_t> heardUs{0};             // last message from the page, any channel
    std::atomic<bool> left{false};               // the page said goodbye (tab closed)
    bool native = false;                         // a BooVI client: extended pad, binary feedback; set before it is shared
    std::string access;                          // the friend's access name, empty for the share link; set before it is shared
    int64_t createdUs = 0;
    std::string remote, candidate;               // written before connectedHandled
    bool relayed = false;
};

struct GuestStreamer::Impl {
    // ---- shared state (m) ----
    mutable std::mutex m;
    std::condition_variable controlCv;
    // ICE gathering is waited on apart from m: the UI thread reads status() meanwhile.
    std::mutex gatherMutex;
    std::condition_variable gatherCv;
    ShareSettings settings;
    bool active = false, stopping = false, kickRequested = false;
    std::string token, tailscaleIp, error;
    json cloudflareIce = json::array();   // short-lived TURN credentials; never logged
    std::shared_ptr<GuestPeer> peer;
    SourceLimiter limiter;
    std::atomic<AccessDesk*> desk{nullptr};
    std::atomic<ControlMode> mode{ControlMode::GuestPlays};
    std::shared_ptr<TestLimit> testLimit;

    // ---- what the guest receives (qualityMutex) ----
    std::mutex qualityMutex;
    RateControl rate;
    Quality applied{Feed::Real, Size::P1080, 0};
    double linkRtt = -1, linkLoss = -1;

    // ---- the link test ----
    std::mutex probeMutex;   // starting and joining probeThread
    std::thread probeThread;
    std::atomic<bool> probing{false}, probeStop{false};
    std::atomic<bool> guestLive{false};
    std::thread control, turnFetch;

    // ---- the guest's pad (inputMutex), written by the input channel, read every 1 ms ----
    std::mutex inputMutex;
    GuestPad latest;
    int64_t latestRecvUs = 0;
    bool haveInput = false;
    // The oldest pad change the PS5 has not been handed yet. A repeat of the same pad (the
    // BooVI client's 8 ms keepalive, the page's 50 ms one) is no input: the PS5 has that state.
    bool changePending = false;
    uint16_t changeSeq = 0;
    int64_t changeRecvUs = 0;
    uint64_t inputs = 0;
    // This guest's real pad: when it first showed up, and whether any button or trigger has come
    // through since (a pad the browser misreads sends states with no button ever).
    int64_t padSinceUs = 0;
    bool buttonSeen = false;
    int64_t activeUs = 0;    // the pad last changed: a button, a trigger, a stick past a small dead zone
    struct Ack { uint16_t seq; int64_t recvUs, handedUs; };
    std::vector<Ack> acks;
    remoteplay::ControllerState lastHanded;
    int64_t lastHandedUs = 0;
    std::vector<double> recvToHandedMs;
    uint8_t rumbleL = 0, rumbleR = 0;

    // ---- statistics (statsMutex) ----
    std::mutex statsMutex;
    uint64_t framesSent = 0, bytesSent = 0, framesWindow = 0, bytesWindow = 0;
    std::vector<double> presentToSent, encode, decodedToSent, decodedReal, decodedSmooth;
    double windowPresentToSent = -1, windowEncode = -1, windowDecodedToSent = -1, fps = 0, mbps = 0;
    double lastDecodedReal = -1, lastDecodedSmooth = -1;   // kept across feed changes
    json report;                        // the page's last latency report

    // Last: destroyed first, so no thread of theirs outlives the state above.
    GuestTunnel tunnel;
    GuestHttp http;
    GuestAudio audio{[this](const std::vector<uint8_t>& packet, uint32_t ts) { sendAudio(packet, ts); }};
    GuestVideo video{[this](EncodedFrame&& frame) { sendVideo(std::move(frame)); }};

    std::shared_ptr<GuestPeer> currentPeer() { std::lock_guard lock(m); return peer; }

    // ---- media (encoder threads) ----
    void sendVideo(EncodedFrame&& frame) {
        auto p = currentPeer();
        if (!p || !p->connectedHandled || !p->video || !p->video->isOpen()) return;
        const uint32_t ts = uint32_t(uint64_t(frame.captureUs) * 9 / 100);
        try { p->video->sendFrame(reinterpret_cast<const std::byte*>(frame.data.data()), frame.data.size(), rtc::FrameInfo(ts)); }
        catch (const std::exception& e) { veyra::log::warn("guest-rtc", std::string("video send: ") + e.what()); return; }
        const int64_t sent = nowUs();
        {
            std::lock_guard lock(statsMutex);
            ++framesSent; ++framesWindow; bytesSent += frame.data.size(); bytesWindow += frame.data.size();
            presentToSent.push_back(double(sent - frame.captureUs) / 1000.0);
            encode.push_back(double(frame.encodedUs - frame.captureUs) / 1000.0);
            if (frame.sourceUs > 0) {
                decodedToSent.push_back(double(sent - frame.sourceUs) / 1000.0);
                (frame.presented ? decodedSmooth : decodedReal).push_back(double(sent - frame.sourceUs) / 1000.0);
            }
        }
        if (p->stats && p->stats->isOpen()) {
            const auto text = std::format(R"({{"t":"f","r":{},"c":{},"d":{},"e":{},"s":{},"k":{},"b":{}}})", ts, frame.captureUs, frame.sourceUs, frame.encodedUs, sent, frame.key ? 1 : 0, frame.data.size());
            try { p->stats->send(text); } catch (...) {}
        }
    }
    void sendAudio(const std::vector<uint8_t>& packet, uint32_t ts) {
        auto p = currentPeer();
        if (!p || !p->connectedHandled || !p->audio || !p->audio->isOpen()) return;
        try { p->audio->sendFrame(reinterpret_cast<const std::byte*>(packet.data()), packet.size(), rtc::FrameInfo(ts)); } catch (...) {}
    }

    // ---- signaling (HTTP threads) ----
    HttpResponse handle(const HttpRequest& r) {
        if (r.method == "GET" && (r.path == "/" || r.path == "/index.html")) {
            std::string page;
            { std::lock_guard lock(m); page = readFile(settings.webRoot + "/index.html"); }
            if (page.empty()) return {404, "text/plain", "page missing"};
            return {200, "text/html; charset=utf-8", std::move(page)};
        }
        if (r.method == "GET" && r.path == "/font.ttf") {
            std::string font;
            { std::lock_guard lock(m); font = readFile(settings.fontFile); }
            if (font.empty()) return {404, "text/plain", "font missing"};
            return {200, "font/ttf", std::move(font)};
        }
        if (r.method == "POST" && (r.path == "/session" || r.path == "/answer")) {
            if (!limiter.admit(r.source, nowUs())) return {429, "application/json", R"({"error":"trop d'essais, attends une minute","state":"locked"})"};
            return r.path == "/session" ? session(r.body, r.source) : answer(r.body, r.source);
        }
        return {404, "text/plain", "not found"};
    }
    // A wrong credential counts against its source address only (SourceLimiter).
    void refused(const std::string& source, const char* what) {
        const bool locked = limiter.failed(source, nowUs());
        // The address without its last part: enough to tell sources apart, not to name anyone.
        const size_t cut = source.find_last_of(".:");
        veyra::log::warn("guest", std::format("request with a wrong {} refused from {}x{}", what, source.substr(0, cut == std::string::npos ? 0 : cut + 1),
                                              locked ? "; that address waits a minute" : ""));
    }
    bool tokenOk(const std::string& candidate, const std::string& source) {
        {
            std::lock_guard lock(m);
            if (active && !stopping && sameToken(candidate, token)) return true;
        }
        refused(source, "token");
        return false;
    }
    // A friend's access. Knocking (/session): the visit's state until the host's side lets it in,
    // then the access name. /answer: the name while the visit is in. Empty, with `out` set, when
    // the request ends here.
    std::optional<std::string> accessOk(const std::string& candidate, const std::string& source, bool knock, HttpResponse& out) {
        static const std::string invalid = R"({"error":"lien invalide ou révoqué","state":"invalid"})";
        AccessDesk* d = desk.load();
        if (d && !knock) {
            if (auto name = d->admitted(candidate)) return name;
        } else if (d) {
            SYSTEMTIME local{};
            GetLocalTime(&local);
            const auto k = d->knock(candidate, nowUs(), int64_t(std::time(nullptr)), local.wHour * 60 + local.wMinute);
            if (k.admitted) return k.name;
            if (!k.invalid) {
                out = {k.status, "application/json", json{{"state", k.state}, {"host", k.host}, {"name", k.name}, {"detail", k.detail}}.dump()};
                return std::nullopt;
            }
        }
        refused(source, "access");
        out = {403, "application/json", invalid};
        return std::nullopt;
    }
    HttpResponse session(const std::string& body, const std::string& source) {
        json in = json::parse(body, nullptr, false);
        const bool byAccess = in.is_object() && in.contains("access") && in["access"].is_string();
        if (!in.is_object() || (!byAccess && (!in.contains("token") || !in["token"].is_string()))) return {400, "text/plain", "bad request"};
        std::string access;
        if (byAccess) {
            HttpResponse wait;
            auto name = accessOk(in["access"].get<std::string>(), source, true, wait);
            if (!name) return wait;
            access = *name;
        } else if (!tokenOk(in["token"].get<std::string>(), source)) return {403, "application/json", R"({"error":"lien invalide ou expiré"})"};
        // A BooVI client says so; it then gets real pictures only and sends the extended pad.
        const bool native = in.contains("client") && in["client"] == "boovi";
        auto p = createPeer(native, access);
        if (!p) return {503, "application/json", R"({"error":"impossible de préparer la connexion"})"};
        {
            std::unique_lock lock(gatherMutex);
            gatherCv.wait_for(lock, std::chrono::seconds(3), [&] { return p->gathered.load() || p->retired.load(); });
        }
        if (currentPeer() != p) return {409, "application/json", R"({"error":"une autre connexion a pris la place"})"};
        const auto local = p->pc->localDescription();
        if (!local) return {503, "application/json", R"({"error":"pas d'offre"})"};
        json out{{"id", p->id}, {"sdp", std::string(*local)}, {"mode", modeName(mode.load())}, {"input", kExtendedVersion}};
        if (AccessDesk* d = desk.load(); d && !access.empty()) out["host"] = d->hostName();
        {
            // STUN first: the browser keeps a direct path when there is one, the relay is the fallback.
            std::lock_guard lock(m);
            if (settings.reach != Reach::Tailnet) {
                out["ice"] = json::array({json{{"urls", kStunUrl}}});
                for (const auto& server : cloudflareIce) out["ice"].push_back(server);
            } else out["ice"] = json::array();
        }
        return {200, "application/json", out.dump()};
    }
    HttpResponse answer(const std::string& body, const std::string& source) {
        json in = json::parse(body, nullptr, false);
        const bool byAccess = in.is_object() && in.contains("access") && in["access"].is_string();
        if (!in.is_object() || (!byAccess && (!in.contains("token") || !in["token"].is_string())) || !in.contains("id") || !in.contains("sdp") || !in["sdp"].is_string() || !in["id"].is_string())
            return {400, "text/plain", "bad request"};
        if (byAccess) {
            HttpResponse refusedOut;
            if (!accessOk(in["access"].get<std::string>(), source, false, refusedOut)) return refusedOut;
        } else if (!tokenOk(in["token"].get<std::string>(), source)) return {403, "application/json", R"({"error":"lien invalide ou expiré"})"};
        auto p = currentPeer();
        if (!p || p->id != in["id"].get<std::string>()) return {409, "application/json", R"({"error":"connexion remplacée"})"};
        const std::string sdp = in["sdp"].get<std::string>();
        try {
            rtc::Description remote(sdp, rtc::Description::Type::Answer);
            int pt = 0;
            for (int i = 0; i < remote.mediaCount(); ++i) {
                auto entry = remote.media(i);
                if (auto media = std::get_if<rtc::Description::Media*>(&entry); media && *media && (*media)->type() == "video") {
                    if ((*media)->hasPayloadType(kVideoPt)) pt = kVideoPt;
                    else if ((*media)->hasPayloadType(kVideoPtBaseline)) pt = kVideoPtBaseline;
                }
            }
            if (!pt) return {400, "application/json", R"({"error":"ce navigateur ne décode pas le h.264"})"};
            p->videoConfig->payloadType = uint8_t(pt);
            p->pc->setRemoteDescription(remote);
            veyra::log::info("guest", std::format("answer accepted peer={} videoPt={}", p->id, pt));
        } catch (const std::exception& e) {
            veyra::log::warn("guest", std::string("answer refused: ") + e.what());
            return {400, "application/json", R"({"error":"réponse invalide"})"};
        }
        return {204, "text/plain", ""};
    }

    std::shared_ptr<GuestPeer> createPeer(bool native, const std::string& access) {
        initRtcLogOnce();
        ShareSettings s; std::string bindIp; json ice;
        {
            std::lock_guard lock(m);
            s = settings; bindIp = tailscaleIp; ice = cloudflareIce;
        }
        rtc::Configuration config;
        if (s.reach != Reach::Tailnet) config.iceServers = hostIceServers(ice);
        else config.bindAddress = bindIp.empty() ? std::string("127.0.0.1") : bindIp;
        config.mtu = 1280;
        config.disableAutoNegotiation = true;
        auto p = std::make_shared<GuestPeer>();
        p->id = randomToken().substr(0, 12);
        p->native = native;
        p->access = access;
        p->createdUs = nowUs();
        try {
            p->pc = std::make_shared<rtc::PeerConnection>(config);
            std::weak_ptr<GuestPeer> weak = p;
            p->pc->onStateChange([this, weak](rtc::PeerConnection::State state) {
                if (auto self = weak.lock()) { self->state = int(state); controlCv.notify_all(); }
            });
            p->pc->onGatheringStateChange([this, weak](rtc::PeerConnection::GatheringState state) {
                if (state != rtc::PeerConnection::GatheringState::Complete) return;
                if (auto self = weak.lock()) { self->gathered = true; std::lock_guard lock(gatherMutex); gatherCv.notify_all(); }
            });

            rtc::Description::Video videoMedia("video", rtc::Description::Direction::SendOnly);
            // Level 5.2: 1440p at 120 pictures a second (NVENC picks the stream's own level).
            videoMedia.addH264Codec(kVideoPt, "profile-level-id=640c34;packetization-mode=1;level-asymmetry-allowed=1");
            videoMedia.addH264Codec(kVideoPtBaseline, "profile-level-id=42e034;packetization-mode=1;level-asymmetry-allowed=1");
            videoMedia.addSSRC(kVideoSsrc, "boovi", "boovi-video", "boovi-video-track");
            videoMedia.addExtMap(rtc::Description::Entry::ExtMap(kPlayoutDelayId, kPlayoutDelayUri));
            p->video = p->pc->addTrack(videoMedia);
            p->videoConfig = std::make_shared<rtc::RtpPacketizationConfig>(kVideoSsrc, "boovi", uint8_t(kVideoPt), rtc::H264RtpPacketizer::ClockRate);
            p->videoConfig->playoutDelayId = kPlayoutDelayId;
            p->videoConfig->playoutDelayMin = 0;
            p->videoConfig->playoutDelayMax = 0;
            auto packetizer = std::make_shared<rtc::H264RtpPacketizer>(rtc::NalUnit::Separator::StartSequence, p->videoConfig, kMaxFragment);
            packetizer->addToChain(std::make_shared<rtc::RtcpSrReporter>(p->videoConfig));
            packetizer->addToChain(std::make_shared<rtc::RtcpNackResponder>(2048));
            packetizer->addToChain(std::make_shared<rtc::PliHandler>([this] {
                veyra::log::info("guest", "keyframe requested by the guest (PLI/FIR)");
                video.requestKeyframe();
            }));
            // libdatachannel's pacer drains on a thread-pool timer; opt in to compare (it is
            // off by default: a whole frame then leaves at once, which the measurements favour).
            if (const char* pace = std::getenv("VEYRA_GUEST_PACING"); pace && *pace == '1')
                packetizer->addToChain(std::make_shared<rtc::PacingHandler>(double(s.limit.kbps) * 1000.0 * 4, std::chrono::milliseconds(1)));
            if (testLimit) packetizer->addToChain(std::make_shared<TestLimitHandler>(testLimit));
            p->video->setMediaHandler(packetizer);

            // Audio in a stream of its own: the browser then never delays the picture to
            // line it up with the sound.
            rtc::Description::Audio audioMedia("audio", rtc::Description::Direction::SendOnly);
            audioMedia.addOpusCodec(kAudioPt, "minptime=10;useinbandfec=1;stereo=1;sprop-stereo=1");
            audioMedia.addSSRC(kAudioSsrc, "boovi", "boovi-audio", "boovi-audio-track");
            p->audio = p->pc->addTrack(audioMedia);
            p->audioConfig = std::make_shared<rtc::RtpPacketizationConfig>(kAudioSsrc, "boovi", uint8_t(kAudioPt), rtc::OpusRtpPacketizer::DefaultClockRate);
            auto opus = std::make_shared<rtc::OpusRtpPacketizer>(p->audioConfig);
            opus->addToChain(std::make_shared<rtc::RtcpSrReporter>(p->audioConfig));
            p->audio->setMediaHandler(opus);

            rtc::DataChannelInit unreliable;
            unreliable.reliability.unordered = true;
            unreliable.reliability.maxRetransmits = 0;
            p->input = p->pc->createDataChannel("input", unreliable);
            p->feedback = p->pc->createDataChannel("feedback");
            p->stats = p->pc->createDataChannel("stats");
            p->probe = p->pc->createDataChannel("probe", unreliable);
            p->input->onMessage([this, weak](rtc::message_variant data) {
                const auto* bytes = std::get_if<rtc::binary>(&data);
                if (!bytes) return;
                const auto pad = parseInputMessage(std::span<const std::byte>(bytes->data(), bytes->size()));
                if (!pad) return;
                const int64_t t = nowUs();
                if (auto self = weak.lock()) self->heardUs = t;
                std::lock_guard lock(inputMutex);
                // Unordered channel: an older state arriving late never overwrites a newer one.
                if (haveInput && int16_t(pad->seq - latest.seq) <= 0) return;
                if (!changePending && (!haveInput || toControllerState(*pad) != toControllerState(latest))) {
                    changePending = true; changeSeq = pad->seq; changeRecvUs = t;
                }
                bool moved = !haveInput || pad->buttons != latest.buttons || pad->l2 != latest.l2 || pad->r2 != latest.r2;
                for (size_t i = 0; i < pad->axes.size(); ++i) moved |= std::abs(int(pad->axes[i]) - int(latest.axes[i])) > 2000;
                if (moved && !pad->synthetic) activeUs = t;
                latest = *pad; latestRecvUs = t; haveInput = true; ++inputs;
                if (pad->connected && !pad->synthetic) {
                    if (!padSinceUs) padSinceUs = t;
                    if (pad->buttons || pad->l2 || pad->r2) buttonSeen = true;
                }
                // Every fourth state is acknowledged on arrival too, so the page measures its
                // input's trip even while no PS5 session reads it (handedUs 0).
                if (pad->seq % 4 == 0 && acks.size() < 256) acks.push_back({pad->seq, t, 0});
            });
            p->stats->onMessage([this, weak](rtc::message_variant data) {
                const auto* text = std::get_if<std::string>(&data);
                auto self = weak.lock();
                if (!text || !self) return;
                const json in = json::parse(*text, nullptr, false);
                if (!in.is_object() || !in.contains("t") || !in["t"].is_string()) return;
                const auto kind = in["t"].get<std::string>();
                self->heardUs = nowUs();
                if (kind == "bye") { self->left = true; controlCv.notify_all(); return; }
                if (kind == "ping" && in.contains("c") && in["c"].is_number()) {
                    const auto reply = std::format(R"({{"t":"pong","c":{},"h":{}}})", in["c"].get<double>(), nowUs());
                    try { self->stats->send(reply); } catch (...) {}
                } else if (kind == "rep") {
                    onReport(in);
                } else if (kind == "want") {
                    onWish(in);
                } else if (kind == "link") {
                    onLink(in);
                } else if (kind == "probe") {
                    startProbe(self);
                } else if (kind == "probe-stop") {
                    probeStop = true;
                }
            });
            p->pc->setLocalDescription(rtc::Description::Type::Offer);
        } catch (const std::exception& e) {
            veyra::log::error("guest", std::string("peer setup failed: ") + e.what());
            return nullptr;
        }
        std::shared_ptr<GuestPeer> previous;
        {
            std::lock_guard lock(m);
            if (!active || stopping) return nullptr;
            previous = std::move(peer);
            peer = p;
        }
        if (previous) retirePeer(previous, "replaced by a new connection");
        veyra::log::info("guest", std::format("peer {} offered bind={} reach={} iceServers={} client={}", p->id, config.bindAddress.value_or("any"), reachName(s.reach), config.iceServers.size(), native ? "boovi" : "browser"));
        controlCv.notify_all();
        return p;
    }

    // Any thread but a libdatachannel callback.
    void retirePeer(const std::shared_ptr<GuestPeer>& p, const char* why) {
        if (!p || p->retired.exchange(true)) return;
        { std::lock_guard lock(gatherMutex); gatherCv.notify_all(); }
        probeStop = true;
        if (p->connectedHandled) {
            guestLive = false;
            video.setEnabled(false);
            audio.stop();
        }
        if (p->stats && p->stats->isOpen()) {
            try { p->stats->send(std::format(R"({{"t":"bye","why":"{}"}})", why)); } catch (...) {}
        }
        try { p->pc->close(); } catch (...) {}
        {
            std::lock_guard lock(inputMutex);
            haveInput = false; changePending = false; acks.clear();
            padSinceUs = 0; buttonSeen = false; activeUs = 0;
        }
        veyra::log::info("guest", std::format("peer {} closed: {}", p->id, why));
    }

    void onReport(const json& in) {
        auto num = [&](const char* key) { return in.contains(key) && in[key].is_number() ? in[key].get<double>() : -1.0; };
        { std::lock_guard lock(statsMutex); report = in; }
        // The page's loss and network time over its last 2 s window (GuestQuality.h).
        { std::lock_guard lock(qualityMutex); rate.report(std::max(0.0, num("loss")), num("net"), nowUs()); }
        applyQuality("report");
        veyra::log::info("guest-report", in.dump());
    }
    // The guest's choice: automatique, or a feed, size and bitrate cap of their own.
    void onWish(const json& in) {
        auto text = [&](const char* key) { return in.contains(key) && in[key].is_string() ? in[key].get<std::string>() : std::string(); };
        Wish wish;
        wish.automatic = !in.contains("auto") || !in["auto"].is_number() || in["auto"].get<int>() != 0;
        wish.feed = parseFeed(text("feed"), Feed::Smooth120);
        wish.size = parseSize(text("size"), Size::Native);
        if (in.contains("mbps") && in["mbps"].is_number()) wish.kbps = uint32_t(std::clamp(in["mbps"].get<double>(), 4.0, 200.0) * 1000);
        { std::lock_guard lock(qualityMutex); rate.setWish(wish, nowUs()); }
        veyra::log::info("guest-quality", std::format("guest wants {} feed={} size={} kbps={}", wish.automatic ? "auto" : "manual", feedName(wish.feed), sizeName(wish.size), wish.kbps));
        applyQuality("guest choice");
    }
    // The page's link test result.
    void onLink(const json& in) {
        auto num = [&](const char* key) { return in.contains(key) && in[key].is_number() ? in[key].get<double>() : -1.0; };
        const double kbps = num("kbps");
        if (kbps <= 0) return;
        {
            std::lock_guard lock(qualityMutex);
            rate.linkTested(uint32_t(std::min(kbps, 2e6)), nowUs());
            linkRtt = num("rtt"); linkLoss = num("loss");
        }
        veyra::log::info("guest-quality", std::format("link test {:.1f} Mb/s rtt={:.1f} ms loss={:.2f}%", kbps / 1000, num("rtt"), std::max(0.0, num("loss")) * 100));
        applyQuality("link test");
    }
    // The encoder follows the controller's mode.
    void applyQuality(const char* why) {
        const auto peerNow = currentPeer();
        const bool nativeClient = peerNow && peerNow->native;
        Quality q;
        {
            std::lock_guard lock(qualityMutex);
            q = rate.current();
            // A BooVI client generates its own frames: it always gets the real pictures.
            if (nativeClient) q.feed = Feed::Real;
            if (q == applied) return;
            const bool modeChanged = q.feed != applied.feed || q.size != applied.size;
            applied = q;
            if (modeChanged) veyra::log::info("guest-quality", std::format("feed={} size={} kbps={} link={} ({})", feedName(q.feed), sizeName(q.size), q.kbps, rate.linkKbps(), why));
            else veyra::log::info("guest", std::format("bitrate {} kbps ({})", q.kbps, why));
        }
        video.setQuality(sizeHeight(q.size), feedFps(q.feed), q.feed != Feed::Real, q.kbps);
        if (auto p = currentPeer(); p && p->connectedHandled) sendStatus(p);
    }

    // ---- the link test: a rising ramp of padding, 300 ms a step, from 4 Mb/s up to 1.5x the
    // host's upload (at most 200 Mb/s). The page measures what arrives and stops it early. ----
    void startProbe(const std::shared_ptr<GuestPeer>& p) {
        std::lock_guard lock(probeMutex);
        if (!p || !p->probe || p->retired || probing.exchange(true)) return;
        if (probeThread.joinable()) probeThread.join();
        probeStop = false;
        const uint32_t top = [&] { std::lock_guard l(m); return std::clamp(settings.limit.kbps * 3 / 2, 20000u, 200000u); }();
        probeThread = std::thread([this, p, top] { runProbe(p, top); probing = false; });
    }
    void runProbe(std::shared_ptr<GuestPeer> p, uint32_t topKbps) {
        SetThreadDescription(GetCurrentThread(), L"Guest link test");
        std::vector<uint32_t> steps;
        for (double r = 4000; r < topKbps; r *= 1.5) steps.push_back(uint32_t(r));
        steps.push_back(topKbps);
        HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        rtc::binary message(1100);
        uint32_t seq = 0;
        uint64_t sentBytes = 0, skipped = 0;
        const int64_t began = nowUs();
        size_t done = 0;
        for (size_t step = 0; step < steps.size() && !probeStop && !p->retired; ++step, ++done) {
            const int64_t start = nowUs();
            uint64_t stepBytes = 0;
            for (;;) {
                const int64_t elapsed = nowUs() - start;
                if (elapsed >= 300000 || probeStop || p->retired) break;
                const uint64_t due = uint64_t(double(steps[step]) * double(elapsed) / 8000.0);
                while (stepBytes < due) {
                    if (p->probe->bufferedAmount() > (1u << 20)) { ++skipped; break; }   // the sender's own limit
                    const double hostMs = double(nowUs()) / 1000.0;
                    const uint16_t count = uint16_t(steps.size());
                    message[0] = std::byte('Q'); message[1] = std::byte(step);
                    std::memcpy(message.data() + 2, &count, 2);
                    std::memcpy(message.data() + 4, &seq, 4);
                    std::memcpy(message.data() + 8, &hostMs, 8);
                    std::memcpy(message.data() + 16, &steps[step], 4);
                    ++seq; stepBytes += message.size();
                    if (testLimit && !testLimit->pass(message.size() + 48)) continue;
                    try { p->probe->send(message); } catch (...) { probeStop = true; break; }
                }
                if (timer) {
                    LARGE_INTEGER due1ms{}; due1ms.QuadPart = -10000;
                    SetWaitableTimer(timer, &due1ms, 0, nullptr, nullptr, FALSE);
                    WaitForSingleObject(timer, 20);
                } else std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            sentBytes += stepBytes;
        }
        if (timer) CloseHandle(timer);
        if (p->stats && p->stats->isOpen()) {
            try { p->stats->send(std::format(R"({{"t":"probe-end","n":{},"steps":{}}})", seq, done)); } catch (...) {}
        }
        veyra::log::info("guest-quality", std::format("link test sent {} steps up to {} kbps, {:.1f} MB in {:.0f} ms, sender-limited {} times{}",
            done, done ? steps[done - 1] : 0, double(sentBytes) / 1e6, double(nowUs() - began) / 1000.0, skipped, probeStop ? " (stopped by the page)" : ""));
    }

    // ---- control thread: lifecycle and the 1 s statistics ----
    void controlLoop() {
        SetThreadDescription(GetCurrentThread(), L"Guest control");
        int64_t lastTick = nowUs(), lastLog = 0;
        std::unique_lock lock(m);
        while (active && !stopping) {
            controlCv.wait_for(lock, std::chrono::milliseconds(100));
            if (!active || stopping) break;
            auto p = peer;
            const bool kick = std::exchange(kickRequested, false);
            if (kick) { peer.reset(); token = randomToken(); }
            lock.unlock();
            if (kick && p) { retirePeer(p, "kick"); p.reset(); }
            if (p) {
                const auto state = rtc::PeerConnection::State(p->state.load());
                if (state == rtc::PeerConnection::State::Connected && !p->connectedHandled) onConnected(p);
                const bool dead = state == rtc::PeerConnection::State::Failed || state == rtc::PeerConnection::State::Closed ||
                                  state == rtc::PeerConnection::State::Disconnected;
                const bool late = !p->connectedHandled && nowUs() - p->createdUs > kConnectTimeoutUs;
                // The page sends its pad every 50 ms (every second at worst, from a background
                // tab): silence this long means it is gone even before ICE notices.
                const bool silent = p->connectedHandled && nowUs() - p->heardUs.load() > kSilentUs;
                if (dead || late || silent || p->left.load()) {
                    { std::lock_guard l(m); if (peer == p) peer.reset(); }
                    retirePeer(p, p->left.load() ? "guest left" : dead ? "connection lost" : silent ? "guest silent" : "never connected");
                    p.reset();
                }
            }
            if (p && p->connectedHandled) sendAcks(p);
            const int64_t t = nowUs();
            if (t - lastTick >= 1000000) {
                tick(p, double(t - lastTick) / 1e6);
                lastTick = t;
                if (t - lastLog >= 5000000 && p && p->connectedHandled) { lastLog = t; logSummary(); }
            }
            lock.lock();
        }
    }
    void onConnected(const std::shared_ptr<GuestPeer>& p) {
        rtc::Candidate local, remote;
        if (p->pc->getSelectedCandidatePair(&local, &remote)) {
            p->remote = remote.address().value_or("?");
            p->candidate = candidateName(remote.type());
            p->relayed = remote.type() == rtc::Candidate::Type::Relayed || local.type() == rtc::Candidate::Type::Relayed;
        }
        {
            std::lock_guard lock(statsMutex);
            presentToSent.clear(); encode.clear(); decodedToSent.clear(); decodedReal.clear(); decodedSmooth.clear(); report = json();
        }
        {
            // A new guest starts in automatique on real pictures until its page says more.
            const Limit limit = [&] { std::lock_guard l(m); return settings.limit; }();
            std::lock_guard lock(qualityMutex);
            rate = RateControl{};
            rate.setLimit(limit);
            applied = {Feed::Real, Size::P1080, 0};
            linkRtt = linkLoss = -1;
        }
        applyQuality("guest connected");
        if (testLimit) testLimit->armed(nowUs());
        video.setEnabled(true);
        video.requestKeyframe();
        audio.start();
        p->heardUs = nowUs();
        p->connectedHandled = true;
        guestLive = true;
        veyra::log::info("guest", std::format("guest connected peer={} candidate={} local={} relayed={} client={}{} after {:.0f} ms",
            p->id, p->candidate, candidateName(local.type()), p->relayed, p->native ? "boovi" : "browser",
            p->access.empty() ? std::string() : " access=" + p->access, double(nowUs() - p->createdUs) / 1000.0));
        sendStatus(p);
    }
    void sendAcks(const std::shared_ptr<GuestPeer>& p) {
        std::vector<Ack> batch;
        { std::lock_guard lock(inputMutex); batch.swap(acks); }
        if (batch.empty() || !p->stats || !p->stats->isOpen()) return;
        std::string text = R"({"t":"ack","a":[)";
        for (size_t i = 0; i < batch.size(); ++i)
            text += std::format("{}[{},{},{}]", i ? "," : "", batch[i].seq, batch[i].recvUs, batch[i].handedUs);
        text += "]}";
        try { p->stats->send(text); } catch (...) {}
    }
    void sendStatus(const std::shared_ptr<GuestPeer>& p) {
        if (!p || !p->stats || !p->stats->isOpen()) return;
        const auto v = video.stats();
        Quality q; Limit limit; Wish wish; bool automatic; uint32_t link, tested; Quality best;
        {
            std::lock_guard lock(qualityMutex);
            q = rate.current(); limit = rate.limit(); wish = rate.wish(); automatic = rate.automatic();
            link = rate.linkKbps(); tested = rate.testedKbps(); best = bestFor(limit, tested);
        }
        double sendFps, sendMbps;
        { std::lock_guard lock(statsMutex); sendFps = fps; sendMbps = mbps; }
        const auto text = std::format(
            R"({{"t":"st","mode":"{}","kbps":{},"w":{},"h":{},"src":{},"fps":{:.1f},"mbps":{:.2f},"feed":"{}","size":"{}","auto":{},"link":{},"tested":{},)"
            R"("max":{{"feed":"{}","size":"{}","kbps":{}}},"want":{{"feed":"{}","size":"{}","kbps":{}}},"best":{{"feed":"{}","size":"{}"}}}})",
            modeName(mode.load()), q.kbps, v.width, v.height, v.sourceHeight, sendFps, sendMbps, feedName(q.feed), sizeName(q.size), automatic ? 1 : 0, link, tested,
            feedName(limit.feed), sizeName(limit.size), limit.kbps, feedName(wish.feed), sizeName(wish.size), wish.kbps, feedName(best.feed), sizeName(best.size));
        try { p->stats->send(text); } catch (...) {}
    }
    void tick(const std::shared_ptr<GuestPeer>& p, double seconds) {
        {
            std::lock_guard lock(statsMutex);
            fps = framesWindow / seconds;
            mbps = double(bytesWindow) * 8 / seconds / 1e6;
            framesWindow = bytesWindow = 0;
            if (presentToSent.size() >= 30) {
                windowPresentToSent = median(presentToSent); windowEncode = median(encode); windowDecodedToSent = median(decodedToSent);
                presentToSent.clear(); encode.clear(); decodedToSent.clear();
            }
            if (decodedReal.size() >= 10) { lastDecodedReal = median(decodedReal); decodedReal.clear(); }
            if (decodedSmooth.size() >= 10) { lastDecodedSmooth = median(decodedSmooth); decodedSmooth.clear(); }
        }
        if (p && p->connectedHandled) sendStatus(p);
    }
    void logSummary() {
        std::vector<double> handed;
        remoteplay::ControllerState last; uint64_t count = 0; bool synthetic = false; uint32_t received = 0;
        GuestPad pad;
        {
            std::lock_guard lock(inputMutex);
            handed.swap(recvToHandedMs); last = lastHanded; count = inputs; synthetic = latest.synthetic; received = latest.buttons; pad = latest;
        }
        Quality q; uint32_t link;
        { std::lock_guard lock(qualityMutex); q = rate.current(); link = rate.linkKbps(); }
        const auto v = video.stats();
        std::lock_guard lock(statsMutex);
        veyra::log::info("guest-latency", std::format(
            "host presentToSent p50={:.2f} ms encode p50={:.2f} ms decodedToSent p50={:.2f} ms (real {:.2f}, fluide {:.2f}) fps={:.1f} sent={:.2f} Mb/s target={} kbps feed={} size={} {}x{} link={} kbps | input recvToHandedPs5 p50={:.2f} ms changes={} inputs={} synthetic={} last lx={} ly={} rx={} ry={} buttons=0x{:X} received buttons=0x{:X}",
            windowPresentToSent, windowEncode, windowDecodedToSent, lastDecodedReal, lastDecodedSmooth, fps, mbps, q.kbps, feedName(q.feed), sizeName(q.size), v.width, v.height, link,
            median(handed), handed.size(), count, synthetic, last.leftX, last.leftY, last.rightX, last.rightY, last.buttons, received));
        if (pad.version)
            veyra::log::info("guest-latency", std::format("extended pad v{} motion={} gyro=({:.2f},{:.2f},{:.2f}) accel=({:.2f},{:.2f},{:.2f}) samples={} touch=({},{},{}) rx={}",
                pad.version, pad.motionValid, pad.gyro[0], pad.gyro[1], pad.gyro[2], pad.accel[0], pad.accel[1], pad.accel[2], pad.motionSampleCount,
                pad.touches[0].id, pad.touches[0].x, pad.touches[0].y, pad.axes[2]));
    }
};

GuestStreamer::GuestStreamer() : p_(std::make_shared<Impl>()) {}
GuestStreamer::~GuestStreamer() { stop(); }

engine::EarlyCapture& GuestStreamer::capture() { return p_->video; }

bool GuestStreamer::start(const ShareSettings& settings, std::string& error) {
    auto& p = *p_;
    if (active()) return true;
    std::optional<CloudflareConfig> cloudflare;
    if (settings.reach != Reach::Tailnet) cloudflare = loadCloudflareConfig();
    if (settings.reach == Reach::Tunnel && !cloudflare) {
        error = "aucun tunnel cloudflare configuré sur ce pc";
        std::lock_guard lock(p.m); p.error = error; return false;
    }
    // Through Cloudflare, cloudflared is the only client: loopback is enough.
    const std::string ip = tailscaleAddress();
    std::vector<std::string> addresses{"127.0.0.1"};
    if (!ip.empty() && settings.reach == Reach::Tailnet) addresses.push_back(ip);
    {
        std::lock_guard lock(p.m);
        p.settings = settings;
        p.tailscaleIp = ip;
        p.token = randomToken();
        p.error.clear();
        p.cloudflareIce = json::array();
    }
    p.testLimit = TestLimit::fromEnvironment();
    if (p.testLimit) veyra::log::warn("guest", std::format("test bottleneck {} kbps from {:.0f} s after the guest connects", p.testLimit->kbps, double(p.testLimit->afterUs) / 1e6));
    {
        std::lock_guard lock(p.qualityMutex);
        p.rate = RateControl{};
        p.rate.setLimit(settings.limit);
    }
    p.video.setQuality(1080, 60, false, defaultKbps(Feed::Real, Size::P1080));
    // The first free port from the configured one, so a second BooVI still shares; the named
    // tunnel's ingress points at the configured port only.
    const uint16_t lastPort = settings.reach == Reach::Tunnel ? settings.port : uint16_t(settings.port + 9);
    bool listening = false;
    for (uint16_t port = settings.port; port <= lastPort && !listening; ++port)
        listening = p.http.start(addresses, port, [impl = p_.get()](const HttpRequest& r) { return impl->handle(r); }, error);
    if (!listening) { std::lock_guard lock(p.m); p.error = error; p.token.clear(); return false; }
    const bool tunnelOk = settings.reach == Reach::Tunnel ? p.tunnel.startNamed(cloudflare->tunnelToken, cloudflare->hostname, error)
                        : settings.reach == Reach::Quick ? p.tunnel.startQuick(p.http.port(), error) : true;
    if (!tunnelOk) { p.http.stop(); std::lock_guard lock(p.m); p.error = error; p.token.clear(); return false; }
    {
        std::lock_guard lock(p.m);
        p.active = true; p.stopping = false; p.kickRequested = false;
        p.settings.port = p.http.port();
    }
    if (cloudflare && !cloudflare->turnKeyId.empty()) {
        p.turnFetch = std::thread([impl = p_.get(), config = *cloudflare] {
            SetThreadDescription(GetCurrentThread(), L"Guest turn");
            std::string why;
            json ice = fetchTurnIceServers(config, kTurnTtlSeconds, why);
            std::lock_guard lock(impl->m);
            impl->cloudflareIce = std::move(ice);
            if (impl->cloudflareIce.empty()) veyra::log::warn("guest", "cloudflare turn unavailable: " + why);
            else veyra::log::info("guest", std::format("cloudflare turn credentials ready ({} server entries, ttl {} s)", impl->cloudflareIce.size(), kTurnTtlSeconds));
        });
    }
    p.control = std::thread([impl = p_.get()] { impl->controlLoop(); });
    const auto bound = p.http.bound();
    std::string where;
    for (const auto& a : bound) where += (where.empty() ? "" : ",") + a;
    veyra::log::info("guest", std::format("sharing started port={} listen={} tailscale={} reach={} max feed={} size={} kbps={}",
        p.http.port(), where, ip.empty() ? "down" : "up", reachName(settings.reach), feedName(settings.limit.feed), sizeName(settings.limit.size), settings.limit.kbps));
    return true;
}

void GuestStreamer::stop() {
    auto& p = *p_;
    std::shared_ptr<GuestPeer> last;
    {
        std::lock_guard lock(p.m);
        if (!p.active) return;
        p.stopping = true;
        last = std::move(p.peer);
    }
    p.controlCv.notify_all();
    if (p.control.joinable()) p.control.join();
    if (p.turnFetch.joinable()) p.turnFetch.join();
    p.tunnel.stop();
    p.http.stop();
    p.retirePeer(last, "sharing stopped");
    {
        // After the peer is closed: no page can ask for another link test.
        std::lock_guard lock(p.probeMutex);
        p.probeStop = true;
        if (p.probeThread.joinable()) p.probeThread.join();
    }
    p.video.setEnabled(false);
    p.audio.stop();
    p.guestLive = false;
    {
        std::lock_guard lock(p.m);
        p.active = false; p.stopping = false;
        p.token.clear();   // the link dies with the share session
        p.cloudflareIce = json::array();
    }
    veyra::log::info("guest", "sharing stopped");
}

bool GuestStreamer::active() const { std::lock_guard lock(p_->m); return p_->active; }

void GuestStreamer::kick() {
    { std::lock_guard lock(p_->m); if (!p_->active) return; p_->kickRequested = true; }
    p_->controlCv.notify_all();
    veyra::log::info("guest", "kick requested; the link changes");
}

void GuestStreamer::setMode(ControlMode mode) {
    p_->mode = mode;
    veyra::log::info("guest", std::format("control mode {}", modeName(mode)));
    if (auto peer = p_->currentPeer(); peer && peer->connectedHandled) p_->sendStatus(peer);
}
ControlMode GuestStreamer::mode() const { return p_->mode.load(); }

void GuestStreamer::setAccessDesk(AccessDesk* desk) { p_->desk = desk; }

void GuestStreamer::setLimit(const Limit& limit) {
    { std::lock_guard lock(p_->m); p_->settings.limit = limit; }
    { std::lock_guard lock(p_->qualityMutex); p_->rate.setLimit(limit); }
    veyra::log::info("guest-quality", std::format("host maximum feed={} size={} kbps={}", feedName(limit.feed), sizeName(limit.size), limit.kbps));
    p_->applyQuality("host maximum");
}

ShareStatus GuestStreamer::status() const {
    auto& p = *p_;
    ShareStatus out;
    std::shared_ptr<GuestPeer> peer;
    {
        std::lock_guard lock(p.m);
        out.token = p.token; out.port = p.settings.port; out.tailscaleIp = p.tailscaleIp; out.error = p.error;
        out.reach = p.settings.reach; out.turnReady = !p.cloudflareIce.empty();
        peer = p.peer;
        out.phase = !p.active ? Phase::Off : !peer ? Phase::Waiting : peer->connectedHandled ? Phase::Connected : Phase::Connecting;
        if (!p.active) out.port = 0;
    }
    out.mode = p.mode.load();
    {
        std::lock_guard lock(p.qualityMutex);
        out.quality = p.rate.current(); out.limit = p.rate.limit(); out.automatic = p.rate.automatic();
        out.linkKbps = p.rate.testedKbps(); out.linkRttMs = p.linkRtt; out.linkLossPercent = p.linkLoss < 0 ? -1 : p.linkLoss * 100;
        out.targetKbps = out.quality.kbps;
    }
    out.tunnel = p.tunnel.phase();
    out.publicUrl = p.tunnel.publicUrl();
    if (out.error.empty() && out.tunnel == TunnelPhase::Failed) out.error = p.tunnel.error();
    if (peer && peer->connectedHandled.load()) {
        out.remote = peer->remote; out.candidate = peer->candidate; out.relayed = peer->relayed; out.nativeClient = peer->native;
        out.accessName = peer->access;
        if (auto rtt = peer->pc->rtt()) out.rttMs = double(rtt->count());
    }
    const auto v = p.video.stats();
    out.encoder = v.encoder; out.width = v.width; out.height = v.height; out.sourceHeight = v.sourceHeight;
    if (out.error.empty()) out.error = v.error;
    {
        std::lock_guard lock(p.inputMutex);
        out.inputs = p.inputs; out.padConnected = p.haveInput && p.latest.connected; out.synthetic = p.haveInput && p.latest.synthetic;
        out.padSilent = out.padConnected && !out.synthetic && !p.buttonSeen && p.padSinceUs && nowUs() - p.padSinceUs > kPadSilentUs;
        if (peer && peer->connectedHandled.load()) out.idleSeconds = double(nowUs() - std::max(p.activeUs, peer->createdUs)) / 1e6;
    }
    std::lock_guard lock(p.statsMutex);
    out.frames = p.framesSent; out.fps = p.fps; out.sentMbps = p.mbps;
    out.presentToSentMs = p.windowPresentToSent; out.encodeMs = p.windowEncode; out.decodedToSentMs = p.windowDecodedToSent;
    out.decodedToSentRealMs = p.lastDecodedReal; out.decodedToSentSmoothMs = p.lastDecodedSmooth;
    auto num = [&](const char* key) { return p.report.is_object() && p.report.contains(key) && p.report[key].is_number() ? p.report[key].get<double>() : -1.0; };
    out.presentToDisplayMs = num("disp");
    out.decodedToDisplayMs = num("src");
    out.receiveToDisplayMs = num("rxdisp");
    out.decodeMs = num("dec");
    out.inputToHostMs = num("inhost");
    out.inputToPs5Ms = num("inps5");
    out.displayHz = num("hz");
    out.guestFps = num("fps");
    out.lossPercent = std::max(0.0, num("loss")) * 100;
    auto text = [&](const char* key) {
        return p.report.is_object() && p.report.contains(key) && p.report[key].is_string() ? p.report[key].get<std::string>().substr(0, 64) : std::string();
    };
    out.padName = text("pad");
    out.padMapping = text("map");
    if (num("rtt") >= 0) out.rttMs = num("rtt");
    return out;
}

std::optional<remoteplay::ControllerState> GuestStreamer::guestState(remoteplay::HostTime now) {
    auto& p = *p_;
    if (!p.guestLive.load()) return std::nullopt;
    std::lock_guard lock(p.inputMutex);
    if (!p.haveInput || now / 10 - p.latestRecvUs > kGuestStale / 10) return remoteplay::ControllerState{};
    return toControllerState(p.latest);
}

void GuestStreamer::handedToPs5(const remoteplay::ControllerState& sent) {
    auto& p = *p_;
    if (!p.guestLive.load()) return;
    std::lock_guard lock(p.inputMutex);
    p.lastHanded = sent;
    if (!p.changePending) return;
    p.changePending = false;
    const int64_t handedUs = nowUs();   // after the session took it, not the pass's start
    if (p.acks.size() < 256) p.acks.push_back({p.changeSeq, p.changeRecvUs, handedUs});
    if (p.recvToHandedMs.size() < 20000) p.recvToHandedMs.push_back(double(handedUs - p.changeRecvUs) / 1000.0);
}

void GuestStreamer::feedback(const remoteplay::ControllerFeedback& feedback) {
    auto& p = *p_;
    // A BooVI client drives a real pad: rumble, adaptive triggers and haptics, as one binary message.
    if (auto peer = p.guestLive.load() && p.mode.load() != ControlMode::WatchOnly ? p.currentPeer() : nullptr; peer && peer->native) {
        if ((!feedback.rumble && !feedback.triggers && !feedback.motionReset && feedback.haptics.empty()) || !peer->feedback || !peer->feedback->isOpen()) return;
        const auto bytes = encodeFeedbackMessage(feedback);
        try { peer->feedback->send(bytes.data(), bytes.size()); } catch (...) {}
        return;
    }
    if (!feedback.rumble || !p.guestLive.load() || p.mode.load() == ControlMode::WatchOnly) return;
    {
        std::lock_guard lock(p.inputMutex);
        if (feedback.left == p.rumbleL && feedback.right == p.rumbleR) return;
        p.rumbleL = feedback.left; p.rumbleR = feedback.right;
    }
    auto peer = p.currentPeer();
    if (!peer || !peer->feedback || !peer->feedback->isOpen()) return;
    try { peer->feedback->send(std::format(R"({{"t":"rumble","l":{},"r":{}}})", feedback.left, feedback.right)); } catch (...) {}
}

} // namespace veyra::guest
