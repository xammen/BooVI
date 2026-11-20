// Fork, camera anticipation: the presenter's part (docs/CAMERA_ANTICIPATION_2026-11-20.md).
//
// present() hands each picture to anticipationIngest(): the picture is copied to the stage, its
// global motion against the picture before is measured, the model takes both. The blit then draws
// from the stage, shifted. Between two pictures the engine's owner thread calls anticipationTick()
// once per screen refresh: the same picture again, with the shift the stick has asked for by then.
#include "veyra/engine/VideoPresenter.h"
#include "veyra/engine/AnticipationStage.h"
#include "veyra/pipeline/EnhanceGraph.h"
#include "veyra/gfx/D3D12DeviceContext.h"

#include <algorithm>
#include <cstdlib>
#include <optional>
#include <random>
#include <vector>

namespace veyra::engine {
namespace {
using Clock = std::chrono::steady_clock;
int64_t host100ns() { return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count() / 100; }
double milliseconds(int64_t t100ns) { return double(t100ns) / 10000.0; }
double envNumber(const wchar_t* name, double fallback) {
    wchar_t text[32]{};
    return GetEnvironmentVariableW(name, text, 32) ? _wtof(text) : fallback;
}
double percentile(std::vector<double> values, double p) {
    if (values.empty()) return 0;
    std::sort(values.begin(), values.end());
    return values[std::min(values.size() - 1, size_t(double(values.size()) * p))];
}
void transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource; barrier.Transition.StateBefore = from; barrier.Transition.StateAfter = to;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(1, &barrier);
}

// The bench (VEYRA_TEST_ANTICIPATION_BENCH=1, with the simulated game): it drives the stick and
// reads what each Present drew. A learning sweep, then stick steps with the shift off and on,
// as the guest page's bench did (web/guest/index.html, `bench.run`).
class Bench {
public:
    bool warp() const { return phase_ != Phase::Trials || (mode_ & 1); }
    bool done() const { return phase_ == Phase::Done; }
    // Called before every draw: moves the scripted stick. A move is dated at its own scheduled
    // time, not at the draw that notices it: a real stick does not move in step with the screen.
    void step(double drawMs, CameraAnticipation& model, double scanoutMs) {
        if (!startMs_) { startMs_ = drawMs; nextMs_ = drawMs + 2500; }
        if (drawMs < nextMs_ || phase_ == Phase::Done) return;
        const double now = nextMs_;
        auto set = [&](float x, float y) { model.stick().note(now, x, y); stickX_ = x; };
        switch (phase_) {
        case Phase::Wait: phase_ = Phase::Learn; [[fallthrough]];
        case Phase::Learn:
            if (step_ < 20) {
                const double angle = step_ * 0.9;
                set(float(std::cos(angle) * (0.3 + 0.7 * ((step_ * 7) % 10) / 10.0)), float(std::sin(angle) * (0.3 + 0.7 * ((step_ * 3) % 10) / 10.0)));
                ++step_; nextMs_ = now + 450;
            } else { set(0, 0); phase_ = Phase::Trials; trialStep_ = 0; nextMs_ = now + 600; }
            break;
        case Phase::Trials:
            if (trialStep_ == 0) { nextMs_ = now + 500 + double(rng_() % 120); trialStep_ = 1; }
            else if (trialStep_ == 1) { observing_ = true; haveBase_ = false; t0_ = 0; moved_ = false; nextMs_ = now + 120; trialStep_ = 2; }
            else if (trialStep_ == 2) { t0_ = now; set(0.8f, 0); nextMs_ = now + 650; trialStep_ = 3; }
            else {
                // The release is dated here, at its own time: the ideal camera is summed draw by draw
                // and must not run past it with the stick still held.
                set(0, 0); observing_ = false; trialStep_ = 0;
                if (++trial_ == 8) { trial_ = 0; if (++mode_ == 4) { phase_ = Phase::Done; report(model, scanoutMs); } }
                nextMs_ = now;
            }
            break;
        case Phase::Done: break;
        }
    }
    // Called at every Present, made at `now` and on screen at `screen`: `shown` is the camera it
    // puts there (the picture's own, less the shift), `ideal` the camera with no delay at all at
    // that time; both in picture fractions.
    void drawn(double now, double screen, double shown, double ideal) {
        if (!observing_) return;
        if (!haveBase_) { base_ = shown; haveBase_ = true; return; }
        if (!t0_) return;
        auto& r = results_[mode_ & 1];
        if (!moved_ && std::abs(shown - base_) >= kMoved) { moved_ = true; r.present.push_back(now - t0_); r.screen.push_back(screen - t0_); }
        if (now - t0_ > 250) {
            r.error.push_back(std::abs(ideal - shown));
            const double speed = CameraSimulation::kGainX * CameraSimulation::curve(stickX_);
            if (speed) r.behind.push_back((ideal - shown) / speed * 1000.0);
        }
    }

private:
    static constexpr double kMoved = 3.0 / 1280.0;   // the page's 3 px of 1280
    enum class Phase { Wait, Learn, Trials, Done };
    struct Result { std::vector<double> present, screen, error, behind; };
    void report(const CameraAnticipation& model, double scanoutMs) const {
        const auto s = model.stats();
        auto line = [&](const Result& r) {
            return std::format("{{\"n\":{},\"stickToPresentMs\":{:.1f},\"stickToPresentP90Ms\":{:.1f},\"stickToScreenMs\":{:.1f},\"steadyErrorPercent\":{:.3f},\"steadyBehindMs\":{:.1f}}}",
                r.present.size(), percentile(r.present, 0.5), percentile(r.present, 0.9), percentile(r.screen, 0.5), percentile(r.error, 0.5) * 100.0, percentile(r.behind, 0.5));
        };
        log::info("anticipation-bench", std::format("{{\"learntLagMs\":{:.1f},\"confidence\":{:.2f},\"gainX\":{:.3f},\"gainY\":{:.3f},\"presentToScanoutMs\":{:.2f},\"off\":{},\"on\":{}}}",
            s.lagMs, s.confidence, s.gainX, s.gainY, scanoutMs, line(results_[0]), line(results_[1])));
    }
    Phase phase_ = Phase::Wait;
    double startMs_ = 0, nextMs_ = 0, t0_ = 0, base_ = 0;
    float stickX_ = 0;
    int step_ = 0, trialStep_ = 0, trial_ = 0, mode_ = 0;
    bool observing_ = false, haveBase_ = false, moved_ = false;
    std::array<Result, 2> results_;
    std::minstd_rand rng_{7};
};
} // namespace

