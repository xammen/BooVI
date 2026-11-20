#include "veyra/guest/CloudflaredLog.h"
#include <cstdio>

using veyra::guest::TunnelEvent;
using veyra::guest::parseCloudflaredLine;

static int failures = 0;
static void expect(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

int main() {
    // Lines as cloudflared 2026.8.3 prints them (a quick tunnel run, addresses and ids changed).
    const auto url = parseCloudflaredLine("2026-11-20T03:13:01Z INF |  https://inside-micro-never-cancel.trycloudflare.com                                       |");
    expect(url.quickUrl == "https://inside-micro-never-cancel.trycloudflare.com", "quick tunnel: the URL in the banner");
    expect(url.event == TunnelEvent::None, "quick tunnel: the banner is not a connection event");

    const auto banner = parseCloudflaredLine("2026-11-20T03:13:01Z INF |  Your quick Tunnel has been created! Visit it at (it may take some time to be reachable):  |");
    expect(banner.quickUrl.empty() && banner.event == TunnelEvent::None, "quick tunnel: the banner's title is nothing");

    const auto api = parseCloudflaredLine("2026-11-20T03:13:01Z INF Requesting new quick Tunnel on trycloudflare.com...");
    expect(api.quickUrl.empty(), "quick tunnel: the request line names no URL");

    const auto up = parseCloudflaredLine("2026-11-20T03:13:01Z INF Registered tunnel connection connIndex=2 connection=00000000-0000-0000-0000-000000000000 event=0 ip=198.51.100.7 location=mrs01 protocol=quic");
    expect(up.event == TunnelEvent::Up && up.connIndex == 2, "registered: connection 2 up");

    const auto pre = parseCloudflaredLine("2026-11-20T03:13:01Z INF Tunnel connection curve preferences: [X25519MLKEM768 CurveID(65074) CurveP256] connIndex=0 event=0 ip=198.51.100.7");
    expect(pre.event == TunnelEvent::None, "curve preferences: not a connection yet");

    const auto down = parseCloudflaredLine("2026-11-20T03:20:00Z ERR Connection terminated error=\"timeout: no recent network activity\" connIndex=1");
    expect(down.event == TunnelEvent::Down && down.connIndex == 1, "terminated: connection 1 down");

    const auto gone = parseCloudflaredLine("2026-11-20T03:21:00Z INF Unregistered tunnel connection connIndex=3 event=0 ip=198.51.100.7");
    expect(gone.event == TunnelEvent::Down && gone.connIndex == 3, "unregistered: connection 3 down");

    const auto failed = parseCloudflaredLine("2026-11-20T03:13:01Z ERR failed to request quick Tunnel: Post \"https://api.trycloudflare.com/tunnel\": dial tcp: i/o timeout");
    expect(failed.quickUrl.empty(), "the quick tunnel API's own address is not a tunnel URL");

    const auto other =parseCloudflaredLine("2026-11-20T03:13:01Z INF Visit https://example.com/docs for more");
    expect(other.quickUrl.empty() && other.event == TunnelEvent::None, "another https URL is not a quick tunnel");

    if (failures) { std::printf("%d FAILED\n", failures); return 1; }
    std::printf("all passed\n");
    return 0;
}
