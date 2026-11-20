// Compares mouse-to-stick mappers offline on a model mouse: a sensor that reports whole counts at
// its polling rate, read by a 1 ms loop with the jitter the input thread logs (p95 1.5 ms).
// For each movement profile it prints the onset delay (movement start to the first stick off
// center), the stop delay (last report to the stick centered), the lag that best aligns the
// stick with the ideal stick (the same curve fed the true speed), the error left after that
// alignment, and the reads that fell to center while the mouse moved.
// Built twice by scripts\fork\kbm-replay.cmd: against include\ and against another KbmMapper.h.
// KBM_OLD_API: that header names the gain `sensitivity` (full tilt at 4000 (1 - 0.9 s)).
#include "veyra/remoteplay/KbmMapper.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <random>
#include <vector>

using namespace veyra::remoteplay;

namespace {
constexpr double kFullTilt = 1840;   // counts/s at full tilt for both mappers (the old default)

KbmSettings settings() {
    KbmSettings s;
#ifdef KBM_OLD_API
    s.sensitivity = float((1 - kFullTilt / 4000) / 0.9);
#else
    s.speed = float(std::log(kFullTilt / 3000) / std::log(200.0 / 3000));
#endif
    return s;
}

double ideal(double speed) {
    if (speed <= 0) return 0;
    const KbmSettings s = settings();
    const double r = std::min(1.0, speed / kFullTilt);
    return s.deadzone + (1 - s.deadzone) * std::pow(r, 1.6 - 1.2 * s.curve);
}

struct Result { double onset = 0, stop = 0, lag = 0, error = 0; int dropouts = 0; };

// speed(t) in counts/s for t in seconds; the movement lasts `length` seconds.
Result run(int rate, double length, const std::function<double(double)>& speed, unsigned seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> tickJitter(0, 0.0005), usb(0.0002, 0.0004);
    KbmMapper m;
    m.setSettings(settings());
    struct Arrival { double t; int dx; };
    std::vector<Arrival> arrivals;
    double owed = 0, lastReport = -1;
    const double poll = 1.0 / rate;
    for (double t = 0.1 + poll; t <= 0.1 + length + 1e-9; t += poll) {
        // Integrate the speed over the poll interval in 0.1 ms steps.
        for (double u = t - poll; u < t - 1e-9; u += 0.0001) owed += speed(u - 0.1) * 0.0001;
        const int dx = int(owed);
        if (!dx) continue;
        owed -= dx;
        arrivals.push_back({t + usb(rng), dx});
        lastReport = arrivals.back().t;
    }
    std::vector<double> times, stick, truth;
    size_t next = 0;
    Result r{-1, -1, 0, 0, 0};
    for (int k = 0; k < int((0.2 + length + 0.1) * 1000); ++k) {
        const double t = k * 0.001 + tickJitter(rng);
        while (next < arrivals.size() && arrivals[next].t <= t) m.mouseMove(arrivals[next++].dx, 0);
        const auto s = m.sample(HostTime(t * 1e7));
        const double x = s.rightX / 32767.0;
        times.push_back(t); stick.push_back(x);
        const double v = t >= 0.1 && t <= 0.1 + length ? speed(t - 0.1) : 0;
        truth.push_back(v);
        if (r.onset < 0 && x > 0) r.onset = (t - arrivals.front().t) * 1000;
        if (t > lastReport && r.stop < 0 && x == 0) r.stop = (t - lastReport) * 1000;
        if (v > 50 && t > arrivals.front().t + 0.003 && t < lastReport && x == 0) ++r.dropouts;
    }
    // Lag: the shift of the ideal stick that minimizes the squared error, in 0.25 ms steps.
    double best = 1e18;
    for (double lag = 0; lag <= 0.020; lag += 0.00025) {
        double e = 0;
        int n = 0;
        for (size_t i = 0; i < times.size(); ++i) {
            const double u = times[i] - lag - 0.1;
            const double want = u >= 0 && u <= length ? ideal(speed(u)) : 0;
            e += (stick[i] - want) * (stick[i] - want);
            ++n;
        }
        if (e < best) { best = e; r.lag = lag * 1000; r.error = std::sqrt(e / n); }
    }
    return r;
}

// Slow aim at the default settings: a 1000 Hz mouse moving at a constant `speed` for 1 s, read
// every 1 ms. The stick's magnitude over the movement: the mean, the 95th percentile and the
// highest read, the share of reads off center and of reads at more than half tilt. `warm`: the
// same right after a quick turn (1000 counts/s for 100 ms, then 300 ms still), as when the aim
// is corrected after turning.
struct Aim { double mean = 0, p95 = 0, peak = 0, moving = 0, overHalf = 0; };
Aim aim(double speed, unsigned seed, bool warm) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> tickJitter(0, 0.0005), usb(0.0002, 0.0004), phase(0, 1);
    KbmMapper m;
    struct Arrival { double t; int dx; };
    std::vector<Arrival> arrivals;
    if (warm) for (int i = 0; i < 100; ++i) arrivals.push_back({0.2 + i * 0.001 + usb(rng), 1});
    double owed = phase(rng);
    for (double t = 0.6; t <= 1.6; t += 0.001) {
        owed += speed * 0.001;
        const int dx = int(owed);
        if (!dx) continue;
        owed -= dx;
        arrivals.push_back({t + usb(rng), dx});
    }
    std::vector<double> mags;
    size_t next = 0;
    for (int k = 0; k < 1700; ++k) {
        const double t = k * 0.001 + tickJitter(rng);
        while (next < arrivals.size() && arrivals[next].t <= t) m.mouseMove(arrivals[next++].dx, 0);
        const auto s = m.sample(HostTime(t * 1e7));
        if (t > 0.7 && t < 1.6) mags.push_back(std::hypot(s.rightX / 32767.0, s.rightY / 32767.0));
    }
    Aim a;
    for (const double v : mags) { a.mean += v; a.moving += v > 0; a.overHalf += v > 0.5; a.peak = std::max(a.peak, v); }
    a.mean /= double(mags.size()); a.moving /= double(mags.size()); a.overHalf /= double(mags.size());
    std::sort(mags.begin(), mags.end());
    a.p95 = mags[size_t(0.95 * double(mags.size() - 1))];
    return a;
}

