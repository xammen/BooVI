#include "veyra/guest/TurnTunnel.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <wincrypt.h>

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <atomic>
#include <format>
#include <mutex>
#include <thread>

#include "veyra/Log.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "crypt32.lib")

namespace veyra::guest {
namespace {

constexpr size_t kMaxQueued = 4u << 20;   // bytes waiting for the server; past it, datagrams are dropped as UDP would
constexpr int kConnectMs = 4000;

bool wouldBlock() { const int e = WSAGetLastError(); return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS; }

// The system's trusted roots, so the server's certificate is checked as WinHTTP would.
void loadWindowsRoots(SSL_CTX* ctx) {
    HCERTSTORE store = CertOpenSystemStoreW(0, L"ROOT");
    if (!store) return;
    X509_STORE* roots = SSL_CTX_get_cert_store(ctx);
    for (PCCERT_CONTEXT c = nullptr; (c = CertEnumCertificatesInStore(store, c)) != nullptr;) {
        const unsigned char* der = c->pbCertEncoded;
        if (X509* x = d2i_X509(nullptr, &der, long(c->cbCertEncoded))) { X509_STORE_add_cert(roots, x); X509_free(x); }
    }
    CertCloseStore(store, 0);
}

} // namespace

struct TurnTunnel::Impl {
    std::string host;
    uint16_t port = 0;
    bool tls = true;
    SOCKET udp = INVALID_SOCKET, tcp = INVALID_SOCKET;
    SSL_CTX* ctx = nullptr;
    SSL* ssl = nullptr;
    uint16_t local = 0;
    std::atomic<bool> stopping{false};
    std::thread thread;
    mutable std::mutex m;
    TurnTunnelStats st;
    sockaddr_in juice{};            // where libjuice sends from: the replies go there
    bool haveJuice = false;
    std::string out, in;            // the stream, each way

    ~Impl() {
        if (ssl) SSL_free(ssl);
        if (ctx) SSL_CTX_free(ctx);
        if (tcp != INVALID_SOCKET) closesocket(tcp);
        if (udp != INVALID_SOCKET) closesocket(udp);
        WSACleanup();
    }

    void fail(const std::string& why) {
        { std::lock_guard lock(m); st.connected = false; if (st.error.empty()) st.error = why; }
        veyra::log::warn("guest-turn-tunnel", std::format("{} {}:{}: {}", tls ? "tls" : "tcp", host, port, why));
    }

