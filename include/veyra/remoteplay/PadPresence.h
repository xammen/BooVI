#pragma once
#include <cstdint>
#include <string>
#include <string_view>

namespace veyra::remoteplay {

// What the PS5 stream's overlay says about the PC's pad (PadNotice.qml). A DualSense left
// paired with the PS5 never reaches the PC, so after kGrace without a pad the overlay shows
// how to bring it over; a pad that comes in later gets a short toast. The UI thread calls
// update() every tick. State model: docs/CONTROLLER_CONNECTION_2026-11-20.md.
class PadPresence {
public:
    enum class State : uint8_t { Off, Waiting, Missing, Quiet, Reading };
    static constexpr uint64_t kGraceMs = 3000;   // no pad this long while live: the hint shows
    static constexpr uint64_t kSettleMs = 1000;  // a pad read this soon after the picture started was already there

    // `live`: a PS5 picture streams and the PC sends input (not view-only). `kbm`: keyboard and
    // mouse are captured. `pad`: a pad is read, named `name` (a new name is a switch).
    // Returns true when state() or arrivals() changed.
    bool update(bool live, bool kbm, bool pad, std::string_view name, uint64_t nowMs) {
        const State oldState = state_;
        const unsigned oldArrivals = arrivals_;
        if (!live) {
            state_ = State::Off;
            name_.clear();
        } else if (pad) {
            if (state_ == State::Off) liveSince_ = nowMs;
            const bool switched = state_ == State::Reading && name != name_;
            const bool cameLate = state_ != State::Off && state_ != State::Reading && nowMs - liveSince_ >= kSettleMs;
            if (switched || cameLate) ++arrivals_;
            state_ = State::Reading;
            name_ = name;
        } else {
            if (state_ == State::Off) liveSince_ = nowMs;
            if (state_ == State::Off || state_ == State::Reading) {
                noPadSince_ = nowMs;
                state_ = State::Waiting;
            }
            name_.clear();
            if (kbm && (state_ == State::Waiting || state_ == State::Missing)) state_ = State::Quiet;
            if (state_ == State::Waiting && nowMs - noPadSince_ >= kGraceMs) state_ = State::Missing;
        }
        return state_ != oldState || arrivals_ != oldArrivals;
    }
    // The user closed the hint: quiet until a pad is read.
    void close() {
        if (state_ == State::Waiting || state_ == State::Missing) state_ = State::Quiet;
    }

    State state() const { return state_; }
    bool hint() const { return state_ == State::Missing; }
    unsigned arrivals() const { return arrivals_; }   // pads that came in mid-stream, this run

private:
    State state_ = State::Off;
    std::string name_;
    uint64_t liveSince_ = 0, noPadSince_ = 0;
    unsigned arrivals_ = 0;
};

}
