#pragma once
// Fork, partage: the smallest HTTP/1.1 server the guest page needs. It listens on the
// addresses it is given (loopback, and the Tailscale address when there is one, never all
// interfaces), answers each request on a thread of its own and closes the connection.
// Bodies are capped at 64 KB; a client that sends nothing for 5 s is dropped.
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace veyra::guest {

struct HttpRequest {
    std::string method, path, query, body;
    // Who asked: the peer's address, or the guest's as the broker saw it (GuestBroker.h).
    // Never logged whole.
    std::string source;
    // Where the handler is (a string literal), for the broker's log of a request past its deadline.
    std::atomic<const char*>* stage = nullptr;
    void at(const char* where) const { if (stage) stage->store(where); }
    // The broker's TURN lease this request took: a request past its deadline hands it back.
    std::function<void(const std::string&)> onLease;
    void holdLease(const std::string& id) const { if (onLease && !id.empty()) onLease(id); }
};
struct HttpResponse {
    int status = 200;
    std::string type = "text/plain; charset=utf-8";
    std::string body;
};

class GuestHttp {
public:
    using Handler = std::function<HttpResponse(const HttpRequest&)>;
    GuestHttp();
    ~GuestHttp();
    GuestHttp(const GuestHttp&) = delete;
    GuestHttp& operator=(const GuestHttp&) = delete;
    // Binds `port` on each address (the first one must bind; the others are best effort).
    bool start(const std::vector<std::string>& addresses, uint16_t port, Handler handler, std::string& error);
    void stop();   // closes the sockets and waits for requests in progress
    uint16_t port() const;
    std::vector<std::string> bound() const;
private:
    struct Impl;
    std::shared_ptr<Impl> p_;
};

} // namespace veyra::guest
