#include "GuestHttp.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include "veyra/Log.h"

#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <format>
#include <memory>
#include <mutex>
#include <thread>

#pragma comment(lib, "ws2_32.lib")

namespace veyra::guest {
namespace {

constexpr size_t kMaxHeader = 16 * 1024;
constexpr size_t kMaxBody = 64 * 1024;
constexpr int kMaxConnections = 8;
constexpr DWORD kRecvTimeoutMs = 5000;

const char* reason(int status) {
    switch (status) {
    case 200: return "OK"; case 204: return "No Content"; case 400: return "Bad Request";
    case 403: return "Forbidden"; case 404: return "Not Found"; case 405: return "Method Not Allowed";
    case 409: return "Conflict"; case 413: return "Payload Too Large"; case 429: return "Too Many Requests";
    case 503: return "Service Unavailable"; default: return "Error";
    }
}

std::string lower(std::string s) { for (auto& c : s) c = char(tolower(uint8_t(c))); return s; }

void sendAll(SOCKET s, const std::string& data) {
    size_t at = 0;
    while (at < data.size()) {
        const int n = send(s, data.data() + at, int(std::min<size_t>(data.size() - at, 1 << 20)), 0);
        if (n <= 0) return;
        at += size_t(n);
    }
}

} // namespace

struct GuestHttp::Impl : std::enable_shared_from_this<GuestHttp::Impl> {
    std::vector<SOCKET> listeners;
    std::vector<std::string> bound;
    uint16_t port = 0;
    Handler handler;
    std::thread acceptThread;
    std::atomic<bool> running{false};
    std::mutex m;
    std::condition_variable cv;
    int active = 0;
    bool wsa = false;

    void serve(SOCKET client, std::string peer) {
        DWORD timeout = kRecvTimeoutMs;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        std::string data;
        size_t headerEnd = std::string::npos;
        char buffer[4096];
        while (headerEnd == std::string::npos) {
            const int n = recv(client, buffer, sizeof(buffer), 0);
            if (n <= 0) return;
            data.append(buffer, size_t(n));
            headerEnd = data.find("\r\n\r\n");
            if (headerEnd == std::string::npos && data.size() > kMaxHeader) return;
        }
        HttpRequest request;
        const std::string head = data.substr(0, headerEnd);
        const size_t lineEnd = head.find("\r\n");
        const std::string line = head.substr(0, lineEnd);
        const size_t sp1 = line.find(' '), sp2 = line.find(' ', sp1 + 1);
        if (sp1 == std::string::npos || sp2 == std::string::npos) return;
        request.method = line.substr(0, sp1);
        std::string target = line.substr(sp1 + 1, sp2 - sp1 - 1);
        if (const size_t q = target.find('?'); q != std::string::npos) { request.query = target.substr(q + 1); target.resize(q); }
        request.path = target;
        request.source = std::move(peer);
        size_t length = 0;
        for (size_t at = lineEnd; at != std::string::npos && at < head.size();) {
            const size_t next = head.find("\r\n", at + 2);
            const std::string header = head.substr(at + 2, (next == std::string::npos ? head.size() : next) - at - 2);
            if (const size_t colon = header.find(':'); colon != std::string::npos && lower(header.substr(0, colon)) == "content-length")
                length = size_t(std::strtoull(header.c_str() + colon + 1, nullptr, 10));
            at = next;
        }
        HttpResponse response;
        if (length > kMaxBody) response = {413, "text/plain", "too large"};
        else {
            request.body = data.substr(headerEnd + 4);
            while (request.body.size() < length) {
                const int n = recv(client, buffer, sizeof(buffer), 0);
                if (n <= 0) return;
                request.body.append(buffer, size_t(n));
            }
            request.body.resize(length);
            try { response = handler(request); }
            catch (const std::exception& e) { veyra::log::error("guest-http", e.what()); response = {500, "text/plain", "error"}; }
        }
        // The token rides in the page's URL: never let it leave as a referrer, never cache.
        std::string out = std::format("HTTP/1.1 {} {}\r\nContent-Type: {}\r\nContent-Length: {}\r\nConnection: close\r\n"
            "Cache-Control: no-store\r\nReferrer-Policy: no-referrer\r\nX-Content-Type-Options: nosniff\r\nX-Frame-Options: DENY\r\n"
            "Content-Security-Policy: default-src 'self'; script-src 'self' 'unsafe-inline'; style-src 'self' 'unsafe-inline'; "
            "connect-src 'self'; media-src 'self' blob:; img-src 'self' data:; font-src 'self'; frame-ancestors 'none'\r\n\r\n",
            response.status, reason(response.status), response.type, response.body.size());
        out += response.body;
        sendAll(client, out);
        shutdown(client, SD_SEND);
    }

