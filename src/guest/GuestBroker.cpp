#include "GuestBroker.h"

#include "veyra/Log.h"
#include "veyra/guest/BrokerSocket.h"
#include "veyra/guest/GuestRoom.h"

#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cstdlib>
#include <format>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <vector>

#ifndef BOOVI_BROKER_URL
#define BOOVI_BROKER_URL ""
#endif

namespace veyra::guest {
using json = nlohmann::json;
namespace {

constexpr int kMaxRequests = 8;                       // guest requests handled at once, as the local server
constexpr int64_t kDeadlineMs = 10000;                 // a request still running this long is answered 503
constexpr auto kPingEvery = std::chrono::seconds(25);  // the room answers "pong" without waking
constexpr auto kSilentFor = std::chrono::seconds(70);  // nothing heard this long: the socket is dead

// Only the paths a guest may ask for are named in the log.
const char* pathName(const std::string& path) { return path == "/session" ? "/session" : path == "/answer" ? "/answer" : "other"; }

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    std::wstring out(size_t(MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), out.data(), int(out.size()));
    return out;
}

struct Endpoint { bool secure = true; std::string host; uint16_t port = 443; };

std::optional<Endpoint> parseUrl(std::string url) {
    Endpoint e;
    if (url.starts_with("https://")) url.erase(0, 8);
    else if (url.starts_with("http://")) { url.erase(0, 7); e.secure = false; e.port = 80; }
    else return std::nullopt;
    if (const size_t slash = url.find('/'); slash != std::string::npos) url.resize(slash);
    if (const size_t colon = url.rfind(':'); colon != std::string::npos) {
        e.port = uint16_t(std::atoi(url.c_str() + colon + 1));
        url.resize(colon);
    }
    if (url.empty() || !e.port) return std::nullopt;
    for (char c : url) if (!(std::isalnum(uint8_t(c)) || c == '-' || c == '.')) return std::nullopt;
    e.host = url;
    return e;
}

// The guest's address as the broker saw it (Cf-Connecting-Ip): only an address's characters.
std::string cleanAddress(const std::string& ip) {
    std::string out;
    for (char c : ip) if (std::isxdigit(uint8_t(c)) || c == '.' || c == ':') out.push_back(c);
    return out.size() <= 45 ? out : std::string();
}

} // namespace

std::string brokerUrl() {
    if (const char* v = std::getenv("VEYRA_GUEST_BROKER"); v && *v) return v;
    return BOOVI_BROKER_URL;
}

struct GuestBroker::Impl : std::enable_shared_from_this<GuestBroker::Impl> {
    Endpoint endpoint;
    std::string room, roomKey, ownerKey;
    Handler handler;
    BrokerSocket socket;
    std::thread thread, keepalive;
    std::atomic<bool> running{false};
    std::atomic<int64_t> heardMs{0};

    mutable std::mutex m;
    std::condition_variable cv;
    BrokerPhase phase = BrokerPhase::Off;
    bool entitled = false;
    std::string error;
    int active = 0;                                  // guest requests in flight
    uint32_t seq = 0;
    std::set<uint32_t> waiting;                      // TURN requests a guest's /session waits for
    std::map<uint32_t, json> replies;                // their answers
    std::vector<std::string> ends;                   // leases to hand back once connected

    static int64_t nowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    void run() {
        SetThreadDescription(GetCurrentThread(), L"Guest broker");
        int backoff = 1;
        while (running) {
            const bool reached = session();
            {
                std::unique_lock lock(m);
                if (!running) break;
                phase = BrokerPhase::Connecting;
                entitled = false;
                cv.notify_all();
                backoff = reached ? 1 : std::min(backoff * 2, 15);
                cv.wait_for(lock, std::chrono::seconds(backoff), [&] { return !running.load(); });
            }
        }
    }

