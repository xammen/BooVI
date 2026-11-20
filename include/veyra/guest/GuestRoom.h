#pragma once
// Fork, partage: a share's room on BooVI's broker (broker/, a Cloudflare Worker). The host keeps
// a room key (264 random bits) on this PC; the room's id, the part of the link a guest sees, is
// the first 132 bits of SHA-256 over "boovi room v1\n" and the key. The broker opens a room only
// to the key its id is made from (broker/src/ids.ts derives it the same way).
#include <cctype>
#include <string>
#include <string_view>

#include "veyra/guest/GuestAccess.h"

namespace veyra::guest {

inline std::string newRoomKey() { return randomBase64Url(33); }

inline bool isRoomKey(std::string_view key) {
    if (key.size() != 44) return false;
    for (char c : key) if (!(std::isalnum(uint8_t(c)) || c == '-' || c == '_')) return false;
    return true;
}

inline std::string roomIdOf(std::string_view key) {
    const TokenHash h = hashToken(std::string("boovi room v1\n").append(key));
    static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    for (size_t i = 0; i < 18; i += 3) {   // 18 bytes are 24 characters; the id is the first 22
        const uint32_t v = (uint32_t(h[i]) << 16) | (uint32_t(h[i + 1]) << 8) | h[i + 2];
        for (int k = 3; k >= 0; --k) out.push_back(alphabet[(v >> (6 * k)) & 63]);
    }
    out.resize(22);
    return out;
}

} // namespace veyra::guest
