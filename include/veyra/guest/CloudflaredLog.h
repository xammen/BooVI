#pragma once
// Fork, partage: what BooVI reads in cloudflared's log lines (GuestTunnel.cpp). The lines
// themselves are never logged or shown.
//   INF |  https://inside-micro-never-cancel.trycloudflare.com      |      -> a quick tunnel's URL
//   INF Registered tunnel connection connIndex=0 connection=… location=mrs01 protocol=quic
//   ERR Connection terminated error="…" connIndex=0   /   INF Unregistered tunnel connection connIndex=0
#include <optional>
#include <string>
#include <string_view>

namespace veyra::guest {

enum class TunnelEvent { None, Up, Down };

struct CloudflaredLine {
    TunnelEvent event = TunnelEvent::None;
    int connIndex = -1;
    std::string quickUrl;   // https://<name>.trycloudflare.com, empty otherwise
};

inline CloudflaredLine parseCloudflaredLine(std::string_view line) {
    CloudflaredLine out;
    constexpr std::string_view kScheme = "https://", kQuick = ".trycloudflare.com";
    if (const size_t at = line.find(kScheme); at != std::string_view::npos) {
        size_t end = at + kScheme.size();
        while (end < line.size()) {
            const char c = line[end];
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.')) break;
            ++end;
        }
        const std::string_view url = line.substr(at, end - at);
        if (url.size() > kScheme.size() + kQuick.size() && url.ends_with(kQuick) && url.find("api.") == std::string_view::npos)
            out.quickUrl = std::string(url);
    }
    if (line.find("Registered tunnel connection") != std::string_view::npos && line.find("Unregistered") == std::string_view::npos)
        out.event = TunnelEvent::Up;
    else if (line.find("Unregistered tunnel connection") != std::string_view::npos || line.find("Connection terminated") != std::string_view::npos)
        out.event = TunnelEvent::Down;
    if (out.event != TunnelEvent::None) {
        constexpr std::string_view kIndex = "connIndex=";
        if (const size_t at = line.find(kIndex); at != std::string_view::npos) {
            int n = 0; size_t i = at + kIndex.size();
            bool digits = false;
            for (; i < line.size() && line[i] >= '0' && line[i] <= '9'; ++i) { n = n * 10 + (line[i] - '0'); digits = true; }
            if (digits) out.connIndex = n;
        }
    }
    return out;
}

} // namespace veyra::guest
