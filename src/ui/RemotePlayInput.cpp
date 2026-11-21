#include "veyra/ui/RemotePlayInput.h"

#include <avrt.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <vector>

#include "veyra/Log.h"
#include "veyra/engine/EngineController.h"
#include "veyra/moonlight/InputRouter.h"
#include "veyra/remoteplay/PadPolicy.h"
#ifdef VEYRA_ENABLE_GUEST
#include "veyra/guest/GuestStreamer.h"
#endif

#pragma comment(lib, "avrt.lib")

namespace veyra::ui {
namespace {

// The same test as "a Veyra window has the focus" on the UI thread, without asking Qt.
bool foregroundIsOurs() {
    DWORD pid = 0;
    const HWND foreground = GetForegroundWindow();
    return foreground && GetWindowThreadProcessId(foreground, &pid) && pid == GetCurrentProcessId();
}

// Mouse and keyboard to `target` even though it is never the foreground window; null removes both.
bool registerRawInput(HWND target) {
    const DWORD flags = target ? RIDEV_INPUTSINK : RIDEV_REMOVE;
    const RAWINPUTDEVICE devices[] = {{0x01, 0x02, flags, target}, {0x01, 0x06, flags, target}};
    return RegisterRawInputDevices(devices, 2, sizeof(RAWINPUTDEVICE)) != FALSE;
}

std::string percentiles(std::vector<float>& values) {
    std::sort(values.begin(), values.end());
    const size_t n = values.size();
    return std::format("p50={:.2f} p95={:.2f} p99={:.2f} max={:.2f}", values[n / 2], values[n * 95 / 100], values[n * 99 / 100], values.back());
}

} // namespace

void RemotePlayInput::start() {
    {
        // Under the lock the thread decides to end with: either it already chose to end, and is
        // joined here, or it reads the session opened before this call and keeps running.
        std::lock_guard lock(mutex_);
        if (running() && !ended_) return;
    }
    if (running()) thread_.join();
    ended_ = false;
    focused_ = true;
    thread_ = std::jthread([this](std::stop_token stop) { run(stop); });
}

void RemotePlayInput::stop() {
    if (!running()) return;
    thread_.request_stop();
    thread_.join();
    std::lock_guard lock(mutex_);
    capabilities_ = {};
}

void RemotePlayInput::setKbmSettings(const remoteplay::KbmSettings& settings) {
    std::lock_guard lock(mutex_);
    settings_ = settings;
    ++settingsVersion_;
}

remoteplay::ControllerInput::Capabilities RemotePlayInput::capabilities() const {
    std::lock_guard lock(mutex_);
    return capabilities_;
}

void RemotePlayInput::run(std::stop_token stop) {
    SetThreadDescription(GetCurrentThread(), L"PS5 input");
    DWORD task = 0;
    const HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Games", &task);
    if (!mmcss) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    const bool highResolution = timer != nullptr;
    if (!timer) timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    const HWND sink = CreateWindowExW(0, L"Message", nullptr, 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    veyra::log::info("remoteplay-input", std::format("input thread started mmcss={} highResolutionTimer={} rawInputWindow={}",
        mmcss != nullptr, highResolution, sink != nullptr));

    remoteplay::ControllerInput pad;
    if (!pad.start()) veyra::log::warn("remoteplay-input", "SDL gamepad initialization failed");
    remoteplay::KbmMapper kbm;
    moonlight::InputRouter router(kbm, true);   // Ctrl+M (the bindings overlay) never reaches the PS5
    remoteplay::PadSendGate gate;
    remoteplay::PsTap tap;
    unsigned settingsSeen = ~0u;
    bool rawOn = false;
    std::vector<float> gaps, changeAges;
    gaps.reserve(10000);
    remoteplay::HostTime last = 0, next = remoteplay::monotonic100ns();
    unsigned sends = 0, iteration = 0;
    // Where the slowest pass of the window spent its time, in 100 ns: the wait, then each step.
    enum Step { Wait, Raw, Poll, Send, Feedback, Steps };
    std::array<remoteplay::HostTime, Steps> steps{}, worstSteps{};
    float worst = 0;
    // VEYRA_KBM_TRACE=<csv>: one row per pass while the mouse moves or the right stick is off
    // center (time, counts read, counts from a real mouse, stick, handed to the session), written
    // when the thread ends. Injected input (SendInput) has no device; a real mouse has one.
    // scripts/fork/kbm-latency.py joins it with the movements tests/fork/KbmSweep.cpp injected.
    struct TraceRow { remoteplay::HostTime t; int dx, dy, device; int16_t rx, ry; bool sent; };
    const char* tracePath = std::getenv("VEYRA_KBM_TRACE");
    std::vector<TraceRow> trace;
    if (tracePath) trace.reserve(200000);
    // VEYRA_KBM_SWEEP=<csv>: a model mouse moves the right stick by itself, 40 s after the stream
    // starts, without the capture (no focus, no cursor taken): 12 sweeps of 200 ms, one per second,
    // 1000 Hz then 125 Hz reports at 250, 1000 and 3000 counts/s, alternating direction. Each report
    // is written to the csv (KbmSweep.cpp's format) when the thread ends.
    struct SweepMove { remoteplay::HostTime t; int sweep, speed, rate, dx; };
    const char* sweepPath = std::getenv("VEYRA_KBM_SWEEP");
    std::vector<SweepMove> sweepMoves;
    const remoteplay::HostTime sweepStart = remoteplay::monotonic100ns() + 400000000;
    int sweepPolls = 0, sweepIndex = -1;
    double sweepOwed = 0;

    while (!stop.stop_requested()) {
        // A 1 ms period from fixed deadlines, so a late wake shortens the next wait.
        next = std::max(next + 10000, remoteplay::monotonic100ns() + 2000);
        LARGE_INTEGER due{};
        due.QuadPart = -(next - remoteplay::monotonic100ns());
        auto mark = remoteplay::monotonic100ns();
        const auto lap = [&](Step step) { const auto t = remoteplay::monotonic100ns(); steps[step] = t - mark; mark = t; };
        SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
        WaitForSingleObject(timer, 50);
        lap(Wait);
        const remoteplay::HostTime now = mark;
        if (last) {
            gaps.push_back(float(now - last) / 10000.f);
            if (gaps.back() > worst) { worst = gaps.back(); worstSteps = steps; }
        }
        last = now;

        {
            std::lock_guard lock(mutex_);
            if (settingsVersion_ != settingsSeen) { kbm.setSettings(settings_); settingsSeen = settingsVersion_; }
        }
        // Keyboard and mouse only while captured and the captured window is in front.
        const HWND captured = kbmWindow_;
        const bool kbmLive = sink && captured && GetForegroundWindow() == captured;
        if (kbmLive != rawOn) {
            if (!registerRawInput(kbmLive ? sink : nullptr) && kbmLive) veyra::log::warn("remoteplay-input", "raw keyboard/mouse registration failed");
            rawOn = kbmLive;
            if (!rawOn) { kbm.releaseAll(); router.reset(); }
        }
        MSG msg;
        int tickDx = 0, tickDy = 0, tickDevice = 0;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_INPUT && rawOn) {
                alignas(8) BYTE buffer[128];
                UINT size = sizeof(buffer);
                if (GetRawInputData(reinterpret_cast<HRAWINPUT>(msg.lParam), RID_INPUT, buffer, &size, sizeof(RAWINPUTHEADER)) != UINT(-1)) {
                    const auto& raw = *reinterpret_cast<const RAWINPUT*>(buffer);
                    router.raw(raw);
                    if (raw.header.dwType == RIM_TYPEMOUSE) {
                        tickDx += raw.data.mouse.lLastX; tickDy += raw.data.mouse.lLastY;
                        if (raw.header.hDevice) tickDevice += std::abs(raw.data.mouse.lLastX) + std::abs(raw.data.mouse.lLastY);
                    }
                }
            }
            DispatchMessageW(&msg);   // DefWindowProc frees the raw input
        }
        (void)router.takeReserved();   // the hotkeys act from the UI thread's own messages
        bool sweeping = false;
        if (sweepPath && now >= sweepStart && now < sweepStart + 13 * 10000000LL) {
            sweeping = true;
            const int k = int((now - sweepStart) / 10000000);
            const remoteplay::HostTime inSweep = (now - sweepStart) % 10000000;
            if (k != sweepIndex) { sweepIndex = k; sweepPolls = 0; sweepOwed = 0; }
            if (k < 12 && inSweep < 2000000) {
                static constexpr int speeds[] = {250, 1000, 3000};
                const int rate = k < 6 ? 1000 : 125, speed = speeds[(k % 6) / 2], direction = k % 2 ? -1 : 1;
                const remoteplay::HostTime poll = 10000000 / rate;
                const int due = int(std::min<remoteplay::HostTime>(inSweep / poll + 1, 2000000 / poll));
                for (; sweepPolls < due; ++sweepPolls) sweepOwed += double(speed) / rate;
                if (const int dx = int(sweepOwed)) {
                    sweepOwed -= dx;
                    kbm.mouseMove(dx * direction, 0);
                    tickDx += dx * direction;
                    sweepMoves.push_back({now, k, speed, rate, dx * direction});
                }
            }
        }
        lap(Raw);

        const bool focused = background_ || foregroundIsOurs();
        focused_ = focused;
        if (calibrate_.exchange(false)) pad.calibrate();
        auto state = pad.poll(focused);
        if (rawOn || sweeping) state = remoteplay::mergeInputs(state, kbm.sample(now));
#ifdef VEYRA_ENABLE_GUEST
        guest::GuestStreamer* const guest = guest_.load();
        // The owner's own pad first: a new button takes the controller back from a guest.
        if (guest && focused) guest->hostPad(state, now);
        const auto guestPad = guest ? guest->guestState(now) : std::nullopt;
        if (guest) state = guest::combine(guest->mode(), state, guestPad);
#endif
        if (tapPs_.exchange(false)) tap.press(now);
        state = tap.apply(state, now);
        lap(Poll);
        const bool send = gate.due(state, now);
        if (send) {
            engine_.remotePlayController(state);
            ++sends;
            if (const double age = pad.changeAgeMs(); age >= 0) changeAges.push_back(float(age));
#ifdef VEYRA_ENABLE_GUEST
            if (guestPad && guest->mode() != guest::ControlMode::WatchOnly) guest->handedToPs5(state);
#endif
        }
        if (tracePath && trace.size() < trace.capacity() && (tickDx || tickDy || state.rightX || state.rightY || (!trace.empty() && (trace.back().rx || trace.back().ry))))
            trace.push_back({now, tickDx, tickDy, tickDevice, state.rightX, state.rightY, send});
        lap(Send);
        {
            auto feedback = engine_.remotePlayFeedback();
#ifdef VEYRA_ENABLE_GUEST
            if (guest) guest->feedback(feedback);
#endif
            pad.feedback(std::move(feedback), focused);
        }
        lap(Feedback);

        if (iteration++ % 100 == 0) {
            std::lock_guard lock(mutex_);
            capabilities_ = pad.capabilities();
            if (const auto session = engine_.inputSession(); !remoteplay::inputWanted(session)) {
                ended_ = true;
                veyra::log::info("remoteplay-input", std::format("the session ended (remotePlay={} share={} running={}); the input thread stops by itself",
                    session.remotePlay, session.guestClient, session.running));
                break;
            }
        }
        if (gaps.size() >= 10000) {   // about 10 s at 1 ms
            std::string line = std::format("loop interval ms {} samples={} sends={} focused={} kbm={}", percentiles(gaps), gaps.size(), sends, focused, rawOn);
            // The steps of the pass before the slowest wake, then that wake's own wait.
            remoteplay::HostTime accounted = 0;
            for (const auto t : worstSteps) accounted += t;
            line += std::format(" | slowest previous pass ms raw={:.2f} poll={:.2f} send={:.2f} feedback={:.2f} wait={:.2f} other={:.2f}",
                worstSteps[Raw] / 1e4, worstSteps[Poll] / 1e4, worstSteps[Send] / 1e4, worstSteps[Feedback] / 1e4, worstSteps[Wait] / 1e4,
                worst - accounted / 1e4);
            if (!changeAges.empty()) line += std::format(" | pad change to send ms {} changes={}", percentiles(changeAges), changeAges.size());
            veyra::log::info("remoteplay-input-timing", line);
            gaps.clear(); changeAges.clear(); sends = 0; worst = 0;
        }
    }

