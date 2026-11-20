#include "GuestVideo.h"

#include "veyra/Log.h"
#include "veyra/pipeline/GpuPassUtils.h"
#include "veyra/sink/NvencLiveEncoder.h"
#include "veyra/sink/RecordingAudioTap.h"

#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <format>
#include <mutex>
#include <thread>

namespace veyra::guest {
using Microsoft::WRL::ComPtr;
namespace {

constexpr unsigned kSlots = 3;
constexpr uint32_t kGpuWaitMs = 1000;
constexpr uint32_t kRealFps = 60;   // real pictures: at most this many a second, as before

uint32_t adapterVendor(ID3D12Device* device) {
    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return 0;
    ComPtr<IDXGIAdapter1> adapter;
    if (FAILED(factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&adapter)))) return 0;
    DXGI_ADAPTER_DESC1 desc{};
    return SUCCEEDED(adapter->GetDesc1(&desc)) ? desc.VendorId : 0;
}

std::string narrow(const std::wstring& text) {
    std::string out;
    for (wchar_t c : text) out.push_back(c < 128 ? char(c) : '?');
    return out;
}

} // namespace

// One encoder session for one device, picture and stream size. The engine thread converts
// into its slots; the encoder thread opens NVENC, encodes and hands bitstreams to the sink.
struct GuestVideo::Pipe {
    ComPtr<ID3D12Device> device;
    uint32_t outW = 0, outH = 0, srcW = 0, srcH = 0, hdrMode = 0;
    std::atomic<uint32_t> kbps{20000}, fps{60};
    Sink sink;

    pipeline::ComputePass convert;
    pipeline::DescriptorStager stager;
    pipeline::StateTracker states;
    ComPtr<ID3D12Resource> y, uv;
    struct Slot {
        ComPtr<ID3D12Resource> nv12;
        std::atomic<int> state{0};   // 0 free, 1 recorded, 2 queued
        ComPtr<ID3D12Fence> fence;
        uint64_t value = 0;
        int64_t captureUs = 0, sourceUs = 0;
        bool presented = false;
    };
    std::array<Slot, kSlots> slots;
    int pending = -1;

    std::thread thread;
    std::mutex jobMutex;
    std::condition_variable jobCv;
    std::deque<unsigned> jobs;
    bool closed = false;
    std::atomic<bool> ready{false}, finished{false}, keyframe{true};
    std::atomic<uint64_t> encoded{0};
    mutable std::mutex textMutex;
    std::string error, encoderName;

    ~Pipe() { if (thread.joinable()) thread.join(); }

    bool matches(ID3D12Device* d, const Frame& frame, uint32_t w, uint32_t h) const {
        return d == device.Get() && frame.width == srcW && frame.height == srcH && frame.hdrMode == hdrMode && w == outW && h == outH;
    }
    // Every slot's commands have run and the thread is gone: the memory can go.
    bool done() const {
        if (!finished.load() || pending >= 0) return false;
        for (const auto& slot : slots)
            if (slot.fence && slot.fence->GetCompletedValue() < slot.value) return false;
        return true;
    }
    void close() {
        { std::lock_guard lock(jobMutex); closed = true; }
        jobCv.notify_all();
    }
    void fail(std::string reason) {
        veyra::log::error("guest-video", reason);
        { std::lock_guard lock(textMutex); if (error.empty()) error = std::move(reason); }
        close();
    }

