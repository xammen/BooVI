#pragma once
// Fork, partage: cloudflared, run for the length of a share session so the guest page and its
// signaling reach this PC through Cloudflare. Named: the user's tunnel (token from
// GuestCloudflare.h, handed over in the child's environment, never on its command line).
// Quick: `tunnel --url`, an account-less trycloudflare.com URL read from cloudflared's output.
// The process lives in a kill-on-close job object, so it never outlives BooVI.
#include <cstdint>
#include <memory>
#include <string>

#include "veyra/guest/GuestStreamer.h"

namespace veyra::guest {

class GuestTunnel {
public:
    GuestTunnel();
    ~GuestTunnel();
    GuestTunnel(const GuestTunnel&) = delete;
    GuestTunnel& operator=(const GuestTunnel&) = delete;

    bool startNamed(const std::string& token, const std::string& hostname, std::string& error);
    bool startQuick(uint16_t port, std::string& error);
    void stop();
    TunnelPhase phase() const;
    std::string publicUrl() const;   // https://host, empty until a quick tunnel prints it
    std::string error() const;

private:
    struct Impl;
    std::shared_ptr<Impl> p_;
};

} // namespace veyra::guest
