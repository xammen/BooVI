#pragma once
// Fork, partage: TURN over TLS (or TCP) for libjuice, which speaks TURN over UDP only.
// The tunnel listens on a UDP port of 127.0.0.1 and carries what libjuice sends there to the
// TURN server over one TLS connection (RFC 5766 §2.1 framing: STUN messages and ChannelData,
// the latter padded to 4 bytes on a stream), and the server's replies back. The allocation, its
// relayed address and the peers stay UDP on the server's side, so the other end sees an
// ordinary relay candidate. Browsers do the same over turns:...:443 when UDP is blocked.
//
// One tunnel per TURN server entry and per peer connection; it closes with its owner.
#include <cstdint>
#include <memory>
#include <string>

namespace veyra::guest {

struct TurnTunnelStats {
    bool connected = false;      // the TLS (or TCP) connection to the server is up
    std::string error;           // why it is not, once it failed
    uint64_t sent = 0, received = 0;   // messages each way
};

class TurnTunnel {
public:
    // Binds 127.0.0.1:<any> now; the connection to `host`:`port` is made on the tunnel's own
    // thread, and what libjuice sends meanwhile waits for it. Null when the UDP port cannot bind.
    static std::unique_ptr<TurnTunnel> open(const std::string& host, uint16_t port, bool tls, std::string& error);
    ~TurnTunnel();
    TurnTunnel(const TurnTunnel&) = delete;
    TurnTunnel& operator=(const TurnTunnel&) = delete;

    uint16_t localPort() const;  // the TURN server address to give libjuice: 127.0.0.1:<this>
    TurnTunnelStats stats() const;

private:
    TurnTunnel() = default;
    struct Impl;
    std::shared_ptr<Impl> p_;
};

} // namespace veyra::guest
