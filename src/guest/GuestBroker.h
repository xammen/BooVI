#pragma once
// Fork, partage: the share's room on BooVI's broker (broker/, GuestRoom.h). For a share session
// the host keeps one outbound WebSocket to wss://<broker>/api/host/<room id>, and reconnects it
// when it drops. Each guest's signaling (/session, /answer from the page or a BooVI client)
// arrives on it wrapped with the guest's address, goes through the same handler as the local
// HTTP server's (GuestHttp.h), and the reply goes back to that guest.
//
// With the owner key (GuestCloudflare.h) the broker also lends TURN credentials, one lease per
// guest, under its monthly cap; endTurn() hands a lease back as soon as the guest no longer
// needs the relay, which refunds what is left of it.
#include <chrono>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "GuestHttp.h"
#include "veyra/guest/GuestStreamer.h"

namespace veyra::guest {

struct TurnLease {
    std::string id;                                       // empty: no relay for this guest
    nlohmann::json iceServers = nlohmann::json::array();  // never logged
    std::string why;                                      // when empty: "not-entitled", "budget", "turn", "offline", "timeout"
};

class GuestBroker {
public:
    using Handler = std::function<HttpResponse(const HttpRequest&)>;

    GuestBroker();
    ~GuestBroker();
    GuestBroker(const GuestBroker&) = delete;
    GuestBroker& operator=(const GuestBroker&) = delete;

    // `url`: https://<broker host>[:port]; `roomKey`: GuestRoom.h; `ownerKey` may be empty.
    bool start(const std::string& url, const std::string& roomKey, const std::string& ownerKey, Handler handler, std::string& error);
    void stop();
    BrokerPhase phase() const;
    bool entitled() const;        // the broker knows the owner key
    std::string roomLink() const; // https://<broker host>/<room id>
    std::string error() const;

    TurnLease requestTurn(std::chrono::milliseconds timeout);
    void endTurn(const std::string& lease);

private:
    struct Impl;
    std::shared_ptr<Impl> p_;
};

} // namespace veyra::guest