struct VideoPresenter::Anticipation {
    CameraAnticipation* model = nullptr;
    bool enabled = false;
    AnticipationStage stage;
    diagnostics::GpuTimer tickTimer, pictureTimer;
    bool timersReady = false;
    MotionProfiles previous;
    bool previousValid = false;
    int64_t pictureTime = -1;                  // the engine's word on the next picture
    // The picture on the stage and how the last present drew it.
    bool running = false;
    int64_t pictureAt = 0, due = 0, lastCall = 0, target = 0;   // target: when the last draw reaches the screen
    bool locked = false;                       // the ticks follow the screen's refreshes
    float dims[16]{};
    unsigned clientW = 0, clientH = 0, bufferW = 0, bufferH = 0;
    std::array<double, 2> camera{};            // the simulated game's camera in that picture
    std::optional<CameraSimulation> simulation;
    std::unique_ptr<Bench> bench;
    // The last second, for the log and the stats card.
    std::vector<double> intervals, tickGpu, pictureGpu, tickCpu, measureWait, pictureAge;
    uint64_t ticks = 0, pictures = 0, missed = 0, undated = 0, timerId = 0, tickSample = 0, pictureSample = 0;
    int64_t windowStart = 0;
    AnticipationReport report;

    Anticipation() {
        const double lag = envNumber(L"VEYRA_TEST_ANTICIPATION_SIM", 0);
        if (lag > 0) {
            simulation.emplace(lag);
            log::info("anticipation", std::format("simulated game: the picture is a panorama, the camera follows the right stick {:.0f} ms late (test)", lag));
            if (envNumber(L"VEYRA_TEST_ANTICIPATION_BENCH", 0) > 0) bench = std::make_unique<Bench>();
        }
    }
    double periodMs(double refreshHz) const { return 1000.0 / std::clamp(refreshHz >= 1 ? refreshHz : 120.0, 30.0, 500.0); }
    // The screen's refreshes from DXGI's frame statistics, when they are fresh and whole; else none.
    static gfx::PresentSink::RefreshGrid grid(gfx::PresentSink::RefreshGrid g, int64_t now) {
        static const bool free = envNumber(L"VEYRA_TEST_ANTICIPATION_FREE_RUN", 0) > 0;
        const bool usable = !free && g.period100ns > 10000 && g.latch100ns > 0 && g.lastVblank100ns > 0 && now - g.lastVblank100ns < 5000000 && now > g.lastVblank100ns - 1000000;
        return usable ? g : gfx::PresentSink::RefreshGrid{};
    }
    void presented(int64_t call, bool tick) {
        if (lastCall && call > lastCall) intervals.push_back(milliseconds(call - lastCall));
        lastCall = call;
        tick ? ++ticks : ++pictures;
    }
    void collect(diagnostics::GpuTimer& timer, ID3D12Fence* fence, uint64_t& seen, std::vector<double>& into) {
        timer.collect(fence);
        const auto& last = timer.last();
        if (last.identity.sourceFrameId == seen) return;
        seen = last.identity.sourceFrameId;
        if (const auto ms = last.gpu[size_t(diagnostics::GpuStage::Blit)].milliseconds) into.push_back(*ms);
    }
};

