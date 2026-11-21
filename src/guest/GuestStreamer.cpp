#include "veyra/guest/GuestStreamer.h"

#include <winsock2.h>
#include <iphlpapi.h>
#include <ws2tcpip.h>
#include <bcrypt.h>

#include <rtc/rtc.hpp>
#include <nlohmann/json.hpp>

#include "GuestAudio.h"
#include "GuestBroker.h"
#include "GuestCloudflare.h"
#include "GuestHttp.h"
#include "GuestVideo.h"
#include "veyra/guest/GuestAccess.h"
#include "veyra/guest/GuestRoom.h"
#include "veyra/guest/InboundBudget.h"
#include "veyra/guest/Lifeline.h"
#include "veyra/Log.h"
#include "veyra/sink/RecordingAudioTap.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
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
#include <unordered_map>

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
constexpr int64_t kOrphanUs = 60000000;   // a room member with no connection this long leaves (two slow reconnects fit)
constexpr int64_t kSilentUs = 3000000;
constexpr int64_t kPadSilentUs = 15000000;  // a pad that has sent no button this long is flagged to the host
constexpr int64_t kKickBanUs = 600000000;   // a share-link guest the host disconnected: that address waits 10 min
constexpr int64_t kReportLogUs = 10000000;   // a viewer's latency report is logged at most this often
constexpr int64_t kRefusalRepeatUs = 2000000;   // the same room refusal is told once in this long
constexpr int kTurnTtlSeconds = 4 * 3600;   // the user's own TURN key: credentials last this long; past it, restart the share
constexpr auto kLeaseWait = std::chrono::milliseconds(3000);   // the broker's TURN lease, before the offer goes without it
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

const char* reachName(Reach r) { return r == Reach::Broker ? "broker" : "tailnet"; }
const char* relayName(Relay r) { return r == Relay::Owner ? "owner" : r == Relay::OwnKey ? "own-key" : "none"; }

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

struct GuestPeer;

// Where a retired viewer's connection is closed and destroyed: its own thread, never the UI
// thread, a libdatachannel callback or a thread holding a lock of ours. rtc::PeerConnection's
// close can block, and its destructor waits for libdatachannel's processor (impl's
// mProcessor.join()); from a callback of that processor it waits for itself. On 2026-11-20 at
// 23:02 the share wedged after a guest went silent: the UI thread's status() called the dying
// peer's rtt() and could hold the last reference to it, so its destructor could run there; the
// UI stopped and every later /session stayed at "new peer". A peer is destroyed here once nobody
// else holds it (a callback still running keeps it), so its destructor never runs elsewhere.
// The thread outlives the streamer, detached: a teardown stuck for good holds only this thread.
class PeerReaper {
public:
    PeerReaper() : s_(std::make_shared<State>()) { std::thread([s = s_] { run(*s); }).detach(); }
    // At exit: what is queued gets 3 s to go, then the thread is left to the process's end.
    ~PeerReaper() {
        std::unique_lock lock(s_->m);
        s_->quit = true;
        s_->cv.notify_all();
        s_->cv.wait_for(lock, std::chrono::seconds(3), [&] { return s_->queue.empty() && !s_->busy; });
    }
    void bury(std::shared_ptr<GuestPeer> p, std::string why) {
        { std::lock_guard lock(s_->m); s_->queue.emplace_back(std::move(p), std::move(why)); }
        s_->cv.notify_all();
    }

private:
    struct State {
        std::mutex m;
        std::condition_variable cv;
        std::deque<std::pair<std::shared_ptr<GuestPeer>, std::string>> queue;
        bool quit = false;
        bool busy = false;
    };
    static void run(State& s);
    std::shared_ptr<State> s_;
};

// One viewer's connection and everything kept for it. Callbacks hold it weakly: the peer
// connection owns them.
struct GuestPeer {
    // Set before it is shared.
    std::string id;                              // the signaling id (/answer): never shown to other viewers
    std::string tag;                             // the room's public id
    std::string gid;                             // the page's own id: a reconnection replaces its previous peer
    std::string name;
    std::string source;                          // the address it came from (broker: the guest's)
    PeerCredential credential;                   // what /session was shown: /answer shows the same
    bool native = false;                         // a BooVI client: extended pad, binary feedback
    std::string accessId;                        // the friend's access, empty for the share link
    bool watchOnly = false;                      // a friend whose access only watches
    int64_t createdUs = 0;

    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::Track> video, audio;
    std::shared_ptr<rtc::RtpPacketizationConfig> videoConfig, audioConfig;
    std::shared_ptr<rtc::DataChannel> input, feedback, stats, probe, room;
    std::atomic<int> state{int(rtc::PeerConnection::State::New)};
    std::atomic<bool> gathered{false};
    // When each kind of local candidate first came, from createdUs (0: none yet).
    std::atomic<int64_t> firstSrflxUs{0}, firstRelayUs{0};
    // Its life: connecting, then connected (an encoder and a place in the room) or retired. Both
    // flags change under lifeMutex only (onConnected, retirePeer), so a peer retired while it
    // connects never keeps an encoder or a place.
    std::mutex lifeMutex;
    std::atomic<bool> connectedHandled{false};   // set by the control thread once media flow
    std::atomic<bool> retired{false};
    std::atomic<bool> replaced{false};           // its page reconnected: the new peer keeps its place in the room
    std::atomic<bool> roomWanted{false};         // its room channel opened: send it the snapshot
    std::atomic<int64_t> heardUs{0};             // last message from the page, any channel
    std::atomic<bool> left{false};               // the page said goodbye (tab closed)
    std::atomic<int> slot{-1};                   // its encoder in the GuestVideoSet, while connected
    std::atomic<double> sctpRttMs{-1};           // sampled by the control thread: status() never calls into the connection
    std::string remote, candidate;               // written before connectedHandled
    bool relayed = false;
    // The broker's TURN lease for this viewer, handed back once the relay is not needed.
    std::mutex leaseMutex;
    std::string lease;
    std::string takeLease() { std::lock_guard lock(leaseMutex); return std::exchange(lease, {}); }

    // ---- what it receives (qualityMutex) ----
    std::mutex qualityMutex;
    RateControl rate;
    Quality applied{Feed::Real, Size::P1080, 0};
    uint32_t shareKbps = 0;
    bool latencyFirst = false;                   // it plays: automatique's top rung is real pictures
    double linkRtt = -1, linkLoss = -1;
    std::atomic<int> decodeSlowReports{0};
    std::atomic<int64_t> behindKeyUs{0}, decodeStepUs{0};

    // ---- what it may send (inboundMutex) ----
    std::mutex inboundMutex;
    InboundBudget inbound;
    RoomRefusal lastRefusal = RoomRefusal::None;
    int64_t lastRefusalUs = 0;

    // ---- its link test ----
    std::mutex probeMutex;
    std::thread probeThread;
    std::atomic<bool> probing{false}, probeStop{false};

    // ---- its pad (inputMutex), written by the input channel, read every 1 ms while it holds the controller ----
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
    int64_t padSinceUs = 0;
    bool buttonSeen = false;
    int64_t activeUs = 0;    // the pad last changed: a button, a trigger, a stick past a small dead zone
    struct Ack { uint16_t seq; int64_t recvUs, handedUs; };
    std::vector<Ack> acks;
    remoteplay::ControllerState lastHanded;
    std::vector<double> recvToHandedMs;
    uint8_t rumbleL = 0, rumbleR = 0;

    // ---- statistics (statsMutex) ----
    std::mutex statsMutex;
    uint64_t framesSent = 0, bytesSent = 0, framesWindow = 0, bytesWindow = 0;
    uint64_t undatedWindow = 0;                  // frames of the window without a source time
    std::vector<double> presentToSent, encode, decodedToSent, decodedReal, decodedSmooth;
    double windowPresentToSent = -1, windowEncode = -1, windowDecodedToSent = -1, fps = 0, mbps = 0;
    size_t windowDated = 0, windowUndated = 0;
    json report;                                 // the page's last latency report
    int64_t reportLoggedUs = 0;

    ~GuestPeer() { if (probeThread.joinable()) probeThread.join(); }
};

