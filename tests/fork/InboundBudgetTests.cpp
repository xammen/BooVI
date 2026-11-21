// What one viewer may send the host (InboundBudget.h): a page's own rhythm passes, a flood does not.
#include "veyra/guest/InboundBudget.h"
#include <cstdio>

using namespace veyra::guest;

static int failures = 0;
static void expect(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

int main() {
    const int64_t ms = 1000;
    {
        // The guest page for a minute: a ping every 500 ms, a report every 2 s, a wish and a link
        // test at the start, a "behind" every 2 s at worst.
        InboundBudget b;
        bool all = true;
        for (int i = 0; i < 8; ++i) all &= b.admit(Inbound::Stats, 1 + i * 60 * ms, 60) && b.admit(Inbound::Ping, 1 + i * 60 * ms);
        for (int64_t t = 1; t <= 60'000 * ms; t += 500 * ms) {
            all &= b.admit(Inbound::Stats, t, 300) && b.admit(Inbound::Ping, t);
            if (t % (2000 * ms) == 1) all &= b.admit(Inbound::Stats, t, 900) && b.admit(Inbound::Report, t) && b.admit(Inbound::Stats, t, 60) && b.admit(Inbound::Behind, t);
        }
        all &= b.admit(Inbound::Wish, 1) && b.admit(Inbound::Wish, 2) && b.admit(Inbound::Probe, 3) && b.admit(Inbound::Probe, 4);
        expect(all && b.takeDropped() == 0, "a page's own rhythm, its first clock-sync burst included, is never dropped");
    }
    {
        InboundBudget b;
        int passed = 0;
        for (int i = 0; i < 1000; ++i) passed += b.admit(Inbound::Report, 1 + i * ms);
        expect(passed >= 4 && passed <= 7 && b.takeDropped() == uint64_t(1000 - passed), "1000 reports in a second: a burst and the rate pass, the rest is counted");
        expect(b.takeDropped() == 0, "the count is taken once");
        expect(b.admit(Inbound::Report, 1 + 1000 * ms + 600 * ms), "the bucket refills");
    }
    {
        InboundBudget b;
        expect(!b.admit(Inbound::Stats, 1, InboundBudget::kMaxStatsBytes + 1) && b.admit(Inbound::Stats, 2, InboundBudget::kMaxStatsBytes),
               "a stats message over 4 KB is dropped, 4 KB passes");
    }
    {
        InboundBudget b;
        int passed = 0;
        for (int i = 0; i < 100; ++i) passed += b.admit(Inbound::Room, 1 + i * 10 * ms);
        expect(passed >= 8 && passed <= 12, "room commands: a burst of 8, then 4 a second");
        bool bye = true;
        for (int i = 0; i < 100; ++i) bye &= b.admit(Inbound::Bye, 1);
        expect(bye, "a goodbye always passes");
    }
    expect(inboundOf("link") == Inbound::Wish && inboundOf("probe-stop") == Inbound::Probe && inboundOf("x") == Inbound::Stats, "kinds");
    std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
