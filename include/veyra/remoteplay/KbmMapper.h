#pragma once
// Keyboard and mouse to a DualSense state for PS5 Remote Play. The PS5 only accepts a
// controller over Remote Play, so the mouse becomes the right stick (or the gyro) and keys
// become buttons. Keys are bound by scan code, so the same physical keys work on AZERTY
// and QWERTY. Fed by moonlight::InputRouter from raw input on the PS5 input thread
// (RemotePlayInput.h), which also reads sample() every 1 ms.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>
#include "veyra/moonlight/InputRouter.h"
#include "veyra/remoteplay/Types.h"

namespace veyra::remoteplay {

enum class KbmTarget : uint8_t { None, Button, LeftUp, LeftDown, LeftLeft, LeftRight, L2, R2 };

struct KbmBinding {
    KbmTarget target = KbmTarget::None;
    uint32_t button = 0; // ControllerState::Button bit when target == Button
};

// Mouse inputs that can be bound: the five buttons, then the wheel's two directions.
enum class KbmMouse : uint8_t { Left = 1, Middle, Right, X1, X2, WheelUp, WheelDown };

// One bound input, for the bindings overlay: a key (scan code set 1 plus the extended flag)
// or a mouse input.
struct KbmBound {
    uint32_t scan = 0;
    bool extended = false;
    KbmMouse mouse{};   // 0 for a key
    KbmBinding binding;
};

struct KbmSettings {
    enum class Mouse : uint8_t { RightStick, Gyro };
    Mouse mouse = Mouse::RightStick;
    // Camera speed, 0..1: the mouse speed that gives full tilt, from 3000 counts/s down to
    // 200, on a log scale. 0.5 is 775 counts/s, 2.4 times the gain of the old default.
    float speed = 0.5f;
    float curve = 0.8f;        // 0..1, 0 = slow start, 1 = fast start (exponent 1.6 - 1.2 c)
    float deadzone = 0.24f;    // the game's inner stick deadzone to jump over, 0..0.4
    // Fine aim, 0..1: below a knee speed (a quarter of full tilt at 1) the stick's travel past the
    // deadzone grows in proportion to the mouse speed, so each count turns the camera by the same
    // small amount however slowly the mouse moves. Above the knee the curve is unchanged. 0 = off.
    float precision = 0.5f;
    double fullTilt() const { return 3000.0 * std::pow(200.0 / 3000.0, double(std::clamp(speed, 0.f, 1.f))); }
    double precisionKnee() const { return fullTilt() * 0.25 * double(std::clamp(precision, 0.f, 1.f)); }
    // Gyro mode: radians per count, the same speed scale (3 rad/s at the full-tilt speed).
    double gyroPerCount() const { return 3.0 / fullTilt(); }
};

class KbmMapper final : public moonlight::InputSink {
public:
    KbmMapper() { bindDefaults(); }

    void setSettings(const KbmSettings& s) { settings_ = s; }
    const KbmSettings& settings() const { return settings_; }

    // Scan code (set 1) plus the extended flag, as WM_KEYDOWN reports them.
    void bindKey(uint32_t scan, bool extended, KbmBinding b) { keys_[slot(scan, extended)] = b; }
    void bindMouse(KbmMouse input, KbmBinding b) { mouseBinds_[size_t(input)] = b; }

    // Every bound input: keys in scan code order, then the mouse.
    std::vector<KbmBound> bindings() const {
        std::vector<KbmBound> out;
        for (size_t i = 0; i < keys_.size(); ++i)
            if (keys_[i].target != KbmTarget::None) out.push_back({uint32_t(i & 0xFF), (i & 0x100) != 0, {}, keys_[i]});
        for (size_t i = 1; i < mouseBinds_.size(); ++i)
            if (mouseBinds_[i].target != KbmTarget::None) out.push_back({0, false, KbmMouse(i), mouseBinds_[i]});
        return out;
    }

    void key(uint32_t, uint32_t scan, bool extended, bool down) override { held_[slot(scan, extended)] = down; }
    void mouseMove(int dx, int dy) override { dx_ += dx; dy_ += dy; }
    void mouseButton(int button, bool down) override { if (button >= 1 && button <= 5) mouseHeld_[size_t(button)] = down; }
    void scroll(int delta, bool horizontal) override {
        if (horizontal || delta == 0) return;
        wheel_ = delta > 0 ? KbmMouse::WheelUp : KbmMouse::WheelDown;
        wheelUntil_ = 0;
    }

    // Everything released (capture ended or focus lost).
    void releaseAll() { held_ = {}; mouseHeld_ = {}; dx_ = dy_ = 0; wheel_ = {}; reports_ = {}; reportCount_ = 0; }

    // Mouse speed is measured over the last kSpan of movement, between the instants reports
    // arrived, so a 1000 Hz mouse moves the stick on the next read. The old fixed 8 ms windows
    // held each reading for a whole window: about 8 ms of added latency on average.
    static constexpr HostTime kTick = 10000;   // the input thread's period, 1 ms in 100 ns
    static constexpr HostTime kSpan = 40000;   // 4 ms
    static constexpr HostTime kGap = 250000;   // longer between two reports: the mouse had stopped
    static constexpr HostTime kWheelHold = 80000; // a wheel notch holds its button 8 ms

