#pragma once
// Fork, "anticipation caméra" (docs/CAMERA_ANTICIPATION_2026-11-20.md).
//
// A friend who plays a share sees each picture one whole loop after the stick moved: pad ->
// host -> PS5 -> game -> encoders -> wire -> this screen. Nothing upstream can be predicted,
// but turning the camera shifts the whole picture, and the stick is known here the moment it
// moves. So every screen refresh the newest picture is drawn shifted by the camera motion the
// stick has asked for and the stream has not shown yet (VR timewarp, Reflex 2 Frame Warp).
//
// This file is the arithmetic, with no GPU in it:
//  - StickHistory: the right stick as sent, with its times (written by the input thread);
//  - motionProfiles / motionBetween: the global shift between two 160x90 luma copies;
//  - CameraModel: learnt while playing, the loop delay and the speed per deflection, per axis,
//    and how much of the measured motion the stick explains (the confidence);
//  - CameraAnticipation: per picture, the shift to draw at a given time, and a residual that
//    keeps what is on screen continuous when a new picture replaces the one being shifted.
//
// Ported from the guest page's prototype (web/guest/index.html, `lab`).
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <deque>
#include <mutex>

namespace veyra::engine {

// Right stick in -1..1, each value held until the next entry. Times in ms on this PC's clock.
class StickHistory {
public:
    struct Entry { double t = 0; float x = 0, y = 0; };
    void note(double tMs, float x, float y) {
        std::lock_guard lock(mutex_);
        if (!entries_.empty()) {
            if (entries_.back().x == x && entries_.back().y == y) return;
            tMs = std::max(tMs, entries_.back().t);
        }
        entries_.push_back({tMs, x, y});
        while (entries_.size() > 2 && entries_[1].t < tMs - kKeepMs) entries_.pop_front();
    }
    // Integral of f(entry) over [t0, t1], in f units x ms; negative when t1 < t0.
    template <class F> double sum(double t0, double t1, F&& f) const {
        if (t1 < t0) return -sum(t1, t0, f);
        std::lock_guard lock(mutex_);
        double acc = 0, end = t1;
        for (auto it = entries_.rbegin(); it != entries_.rend() && end > t0; ++it) {
            const double start = std::max(it->t, t0);
            if (start < end) { acc += double(f(*it)) * (end - start); end = start; }
        }
        return acc;
    }
    Entry at(double tMs) const {
        std::lock_guard lock(mutex_);
        for (auto it = entries_.rbegin(); it != entries_.rend(); ++it) if (it->t <= tMs) return *it;
        return {};
    }
    void clear() { std::lock_guard lock(mutex_); entries_.clear(); }

private:
    static constexpr double kKeepMs = 2500;
    mutable std::mutex mutex_;
    std::deque<Entry> entries_;
};

// ---- global motion between two pictures: the shift that lines up their column and row profiles
constexpr int kMotionWidth = 160, kMotionHeight = 90;
struct MotionProfiles {
    std::array<float, kMotionWidth> col{};
    std::array<float, kMotionHeight> row{};
};
// luma: kMotionWidth x kMotionHeight values, row 0 at the top. `pitch` is in floats.
inline MotionProfiles motionProfiles(const float* luma, size_t pitch = kMotionWidth) {
    std::array<float, kMotionWidth> col{};
    std::array<float, kMotionHeight> row{};
    const int x0 = int(kMotionWidth * 0.15), x1 = int(kMotionWidth * 0.85), y0 = int(kMotionHeight * 0.2), y1 = int(kMotionHeight * 0.8);
    for (int y = 0; y < kMotionHeight; ++y) {
        const float* line = luma + size_t(y) * pitch;
        const bool inY = y >= y0 && y < y1;
        for (int x = 0; x < kMotionWidth; ++x) {
            if (inY) col[x] += line[x];
            if (x >= x0 && x < x1) row[y] += line[x];
        }
    }
    MotionProfiles out;
    for (int i = 1; i < kMotionWidth - 1; ++i) out.col[i] = col[i + 1] - col[i - 1];
    for (int i = 1; i < kMotionHeight - 1; ++i) out.row[i] = row[i + 1] - row[i - 1];
    return out;
}
// Picture fractions the content moved from `prev` to `cur` (+x right, +y down), and how well the
// two profiles line up at that shift (0..1) per axis.
struct MotionEstimate { double dx = 0, dy = 0, cx = 0, cy = 0; };
namespace detail {
// cur[i] ~ prev[i - d]: the content moved by d samples. lo..hi leaves the picture's rim out.
template <size_t N> inline void profileShift(const std::array<float, N>& cur, const std::array<float, N>& prev, int range, int lo, int hi, double& shift, double& score) {
    std::array<double, 64> scores{};
    double best = -2; int bestD = 0;
    for (int d = -range; d <= range; ++d) {
        double sxy = 0, sxx = 0, syy = 0;
        for (int i = std::max(lo, lo + d); i < std::min(hi, hi + d); ++i) {
            const double a = cur[size_t(i)], p = prev[size_t(i - d)];
            sxy += a * p; sxx += a * a; syy += p * p;
        }
        const double s = sxx > 1e-9 && syy > 1e-9 ? sxy / std::sqrt(sxx * syy) : 0;
        scores[size_t(d + range)] = s;
        if (s > best) { best = s; bestD = d; }
    }
    double frac = 0; const int k = bestD + range;
    if (k > 0 && k < 2 * range) {
        const double a = scores[size_t(k - 1)], c = scores[size_t(k + 1)], den = a - 2 * scores[size_t(k)] + c;
        if (den < 0) frac = std::clamp(0.5 * (a - c) / den, -0.5, 0.5);
    }
    shift = bestD + frac; score = std::max(0.0, best);
}
} // namespace detail
inline MotionEstimate motionBetween(const MotionProfiles& cur, const MotionProfiles& prev) {
    MotionEstimate m; double x = 0, y = 0;
    detail::profileShift(cur.col, prev.col, 28, 4, kMotionWidth - 4, x, m.cx);
    detail::profileShift(cur.row, prev.row, 16, 9, kMotionHeight - 9, y, m.cy);
    m.dx = x / kMotionWidth; m.dy = y / kMotionHeight;
    return m;
}

// ---- what the stick does to the picture: a delay, and a speed per deflection, learnt per axis
class CameraModel {
public:
    static constexpr int kLags = 20;
    static constexpr double kLagStepMs = 20;
    // One new picture: v is its content speed (picture fractions per second) over the dt ms
    // before t (its time on the stick's clock); sure is the measurement's score per axis.
    void learn(double t, double dt, const double v[2], const double sure[2], const StickHistory& stick) {
        constexpr double kDecay = 0.985;
        // An axis learns only while its stick moved within the longest delay: what the picture
        // does with the stick at rest says nothing about the stick, and must not wear the model away.
        const double span = kLags * kLagStepMs + dt;
        bool live[2];
        for (int k = 0; k < 2; ++k)
            live[k] = sure[k] >= 0.5 && stick.sum(t - span, t, [k](const StickHistory::Entry& e) { return std::abs(k ? e.y : e.x); }) > 0.02 * span;
        for (int k = 0; k < 2; ++k) {
            if (!live[k]) continue;
            auto& a = axes_[k];
            a.syy = a.syy * kDecay + v[k] * v[k];
            for (int j = 0; j < kLags; ++j) {
                const double s = mean(stick, k, t - j * kLagStepMs - dt, t - j * kLagStepMs, dt);
                a.sxy[j] = a.sxy[j] * kDecay + v[k] * s;
                a.sxx[j] = a.sxx[j] * kDecay + s * s;
            }
        }
        // One delay for both axes: the one whose stick history explains the most motion.
        std::array<double, kLags> explained{};
        int bestJ = -1; double best = 0;
        for (int j = 0; j < kLags; ++j) {
            for (const auto& a : axes_) if (a.sxx[j] > 0.3) explained[j] += a.sxy[j] * a.sxy[j] / a.sxx[j];
            if (explained[j] > best) { best = explained[j]; bestJ = j; }
        }
        if (bestJ < 0) { confidence_ = 0; return; }
        double frac = 0;
        if (bestJ > 0 && bestJ < kLags - 1) {
            const double den = explained[bestJ - 1] - 2 * explained[bestJ] + explained[bestJ + 1];
            if (den < 0) frac = std::clamp(0.5 * (explained[bestJ - 1] - explained[bestJ + 1]) / den, -0.5, 0.5);
        }
        lagMs_ = (bestJ + frac) * kLagStepMs;
        const double total = axes_[0].syy + axes_[1].syy;
        confidence_ = total > 0 ? std::min(1.0, best / total) : 0;
        for (int k = 0; k < 2; ++k) {
            auto& a = axes_[k];
            a.gain = a.sxx[bestJ] > 0.3 ? a.sxy[bestJ] / a.sxx[bestJ] : 0;
            if (!live[k]) continue;
            // The response curve: mean speed per deflection band, at the chosen delay.
            const double s = mean(stick, k, t - lagMs_ - dt, t - lagMs_, dt), m = std::abs(s);
            for (size_t b = 0; b + 1 < kEdges.size(); ++b)
                if (m >= kEdges[b] && m < kEdges[b + 1]) {
                    auto& bin = a.bins[b];
                    bin.v = bin.v * 0.97 + v[k] * (s < 0 ? -1.0 : 1.0); bin.s = bin.s * 0.97 + m; bin.n = bin.n * 0.97 + 1;
                }
        }
    }
    // Content speed (picture fractions per second) for a deflection: the learnt band means joined
    // by straight lines, else the plain gain. Nothing inside the dead zone.
    double speed(int k, double s) const {
        const auto& a = axes_[k];
        const double m = std::abs(s);
        if (m < kEdges[0]) return 0;
        bool hasLo = false, hasHi = false; double loS = 0, loV = 0, hiS = 0, hiV = 0;
        for (const auto& bin : a.bins) {
            if (bin.n < 4) continue;
            const double bs = bin.s / bin.n, bv = bin.v / bin.n;
            if (bs <= m) { hasLo = true; loS = bs; loV = bv; }
            else if (!hasHi) { hasHi = true; hiS = bs; hiV = bv; }
        }
        const double v = hasLo && hasHi ? loV + (hiV - loV) * (m - loS) / (hiS - loS) : hasLo ? loV * m / loS : hasHi ? hiV * m / hiS : a.gain * m;
        return s < 0 ? -v : v;
    }
    // Content displacement (picture fractions) the stick asked for between t0 and t1.
    double displacement(int k, double t0, double t1, const StickHistory& stick) const {
        return stick.sum(t0, t1, [this, k](const StickHistory::Entry& e) { return speed(k, k ? e.y : e.x); }) / 1000.0;
    }
    double lagMs() const { return lagMs_; }
    double confidence() const { return confidence_; }
    double gain(int k) const { return axes_[k].gain; }
    // 0 below a quarter of the motion explained, 1 from 55 %: a menu, a cutscene or any picture
    // that does not follow the stick gets no shift.
    double weight() const { return std::clamp((confidence_ - 0.25) / 0.3, 0.0, 1.0); }

private:
    static constexpr std::array<double, 6> kEdges{0.06, 0.25, 0.45, 0.65, 0.85, 1.01};
    static double mean(const StickHistory& stick, int k, double t0, double t1, double dt) {
        return stick.sum(t0, t1, [k](const StickHistory::Entry& e) { return k ? e.y : e.x; }) / dt;
    }
    struct Bin { double v = 0, s = 0, n = 0; };
    struct Axis {
        std::array<double, kLags> sxy{}, sxx{};
        double syy = 0, gain = 0;
        std::array<Bin, 5> bins{};
    };
    std::array<Axis, 2> axes_{};
    double lagMs_ = 0, confidence_ = 0;
};

// ---- per picture: the shift to draw, continuous across pictures
class CameraAnticipation {
public:
    struct Shift { float x = 0, y = 0; };
    struct Stats {
        bool learnt = false;      // a delay and a speed are known
        bool active = false;      // the last draw was shifted by the model
        double confidence = 0, lagMs = 0, gainX = 0, gainY = 0;
        float shiftX = 0, shiftY = 0;
        double residualX = 0, residualY = 0;      // what the last pictures left to fade
        double expectedX = 0, measuredX = 0;      // the last picture's motion: the model's, the picture's own
        uint64_t pictures = 0, measured = 0;
    };
    // Limits, in picture fractions. The first covers the loop's delay up to the picture's first
    // draw; the second what the stick adds while that picture stays on screen, so a fast turn
    // whose delay part is at its limit still moves between two pictures.
    static constexpr double kDelayLimit = 0.08, kHoldLimit = 0.06, kResidualLimit = 0.03, kResidualMs = 80;

