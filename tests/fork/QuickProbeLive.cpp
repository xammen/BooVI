// Live check of probePs5 against a real console: prints its state and how long the answer took.
// Usage: QuickProbeLive.exe <ps5 ip>
#include "veyra/remoteplay/QuickProbe.h"
#include <chrono>
#include <cstdio>

int main(int argc, char** argv) {
    if (argc < 2) { std::printf("usage: QuickProbeLive <ip>\n"); return 2; }
    const auto start = std::chrono::steady_clock::now();
    const auto state = veyra::remoteplay::probePs5(argv[1]);
    const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    const char* name = state == veyra::remoteplay::ProbeState::Ready ? "ready"
                     : state == veyra::remoteplay::ProbeState::RestMode ? "rest mode" : "no answer";
    std::printf("%s: %s in %.1f ms\n", argv[1], name, ms);
    return state == veyra::remoteplay::ProbeState::NoAnswer ? 1 : 0;
}