    // Engine thread.
    bool create(std::string& reason) {
        std::vector<uint8_t> shader;
        if (!convert.loadShader("GuestNv12.dxil", shader) || !convert.create(device.Get(), shader, kSlots + 2, 1, 2, 8, 0, true)) {
            reason = "guest conversion shader"; return false;
        }
        if (!stager.initialize(device.Get(), kSlots)) { reason = "guest descriptors"; return false; }
        y = pipeline::makeTexture(device.Get(), outW, outH, DXGI_FORMAT_R8_UNORM, true);
        uv = pipeline::makeTexture(device.Get(), outW / 2, outH / 2, DXGI_FORMAT_R8G8_UNORM, true);
        if (!y || !uv) { reason = "guest video memory"; return false; }
        pipeline::makeUav(device.Get(), y.Get(), DXGI_FORMAT_R8_UNORM, pipeline::cpuHandleOf(convert, kSlots));
        pipeline::makeUav(device.Get(), uv.Get(), DXGI_FORMAT_R8G8_UNORM, pipeline::cpuHandleOf(convert, kSlots + 1));
        for (auto& slot : slots) {
            slot.nv12 = pipeline::makeTexture(device.Get(), outW, outH, DXGI_FORMAT_NV12, false);
            if (!slot.nv12) { reason = "guest video memory"; return false; }
        }
        thread = std::thread([this] { run(); });
        return true;
    }
    bool capture(ID3D12GraphicsCommandList* list, const Frame& frame, int64_t nowUs, bool presented) {
        if (!ready.load()) return false;
        int index = -1;
        for (unsigned i = 0; i < kSlots; ++i) { int expected = 0; if (slots[i].state.compare_exchange_strong(expected, 1)) { index = int(i); break; } }
        if (index < 0) return false;
        auto& slot = slots[size_t(index)];
        slot.captureUs = nowUs;
        slot.sourceUs = frame.sourceHost100ns / 10;
        slot.presented = presented;
        D3D12_SHADER_RESOURCE_VIEW_DESC view{}; view.Format = frame.format; view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; view.Texture2D.MipLevels = 1;
        stager.stageSrv(frame.texture, &view, convert.heap.Get(), UINT(index));
        D3D12_RESOURCE_BARRIER source{}; source.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; source.Transition.pResource = frame.texture;
        source.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        source.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON; source.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        list->ResourceBarrier(1, &source);
        states.transition(list, y.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        states.transition(list, uv.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        // No output dither: noise is what a low-latency CBR encoder can least afford.
        const float constants[8] = {std::bit_cast<float>(outW), std::bit_cast<float>(outH), std::bit_cast<float>(hdrMode), 0, 0, 0, 0, 0};
        convert.bind(list, constants, pipeline::gpuHandleOf(convert, UINT(index)).ptr, pipeline::gpuHandleOf(convert, kSlots).ptr);
        list->Dispatch((outW + 15) / 16, (outH + 15) / 16, 1);
        states.uavBarrier(list, y.Get()); states.uavBarrier(list, uv.Get());
        states.transition(list, y.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
        states.transition(list, uv.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
        std::swap(source.Transition.StateBefore, source.Transition.StateAfter);
        list->ResourceBarrier(1, &source);
        D3D12_RESOURCE_BARRIER target{}; target.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; target.Transition.pResource = slot.nv12.Get();
        target.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        target.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON; target.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        list->ResourceBarrier(1, &target);
        for (UINT plane = 0; plane < 2; ++plane) {
            D3D12_TEXTURE_COPY_LOCATION from{}, to{};
            from.pResource = plane ? uv.Get() : y.Get(); from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            to.pResource = slot.nv12.Get(); to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; to.SubresourceIndex = plane;
            list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        }
        std::swap(target.Transition.StateBefore, target.Transition.StateAfter);
        list->ResourceBarrier(1, &target);
        pending = index;
        return true;
    }
    void submitted(ID3D12Fence* fence, uint64_t value) {
        if (pending < 0) return;
        auto& slot = slots[size_t(pending)]; pending = -1;
        slot.fence = fence; slot.value = value; slot.state = 2;
        { std::lock_guard lock(jobMutex); jobs.push_back(unsigned(&slot - slots.data())); }
        jobCv.notify_one();
    }
    void discarded() {
        if (pending < 0) return;
        slots[size_t(pending)].state = 0; pending = -1;
    }

    // Encoder thread.
    void run() {
        SetThreadDescription(GetCurrentThread(), L"Guest video");
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
        encode();
        ready = false;
        finished = true;
    }
    void encode() {
        sink::NvencLiveEncoder encoder;
        sink::NvencLiveEncoder::Config config;
        config.width = outW; config.height = outH; config.fps = fps.load();
        config.lowLatency = true; config.bitrateKbps = kbps.load();
        std::vector<ID3D12Resource*> inputs;
        for (auto& slot : slots) inputs.push_back(slot.nv12.Get());
        if (!encoder.open(device.Get(), config, inputs)) { fail("NVENC: " + narrow(encoder.lastError())); return; }
        uint32_t appliedKbps = config.bitrateKbps, appliedFps = config.fps;
        { std::lock_guard lock(textMutex); encoderName = narrow(encoder.describe()); }
        veyra::log::info("guest-video", std::format("encoder ready {}x{} from {}x{} hdr={} fps={} cbrKbps={}", outW, outH, srcW, srcH, hdrMode, appliedFps, appliedKbps));
        ready = true;
        for (;;) {
            unsigned index = 0;
            {
                std::unique_lock lock(jobMutex);
                jobCv.wait(lock, [&] { return !jobs.empty() || closed; });
                if (closed) break;
                // Behind: keep the newest frame only, the guest wants now, not then.
                while (jobs.size() > 1) { slots[jobs.front()].fence.Reset(); slots[jobs.front()].state = 0; jobs.pop_front(); }
                index = jobs.front(); jobs.pop_front();
            }
            if (const uint32_t want = kbps.load(), wantFps = fps.load(); want != appliedKbps || wantFps != appliedFps) {
                if (encoder.setRate(want, wantFps)) veyra::log::info("guest-video", std::format("rate {} kbps {} fps -> {} kbps {} fps", appliedKbps, appliedFps, want, wantFps));
                appliedKbps = want; appliedFps = wantFps;
            }
            auto& slot = slots[index];
            const bool key = keyframe.exchange(false);
            const int64_t sourceUs = slot.sourceUs;   // one frame in flight: the packet below is this slot's
            const bool presented = slot.presented;
            if (!encoder.submit(index, slot.fence.Get(), slot.value, slot.captureUs, key)) { fail("NVENC submit: " + narrow(encoder.lastError())); break; }
            sink::NvencLiveEncoder::Packet packet; bool done = false;
            if (!encoder.takeOldest(packet, true, kGpuWaitMs, done) || !done) { fail("NVENC output: " + narrow(encoder.lastError())); break; }
            const int64_t encodedUs = sink::recordingClockUs();
            slots[packet.input].fence.Reset(); slots[packet.input].state = 0;
            ++encoded;
            EncodedFrame frame{std::move(packet.data), packet.key, packet.ptsUs, sourceUs, encodedUs, outW, outH, presented};
            sink(std::move(frame));
        }
        encoder.close();
    }
};

struct GuestVideo::State {
    Sink sink;
    std::mutex m;
    std::atomic<bool> enabled{false}, presented{false};
    uint32_t height = 1080, kbps = 20000, fps = kRealFps;
    // Real pictures: the frame cap as a bucket of two pictures: a live source's pictures arrive
    // unevenly (network, decode) and each one is wanted; a faster source still averages kRealFps.
    int64_t creditAtUs = 0;
    double credit = 2;
    // Presented frames: the next slot of the fps grid. A frame is taken from a quarter interval
    // before its slot, so a 120 Hz output halves evenly to 60 and an uneven one stays near the grid.
    int64_t dueUs = 0;
    std::shared_ptr<Pipe> current, pendingPipe;
    std::vector<std::shared_ptr<Pipe>> retired;
    std::atomic<uint64_t> skipped{0};
    std::string error;
    bool vendorChecked = false;

    void retire(std::shared_ptr<Pipe>& pipe) {
        if (!pipe) return;
        pipe->close();
        retired.push_back(std::move(pipe));
    }
    void reap() {
        std::erase_if(retired, [](const std::shared_ptr<Pipe>& p) { return p->done(); });
    }
};

GuestVideo::GuestVideo(Sink sink) : s_(std::make_unique<State>()) { s_->sink = std::move(sink); }

GuestVideo::~GuestVideo() {
    std::unique_lock lock(s_->m);
    s_->enabled = false;
    s_->retire(s_->current);
    // The presenter is gone or no longer calls in: wait for the GPU and the threads.
    for (int i = 0; i < 300 && !s_->retired.empty(); ++i) {
        s_->reap();
        if (s_->retired.empty()) break;
        lock.unlock(); std::this_thread::sleep_for(std::chrono::milliseconds(10)); lock.lock();
    }
    for (auto& p : s_->retired) p->pending = -1;
}

void GuestVideo::setQuality(uint32_t height, uint32_t fps, bool presented, uint32_t kbps) {
    std::lock_guard lock(s_->m);
    s_->height = std::clamp(height, 360u, 2160u);
    s_->fps = presented ? std::clamp(fps, 10u, 240u) : kRealFps;
    if (s_->presented != presented) { s_->presented = presented; s_->dueUs = 0; }
    s_->kbps = std::clamp(kbps, 1000u, 200000u);
    if (s_->current) { s_->current->kbps = s_->kbps; s_->current->fps = s_->fps; }
}

void GuestVideo::setBitrate(uint32_t kbps) {
    std::lock_guard lock(s_->m);
    s_->kbps = std::clamp(kbps, 1000u, 200000u);
    if (s_->current) s_->current->kbps = s_->kbps;
}

void GuestVideo::setEnabled(bool on) {
    std::lock_guard lock(s_->m);
    if (s_->enabled == on) return;
    s_->enabled = on;
    if (!on) s_->retire(s_->current);
    s_->error.clear();
}

void GuestVideo::requestKeyframe() {
    std::lock_guard lock(s_->m);
    if (s_->current) s_->current->keyframe = true;
}

VideoStats GuestVideo::stats() const {
    std::lock_guard lock(s_->m);
    VideoStats out;
    out.skipped = s_->skipped.load();
    out.error = s_->error;
    if (const auto& p = s_->current) {
        out.encoded = p->encoded.load();
        out.width = p->outW; out.height = p->outH; out.sourceHeight = p->srcH;
        std::lock_guard text(p->textMutex);
        out.encoder = p->encoderName;
        if (out.error.empty()) out.error = p->error;
    }
    return out;
}

bool GuestVideo::wanted() const {
    auto& s = *s_;
    if (!s.enabled.load(std::memory_order_relaxed)) return false;
    std::lock_guard lock(s.m);
    if (s.credit + double(sink::recordingClockUs() - s.creditAtUs) * kRealFps / 1e6 < 1) return false;
    // An encoder still opening takes nothing; one that stopped is reported by record().
    return !s.current || s.current->ready.load() || s.current->finished.load();
}

bool GuestVideo::presentedFeed() const { return s_->presented.load(std::memory_order_relaxed); }

bool GuestVideo::record(ID3D12GraphicsCommandList* list, const Frame& frame) {
    auto& s = *s_;
    std::lock_guard lock(s.m);
    if (s.pendingPipe) { s.pendingPipe->discarded(); s.pendingPipe.reset(); }
    s.reap();
    const bool presented = s.presented.load();
    if (!s.enabled || !list || !frame.texture || (frame.generated && !presented)) return false;
    if (s.current && !s.current->ready.load() && s.current->finished.load()) {
        // The encoder failed: report it and stop converting frames for nothing.
        std::lock_guard text(s.current->textMutex);
        s.error = s.current->error.empty() ? std::string("guest encoder stopped") : s.current->error;
        s.enabled = false;
        s.retire(s.current);
        return false;
    }
    const int64_t now = sink::recordingClockUs();
    if (presented) {
        const int64_t interval = 1000000 / s.fps;
        if (now < s.dueUs - interval / 4) return false;
        s.dueUs = now - s.dueUs > interval ? now + interval : s.dueUs + interval;
    } else {
        s.credit = std::min(2.0, s.credit + double(now - s.creditAtUs) * kRealFps / 1e6);
        s.creditAtUs = now;
        if (s.credit < 1) return false;
        s.credit -= 1;
    }

    ComPtr<ID3D12Device> device;
    if (FAILED(frame.texture->GetDevice(IID_PPV_ARGS(&device)))) return false;
    if (!s.vendorChecked) {
        s.vendorChecked = true;
        if (adapterVendor(device.Get()) != 0x10DE) { s.error = "le partage a besoin d'une carte nvidia (nvenc)"; s.enabled = false; return false; }
    }
    const uint32_t outH = std::min(s.height, frame.height) & ~1u;
    const uint32_t outW = uint32_t(std::lround(double(frame.width) * outH / std::max(1u, frame.height))) & ~1u;
    if (outW < 64 || outH < 64) return false;
    if (s.current && !s.current->matches(device.Get(), frame, outW, outH)) {
        veyra::log::info("guest-video", std::format("picture {}x{} hdr={} -> {}x{} hdr={}, stream {}x{} -> {}x{}; new encoder", s.current->srcW, s.current->srcH, s.current->hdrMode, frame.width, frame.height, frame.hdrMode, s.current->outW, s.current->outH, outW, outH));
        s.retire(s.current);
    }
    if (!s.current) {
        if (s.retired.size() >= 3) return false;   // previous encoders still closing
        auto pipe = std::make_shared<Pipe>();
        pipe->device = device; pipe->srcW = frame.width; pipe->srcH = frame.height; pipe->hdrMode = frame.hdrMode;
        pipe->outW = outW; pipe->outH = outH; pipe->fps = s.fps; pipe->kbps = s.kbps; pipe->sink = s.sink;
        std::string reason;
        if (!pipe->create(reason)) { s.error = reason; s.enabled = false; veyra::log::error("guest-video", reason); return false; }
        s.current = std::move(pipe);
    }
    if (!s.current->capture(list, frame, now, presented)) { if (s.current->ready.load()) ++s.skipped; return false; }
    s.pendingPipe = s.current;
    return true;
}

void GuestVideo::submitted(ID3D12Fence* fence, uint64_t value) {
    std::lock_guard lock(s_->m);
    if (s_->pendingPipe) { s_->pendingPipe->submitted(fence, value); s_->pendingPipe.reset(); }
}

void GuestVideo::discarded() {
    std::lock_guard lock(s_->m);
    if (s_->pendingPipe) { s_->pendingPipe->discarded(); s_->pendingPipe.reset(); }
}

} // namespace veyra::guest
