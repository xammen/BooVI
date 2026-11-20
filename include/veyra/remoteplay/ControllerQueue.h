#pragma once
#include <algorithm>
#include <deque>
#include <utility>
#include "veyra/remoteplay/Types.h"

namespace veyra::remoteplay {

using ControllerQueue = std::deque<std::pair<HostTime, ControllerState>>;

// True when a game sees `next` as the same event state as `prev`: same buttons, same touch
// contacts, same focus state. Only sticks, triggers and motion differ.
inline bool sameEdges(const ControllerState& prev, const ControllerState& next) {
    if (prev.inputActive != next.inputActive || prev.buttons != next.buttons) return false;
    for (size_t i = 0; i < prev.touches.size(); ++i)
        if (prev.touches[i].id != next.touches[i].id) return false;
    return true;
}

// Queues `next`. The session thread sends one queued state every 4 ms, so a stall left a
// backlog that was replayed late, analog positions included (up to 100 ms old). A state that
// changes no button or touch now replaces the last queued one instead: every press and
// release still reaches the PS5 in order, and stick positions are always the newest.
// The replaced state's motion samples are kept in front of the new ones (newest 16 kept),
// so the gyro integration on the console still sees every sample it can hold.
inline void pushCoalesced(ControllerQueue& queue, HostTime stamp, ControllerState next) {
    if (!queue.empty() && sameEdges(queue.back().second, next)) {
        const ControllerState& prev = queue.back().second;
        std::array<ControllerState::MotionSample, 32> merged{};
        size_t count = 0;
        for (size_t i = 0; i < prev.motionSampleCount; ++i) merged[count++] = prev.motionSamples[i];
        for (size_t i = 0; i < next.motionSampleCount; ++i) merged[count++] = next.motionSamples[i];
        const size_t keep = std::min<size_t>(count, next.motionSamples.size());
        std::copy(merged.begin() + (count - keep), merged.begin() + count, next.motionSamples.begin());
        next.motionSampleCount = static_cast<uint8_t>(keep);
        queue.back() = {stamp, std::move(next)};
        return;
    }
    queue.push_back({stamp, std::move(next)});
}

}
