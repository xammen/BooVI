#include "veyra/engine/CameraAnticipation.h"
#include <cstdio>
#include <random>
#include <vector>

using namespace veyra::engine;

static int failures = 0;
static void expect(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}
static int64_t ticks(double ms) { return int64_t(ms * 10000.0); }

// A smooth texture with detail on both axes, sampled at any position (picture fractions).
static float texture(double u, double v) {
    return float(0.5 + 0.2 * std::sin(u * 41.0 + 1.3) + 0.15 * std::sin(u * 97.0 + v * 13.0) + 0.2 * std::sin(v * 53.0 + 0.4) + 0.1 * std::sin(v * 119.0 + u * 7.0));
}
static MotionProfiles profilesAt(double camX, double camY) {
    std::vector<float> luma(size_t(kMotionWidth) * kMotionHeight);
    for (int y = 0; y < kMotionHeight; ++y)
        for (int x = 0; x < kMotionWidth; ++x)
            luma[size_t(y) * kMotionWidth + x] = texture((x + 0.5) / kMotionWidth + camX, (y + 0.5) / kMotionHeight + camY);
    return motionProfiles(luma.data());
}

// The bench's game: 30 pictures a second of a camera that follows the stick `lag` late, shown
// `show` ms after the host had them, redrawn every `refresh` ms. Returns what the screen showed.
struct Run {
    double worstStep = 0;        // largest jump of the shown camera across a picture change, picture fractions
    double steadyError = 0;      // mean |ideal - shown| while the stick is held, picture fractions
    double perPicture = 0;       // the camera's motion per picture while held
    double borderShift = 0;      // largest shift drawn
};
static void learnSweep(CameraAnticipation& a, CameraSimulation& sim, double& t, double& lastCamX, double& lastCamY, bool& have, double frame, double show) {
    for (int i = 0; i < 24; ++i) {
        const double angle = i * 0.9;
        a.input(ticks(t), float(std::cos(angle) * (0.3 + 0.7 * ((i * 7) % 10) / 10.0)), float(std::sin(angle) * (0.3 + 0.7 * ((i * 3) % 10) / 10.0)));
        for (double end = t + 450; t < end; t += frame) {
            const auto cam = sim.cameraAt(t, a.stick());
            sim.idealAt(t, a.stick());   // the same origin as the camera
            MotionEstimate m{-(cam[0] - lastCamX), -(cam[1] - lastCamY), 1, 1};
            a.picture(ticks(t), ticks(t + show), have ? &m : nullptr);
            lastCamX = cam[0]; lastCamY = cam[1]; have = true;
        }
    }
}
static Run heldTurn(CameraAnticipation& a, CameraSimulation& sim, double& t, double& lastCamX, double& lastCamY, double frame, double show, double refresh, float deflection, bool warp) {
    Run r;
    a.input(ticks(t), 0, 0);
    double shownBefore = 0; bool haveShown = false; int samples = 0;
    const double start = t + 600;
    a.input(ticks(start), deflection, 0);
    for (double end = t + 2000; t < end; t += frame) {
        const auto cam = sim.cameraAt(t, a.stick());
        MotionEstimate m{-(cam[0] - lastCamX), -(cam[1] - lastCamY), 1, 1};
        const double perPicture = std::abs(cam[0] - lastCamX);
        lastCamX = cam[0]; lastCamY = cam[1];
        a.picture(ticks(t), ticks(t + show), &m);
        bool first = true;
        for (double draw = t + show; draw < t + show + frame - 0.01; draw += refresh) {
            const auto s = warp ? a.shift(ticks(draw)) : CameraAnticipation::Shift{};
            const double shown = cam[0] - s.x;          // the camera the screen shows
            if (draw > start + 700) {
                if (first && haveShown) r.worstStep = std::max(r.worstStep, std::abs(shown - shownBefore));
                r.steadyError += std::abs(sim.idealAt(draw, a.stick())[0] - shown); ++samples;
                r.perPicture = perPicture;
                r.borderShift = std::max(r.borderShift, double(std::abs(s.x)));
            }
            shownBefore = shown; haveShown = true; first = false;
        }
    }
    if (samples) r.steadyError /= samples;
    return r;
}

