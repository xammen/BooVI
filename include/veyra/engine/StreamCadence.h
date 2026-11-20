#pragma once
#include <algorithm>
#include <bit>
#include <cstdint>

namespace veyra::engine {
// Content cadence of a console stream (2026-11-20).
//
// PS5 Remote Play sends a fixed 60 pictures a second. A game that renders 30 sends
// every picture twice, and frame generation then interpolates half its pairs between
// two equal pictures: the motion stays at 30 steps a second, each followed by one
// blended picture. Dropping the second copies before any GPU work leaves pairs of real
// pictures 33 ms apart, which frame generation can fill evenly.
//
// ContentDedup.h does this for capture cards, where a repeat is the same picture plus
// sensor noise. A compressed stream is different: a slow fade or a drifting menu
// background changes every picture by less than any fixed noise band, so "repeat until
// the change adds up" turned one into 10-20 irregular pictures a second and held real
// frames up to 350 ms (pause menu of a PS5 game, software-decoded run, 2026-11-20).
// So this class never drops on a threshold alone. It looks for the one pattern worth
// acting on, changed / unchanged / changed / unchanged, and drops second copies only
// while that pattern holds:
//
//   Full  every picture is kept (60 fps content, menus, stills, anything irregular).
//   Half  the game runs at half the stream rate: a picture that repeats the one before
//         it is dropped.
//
// The input is, for each picture, how many of its luma samples differ from the picture
// before it (StreamPictureDiff, a noise band of 3 levels in 255). A second copy is not
// equal to its first: the console's encoder keeps refining it. Two 30 fps scenes of one
// PS5 game (1080p HEVC, 2026-11-20):
//
//   quiet scene   copies 0-171 samples, mostly under 20     new pictures 3 800-120 000
//   busy scene    copies 800-2 300, now and then 5 600      new pictures 17 000-1 000 000
//
// No constant separates those, and four times the usual copy took an 81-sample copy for
// a new picture. So each picture is judged against its neighbour: a copy changes at most
// a third of the new picture it follows, a new picture more than twice the copies before it.
//
// That game also shows three new pictures in a row when it misses its frame time (a torn
// picture, then the rest of it). Only four in a row, or three quarters of the last 32
// pictures, mean the content got faster than half the stream.
class StreamCadence {
public:
    enum class Mode { Full, Half };
    struct Decision {
        bool keep = true;
        bool newPicture = true;   // differs from the picture before it (for the content rate shown)
        bool hold = false;        // a kept repeat: the picture is still, refreshed at the content cadence
        Mode mode = Mode::Full;
    };
    struct Stats { uint64_t observed = 0, kept = 0, dropped = 0, newPictures = 0, held = 0, entered = 0, left = 0; };

    // Alternating steps in a row before second copies are dropped: 16 pictures, 0.27 s.
    static constexpr unsigned kEnterSteps = 16;
    // After content left Half by running faster, it has to alternate this much longer.
    static constexpr unsigned kMaxEnterSteps = 120;
    // To enter, a changed picture has this many times the samples of the unchanged one beside it.
    static constexpr uint64_t kEnterRatio = 4;
    // A copy changes at most 1/kCopyRatio of the new picture before it.
    static constexpr uint64_t kCopyRatio = 3;
    // A new picture changes more than kNewRatio times the smaller of the two copies before it
    // (one copy in fifty is refined several times as much as the others).
    static constexpr uint64_t kNewRatio = 2;
    // New pictures in a row, and new pictures among the last 32, that end Half.
    static constexpr unsigned kFastRun = 4, kFastOf32 = 24;
    // A picture that has repeated this long is still: refresh it every second picture.
    static constexpr unsigned kHoldAfter = 4;

    void reset() { const auto stats = stats_; *this = StreamCadence{}; stats_ = stats; }
    Mode mode() const { return mode_; }
    const Stats& stats() const { return stats_; }

    // The smallest count that makes a new picture: about 8 per million samples, 16 at least.
    static constexpr uint64_t floorFor(uint64_t total) { return std::max<uint64_t>(16, total / 131072); }

    // A picture with no measurement (first of a session, a decode on the CPU): everything is kept.
    Decision blind() {
        reset();
        return count({true, true, false, Mode::Full});
    }