    // The controller state for this instant: keys and buttons as held now, the right stick
    // (or the gyro) from the mouse speed.
    ControllerState sample(HostTime now) {
        if (dx_ || dy_) {
            reportAt_ = (reportAt_ + 1) % reports_.size();
            reports_[reportAt_] = {now, dx_, dy_};
            reportCount_ = std::min(reportCount_ + 1, reports_.size());
            dx_ = dy_ = 0;
        }
        ControllerState s = mouseState(now);
        s.inputActive = true;
        int up = 0, down = 0, left = 0, right = 0;
        auto apply = [&](const KbmBinding& b) {
            switch (b.target) {
            case KbmTarget::Button: s.buttons |= b.button; break;
            case KbmTarget::LeftUp: up = 1; break;
            case KbmTarget::LeftDown: down = 1; break;
            case KbmTarget::LeftLeft: left = 1; break;
            case KbmTarget::LeftRight: right = 1; break;
            case KbmTarget::L2: s.l2 = 255; break;
            case KbmTarget::R2: s.r2 = 255; break;
            case KbmTarget::None: break;
            }
        };
        for (size_t i = 0; i < held_.size(); ++i) if (held_[i]) apply(keys_[i]);
        for (size_t i = 1; i < mouseHeld_.size(); ++i) if (mouseHeld_[i]) apply(mouseBinds_[i]);
        if (wheel_ != KbmMouse{}) {
            if (!wheelUntil_) wheelUntil_ = now + kWheelHold;
            if (now < wheelUntil_) apply(mouseBinds_[size_t(wheel_)]);
            else wheel_ = {};
        }

        const float lx = float(right - left), ly = float(down - up);
        const float len = std::sqrt(lx * lx + ly * ly);
        if (len > 0) { s.leftX = toAxis(lx / len); s.leftY = toAxis(ly / len); }
        return s;
    }

private:
    struct Report { HostTime t = 0; long long dx = 0, dy = 0; };

    const Report& report(size_t age) const { return reports_[(reportAt_ + reports_.size() - age) % reports_.size()]; }

    // How long the movement in report `age` (0 = newest) took: since the report before it. After
    // a pause, a report of several counts came within one report interval (a flick). A single
    // count took as long as the next report took to come, and until one comes it is the slowest
    // movement that still reads as continuous, one count per kGap. Read over one report interval,
    // a lone count after a quick turn (interval 1 ms) was full tilt for 2 ms.
    HostTime covers(size_t age, HostTime interval) const {
        if (age + 1 < reportCount_) {
            const HostTime gap = report(age).t - report(age + 1).t;
            if (gap <= kGap) return gap;
        }
        const Report& r = report(age);
        if (std::max(std::llabs(r.dx), std::llabs(r.dy)) > 1) return interval;
        return age > 0 ? std::clamp(report(age - 1).t - r.t, kTick, kGap) : kGap;
    }

    // Mouse speed in counts per second at `now`. 0 once reports stopped coming.
    std::array<double, 2> velocity(HostTime now) const {
        if (!reportCount_) return {0, 0};
        // The mouse's report interval: the shortest gap seen between reports. Until two reports
        // came, the 8 ms of a 125 Hz mouse, so a first report never overshoots.
        HostTime interval = 80000;
        for (size_t i = 0; i + 1 < reportCount_; ++i) interval = std::min(interval, std::max(kTick, report(i).t - report(i + 1).t));
        // The gap the next report should come within: the longer of the last two, so a slow,
        // uneven movement (reports 1 to 3 counts apart) does not read as a stop.
        HostTime expected = covers(0, interval);
        if (reportCount_ > 1 && report(0).t - report(1).t <= kGap) expected = std::max(expected, covers(1, interval));
        // A lone count after a pause holds half its kGap: the stick stays past the deadzone for
        // about 25 ms, long enough for a 60 Hz game to see it, and lets go soon after a stop.
        else if (expected == kGap) expected = kGap / 2;
        const HostTime age = now - report(0).t;
        // A report twice as late as expected: the mouse stopped. Between one and two gaps
        // late, the speed can be at most what a report arriving now would show.
        if (age > 2 * expected + kTick) return {0, 0};
        const double late = age > expected + kTick ? double(expected) / double(age) : 1.0;
        double sx = 0, sy = 0;
        HostTime span = 0;
        for (size_t i = 0; i < reportCount_ && span < kSpan; ++i) {
            const HostTime len = covers(i, interval), take = std::min(len, kSpan - span);
            const double part = double(take) / double(len);
            sx += double(report(i).dx) * part;
            sy += double(report(i).dy) * part;
            span += take;
            if (i + 1 >= reportCount_ || report(i).t - report(i + 1).t > kGap) break;   // movement started here
        }
        const double perSecond = 1e7 / double(span) * late;
        return {sx * perSecond, sy * perSecond};
    }

