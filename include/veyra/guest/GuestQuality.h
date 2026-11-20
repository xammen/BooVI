#pragma once
// Fork, partage: what the guest receives. The host sets the most it allows (feed, size, its
// upload); the guest picks within it, by hand or "automatique". Automatique starts from the
// page's link test and follows the page's reports: loss and a rising network delay lower the
// bitrate, then the rung (fluide 120 -> 60 -> real pictures, native size -> 1080p); a clean link
// climbs back up to what the test measured.
// Pure: tested in tests/fork/GuestQualityTests.cpp.
#include <algorithm>
#include <cstdint>
#include <deque>
#include <string_view>
#include <vector>

namespace veyra::guest {

// Ordered: a larger value is more.
enum class Feed : uint8_t { Real, Smooth60, Smooth120 };
enum class Size : uint8_t { P720, P1080, Native };

struct Quality {
    Feed feed = Feed::Real;
    Size size = Size::P1080;
    uint32_t kbps = 20000;
    bool operator==(const Quality&) const = default;
};

// The host's maximum: feed, size and its upload for the guest.
struct Limit {
    Feed feed = Feed::Real;
    Size size = Size::P1080;
    uint32_t kbps = 50000;
};

// The guest's choice. Manual: this feed, size and bitrate cap (within the host's limit).
struct Wish {
    bool automatic = true;
    Feed feed = Feed::Smooth120;
    Size size = Size::Native;
    uint32_t kbps = 100000;
};

constexpr uint32_t kMinGuestKbps = 4000;
constexpr double kLinkMargin = 0.75;   // a mode takes at most 75 % of the measured link

// Pictures a second the encoder is set for. Real pictures are capped at 60, as before.
constexpr uint32_t feedFps(Feed f) { return f == Feed::Smooth120 ? 120 : 60; }
// The stream height asked of the tap; Native keeps the tap's own height (the host's
// super-resolved output), capped at 2160.
constexpr uint32_t sizeHeight(Size s) { return s == Size::P720 ? 720 : s == Size::P1080 ? 1080 : 2160; }

// What each mode needs for a clean picture with NVENC's low-latency CBR, kb/s.
constexpr uint32_t defaultKbps(Feed f, Size s) {
    constexpr uint32_t table[3][3] = {{10000, 14000, 20000},    // 720p: real, 60, 120
                                      {20000, 25000, 35000},    // 1080p
                                      {30000, 38000, 52000}};   // native (1440p)
    return table[size_t(s)][size_t(f)];
}

constexpr std::string_view feedName(Feed f) { return f == Feed::Real ? "real" : f == Feed::Smooth60 ? "60" : "120"; }
constexpr std::string_view sizeName(Size s) { return s == Size::P720 ? "720" : s == Size::P1080 ? "1080" : "native"; }
inline Feed parseFeed(std::string_view v, Feed fallback) {
    return v == "real" ? Feed::Real : v == "60" ? Feed::Smooth60 : v == "120" ? Feed::Smooth120 : fallback;
}
inline Size parseSize(std::string_view v, Size fallback) {
    return v == "720" ? Size::P720 : v == "1080" ? Size::P1080 : v == "native" ? Size::Native : fallback;
}

// Automatique's rungs, from the least to the most: each feed the host allows at 1080p (or
// its smaller size), then the host's feed at native size when it allows that.
inline std::vector<Quality> ladder(const Limit& limit) {
    const Size base = std::min(limit.size, Size::P1080);
    std::vector<Quality> out;
    for (int f = 0; f <= int(limit.feed); ++f) out.push_back({Feed(f), base, defaultKbps(Feed(f), base)});
    if (limit.size == Size::Native) out.push_back({limit.feed, Size::Native, defaultKbps(limit.feed, Size::Native)});
    return out;
}

// The best rung that fits a measured link with its margin; the least without a measurement.
inline Quality bestFor(const Limit& limit, uint32_t linkKbps) {
    const auto rungs = ladder(limit);
    size_t i = 0;
    while (i + 1 < rungs.size() && uint32_t(linkKbps * kLinkMargin) >= rungs[i + 1].kbps) ++i;
    return rungs[i];
}

class RateControl {
public:
    void setLimit(const Limit& limit) { limit_ = limit; if (wish_.automatic) rung_ = std::min(rung_, ladder(limit_).size() - 1); clampKbps(); }
    void setWish(const Wish& wish, int64_t nowUs) {
        const bool wasAuto = wish_.automatic;
        wish_ = wish;
        if (wish_.automatic && !wasAuto) pickFromLink(nowUs);
        kbps_ = ceiling();
    }
    // The page's link test: the throughput it sustained, kb/s.
    void linkTested(uint32_t kbps, int64_t nowUs) {
        tested_ = link_ = kbps;
        pickFromLink(nowUs);
        kbps_ = ceiling();
    }
    // The page's report every 2 s: loss over the window, and the median network time of its
    // pictures (sent -> received, clock-synced), whose rise above its recent floor is queueing.
    void report(double loss, double netMs, int64_t nowUs) {
        double queue = 0;
        if (netMs >= 0) {
            nets_.push_back(netMs);
            if (nets_.size() > 15) nets_.pop_front();   // 30 s
            queue = netMs - *std::min_element(nets_.begin(), nets_.end());
        }
        queueMs_ = queue;
        const bool severe = loss > 0.03 || queue > 60;
        const bool congested = severe || loss > 0.01 || queue > 25;
        if (congested) {
            // The link took less than what was sent: that is the new estimate.
            if (link_) link_ = std::min(link_, uint32_t(kbps_ / kLinkMargin * (severe ? 0.75 : 0.9)));
            else link_ = uint32_t(kbps_ * (severe ? 0.75 : 0.9) / kLinkMargin);
            kbps_ = std::max(kMinGuestKbps, uint32_t(kbps_ * (severe ? 0.75 : 0.9)));
            clean_ = 0;
            congestedAtUs_ = nowUs;
            if (wish_.automatic) {
                const auto rungs = ladder(limit_);
                // Starved: this rung needs more than the link gives, take the one below.
                while (rung_ > 0 && kbps_ < rungs[rung_].kbps * 0.7) { --rung_; downAtUs_ = nowUs; }
                kbps_ = std::min(kbps_, ceiling());
            }
            return;
        }
        if (loss > 0.002 || queue > 10 || ++clean_ < 3) return;
        clean_ = 0;
        // Right after a loss the link is where it was: climbing back at once only finds it again.
        if (nowUs - congestedAtUs_ < kHoldUs) return;
        if (link_ && tested_) link_ = std::min(tested_, uint32_t(link_ * 1.1));
        if (wish_.automatic) {
            const auto rungs = ladder(limit_);
            if (rung_ + 1 < rungs.size() && nowUs - downAtUs_ > kHoldUs && usable() >= rungs[rung_ + 1].kbps) ++rung_;
        }
        kbps_ = std::min(ceiling(), uint32_t(kbps_ * 1.1));
    }