    // The picture before this one was never seen (the reader fell behind): the next count spans
    // two pictures, so it neither continues an alternation nor counts as new pictures in a row.
    void gap() { run_ = 0; havePrevious_ = false; previousNew_ = false; newRun_ = 0; }

    // changed: samples of this picture beyond the noise band of the previous picture, of `total`.
    // shown: the picture was already processed when its count arrived (in Full nothing waits for
    // the count), so it cannot be dropped any more: a second copy then counts as a kept still.
    Decision observe(uint64_t changed, uint64_t total, bool shown = false) {
        const uint64_t floor = floorFor(total);
        if (mode_ == Mode::Full) {
            auto d = observeFull(changed, floor);
            if (!d.keep && shown) { d.keep = true; sinceKept_ = 0; }
            return count(d);
        }
        const bool repeat = previousNew_ ? changed <= floor || changed * kCopyRatio <= lastNew_
                                         : changed <= std::max(floor, std::min(lastCopy_, earlierCopy_) * kNewRatio);
        recent_ = (recent_ << 1) | uint32_t(!repeat);
        ++inHalf_;
        if (repeat) {
            earlierCopy_ = lastCopy_; lastCopy_ = changed; previousNew_ = false; newRun_ = 0;
            ++sinceKept_;
            if (!shown && (still_ ? sinceKept_ < 2 : sinceKept_ < kHoldAfter)) return count({false, false, false, Mode::Half});
            still_ = true; sinceKept_ = 0; stable_ = 0;
            return count({true, false, true, Mode::Half});
        }
        if (++newRun_ >= kFastRun || (inHalf_ >= 32 && unsigned(std::popcount(recent_)) >= kFastOf32)) {
            mode_ = Mode::Full; ++stats_.left;
            need_ = std::min(need_ * 2, kMaxEnterSteps);
            run_ = 0; havePrevious_ = true; previous_ = changed;
            return count({true, true, false, Mode::Full});
        }
        previousNew_ = true; still_ = false; sinceKept_ = 0; lastNew_ = changed;
        // Ten seconds of steady half-rate content forgive an earlier exit.
        if (++stable_ >= 300) { need_ = kEnterSteps; stable_ = 0; }
        return count({true, true, false, Mode::Half});
    }

private:
    Decision observeFull(uint64_t changed, uint64_t floor) {
        // A copy of a new picture is no new picture, also while every picture is still kept.
        Decision d{true, changed >= floor && (!havePrevious_ || changed * kCopyRatio > previous_), false, Mode::Full};
        ++index_;
        if (havePrevious_) {
            const bool lowNow = changed < previous_;
            const uint64_t low = lowNow ? changed : previous_, high = lowNow ? previous_ : changed;
            const bool parity = (lowNow ? index_ : index_ - 1) & 1;
            const bool step = high >= floor && high >= low * kEnterRatio + floor;
            if (!step) run_ = 0;
            else if (run_ && parity != lowParity_) { run_ = 1; lowParity_ = parity; }
            else { ++run_; lowParity_ = parity; }
            if (run_ >= need_ && lowNow) {
                // This picture is a second copy: the first one to go.
                mode_ = Mode::Half; ++stats_.entered;
                lastNew_ = previous_; lastCopy_ = earlierCopy_ = changed;
                previousNew_ = false; still_ = false; sinceKept_ = 1; stable_ = 0; run_ = 0;
                newRun_ = 0; recent_ = 0; inHalf_ = 0;
                d = {false, false, false, Mode::Half};
            }
        }
        previous_ = changed; havePrevious_ = true;
        return d;
    }
    Decision count(Decision d) {
        ++stats_.observed;
        stats_.kept += d.keep; stats_.dropped += !d.keep; stats_.newPictures += d.newPicture; stats_.held += d.hold;
        return d;
    }

    Mode mode_ = Mode::Full;
    uint64_t index_ = 0, previous_ = 0, lastNew_ = 0, lastCopy_ = 0, earlierCopy_ = 0, inHalf_ = 0;
    uint32_t recent_ = 0;
    unsigned run_ = 0, need_ = kEnterSteps, sinceKept_ = 0, stable_ = 0, newRun_ = 0;
    bool havePrevious_ = false, lowParity_ = false, previousNew_ = false, still_ = false;
    Stats stats_;
};
}
