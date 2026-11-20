#pragma once
#include <cstdint>
#include <span>
#include "veyra/remoteplay/Types.h"

namespace veyra::remoteplay {

// One pad SDL reports as a gamepad. `dualSense` is SDL_GAMEPAD_TYPE_PS5.
struct PadCandidate {
    uint32_t id = 0;
    bool dualSense = false;
};

// Which pad the PS5 stream reads. Upstream opened the first pad SDL listed and kept it,
// so a virtual pad (DS4Windows, DSX, a streaming tool) that was already there won over a
// DualSense switched on after the stream started. The fork reads a DualSense whenever one
// is connected, and otherwise the first pad. Returns 0 when there is no pad.
// `current` is the pad being read now (0 for none).
inline uint32_t choosePad(uint32_t current, std::span<const PadCandidate> available) {
    const PadCandidate* first = nullptr;
    const PadCandidate* dualSense = nullptr;
    const PadCandidate* kept = nullptr;
    for (const auto& pad : available) {
        if (!first) first = &pad;
        if (pad.dualSense && !dualSense) dualSense = &pad;
        if (pad.id == current) kept = &pad;
    }
    if (kept && kept->dualSense) return kept->id;
    if (dualSense) return dualSense->id;
    if (kept) return kept->id;
    return first ? first->id : 0;
}

// When the input thread hands the merged pad and keyboard/mouse state to the session: at once
// when it differs from the last state handed over, and otherwise every `keepalive`, because
// the session drops to neutral after 100 ms without a state. Pacing the network is not this
// gate's job: chiaki sends a state packet at most every 8 ms and button edges as events.
struct PadSendGate {
    static constexpr HostTime kKeepalive = 80000; // 8 ms in 100 ns
    ControllerState last;
    HostTime lastSent = 0;
    bool sent = false;
    bool due(const ControllerState& next, HostTime now) {
        if (sent && next == last && now - lastSent < kKeepalive) return false;
        last = next; lastSent = now; sent = true;
        return true;
    }
};

// A PS press the app makes by itself: after a wake the PS5 waits on its "press the PS button"
// screen. Held for kHold on top of the real input, so the gate sends the press, then the release.
struct PsTap {
    static constexpr HostTime kHold = 1500000; // 150 ms in 100 ns
    HostTime until = 0;
    void press(HostTime now) { until = now + kHold; }
    ControllerState apply(ControllerState state, HostTime now) const {
        if (now >= until) return state;
        state.inputActive = true;
        state.buttons |= ControllerState::PS;
        return state;
    }
};

}