void VideoPresenter::setAnticipation(CameraAnticipation* model) {
    if (!anticipation_) anticipation_ = std::make_shared<Anticipation>();
    anticipation_->model = model;
    if (model && anticipation_->bench) model->setScripted(true);
}
void VideoPresenter::enableAnticipation(bool on) {
    if (!anticipation_ || anticipation_->enabled == on) return;
    anticipation_->enabled = on;
    if (!on) { anticipation_->running = false; anticipation_->report = {}; }
    log::info("anticipation", on ? "on: each picture is redrawn every screen refresh, shifted by the stick" : "off");
}
bool VideoPresenter::anticipationEnabled() const { return anticipation_ && anticipation_->enabled; }
void VideoPresenter::anticipationPicture(int64_t pictureTime100ns) { if (anticipation_) anticipation_->pictureTime = pictureTime100ns; }
int64_t VideoPresenter::anticipationDue100ns() const { return anticipation_ && anticipation_->running ? anticipation_->due : 0; }
VideoPresenter::AnticipationReport VideoPresenter::anticipationReport() const { return anticipation_ ? anticipation_->report : AnticipationReport{}; }
void VideoPresenter::anticipationClose() {
    if (!anticipation_) return;
    auto& a = *anticipation_;
    a.running = false; a.previousValid = false; a.timersReady = false;
    a.stage.close(); a.tickTimer.close(); a.pictureTimer.close();
    if (a.model) a.model->discontinuity();
}