    // One connection to the room, until it drops. True when the room was reached.
    bool session() {
        int status = 0;
        std::string why;
        std::wstring headers = widen(std::format("Authorization: Bearer {}\r\n", roomKey));
        if (!ownerKey.empty()) headers += widen(std::format("X-Boovi-Owner: {}\r\n", ownerKey));
        const bool open = socket.open(widen(endpoint.host), endpoint.port, endpoint.secure, widen("/api/host/" + room), headers, status, why);
        SecureZeroMemory(headers.data(), headers.size() * sizeof(wchar_t));
        if (!open) {
            std::lock_guard lock(m);
            if (error != why) veyra::log::warn("guest-broker", "room unreachable: " + why);
            error = why;
            return false;
        }
        heardMs = nowMs();
        std::string text;
        uint16_t closeCode = 0;
        while (running && socket.receive(text, closeCode)) {
            heardMs = nowMs();
            onMessage(text);
        }
        socket.close();
        veyra::log::info("guest-broker", std::format("room connection closed (code {})", closeCode));
        return true;
    }

    void onMessage(const std::string& text) {
        if (text == "pong") return;
        const json in = json::parse(text, nullptr, false);
        if (!in.is_object() || !in.contains("t") || !in["t"].is_string()) return;
        const std::string t = in["t"].get<std::string>();
        if (t == "ready") {
            std::vector<std::string> pending;
            {
                std::lock_guard lock(m);
                phase = BrokerPhase::Connected;
                entitled = in.value("entitled", false);
                error.clear();
                pending.swap(ends);
                cv.notify_all();
            }
            veyra::log::info("guest-broker", std::format("room open entitled={} ttl={} s", entitled, in.value("ttl", 0)));
            for (const auto& lease : pending) sendEnd(lease);
        } else if (t == "msg" && in.contains("g") && in["g"].is_string() && in.contains("d") && in["d"].is_string()) {
            serve(in["g"].get<std::string>(), cleanAddress(in.value("ip", std::string())), in["d"].get<std::string>());
        } else if (t == "turn" && in.contains("n") && in["n"].is_number_unsigned()) {
            const uint32_t n = in["n"].get<uint32_t>();
            {
                std::lock_guard lock(m);
                if (waiting.contains(n)) { replies[n] = in; cv.notify_all(); return; }
            }
            // Too late for its guest: hand it straight back.
            if (in.value("ok", false) && in.contains("lease") && in["lease"].is_string()) sendEnd(in["lease"].get<std::string>());
        } else if (t == "turn-end" && !in.value("ok", false)) {
            veyra::log::warn("guest-broker", "a turn lease was not ended: already handed back, or charged until it expires");
        }
    }

    // One guest request in flight. Whoever sets `answered` first replies: the handler, or the
    // deadline when the handler is still running at kDeadlineMs.
    struct Call {
        std::string guest;
        json id;
        const char* path = "other";
        int64_t startMs = 0;
        std::atomic<bool> answered{false};
        std::atomic<const char*> stage{"queued"};
        bool overdue = false;                        // under m
        std::string lease;                           // under m: the TURN lease it took
    };
    std::vector<std::shared_ptr<Call>> calls;        // under m
    int overdue = 0;                                 // calls past their deadline whose handler still runs

    void reply(const std::string& guest, const json& id, int status, const std::string& body) {
        const json out{{"id", id}, {"status", status}, {"body", body}};
        socket.send(json{{"t", "msg"}, {"g", guest}, {"d", out.dump()}}.dump());
    }

