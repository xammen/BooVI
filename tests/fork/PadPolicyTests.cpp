#include "veyra/remoteplay/PadPolicy.h"
#include <cstdio>
#include <vector>

using veyra::remoteplay::PadCandidate;
using veyra::remoteplay::choosePad;

static int failures = 0;
static void expect(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

int main() {
    const PadCandidate xboxVirtual{11, false};
    const PadCandidate dualSense{12, true};
    const PadCandidate secondDualSense{13, true};

    expect(choosePad(0, std::vector<PadCandidate>{}) == 0, "no pad: nothing is read");
    expect(choosePad(0, std::vector{xboxVirtual}) == 11, "only a virtual pad: it is read");
    expect(choosePad(0, std::vector{xboxVirtual, dualSense}) == 12, "virtual listed first: the DualSense is read");
    expect(choosePad(11, std::vector{xboxVirtual, dualSense}) == 12, "DualSense arrives while a virtual pad is read: switch to it");
    expect(choosePad(12, std::vector{xboxVirtual, dualSense, secondDualSense}) == 12, "a DualSense being read is kept");
    expect(choosePad(13, std::vector{dualSense, secondDualSense}) == 13, "the second DualSense being read is kept");
    expect(choosePad(12, std::vector{xboxVirtual}) == 11, "the DualSense left: fall back to the remaining pad");
    expect(choosePad(99, std::vector{dualSense}) == 12, "the read pad vanished: pick the DualSense");

    using veyra::remoteplay::ControllerState;
    constexpr veyra::remoteplay::HostTime ms = 10000;
    veyra::remoteplay::PadSendGate gate;
    ControllerState idle;
    idle.inputActive = true;
    expect(gate.due(idle, 100 * ms), "send gate: the first state goes out");
    expect(!gate.due(idle, 101 * ms), "send gate: the same state 1 ms later waits");
    expect(!gate.due(idle, 107 * ms), "send gate: and still waits at 7 ms");
    expect(gate.due(idle, 108 * ms), "send gate: unchanged for 8 ms, it goes out again as a keepalive");
    ControllerState pressed = idle;
    pressed.buttons = ControllerState::Cross;
    expect(gate.due(pressed, 109 * ms), "send gate: a press goes out at once");
    ControllerState tilted = pressed;
    tilted.leftX = 1;
    expect(gate.due(tilted, 109 * ms), "send gate: the smallest stick change goes out at once");
    expect(!gate.due(tilted, 116 * ms), "send gate: the keepalive restarts from the last send");

    veyra::remoteplay::PsTap tap;
    ControllerState none;
    expect(tap.apply(none, 200 * ms) == none, "PS tap: nothing is added before a request");
    tap.press(200 * ms);
    veyra::remoteplay::PadSendGate tapGate;
    tapGate.due(none, 199 * ms);
    const auto held = tap.apply(none, 200 * ms);
    expect(held.buttons == ControllerState::PS && held.inputActive, "PS tap: PS is held, even with no pad");
    expect(tapGate.due(held, 200 * ms), "PS tap: the press goes out at once");
    expect(tap.apply(pressed, 349 * ms).buttons == (ControllerState::Cross | ControllerState::PS), "PS tap: added on top of the real buttons");
    expect(!tapGate.due(tap.apply(none, 205 * ms), 205 * ms), "PS tap: holding is not a new state");
    const auto released = tap.apply(none, 350 * ms);
    expect(released == none, "PS tap: released after 150 ms");
    expect(tapGate.due(released, 350 * ms), "PS tap: the release goes out at once");
    std::printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
