// The live audio drift loop's re-anchor rule (CaptureAudioDsp.h captureDriftReanchor), on the
// errors reconstructed from the field log of 2026-11-20 (live-audio-sync: skew + video PTS - video
// host + ingress map + target).
#include "veyra/sink/CaptureAudioDsp.h"
#include <cstdio>
#include <optional>

using veyra::sink::captureDriftReanchor;

static int failures = 0;
static void expect(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

int main() {
    // 22:45 to 22:53: the target sat at 0 to 5 ms and every fresh anchor came back to -42 to
    // -55 ms. Jitter around -55 to -61 re-anchored 20 times in 8 minutes.
    expect(!captureDriftReanchor(-61.1, -56.0, -54.7), "jitter at the latency floor keeps playing (22:48:43)");
    expect(!captureDriftReanchor(-60.3, -57.0, -44.1), "late by 13 ms past what the anchor reached keeps playing (22:49:09)");
    expect(!captureDriftReanchor(-62.0, -60.5, -42.6), "one late spike past -60 with the filter at -60.5 keeps playing");
    // Video delay jumps (frame generation hold): audio ahead past 60 ms still re-anchors.
    expect(captureDriftReanchor(64.0, 30.0, -21.0), "audio 64 ms ahead after the target jumped re-anchors (22:25:11)");
    expect(captureDriftReanchor(61.0, 45.0, std::nullopt), "audio ahead before the anchor was measured re-anchors");
    // A video delay that drops: the anchor had reached the target, now audio is far behind it.
    expect(captureDriftReanchor(-75.0, -68.0, -3.0), "audio 68 ms late after the target dropped re-anchors");
    // A real slide beyond the floor (a stall, a lost block) re-anchors.
    expect(captureDriftReanchor(-90.0, -80.0, -50.0), "late 30 ms past what the anchor reached re-anchors");
    // Before the anchor is measured the old 60 ms limit applies (on the filtered error).
    expect(captureDriftReanchor(-70.0, -61.0, std::nullopt), "unmeasured anchor: late past 60 ms re-anchors");
    expect(!captureDriftReanchor(-58.0, -55.0, std::nullopt), "unmeasured anchor: late under 60 ms keeps playing");
    if (failures) { std::printf("%d FAILED\n", failures); return 1; }
    std::printf("all passed\n");
    return 0;
}