    // A guest's request, on its own thread: /session may wait for the host's ICE gathering.
    // Every request gets a reply and a log line (path, status, time), never silence.
    void serve(std::string guest, std::string ip, std::string text) {
        const json in = json::parse(text, nullptr, false);
        if (!in.is_object() || !in.contains("id") || !in.contains("path") || !in["path"].is_string()) {
            veyra::log::warn("guest-broker", "request without id or path: nothing to answer");
            return;
        }
        auto call = std::make_shared<Call>();
        call->guest = std::move(guest);
        call->id = in["id"];
        call->path = pathName(in["path"].get<std::string>());
        call->startMs = nowMs();
        int busy = 0, stuck = 0;
        {
            std::lock_guard lock(m);
            if (active >= kMaxRequests) { busy = active; stuck = overdue; }
            else { ++active; calls.push_back(call); }
        }
        if (busy) {
            reply(call->guest, call->id, 503, R"({"error":"l'hôte est occupé, réessaie dans un instant","state":"busy"})");
            veyra::log::warn("guest-broker", std::format("{} 503 busy ({} requests running, {} past their deadline)", call->path, busy, stuck));
            return;
        }
        std::thread([self = shared_from_this(), call, ip = std::move(ip), in] {
            HttpRequest request;
            request.method = "POST";
            request.path = in["path"].get<std::string>();
            request.body = in.contains("body") ? in["body"].dump() : "{}";
            request.source = ip;
            request.stage = &call->stage;
            request.onLease = [s = self.get(), call](const std::string& id) { s->holdLease(*call, id); };
            HttpResponse response;
            try { response = self->handler(request); }
            catch (const std::exception& e) { veyra::log::error("guest-broker", e.what()); response = {500, "text/plain", "error"}; }
            const int64_t ms = nowMs() - call->startMs;
            if (!call->answered.exchange(true)) {
                self->reply(call->guest, call->id, response.status, response.body);
                veyra::log::info("guest-broker", std::format("{} {} {} ms", call->path, response.status, ms));
            } else {
                veyra::log::warn("guest-broker", std::format("{} finished after {} ms with {}, too late: the guest had a 503", call->path, ms, response.status));
            }
            std::lock_guard lock(self->m);
            if (call->overdue) --self->overdue;
            std::erase(self->calls, call);
            --self->active;
            self->cv.notify_all();
        }).detach();
    }

    // A call's TURN lease; one taken after its deadline goes straight back.
    void holdLease(Call& c, const std::string& id) {
        bool late;
        { std::lock_guard lock(m); c.lease = id; late = c.overdue; }
        if (late) { sendEnd(id); veyra::log::warn("guest-broker", "a turn lease taken past the deadline is handed back"); }
    }

    // Calls past their deadline: the guest gets a 503 now, the log says where the handler is,
    // and the TURN lease it holds goes back (a handler stuck for good would keep it charged).
    void enforceDeadlines() {
        std::vector<std::shared_ptr<Call>> late;
        std::vector<std::string> leases;
        {
            std::lock_guard lock(m);
            for (const auto& c : calls)
                if (nowMs() - c->startMs >= kDeadlineMs && !c->answered.exchange(true)) {
                    late.push_back(c); c->overdue = true; ++overdue;
                    if (!c->lease.empty()) leases.push_back(c->lease);
                }
        }
        for (const auto& c : late) {
            reply(c->guest, c->id, 503, R"({"error":"l'hôte ne répond pas, réessaie","state":"stuck"})");
            veyra::log::error("guest-broker", std::format("{} still running after {} ms at \"{}\": answered 503", c->path, nowMs() - c->startMs, c->stage.load()));
        }
        for (const auto& lease : leases) sendEnd(lease);
        if (!leases.empty()) veyra::log::warn("guest-broker", std::format("{} turn lease(s) of late requests handed back", leases.size()));
    }

    void sendEnd(const std::string& lease) {
        uint32_t n;
        { std::lock_guard lock(m); n = ++seq; }
        if (!socket.send(json{{"t", "turn-end"}, {"n", n}, {"lease", lease}}.dump())) {
            std::lock_guard lock(m);
            ends.push_back(lease);
        }
    }

    // Request deadlines every 250 ms, the room's ping every kPingEvery.
    void keepaliveLoop() {
        SetThreadDescription(GetCurrentThread(), L"Guest broker ping");
        int64_t pingedMs = nowMs();
        std::unique_lock lock(m);
        while (running) {
            cv.wait_for(lock, std::chrono::milliseconds(250), [&] { return !running.load(); });
            if (!running) break;
            const bool connected = phase == BrokerPhase::Connected;
            lock.unlock();
            enforceDeadlines();
            if (connected && nowMs() - pingedMs >= std::chrono::duration_cast<std::chrono::milliseconds>(kPingEvery).count()) {
                pingedMs = nowMs();
                if (nowMs() - heardMs.load() > std::chrono::duration_cast<std::chrono::milliseconds>(kSilentFor).count()) {
                    veyra::log::warn("guest-broker", "room silent, reconnecting");
                    socket.close();
                } else socket.send("ping");
            }
            lock.lock();
        }
    }
};

GuestBroker::GuestBroker() : p_(std::make_shared<Impl>()) {}
GuestBroker::~GuestBroker() { stop(); }

bool GuestBroker::start(const std::string& url, const std::string& roomKey, const std::string& ownerKey, Handler handler, std::string& error) {
    stop();
    const auto endpoint = parseUrl(url);
    if (!endpoint) { error = "adresse du broker invalide"; return false; }
    if (!isRoomKey(roomKey)) { error = "clé de salon invalide"; return false; }
    auto& p = *p_;
    p.endpoint = *endpoint;
    p.roomKey = roomKey;
    p.ownerKey = ownerKey;
    p.room = roomIdOf(roomKey);
    p.handler = std::move(handler);
    {
        std::lock_guard lock(p.m);
        p.phase = BrokerPhase::Connecting;
        p.entitled = false;
        p.error.clear();
    }
    p.running = true;
    p.thread = std::thread([impl = p_] { impl->run(); });
    p.keepalive = std::thread([impl = p_] { impl->keepaliveLoop(); });
    veyra::log::info("guest-broker", std::format("opening the room on {} owner={}", p.endpoint.host, !ownerKey.empty()));
    return true;
}

void GuestBroker::stop() {
    auto& p = *p_;
    if (!p.running.exchange(false)) return;
    p.cv.notify_all();
    p.socket.close();
    if (p.thread.joinable()) p.thread.join();
    if (p.keepalive.joinable()) p.keepalive.join();
    std::unique_lock lock(p.m);
    // Requests in flight finish before the handler's owner goes.
    p.cv.wait_for(lock, std::chrono::seconds(6), [&] { return p.active == 0; });
    p.phase = BrokerPhase::Off;
    p.entitled = false;
    p.replies.clear();
    p.waiting.clear();
    if (!p.ends.empty()) veyra::log::warn("guest-broker", std::format("{} turn lease(s) left to expire", p.ends.size()));
    p.ends.clear();
    SecureZeroMemory(p.roomKey.data(), p.roomKey.size());
    SecureZeroMemory(p.ownerKey.data(), p.ownerKey.size());
    p.roomKey.clear(); p.ownerKey.clear();
}

BrokerPhase GuestBroker::phase() const { std::lock_guard lock(p_->m); return p_->phase; }
int GuestBroker::overdue() const { std::lock_guard lock(p_->m); return p_->overdue; }
bool GuestBroker::entitled() const { std::lock_guard lock(p_->m); return p_->entitled; }
std::string GuestBroker::error() const { std::lock_guard lock(p_->m); return p_->error; }

std::string GuestBroker::roomLink() const {
    const auto& e = p_->endpoint;
    if (p_->room.empty()) return {};
    const bool defaultPort = (e.secure && e.port == 443) || (!e.secure && e.port == 80);
    return std::format("{}://{}{}/{}", e.secure ? "https" : "http", e.host, defaultPort ? std::string() : std::format(":{}", e.port), p_->room);
}

TurnLease GuestBroker::requestTurn(std::chrono::milliseconds timeout) {
    auto& p = *p_;
    TurnLease out;
    uint32_t n;
    {
        std::lock_guard lock(p.m);
        if (p.phase != BrokerPhase::Connected) { out.why = "offline"; return out; }
        n = ++p.seq;
        p.waiting.insert(n);
    }
    if (!p.socket.send(json{{"t", "turn"}, {"n", n}}.dump())) { std::lock_guard lock(p.m); p.waiting.erase(n); out.why = "offline"; return out; }
    std::unique_lock lock(p.m);
    p.cv.wait_for(lock, timeout, [&] { return p.replies.contains(n) || !p.running.load(); });
    p.waiting.erase(n);
    if (!p.replies.contains(n)) { out.why = "timeout"; return out; }
    const json reply = std::move(p.replies[n]);
    p.replies.erase(n);
    if (!reply.value("ok", false) || !reply.contains("lease") || !reply["lease"].is_string()) {
        out.why = reply.value("why", std::string("turn"));
        return out;
    }
    out.id = reply["lease"].get<std::string>();
    if (reply.contains("iceServers") && reply["iceServers"].is_array()) out.iceServers = reply["iceServers"];
    return out;
}

void GuestBroker::endTurn(const std::string& lease) {
    if (lease.empty() || !p_->running) return;
    p_->sendEnd(lease);
}

} // namespace veyra::guest
