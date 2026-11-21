#pragma once
// Fork, partage: this PC's sharing secrets, DPAPI-encrypted for the current Windows user under
// <user data>\guest (UserDataRoot.h). Nothing here is logged.
//   cloudflare.dpapi   written by scripts\fork\guest-cf-secrets.ps1: the owner key, which lets
//                      the broker lend its TURN relay (GuestBroker.h), and/or this user's own
//                      Cloudflare TURN key.
//   room.dpapi         the room key (GuestRoom.h), made on first use: the broker link and the
//                      friends' access links (GuestAccess.h) stay the same from share to share.
//   discord.dpapi      the Discord webhook URL the owner pasted in réglages → partage
//                      (GuestNotify.h, GuestStreamer.h); a secret: whoever has it posts in the channel.
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace veyra::guest {

struct CloudflareConfig {
    std::string ownerKey;
    std::string turnKeyId, turnApiToken;
};

std::optional<CloudflareConfig> loadCloudflareConfig();

// Short-lived credentials from the user's own TURN key, as RTCPeerConnection's iceServers
// ([{urls: [...]}, {urls: [...], username, credential}]). An empty array and `error` on failure.
nlohmann::json fetchTurnIceServers(const CloudflareConfig& config, int ttlSeconds, std::string& error);

// This PC's room key; empty when it can be neither read nor made.
std::string loadOrCreateRoomKey();

} // namespace veyra::guest