    StickHistory& stick() { return stick_; }
    const StickHistory& stick() const { return stick_; }
    const CameraModel& model() const { return model_; }

    // The input thread: the right stick as it leaves for the host.
    void input(int64_t host100ns, float x, float y) { if (!scripted_.load(std::memory_order_relaxed)) stick_.note(double(host100ns) / 10000.0, x, y); }
    // The bench drives the stick itself (stick().note); the pad's own values are then left out.
    void setScripted(bool on) { scripted_.store(on, std::memory_order_relaxed); }

    // A new picture replaces the one on screen at `now`. pictureTime is when the host had it, on
    // this PC's clock. motion: the content's measured displacement from the picture it replaces
    // (null when there is none: first picture, a failed readback).
    void picture(int64_t pictureTime100ns, int64_t now100ns, const MotionEstimate* motion) {
        const double t = double(pictureTime100ns) / 10000.0, now = double(now100ns) / 10000.0;
        ++stats_.pictures;
        const double shown = std::clamp(now - t, 0.0, 250.0);
        showMs_ = havePicture_ ? showMs_ + (shown - showMs_) * 0.1 : shown;
        if (havePicture_ && motion) {
            const double dt = t - pictureMs_;
            if (dt > 4 && dt < 250) {
                ++stats_.measured;
                const double v[2] = {motion->dx * 1000.0 / dt, motion->dy * 1000.0 / dt}, sure[2] = {motion->cx, motion->cy};
                model_.learn(t, dt, v, sure, stick_);
                // What was on screen: the old picture and its shift. What comes: the new picture,
                // moved by `motion`, and its own shift. The difference of the two shifts is the motion
                // the model expected; where the picture did otherwise, the residual takes the
                // difference and fades, so the screen sees no step. Never more than the expected
                // motion itself: content that moves without the stick is not held back.
                const auto before = raw(pictureMs_, now), after = raw(t, now);
                decay(now);
                const double measured[2] = {motion->dx, motion->dy};
                stats_.expectedX = before[0] - after[0]; stats_.measuredX = motion->dx;
                for (int k = 0; k < 2; ++k) {
                    const double expected = before[k] - after[k];
                    if (sure[k] < 0.5 || expected == 0) continue;
                    const double error = std::clamp(expected - measured[k], -std::abs(expected), std::abs(expected));
                    residual_[k] = std::clamp(residual_[k] + error, -kResidualLimit, kResidualLimit);
                }
            }
        }
        if (!havePicture_) residualAtMs_ = now;
        pictureMs_ = t; havePicture_ = true;
    }
    // The displacement to draw the current picture with, for a draw that reaches the screen at `at`.
    Shift shift(int64_t at100ns) {
        if (!havePicture_) return {};
        const double at = double(at100ns) / 10000.0;
        const auto s = raw(pictureMs_, at);
        decay(at);
        const Shift out{float(s[0] + residual_[0]), float(s[1] + residual_[1])};
        stats_.shiftX = out.x; stats_.shiftY = out.y;
        stats_.active = model_.weight() > 0 && (s[0] != 0 || s[1] != 0);
        return out;
    }
    // A break in the stream (reconnect, decoder reset): the next picture has no predecessor.
    void discontinuity() { havePicture_ = false; residual_ = {}; }
    Stats stats() const {
        Stats s = stats_;
        s.confidence = model_.confidence(); s.lagMs = model_.lagMs(); s.gainX = model_.gain(0); s.gainY = model_.gain(1);
        s.learnt = model_.weight() > 0;
        s.residualX = residual_[0]; s.residualY = residual_[1];
        return s;
    }

private:
    // The model's shift for a picture of time t drawn at `at`, without the residual.
    std::array<double, 2> raw(double t, double at) const {
        const double w = model_.weight();
        if (w <= 0) return {0, 0};
        const double first = t + showMs_;
        std::array<double, 2> out{};
        for (int k = 0; k < 2; ++k)
            out[k] = std::clamp(w * model_.displacement(k, t - model_.lagMs(), first, stick_), -kDelayLimit, kDelayLimit)
                   + std::clamp(w * model_.displacement(k, first, at, stick_), -kHoldLimit, kHoldLimit);
        return out;
    }
    void decay(double now) {
        if (now <= residualAtMs_) return;
        const double keep = std::exp(-(now - residualAtMs_) / kResidualMs);
        residual_[0] *= keep; residual_[1] *= keep; residualAtMs_ = now;
    }

