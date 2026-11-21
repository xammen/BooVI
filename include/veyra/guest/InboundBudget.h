#pragma once
// Fork, partage: what one viewer may send the host on its "stats" and "room" data channels. A page
// sends a clock-sync ping every 500 ms (8 more in its first 420 ms), a latency report every 2 s,
// its quality wish and link test on a click; anything well past that is a broken or hostile page. Each kind of message has its own
// token bucket; a message past its bucket, or a stats message over kMaxStatsBytes, is dropped and
// counted, and the host logs the count, not the messages.
// Pure: tested in tests/fork/InboundBudgetTests.cpp.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>

namespace veyra::guest {

class TokenBucket {
public:
    constexpr TokenBucket(double perSecond, double burst) : perSecond_(perSecond), burst_(burst), tokens_(burst) {}
    bool take(int64_t nowUs) {
        if (atUs_) tokens_ = std::min(burst_, tokens_ + double(nowUs - atUs_) * perSecond_ / 1e6);
        atUs_ = nowUs;
        if (tokens_ < 1) return false;
        tokens_ -= 1;
        return true;
    }

private:
    double perSecond_, burst_, tokens_;
    int64_t atUs_ = 0;
};

enum class Inbound : uint8_t { Stats, Ping, Report, Behind, Wish, Probe, Bye, Room };

// The kind of a stats message, from its "t".
constexpr Inbound inboundOf(std::string_view t) {
    if (t == "ping") return Inbound::Ping;
    if (t == "rep") return Inbound::Report;
    if (t == "behind") return Inbound::Behind;
    if (t == "want" || t == "link") return Inbound::Wish;
    if (t == "probe" || t == "probe-stop") return Inbound::Probe;
    if (t == "bye") return Inbound::Bye;
    return Inbound::Stats;
}

// One viewer's budget. Inbound::Stats is every stats message before it is read; Inbound::Room
// every room command. Not thread-safe: the caller holds the viewer's lock.
class InboundBudget {
public:
    static constexpr size_t kMaxStatsBytes = 4096;

    bool admit(Inbound kind, int64_t nowUs, size_t bytes = 0) {
        if (kind == Inbound::Bye) return true;
        const bool ok = (kind != Inbound::Stats || bytes <= kMaxStatsBytes) && bucket(kind).take(nowUs);
        if (!ok) ++dropped_;
        return ok;
    }
    // Messages dropped since the last call.
    uint64_t takeDropped() { return std::exchange(dropped_, 0); }

private:
    TokenBucket& bucket(Inbound kind) {
        switch (kind) {
        case Inbound::Ping: return ping_;
        case Inbound::Report: return report_;
        case Inbound::Behind: return behind_;
        case Inbound::Wish: return wish_;
        case Inbound::Probe: return probe_;
        case Inbound::Room: return room_;
        default: return stats_;
        }
    }
    TokenBucket stats_{20, 40}, ping_{4, 12}, report_{2, 4}, behind_{1, 3}, wish_{1, 4}, probe_{1, 3}, room_{4, 8};
    uint64_t dropped_ = 0;
};

} // namespace veyra::guest
