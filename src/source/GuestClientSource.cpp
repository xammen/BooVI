// Fork, "rejoindre un partage": see GuestClientSource.h. The receive side follows
// XboxSessionSource (same decoder, audio session and engine contract); the signaling is the
// guest page's (web/guest/index.html), answered natively.
#include "veyra/source/GuestClientSource.h"

#include <windows.h>
#include <winhttp.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <mmreg.h>

#include <opus/opus.h>
#include <rtc/rtc.hpp>
#include <nlohmann/json.hpp>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
}

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <format>
#include <mutex>
#include <numbers>
#include <optional>
#include <thread>
#include <vector>

#include "veyra/Log.h"
#include "veyra/guest/BrokerSocket.h"
#include "veyra/guest/GuestProtocol.h"
#include "veyra/guest/TurnTunnel.h"
#include "veyra/pipeline/ColorMetadata.h"
#include "veyra/sink/AudioFormat.h"
#include "veyra/source/CaptureCompressedDecoder.h"
#include "veyra/xbox/VideoDecodeRecovery.h"

#pragma comment(lib, "winhttp.lib")

namespace veyra::source {
namespace {

using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

int64_t host100ns() { return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count() / 100; }
int64_t nowUs() { return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count(); }

std::wstring widen(const std::string& text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0);
    std::wstring out(size_t(std::max(size, 0)), L'\0');
    if (size > 0) MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), out.data(), size);
    return out;
}

double median(std::vector<double> v) {
    if (v.empty()) return -1;
    std::nth_element(v.begin(), v.begin() + ptrdiff_t(v.size() / 2), v.end());
    return v[v.size() / 2];
}

double movingAverage(double current, double sample) { return current > 0 ? current * 0.95 + sample * 0.05 : sample; }

pipeline::SourcePixelFormat pixelFormatOf(const AVFrame& frame) {
    if ((frame.format == AV_PIX_FMT_D3D12 || frame.format == AV_PIX_FMT_D3D11) && frame.hw_frames_ctx) {
        const auto* context = reinterpret_cast<const AVHWFramesContext*>(frame.hw_frames_ctx->data);
        return context->sw_format == AV_PIX_FMT_NV12 ? pipeline::SourcePixelFormat::NV12
             : context->sw_format == AV_PIX_FMT_P010 ? pipeline::SourcePixelFormat::P010 : pipeline::SourcePixelFormat::Unknown;
    }
    switch (frame.format) {
    case AV_PIX_FMT_NV12: return pipeline::SourcePixelFormat::NV12;
    case AV_PIX_FMT_YUV420P: return pipeline::SourcePixelFormat::Yuv420P;
    default: return pipeline::SourcePixelFormat::Unknown;
    }
}

// The host's NVENC stream is 8-bit H.264 SDR, BT.709 limited range (GuestNv12.hlsl).
pipeline::ColorDescription streamColor() {
    pipeline::ColorDescription c;
    c.pixelFormat = pipeline::SourcePixelFormat::NV12;
    c.range = pipeline::ColorRange::Limited;
    c.rangeAssumed = c.matrixAssumed = c.transferAssumed = c.primariesAssumed = true;
    c.matrix = pipeline::YuvMatrix::BT709;
    c.transfer = pipeline::TransferFunction::BT709;
    c.primaries = pipeline::ColorPrimaries::BT709;
    c.preserveSdrCodeValues = true;
    return c;
}

AVFrame* allocNv12(unsigned width, unsigned height) {
    AVFrame* frame = av_frame_alloc();
    if (!frame) return nullptr;
    frame->format = AV_PIX_FMT_NV12;
    frame->width = int(width);
    frame->height = int(height);
    if (av_frame_get_buffer(frame, 32) < 0) { av_frame_free(&frame); return nullptr; }
    return frame;
}

bool startsDecoding(const std::vector<uint8_t>& unit) {
    for (size_t i = 0; i + 3 < unit.size(); ++i) {
        if (unit[i] == 0 && unit[i + 1] == 0 && unit[i + 2] == 1) {
            const uint8_t type = unit[i + 3] & 0x1F;
            if (type == 5 || type == 7) return true;
            i += 2;
        }
    }
    return false;
}

// One request to the host's own server, http or https (WinHTTP: the system's roots and proxy).
struct HttpResult { int status = 0; std::string body; std::string error; };
HttpResult httpRequest(const guest::ShareLink& link, const wchar_t* method, const wchar_t* path, const std::string& body) {
    HttpResult out;
    HINTERNET session = WinHttpOpen(L"BooVI", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) { out.error = std::format("winhttp open {}", GetLastError()); return out; }
    WinHttpSetTimeouts(session, 5000, 5000, 8000, 8000);
    HINTERNET connection = WinHttpConnect(session, widen(link.host).c_str(), link.port, 0);
    HINTERNET request = connection ? WinHttpOpenRequest(connection, method, path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                        link.https ? WINHTTP_FLAG_SECURE : 0) : nullptr;
    const wchar_t* headers = L"Content-Type: application/json\r\n";
    if (!request || !WinHttpSendRequest(request, headers, DWORD(-1), const_cast<char*>(body.data()), DWORD(body.size()), DWORD(body.size()), 0) ||
        !WinHttpReceiveResponse(request, nullptr)) {
        out.error = std::format("winhttp {}", GetLastError());
    } else {
        DWORD status = 0, size = sizeof(status);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
        out.status = int(status);
        for (;;) {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(request, &available) || !available) break;
            const size_t at = out.body.size();
            out.body.resize(at + available);
            DWORD read = 0;
            if (!WinHttpReadData(request, out.body.data() + at, available, &read)) { out.body.resize(at); break; }
            out.body.resize(at + read);
            if (out.body.size() > 1 << 20) break;
        }
    }
    if (request) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return out;
}

// The host's signaling (/session, /answer): its own server, or its room on BooVI's broker
// (GuestRoom.h), where each request goes as {id, path, body} and its reply comes back as
// {id, status, body}. The room's socket stays open from the first request to the last.
class Signaling {
public:
    explicit Signaling(const guest::ShareLink& link) : link_(link) {}
    ~Signaling() { socket_.close(); }

    HttpResult call(const wchar_t* path, const std::string& body) {
        if (link_.room.empty()) return httpRequest(link_, L"POST", path, body);
        HttpResult out;
        if (!open_ && !open(out)) return out;
        const int id = ++next_;
        const std::string name = path == std::wstring(L"/session") ? "/session" : "/answer";
        if (!socket_.send(json{{"id", id}, {"path", name}, {"body", json::parse(body, nullptr, false)}}.dump())) { out.error = "room closed"; return out; }
        // The host answers at once, or after its ICE gathering (3 s at most): past 15 s, the room is dead.
        std::mutex m;
        std::condition_variable cv;
        bool done = false;
        std::thread watchdog([&] { std::unique_lock lock(m); if (!cv.wait_for(lock, std::chrono::seconds(15), [&] { return done; })) socket_.close(); });
        std::string text;
        uint16_t code = 0;
        while (socket_.receive(text, code)) {
            const json in = json::parse(text, nullptr, false);
            if (!in.is_object()) continue;
            if (in.contains("t") && in["t"] == "closed") { out.error = in.value("why", std::string("closed")); break; }
            if (in.value("id", 0) != id) continue;
            out.status = in.value("status", 0);
            out.body = in.value("body", std::string());
            break;
        }
        if (out.status == 0 && out.error.empty()) out.error = code == 4404 ? "offline" : code == 4429 ? "busy" : std::format("room closed ({})", code);
        { std::lock_guard lock(m); done = true; }
        cv.notify_all();
        watchdog.join();
        if (!out.error.empty()) open_ = false;
        return out;
    }

private:
    bool open(HttpResult& out) {
        int status = 0;
        if (!socket_.open(widen(link_.host), link_.port, link_.https, widen("/api/room/" + link_.room), L"", status, out.error)) {
            out.status = status;
            return false;
        }
        open_ = true;
        return true;
    }