void slowAim(bool warm) {
    std::printf("\nslow aim%s, default settings, 1000 Hz (stick magnitude; full tilt at %.0f counts/s)\n", warm ? " after a quick turn" : "", KbmSettings{}.fullTilt());
    std::printf("%8s | %6s %6s %6s | %7s %7s\n", "counts/s", "mean", "p95", "peak", "moving", ">half");
    for (const double speed : {5.0, 10.0, 20.0, 30.0, 40.0, 60.0, 80.0, 120.0, 200.0, 400.0, 775.0, 2000.0}) {
        Aim sum;
        const int runs = 20;
        for (int i = 0; i < runs; ++i) {
            const Aim a = aim(speed, 99u + unsigned(i), warm);
            sum.mean += a.mean / runs; sum.p95 += a.p95 / runs; sum.peak = std::max(sum.peak, a.peak);
            sum.moving += a.moving / runs; sum.overHalf += a.overHalf / runs;
        }
        std::printf("%8.0f | %6.3f %6.3f %6.3f | %6.0f%% %6.0f%%\n", speed, sum.mean, sum.p95, sum.peak, sum.moving * 100, sum.overHalf * 100);
    }
}
}

int main() {
    struct Profile { const char* name; double length; std::function<double(double)> speed; };
    const double pi = 3.14159265358979;
    const Profile profiles[] = {
        {"constant 250/s ", 0.2, [](double) { return 250.0; }},
        {"constant 1000/s", 0.2, [](double) { return 1000.0; }},
        {"constant 3000/s", 0.2, [](double) { return 3000.0; }},
        {"swing 0-2500/s ", 0.6, [pi](double t) { const double s = std::sin(pi * t / 0.6); return 2500 * s * s; }},
        {"slow 80-400/s  ", 0.6, [pi](double t) { return 240 + 160 * std::sin(2 * pi * t / 0.3); }},
        {"flick 8000/s   ", 0.03, [](double) { return 8000.0; }},
    };
    std::printf("%-15s %5s | %8s %8s %7s %7s %8s\n", "profile", "rate", "onset ms", "stop ms", "lag ms", "error", "dropouts");
    for (const int rate : {1000, 125}) {
        for (const auto& p : profiles) {
            Result sum{};
            const int runs = 20;
            for (int i = 0; i < runs; ++i) {
                const Result r = run(rate, p.length, p.speed, 1234u + unsigned(i));
                sum.onset += r.onset / runs; sum.stop += r.stop / runs; sum.lag += r.lag / runs;
                sum.error += r.error / runs; sum.dropouts += r.dropouts;
            }
            std::printf("%-15s %5d | %8.2f %8.2f %7.2f %7.3f %8d\n", p.name, rate, sum.onset, sum.stop, sum.lag, sum.error, sum.dropouts);
        }
    }
    slowAim(false);
    slowAim(true);
}