    engine_.remotePlayController({});
    if (tracePath) {
        if (FILE* f = std::fopen(tracePath, "w")) {
            std::fprintf(f, "t100ns,dx,dy,device,rx,ry,sent\n");
            for (const auto& r : trace) std::fprintf(f, "%lld,%d,%d,%d,%d,%d,%d\n", r.t, r.dx, r.dy, r.device, r.rx, r.ry, int(r.sent));
            std::fclose(f);
        }
        veyra::log::info("remoteplay-input", std::format("kbm trace rows={}", trace.size()));
    }
    if (sweepPath) {
        if (FILE* f = std::fopen(sweepPath, "w")) {
            std::fprintf(f, "t100ns,sweep,speed,rate,dx\n");
            for (const auto& m : sweepMoves) std::fprintf(f, "%lld,%d,%d,%d,%d\n", m.t, m.sweep, m.speed, m.rate, m.dx);
            std::fclose(f);
        }
        veyra::log::info("remoteplay-input", std::format("kbm sweep moves={}", sweepMoves.size()));
    }
    if (rawOn) registerRawInput(nullptr);
    pad.stop();
    if (sink) DestroyWindow(sink);
    if (timer) CloseHandle(timer);
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    veyra::log::info("remoteplay-input", "input thread stopped");
}

} // namespace veyra::ui