    StickHistory stick_;
    std::atomic<bool> scripted_{false};
    CameraModel model_;
    Stats stats_;
    std::array<double, 2> residual_{};
    double pictureMs_ = 0, showMs_ = 0, residualAtMs_ = 0;
    bool havePicture_ = false;
};

// ---- a simulated game for the bench: the camera follows the right stick lagMs late through a
// dead zone and a power curve; the position is in picture fractions, +x looks right.
class CameraSimulation {
public:
    static constexpr double kGainX = 0.9, kGainY = 0.6;   // picture fractions per second at full deflection
    explicit CameraSimulation(double lagMs = 100) : lagMs_(lagMs) {}
    static double curve(double s) {
        const double m = std::abs(s);
        if (m < 0.1) return 0;
        const double v = std::pow((m - 0.1) / 0.9, 1.6);
        return s < 0 ? -v : v;
    }
    // The camera a picture taken at t shows: the stick up to t - lag.
    std::array<double, 2> cameraAt(double tMs, const StickHistory& stick) {
        if (!started_) { camMs_ = tMs; started_ = true; }
        if (tMs > camMs_) { advance(cam_, camMs_ - lagMs_, tMs - lagMs_, stick); camMs_ = tMs; }
        return cam_;
    }
    // Where the camera would be at t with no delay at all.
    std::array<double, 2> idealAt(double tMs, const StickHistory& stick) {
        if (!idealStarted_) { idealMs_ = tMs; idealStarted_ = true; }
        if (tMs > idealMs_) { advance(ideal_, idealMs_, tMs, stick); idealMs_ = tMs; }
        return ideal_;
    }
    double lagMs() const { return lagMs_; }

private:
    static void advance(std::array<double, 2>& pos, double from, double to, const StickHistory& stick) {
        pos[0] += stick.sum(from, to, [](const StickHistory::Entry& e) { return curve(e.x); }) / 1000.0 * kGainX;
        pos[1] += stick.sum(from, to, [](const StickHistory::Entry& e) { return curve(e.y); }) / 1000.0 * kGainY;
    }
    double lagMs_ = 100, camMs_ = 0, idealMs_ = 0;
    std::array<double, 2> cam_{}, ideal_{};
    bool started_ = false, idealStarted_ = false;
};

} // namespace veyra::engine