    void acceptLoop() {
        SetThreadDescription(GetCurrentThread(), L"Guest http");
        while (running.load()) {
            fd_set set; FD_ZERO(&set);
            for (SOCKET s : listeners) FD_SET(s, &set);
            timeval tv{0, 200000};
            if (select(0, &set, nullptr, nullptr, &tv) <= 0) continue;
            for (SOCKET s : listeners) {
                if (!FD_ISSET(s, &set)) continue;
                sockaddr_in from{};
                int fromSize = sizeof(from);
                SOCKET client = accept(s, reinterpret_cast<sockaddr*>(&from), &fromSize);
                if (client == INVALID_SOCKET) continue;
                char text[INET_ADDRSTRLEN]{};
                inet_ntop(AF_INET, &from.sin_addr, text, sizeof(text));
                {
                    std::lock_guard lock(m);
                    if (active >= kMaxConnections) { closesocket(client); continue; }
                    ++active;
                }
                std::thread([self = shared_from_this(), client, peer = std::string(text)] {
                    self->serve(client, peer);
                    closesocket(client);
                    { std::lock_guard lock(self->m); --self->active; }
                    self->cv.notify_all();
                }).detach();
            }
        }
    }
};

GuestHttp::GuestHttp() : p_(std::make_shared<Impl>()) {}
GuestHttp::~GuestHttp() { stop(); }
uint16_t GuestHttp::port() const { return p_->port; }
std::vector<std::string> GuestHttp::bound() const { return p_->bound; }

bool GuestHttp::start(const std::vector<std::string>& addresses, uint16_t port, Handler handler, std::string& error) {
    stop();
    auto& p = *p_;
    WSADATA wsaData{};
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) { error = "winsock"; return false; }
    p.wsa = true;
    p.handler = std::move(handler);
    for (size_t i = 0; i < addresses.size(); ++i) {
        sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_port = htons(port);
        if (inet_pton(AF_INET, addresses[i].c_str(), &addr.sin_addr) != 1) continue;
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) continue;
        BOOL exclusive = TRUE;
        setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
        if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(s, 16) != 0) {
            const int code = WSAGetLastError();
            closesocket(s);
            veyra::log::warn("guest-http", std::format("cannot listen on {}:{} wsa={}", addresses[i], port, code));
            if (i == 0) { error = std::format("port {} occupé ({})", port, code); stop(); return false; }
            continue;
        }
        p.listeners.push_back(s);
        p.bound.push_back(addresses[i]);
    }
    if (p.listeners.empty()) { error = "aucune adresse d'écoute"; stop(); return false; }
    p.port = port;
    p.running = true;
    p.acceptThread = std::thread([impl = p_] { impl->acceptLoop(); });
    return true;
}

void GuestHttp::stop() {
    auto& p = *p_;
    p.running = false;
    if (p.acceptThread.joinable()) p.acceptThread.join();
    for (SOCKET s : p.listeners) closesocket(s);
    p.listeners.clear(); p.bound.clear(); p.port = 0;
    {
        std::unique_lock lock(p.m);
        p.cv.wait_for(lock, std::chrono::seconds(6), [&] { return p.active == 0; });
    }
    if (p.wsa) { WSACleanup(); p.wsa = false; }
}

} // namespace veyra::guest