int main() {
    {
        const auto a = profilesAt(0.2, 0.1), b = profilesAt(0.2 + 3.4 / kMotionWidth, 0.1 - 1.2 / kMotionHeight);
        const auto m = motionBetween(b, a);   // the camera moved right and up: the content left and down
        expect(std::abs(m.dx * kMotionWidth + 3.4) < 0.25, "global motion: 3.4 columns left, to a quarter of a column");
        expect(std::abs(m.dy * kMotionHeight - 1.2) < 0.25, "global motion: 1.2 rows down, to a quarter of a row");
        expect(m.cx > 0.9 && m.cy > 0.9, "global motion: both profiles line up");
        const auto still = motionBetween(a, a);
        expect(std::abs(still.dx) < 1e-6 && std::abs(still.dy) < 1e-6, "global motion: the same picture has not moved");
    }
    {
        StickHistory h;
        h.note(0, 0, 0); h.note(100, 1, 0); h.note(200, 0.5f, 0);
        const auto x = [](const StickHistory::Entry& e) { return e.x; };
        expect(std::abs(h.sum(50, 250, x) - 125) < 1e-9, "stick history: integral over held values");
        expect(std::abs(h.sum(250, 50, x) + 125) < 1e-9, "stick history: a reversed span is negative");
        expect(h.at(150).x == 1 && h.at(-5).x == 0, "stick history: the value in force at a time");
    }
    const double frame = 1000.0 / 30, show = 30, refresh = 1000.0 / 240;
    {
        CameraAnticipation a; CameraSimulation sim(100);
        double t = 1000, camX = 0, camY = 0; bool have = false;
        learnSweep(a, sim, t, camX, camY, have, frame, show);
        const auto s = a.stats();
        std::printf("     learnt lag %.1f ms, confidence %.2f, gain %.2f / %.2f\n", s.lagMs, s.confidence, s.gainX, s.gainY);
        expect(std::abs(s.lagMs - 100) < 12, "model: the loop delay of a 100 ms game, to 12 ms");
        expect(s.confidence > 0.8 && s.learnt, "model: the stick explains the motion");
        expect(s.gainX < 0 && s.gainY < 0, "model: the content moves against the stick");

        const auto off = heldTurn(a, sim, t, camX, camY, frame, show, refresh, 0.8f, false);
        const auto on = heldTurn(a, sim, t, camX, camY, frame, show, refresh, 0.8f, true);
        std::printf("     held turn: behind the stick %.4f without, %.4f with; step at a new picture %.5f of %.5f a picture; border %.3f\n",
            off.steadyError, on.steadyError, on.worstStep, on.perPicture, on.borderShift);
        expect(on.steadyError < off.steadyError * 0.25, "held turn: the shown camera is at least four times closer to the stick");
        expect(on.worstStep < on.perPicture * 0.35, "held turn: a new picture moves the screen by under a third of its own motion");
        expect(off.worstStep > on.perPicture * 0.9, "held turn: without it each picture is a full step");
        expect(on.borderShift <= CameraAnticipation::kDelayLimit + CameraAnticipation::kHoldLimit + CameraAnticipation::kResidualLimit + 1e-6, "held turn: the shift stays inside its limits");

        // The stick at rest while the picture pans (a cutscene): nothing is shifted, nothing held back.
        a.input(ticks(t), 0, 0);
        double worst = 0;
        for (double end = t + 1500; t < end; t += frame) {
            MotionEstimate m{-0.01, 0, 1, 1};
            a.picture(ticks(t), ticks(t + show), &m);
            if (t > end - 700) for (double draw = t + show; draw < t + show + frame; draw += refresh) worst = std::max(worst, double(std::abs(a.shift(ticks(draw)).x)));
        }
        expect(worst < 1e-4, "stick at rest: a moving picture is left alone");
        expect(a.stats().confidence > 0.8, "stick at rest: the model is not worn away");
    }
    {
        // A fast turn whose delay part is at its limit still moves between two pictures.
        CameraAnticipation a; CameraSimulation sim(200);
        double t = 1000, camX = 0, camY = 0; bool have = false;
        learnSweep(a, sim, t, camX, camY, have, frame, show);
        const auto on = heldTurn(a, sim, t, camX, camY, frame, show, refresh, 1.0f, true);
        std::printf("     fast turn, 200 ms game: step %.5f of %.5f a picture, border %.3f\n", on.worstStep, on.perPicture, on.borderShift);
        expect(on.borderShift > CameraAnticipation::kDelayLimit, "fast turn: the delay part reached its limit");
        expect(on.worstStep < on.perPicture * 0.35, "fast turn: pictures still blend into each other");
    }
    {
        // Content that does not follow the stick: the stick moves, the picture does something else.
        CameraAnticipation a;
        std::mt19937 rng(7); std::uniform_real_distribution<double> noise(-0.02, 0.02);
        double t = 1000; bool have = false;
        for (int i = 0; i < 24; ++i) {
            a.input(ticks(t), float(std::cos(i * 0.9)), float(std::sin(i * 0.9)));
            for (double end = t + 450; t < end; t += frame) {
                MotionEstimate m{noise(rng), noise(rng), 1, 1};
                a.picture(ticks(t), ticks(t + show), have ? &m : nullptr);
                have = true;
            }
        }
        const auto shift = a.shift(ticks(t + show));
        std::printf("     unrelated content: confidence %.3f\n", a.stats().confidence);
        expect(a.stats().confidence < 0.25 && !a.stats().learnt, "unrelated content: no confidence");
        expect(shift.x == 0 && shift.y == 0, "unrelated content: no shift");
    }
    {
        // Poor measurements (a fade, a flat picture) teach nothing.
        CameraAnticipation a; CameraSimulation sim(100);
        double t = 1000; bool have = false; double camX = 0;
        for (int i = 0; i < 24; ++i) {
            a.input(ticks(t), float(std::cos(i * 0.9)), 0);
            for (double end = t + 450; t < end; t += frame) {
                const auto cam = sim.cameraAt(t, a.stick());
                MotionEstimate m{-(cam[0] - camX), 0, 0.2, 0.2};
                a.picture(ticks(t), ticks(t + show), have ? &m : nullptr);
                camX = cam[0]; have = true;
            }
        }
        expect(!a.stats().learnt, "unsure measurements: nothing learnt");
    }
    std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