    ControllerState mouseState(HostTime now) const {
        ControllerState s;
        const auto [vx, vy] = velocity(now);
        if (settings_.mouse == KbmSettings::Mouse::Gyro) {
            const double gain = settings_.gyroPerCount();
            s.gyro = {float(-vy * gain), float(-vx * gain), 0.f};
            s.accel = {0.f, 1.f, 0.f};
            s.motionValid = true;
            s.motionTimestampUs = uint64_t(now / 10);
            s.motionSamples[0] = {s.gyro, s.accel, s.motionTimestampUs};
            s.motionSampleCount = 1;
            return s;
        }
        const double speed = std::sqrt(vx * vx + vy * vy);
        if (speed <= 0) return s;
        const double exponent = 1.6 - 1.2 * settings_.curve;
        const double knee = settings_.precisionKnee();
        const double r = std::min(1.0, std::max(speed, knee) / settings_.fullTilt());
        const double travel = std::pow(r, exponent) * (speed < knee ? speed / knee : 1.0);
        const double magnitude = settings_.deadzone + (1.0 - settings_.deadzone) * travel;
        s.rightX = toAxis(float(vx / speed * magnitude));
        s.rightY = toAxis(float(vy / speed * magnitude));
        return s;
    }

    static size_t slot(uint32_t scan, bool extended) { return (scan & 0xFF) | (extended ? 0x100u : 0u); }
    static int16_t toAxis(float v) { return int16_t(std::lround(std::clamp(v, -1.f, 1.f) * 32767.f)); }

    void bindDefaults() {
        using B = ControllerState;
        auto button = [](uint32_t bit) { return KbmBinding{KbmTarget::Button, bit}; };
        bindKey(0x11, false, {KbmTarget::LeftUp});    // W / Z
        bindKey(0x1F, false, {KbmTarget::LeftDown});  // S
        bindKey(0x1E, false, {KbmTarget::LeftLeft});  // A / Q
        bindKey(0x20, false, {KbmTarget::LeftRight}); // D
        bindKey(0x39, false, button(B::Cross));       // space
        bindKey(0x12, false, button(B::Circle));      // E
        bindKey(0x13, false, button(B::Square));      // R
        bindKey(0x21, false, button(B::Triangle));    // F
        bindKey(0x2A, false, button(B::L3));          // left shift
        bindKey(0x2E, false, button(B::R3));          // C
        bindKey(0x10, false, button(B::L1));          // Q / A
        bindKey(0x2C, false, button(B::R1));          // Z / W, first letter of the bottom row
        bindKey(0x01, false, button(B::Options));     // escape
        bindKey(0x0F, false, button(B::Touchpad));    // tab
        bindKey(0x48, true, button(B::Up));
        bindKey(0x50, true, button(B::Down));
        bindKey(0x4B, true, button(B::Left));
        bindKey(0x4D, true, button(B::Right));
        bindMouse(KbmMouse::Left, {KbmTarget::R2});
        bindMouse(KbmMouse::Right, {KbmTarget::L2});
        bindMouse(KbmMouse::Middle, button(B::R3));
        bindMouse(KbmMouse::X1, button(B::L1));
        bindMouse(KbmMouse::X2, button(B::R1));
        bindMouse(KbmMouse::WheelUp, button(B::Up));
        bindMouse(KbmMouse::WheelDown, button(B::Down));
    }

    KbmSettings settings_;
    std::array<KbmBinding, 0x200> keys_{};
    std::array<bool, 0x200> held_{};
    std::array<KbmBinding, 8> mouseBinds_{};   // indexed by KbmMouse
    std::array<bool, 6> mouseHeld_{};          // buttons 1-5
    long long dx_ = 0, dy_ = 0;                // movement since the last sample()
    std::array<Report, 32> reports_{};         // ring of the last reads that had movement
    size_t reportAt_ = 0, reportCount_ = 0;
    KbmMouse wheel_{};                         // wheel notch being pressed, {} for none
    HostTime wheelUntil_ = 0;
};

// The state sent to the PS5 when a physical pad and the keyboard/mouse are both active:
// buttons and triggers combine, each stick takes whichever source is deflected further.
inline ControllerState mergeInputs(const ControllerState& pad, const ControllerState& kbm) {
    if (!pad.inputActive) return kbm;
    if (!kbm.inputActive) return pad;
    ControllerState s = pad;
    s.buttons |= kbm.buttons;
    s.l2 = std::max(pad.l2, kbm.l2);
    s.r2 = std::max(pad.r2, kbm.r2);
    auto mag = [](int16_t x, int16_t y) { return int(x) * x + int(y) * y; };
    if (mag(kbm.leftX, kbm.leftY) > mag(pad.leftX, pad.leftY)) { s.leftX = kbm.leftX; s.leftY = kbm.leftY; }
    if (mag(kbm.rightX, kbm.rightY) > mag(pad.rightX, pad.rightY)) { s.rightX = kbm.rightX; s.rightY = kbm.rightY; }
    if (kbm.motionValid) {
        s.gyro = kbm.gyro; s.accel = kbm.accel; s.motionValid = true; s.motionTimestampUs = kbm.motionTimestampUs;
        s.motionSamples = kbm.motionSamples; s.motionSampleCount = kbm.motionSampleCount;
    }
    return s;
}

}