void PeerReaper::run(State& s) {
    SetThreadDescription(GetCurrentThread(), L"Guest teardown");
    // Tests: VEYRA_TEST_GUEST_STALL_TEARDOWN=1 makes the first teardown hang for good, as a
    // libdatachannel close or destructor that never returns.
    bool stallOnce = [] { const char* v = std::getenv("VEYRA_TEST_GUEST_STALL_TEARDOWN"); return v && *v == '1'; }();
    for (;;) {
        std::pair<std::shared_ptr<GuestPeer>, std::string> item;
        {
            std::unique_lock lock(s.m);
            s.cv.wait(lock, [&] { return s.quit || !s.queue.empty(); });
            if (s.queue.empty()) return;
            item = std::move(s.queue.front());
            s.queue.pop_front();
            s.busy = true;
        }
        auto& [p, why] = item;
        const int64_t began = nowUs();
        if (stallOnce) {
            stallOnce = false;
            veyra::log::warn("guest", std::format("test: the teardown of peer {} hangs for good", p->id));
            for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        { std::lock_guard lock(p->probeMutex); if (p->probeThread.joinable()) p->probeThread.join(); }
        if (p->stats && p->stats->isOpen()) {
            try { p->stats->send(std::format(R"({{"t":"bye","why":"{}"}})", why)); } catch (...) {}
        }
        try { p->pc->close(); } catch (...) {}
        // Whoever else still holds it (a callback running, a status copy) lets go first, so the
        // destructor runs on this thread.
        bool said = false;
        while (p.use_count() > 1) {
            if (!said && nowUs() - began > 5'000'000) { said = true; veyra::log::warn("guest", std::format("peer {} still held 5 s after its close", p->id)); }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        p.reset();
        if (const double ms = double(nowUs() - began) / 1000.0; ms > 1000) veyra::log::warn("guest", std::format("a peer's teardown took {:.0f} ms", ms));
        std::lock_guard lock(s.m);
        s.busy = false;
        s.cv.notify_all();
    }
}

struct GuestStreamer::Impl {
    // ---- shared state (m) ----
    mutable std::mutex m;
    std::condition_variable controlCv;
    // ICE gathering is waited on apart from m: the UI thread reads status() meanwhile.
    std::mutex gatherMutex;
    std::condition_variable gatherCv;
    ShareSettings settings;
    bool active = false, stopping = false, renewRequested = false;
    std::string token, tailscaleIp, error;
    Relay relay = Relay::None;
    json cloudflareIce = json::array();   // the user's own TURN key's credentials; never logged
    std::vector<std::shared_ptr<GuestPeer>> peers;   // every viewer not retired, in arrival order
    // A page's own id -> its room tag and the credential that made it, for the share session. A
    // tag is handed back only to the same credential.
    struct GidTag { std::string tag; TokenHash hash; };
    std::unordered_map<std::string, GidTag> gidTags;
    std::vector<std::string> kickTags, dropAccessIds;   // the UI's requests, carried out by the control thread
    std::vector<std::pair<std::string, int64_t>> banned;   // share-link addresses kicked, until
    SourceLimiter limiter;
    std::atomic<AccessDesk*> desk{nullptr};
    std::atomic<ControlMode> mode{ControlMode::GuestPlays};
    std::shared_ptr<TestLimit> testLimit;
    // Tests: the next N peers never get made (VEYRA_TEST_GUEST_STALL=<n>): a handler stuck
    // until the share stops, as the 2026-11-20 wedge.
    std::atomic<int> testStalls{0};
    // Tests: onConnected holds this long once it has an encoder (VEYRA_TEST_GUEST_CONNECT_HOLD_MS):
    // a page that reconnects meanwhile retires the peer while it connects.
    int testConnectHoldMs = 0;

    // ---- the encoders lent to viewers (slotMutex) ----
    std::mutex slotMutex;
    std::array<std::weak_ptr<GuestPeer>, GuestVideoSet::kSlots> slotPeer;

    // ---- the room (roomMutex) ----
    std::mutex roomMutex;
    Room room;
    RoomRules rules;
    uint64_t sentRev = ~0ull;
    bool roomDirty = false;                      // resend the snapshot (history changed)
    std::vector<RoomEvent> events;               // for the UI's drainEvents()
    std::vector<PlayRecord> history;
    std::atomic<bool> ownerPriority{true};
    std::array<std::array<std::atomic<uint32_t>, 2>, GuestVideoSet::kSlots> unsent{};   // [no viewer, track closed]
    std::atomic<int64_t> lastPlayUs{0};          // any player's pad last changed, or a viewer came
    std::atomic<bool> rebalanceWanted{false};    // the UI changed the mode or the limit
    bool lastLive = false;                       // control thread: the source was live at the last rebalance

    // ---- the holder, read by the PS5 input thread (holderMutex) ----
    std::mutex holderMutex;
    std::shared_ptr<GuestPeer> holder;
    // The host's own pad as the input thread last saw it (that thread only).
    uint32_t hostButtons = 0;
    uint8_t hostL2 = 0, hostR2 = 0;

    // The fluide feeds' measured cost over real pictures, ms: decoded here -> sent, per tap, with
    // when each was last measured (any viewer). The pages show the difference.
    std::mutex costMutex;
    double lastReal = -1, lastSmooth = -1;
    int64_t lastRealUs = 0, lastSmoothUs = 0;

    std::thread control, turnFetch;
    // The control loop's heartbeat: status() says the share is stuck when it stops.
    std::atomic<int64_t> controlBeatUs{0};
    std::atomic<const char*> controlStage{"idle"};
    std::atomic<bool> stallLogged{false};
    std::mutex audioMutex;
    std::atomic<bool> audioOn{false};

    PeerReaper reaper;
    // Every libdatachannel callback reaches this through the lifeline: a connection the reaper
    // still holds when the streamer goes calls nothing. ~Impl cuts it before any member goes.
    Lifeline<Impl> lifeline{*this};
    // Last: destroyed first, so no thread of theirs outlives the state above.
    GuestBroker broker;
    GuestHttp http;
    GuestAudio audio{[this](const std::vector<uint8_t>& packet, uint32_t ts) { sendAudio(packet, ts); }};
    GuestVideoSet videos{[this](size_t slot, EncodedFrame&& frame) { sendVideo(slot, std::move(frame)); }};

    ~Impl() { lifeline.cut(); }

    std::vector<std::shared_ptr<GuestPeer>> livePeers() {
        std::lock_guard lock(m);
        std::vector<std::shared_ptr<GuestPeer>> out;
        for (const auto& p : peers) if (p->connectedHandled && !p->retired) out.push_back(p);
        return out;
    }
    std::shared_ptr<GuestPeer> peerById(const std::string& id) {
        std::lock_guard lock(m);
        for (const auto& p : peers) if (p->id == id) return p;
        return nullptr;
    }

    // ---- media (encoder threads) ----
    void sendVideo(size_t slot, EncodedFrame&& frame) {
        std::shared_ptr<GuestPeer> p;
        { std::lock_guard lock(slotMutex); p = slotPeer[slot].lock(); }
        // Bitstreams that found no open track to go to, by encoder: logged with guest-latency.
        if (!p || p->retired || !p->connectedHandled) { ++unsent[slot][0]; return; }
        if (!p->video || !p->video->isOpen()) { ++unsent[slot][1]; return; }
        const uint32_t ts = uint32_t(uint64_t(frame.captureUs) * 9 / 100);
        try { p->video->sendFrame(reinterpret_cast<const std::byte*>(frame.data.data()), frame.data.size(), rtc::FrameInfo(ts)); }
        catch (const std::exception& e) { veyra::log::warn("guest-rtc", std::string("video send: ") + e.what()); return; }
        const int64_t sent = nowUs();
        {
            std::lock_guard lock(p->statsMutex);
            ++p->framesSent; ++p->framesWindow; p->bytesSent += frame.data.size(); p->bytesWindow += frame.data.size();
            p->presentToSent.push_back(double(sent - frame.captureUs) / 1000.0);
            p->encode.push_back(double(frame.encodedUs - frame.captureUs) / 1000.0);
            if (frame.sourceUs > 0) {
                p->decodedToSent.push_back(double(sent - frame.sourceUs) / 1000.0);
                (frame.presented ? p->decodedSmooth : p->decodedReal).push_back(double(sent - frame.sourceUs) / 1000.0);
            } else ++p->undatedWindow;
        }
        if (p->stats && p->stats->isOpen()) {
            const auto text = std::format(R"({{"t":"f","r":{},"c":{},"d":{},"e":{},"s":{},"k":{},"b":{}}})", ts, frame.captureUs, frame.sourceUs, frame.encodedUs, sent, frame.key ? 1 : 0, frame.data.size());
            try { p->stats->send(text); } catch (...) {}
        }
    }
    void sendAudio(const std::vector<uint8_t>& packet, uint32_t ts) {
        for (const auto& p : livePeers()) {
            if (!p->audio || !p->audio->isOpen()) continue;
            try { p->audio->sendFrame(reinterpret_cast<const std::byte*>(packet.data()), packet.size(), rtc::FrameInfo(ts)); } catch (...) {}
        }
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
            return r.path == "/session" ? session(r) : answer(r);
        }
        return {404, "text/plain", "not found"};
    }
    static std::string masked(const std::string& source) {
        const size_t cut = source.find_last_of(".:");
        return source.substr(0, cut == std::string::npos ? 0 : cut + 1) + "x";
    }
    // A wrong credential counts against its source address only (SourceLimiter).
    void refused(const std::string& source, const char* what) {
        const bool locked = limiter.failed(source, nowUs());
        // The address without its last part: enough to tell sources apart, not to name anyone.
        veyra::log::warn("guest", std::format("request with a wrong {} refused from {}{}", what, masked(source), locked ? "; that address waits a minute" : ""));
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
    // then the friend. /answer: the friend while the visit is in. Empty, with `out` set, when the
    // request ends here.
    std::optional<Admission> accessOk(const std::string& candidate, const std::string& source, bool knock, HttpResponse& out) {
        static const std::string invalid = R"({"error":"lien invalide ou révoqué","state":"invalid"})";
        AccessDesk* d = desk.load();
        if (d && !knock) {
            if (auto friendIn = d->admitted(candidate)) return friendIn;
        } else if (d) {
            SYSTEMTIME local{};
            GetLocalTime(&local);
            const auto k = d->knock(candidate, nowUs(), int64_t(std::time(nullptr)), local.wHour * 60 + local.wMinute);
            if (k.admitted) return Admission{k.accessId, k.name, k.watchOnly};
            if (!k.invalid) {
                out = {k.status, "application/json", json{{"state", k.state}, {"host", k.host}, {"name", k.name}, {"detail", k.detail}}.dump()};
                return std::nullopt;
            }
        }
        refused(source, "access");
        out = {403, "application/json", invalid};
        return std::nullopt;
    }
    HttpResponse session(const HttpRequest& r) {
        const std::string& source = r.source;
        json in = json::parse(r.body, nullptr, false);
        const bool byAccess = in.is_object() && in.contains("access") && in["access"].is_string();
        if (!in.is_object() || (!byAccess && (!in.contains("token") || !in["token"].is_string()))) return {400, "text/plain", "bad request"};
        Admission access;
        r.at("credential");
        if (byAccess) {
            HttpResponse wait;
            auto friendIn = accessOk(in["access"].get<std::string>(), source, true, wait);
            if (!friendIn) return wait;
            access = std::move(*friendIn);
        } else if (!tokenOk(in["token"].get<std::string>(), source)) return {403, "application/json", R"({"error":"lien invalide ou expiré"})"};
        if (access.accessId.empty()) {
            std::lock_guard lock(m);
            const int64_t now = nowUs();
            std::erase_if(banned, [&](const auto& b) { return b.second <= now; });
            for (const auto& b : banned)
                if (b.first == source) {
                    veyra::log::info("guest", "session refused: this address was disconnected by the host");
                    return {403, "application/json", R"({"error":"l'hôte t'a déconnecté","state":"kicked"})"};
                }
        }
        const auto text = [&](const char* key, size_t most) { return in.contains(key) && in[key].is_string() ? in[key].get<std::string>().substr(0, most) : std::string(); };
        // A BooVI client says so; it then gets real pictures only and sends the extended pad.
        const bool native = in.contains("client") && in["client"] == "boovi";
        std::string gid;
        for (const char c : text("gid", 40)) if (std::isalnum(uint8_t(c)) || c == '-' || c == '_') gid += c;
        const std::string name = access.accessId.empty() ? cleanGuestName(text("name", 120)) : access.name;
        // A full room: only a page that reconnects (its gid) gets its place back. A place is a room
        // member, or a viewer still connecting.
        {
            std::vector<std::string> places;
            { std::lock_guard lock(roomMutex); for (const auto& member : room.members()) places.push_back(member.id); }
            std::lock_guard lock(m);
            for (const auto& p : peers)
                if (!p->retired && std::find(places.begin(), places.end(), p->tag) == places.end()) places.push_back(p->tag);
            const auto mine = gid.empty() ? gidTags.end() : gidTags.find(gid);
            const int others = int(std::count_if(places.begin(), places.end(), [&](const std::string& tag) { return mine == gidTags.end() || tag != mine->second.tag; }));
            if (others >= settings.rules.room.maxMembers) {
                veyra::log::info("guest", std::format("session refused: the room is full ({} viewers)", others));
                return {409, "application/json", R"({"error":"le salon est plein","state":"full"})"};
            }
        }
        // The relay is lent only once the guest has shown its token or access.
        const auto [reach, relayFrom] = [&] { std::lock_guard lock(m); return std::pair{settings.reach, relay}; }();
        TurnLease lease;
        json ice = json::array();
        if (reach == Reach::Broker && relayFrom == Relay::Owner) {
            const int64_t asked = nowUs();
            r.at("turn lease");
            lease = broker.requestTurn(kLeaseWait);
            r.holdLease(lease.id);
            ice = lease.iceServers;
            veyra::log::info("guest", std::format("turn lease {} in {:.0f} ms", lease.id.empty() ? "refused (" + lease.why + ")" : std::string("granted"),
                                                  double(nowUs() - asked) / 1000.0));
        } else if (reach == Reach::Broker) {
            std::lock_guard lock(m);
            ice = cloudflareIce;
        }
        const PeerCredential credential{hashToken(in[byAccess ? "access" : "token"].get<std::string>()), !byAccess, source};
        auto p = createPeer(native, access, credential, name, gid, ice, lease.id, r);
        if (!p) {
            broker.endTurn(lease.id);
            veyra::log::warn("guest", "session failed: the peer could not be made (share stopping, or WebRTC set-up failed)");
            return {503, "application/json", R"({"error":"impossible de préparer la connexion"})"};
        }
        r.at("ice gathering");
        {
            std::unique_lock lock(gatherMutex);
            gatherCv.wait_for(lock, std::chrono::seconds(3), [&] { return p->gathered.load() || p->retired.load(); });
        }
        veyra::log::info("guest", std::format("peer {} offer after {:.0f} ms (gathering {}; first srflx {:.0f} ms, relay {:.0f} ms)", p->id,
            double(nowUs() - p->createdUs) / 1000.0, p->gathered.load() ? "complete" : "cut at 3 s",
            double(p->firstSrflxUs.load()) / 1000.0, double(p->firstRelayUs.load()) / 1000.0));
        if (p->retired) {
            veyra::log::warn("guest", std::format("session failed: peer {} was replaced or closed while gathering", p->id));
            return {409, "application/json", R"({"error":"une autre connexion a pris la place"})"};
        }
        const auto local = p->pc->localDescription();
        if (!local) {
            veyra::log::warn("guest", std::format("session failed: peer {} has no offer", p->id));
            return {503, "application/json", R"({"error":"pas d'offre"})"};
        }
        json out{{"id", p->id}, {"tag", p->tag}, {"sdp", std::string(*local)}, {"mode", modeName(mode.load())}, {"input", kExtendedVersion}, {"room", 1}};
        if (AccessDesk* d = desk.load(); d && !access.accessId.empty()) {
            out["host"] = d->hostName();
            // Admitted while the console still wakes: the page connects now, the picture follows.
            if (const auto v = d->visit(); v && v->phase != VisitPhase::Ready && v->phase != VisitPhase::Playing) out["console"] = visitPhaseName(v->phase);
        }
        // STUN first: the browser keeps a direct path when there is one, the relay is the fallback.
        out["ice"] = json::array();
        if (reach == Reach::Broker) {
            out["ice"].push_back(json{{"urls", kStunUrl}});
            for (const auto& server : ice) out["ice"].push_back(server);
        }
        return {200, "application/json", out.dump()};
    }
    HttpResponse answer(const HttpRequest& r) {
        const std::string& source = r.source;
        json in = json::parse(r.body, nullptr, false);
        const bool byAccess = in.is_object() && in.contains("access") && in["access"].is_string();
        if (!in.is_object() || (!byAccess && (!in.contains("token") || !in["token"].is_string())) || !in.contains("id") || !in.contains("sdp") || !in["sdp"].is_string() || !in["id"].is_string())
            return {400, "text/plain", "bad request"};
        if (byAccess) {
            HttpResponse refusedOut;
            if (!accessOk(in["access"].get<std::string>(), source, false, refusedOut)) return refusedOut;
        } else if (!tokenOk(in["token"].get<std::string>(), source)) return {403, "application/json", R"({"error":"lien invalide ou expiré"})"};
        auto p = peerById(in["id"].get<std::string>());
        if (!p || p->retired) {
            veyra::log::warn("guest", "answer refused: its peer was replaced or closed");
            return {409, "application/json", R"({"error":"connexion remplacée"})"};
        }
        if (!p->credential.admits({hashToken(in[byAccess ? "access" : "token"].get<std::string>()), !byAccess, source})) {
            refused(source, "credential for that connection");
            return {403, "application/json", R"({"error":"cette connexion n'est pas la tienne"})"};
        }
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
            r.at("set remote description");
            p->pc->setRemoteDescription(remote);
            veyra::log::info("guest", std::format("answer accepted peer={} videoPt={}", p->id, pt));
        } catch (const std::exception& e) {
            veyra::log::warn("guest", std::string("answer refused: ") + e.what());
            return {400, "application/json", R"({"error":"réponse invalide"})"};
        }
        return {204, "text/plain", ""};
    }

    std::shared_ptr<GuestPeer> createPeer(bool native, const Admission& access, const PeerCredential& credential, const std::string& name, const std::string& gid,
                                          const json& ice, const std::string& lease, const HttpRequest& r) {
        initRtcLogOnce();
        r.at("new peer");
        if (testStalls.fetch_sub(1) > 0) {
            veyra::log::warn("guest", "test: this peer is stuck until the share stops");
            for (;;) { { std::lock_guard lock(m); if (!active || stopping) return nullptr; } std::this_thread::sleep_for(std::chrono::milliseconds(50)); }
        }
        ShareSettings s; std::string bindIp;
        {
            std::lock_guard lock(m);
            s = settings; bindIp = tailscaleIp;
        }
        rtc::Configuration config;
        if (s.reach == Reach::Broker) config.iceServers = hostIceServers(ice);
        else config.bindAddress = bindIp.empty() ? std::string("127.0.0.1") : bindIp;
        config.mtu = 1280;
        config.disableAutoNegotiation = true;
        auto p = std::make_shared<GuestPeer>();
        p->id = randomToken().substr(0, 12);
        p->tag = randomToken().substr(0, 6);
        p->gid = gid;
        p->name = name;
        p->source = credential.source;
        p->credential = credential;
        p->native = native;
        p->accessId = access.accessId;
        p->watchOnly = access.watchOnly;
        p->lease = lease;
        p->createdUs = nowUs();
        try {
            p->pc = std::make_shared<rtc::PeerConnection>(config);
            std::weak_ptr<GuestPeer> weak = p;
            p->pc->onStateChange(lifeline.guard([weak](Impl& impl, rtc::PeerConnection::State state) {
                if (auto self = weak.lock()) { self->state = int(state); impl.controlCv.notify_all(); }
            }));
            p->pc->onGatheringStateChange(lifeline.guard([weak](Impl& impl, rtc::PeerConnection::GatheringState state) {
                if (state != rtc::PeerConnection::GatheringState::Complete) return;
                if (auto self = weak.lock()) { self->gathered = true; std::lock_guard lock(impl.gatherMutex); impl.gatherCv.notify_all(); }
            }));
            p->pc->onLocalCandidate([weak](rtc::Candidate candidate) {
                auto self = weak.lock();
                const auto type = candidate.type();
                if (!self || type == rtc::Candidate::Type::Host) return;
                auto& first = type == rtc::Candidate::Type::Relayed ? self->firstRelayUs : self->firstSrflxUs;
                int64_t none = 0;
                first.compare_exchange_strong(none, std::max<int64_t>(1, nowUs() - self->createdUs));
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
            packetizer->addToChain(std::make_shared<rtc::PliHandler>(lifeline.guard([weak](Impl& impl) {
                auto self = weak.lock();
                if (!self) return;
                veyra::log::info("guest", std::format("keyframe requested by viewer {} (PLI/FIR)", self->tag));
                if (const int slot = self->slot.load(); slot >= 0) impl.videos.at(size_t(slot)).requestKeyframe();
            })));
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
            p->room = p->pc->createDataChannel("room");
            p->input->onMessage(lifeline.guard([weak](Impl& impl, rtc::message_variant data) {
                const auto* bytes = std::get_if<rtc::binary>(&data);
                auto self = weak.lock();
                if (!bytes || !self) return;
                const auto pad = parseInputMessage(std::span<const std::byte>(bytes->data(), bytes->size()));
                if (!pad) return;
                const int64_t t = nowUs();
                self->heardUs = t;
                std::lock_guard lock(self->inputMutex);
                // Unordered channel: an older state arriving late never overwrites a newer one.
                if (self->haveInput && int16_t(pad->seq - self->latest.seq) <= 0) return;
                if (!self->changePending && (!self->haveInput || toControllerState(*pad) != toControllerState(self->latest))) {
                    self->changePending = true; self->changeSeq = pad->seq; self->changeRecvUs = t;
                }
                bool moved = !self->haveInput || pad->buttons != self->latest.buttons || pad->l2 != self->latest.l2 || pad->r2 != self->latest.r2;
                for (size_t i = 0; i < pad->axes.size(); ++i) moved |= std::abs(int(pad->axes[i]) - int(self->latest.axes[i])) > 2000;
                if (moved && !pad->synthetic) { self->activeUs = t; impl.lastPlayUs = t; }
                self->latest = *pad; self->latestRecvUs = t; self->haveInput = true; ++self->inputs;
                if (pad->connected && !pad->synthetic) {
                    if (!self->padSinceUs) self->padSinceUs = t;
                    if (pad->buttons || pad->l2 || pad->r2) self->buttonSeen = true;
                }
                // Every fourth state is acknowledged on arrival too, so the page measures its
                // input's trip even while no PS5 session reads it (handedUs 0).
                if (pad->seq % 4 == 0 && self->acks.size() < 256) self->acks.push_back({pad->seq, t, 0});
            }));
            p->stats->onMessage(lifeline.guard([weak](Impl& impl, rtc::message_variant data) {
                const auto* text = std::get_if<std::string>(&data);
                auto self = weak.lock();
                if (!text || !self || !admit(*self, Inbound::Stats, text->size())) return;
                const json in = json::parse(*text, nullptr, false);
                if (!in.is_object() || !in.contains("t") || !in["t"].is_string()) return;
                const auto kind = in["t"].get<std::string>();
                self->heardUs = nowUs();
                if (!admit(*self, inboundOf(kind))) return;
                if (kind == "bye") { self->left = true; impl.controlCv.notify_all(); return; }
                if (kind == "ping" && in.contains("c") && in["c"].is_number()) {
                    const auto reply = std::format(R"({{"t":"pong","c":{},"h":{}}})", in["c"].get<double>(), nowUs());
                    try { self->stats->send(reply); } catch (...) {}
                } else if (kind == "rep") {
                    impl.onReport(self, in);
                } else if (kind == "behind") {
                    impl.onBehind(self, in);
                } else if (kind == "want") {
                    impl.onWish(self, in);
                } else if (kind == "link") {
                    impl.onLink(self, in);
                } else if (kind == "probe") {
                    impl.startProbe(self);
                } else if (kind == "probe-stop") {
                    self->probeStop = true;
                }
            }));
            p->room->onOpen(lifeline.guard([weak](Impl& impl) { if (auto self = weak.lock()) { self->roomWanted = true; impl.controlCv.notify_all(); } }));
            p->room->onMessage(lifeline.guard([weak](Impl& impl, rtc::message_variant data) {
                const auto* text = std::get_if<std::string>(&data);
                auto self = weak.lock();
                if (!text || !self || text->size() > 512 || !admit(*self, Inbound::Room)) return;
                const json in = json::parse(*text, nullptr, false);
                if (!in.is_object() || !in.contains("t") || !in["t"].is_string()) return;
                impl.onRoomCommand(self, roomCommandOf(in["t"].get<std::string>()));
            }));
            r.at("local offer");
            p->pc->setLocalDescription(rtc::Description::Type::Offer);
        } catch (const std::exception& e) {
            veyra::log::error("guest", std::string("peer setup failed: ") + e.what());
            return nullptr;
        }
        std::shared_ptr<GuestPeer> previous;
        // The room's tags, read before `m` (roomMutex is never taken under it), for the eviction below.
        std::vector<std::string> memberTags;
        if (!gid.empty()) { std::lock_guard lock(roomMutex); for (const auto& member : room.members()) memberTags.push_back(member.id); }
        {
            std::lock_guard lock(m);
            if (!active || stopping) return nullptr;
            // The same page reconnecting takes its own place back in the room, even when the
            // connection it replaced is already gone.
            if (!gid.empty()) {
                for (const auto& old : peers)
                    if (old->gid == gid && !old->retired) { previous = old; break; }
                if (gidTags.size() >= 256 && !gidTags.contains(gid))
                    std::erase_if(gidTags, [&](const auto& e) {   // only ids whose tag nobody holds any more
                        const std::string& tag = e.second.tag;
                        return std::find(memberTags.begin(), memberTags.end(), tag) == memberTags.end() &&
                               std::none_of(peers.begin(), peers.end(), [&](const auto& q) { return q->tag == tag; });
                    });
                const auto [it, added] = gidTags.try_emplace(gid, GidTag{p->tag, p->credential.hash});
                if (!added) {
                    if (sameHash(it->second.hash, p->credential.hash)) p->tag = it->second.tag;
                    else previous.reset();   // another credential with this id: a new member, nobody replaced
                }
            }
            if (previous) previous->replaced = true;
            peers.push_back(p);
        }
        r.at("retire the previous peer");
        if (previous) retirePeer(previous, "replaced by its own new connection");
        veyra::log::info("guest", std::format("peer {} viewer {} offered bind={} reach={} iceServers={} client={}", p->id, p->tag, config.bindAddress.value_or("any"),
                                              reachName(s.reach), config.iceServers.size(), native ? "boovi" : "browser"));
        controlCv.notify_all();
        return p;
    }

    // Any thread but a libdatachannel callback, without m held. Our own state only, no call into
    // libdatachannel: the reaper says goodbye, closes and destroys the connection.
    void retirePeer(const std::shared_ptr<GuestPeer>& p, const char* why) {
        if (!p) return;
        bool wasConnected;
        {
            std::lock_guard life(p->lifeMutex);
            if (p->retired.exchange(true)) return;
            wasConnected = p->connectedHandled;
        }
        broker.endTurn(p->takeLease());
        { std::lock_guard lock(gatherMutex); gatherCv.notify_all(); }
        p->probeStop = true;
        if (const int slot = p->slot.exchange(-1); slot >= 0) {
            videos.at(size_t(slot)).setEnabled(false);
            std::lock_guard lock(slotMutex);
            slotPeer[size_t(slot)].reset();
        }
        reaper.bury(p, why);
        { std::lock_guard lock(m); std::erase(peers, p); }
        if (wasConnected && !p->replaced) { std::lock_guard lock(roomMutex); room.leave(p->tag, nowUs()); }
        syncAudio();
        controlCv.notify_all();
        veyra::log::info("guest", std::format("peer {} viewer {} closed: {}", p->id, p->tag, why));
    }

    // One Opus encoder for every viewer: on while any is connected. The decision reads the peers
    // again under audioMutex, so a viewer connecting while another retires never ends silent.
    void syncAudio() {
        std::lock_guard lock(audioMutex);
        const bool wanted = !livePeers().empty();
        if (wanted == audioOn) return;
        audioOn = wanted;
        if (wanted) audio.start(); else audio.stop();
    }

    // The viewer's budget for what it sends (InboundBudget.h); past it, the message is dropped.
    static bool admit(GuestPeer& p, Inbound kind, size_t bytes = 0) {
        std::lock_guard lock(p.inboundMutex);
        return p.inbound.admit(kind, nowUs(), bytes);
    }

    // ---- the room ----
    void onRoomCommand(const std::shared_ptr<GuestPeer>& p, RoomCommand c) {
        if (c == RoomCommand::None || !p->connectedHandled) return;
        RoomRefusal refusal = RoomRefusal::None;
        {
            std::lock_guard lock(roomMutex);
            const int64_t now = nowUs();
            switch (c) {
            case RoomCommand::Ask: refusal = room.ask(p->tag, now); break;
            case RoomCommand::Cancel: room.cancel(p->tag); break;
            case RoomCommand::Release: room.release(p->tag, now); break;
            case RoomCommand::Roll: refusal = room.roll(p->tag, now, secureRandom); break;
            default: break;
            }
        }
        bool tell = refusal != RoomRefusal::None;
        if (tell) {
            // A page repeating a refused command hears the refusal once.
            const int64_t now = nowUs();
            std::lock_guard lock(p->inboundMutex);
            tell = refusal != p->lastRefusal || now - p->lastRefusalUs >= kRefusalRepeatUs;
            p->lastRefusal = refusal;
            p->lastRefusalUs = now;
        }
        if (tell && p->room && p->room->isOpen()) {
            try { p->room->send(std::format(R"({{"t":"refused","why":"{}"}})", refusalName(refusal))); } catch (...) {}
        }
        controlCv.notify_all();
    }
    static uint32_t secureRandom(uint32_t bound) {
        // Rejection sampling: no modulo bias.
        const uint32_t limit = UINT32_MAX - UINT32_MAX % bound;
        for (;;) {
            uint32_t v = 0;
            BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&v), sizeof(v), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
            if (v < limit) return v % bound;
        }
    }
    // Control thread: the room's clock, its snapshots, who holds the controller, the upload's share.
    void syncRoom() {
        const auto live = livePeers();
        const int64_t now = nowUs();
        std::vector<std::pair<std::shared_ptr<GuestPeer>, std::string>> out;
        std::string holderTag;
        int idleMinutes;
        std::vector<std::string> backed, orphans;
        for (const auto& p : live) backed.push_back(p->tag);
        {
            std::lock_guard lock(roomMutex);
            orphans = room.sweep(backed, now, kOrphanUs);
            idleMinutes = rules.idleMinutes;
            // The holder's pad still past the idle limit while someone waits: it passes on.
            if (const auto h = room.holder(); h && idleMinutes > 0 && !room.queue().empty())
                for (const auto& p : live)
                    if (p->tag == *h) {
                        int64_t active;
                        { std::lock_guard l(p->inputMutex); active = std::max(p->activeUs, p->createdUs); }
                        if (now - active > int64_t(idleMinutes) * 60'000'000) room.release(*h, now, Release::Idle);
                    }
            room.tick(now);
            const bool changed = room.revision() != sentRev || roomDirty;
            sentRev = room.revision();
            roomDirty = false;
            for (const auto& p : live)
                if (changed || p->roomWanted.exchange(false)) out.emplace_back(p, snapshotJson(room, p->tag, now, rules.historyToGuests ? history : std::vector<PlayRecord>{}));
            for (auto& e : room.drainEvents()) if (events.size() < 512) events.push_back(std::move(e));
            holderTag = room.holder().value_or("");
        }
        for (const auto& tag : orphans) veyra::log::info("guest-room", std::format("viewer {} has had no connection for {} s: it leaves the room", tag, kOrphanUs / 1000000));
        for (const auto& [p, text] : out)
            if (p->room && p->room->isOpen()) { try { p->room->send(text); } catch (...) {} }
        std::shared_ptr<GuestPeer> h;
        for (const auto& p : live) if (p->tag == holderTag) h = p;
        bool holderChanged;
        std::shared_ptr<GuestPeer> previous;
        {
            std::lock_guard lock(holderMutex);
            holderChanged = holder != h;
            previous = std::exchange(holder, h);
        }
        // The one who lost the controller stops rumbling.
        if (holderChanged && previous && !previous->retired && !previous->native && previous->feedback && previous->feedback->isOpen()) {
            try { previous->feedback->send(R"({"t":"rumble","l":0,"r":0})"); } catch (...) {}
        }
        if (holderChanged) {
            if (h) { std::lock_guard l(h->inputMutex); h->changePending = false; h->acks.clear(); }
            veyra::log::info("guest-room", std::format("controller: {}", h ? "viewer " + h->tag : std::string("free")));
        }
        if (holderChanged || !out.empty() || rebalanceWanted.exchange(false) || videos.liveSource() != lastLive) rebalance(live, h);
    }
    // Each viewer's part of the host's upload (shareUpload), and the player's latency-first ladder.
    void rebalance(const std::vector<std::shared_ptr<GuestPeer>>& live, const std::shared_ptr<GuestPeer>& h) {
        const Limit limit = [&] { std::lock_guard lock(m); return settings.limit; }();
        int holderIndex = -1;
        for (size_t i = 0; i < live.size(); ++i) if (live[i] == h) holderIndex = int(i);
        const auto shares = shareUpload(limit.kbps, limit.kbps, live.size(), holderIndex);
        // Real pictures first for the player, on a live source only: there they come before frame
        // generation; a file hands real pictures over at their present, where frame generation
        // can leave none.
        const bool plays = mode.load() != ControlMode::WatchOnly && videos.liveSource();
        lastLive = videos.liveSource();
        for (size_t i = 0; i < live.size(); ++i) {
            {
                std::lock_guard lock(live[i]->qualityMutex);
                live[i]->shareKbps = shares[i];
                live[i]->latencyFirst = plays && int(i) == holderIndex;
                live[i]->rate.setLatencyFirst(live[i]->latencyFirst, nowUs());
                live[i]->rate.setLimit({limit.feed, limit.size, shares[i]});
            }
            applyQuality(live[i], "share of the upload");
        }
    }

    void onReport(const std::shared_ptr<GuestPeer>& p, const json& in) {
        auto num = [&](const char* key) { return in.contains(key) && in[key].is_number() ? in[key].get<double>() : -1.0; };
        double sentFps;
        bool logIt;
        {
            std::lock_guard lock(p->statsMutex);
            p->report = in; sentFps = p->fps;
            logIt = nowUs() - p->reportLoggedUs >= kReportLogUs;
            if (logIt) p->reportLoggedUs = nowUs();
        }
        // The page's loss and network time over its last 2 s window (GuestQuality.h).
        { std::lock_guard lock(p->qualityMutex); p->rate.report(std::max(0.0, num("loss")), num("net"), nowUs()); }
        // A decoder that takes most of the frame interval, or shows fewer pictures than arrive
        // while its buffer grows, is about to pile pictures up: two such reports (4 s) in a row.
        const double dec = num("dec"), shown = num("fps"), jb = num("jb");
        const bool slow = sentFps > 10 && ((dec > 0 && dec > 0.85 * 1000.0 / sentFps) || (shown >= 0 && shown < 0.85 * sentFps && jb > 50));
        p->decodeSlowReports = slow ? p->decodeSlowReports + 1 : 0;
        if (p->decodeSlowReports >= 2) {
            p->decodeSlowReports = 0;
            stepDownForDecoder(p, std::format("decode {:.1f} ms for {:.0f} pictures a second, {:.0f} shown, buffer {:.0f} ms", dec, sentFps, shown, jb));
        }
        applyQuality(p, "report");
        if (!logIt) return;
        std::string text = in.dump();
        if (text.size() > 600) {
            size_t cut = 600;
            while (cut && (uint8_t(text[cut]) & 0xC0) == 0x80) --cut;
            text.resize(cut);
            text += " ...";
        }
        veyra::log::info("guest-report", std::format("viewer {} {}", p->tag, text));
    }
    // The page says its pictures pile up (GuestQuality.h, decoderBehind): a keyframe makes the
    // browser drop what waits for its decoder, and automatique sends less.
    void onBehind(const std::shared_ptr<GuestPeer>& p, const json& in) {
        auto num = [&](const char* key) { return in.contains(key) && in[key].is_number() ? in[key].get<double>() : -1.0; };
        const int64_t now = nowUs();
        if (now - p->behindKeyUs < 1500000) return;
        p->behindKeyUs = now;
        if (const int slot = p->slot.load(); slot >= 0) videos.at(size_t(slot)).requestKeyframe();
        veyra::log::info("guest", std::format("viewer {} behind: buffer {:.0f} ms, decode {:.1f} ms, {:.0f} shown a second; keyframe to catch up", p->tag, num("jb"), num("dec"), num("fps")));
        if (now - p->decodeStepUs > 4000000) stepDownForDecoder(p, "pictures pile up");
    }
    void stepDownForDecoder(const std::shared_ptr<GuestPeer>& p, const std::string& why) {
        bool stepped;
        Quality q;
        { std::lock_guard lock(p->qualityMutex); stepped = p->rate.decoderBehind(); q = p->rate.current(); }
        p->decodeStepUs = nowUs();
        if (stepped) veyra::log::info("guest-quality", std::format("viewer {} decoder behind ({}): down to feed={} size={}", p->tag, why, feedName(q.feed), sizeName(q.size)));
        applyQuality(p, "decoder behind");
    }
    // The viewer's choice: automatique, or a feed, size and bitrate cap of their own.
    void onWish(const std::shared_ptr<GuestPeer>& p, const json& in) {
        auto text = [&](const char* key) { return in.contains(key) && in[key].is_string() ? in[key].get<std::string>() : std::string(); };
        Wish wish;
        wish.automatic = !in.contains("auto") || !in["auto"].is_number() || in["auto"].get<int>() != 0;
        wish.feed = parseFeed(text("feed"), Feed::Smooth120);
        wish.size = parseSize(text("size"), Size::Native);
        if (in.contains("mbps") && in["mbps"].is_number()) wish.kbps = uint32_t(std::clamp(in["mbps"].get<double>(), 4.0, 200.0) * 1000);
        { std::lock_guard lock(p->qualityMutex); p->rate.setWish(wish, nowUs()); }
        veyra::log::info("guest-quality", std::format("viewer {} wants {} feed={} size={} kbps={}", p->tag, wish.automatic ? "auto" : "manual", feedName(wish.feed), sizeName(wish.size), wish.kbps));
        applyQuality(p, "guest choice");
    }
    // The page's link test result.
    void onLink(const std::shared_ptr<GuestPeer>& p, const json& in) {
        auto num = [&](const char* key) { return in.contains(key) && in[key].is_number() ? in[key].get<double>() : -1.0; };
        const double kbps = num("kbps");
        if (kbps <= 0) return;
        {
            std::lock_guard lock(p->qualityMutex);
            p->rate.linkTested(uint32_t(std::min(kbps, 2e6)), nowUs());
            p->linkRtt = num("rtt"); p->linkLoss = num("loss");
        }
        veyra::log::info("guest-quality", std::format("viewer {} link test {:.1f} Mb/s rtt={:.1f} ms loss={:.2f}%", p->tag, kbps / 1000, num("rtt"), std::max(0.0, num("loss")) * 100));
        applyQuality(p, "link test");
    }
    // The viewer's encoder follows its controller's mode.
    void applyQuality(const std::shared_ptr<GuestPeer>& p, const char* why) {
        Quality q;
        {
            std::lock_guard lock(p->qualityMutex);
            q = p->rate.current();
            // A BooVI client generates its own frames: it always gets the real pictures.
            if (p->native) q.feed = Feed::Real;
            if (q == p->applied) return;
            const bool modeChanged = q.feed != p->applied.feed || q.size != p->applied.size;
            p->applied = q;
            if (modeChanged) veyra::log::info("guest-quality", std::format("viewer {} feed={} size={} kbps={} link={} share={}{} ({})", p->tag, feedName(q.feed), sizeName(q.size), q.kbps,
                                                                           p->rate.linkKbps(), p->shareKbps, p->latencyFirst ? " plays" : "", why));
            else veyra::log::info("guest", std::format("viewer {} bitrate {} kbps ({})", p->tag, q.kbps, why));
        }
        if (const int slot = p->slot.load(); slot >= 0) videos.at(size_t(slot)).setQuality(sizeHeight(q.size), feedFps(q.feed), q.feed != Feed::Real, q.kbps);
        if (p->connectedHandled) sendStatus(p);
    }

    // ---- the link test: a rising ramp of padding, 300 ms a step, from 4 Mb/s up to 1.5x the
    // host's upload (at most 200 Mb/s). The page measures what arrives and stops it early. ----
    void startProbe(const std::shared_ptr<GuestPeer>& p) {
        std::lock_guard lock(p->probeMutex);
        if (!p->probe || p->retired || p->probing.exchange(true)) return;
        if (p->probeThread.joinable()) p->probeThread.join();
        p->probeStop = false;
        const uint32_t top = [&] { std::lock_guard l(m); return std::clamp(settings.limit.kbps * 3 / 2, 20000u, 200000u); }();
        std::weak_ptr<GuestPeer> weak = p;
        p->probeThread = std::thread([limit = testLimit, weak, top] {
            auto self = weak.lock();
            if (!self) return;
            runProbe(*self, top, limit.get());
            self->probing = false;
        });
    }
    static void runProbe(GuestPeer& p, uint32_t topKbps, TestLimit* testLimit) {
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
        for (size_t step = 0; step < steps.size() && !p.probeStop && !p.retired; ++step, ++done) {
            const int64_t start = nowUs();
            uint64_t stepBytes = 0;
            for (;;) {
                const int64_t elapsed = nowUs() - start;
                if (elapsed >= 300000 || p.probeStop || p.retired) break;
                const uint64_t due = uint64_t(double(steps[step]) * double(elapsed) / 8000.0);
                while (stepBytes < due) {
                    if (p.probe->bufferedAmount() > (1u << 20)) { ++skipped; break; }   // the sender's own limit
                    const double hostMs = double(nowUs()) / 1000.0;
                    const uint16_t count = uint16_t(steps.size());
                    message[0] = std::byte('Q'); message[1] = std::byte(step);
                    std::memcpy(message.data() + 2, &count, 2);
                    std::memcpy(message.data() + 4, &seq, 4);
                    std::memcpy(message.data() + 8, &hostMs, 8);
                    std::memcpy(message.data() + 16, &steps[step], 4);
                    ++seq; stepBytes += message.size();
                    if (testLimit && !testLimit->pass(message.size() + 48)) continue;
                    try { p.probe->send(message); } catch (...) { p.probeStop = true; break; }
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
        if (p.stats && p.stats->isOpen()) {
            try { p.stats->send(std::format(R"({{"t":"probe-end","n":{},"steps":{}}})", seq, done)); } catch (...) {}
        }
        veyra::log::info("guest-quality", std::format("viewer {} link test sent {} steps up to {} kbps, {:.1f} MB in {:.0f} ms, sender-limited {} times{}",
            p.tag, done, done ? steps[done - 1] : 0, double(sentBytes) / 1e6, double(nowUs() - began) / 1000.0, skipped, p.probeStop ? " (stopped by the page)" : ""));
    }

    // ---- control thread: lifecycle, the room, and the 1 s statistics ----
    void controlLoop() {
        SetThreadDescription(GetCurrentThread(), L"Guest control");
        int64_t lastTick = nowUs(), lastLog = 0;
        std::unique_lock lock(m);
        while (active && !stopping) {
            controlStage = "idle";
            controlBeatUs = nowUs();
            controlCv.wait_for(lock, std::chrono::milliseconds(100));
            if (!active || stopping) break;
            controlBeatUs = nowUs();
            auto all = peers;
            const bool renew = std::exchange(renewRequested, false);
            if (renew) token = randomToken();
            const auto kicks = std::exchange(kickTags, {});
            const auto drops = std::exchange(dropAccessIds, {});
            for (const auto& p : all)
                if (p->accessId.empty() && std::find(kicks.begin(), kicks.end(), p->tag) != kicks.end()) banned.emplace_back(p->source, nowUs() + kKickBanUs);
            lock.unlock();
            controlStage = "requests";
            for (const auto& p : all) {
                if (std::find(kicks.begin(), kicks.end(), p->tag) != kicks.end()) retirePeer(p, "kick");
                else if (renew && p->accessId.empty()) retirePeer(p, "link renewed");
                else if (!p->accessId.empty() && std::find(drops.begin(), drops.end(), p->accessId) != drops.end()) retirePeer(p, "visit ended");
            }
            controlStage = "viewers";
            for (const auto& p : all) {
                if (p->retired) continue;
                const auto state = rtc::PeerConnection::State(p->state.load());
                if (state == rtc::PeerConnection::State::Connected && !p->connectedHandled) onConnected(p);
                const bool dead = state == rtc::PeerConnection::State::Failed || state == rtc::PeerConnection::State::Closed ||
                                  state == rtc::PeerConnection::State::Disconnected;
                const bool late = !p->connectedHandled && nowUs() - p->createdUs > kConnectTimeoutUs;
                // The page sends its pad every 50 ms (every second at worst, from a background
                // tab): silence this long means it is gone even before ICE notices.
                const bool silent = p->connectedHandled && nowUs() - p->heardUs.load() > kSilentUs;
                if (dead || late || silent || p->left.load())
                    retirePeer(p, p->left.load() ? "guest left" : dead ? "connection lost" : silent ? "guest silent" : "never connected");
            }
            controlStage = "room";
            syncRoom();
            controlStage = "statistics";
            const auto live = livePeers();
            for (const auto& p : live) sendAcks(p);
            const int64_t t = nowUs();
            if (t - lastTick >= 1000000) {
                for (const auto& p : live) tick(p, double(t - lastTick) / 1e6);
                lastTick = t;
                if (t - lastLog >= 5000000) {
                    lastLog = t;
                    for (const auto& p : live) {
                        logSummary(p);
                        uint64_t dropped;
                        { std::lock_guard l(p->inboundMutex); dropped = p->inbound.takeDropped(); }
                        if (dropped) veyra::log::warn("guest", std::format("viewer {} sent {} messages past its budget in 5 s: dropped", p->tag, dropped));
                    }
                    logIdleEncoders(live);
                }
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
        // A direct path: the lease goes back now, and the rest of it is refunded.
        if (!p->relayed) broker.endTurn(p->takeLease());
        const Limit limit = [&] { std::lock_guard l(m); return settings.limit; }();
        int slot = -1;
        const char* refusal = nullptr;
        {
            std::lock_guard life(p->lifeMutex);
            if (p->retired) return;
            {
                std::lock_guard lock(slotMutex);
                for (size_t i = 0; i < slotPeer.size() && slot < 0; ++i) if (slotPeer[i].expired()) { slot = int(i); slotPeer[i] = p; }
            }
            if (slot >= 0 && testConnectHoldMs > 0) std::this_thread::sleep_for(std::chrono::milliseconds(testConnectHoldMs));
            RoomRefusal joined = RoomRefusal::None;
            if (slot >= 0) {
                std::lock_guard lock(roomMutex);
                if (!room.find(p->tag)) joined = room.join(Member{p->tag, p->name, p->watchOnly ? Seat::Watcher : rules.defaultSeat, p->native, p->accessId}, nowUs());
            }
            if (slot < 0) refusal = "no encoder left";
            else if (joined != RoomRefusal::None) {
                std::lock_guard lock(slotMutex);
                slotPeer[size_t(slot)].reset();
                refusal = "room full";
            } else {
                {
                    // A new viewer starts in automatique on real pictures until its page says more.
                    std::lock_guard lock(p->qualityMutex);
                    p->rate = RateControl{};
                    p->rate.setLimit(limit);
                    p->applied = {Feed::Real, Size::P1080, 0};
                }
                auto& video = videos.at(size_t(slot));
                p->slot = slot;
                if (testLimit) testLimit->armed(nowUs());
                video.setQuality(1080, 60, false, defaultKbps(Feed::Real, Size::P1080));
                video.setEnabled(true);
                video.requestKeyframe();
                p->heardUs = nowUs();
                p->connectedHandled = true;
                p->roomWanted = true;
            }
        }
        if (refusal) { retirePeer(p, refusal); return; }
        syncAudio();
        lastPlayUs = nowUs();
        applyQuality(p, "guest connected");
        veyra::log::info("guest", std::format("guest connected peer={} viewer={} encoder={} candidate={} local={} relayed={} client={}{} after {:.0f} ms",
            p->id, p->tag, slot, p->candidate, candidateName(local.type()), p->relayed, p->native ? "boovi" : "browser",
            p->accessId.empty() ? std::string() : " access=" + p->accessId, double(nowUs() - p->createdUs) / 1000.0));
        sendStatus(p);
    }
    // An encoder that encodes for no connected viewer: a slot that was never given back.
    void logIdleEncoders(const std::vector<std::shared_ptr<GuestPeer>>& live) {
        for (size_t i = 0; i < GuestVideoSet::kSlots; ++i) {
            if (std::any_of(live.begin(), live.end(), [&](const auto& p) { return p->slot.load() == int(i); })) continue;
            if (const uint32_t n = unsent[i][0].exchange(0); n > 30) veyra::log::warn("guest", std::format("encoder {} encoded {} pictures in 5 s for no viewer", i, n));
        }
    }
    void sendAcks(const std::shared_ptr<GuestPeer>& p) {
        std::vector<GuestPeer::Ack> batch;
        { std::lock_guard lock(p->inputMutex); batch.swap(p->acks); }
        if (batch.empty() || !p->stats || !p->stats->isOpen()) return;
        std::string text = R"({"t":"ack","a":[)";
        for (size_t i = 0; i < batch.size(); ++i)
            text += std::format("{}[{},{},{}]", i ? "," : "", batch[i].seq, batch[i].recvUs, batch[i].handedUs);
        text += "]}";
        try { p->stats->send(text); } catch (...) {}
    }
    // The fluide feeds' extra time over real pictures, ms, when both were measured in the last
    // two minutes (any viewer); -1 otherwise.
    double fluideCostMs() {
        std::lock_guard lock(costMutex);
        const int64_t now = nowUs();
        if (lastReal < 0 || lastSmooth < 0 || now - lastRealUs > 120'000'000 || now - lastSmoothUs > 120'000'000) return -1;
        return std::max(0.0, lastSmooth - lastReal);
    }
    void sendStatus(const std::shared_ptr<GuestPeer>& p) {
        if (!p || !p->stats || !p->stats->isOpen()) return;
        const int slot = p->slot.load();
        const auto v = slot >= 0 ? videos.at(size_t(slot)).stats() : VideoStats{};
        Quality q; Limit limit; Wish wish; bool automatic, plays; uint32_t link, tested; Quality best;
        {
            std::lock_guard lock(p->qualityMutex);
            q = p->rate.current(); limit = p->rate.limit(); wish = p->rate.wish(); automatic = p->rate.automatic();
            link = p->rate.linkKbps(); tested = p->rate.testedKbps(); best = p->rate.best(); plays = p->latencyFirst;
        }
        double sendFps, sendMbps;
        { std::lock_guard lock(p->statsMutex); sendFps = p->fps; sendMbps = p->mbps; }
        const auto text = std::format(
            R"({{"t":"st","mode":"{}","kbps":{},"w":{},"h":{},"src":{},"fps":{:.1f},"mbps":{:.2f},"feed":"{}","size":"{}","auto":{},"link":{},"tested":{},)"
            R"("max":{{"feed":"{}","size":"{}","kbps":{}}},"want":{{"feed":"{}","size":"{}","kbps":{}}},"best":{{"feed":"{}","size":"{}"}},"plays":{},"fluideMs":{:.1f}}})",
            modeName(mode.load()), q.kbps, v.width, v.height, v.sourceHeight, sendFps, sendMbps, feedName(q.feed), sizeName(q.size), automatic ? 1 : 0, link, tested,
            feedName(limit.feed), sizeName(limit.size), limit.kbps, feedName(wish.feed), sizeName(wish.size), wish.kbps, feedName(best.feed), sizeName(best.size),
            plays ? 1 : 0, fluideCostMs());
        try { p->stats->send(text); } catch (...) {}
    }
    void tick(const std::shared_ptr<GuestPeer>& p, double seconds) {
        if (!p->retired) { if (const auto rtt = p->pc->rtt()) p->sctpRttMs = double(rtt->count()); }
        double real = -1, smooth = -1;
        {
            std::lock_guard lock(p->statsMutex);
            p->fps = p->framesWindow / seconds;
            p->mbps = double(p->bytesWindow) * 8 / seconds / 1e6;
            p->framesWindow = p->bytesWindow = 0;
            if (p->presentToSent.size() >= 30) {
                p->windowPresentToSent = median(p->presentToSent); p->windowEncode = median(p->encode); p->windowDecodedToSent = median(p->decodedToSent);
                p->windowDated = p->decodedToSent.size(); p->windowUndated = p->undatedWindow;
                p->presentToSent.clear(); p->encode.clear(); p->decodedToSent.clear(); p->undatedWindow = 0;
            }
            if (p->decodedReal.size() >= 10) { real = median(p->decodedReal); p->decodedReal.clear(); }
            if (p->decodedSmooth.size() >= 10) { smooth = median(p->decodedSmooth); p->decodedSmooth.clear(); }
        }
        if (real >= 0 || smooth >= 0) {
            std::lock_guard lock(costMutex);
            if (real >= 0) { lastReal = real; lastRealUs = nowUs(); }
            if (smooth >= 0) { lastSmooth = smooth; lastSmoothUs = nowUs(); }
        }
        sendStatus(p);
    }
    void logSummary(const std::shared_ptr<GuestPeer>& p) {
        std::vector<double> handed;
        remoteplay::ControllerState last; uint64_t count = 0; bool synthetic = false; uint32_t received = 0;
        GuestPad pad;
        {
            std::lock_guard lock(p->inputMutex);
            handed.swap(p->recvToHandedMs); last = p->lastHanded; count = p->inputs; synthetic = p->latest.synthetic; received = p->latest.buttons; pad = p->latest;
        }
        Quality q; uint32_t link;
        { std::lock_guard lock(p->qualityMutex); q = p->rate.current(); link = p->rate.linkKbps(); }
        const int slot = p->slot.load();
        const auto v = slot >= 0 ? videos.at(size_t(slot)).stats() : VideoStats{};
        double real, smooth, realAge, smoothAge;
        {
            // Each tap's last median with its age: a value from before this window is not this window's.
            std::lock_guard lock(costMutex);
            const int64_t now = nowUs();
            real = lastReal; smooth = lastSmooth;
            realAge = lastReal < 0 ? -1 : double(now - lastRealUs) / 1e6;
            smoothAge = lastSmooth < 0 ? -1 : double(now - lastSmoothUs) / 1e6;
        }
        bool holds;
        { std::lock_guard lock(holderMutex); holds = holder == p; }
        std::lock_guard lock(p->statsMutex);
        veyra::log::info("guest-latency", std::format(
            "viewer {} encoder {} unsent {}/{} host presentToSent p50={:.2f} ms encode p50={:.2f} ms decodedToSent p50={:.2f} ms (dated {}, undated {}) (real {:.2f} age {:.0f} s, fluide {:.2f} age {:.0f} s) fps={:.1f} sent={:.2f} Mb/s target={} kbps feed={} size={} {}x{} link={} kbps holds={} | input recvToHandedPs5 p50={:.2f} ms changes={} inputs={} synthetic={} last lx={} ly={} rx={} ry={} buttons=0x{:X} received buttons=0x{:X}",
            p->tag, slot, slot >= 0 ? unsent[size_t(slot)][0].exchange(0) : 0u, slot >= 0 ? unsent[size_t(slot)][1].exchange(0) : 0u, p->windowPresentToSent, p->windowEncode, p->windowDecodedToSent, p->windowDated, p->windowUndated, real, realAge, smooth, smoothAge, p->fps, p->mbps,
            q.kbps, feedName(q.feed), sizeName(q.size), v.width, v.height, link, holds,
            median(handed), handed.size(), count, synthetic, last.leftX, last.leftY, last.rightX, last.rightY, last.buttons, received));
        if (pad.version)
            veyra::log::info("guest-latency", std::format("viewer {} extended pad v{} motion={} gyro=({:.2f},{:.2f},{:.2f}) accel=({:.2f},{:.2f},{:.2f}) samples={} touch=({},{},{}) rx={}",
                p->tag, pad.version, pad.motionValid, pad.gyro[0], pad.gyro[1], pad.gyro[2], pad.accel[0], pad.accel[1], pad.accel[2], pad.motionSampleCount,
                pad.touches[0].id, pad.touches[0].x, pad.touches[0].y, pad.axes[2]));
    }

    ViewerStatus viewerStatus(const std::shared_ptr<GuestPeer>& p) {
        ViewerStatus out;
        out.tag = p->tag; out.name = p->name; out.accessId = p->accessId; out.nativeClient = p->native;
        out.connected = p->connectedHandled.load();
        if (out.connected) {
            out.remote = p->remote; out.candidate = p->candidate; out.relayed = p->relayed;
            out.rttMs = p->sctpRttMs.load();
        }
        if (const int slot = p->slot.load(); slot >= 0) {
            const auto v = videos.at(size_t(slot)).stats();
            out.encoder = v.encoder; out.width = v.width; out.height = v.height; out.sourceHeight = v.sourceHeight;
        }
        {
            std::lock_guard lock(p->qualityMutex);
            out.quality = p->rate.current(); out.automatic = p->rate.automatic(); out.shareKbps = p->shareKbps;
            out.linkKbps = p->rate.testedKbps(); out.linkRttMs = p->linkRtt; out.linkLossPercent = p->linkLoss < 0 ? -1 : p->linkLoss * 100;
            out.targetKbps = out.quality.kbps;
        }
        {
            std::lock_guard lock(p->inputMutex);
            out.inputs = p->inputs; out.padConnected = p->haveInput && p->latest.connected; out.synthetic = p->haveInput && p->latest.synthetic;
            out.padSilent = out.padConnected && !out.synthetic && !p->buttonSeen && p->padSinceUs && nowUs() - p->padSinceUs > kPadSilentUs;
            if (out.connected) out.idleSeconds = double(nowUs() - std::max(p->activeUs, p->createdUs)) / 1e6;
        }
        std::lock_guard lock(p->statsMutex);
        out.frames = p->framesSent; out.fps = p->fps; out.sentMbps = p->mbps;
        out.presentToSentMs = p->windowPresentToSent; out.encodeMs = p->windowEncode; out.decodedToSentMs = p->windowDecodedToSent;
        const auto& rep = p->report;
        auto num = [&](const char* key) { return rep.is_object() && rep.contains(key) && rep[key].is_number() ? rep[key].get<double>() : -1.0; };
        out.presentToDisplayMs = num("disp");
        out.decodedToDisplayMs = num("src");
        out.receiveToDisplayMs = num("rxdisp");
        out.decodeMs = num("dec");
        out.inputToHostMs = num("inhost");
        out.inputToPs5Ms = num("inps5");
        out.displayHz = num("hz");
        out.guestFps = num("fps");
        out.lossPercent = std::max(0.0, num("loss")) * 100;
        auto text = [&](const char* key) { return rep.is_object() && rep.contains(key) && rep[key].is_string() ? rep[key].get<std::string>().substr(0, 64) : std::string(); };
        out.padName = text("pad");
        out.padMapping = text("map");
        if (num("rtt") >= 0) out.rttMs = num("rtt");
        return out;
    }
};

GuestStreamer::GuestStreamer() : p_(std::make_shared<Impl>()) {}
GuestStreamer::~GuestStreamer() { stop(); }

engine::EarlyCapture& GuestStreamer::capture() { return p_->videos; }

bool GuestStreamer::start(const ShareSettings& settings, std::string& error) {
    auto& p = *p_;
    if (active()) return true;
    const bool viaBroker = settings.reach == Reach::Broker;
    const auto cloudflare = viaBroker ? loadCloudflareConfig() : std::nullopt;
    const Relay relay = !cloudflare ? Relay::None : !cloudflare->ownerKey.empty() ? Relay::Owner : Relay::OwnKey;
    std::string roomKey = viaBroker ? loadOrCreateRoomKey() : std::string();
    const std::string url = brokerUrl();
    if (viaBroker && (url.empty() || roomKey.empty())) {
        error = url.empty() ? "cette version n'a pas de broker : choisis le tailnet" : "clé de salon illisible";
        std::lock_guard lock(p.m); p.error = error; return false;
    }
    const std::string ip = tailscaleAddress();
    {
        std::lock_guard lock(p.m);
        p.settings = settings;
        p.tailscaleIp = ip;
        p.token = randomToken();
        p.error.clear();
        p.relay = relay;
        p.cloudflareIce = json::array();
        p.banned.clear();
        p.kickTags.clear(); p.dropAccessIds.clear(); p.renewRequested = false;
        p.gidTags.clear();
    }
    {
        std::lock_guard lock(p.roomMutex);
        p.room = Room{};
        p.room.configure(settings.rules.room);
        p.rules = settings.rules;
        p.sentRev = ~0ull;
        p.events.clear();
    }
    p.ownerPriority = settings.rules.ownerPriority;
    p.lastPlayUs = nowUs();
    p.testLimit = TestLimit::fromEnvironment();
    // Once per process: a share the watchdog restarts is not stuck again.
    static std::once_flag stallArmed;
    std::call_once(stallArmed, [&] { if (const char* v = std::getenv("VEYRA_TEST_GUEST_STALL"); v && *v) p.testStalls = std::atoi(v); });
    if (const char* v = std::getenv("VEYRA_TEST_GUEST_CONNECT_HOLD_MS"); v && *v) p.testConnectHoldMs = std::atoi(v);
    if (p.testLimit) veyra::log::warn("guest", std::format("test bottleneck {} kbps from {:.0f} s after the guest connects", p.testLimit->kbps, double(p.testLimit->afterUs) / 1e6));
    auto handler = [impl = p_.get()](const HttpRequest& r) { return impl->handle(r); };
    if (viaBroker) {
        const bool opened = p.broker.start(url, roomKey, relay == Relay::Owner ? cloudflare->ownerKey : std::string(), handler, error);
        SecureZeroMemory(roomKey.data(), roomKey.size());
        if (!opened) { std::lock_guard lock(p.m); p.error = error; p.token.clear(); return false; }
    } else {
        // The first free port from the configured one, so a second BooVI still shares.
        std::vector<std::string> addresses{"127.0.0.1"};
        if (!ip.empty()) addresses.push_back(ip);
        bool listening = false;
        for (uint16_t port = settings.port; port <= settings.port + 9 && !listening; ++port) listening = p.http.start(addresses, port, handler, error);
        if (!listening) { std::lock_guard lock(p.m); p.error = error; p.token.clear(); return false; }
    }
    {
        std::lock_guard lock(p.m);
        p.active = true; p.stopping = false;
        p.settings.port = viaBroker ? 0 : p.http.port();
    }
    if (relay == Relay::OwnKey) {
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
    p.controlBeatUs = nowUs();
    p.stallLogged = false;
    p.control = std::thread([impl = p_.get()] { impl->controlLoop(); });
    std::string where;
    for (const auto& a : p.http.bound()) where += (where.empty() ? "" : ",") + a;
    veyra::log::info("guest", std::format("sharing started reach={} relay={} port={} listen={} tailscale={} max feed={} size={} upload={} kbps viewers={} turn={} s dice={}",
        reachName(settings.reach), relayName(relay), p.http.port(), where.empty() ? "-" : where, ip.empty() ? "down" : "up",
        feedName(settings.limit.feed), sizeName(settings.limit.size), settings.limit.kbps, settings.rules.room.maxMembers, settings.rules.room.turnUs / 1000000,
        settings.rules.room.diceAllowed));
    return true;
}

void GuestStreamer::stop() {
    auto& p = *p_;
    std::vector<std::shared_ptr<GuestPeer>> last;
    {
        std::lock_guard lock(p.m);
        if (!p.active) return;
        p.stopping = true;
        last = p.peers;
    }
    p.controlCv.notify_all();
    if (p.control.joinable()) p.control.join();
    if (p.turnFetch.joinable()) p.turnFetch.join();
    // Every viewer's lease goes back before the room closes.
    for (const auto& peer : last) p.retirePeer(peer, "sharing stopped");
    p.broker.stop();
    p.http.stop();
    for (size_t i = 0; i < GuestVideoSet::kSlots; ++i) p.videos.at(i).setEnabled(false);
    { std::lock_guard lock(p.audioMutex); if (p.audioOn.exchange(false)) p.audio.stop(); }
    { std::lock_guard lock(p.holderMutex); p.holder.reset(); }
    {
        std::lock_guard lock(p.roomMutex);
        for (auto& e : p.room.drainEvents()) if (p.events.size() < 512) p.events.push_back(std::move(e));
    }
    {
        std::lock_guard lock(p.m);
        p.active = false; p.stopping = false;
        p.token.clear();   // the link dies with the share session
        p.cloudflareIce = json::array();
        p.peers.clear();
    }
    veyra::log::info("guest", "sharing stopped");
}

bool GuestStreamer::active() const { std::lock_guard lock(p_->m); return p_->active; }

std::string brokerRoomLink() {
    const std::string url = brokerUrl();
    if (url.empty()) return {};
    std::string key = loadOrCreateRoomKey();
    if (key.empty()) return {};
    const std::string id = roomIdOf(key);
    SecureZeroMemory(key.data(), key.size());
    return (url.ends_with('/') ? url : url + "/") + id;
}

Relay configuredRelay() {
    const auto config = loadCloudflareConfig();
    return !config ? Relay::None : !config->ownerKey.empty() ? Relay::Owner : Relay::OwnKey;
}

void GuestStreamer::kick(const std::string& tag) {
    { std::lock_guard lock(p_->m); if (!p_->active) return; p_->kickTags.push_back(tag); }
    p_->controlCv.notify_all();
    veyra::log::info("guest", "kick requested for viewer " + tag);
}

void GuestStreamer::renewLink() {
    { std::lock_guard lock(p_->m); if (!p_->active) return; p_->renewRequested = true; }
    p_->controlCv.notify_all();
    veyra::log::info("guest", "new share link requested; share-link viewers go");
}

void GuestStreamer::dropAccess(const std::string& accessId) {
    { std::lock_guard lock(p_->m); if (!p_->active) return; p_->dropAccessIds.push_back(accessId); }
    p_->controlCv.notify_all();
}

void GuestStreamer::setMode(ControlMode mode) {
    p_->mode = mode;
    veyra::log::info("guest", std::format("control mode {}", modeName(mode)));
    p_->rebalanceWanted = true;   // the player's ladder follows the mode; the control thread tells the pages
    p_->controlCv.notify_all();
}
ControlMode GuestStreamer::mode() const { return p_->mode.load(); }

void GuestStreamer::setAccessDesk(AccessDesk* desk) { p_->desk = desk; }

void GuestStreamer::setLimit(const Limit& limit) {
    { std::lock_guard lock(p_->m); p_->settings.limit = limit; }
    veyra::log::info("guest-quality", std::format("host maximum feed={} size={} upload={} kbps", feedName(limit.feed), sizeName(limit.size), limit.kbps));
    p_->rebalanceWanted = true;
    p_->controlCv.notify_all();
}

void GuestStreamer::setRules(const RoomRules& rules) {
    { std::lock_guard lock(p_->m); p_->settings.rules = rules; }
    {
        std::lock_guard lock(p_->roomMutex);
        p_->rules = rules;
        p_->room.configure(rules.room);
    }
    p_->ownerPriority = rules.ownerPriority;
    p_->controlCv.notify_all();
}

void GuestStreamer::give(const std::string& tag) {
    { std::lock_guard lock(p_->roomMutex); p_->room.give(tag, nowUs()); }
    p_->controlCv.notify_all();
}
void GuestStreamer::takeBack() {
    { std::lock_guard lock(p_->roomMutex); p_->room.ownerTake(nowUs()); }
    p_->controlCv.notify_all();
}
void GuestStreamer::handBack() {
    { std::lock_guard lock(p_->roomMutex); p_->room.ownerRelease(nowUs()); }
    p_->controlCv.notify_all();
}
void GuestStreamer::setSeat(const std::string& tag, Seat seat) {
    { std::lock_guard lock(p_->roomMutex); p_->room.setSeat(tag, seat, nowUs()); }
    p_->controlCv.notify_all();
}
void GuestStreamer::roll() {
    RoomRefusal r;
    { std::lock_guard lock(p_->roomMutex); r = p_->room.roll("", nowUs(), Impl::secureRandom); }
    if (r != RoomRefusal::None) veyra::log::info("guest-room", std::format("host's roll refused: {}", refusalName(r)));
    p_->controlCv.notify_all();
}
void GuestStreamer::setHistory(std::vector<PlayRecord> plays) {
    std::lock_guard lock(p_->roomMutex);
    p_->history = std::move(plays);
    if (p_->rules.historyToGuests) p_->roomDirty = true;
}
std::vector<RoomEvent> GuestStreamer::drainEvents() {
    std::lock_guard lock(p_->roomMutex);
    return std::exchange(p_->events, {});
}

ShareStatus GuestStreamer::status() const {
    auto& p = *p_;
    ShareStatus out;
    std::vector<std::shared_ptr<GuestPeer>> all;
    {
        std::lock_guard lock(p.m);
        out.token = p.token; out.port = p.settings.port; out.tailscaleIp = p.tailscaleIp; out.error = p.error;
        out.reach = p.settings.reach; out.relay = p.relay; out.limit = p.settings.limit;
        all = p.peers;
        if (!p.active) out.port = 0;
        bool connected = false, connecting = false;
        for (const auto& peer : all) { connected |= peer->connectedHandled.load(); connecting |= !peer->connectedHandled.load(); }
        out.phase = !p.active ? Phase::Off : connected ? Phase::Connected : connecting ? Phase::Connecting : Phase::Waiting;
    }
    out.mode = p.mode.load();
    out.broker = p.broker.phase();
    // A request past its deadline, or a control loop that stopped: nothing else will come through.
    const bool stalled = out.phase != Phase::Off && nowUs() - p.controlBeatUs.load() > 5000000;
    out.controlAlive = !stalled;
    if (stalled && !p.stallLogged.exchange(true))
        veyra::log::error("guest", std::format("the share's control loop has not run for {:.0f} s, at \"{}\"", double(nowUs() - p.controlBeatUs.load()) / 1e6, p.controlStage.load()));
    out.stuck = stalled || p.broker.overdue() > 0;
    if (out.stuck) out.error = "le partage ne répond plus : redémarre BooVI";
    if (out.reach == Reach::Broker && out.phase != Phase::Off) out.publicUrl = p.broker.roomLink();
    std::vector<std::string> order;
    {
        std::lock_guard lock(p.roomMutex);
        const auto h = p.room.holder();
        out.room.holder = h.value_or("");
        out.room.heldSeconds = h ? double(nowUs() - p.room.heldSinceUs()) / 1e6 : 0;
        out.room.ownerHolds = p.room.ownerHolds();
        out.room.queue.assign(p.room.queue().begin(), p.room.queue().end());
        out.room.dice = p.room.dice();
        if (out.room.dice) out.room.diceAgeSeconds = double(nowUs() - out.room.dice->atUs) / 1e6;
        out.room.rules = p.rules;
        for (const auto& m : p.room.members()) order.push_back(m.id);
    }
    std::vector<ViewerStatus> viewers;
    for (const auto& peer : all) {
        auto v = p.viewerStatus(peer);
        {
            std::lock_guard lock(p.roomMutex);
            if (const Member* m = p.room.find(v.tag)) { v.seat = m->seat; v.name = m->name; }
            v.queuePosition = p.room.queuePosition(v.tag);
        }
        v.holder = !v.tag.empty() && v.tag == out.room.holder;
        out.sentMbps += v.sentMbps;
        viewers.push_back(std::move(v));
    }
    // The room's order, then those still connecting.
    for (const auto& tag : order)
        for (auto& v : viewers) if (v.tag == tag && v.connected) out.viewers.push_back(v);
    for (auto& v : viewers) if (!v.connected) out.viewers.push_back(v);
    if (out.error.empty())
        for (size_t i = 0; i < GuestVideoSet::kSlots; ++i)
            if (const auto s = p.videos.at(i).stats(); !s.error.empty()) { out.error = s.error; break; }
    out.playIdleSeconds = double(nowUs() - p.lastPlayUs.load()) / 1e6;
    return out;
}

std::optional<remoteplay::ControllerState> GuestStreamer::guestState(remoteplay::HostTime now) {
    auto& p = *p_;
    std::shared_ptr<GuestPeer> h;
    { std::lock_guard lock(p.holderMutex); h = p.holder; }
    if (!h || h->retired || !h->connectedHandled) return std::nullopt;
    std::lock_guard lock(h->inputMutex);
    if (!h->haveInput || now / 10 - h->latestRecvUs > kGuestStale / 10) return remoteplay::ControllerState{};
    return toControllerState(h->latest);
}

void GuestStreamer::hostPad(const remoteplay::ControllerState& state, remoteplay::HostTime) {
    auto& p = *p_;
    const bool press = (state.buttons & ~p.hostButtons) != 0 || (state.l2 > 64 && p.hostL2 <= 64) || (state.r2 > 64 && p.hostR2 <= 64);
    p.hostButtons = state.buttons; p.hostL2 = state.l2; p.hostR2 = state.r2;
    if (!press || !p.ownerPriority.load() || p.mode.load() != ControlMode::GuestPlays) return;
    {
        std::lock_guard lock(p.roomMutex);
        if (p.room.ownerHolds()) { p.room.ownerActive(nowUs()); return; }
        if (!p.room.holder()) return;
        p.room.ownerTake(nowUs());
    }
    veyra::log::info("guest-room", "the owner's pad takes the controller back");
    p.controlCv.notify_all();
}

void GuestStreamer::handedToPs5(const remoteplay::ControllerState& sent) {
    auto& p = *p_;
    std::shared_ptr<GuestPeer> h;
    { std::lock_guard lock(p.holderMutex); h = p.holder; }
    if (!h || !h->connectedHandled) return;
    std::lock_guard lock(h->inputMutex);
    h->lastHanded = sent;
    if (!h->changePending) return;
    h->changePending = false;
    const int64_t handedUs = nowUs();   // after the session took it, not the pass's start
    if (h->acks.size() < 256) h->acks.push_back({h->changeSeq, h->changeRecvUs, handedUs});
    if (h->recvToHandedMs.size() < 20000) h->recvToHandedMs.push_back(double(handedUs - h->changeRecvUs) / 1000.0);
}

void GuestStreamer::feedback(const remoteplay::ControllerFeedback& feedback) {
    auto& p = *p_;
    if (p.mode.load() == ControlMode::WatchOnly) return;
    std::shared_ptr<GuestPeer> h;
    { std::lock_guard lock(p.holderMutex); h = p.holder; }
    if (!h || !h->connectedHandled || !h->feedback || !h->feedback->isOpen()) return;
    // A BooVI client drives a real pad: rumble, adaptive triggers and haptics, as one binary message.
    if (h->native) {
        if (!feedback.rumble && !feedback.triggers && !feedback.motionReset && feedback.haptics.empty()) return;
        const auto bytes = encodeFeedbackMessage(feedback);
        try { h->feedback->send(bytes.data(), bytes.size()); } catch (...) {}
        return;
    }
    if (!feedback.rumble) return;
    {
        std::lock_guard lock(h->inputMutex);
        if (feedback.left == h->rumbleL && feedback.right == h->rumbleR) return;
        h->rumbleL = feedback.left; h->rumbleR = feedback.right;
    }
    try { h->feedback->send(std::format(R"({{"t":"rumble","l":{},"r":{}}})", feedback.left, feedback.right)); } catch (...) {}
}

} // namespace veyra::guest
