#pragma once
// Fork, partage: the user's Cloudflare secrets for sharing (a named tunnel's token and a TURN
// key), DPAPI-encrypted for the current Windows user in <user data>\guest\cloudflare.dpapi (UserDataRoot.h).
// scripts\fork\guest-cf-secrets.ps1 writes it. Nothing here is logged.
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace veyra::guest {

struct CloudflareConfig {
    std::string hostname;       // the tunnel's public hostname, e.g. play.example.com
    std::string tunnelToken;
    std::string turnKeyId, turnApiToken;
};

std::optional<CloudflareConfig> loadCloudflareConfig();

// Short-lived credentials from Cloudflare's TURN service, as RTCPeerConnection's iceServers
// ([{urls: [...]}, {urls: [...], username, credential}]). An empty array and `error` on failure.
nlohmann::json fetchTurnIceServers(const CloudflareConfig& config, int ttlSeconds, std::string& error);

} // namespace veyra::guest
