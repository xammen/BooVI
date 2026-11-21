#include "veyra/guest/GuestQuality.h"
#include <cstdio>

using namespace veyra::guest;

static int failures = 0;
static void expect(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

int main() {
    const Limit all{Feed::Smooth120, Size::Native, 80000};
    const int64_t s = 1000000;

    {
        const auto rungs = ladder(all);
        expect(rungs.size() == 4, "the full ladder: real, 60, 120 at 1080p, then 120 at native");
        expect(rungs.front().feed == Feed::Real && rungs.back().feed == Feed::Smooth120 && rungs.back().size == Size::Native, "least to most");
        expect(ladder({Feed::Real, Size::P720, 20000}).size() == 1, "a host that allows only real 720p has one rung");
        expect(ladder({Feed::Smooth60, Size::Native, 50000}).back().size == Size::Native, "native comes at the host's feed");
    }
    {
        RateControl c; c.setLimit(all);
        expect(c.current().feed == Feed::Real && c.current().size == Size::P1080, "automatique starts on real pictures before any test");
        c.linkTested(85000, 0);
        expect(c.current().feed == Feed::Smooth120 && c.current().size == Size::Native, "85 Mb/s: fluide 120 at native size (52 <= 63.75)");
        expect(c.current().kbps <= 80000 && c.current().kbps <= 63750, "within the host's upload and the margin");
    }
    {
        RateControl c; c.setLimit(all);
        c.linkTested(40000, 0);
        expect(c.current().feed == Feed::Smooth60, "40 Mb/s with a 25 % margin: 30 Mb/s usable, fluide 60");
    }
    {
        RateControl c; c.setLimit(all); c.linkTested(100000, 0);
        const auto top = c.current();
        int64_t t = 0;
        for (int i = 0; i < 6; ++i) { t += 2 * s; c.report(0.05, 10, t); }
        expect(c.current().feed < top.feed || c.current().size < top.size, "sustained loss steps the mode down");
        const auto low = c.current();
        for (int i = 0; i < 6; ++i) { t += 2 * s; c.report(0, 10, t); }
        expect(c.current().feed == low.feed && c.current().size == low.size, "no climb within 30 s of a step down");
        for (int i = 0; i < 120; ++i) { t += 2 * s; c.report(0, 10, t); }
        expect(c.current().feed == top.feed && c.current().size == top.size, "a clean link climbs back to the tested mode");
    }
    {
        RateControl c; c.setLimit(all); c.linkTested(100000, 0);
        int64_t t = 0;
        c.report(0, 5, t += 2 * s);
        const uint32_t before = c.current().kbps;
        c.report(0, 45, t += 2 * s);
        expect(c.current().kbps < before, "network delay rising 40 ms above its floor backs off before any loss");
    }
    {
        RateControl c; c.setLimit(all);
        c.setWish({false, Feed::Smooth120, Size::Native, 100000}, 0);
        expect(c.current().kbps == maxKbps(Feed::Smooth120, Size::Native), "manuel à fond: the mode's ceiling, under the host's upload");
        c.setLimit({Feed::Smooth60, Size::P1080, 30000});
        expect(c.current().feed == Feed::Smooth60 && c.current().size == Size::P1080 && c.current().kbps == 30000, "the host's maximum bounds the guest's choice");
        c.report(0.05, -1, 2 * s);
        expect(c.current().feed == Feed::Smooth60 && c.current().kbps < 30000, "manuel keeps its mode, loss still lowers the bitrate");
    }
    {
        RateControl c; c.setLimit({Feed::Smooth120, Size::P1080, 50000});
        c.linkTested(300000, 0);
        expect(c.current().size == Size::P1080 && c.current().feed == Feed::Smooth120, "a host limited to 1080p never sends native");
    }
    {
        // The friend's evening: a 1.4 Gb/s link test, a software decoder.
        RateControl c; c.setLimit({Feed::Smooth120, Size::Native, 100000}); c.linkTested(1400000, 0);
        expect(c.current().feed == Feed::Smooth120 && c.current().size == Size::Native && c.current().kbps <= 52000, "a fast link: fluide 120 native, at its own rate");
        const Feed feeds[] = {Feed::Smooth60, Feed::Real, Feed::Real, Feed::Real};
        const Size sizes[] = {Size::Native, Size::Native, Size::P1080, Size::P720};
        bool order = true;
        for (int i = 0; i < 4; ++i) {
            order = order && c.decoderBehind() && c.current().feed == feeds[i] && c.current().size == sizes[i] && c.current().kbps <= defaultKbps(feeds[i], sizes[i]);
        }
        expect(order, "a decoder behind: fluide 120 -> 60 -> real, then native -> 1080p -> 720p, each at its own rate");
        expect(!c.decoderBehind(), "nothing left below real 720p");
        for (int i = 0; i < 20; ++i) c.report(0, 10, 100 * s + i * 2 * s);
        expect(c.current().feed == Feed::Real && c.current().size == Size::P720, "a clean link does not climb past what the decoder showed");
    }
    {
        RateControl c; c.setLimit({Feed::Smooth120, Size::Native, 100000});
        c.setWish({false, Feed::Smooth60, Size::P1080, 100000}, 0);
        expect(c.current().kbps == maxKbps(Feed::Smooth60, Size::P1080) && c.current().kbps == 30000, "manual 1080p 60 at 100 Mb/s is held to 30 Mb/s");
        expect(!c.decoderBehind() && c.current().feed == Feed::Smooth60, "a manual choice is not stepped down");
    }
    expect(parseFeed("60", Feed::Real) == Feed::Smooth60 && parseFeed("x", Feed::Real) == Feed::Real, "feed names");
    expect(parseSize("native", Size::P720) == Size::Native, "size names");
    std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