bool VideoPresenter::anticipationIngest(gfx::D3D12DeviceContext& ctx, pipeline::EnhanceGraph& graph, unsigned slot, bool generated) {
    auto* a = anticipation_.get();
    if (!a) return false;
    if (!a->enabled || !a->model) { a->running = false; return false; }
    auto* source = generated ? graph.generatedFrameResource(slot) : graph.videoFrameResource(slot);
    if (!source) return false;
    auto like = source->GetDesc();
    like.Format = graph.outputFormat();
    if (!a->stage.ensure(ctx.device(), like)) { a->running = false; return false; }
    if (!a->timersReady) { a->tickTimer.initialize(ctx.device(), presentationQueue_.Get()); a->pictureTimer.initialize(ctx.device(), presentationQueue_.Get()); a->timersReady = true; }
    const int64_t begin = host100ns();
    if (a->pictureTime <= 0) ++a->undated;
    const int64_t pictureTime = a->pictureTime > 0 ? a->pictureTime : begin - 300000;
    a->pictureTime = -1;
    Status status = Status::Ok; uint32_t commandSlot = 0;
    auto* list = presentationRing_.acquireNext(commandSlot, status);
    if (!list) return false;
    const unsigned index = a->stage.next();
    auto* target = a->stage.texture(index);
    pipeline::FrameIdentity timed{}; timed.sourceFrameId = ++a->timerId;
    a->pictureTimer.frame(timed, presentationFence_.Get());
    a->pictureTimer.mark(list, diagnostics::GpuStage::Blit);
    std::array<double, 2> camera{};
    if (a->simulation) {
        // The simulated game: the picture drawn as a panorama at the camera its time gives.
        camera = a->simulation->cameraAt(milliseconds(pictureTime), a->model->stick());
        transition(list, source, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        transition(list, target, AnticipationStage::kRest, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const auto rtv = a->stage.rtv(index);
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        const D3D12_VIEWPORT viewport{0, 0, float(like.Width), float(like.Height), 0, 1};
        const D3D12_RECT rect{0, 0, LONG(like.Width), LONG(like.Height)};
        list->RSSetViewports(1, &viewport); list->RSSetScissorRects(1, &rect);
        ID3D12DescriptorHeap* heaps[] = {pass_.heap.Get()};
        list->SetDescriptorHeaps(1, heaps); list->SetGraphicsRootSignature(pass_.rootSig.Get()); list->SetPipelineState(pass_.pso.Get());
        float dims[16] = {float(like.Width), float(like.Height), 1024.0f, 0.0f};
        dims[14] = float(camera[0] - std::floor(camera[0])); dims[15] = float(camera[1] - std::floor(camera[1]));
        list->SetGraphicsRoot32BitConstants(0, 16, dims, 0);
        list->SetGraphicsRootDescriptorTable(1, {pass_.heap->GetGPUDescriptorHandleForHeapStart().ptr + size_t(slot + (generated ? 2 : 0)) * pass_.increment});
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST); list->DrawInstanced(3, 1, 0, 0);
        transition(list, target, D3D12_RESOURCE_STATE_RENDER_TARGET, AnticipationStage::kRest);
        transition(list, source, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    } else {
        transition(list, source, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
        transition(list, target, AnticipationStage::kRest, D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyResource(target, source);
        transition(list, target, D3D12_RESOURCE_STATE_COPY_DEST, AnticipationStage::kRest);
        transition(list, source, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    }
    a->stage.recordLuma(list, index, graph.hdrOutput() && !graph.hdr10Output());
    a->pictureTimer.mark(list, diagnostics::GpuStage::Blit, true); a->pictureTimer.resolve(list);
    if (!presentationRing_.submitAndSignal(commandSlot)) return false;
    const uint64_t fenceValue = presentationRing_.lastSignaledValue();
    a->pictureTimer.submitted(fenceValue);
    // The luma copy is a few hundred microseconds of GPU behind the picture's own work, which the
    // caller has seen finished. Its answer decides how the picture joins the one on screen, so it
    // is waited for here, briefly; a picture that misses it is shown without a measurement.
    const auto waitStart = Clock::now();
    while (presentationFence_->GetCompletedValue() < fenceValue && Clock::now() - waitStart < std::chrono::microseconds(2000)) YieldProcessor();
    a->measureWait.push_back(std::chrono::duration<double, std::milli>(Clock::now() - waitStart).count());
    MotionProfiles current; MotionEstimate motion; const MotionEstimate* measured = nullptr;
    if (presentationFence_->GetCompletedValue() >= fenceValue && a->stage.readProfiles(current)) {
        if (a->previousValid) { motion = motionBetween(current, a->previous); measured = &motion; }
        a->previous = current; a->previousValid = true;
    } else { a->previousValid = false; ++a->missed; }
    const int64_t now = host100ns();
    a->model->picture(pictureTime, now, measured);
    a->pictureAge.push_back(milliseconds(now - pictureTime));
    static const bool trace = envNumber(L"VEYRA_TEST_ANTICIPATION_TRACE", 0) > 0;
    if (trace) {
        const auto s = a->model->stats();
        log::info("anticipation-trace", std::format("pictureMs={:.2f} ageMs={:.2f} measured={} dx={:.5f} cx={:.2f} expectedX={:.5f} residualX={:.5f} cameraX={:.5f} generated={}",
            milliseconds(pictureTime), milliseconds(now - pictureTime), measured != nullptr, motion.dx, motion.cx, s.expectedX, s.residualX, camera[0], generated));
    }
    a->stage.setCurrent(index); a->camera = camera; a->pictureAt = now;
    a->collect(a->pictureTimer, presentationFence_.Get(), a->pictureSample, a->pictureGpu);
    return true;
}

// Binds the stage's picture and the shift for a draw that starts now. `dims` are the blit's constants.
void VideoPresenter::anticipationBlit(ID3D12GraphicsCommandList* list, const float* dims) {
    auto& a = *anticipation_;
    const int64_t now = host100ns();
    const double period = a.periodMs(sink_.displayRefreshHz());
    if (a.bench) a.bench->step(milliseconds(now), *a.model, scanoutP50Ms_);
    // The stick is taken as held until the draw is on screen. With the screen's refreshes known
    // that is the first one this Present can still make, whatever its own phase: a picture drawn
    // between two ticks then moves by the right amount. Else one usual Present-to-scanout later.
    const auto grid = a.grid(sink_.refreshGrid(), now);
    a.target = grid.period100ns > 0
        ? grid.lastVblank100ns + int64_t(std::ceil(double(now + int64_t(grid.latch100ns) - grid.lastVblank100ns) / grid.period100ns) * grid.period100ns)
        : now + int64_t((scanoutP50Ms_ > 0 ? std::min(scanoutP50Ms_, 25.0) : period) * 10000.0);
    auto shift = a.model->shift(a.target);
    if (a.bench && !a.bench->warp()) shift = {};
    std::copy(dims, dims + 16, a.dims);
    a.dims[2] = dims[2] + 512.0f; a.dims[14] = shift.x; a.dims[15] = shift.y;
    ID3D12DescriptorHeap* heaps[] = {a.stage.blitHeap()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetGraphicsRoot32BitConstants(0, 16, a.dims, 0);
    list->SetGraphicsRootDescriptorTable(1, a.stage.blitSource(a.stage.current()));
}

// After every Present. staged: it drew the stage's picture (the picture's own present or a tick's),
// so the next refresh is due, and the bench reads what was drawn. Every Present counts in the
// second's intervals, anticipation or not: the log line is the pacing's before and after.
void VideoPresenter::anticipationPresented(int64_t call100ns, bool tick, const RECT& client, bool staged) {
    auto& a = *anticipation_;
    a.presented(call100ns, tick);
    if (!staged) a.running = false;   // this picture went straight to the screen: nothing to redraw
    if (staged) {
        const int64_t period = int64_t(a.periodMs(sink_.displayRefreshHz()) * 10000.0);
        const bool wasRunning = a.running;
        if (!tick) { a.clientW = unsigned(client.right); a.clientH = unsigned(client.bottom); a.bufferW = sink_.bufferWidth(); a.bufferH = sink_.bufferHeight(); a.running = true; }
        // The ticks are a steady grid: one the owner thread drew late does not push the ones after
        // it, and a picture's own present stands for the tick that would follow it within half a period.
        // With the screen's refreshes known the grid keeps one phase against them, half a period before
        // the last moment a Present still makes a refresh: a free-running grid drifts through that
        // moment, and then some refreshes get two draws and some none.
        const auto grid = a.grid(sink_.refreshGrid(), call100ns);
        a.locked = grid.period100ns > 0;
        if (a.locked) {
            const double phase = double(grid.lastVblank100ns) - grid.latch100ns - grid.period100ns / 2;
            a.due = int64_t(phase + std::ceil((double(call100ns) + grid.period100ns / 2 - phase) / grid.period100ns) * grid.period100ns);
        } else if (!wasRunning || a.due + 4 * period < call100ns) a.due = call100ns + period;
        else { if (tick) a.due += period; while (a.due < call100ns + period / 2) a.due += period; }
        if (a.bench && a.simulation) {
            const double now = milliseconds(call100ns), screen = milliseconds(a.target);
            const double held = a.model->stick().sum(now, screen, [](const StickHistory::Entry& e) { return CameraSimulation::curve(e.x); }) / 1000.0 * CameraSimulation::kGainX;
            a.bench->drawn(now, screen, a.camera[0] - double(a.dims[14]), a.simulation->idealAt(now, a.model->stick())[0] + held);
        }
    }
    if (!a.model) return;
    if (!a.windowStart) a.windowStart = call100ns;
    if (call100ns - a.windowStart < 10000000) return;
    const double seconds = double(call100ns - a.windowStart) / 1e7;
    const auto stats = a.model->stats();
    auto& r = a.report;
    r.enabled = a.enabled; r.running = a.running;
    r.presentsPerSecond = double(a.intervals.size()) / seconds;
    r.intervalP50Ms = percentile(a.intervals, 0.5); r.intervalP95Ms = percentile(a.intervals, 0.95); r.intervalP99Ms = percentile(a.intervals, 0.99);
    r.gpuTickMs = percentile(a.tickGpu, 0.5); r.gpuPictureMs = percentile(a.pictureGpu, 0.5);
    r.cpuTickMs = percentile(a.tickCpu, 0.5); r.measureWaitMs = percentile(a.measureWait, 0.5);
    r.ticks = a.ticks; r.pictures = a.pictures; r.missedMeasures = a.missed;
    log::info("anticipation", std::format("on={} locked={} presents={:.1f}/s pictures={} ticks={} intervalP50Ms={:.2f} intervalP95Ms={:.2f} intervalP99Ms={:.2f} refreshHz={:.1f} gpuTickMs={:.3f} gpuPictureMs={:.3f} cpuTickMs={:.3f} measureWaitMs={:.3f} missedMeasures={} pictureAgeMs={:.1f} undated={} confidence={:.2f} lagMs={:.1f} gain={:.3f}/{:.3f} learnt={} shift={:.4f}/{:.4f} (one second; locked = the ticks follow the screen's refreshes; intervals between Present calls, pictures and ticks together; GPU times are queue timestamps of the tick's blit and of the picture's copy and luma; pictureAge from the host having the picture to its draw here, undated = pictures whose host time was not known; shift in picture fractions)",
        a.running, a.locked, r.presentsPerSecond, a.pictures, a.ticks, r.intervalP50Ms, r.intervalP95Ms, r.intervalP99Ms, sink_.displayRefreshHz(), r.gpuTickMs, r.gpuPictureMs, r.cpuTickMs, r.measureWaitMs, a.missed, percentile(a.pictureAge, 0.5), a.undated,
        stats.confidence, stats.lagMs, stats.gainX, stats.gainY, stats.learnt, stats.shiftX, stats.shiftY));
    a.intervals.clear(); a.tickGpu.clear(); a.pictureGpu.clear(); a.tickCpu.clear(); a.measureWait.clear(); a.pictureAge.clear();
    a.ticks = a.pictures = a.missed = a.undated = 0; a.windowStart = call100ns;
}

bool VideoPresenter::anticipationTick(gfx::D3D12DeviceContext& ctx) {
    auto* a = anticipation_.get();
    if (!a || !a->running) return false;
    const int64_t now = host100ns();
    if (now < a->due) return false;
    // No picture for a second (a stall, the end): the stage stops until the next one.
    if (!a->enabled || now - a->pictureAt > 10000000) { a->running = false; return false; }
    const int64_t retry = now + int64_t(a->periodMs(sink_.displayRefreshHz()) * 10000.0);
    RECT client{};
    // A resize, a move or another presentation path is the next picture's present to sort out.
    if (!window_ || !GetClientRect(window_, &client) || unsigned(client.right) != a->clientW || unsigned(client.bottom) != a->clientH ||
        sink_.bufferWidth() != a->bufferW || sink_.bufferHeight() != a->bufferH || sink_.xess() || copyBeforePresent_ ||
        GetPropW(window_, L"Veyra.InteractiveMove") || GetPropW(window_, L"Veyra.DpiTransition") || FAILED(ctx.device()->GetDeviceRemovedReason())) { a->due = retry; return false; }
    if (!sink_.presentationReady()) { a->due = now + 5000; return false; }
    const auto begin = Clock::now();
    Status status = Status::Ok; uint32_t commandSlot = 0;
    auto* list = presentationRing_.acquireNext(commandSlot, status);
    if (!list) { a->due = retry; return false; }
    uint32_t backBufferIndex = 0;
    auto* backBuffer = sink_.currentBackBuffer(&backBufferIndex);
    if (!backBuffer) { presentationRing_.discardRecording(); a->due = retry; return false; }
    const bool timed = (a->ticks & 7) == 0;
    if (timed) {
        pipeline::FrameIdentity id{}; id.sourceFrameId = ++a->timerId;
        a->tickTimer.frame(id, presentationFence_.Get());
        a->tickTimer.mark(list, diagnostics::GpuStage::Blit);
    }
    transition(list, backBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv{rtvs_->GetCPUDescriptorHandleForHeapStart().ptr + size_t(backBufferIndex) * inc_};
    const float black[4] = {0, 0, 0, 1};
    list->ClearRenderTargetView(rtv, black, 0, nullptr); list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    const D3D12_VIEWPORT viewport{0, 0, float(sink_.bufferWidth()), float(sink_.bufferHeight()), 0, 1};
    const D3D12_RECT rect{0, 0, LONG(sink_.bufferWidth()), LONG(sink_.bufferHeight())};
    list->RSSetViewports(1, &viewport); list->RSSetScissorRects(1, &rect);
    list->SetGraphicsRootSignature(pass_.rootSig.Get()); list->SetPipelineState(pass_.pso.Get());
    float dims[16]; std::copy(a->dims, a->dims + 16, dims); dims[2] -= 512.0f;
    anticipationBlit(list, dims);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST); list->DrawInstanced(3, 1, 0, 0);
    transition(list, backBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    if (timed) { a->tickTimer.mark(list, diagnostics::GpuStage::Blit, true); a->tickTimer.resolve(list); }
    if (!presentationRing_.submitAndSignal(commandSlot)) { a->running = false; return false; }
    if (timed) a->tickTimer.submitted(presentationRing_.lastSignaledValue());
    lastBuffer_ = backBufferIndex;
    const int64_t call = host100ns();
    const bool presented = sink_.present(status);
    a->tickCpu.push_back(std::chrono::duration<double, std::milli>(Clock::now() - begin).count());
    a->collect(a->tickTimer, presentationFence_.Get(), a->tickSample, a->tickGpu);
    if (!presented) { a->running = false; return false; }
    anticipationPresented(call, true, client, true);
    return true;
}

} // namespace veyra::engine
