#include "veyra/remoteplay/KbmMapper.h"
#include <cstdio>
#include <cstdlib>

using namespace veyra::remoteplay;

static int failures = 0;
static void expect(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

constexpr HostTime ms = 10000; // HostTime is 100 ns

// The right stick after `reads` 1 ms reads, each with `counts` of rightward movement.
static int steadyStick(int counts, KbmSettings settings = {}, int reads = 20) {
    KbmMapper m;
    m.setSettings(settings);
    ControllerState s;
    for (int t = 1; t <= reads; ++t) { m.mouseMove(counts, 0); s = m.sample(t * ms); }
    return s.rightX;
}

int main() {
    {
        KbmMapper m;
        auto s = m.sample(8 * ms);
        expect(s.inputActive && s.buttons == 0 && s.leftX == 0 && s.rightX == 0, "nothing held: neutral but active state");
    }
    {
        KbmMapper m;
        m.key(0, 0x11, false, true); // W / Z
        auto s = m.sample(8 * ms);
        expect(s.leftX == 0 && s.leftY == -32767, "forward key: left stick fully up");
        m.key(0, 0x20, false, true); // D
        s = m.sample(16 * ms);
        expect(std::abs(s.leftX - 23170) <= 1 && std::abs(s.leftY + 23170) <= 1, "diagonal stays on the unit circle");
        m.key(0, 0x11, false, false); m.key(0, 0x20, false, false);
        s = m.sample(24 * ms);
        expect(s.leftX == 0 && s.leftY == 0, "keys released: stick centered");
    }
    {
        KbmMapper m;
        m.key(0, 0x39, false, true);
        m.mouseButton(1, true);
        m.mouseButton(3, true);
        auto s = m.sample(8 * ms);
        expect((s.buttons & ControllerState::Cross) && s.r2 == 255 && s.l2 == 255, "space is cross, left click R2, right click L2");
        m.key(0, 0x48, true, true);
        s = m.sample(16 * ms);
        expect((s.buttons & ControllerState::Up) != 0, "extended arrow up is the d-pad");
        m.key(0, 0x48, false, false); // keypad 8 shares the scan code without the extended flag
        s = m.sample(24 * ms);
        expect((s.buttons & ControllerState::Up) != 0, "releasing keypad 8 does not release arrow up");
        m.key(0, 0x48, true, false);
        s = m.sample(32 * ms);
        expect((s.buttons & ControllerState::Up) == 0, "releasing arrow up releases the d-pad");
    }
    {
        // A 1000 Hz mouse at a constant 1000 counts/s: one report of 1 count per 1 ms read.
        KbmMapper m;
        const HostTime start = 1000 * ms;
        auto s = m.sample(start);
        expect(s.rightX == 0, "no movement: stick centered");
        for (int t = 1; t <= 5; ++t) { m.mouseMove(1, 0); m.sample(start + t * ms); }   // the mouse's rate is learned
        for (int t = 6; t <= 100; ++t) m.sample(start + t * ms);
        m.mouseMove(1, 0);
        s = m.sample(start + 101 * ms);
        expect(s.rightX > 0, "the first read with movement moves the stick");
        int lowest = 32767, highest = 0;
        for (int t = 102; t <= 200; ++t) {
            m.mouseMove(1, 0);
            s = m.sample(start + t * ms);
            lowest = std::min<int>(lowest, s.rightX);
            highest = std::max<int>(highest, s.rightX);
        }
        expect(lowest > 0 && highest == lowest, "constant speed: the stick holds one value");
        expect(std::abs(s.rightX - steadyStick(1)) <= 1, "and that value is the same however long it has moved");
        int centeredAfter = -1;
        for (int t = 201; t <= 220 && centeredAfter < 0; ++t)
            if (m.sample(start + t * ms).rightX == 0) centeredAfter = t - 200;
        expect(centeredAfter > 0 && centeredAfter <= 4, "the mouse stops: stick centered within 4 ms");
    }
    {
        KbmSettings slowest;
        slowest.speed = 0;   // full tilt at 3000 counts/s
        const int slow = steadyStick(1, slowest), fast = steadyStick(2, slowest), huge = steadyStick(100);
        expect(slow > 0 && fast > slow && fast < 32767, "faster mouse, further right stick");
        expect(slow >= int(KbmSettings{}.deadzone * 32767), "the smallest movement jumps over the game deadzone");
        expect(huge == 32767, "very fast movement saturates at full tilt");
    }
    {
        // A flick: one large report after rest is full tilt on that same read.
        KbmMapper m;
        for (int t = 1; t <= 5; ++t) { m.mouseMove(1, 0); m.sample(t * ms); }
        m.sample(100 * ms);
        m.mouseMove(60, 0);
        expect(m.sample(101 * ms).rightX == 32767, "a flick after rest is full tilt at once");
    }
    {
        // Fine aim right after a quick turn: the mapper has learned a 1 ms report interval, then
        // single counts come 50 ms apart (20 counts/s). Each must read as slow movement, not as
        // one count per millisecond (full tilt), and still leave the deadzone long enough to be seen.
        KbmMapper m;
        HostTime t = 0;
        for (int i = 0; i < 100; ++i) { m.mouseMove(1, 0); m.sample(t += ms); }
        t += 300 * ms;
        m.sample(t);
        int highest = 0, heldReads = 0;
        for (int count = 0; count < 10; ++count) {
            m.mouseMove(1, 0);
            for (int i = 0; i < 50; ++i) {
                const int x = m.sample(t += ms).rightX;
                highest = std::max(highest, x);
                if (count == 0 && x > 0) ++heldReads;
            }
        }
        expect(highest > int(KbmSettings{}.deadzone * 32767) && highest < 32767 * 2 / 5, "slow counts after a quick turn: past the deadzone, far from full tilt");
        expect(heldReads >= 17, "a lone count holds the stick for at least one 60 Hz frame");
    }
    {
        // Slower never pushes further: the highest stick at 5 to 40 counts/s stays below the
        // steady stick at 60 counts/s (a 1000 Hz mouse, single counts).
        auto peak = [](int gapMs) {
            KbmMapper m;
            int highest = 0;
            HostTime t = 0;
            for (int i = 0; i < 1000; ++i) {
                if (i % gapMs == 0) m.mouseMove(1, 0);
                highest = std::max<int>(highest, m.sample(t += ms).rightX);
            }
            return highest;
        };
        const int at60 = peak(17);
        bool slower = true;
        for (const int gap : {25, 33, 50, 100, 200}) slower = slower && peak(gap) <= at60;
        expect(slower, "slow aim: a slower mouse never deflects the stick further than a faster one");
    }
    {
        // Precision: below the knee the travel past the deadzone grows in proportion to the speed;
        // above it, and with precision off, the curve is the old one.
        KbmSettings off, on;
        off.precision = 0;
        const double knee = on.precisionKnee();
        const int dz = int(on.deadzone * 32767);
        // Counts per 1 ms read at a 1000 Hz mouse: n counts each read is n * 1000 counts/s.
        expect(steadyStick(1, on) == steadyStick(1, off), "precision: no change above the knee (1000 counts/s)");
        KbmMapper slow, slowOff;
        slow.setSettings(on);
        slowOff.setSettings(off);
        int a = 0, b = 0;
        for (int t = 1; t <= 200; ++t) {
            if (t % 20 == 0) { slow.mouseMove(1, 0); slowOff.mouseMove(1, 0); }   // 50 counts/s, below the knee
            a = std::max<int>(a, slow.sample(t * ms).rightX);
            b = std::max<int>(b, slowOff.sample(t * ms).rightX);
        }
        expect(knee > 50 && a > dz && a < b, "precision: a gentler start just past the deadzone below the knee");
    }
    {
        // Slow and uneven: reports 1 to 3 ms apart never read as a stop while they come.
        KbmMapper m;
        const int gaps[] = {1, 3, 2, 1, 3, 3, 1, 2, 3, 1, 2, 3, 3, 1};
        HostTime t = 10 * ms;
        m.mouseMove(1, 0);
        m.sample(t);
        bool centered = false;
        for (const int gap : gaps) {
            for (int i = 1; i < gap; ++i) centered = centered || m.sample(t + i * ms).rightX == 0;
            t += gap * ms;
            m.mouseMove(1, 0);
            centered = centered || m.sample(t).rightX == 0;
        }
        expect(!centered, "uneven slow movement never drops the stick to center");
    }
    {
        // A 125 Hz mouse: one report every 8 ms, read every 1 ms.
        KbmMapper m;
        bool centered = false;
        int first = 0;
        for (int t = 8; t <= 200; ++t) {
            if (t % 8 == 0) m.mouseMove(8, 0);
            const auto s = m.sample(t * ms);
            if (t == 8) first = s.rightX;
            if (t > 8) centered = centered || s.rightX == 0;
        }
        expect(first > 0, "125 Hz: the first report moves the stick at once");
        expect(!centered, "125 Hz: the stick holds between reports");
        int centeredAfter = -1;
        for (int t = 201; t <= 240 && centeredAfter < 0; ++t)
            if (m.sample(t * ms).rightX == 0) centeredAfter = t - 200;
        expect(centeredAfter > 0 && centeredAfter <= 18, "125 Hz: centered within two report intervals of the stop");
    }
    {
        KbmMapper m;
        m.mouseMove(0, 2);
        m.sample(1 * ms);
        m.mouseMove(0, 2);
        auto s = m.sample(2 * ms);
        expect(s.rightY > 0 && s.rightX == 0, "mouse down is stick down");
    }
    {
        // Camera speed: higher is more stick for the same mouse; the default is well above the
        // old gain (full tilt at 1840 counts/s).
        KbmSettings low, high;
        low.speed = 0.2f;
        high.speed = 0.8f;
        expect(steadyStick(1, high) > steadyStick(1, low), "camera speed: higher is more stick");
        expect(KbmSettings{}.fullTilt() < 1840.0 / 2, "default camera speed more than twice the old gain");
        expect(std::abs(KbmSettings{}.fullTilt() - 775) < 1, "default full tilt at 775 counts/s");
    }
    {
        KbmMapper m;
        m.scroll(120, false);
        auto s = m.sample(8 * ms);
        expect((s.buttons & ControllerState::Up) != 0, "wheel up presses d-pad up");
        s = m.sample(15 * ms);
        expect((s.buttons & ControllerState::Up) != 0, "and holds it 8 ms");
        s = m.sample(16 * ms);
        expect((s.buttons & ControllerState::Up) == 0, "then releases it");
    }
    {
        KbmMapper m;
        KbmSettings gyro;
        gyro.mouse = KbmSettings::Mouse::Gyro;
        m.setSettings(gyro);
        m.mouseMove(3, 0);
        m.sample(1 * ms);
        m.mouseMove(3, 0);
        auto s = m.sample(2 * ms);
        expect(s.motionValid && s.motionSampleCount == 1 && s.gyro[1] < 0 && s.rightX == 0, "gyro mode: mouse turns the gyro, not the stick");
    }
    {
        KbmMapper m;
        m.key(0, 0x11, false, true);
        m.mouseButton(1, true);
        m.mouseMove(50, 0);
        m.sample(1 * ms);
        m.releaseAll();
        auto s = m.sample(2 * ms);
        expect(s.buttons == 0 && s.leftY == 0 && s.r2 == 0 && s.rightX == 0, "releaseAll lets go of everything");
    }
    {
        // The overlay lists what the mapper really does.
        const auto bound = KbmMapper().bindings();
        auto find = [&](uint32_t scan, bool extended, KbmMouse mouse) -> const KbmBound* {
            for (const auto& b : bound) if (b.scan == scan && b.extended == extended && b.mouse == mouse) return &b;
            return nullptr;
        };
        const auto* forward = find(0x11, false, {});
        const auto* up = find(0x48, true, {});
        const auto* click = find(0, false, KbmMouse::Left);
        const auto* wheel = find(0, false, KbmMouse::WheelDown);
        expect(bound.size() == 25, "bindings: 18 keys and 7 mouse inputs");
        expect(forward && forward->binding.target == KbmTarget::LeftUp, "bindings: W / Z is left stick up");
        expect(up && up->binding.button == ControllerState::Up, "bindings: extended arrow up is the d-pad");
        expect(click && click->binding.target == KbmTarget::R2, "bindings: left click is R2");
        expect(wheel && wheel->binding.button == ControllerState::Down, "bindings: wheel down is d-pad down");
        KbmMapper rebound;
        rebound.bindKey(0x12, false, {KbmTarget::L2});
        bool l2 = false;
        for (const auto& b : rebound.bindings()) l2 = l2 || (b.scan == 0x12 && b.binding.target == KbmTarget::L2);
        expect(l2, "bindings: a rebound key shows its new control");
    }
    {
        KbmMapper m;
        veyra::moonlight::InputRouter router(m, true);
        auto key = [&](USHORT vk, USHORT make, USHORT flags) {
            RAWINPUT in{};
            in.header.dwType = RIM_TYPEKEYBOARD;
            in.data.keyboard.VKey = vk; in.data.keyboard.MakeCode = make; in.data.keyboard.Flags = flags;
            router.raw(in);
        };
        auto mouse = [&](LONG dx, USHORT buttons) {
            RAWINPUT in{};
            in.header.dwType = RIM_TYPEMOUSE;
            in.data.mouse.lLastX = dx; in.data.mouse.usButtonFlags = buttons;
            router.raw(in);
        };
        m.sample(0);
        key('W', 0x11, RI_KEY_MAKE);
        key(VK_UP, 0x48, RI_KEY_E0);
        mouse(40, RI_MOUSE_LEFT_BUTTON_DOWN);
        auto s = m.sample(8 * ms);
        expect(s.leftY == -32767 && (s.buttons & ControllerState::Up) && s.rightX > 0 && s.r2 == 255,
               "raw input: scan code key, extended arrow, mouse movement and button");
        key('W', 0x11, RI_KEY_BREAK);
        mouse(0, RI_MOUSE_LEFT_BUTTON_UP);
        s = m.sample(16 * ms);
        expect(s.leftY == 0 && s.r2 == 0, "raw input: releases");
        key(VK_CONTROL, 0x1D, RI_KEY_MAKE); key(VK_MENU, 0x38, RI_KEY_MAKE); key(VK_SHIFT, 0x2A, RI_KEY_MAKE);
        key('Z', 0x2C, RI_KEY_MAKE);
        expect((m.sample(24 * ms).buttons & ControllerState::R1) == 0, "raw input: Ctrl+Alt+Shift+Z stays a hotkey, not R1");
        // The capture ends before the Z release reaches this reader; the next capture starts clean.
        router.reset();
        m.releaseAll();
        key(VK_CONTROL, 0x1D, RI_KEY_BREAK); key(VK_MENU, 0x38, RI_KEY_BREAK); key(VK_SHIFT, 0x2A, RI_KEY_BREAK);
        key('Z', 0x2C, RI_KEY_MAKE);
        expect((m.sample(32 * ms).buttons & ControllerState::R1) != 0, "after a re-capture the hotkey's letter is a key again");
        key('Z', 0x2C, RI_KEY_BREAK);
        expect((m.sample(40 * ms).buttons & ControllerState::R1) == 0, "and its release lets go");
    }
    {
        // Ctrl+M shows the bindings: on the PS5 router it never reaches the mapper.
        struct Keys final : veyra::moonlight::InputSink {
            int m = 0;
            void key(uint32_t vk, uint32_t, bool, bool) override { if (vk == 'M') ++m; }
            void mouseMove(int, int) override {}
            void mouseButton(int, bool) override {}
            void scroll(int, bool) override {}
        } sink, plain;
        veyra::moonlight::InputRouter ps5(sink, true), pc(plain);
        auto key = [](veyra::moonlight::InputRouter& r, USHORT vk, USHORT make, USHORT flags) {
            RAWINPUT in{};
            in.header.dwType = RIM_TYPEKEYBOARD;
            in.data.keyboard.VKey = vk; in.data.keyboard.MakeCode = make; in.data.keyboard.Flags = flags;
            r.raw(in);
        };
        key(ps5, VK_CONTROL, 0x1D, RI_KEY_MAKE);
        key(ps5, 'M', 0x32, RI_KEY_MAKE);
        expect(ps5.takeReserved() == veyra::moonlight::Reserved::ShowKeys, "Ctrl+M: the PS5 router reports the overlay hotkey");
        key(ps5, 'M', 0x32, RI_KEY_MAKE);   // auto-repeat
        expect(ps5.takeReserved() == veyra::moonlight::Reserved::None, "Ctrl+M held: one toggle, not one per repeat");
        key(ps5, 'M', 0x32, RI_KEY_BREAK);
        expect(sink.m == 0, "Ctrl+M: neither the press, the repeat nor the release reaches the PS5");
        key(ps5, VK_CONTROL, 0x1D, RI_KEY_BREAK);
        key(ps5, 'M', 0x32, RI_KEY_MAKE);
        expect(sink.m == 1, "M alone is an ordinary key");
        key(pc, VK_CONTROL, 0x1D, RI_KEY_MAKE);
        key(pc, 'M', 0x32, RI_KEY_MAKE);
        expect(plain.m == 1 && pc.takeReserved() == veyra::moonlight::Reserved::None, "Ctrl+M still reaches a PC host (Moonlight router)");
    }
    {
        ControllerState pad;
        pad.inputActive = true;
        pad.buttons = ControllerState::Circle;
        pad.leftX = 30000;
        pad.r2 = 40;
        ControllerState kbm;
        kbm.inputActive = true;
        kbm.buttons = ControllerState::Cross;
        kbm.leftX = 1000;
        kbm.rightX = 20000;
        kbm.r2 = 255;
        auto s = mergeInputs(pad, kbm);
        expect(s.buttons == (ControllerState::Circle | ControllerState::Cross), "merge: buttons from both");
        expect(s.leftX == 30000 && s.rightX == 20000 && s.r2 == 255, "merge: each stick and trigger takes the stronger source");
        ControllerState idle;
        expect(mergeInputs(idle, kbm).buttons == ControllerState::Cross, "merge: no pad, keyboard state alone");
    }
    std::printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