    guest::ShareLink link_;
    guest::BrokerSocket socket_;
    bool open_ = false;
    int next_ = 0;
};

// An older link (https://<host>/?a=…) that a broker now answers: the room its host keeps for it.
void findLegacyRoom(guest::ShareLink& link) {
    if (!link.room.empty() || !link.https) return;
    const auto reply = httpRequest(link, L"GET", L"/api/legacy", std::string());
    const json in = json::parse(reply.body, nullptr, false);
    if (reply.status != 200 || !in.is_object() || !in.contains("room") || !in["room"].is_string()) return;
    const std::string room = in["room"].get<std::string>();
    if (room.size() != 22 || room.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_") != std::string::npos) return;
    link.room = room;
    log::info("guest-client", "the link's host answers through its broker room");
}

bool envOn(const char* name) { char v[4]{}; return GetEnvironmentVariableA(name, v, 4) && v[0] == '1'; }

// The host's /session "ice" list: STUN, then Cloudflare's TURN over UDP (what libjuice relays).
// Tests: VEYRA_TEST_GUEST_CLIENT_NO_UDP_TURN=1 sends UDP TURN to 192.0.2.1 (TEST-NET-1, nothing
// answers), as a network that drops UDP to the relay.
std::vector<rtc::IceServer> iceServersFrom(const json& list) {
    std::vector<rtc::IceServer> out;
    if (!list.is_array()) return out;
    const bool noUdpTurn = envOn("VEYRA_TEST_GUEST_CLIENT_NO_UDP_TURN");
    for (const auto& server : list) {
        if (!server.is_object() || !server.contains("urls")) continue;
        const json urls = server["urls"].is_array() ? server["urls"] : json::array({server["urls"]});
        const bool credentials = server.contains("username") && server["username"].is_string() && server.contains("credential") && server["credential"].is_string();
        for (const auto& url : urls) {
            if (!url.is_string()) continue;
            const std::string u = url.get<std::string>();
            try {
                if (u.starts_with("stun:")) { out.emplace_back(u); continue; }
                if (!credentials || !u.starts_with("turn:") || u.find("transport=udp") == std::string::npos) continue;
                const std::string hostPort = u.substr(5, u.find('?') - 5);
                const size_t colon = hostPort.rfind(':');
                if (colon == std::string::npos) continue;
                out.emplace_back(noUdpTurn ? std::string("192.0.2.1") : hostPort.substr(0, colon), uint16_t(std::atoi(hostPort.c_str() + colon + 1)),
                                 server["username"].get<std::string>(), server["credential"].get<std::string>(), rtc::IceServer::RelayType::TurnUdp);
            } catch (const std::exception&) {}
        }
    }
    return out;
}

// The same list's TURN over a stream, as a browser falls back to: TLS on 443 first, then any
// TLS, then TCP. Empty host when there is none.
struct StreamTurn { std::string host, username, credential; uint16_t port = 0; bool tls = false; };
StreamTurn streamTurnFrom(const json& list) {
    StreamTurn best;
    int bestRank = 0;
    if (!list.is_array()) return best;
    for (const auto& server : list) {
        if (!server.is_object() || !server.contains("urls") || !server.contains("username") || !server["username"].is_string() ||
            !server.contains("credential") || !server["credential"].is_string()) continue;
        const json urls = server["urls"].is_array() ? server["urls"] : json::array({server["urls"]});
        for (const auto& url : urls) {
            if (!url.is_string()) continue;
            const std::string u = url.get<std::string>();
            const bool tls = u.starts_with("turns:");
            if (!tls && !(u.starts_with("turn:") && u.find("transport=tcp") != std::string::npos)) continue;
            const size_t from = tls ? 6 : 5, query = u.find('?');
            const std::string hostPort = u.substr(from, query == std::string::npos ? std::string::npos : query - from);
            const size_t colon = hostPort.rfind(':');
            const uint16_t port = colon == std::string::npos ? uint16_t(tls ? 5349 : 3478) : uint16_t(std::atoi(hostPort.c_str() + colon + 1));
            const int rank = tls && port == 443 ? 3 : tls ? 2 : 1;
            if (rank <= bestRank || !port) continue;
            bestRank = rank;
            best = {hostPort.substr(0, colon), server["username"].get<std::string>(), server["credential"].get<std::string>(), port, tls};
        }
    }
    return best;
}

// An SDP without its non-relay candidate lines.
std::string relayCandidatesOnly(const std::string& sdp) {
    std::string out;
    size_t at = 0;
    while (at < sdp.size()) {
        size_t end = sdp.find('\n', at);
        end = end == std::string::npos ? sdp.size() : end + 1;
        const std::string_view line(sdp.data() + at, end - at);
        if (!line.starts_with("a=candidate:") || line.find(" typ relay") != std::string_view::npos) out.append(line);
        at = end;
    }
    return out;
}

const char* candidateTypeName(rtc::Candidate::Type t) {
    switch (t) {
    case rtc::Candidate::Type::Host: return "host";
    case rtc::Candidate::Type::ServerReflexive: return "srflx";
    case rtc::Candidate::Type::PeerReflexive: return "prflx";
    case rtc::Candidate::Type::Relayed: return "relay";
    default: return "?";
    }
}
const char* transportName(rtc::Candidate::TransportType t) {
    return t == rtc::Candidate::TransportType::Udp ? "udp" : t == rtc::Candidate::TransportType::Unknown ? "?" : "tcp";
}
// "host 3, srflx 1, relay 2": a description's candidates by type, no address.
std::string candidateSummary(std::optional<rtc::Description> d) {
    if (!d) return "none";
    int n[5]{};
    for (const auto& c : d->extractCandidates()) ++n[std::min(int(c.type()), 4)];
    std::string out;
    const char* names[5] = {"?", "host", "srflx", "prflx", "relay"};
    for (int i = 1; i < 5; ++i) if (n[i]) out += std::format("{}{} {}", out.empty() ? "" : ", ", names[i], n[i]);
    return out.empty() ? "none" : out;
}

// The decoding GPU: its vendor ("amd", "nvidia", "intel") for the stats and the host's report,
// its name for this log.
struct DecodeGpu { std::string vendor, name; };
DecodeGpu decodeGpuOf(ID3D12Device* device) {
    DecodeGpu out{"none", {}};
    if (!device) return out;
    Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    DXGI_ADAPTER_DESC1 desc{};
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || FAILED(factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&adapter))) ||
        FAILED(adapter->GetDesc1(&desc))) return {"unknown", {}};
    out.vendor = desc.VendorId == 0x1002 ? "amd" : desc.VendorId == 0x10DE ? "nvidia" : desc.VendorId == 0x8086 ? "intel" : std::format("0x{:04X}", desc.VendorId);
    for (const wchar_t* c = desc.Description; *c; ++c) out.name.push_back(*c < 128 ? char(*c) : '?');
    return out;
}

bool syntheticEnv() {
    static const bool on = [] { char v[4]{}; return GetEnvironmentVariableA("VEYRA_TEST_GUEST_CLIENT_SYNTH", v, 4) && v[0] == '1'; }();
    return on;
}

} // namespace

