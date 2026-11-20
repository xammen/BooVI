#include "veyra/remoteplay/PadPresence.h"
#include <cstdio>

using veyra::remoteplay::PadPresence;
using State = PadPresence::State;

static int failures = 0;
static void expect(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

int main() {
    constexpr auto grace = PadPresence::kGraceMs;
    {
        PadPresence p;
        p.update(true, false, true, "DualSense Wireless Controller", 1000);
        expect(p.state() == State::Reading && !p.hint(), "pad there when the picture starts: read, no hint");
        expect(p.arrivals() == 0, "pad there when the picture starts: no toast");
        p.update(true, false, false, "", 5000);
        expect(p.state() == State::Waiting && !p.hint(), "pad lost: no hint during the grace");
        p.update(true, false, false, "", 5000 + grace - 1);
        expect(!p.hint(), "pad lost: still no hint just before the grace ends");
        p.update(true, false, false, "", 5000 + grace);
        expect(p.hint(), "pad lost for the grace: the hint shows (the PS5 may have taken it)");
        p.update(true, false, true, "DualSense Wireless Controller", 9000);
        expect(p.state() == State::Reading && !p.hint(), "pad back: the hint goes away by itself");
        expect(p.arrivals() == 1, "pad back: one toast");
        p.update(false, false, true, "DualSense Wireless Controller", 10000);
        expect(p.state() == State::Off, "stream ends: off");
    }
    {
        PadPresence p;
        p.update(true, false, false, "", 0);
        p.update(true, false, false, "", grace - 1);
        expect(p.state() == State::Waiting, "no pad at the start: waiting first");
        expect(p.update(true, false, false, "", grace) && p.hint(), "no pad at the start: the hint shows after the grace, and that is a change");
        expect(!p.update(true, false, false, "", grace + 16), "no pad, nothing new: no change reported");
        p.update(true, false, true, "Xbox Series X Controller", grace + 500);
        expect(!p.hint() && p.arrivals() == 1, "a pad plugged in mid-stream: hint gone, one toast");
    }
    {
        PadPresence p;
        p.update(true, false, false, "", 0);
        p.update(true, false, true, "DualSense Wireless Controller", PadPresence::kSettleMs - 1);
        expect(p.state() == State::Reading && p.arrivals() == 0, "the input thread's first poll lands just after the picture: no toast");
    }
    {
        PadPresence p;
        p.update(true, false, false, "", 0);
        p.update(true, false, false, "", grace);
        p.close();
        expect(p.state() == State::Quiet && !p.hint(), "hint closed: quiet");
        p.update(true, false, false, "", grace * 10);
        expect(!p.hint(), "hint closed: it does not come back while no pad is read");
        p.update(true, false, true, "DualSense Wireless Controller", grace * 10 + 1);
        expect(p.state() == State::Reading && p.arrivals() == 1, "hint closed, then a pad: read, with a toast");
        p.update(true, false, false, "", grace * 11);
        p.update(true, false, false, "", grace * 12);
        expect(p.hint(), "after a pad was read and lost, the hint can show again");
    }
    {
        PadPresence p;
        p.update(true, true, false, "", 0);
        p.update(true, true, false, "", grace * 5);
        expect(p.state() == State::Quiet && !p.hint(), "keyboard and mouse only: never a hint");
        p.update(true, false, false, "", grace * 6);
        expect(!p.hint(), "keyboard released: still calm, no hint");
    }
    {
        PadPresence p;
        p.update(true, false, false, "", 0);
        p.update(true, false, false, "", grace);
        p.update(true, true, false, "", grace + 1);
        expect(!p.hint(), "keyboard taken while the hint shows: the hint goes");
    }
    {
        PadPresence p;
        p.update(true, false, true, "Xbox 360 Controller", 0);
        p.update(true, false, true, "DualSense Wireless Controller", 4000);
        expect(p.state() == State::Reading && p.arrivals() == 1, "switch from a virtual pad to the DualSense: one toast naming it");
        p.update(true, false, true, "DualSense Wireless Controller", 4016);
        expect(p.arrivals() == 1, "the same pad, tick after tick: no more toasts");
    }
    {
        PadPresence p;
        p.update(true, false, false, "", 0);
        p.update(true, false, false, "", grace);
        p.update(false, false, false, "", grace + 1);
        expect(p.state() == State::Off && !p.hint(), "view-only or stream over: off, no hint");
        p.update(true, false, false, "", grace + 2);
        expect(!p.hint(), "a new stream starts its own grace");
        p.close();
        p.update(false, false, false, "", grace * 3);
        p.update(true, false, false, "", grace * 3 + 1);
        p.update(true, false, false, "", grace * 4 + 1);
        expect(p.hint(), "a hint closed in one stream shows again in the next");
    }
    {
        PadPresence p;
        p.update(true, false, true, "", 0);
        p.update(true, false, true, "", grace * 2);
        expect(p.state() == State::Reading && !p.hint(), "a pad SDL gives no name: still read, no hint");
    }
    std::printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