    Quality current() const {
        if (wish_.automatic) { auto q = ladder(limit_)[rung_]; q.kbps = kbps_; return q; }
        return {std::min(wish_.feed, limit_.feed), std::min(wish_.size, limit_.size), kbps_};
    }
    bool automatic() const { return wish_.automatic; }
    const Wish& wish() const { return wish_; }
    const Limit& limit() const { return limit_; }
    uint32_t linkKbps() const { return link_; }
    uint32_t testedKbps() const { return tested_; }
    double queueMs() const { return queueMs_; }

private:
    static constexpr int64_t kHoldUs = 30000000;   // after a loss or a step down, 30 s before climbing again
    uint32_t usable() const { return link_ ? uint32_t(link_ * kLinkMargin) : 0; }
    // The most the encoder gets in the current mode.
    uint32_t ceiling() const {
        uint32_t c = limit_.kbps;
        if (wish_.automatic) {
            c = std::min(c, ladder(limit_)[rung_].kbps);
            if (link_) c = std::min(c, usable());
        } else c = std::min(c, wish_.kbps);
        return std::max(kMinGuestKbps, c);
    }
    void clampKbps() { kbps_ = std::min(kbps_, ceiling()); }
    void pickFromLink(int64_t nowUs) {
        const auto rungs = ladder(limit_);
        const Quality best = bestFor(limit_, link_);
        rung_ = 0;
        while (rung_ + 1 < rungs.size() && !(rungs[rung_].feed == best.feed && rungs[rung_].size == best.size)) ++rung_;
        downAtUs_ = nowUs - kHoldUs;
        nets_.clear();
    }

    Limit limit_;
    Wish wish_;
    size_t rung_ = 0;
    uint32_t kbps_ = defaultKbps(Feed::Real, Size::P1080);
    uint32_t link_ = 0, tested_ = 0;
    int clean_ = 0;
    int64_t downAtUs_ = 0, congestedAtUs_ = INT64_MIN / 2;
    double queueMs_ = 0;
    std::deque<double> nets_;
};

} // namespace veyra::guest