struct GuestClientSource::Impl {
    GuestClientDesc desc;
    guest::ShareLink link;
    std::string peerId;
    // TURN over TLS for libjuice (TurnTunnel.h); before pc, so the peer goes first.
    std::unique_ptr<guest::TurnTunnel> tunnel;
    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::Track> video, audio;
    std::shared_ptr<rtc::DataChannel> input, feedbackChannel, statsChannel;
    // Callbacks run on libdatachannel's threads; close() shuts this gate first.
    struct Gate { std::recursive_mutex mutex; bool open = true; };
    std::shared_ptr<Gate> gate = std::make_shared<Gate>();
    std::mutex stateMutex;
    std::condition_variable stateCv;
    std::atomic<bool> connected{false}, failed{false}, gathered{false}, ended{false};
    std::wstring endReason;
    std::atomic<bool> extendedInput{false};
    std::atomic<bool> relayed{false};
    std::string controlMode;

    // Video: the WebRTC thread queues access units; the decode thread drains them.
    struct Unit { std::vector<uint8_t> data; uint32_t rtp; int64_t arrival; bool inputLoss = false; };
    std::mutex queueMutex;
    std::condition_variable queueCv;
    std::deque<Unit> queue;
    bool stopDecode = false;
    std::thread decodeThread;
    CaptureCompressedDecoder decoder;
    bool decoderOpen = false;
    std::atomic<bool> hardwareDecode{false};
    AVFrame* dummyTarget = nullptr;
    unsigned width = 1920, height = 1080;
    Clock::time_point lastKeyframeRequest{};

    // Per picture: what the host said of it ("f" on the stats channel) and what happened here.
    struct Meta { uint32_t rtp = 0; int64_t captureUs = 0, sourceUs = 0, heardUs = 0; bool received = false; };
    std::mutex metaMutex;
    std::deque<Meta> metas;                       // host announcements, newest last
    std::deque<uint32_t> arrived;                 // RTP stamps of pictures received; the host announces each one just after it
    struct Shown { int64_t pts100 = 0, arrival = 0; uint32_t rtp = 0; };
    std::deque<Shown> decodedPictures;            // decode order, for videoPresented
    std::vector<double> hostToScreen, receiveToScreen;
    int64_t clockOffsetUs = 0;                    // host clock - this clock
    int64_t transit100 = 100000, stampToHost100 = 0;   // pictureHostTime100ns: host -> arrival, stamp -> host time
    bool transitKnown = false, stampKnown = false;
    int64_t bestRttUs = -1, bestRttAtUs = 0;
    bool clockKnown = false;

    // Audio. The host counts Opus samples from 0, apart from the picture's clock: the first
    // packet is placed at the newest picture's time, the RTP stamps (48 kHz) run from there.
    OpusDecoder* opus = nullptr;
    std::vector<float> pcm;
    std::atomic<int64_t> videoPts100{-1};
    int64_t audioBase100 = -1;
    uint32_t audioFirstRtp = 0;
    bool audioDiscontinuity = true, audioReady = false;

    // Input and feedback
    std::mutex inputMutex;
    uint16_t seq = 0;
    std::atomic<uint64_t> inputsSent{0};
    std::mutex feedbackMutex;
    remoteplay::ControllerFeedback feedback;
    bool feedbackPending = false;

    std::atomic<uint64_t> units{0}, decoded{0}, dropped{0}, errors{0}, keyframes{0}, videoBytes{0};
    std::mutex statsMutex;
    double decodeMs = 0, hostToScreenMs = -1, lossPercent = 0;
    std::string decoderName, gpuVendor;           // CaptureCompressedDecoder::backendName(), decodeGpuOf()
    mutable Clock::time_point rateStart = Clock::now();
    mutable uint64_t rateUnits = 0, rateDecoded = 0, rateBytes = 0;
    mutable double receivedFps = 0, decodedFps = 0, videoMbps = 0;

    ~Impl() {
        if (dummyTarget) av_frame_free(&dummyTarget);
        if (opus) opus_decoder_destroy(opus);
    }

    bool sendText(const std::shared_ptr<rtc::DataChannel>& channel, const std::string& text) {
        if (!channel || !channel->isOpen()) return false;
        try { channel->send(text); return true; } catch (const std::exception&) { return false; }
    }
    void requestKeyframe() {
        const auto now = Clock::now();
        if (now - lastKeyframeRequest < std::chrono::milliseconds(300)) return;
        lastKeyframeRequest = now;
        ++keyframes;
        try { if (video) video->requestKeyframe(); } catch (const std::exception&) {}
    }

    void onStats(const std::string& text) {
        const json in = json::parse(text, nullptr, false);
        if (!in.is_object() || !in.contains("t") || !in["t"].is_string()) return;
        const auto kind = in["t"].get<std::string>();
        auto num = [&](const char* key) { return in.contains(key) && in[key].is_number() ? in[key].get<double>() : 0.0; };
        if (kind == "f") {
            Meta m{uint32_t(num("r")), int64_t(num("c")), int64_t(num("d")), nowUs(), false};
            std::lock_guard lock(metaMutex);
            m.received = std::find(arrived.rbegin(), arrived.rend(), m.rtp) != arrived.rend();
            metas.push_back(m);
            if (metas.size() > 512) metas.pop_front();
        } else if (kind == "pong") {
            const int64_t sentUs = int64_t(num("c")), hostUs = int64_t(num("h")), back = nowUs();
            const int64_t rtt = back - sentUs;
            if (rtt < 0 || rtt > 2000000) return;
            std::lock_guard lock(metaMutex);
            // The fastest round trip of the last 10 s bounds the offset best.
            if (!clockKnown || rtt <= bestRttUs || back - bestRttAtUs > 10000000) {
                clockOffsetUs = hostUs - (sentUs + back) / 2;
                bestRttUs = rtt; bestRttAtUs = back; clockKnown = true;
            }
        } else if (kind == "st") {
            std::lock_guard lock(stateMutex);
            if (in.contains("mode") && in["mode"].is_string()) controlMode = in["mode"].get<std::string>().substr(0, 16);
        } else if (kind == "bye") {
            const std::string why = in.contains("why") && in["why"].is_string() ? in["why"].get<std::string>() : "";
            log::info("guest-client", "the host closed the connection: " + why.substr(0, 64));
            std::lock_guard lock(stateMutex);
            if (!ended) {
                endReason = why == "kick" ? L"l'hôte a coupé l'invité" : why == "sharing stopped" ? L"l'hôte a arrêté le partage" : L"l'hôte a fermé la connexion";
                ended = true;
            }
            stateCv.notify_all();
        }
    }
    void onFeedback(const rtc::message_variant& data) {
        remoteplay::ControllerFeedback f;
        if (const auto* bytes = std::get_if<rtc::binary>(&data)) {
            auto parsed = guest::parseFeedbackMessage(std::span<const std::byte>(bytes->data(), bytes->size()));
            if (!parsed) return;
            f = std::move(*parsed);
        } else if (const auto* text = std::get_if<std::string>(&data)) {
            // The browser's form: {"t":"rumble","l":0..255,"r":0..255}.
            const json in = json::parse(*text, nullptr, false);
            if (!in.is_object() || in.value("t", std::string()) != "rumble") return;
            f.rumble = true;
            f.left = uint8_t(std::clamp(in.value("l", 0), 0, 255));
            f.right = uint8_t(std::clamp(in.value("r", 0), 0, 255));
        }
        std::lock_guard lock(feedbackMutex);
        if (feedbackPending) feedback.merge(std::move(f));
        else { feedback = std::move(f); feedbackPending = true; }
    }
};

GuestClientSource::GuestClientSource() : p_(std::make_unique<Impl>()) {
    frameEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
}

GuestClientSource::~GuestClientSource() {
    close();
    if (frameEvent_) CloseHandle(frameEvent_);
}

void GuestClientSource::fail(std::wstring message) {
    std::lock_guard lock(mutex_);
    stats_.state = GuestClientStats::State::Failed;
    stats_.message = std::move(message);
    if (!initialized_) { initialized_ = true; started_ = false; ready_.notify_all(); }
}