    bool connectServer() {
        addrinfo hints{}, *list = nullptr;
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &list) != 0 || !list) { fail("name not resolved"); return false; }
        for (addrinfo* a = list; a && tcp == INVALID_SOCKET && !stopping; a = a->ai_next) {
            SOCKET s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
            if (s == INVALID_SOCKET) continue;
            u_long nb = 1;
            ioctlsocket(s, FIONBIO, &nb);
            if (connect(s, a->ai_addr, int(a->ai_addrlen)) != 0 && !wouldBlock()) { closesocket(s); continue; }
            fd_set w, e; FD_ZERO(&w); FD_ZERO(&e); FD_SET(s, &w); FD_SET(s, &e);
            timeval tv{kConnectMs / 1000, (kConnectMs % 1000) * 1000};
            if (select(0, nullptr, &w, &e, &tv) == 1 && FD_ISSET(s, &w)) tcp = s;
            else closesocket(s);
        }
        freeaddrinfo(list);
        if (tcp == INVALID_SOCKET) { fail("no connection"); return false; }
        const BOOL nodelay = TRUE;
        setsockopt(tcp, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));
        if (tls) {
            // The handshake blocks (bounded), the stream after it does not.
            u_long nb = 0;
            ioctlsocket(tcp, FIONBIO, &nb);
            const DWORD timeout = kConnectMs;
            setsockopt(tcp, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
            setsockopt(tcp, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
            ctx = SSL_CTX_new(TLS_client_method());
            if (!ctx) { fail("tls context"); return false; }
            SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
            SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
            loadWindowsRoots(ctx);
            ssl = SSL_new(ctx);
            SSL_set_fd(ssl, int(tcp));
            SSL_set_tlsext_host_name(ssl, host.c_str());
            SSL_set1_host(ssl, host.c_str());
            SSL_set_mode(ssl, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
            if (SSL_connect(ssl) != 1) {
                const long verify = SSL_get_verify_result(ssl);
                fail(verify != X509_V_OK ? std::format("certificate refused ({})", X509_verify_cert_error_string(verify)) : "tls handshake failed");
                return false;
            }
            nb = 1;
            ioctlsocket(tcp, FIONBIO, &nb);
        }
        std::lock_guard lock(m);
        st.connected = true;
        return true;
    }

    // libjuice's datagrams onto the stream: ChannelData is padded to 4 bytes there.
    void fromJuice() {
        char buf[2048];
        for (;;) {
            sockaddr_in from{};
            int len = sizeof(from);
            const int n = recvfrom(udp, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &len);
            if (n <= 0) return;
            if (from.sin_addr.s_addr != htonl(INADDR_LOOPBACK)) continue;
            juice = from; haveJuice = true;
            if (n < 4 || out.size() > kMaxQueued) continue;
            out.append(buf, size_t(n));
            if ((uint8_t(buf[0]) & 0xC0) == 0x40) out.append((4 - n % 4) % 4, '\0');
            std::lock_guard lock(m);
            ++st.sent;
        }
    }

    // The server's messages back to libjuice, one datagram each.
    void toJuice() {
        size_t at = 0;
        while (in.size() - at >= 4) {
            const auto* b = reinterpret_cast<const uint8_t*>(in.data() + at);
            const size_t length = (size_t(b[2]) << 8) | b[3];
            size_t total, padded;
            if ((b[0] & 0xC0) == 0x00) { total = padded = 20 + length; }        // STUN
            else if ((b[0] & 0xC0) == 0x40) { total = 4 + length; padded = (total + 3) & ~size_t(3); }   // ChannelData
            else { fail("stream out of step"); in.clear(); return; }
            if (in.size() - at < padded) break;
            if (haveJuice) sendto(udp, in.data() + at, int(total), 0, reinterpret_cast<const sockaddr*>(&juice), sizeof(juice));
            at += padded;
            std::lock_guard lock(m);
            ++st.received;
        }
        in.erase(0, at);
    }

    bool flush() {
        while (!out.empty()) {
            const int chunk = int(std::min<size_t>(out.size(), 64 * 1024));
            if (tls) {
                const int n = SSL_write(ssl, out.data(), chunk);
                if (n > 0) { out.erase(0, size_t(n)); continue; }
                const int e = SSL_get_error(ssl, n);
                if (e == SSL_ERROR_WANT_WRITE || e == SSL_ERROR_WANT_READ) return true;
                fail("tls write"); return false;
            }
            const int n = send(tcp, out.data(), chunk, 0);
            if (n > 0) { out.erase(0, size_t(n)); continue; }
            if (wouldBlock()) return true;
            fail("tcp write"); return false;
        }
        return true;
    }

    bool drain() {
        char buf[16 * 1024];
        for (;;) {
            int n;
            if (tls) {
                n = SSL_read(ssl, buf, sizeof(buf));
                if (n <= 0) {
                    const int e = SSL_get_error(ssl, n);
                    if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) break;
                    fail(e == SSL_ERROR_ZERO_RETURN ? "closed by the server" : "tls read"); return false;
                }
            } else {
                n = recv(tcp, buf, sizeof(buf), 0);
                if (n == 0) { fail("closed by the server"); return false; }
                if (n < 0) { if (wouldBlock()) break; fail("tcp read"); return false; }
            }
            in.append(buf, size_t(n));
        }
        toJuice();
        return true;
    }

    void run() {
        SetThreadDescription(GetCurrentThread(), L"Guest TURN tunnel");
        if (!connectServer()) return;
        veyra::log::info("guest-turn-tunnel", std::format("{} {}:{} connected, relaying 127.0.0.1:{}", tls ? "tls" : "tcp", host, port, local));
        while (!stopping) {
            WSAPOLLFD fds[2]{};
            fds[0].fd = udp; fds[0].events = POLLRDNORM;
            fds[1].fd = tcp; fds[1].events = short(POLLRDNORM | (out.empty() ? 0 : POLLWRNORM));
            const bool pending = tls && SSL_pending(ssl) > 0;
            if (!pending && WSAPoll(fds, 2, 50) < 0) { fail("poll"); return; }
            if (fds[0].revents & POLLRDNORM) fromJuice();
            if ((fds[1].revents & (POLLRDNORM | POLLHUP | POLLERR)) || pending) { if (!drain()) return; }
            if (!out.empty() && !flush()) return;
        }
    }
};

std::unique_ptr<TurnTunnel> TurnTunnel::open(const std::string& host, uint16_t port, bool tls, std::string& error) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { error = "winsock"; return nullptr; }
    auto impl = std::make_shared<Impl>();
    impl->host = host; impl->port = port; impl->tls = tls;
    impl->udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in at{};
    at.sin_family = AF_INET;
    at.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int len = sizeof(at);
    if (impl->udp == INVALID_SOCKET || bind(impl->udp, reinterpret_cast<sockaddr*>(&at), sizeof(at)) != 0 ||
        getsockname(impl->udp, reinterpret_cast<sockaddr*>(&at), &len) != 0) { error = std::format("udp bind {}", WSAGetLastError()); return nullptr; }
    u_long nb = 1;
    ioctlsocket(impl->udp, FIONBIO, &nb);
    impl->local = ntohs(at.sin_port);
    impl->thread = std::thread([impl] { impl->run(); });
    std::unique_ptr<TurnTunnel> out(new TurnTunnel());
    out->p_ = std::move(impl);
    return out;
}

TurnTunnel::~TurnTunnel() {
    if (!p_) return;
    p_->stopping = true;
    if (p_->thread.joinable()) p_->thread.join();
}

uint16_t TurnTunnel::localPort() const { return p_->local; }
TurnTunnelStats TurnTunnel::stats() const { std::lock_guard lock(p_->m); return p_->st; }

} // namespace veyra::guest