bool GuestClientSource::connect(GuestClientDesc desc) {
    close();
    {
        std::lock_guard lock(mutex_);
        initialized_ = started_ = false;
        latest_.reset();
        view_.reset();
        sequence_ = skipped_ = 0;
        openFlagPending_ = true;
        stats_ = {};
        stats_.state = GuestClientStats::State::Connecting;
        info_ = {};
    }
    p_ = std::make_unique<Impl>();
    cancel_ = false;
    owner_ = std::jthread([this, request = std::move(desc)]() mutable { run(std::move(request)); });
    std::unique_lock lock(mutex_);
    ready_.wait(lock, [&] { return initialized_; });
    if (!started_) {
        lock.unlock();
        close();
        return false;
    }
    info_ = publishedInfo_;
    return true;
}

void GuestClientSource::run(GuestClientDesc desc) {
    SetThreadDescription(GetCurrentThread(), L"Guest client");
    auto& p = *p_;
    p.desc = std::move(desc);
    const auto cancelled = [&] { return cancel_.load() || (p.desc.stopRequested && p.desc.stopRequested->load()); };
    const auto link = guest::parseShareLink(p.desc.link);
    p.desc.link.assign(p.desc.link.size(), '\0');
    p.desc.link.clear();
    if (!link) { fail(L"lien de partage invalide"); return; }
    p.link = *link;
    { std::lock_guard lock(mutex_); stats_.host = link->host; stats_.message = L"connexion à l'hôte…"; }
    findLegacyRoom(p.link);
    log::info("guest-client", std::format("joining {}://{}:{}{} as a BooVI client", p.link.https ? "https" : "http", p.link.host, p.link.port,
                                          p.link.room.empty() ? "" : " through the broker"));
    Signaling signaling(p.link);

    // 1. Offer from the host. A friend's access knocks until the host's PC lets the visit in
    //    (202 with its state: asking the host, waking the PS5, connecting).
    const char* credentialKey = p.link.access ? "access" : "token";
    HttpResult session;
    for (;;) {
        session = signaling.call(L"/session", json{{credentialKey, p.link.token}, {"client", "boovi"}, {"input", guest::kExtendedVersion}, {"feed", "real"}}.dump());
        if (cancelled() || !p.link.access || session.status != 202 || !session.error.empty()) break;
        const json wait = json::parse(session.body, nullptr, false);
        const std::string state = wait.is_object() && wait.contains("state") && wait["state"].is_string() ? wait["state"].get<std::string>() : "";
        const std::string host = wait.is_object() && wait.contains("host") && wait["host"].is_string() && !wait["host"].get<std::string>().empty() ? wait["host"].get<std::string>() : "ton ami";
        const std::wstring message = state == "asking" ? L"en attente de l'accord de " + widen(host)
                                   : state == "waking" ? std::wstring(L"réveil de la ps5…") : std::wstring(L"connexion…");
        { std::lock_guard lock(mutex_); stats_.message = message; if (host != "ton ami") stats_.hostName = host.substr(0, 24); }
        for (int i = 0; i < 10 && !cancelled(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (cancelled()) { fail(L"annulé"); return; }
    if (p.link.access && session.error.empty() && session.status != 200) {
        const json end = json::parse(session.body, nullptr, false);
        const std::string state = end.is_object() && end.contains("state") && end["state"].is_string() ? end["state"].get<std::string>() : "";
        const std::string detail = end.is_object() && end.contains("detail") && end["detail"].is_string() ? end["detail"].get<std::string>() : "";
        log::warn("guest-client", std::format("access refused status={} state={}", session.status, state));
        fail(state == "busy" ? L"la console est occupée" : state == "refused" ? L"l'hôte a refusé" + (detail.empty() ? L"" : L" : " + widen(detail))
           : state == "expired" ? std::wstring(L"accès expiré") : state == "hours" ? std::wstring(L"en dehors des heures de cet accès")
           : state == "closed" ? std::wstring(L"l'hôte n'accueille pas d'invité en ce moment") : state == "failed" ? L"la console n'a pas démarré" + (detail.empty() ? L"" : L" : " + widen(detail))
           : state == "ended" ? std::wstring(L"visite terminée par l'hôte") : std::wstring(L"lien invalide ou révoqué"));
        return;
    }
    if (!session.error.empty()) {
        log::warn("guest-client", "session request failed: " + session.error);
        fail(session.error == "offline" ? L"l'hôte n'est pas en ligne (partage arrêté ?)" : L"l'hôte ne répond pas (lien expiré ou partage arrêté ?)");
        return;
    }
    const json offer = json::parse(session.body, nullptr, false);
    if (session.status != 200 || !offer.is_object() || !offer.contains("sdp") || !offer["sdp"].is_string() || !offer.contains("id") || !offer["id"].is_string()) {
        log::warn("guest-client", std::format("session refused status={}", session.status));
        const std::string why = offer.is_object() && offer.contains("error") && offer["error"].is_string() ? offer["error"].get<std::string>() : "";
        fail(why.empty() ? std::format(L"l'hôte a refusé la connexion (http {})", session.status) : widen(why));
        return;
    }
    p.peerId = offer["id"].get<std::string>();
    if (offer.contains("host") && offer["host"].is_string() && !offer["host"].get<std::string>().empty()) {
        std::lock_guard lock(mutex_);
        stats_.hostName = offer["host"].get<std::string>().substr(0, 24);
    }
    p.extendedInput = offer.contains("input") && offer["input"].is_number_integer() && offer["input"].get<int>() >= guest::kExtendedVersion;
    if (offer.contains("mode") && offer["mode"].is_string()) p.controlMode = offer["mode"].get<std::string>().substr(0, 16);

    // 2. The peer, with the host's ICE servers.
    rtc::Configuration config;
    const json ice = offer.contains("ice") ? offer["ice"] : json();
    config.iceServers = iceServersFrom(ice);
    config.disableAutoNegotiation = true;
    config.mtu = 1280;
    // libjuice relays over UDP only and takes two TURN servers: the first UDP one, and the
    // stream one through the tunnel, for a network that blocks UDP to the relay.
    if (const auto stream = streamTurnFrom(ice); !stream.host.empty() && !envOn("VEYRA_TEST_GUEST_CLIENT_NO_TUNNEL")) {
        std::string why;
        p.tunnel = guest::TurnTunnel::open(stream.host, stream.port, stream.tls, why);
        if (p.tunnel) {
            bool keptUdp = false;
            std::erase_if(config.iceServers, [&](const rtc::IceServer& s) {
                if (s.type != rtc::IceServer::Type::Turn) return false;
                if (keptUdp) return true;
                keptUdp = true;
                return false;
            });
            config.iceServers.emplace_back("127.0.0.1", p.tunnel->localPort(), stream.username, stream.credential, rtc::IceServer::RelayType::TurnUdp);
            log::info("guest-client", std::format("turn over {} to {}:{} through 127.0.0.1:{}", stream.tls ? "tls" : "tcp", stream.host, stream.port, p.tunnel->localPort()));
        } else log::warn("guest-client", "turn tunnel unavailable: " + why);
    }
    // Tests: VEYRA_TEST_GUEST_CLIENT_RELAY_ONLY=1 uses relay candidates only (this PC's direct
    // paths to a host on the same PC would hide whether the relay works).
    if (envOn("VEYRA_TEST_GUEST_CLIENT_RELAY_ONLY")) config.iceTransportPolicy = rtc::TransportPolicy::Relay;
    try {
        p.pc = std::make_shared<rtc::PeerConnection>(config);
        auto gate = p.gate;
        Impl* impl = &p;
        p.pc->onStateChange([impl, gate](rtc::PeerConnection::State state) {
            std::lock_guard g(gate->mutex); if (!gate->open) return;
            log::info("guest-client", std::format("peer state {}", int(state)));
            if (state == rtc::PeerConnection::State::Connected) impl->connected = true;
            if (state == rtc::PeerConnection::State::Failed || state == rtc::PeerConnection::State::Closed || state == rtc::PeerConnection::State::Disconnected) {
                impl->failed = true;
                std::lock_guard lock(impl->stateMutex);
                if (!impl->ended && impl->connected) { impl->endReason = L"connexion à l'hôte perdue"; impl->ended = true; }
            }
            impl->stateCv.notify_all();
        });
        p.pc->onGatheringStateChange([impl, gate](rtc::PeerConnection::GatheringState state) {
            std::lock_guard g(gate->mutex); if (!gate->open) return;
            if (state == rtc::PeerConnection::GatheringState::Complete) { impl->gathered = true; impl->stateCv.notify_all(); }
        });
        p.pc->onTrack([this, impl, gate](std::shared_ptr<rtc::Track> track) {
            std::lock_guard g(gate->mutex); if (!gate->open) return;
            const auto media = track->description();
            if (media.type() == "video") {
                impl->video = track;
                track->setMediaHandler(std::make_shared<rtc::H264RtpDepacketizer>(rtc::NalUnit::Separator::LongStartSequence));
                track->chainMediaHandler(std::make_shared<rtc::RtcpReceivingSession>());
                track->onFrame([impl, gate](rtc::binary data, rtc::FrameInfo info) {
                    std::lock_guard g2(gate->mutex); if (!gate->open) return;
                    const int64_t arrival = host100ns();
                    ++impl->units;
                    impl->videoBytes += data.size();
                    {
                        std::lock_guard lock(impl->metaMutex);
                        impl->arrived.push_back(info.timestamp);
                        if (impl->arrived.size() > 512) impl->arrived.pop_front();
                        for (auto it = impl->metas.rbegin(); it != impl->metas.rend(); ++it)
                            if (it->rtp == info.timestamp) { it->received = true; break; }
                    }
                    std::vector<uint8_t> unit(data.size());
                    std::memcpy(unit.data(), data.data(), data.size());
                    std::lock_guard lock(impl->queueMutex);
                    const auto lost = xbox::trimVideoInputs(impl->queue, 8);
                    impl->dropped += lost;
                    impl->queue.push_back({std::move(unit), info.timestamp, arrival, lost > 0 && impl->queue.empty()});
                    impl->queueCv.notify_one();
                });
            } else if (media.type() == "audio") {
                impl->audio = track;
                track->setMediaHandler(std::make_shared<rtc::OpusRtpDepacketizer>());
                track->chainMediaHandler(std::make_shared<rtc::RtcpReceivingSession>());
                track->onFrame([this, impl, gate](rtc::binary data, rtc::FrameInfo info) {
                    std::lock_guard g2(gate->mutex); if (!gate->open) return;
                    if (!impl->audioReady || !impl->opus) return;
                    if (impl->audioBase100 < 0) {
                        const int64_t picture = impl->videoPts100.load();
                        if (picture < 0) return;   // sound before the first picture has nothing to follow
                        impl->audioBase100 = picture;
                        impl->audioFirstRtp = info.timestamp;
                    }
                    const int frames = opus_decode_float(impl->opus, reinterpret_cast<const unsigned char*>(data.data()), opus_int32(data.size()), impl->pcm.data(), 5760, 0);
                    if (frames <= 0) { impl->audioDiscontinuity = true; return; }
                    const double ptsMs = double(impl->audioBase100) / 10000.0 + double(uint32_t(info.timestamp - impl->audioFirstRtp)) * 1000.0 / 48000.0;
                    audio_.push(impl->pcm.data(), size_t(frames) * 2 * sizeof(float), ptsMs, impl->audioDiscontinuity);
                    impl->audioDiscontinuity = false;
                });
            }
        });
        p.pc->onDataChannel([impl, gate](std::shared_ptr<rtc::DataChannel> channel) {
            std::lock_guard g(gate->mutex); if (!gate->open) return;
            const std::string label = channel->label();
            if (label == "input") impl->input = channel;
            else if (label == "feedback") {
                impl->feedbackChannel = channel;
                channel->onMessage([impl, gate](rtc::message_variant data) {
                    std::lock_guard g2(gate->mutex); if (gate->open) impl->onFeedback(data);
                });
            } else if (label == "stats") {
                impl->statsChannel = channel;
                channel->onMessage([impl, gate](rtc::message_variant data) {
                    std::lock_guard g2(gate->mutex); if (!gate->open) return;
                    if (const auto* text = std::get_if<std::string>(&data)) impl->onStats(*text);
                });
            }
        });

        // Sound: stereo float at 48 kHz, as the host's Opus.
        int opusError = 0;
        p.opus = opus_decoder_create(48000, 2, &opusError);
        if (p.opus && opusError == OPUS_OK) {
            p.pcm.assign(5760 * 2, 0.f);
            sink::AudioFormat layout;
            layout.channels = 2;
            layout.mask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
            const WAVEFORMATEXTENSIBLE wave = sink::floatWave(layout);
            p.audioReady = audio_.configure(wave.Format, sizeof(wave)) && audio_.start();
            if (!p.audioReady) log::warn("guest-client", "audio output could not start; the stream continues without sound");
        }

        std::string sdp = offer["sdp"].get<std::string>();
        // The relay-only test also drops the host's own candidates: libjuice still checks every
        // pair against them, so a host on the same PC would be reached directly.
        if (envOn("VEYRA_TEST_GUEST_CLIENT_RELAY_ONLY")) sdp = relayCandidatesOnly(sdp);
        p.pc->setRemoteDescription(rtc::Description(sdp, rtc::Description::Type::Offer));
        p.pc->setLocalDescription(rtc::Description::Type::Answer);
    } catch (const std::exception& e) {
        log::error("guest-client", std::string("peer setup failed: ") + e.what());
        fail(L"l'offre de l'hôte n'a pas pu être lue");
        return;
    }
    {
        std::unique_lock lock(p.stateMutex);
        p.stateCv.wait_for(lock, std::chrono::seconds(5), [&] { return p.gathered.load() || cancelled(); });
    }
    const auto local = p.pc->localDescription();
    if (!local) { fail(L"pas de réponse locale"); return; }

    // 3. The answer, every candidate in it (the host does not trickle either).
    const auto answer = signaling.call(L"/answer", json{{p.link.access ? "access" : "token", p.link.token}, {"id", p.peerId}, {"sdp", std::string(*local)}}.dump());
    if (!answer.error.empty() || (answer.status != 204 && answer.status != 200)) {
        log::warn("guest-client", std::format("answer refused status={} error={}", answer.status, answer.error));
        const json reply = json::parse(answer.body, nullptr, false);
        const std::string why = reply.is_object() && reply.contains("error") && reply["error"].is_string() ? reply["error"].get<std::string>() : "";
        fail(why.empty() ? L"l'hôte a refusé la réponse" : widen(why));
        return;
    }
    log::info("guest-client", std::format("answer accepted ice={} extendedInput={} mode={}", config.iceServers.size(), p.extendedInput.load(), p.controlMode));

    // 4. Media.
    {
        std::unique_lock lock(p.stateMutex);
        p.stateCv.wait_for(lock, std::chrono::seconds(20), [&] { return p.connected.load() || p.failed.load() || cancelled(); });
    }
    const auto tunnelState = [&] {
        if (!p.tunnel) return std::string("none");
        const auto t = p.tunnel->stats();
        return std::format("{} (sent {}, received {}){}", t.connected ? "up" : "down", t.sent, t.received, t.error.empty() ? "" : ", " + t.error);
    };
    if (!p.connected) {
        // What each side offered, so the log says whether the relay or the direct path was missing.
        log::warn("guest-client", std::format("no path to the host after 20 s ({}): local {}, host {}, turn tunnel {}", p.failed ? "failed" : "timeout",
                                              candidateSummary(p.pc->localDescription()), candidateSummary(p.pc->remoteDescription()), tunnelState()));
        { std::lock_guard lock(mutex_); stats_.iceFailed = true; }
        fail(p.failed ? L"la connexion à l'hôte a échoué (réseau ou pare-feu)" : L"délai dépassé en se connectant à l'hôte");
        return;
    }
    rtc::Candidate localPair, remotePair;
    std::string path = "?";
    if (p.pc->getSelectedCandidatePair(&localPair, &remotePair)) {
        p.relayed = localPair.type() == rtc::Candidate::Type::Relayed || remotePair.type() == rtc::Candidate::Type::Relayed;
        path = std::format("{} local={}/{} remote={}/{}", p.relayed ? "relay" : "direct", candidateTypeName(localPair.type()), transportName(localPair.transportType()),
                           candidateTypeName(remotePair.type()), transportName(remotePair.transportType()));
    }
    log::info("guest-client", std::format("connected path={} turn tunnel {}", path, tunnelState()));
    p.stopDecode = false;
    p.decodeThread = std::thread([this] { decodeLoop(); });
    p.requestKeyframe();
    {
        std::lock_guard lock(mutex_);
        stats_.state = GuestClientStats::State::Streaming;
        stats_.message.clear();
        publishedInfo_ = {};
        publishedInfo_.opened = true;
        publishedInfo_.kind = pipeline::SourceKind::GuestClient;
        publishedInfo_.averageFps = 60.0;
        publishedInfo_.nominalRateNum = 60;
        publishedInfo_.nominalRateDen = 1;
        publishedInfo_.timestampQuantum = 1.0 / 60.0;
        publishedInfo_.duration = pipeline::Rational::unknown();
        publishedInfo_.width = p.width;
        publishedInfo_.height = p.height;
        publishedInfo_.displayAspect = 16.0 / 9.0;
        publishedInfo_.videoCodecName = "H.264";
        publishedInfo_.color = streamColor();
        info_ = publishedInfo_;
        initialized_ = true;
        started_ = true;
    }
    ready_.notify_all();

    // 5. Clock sync, the reports the host's panel reads, and the test pad.
    auto nextPing = Clock::now(), nextReport = Clock::now() + std::chrono::seconds(2), nextSynth = Clock::now();
    const bool synthetic = syntheticEnv();
    if (synthetic) log::info("guest-client", "test: a synthetic DualSense state (gyro, touch, stick sweep) replaces the pad");
    while (!cancelled() && !p.ended.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(synthetic ? 4 : 20));
        const auto now = Clock::now();
        if (now >= nextPing) {
            nextPing = now + std::chrono::milliseconds(500);
            p.sendText(p.statsChannel, std::format(R"({{"t":"ping","c":{}}})", nowUs()));
        }
        if (synthetic && now >= nextSynth) {
            nextSynth = now + std::chrono::milliseconds(4);
            const double t = double(nowUs()) / 1e6;
            remoteplay::ControllerState s;
            s.inputActive = true;
            s.rightX = int16_t(std::sin(t * 2 * std::numbers::pi / 2) * 20000);
            s.buttons = (int64_t(t) % 2) ? remoteplay::ControllerState::Cross : 0;
            s.touches[0] = {1, uint16_t(960 + 800 * std::sin(t)), 540};
            s.motionValid = true;
            s.gyro = {float(0.8 * std::sin(t * 3)), 0.1f, -0.2f};
            s.accel = {0.0f, 0.98f, 0.05f};
            s.motionTimestampUs = uint64_t(nowUs());
            s.motionSamples[0] = {s.gyro, s.accel, s.motionTimestampUs};
            s.motionSampleCount = 1;
            std::lock_guard lock(p.inputMutex);
            auto pad = guest::fromControllerState(s, ++p.seq, double(nowUs()) / 1000.0);
            pad.synthetic = true;
            if (!p.extendedInput) pad.version = 0;
            const auto bytes = guest::encodeInputMessage(pad);
            if (p.input && p.input->isOpen()) {
                try { p.input->send(bytes.data(), bytes.size()); ++p.inputsSent; } catch (const std::exception&) {}
            }
        }
        if (now >= nextReport) {
            nextReport = now + std::chrono::seconds(2);
            std::vector<double> toScreen, rxScreen;
            int lost = 0, total = 0;
            {
                std::lock_guard lock(p.metaMutex);
                toScreen.swap(p.hostToScreen);
                rxScreen.swap(p.receiveToScreen);
                const int64_t settled = nowUs() - 500000;
                for (auto& m : p.metas) {
                    if (m.heardUs > settled || m.heardUs == 0) continue;
                    ++total;
                    if (!m.received) ++lost;
                    m.heardUs = 0;   // counted once
                }
            }
            const double loss = total ? double(lost) / total : 0.0;
            double decodeMs = 0;
            std::string decoderName, gpuVendor;
            {
                std::lock_guard lock(p.statsMutex);
                if (!toScreen.empty()) p.hostToScreenMs = median(toScreen);
                p.lossPercent = loss * 100;
                decodeMs = p.decodeMs;
                decoderName = p.decoderName; gpuVendor = p.gpuVendor;
            }
            json report{{"t", "rep"}, {"client", "boovi"}, {"map", "native"}, {"loss", loss}, {"dec", decodeMs}, {"decoder", decoderName}, {"gpu", gpuVendor}};
            if (!toScreen.empty()) report["src"] = median(toScreen);
            if (!rxScreen.empty()) report["rxdisp"] = median(rxScreen);
            if (const auto rtt = p.pc->rtt()) report["rtt"] = double(rtt->count());
            p.sendText(p.statsChannel, report.dump());
        }
    }
    if (p.ended.load()) {
        std::wstring reason;
        { std::lock_guard lock(p.stateMutex); reason = p.endReason; }
        std::lock_guard lock(mutex_);
        if (stats_.state != GuestClientStats::State::Failed) { stats_.state = GuestClientStats::State::Ended; stats_.message = reason; }
    }
}

void GuestClientSource::decodeLoop() {
    auto& p = *p_;
    SetThreadDescription(GetCurrentThread(), L"Guest client decode");
    bool seenKeyframe = false, recovering = false, forceSoftware = false;
    xbox::VideoDecodeRecovery recovery;
    int64_t lastPts = -1, rtpHigh = 0;
    uint32_t lastRtp = 0;
    bool haveRtp = false;
    const char* backend = nullptr;
    const auto gpu = decodeGpuOf(p.desc.decodeDevice.get());
    { std::lock_guard lock(p.statsMutex); p.gpuVendor = gpu.vendor; }
    for (;;) {
        Impl::Unit unit;
        {
            std::unique_lock lock(p.queueMutex);
            p.queueCv.wait(lock, [&] { return p.stopDecode || !p.queue.empty(); });
            if (p.stopDecode) break;
            unit = std::move(p.queue.front());
            p.queue.pop_front();
        }
        if (unit.inputLoss) {
            if (p.decoderOpen) p.decoder.recoverAtKeyframe();
            seenKeyframe = false;
            recovering = true;
            p.requestKeyframe();
        }
        if (!seenKeyframe && !startsDecoding(unit.data)) { ++p.dropped; p.requestKeyframe(); continue; }
        if (!p.decoderOpen) {
            p.decoder.setStreamProfile(true);
            p.decoder.setD3D11Fallback(true);
            if (!p.decoder.open(CaptureCodec::H264, p.width, p.height, nullptr, 0,
                                forceSoftware ? nullptr : p.desc.decodeDevice.get(), forceSoftware ? nullptr : p.desc.decodeQueue.get())) {
                log::error("guest-client", "no usable H.264 decoder");
                fail(L"aucun décodeur h.264 utilisable");
                p.ended = true;
                break;
            }
            p.decoderOpen = true;
            if (!p.dummyTarget) p.dummyTarget = allocNv12(16, 16);
        }
        // 90 kHz RTP timestamps (the host's clock), unwrapped, to 100 ns ticks.
        if (haveRtp && unit.rtp < lastRtp && lastRtp - unit.rtp > 0x80000000u) rtpHigh += int64_t(1) << 32;
        lastRtp = unit.rtp;
        haveRtp = true;
        int64_t pts = (rtpHigh + int64_t(unit.rtp)) * 1000 / 9;
        if (pts <= lastPts) pts = lastPts + 1;
        lastPts = pts;
        p.videoPts100 = pts;

        const auto begin = Clock::now();
        const bool needsTarget = p.decoder.framesDecoded() == 0 || !p.decoder.hardwareActive();
        AVFrame* target = needsTarget ? allocNv12(p.width, p.height) : p.dummyTarget;
        AVFrame* out = nullptr;
        bool hardware = false;
        const bool produced = target && p.decoder.decode(unit.data.data(), unit.data.size(), pts, target, &out, hardware);
        p.hardwareDecode.store(p.decoder.hardwareActive(), std::memory_order_relaxed);
        const double decodeMs = std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
        // Opened, or fell back to software on its first picture: say which decoder runs, on which GPU.
        if (const char* now = p.decoder.backendName(); now != backend) {
            backend = now;
            const bool software = !p.decoder.hardwareActive();
            const auto line = std::format("decoder {} gpu={} ({})", now, gpu.vendor, gpu.name);
            if (software && p.desc.decodeDevice) log::warn("guest-client", line + " - software decode, hardware was not usable");
            else log::info("guest-client", line);
            std::lock_guard lock(p.statsMutex);
            p.decoderName = now;
        }
        if (!produced) {
            if (needsTarget && target) av_frame_free(&target);
            if (!p.decoder.waitingForInput()) {
                ++p.errors;
                seenKeyframe = false;
                recovering = true;
                const auto action = recovery.failed(p.decoder.hardwareActive());
                log::warn("guest-client", std::format("decode failed backend={} errors={} detail={}", p.decoder.backendName(), p.errors.load(), p.decoder.lastError()));
                if (action == xbox::VideoDecodeRecoveryAction::Stop) { fail(L"le décodage vidéo échoue sans cesse"); p.ended = true; break; }
                if (action == xbox::VideoDecodeRecoveryAction::ResetHistory) p.decoder.recoverAtKeyframe();
                else {
                    p.decoder.close();
                    p.decoderOpen = false;
                    if (action == xbox::VideoDecodeRecoveryAction::UseSoftware) forceSoftware = true;
                }
                p.requestKeyframe();
            }
            continue;
        }
        std::shared_ptr<AVFrame> frame;
        if (hardware) {
            if (AVFrame* cloned = av_frame_clone(out)) frame.reset(cloned, [](AVFrame* f) { av_frame_free(&f); });
            if (needsTarget && target) av_frame_free(&target);
        } else {
            frame.reset(target, [](AVFrame* f) { av_frame_free(&f); });
        }
        if (!frame) { ++p.errors; continue; }
        recovery.picture();
        seenKeyframe = true;
        p.width = unsigned(frame->width);
        p.height = unsigned(frame->height);
        const int64_t framePts = frame->pts != AV_NOPTS_VALUE ? frame->pts : pts;

        pipeline::FramePacket packet{};
        packet.sourceKind = pipeline::SourceKind::GuestClient;
        packet.pts = pipeline::Rational{framePts, 10000000};
        packet.duration = pipeline::Rational{166667, 10000000};
        packet.arrivalHost100ns = unit.arrival;
        packet.decodedHost100ns = host100ns();
        // D3D11VA: the graph reads the decoder's shared copy, not the AVFrame's texture.
        if (hardware && frame->format == AV_PIX_FMT_D3D11 && !p.decoder.hardwareSurface(packet.hardwareSurface)) { ++p.errors; continue; }
        if (recovering) {
            packet.flags |= static_cast<pipeline::FrameFlags>(pipeline::FrameFlagBits::Discontinuity);
            recovering = false;
        }
        auto fallback = streamColor();
        packet.colorInfo = pipeline::resolveFrameColor(*frame, fallback);
        packet.colorInfo.pixelFormat = pixelFormatOf(*frame);
        if (packet.colorInfo.pixelFormat == pipeline::SourcePixelFormat::Unknown) { ++p.errors; continue; }

        SourceInfo info;
        {
            std::lock_guard lock(mutex_);
            info = info_;
            stats_.width = unsigned(frame->width);
            stats_.height = unsigned(frame->height);
        }
        info.color = packet.colorInfo;
        info.hardwareDecodeActive = frame->format == AV_PIX_FMT_D3D12 || frame->format == AV_PIX_FMT_D3D11;
        info.videoDecodePath = p.decoder.backendName();
        if (info.width != unsigned(frame->width) || info.height != unsigned(frame->height)) {
            info.width = unsigned(frame->width);
            info.height = unsigned(frame->height);
            info.displayAspect = frame->height > 0 ? double(frame->width) / double(frame->height) : 0.0;
            packet.flags |= static_cast<pipeline::FrameFlags>(pipeline::FrameFlagBits::Resize);
        }
        {
            std::lock_guard lock(p.metaMutex);
            p.decodedPictures.push_back({framePts, unit.arrival, unit.rtp});
            if (p.decodedPictures.size() > 64) p.decodedPictures.pop_front();
        }
        { std::lock_guard lock(p.statsMutex); p.decodeMs = movingAverage(p.decodeMs, decodeMs); }
        if (++p.decoded == 1) log::info("guest-client", std::format("first picture decoded {}x{} path={}", frame->width, frame->height, info.videoDecodePath));
        publish(std::move(frame), packet, info);
    }
}

void GuestClientSource::publish(std::shared_ptr<AVFrame> frame, const pipeline::FramePacket& packet, const SourceInfo& info) {
    {
        std::lock_guard lock(mutex_);
        Frame item{std::move(frame), packet, info};
        item.packet.sequence = ++sequence_;
        if (openFlagPending_) {
            item.packet.flags |= static_cast<pipeline::FrameFlags>(pipeline::FrameFlagBits::Open);
            openFlagPending_ = false;
        }
        if (latest_) {
            ++skipped_;
            item.packet.flags |= latest_->packet.flags;
            item.packet.flags |= static_cast<pipeline::FrameFlags>(pipeline::FrameFlagBits::Drop);
        }
        latest_ = std::move(item);
    }
    if (frameEvent_) SetEvent(frameEvent_);
}

SourceReadStatus GuestClientSource::read(pipeline::FramePacket& packet, const AVFrame** frame) {
    if (frame) *frame = nullptr;
    std::lock_guard lock(mutex_);
    if (stats_.state == GuestClientStats::State::Failed || stats_.state == GuestClientStats::State::Ended) return SourceReadStatus::Error;
    if (!latest_) return started_ ? SourceReadStatus::Waiting : SourceReadStatus::Error;
    packet = latest_->packet;
    info_ = latest_->info;
    view_ = std::move(latest_->frame);
    latest_.reset();
    if (frame) *frame = view_.get();
    return SourceReadStatus::Frame;
}

void GuestClientSource::videoPresented(double ptsMs, int64_t presented100ns, bool sourceFrame) {
    std::optional<int64_t> arrival;
    if (p_) {
        auto& p = *p_;
        const int64_t pts100 = int64_t(ptsMs * 10000.0);
        std::lock_guard lock(p.metaMutex);
        for (const auto& shown : p.decodedPictures) {
            if (std::llabs(shown.pts100 - pts100) >= 5) continue;
            arrival = shown.arrival;
            if (!sourceFrame) break;
            p.receiveToScreen.push_back(double(presented100ns - shown.arrival) / 10000.0);
            if (p.clockKnown) {
                for (auto it = p.metas.rbegin(); it != p.metas.rend(); ++it) {
                    if (it->rtp != shown.rtp) continue;
                    const int64_t hostStart = it->sourceUs > 0 ? it->sourceUs : it->captureUs;
                    if (hostStart > 0) p.hostToScreen.push_back(double(presented100ns / 10 + p.clockOffsetUs - hostStart) / 1000.0);
                    break;
                }
            }
            if (p.receiveToScreen.size() > 4000) p.receiveToScreen.clear();
            if (p.hostToScreen.size() > 4000) p.hostToScreen.clear();
            break;
        }
    }
    audio_.videoPresented(ptsMs, presented100ns, arrival);
}

int64_t GuestClientSource::pictureHostTime100ns(int64_t pts100) {
    if (!p_) return -1;
    auto& p = *p_;
    std::lock_guard lock(p.metaMutex);
    for (auto shown = p.decodedPictures.rbegin(); shown != p.decodedPictures.rend(); ++shown) {
        if (std::llabs(shown->pts100 - pts100) >= 5) continue;
        int64_t host = 0;
        if (p.clockKnown)
            for (auto meta = p.metas.rbegin(); meta != p.metas.rend(); ++meta) {
                if (meta->rtp != shown->rtp) continue;
                const int64_t hostStart = meta->sourceUs > 0 ? meta->sourceUs : meta->captureUs;
                if (hostStart > 0) host = (hostStart - p.clockOffsetUs) * 10;
                break;
            }
        const int64_t transit = shown->arrival - host;
        if (host > 0 && transit >= 0 && transit < 5000000) {
            p.transit100 = p.transitKnown ? p.transit100 + (transit - p.transit100) / 8 : transit;
            p.transitKnown = true;
        } else host = shown->arrival - p.transit100;   // the host's word on this picture is not here yet
        p.stampToHost100 = host - pts100; p.stampKnown = true;
        return host;
    }
    return p.stampKnown ? pts100 + p.stampToHost100 : -1;
}

void GuestClientSource::controller(const remoteplay::ControllerState& state) {
    if (!p_ || syntheticEnv()) return;
    auto& p = *p_;
    std::lock_guard lock(p.inputMutex);
    if (!p.input || !p.input->isOpen()) return;
    auto pad = guest::fromControllerState(state, ++p.seq, double(nowUs()) / 1000.0);
    if (!p.extendedInput) pad.version = 0;   // a host that only reads the browser's message
    const auto bytes = guest::encodeInputMessage(pad);
    try { p.input->send(bytes.data(), bytes.size()); ++p.inputsSent; } catch (const std::exception&) {}
}

remoteplay::ControllerFeedback GuestClientSource::takeFeedback() {
    remoteplay::ControllerFeedback out;
    if (!p_) return out;
    std::lock_guard lock(p_->feedbackMutex);
    if (p_->feedbackPending) { out = std::move(p_->feedback); p_->feedback = {}; p_->feedbackPending = false; }
    return out;
}

GuestClientStats GuestClientSource::stats() const {
    GuestClientStats s;
    { std::lock_guard lock(mutex_); s = stats_; }
    if (!p_) return s;
    auto& p = *p_;
    s.units = p.units.load();
    s.decoded = p.decoded.load();
    s.dropped = p.dropped.load() + skipped();
    s.decodeErrors = p.errors.load();
    s.keyframeRequests = p.keyframes.load();
    s.inputsSent = p.inputsSent.load();
    s.hardwareDecode = p.hardwareDecode.load(std::memory_order_relaxed);
    { std::lock_guard lock(p.statsMutex); s.decoder = p.decoderName; s.gpu = p.gpuVendor; }
    s.relayed = p.relayed.load();
    s.extendedInput = p.extendedInput.load();
    { std::lock_guard lock(p.stateMutex); s.controlMode = p.controlMode; }
    {
        std::lock_guard g(p.gate->mutex);
        if (p.gate->open && p.pc && s.state == GuestClientStats::State::Streaming)
            if (const auto rtt = p.pc->rtt()) s.rttMs = double(rtt->count());
    }
    std::lock_guard lock(p.statsMutex);
    s.decodeMs = p.decodeMs;
    s.hostToScreenMs = p.hostToScreenMs;
    s.lossPercent = p.lossPercent;
    const auto now = Clock::now();
    const double seconds = std::chrono::duration<double>(now - p.rateStart).count();
    if (seconds >= 1.0) {
        const uint64_t bytes = p.videoBytes.load();
        p.receivedFps = double(s.units - p.rateUnits) / seconds;
        p.decodedFps = double(s.decoded - p.rateDecoded) / seconds;
        p.videoMbps = double(bytes - p.rateBytes) * 8.0 / seconds / 1e6;
        p.rateUnits = s.units; p.rateDecoded = s.decoded; p.rateBytes = bytes;
        p.rateStart = now;
    }
    s.receivedFps = p.receivedFps;
    s.decodedFps = p.decodedFps;
    s.videoMbps = p.videoMbps;
    return s;
}

uint64_t GuestClientSource::unitsReceived() const { return p_ ? p_->units.load() : 0; }

uint64_t GuestClientSource::skipped() const {
    std::lock_guard lock(mutex_);
    return skipped_;
}

void GuestClientSource::close() noexcept {
    cancel_ = true;
    if (p_) p_->stateCv.notify_all();
    if (owner_.joinable()) owner_.join();
    if (p_) {
        auto& p = *p_;
        // A released pad, then goodbye, so the host's own pad takes over at once.
        if (p.input && p.input->isOpen()) {
            try {
                std::lock_guard lock(p.inputMutex);
                auto pad = guest::fromControllerState({}, ++p.seq, double(nowUs()) / 1000.0);
                if (!p.extendedInput) pad.version = 0;
                const auto bytes = guest::encodeInputMessage(pad);
                p.input->send(bytes.data(), bytes.size());
            } catch (...) {}
        }
        p.sendText(p.statsChannel, R"({"t":"bye"})");
        { std::lock_guard g(p.gate->mutex); p.gate->open = false; }
        {
            std::lock_guard lock(p.queueMutex);
            p.stopDecode = true;
        }
        p.queueCv.notify_all();
        if (p.decodeThread.joinable()) p.decodeThread.join();
        for (const auto& dc : {p.input, p.feedbackChannel, p.statsChannel}) if (dc) dc->resetCallbacks();
        if (p.video) p.video->resetCallbacks();
        if (p.audio) p.audio->resetCallbacks();
        if (p.pc) { p.pc->resetCallbacks(); try { p.pc->close(); } catch (...) {} }
        p.input.reset(); p.feedbackChannel.reset(); p.statsChannel.reset(); p.video.reset(); p.audio.reset(); p.pc.reset();
        p.tunnel.reset();
        p.hardwareDecode = false;
        if (p.decoderOpen) { p.decoder.close(); p.decoderOpen = false; }
    }
    audio_.stop();
    std::lock_guard lock(mutex_);
    latest_.reset();
    view_.reset();
    info_ = {};
    started_ = false;
    if (stats_.state == GuestClientStats::State::Streaming || stats_.state == GuestClientStats::State::Connecting)
        stats_.state = GuestClientStats::State::Idle;
}

} // namespace veyra::source
