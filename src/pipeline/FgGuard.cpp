#include "veyra/pipeline/FgGuard.h"

#include "veyra/Log.h"

#include <bit>
#include <cstring>
#include <format>

namespace veyra::pipeline {
namespace {
float bits(uint32_t v) { return std::bit_cast<float>(v); }
// A sample is "explained" below this colour difference (encoded 0..1), and two moved frames
// "agree" below this mean difference; both above the stream's coding noise.
constexpr float kExplainLimit = 0.08f, kAgreeLimit = 24.0f / 255.0f;
// Global-motion blending from this many pixels of motion a pair, at 1080p: below it the
// generators are already right, and the bilinear resampling would only soften (FSR 3.1, 2.4 px).
constexpr float kMinSpeed1080 = 4.0f;
}  // namespace

bool FgGuard::create(const Desc& desc)
{
    desc_ = desc;
    overlays_ = desc.overlays;
    motion_ = desc.motion;
    if (!overlays_ && !motion_) return true;
    auto* device = desc.device;
    const uint32_t w = desc.width, h = desc.height;
    score_ = makeTexture(device, w, h, DXGI_FORMAT_R16_FLOAT, true);
    stats_ = makeTexture(device, 2, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, true);
    dummyFlow_ = makeTexture(device, 1, 1, DXGI_FORMAT_R16G16_FLOAT, false);
    if (!score_ || !stats_ || !dummyFlow_) return false;
    if (overlays_)
        for (auto& texture : hudless_)
            if (!(texture = makeTexture(device, w, h, desc.colorFormat, true))) return false;
    std::vector<uint8_t> cs;
    if (!detect_.loadShader("FgHudDetect.dxil", cs) || !detect_.create(device, cs, 8, 3, 1, 8, 0, true)) return false;
    if (!inpaint_.loadShader("FgHudInpaint.dxil", cs) || !inpaint_.create(device, cs, 8, 3, 1)) return false;
    if (!stats_pass_.loadShader("FgStats.dxil", cs) || !stats_pass_.create(device, cs, 8, 3, 1, 8, 0, true)) return false;
    if (!finalize_.loadShader("FgFinalize.dxil", cs) || !finalize_.create(device, cs, desc.generatedCount * 7, 6, 1, 8, 0, true)) return false;
    auto srv = [&](ID3D12Resource* resource, DXGI_FORMAT format, ComputePass& pass, UINT slot) {
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = format;
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Texture2D.MipLevels = 1;
        desc.stager->stageSrv(resource, &view, pass.heap.Get(), slot);
    };
    auto uav = [&](ID3D12Resource* resource, DXGI_FORMAT format, ComputePass& pass, UINT slot) {
        makeUav(device, resource, format, {pass.heap->GetCPUDescriptorHandleForHeapStart().ptr + slot * pass.increment});
    };
    const auto colour = desc.colorFormat;
    auto* flow = desc.flow ? desc.flow : dummyFlow_.Get();
    for (unsigned p = 0; p < 2; ++p) {
        auto* a = desc.real[1 - p];
        auto* b = desc.real[p];
        srv(a, colour, detect_, p * 4); srv(b, colour, detect_, p * 4 + 1); srv(flow, DXGI_FORMAT_R16G16_FLOAT, detect_, p * 4 + 2);
        uav(score_.Get(), DXGI_FORMAT_R16_FLOAT, detect_, p * 4 + 3);
        if (overlays_) {
            srv(b, colour, inpaint_, p * 4); srv(score_.Get(), DXGI_FORMAT_R16_FLOAT, inpaint_, p * 4 + 1); srv(a, colour, inpaint_, p * 4 + 2);
            uav(hudless_[p].Get(), colour, inpaint_, p * 4 + 3);
        }
        srv(a, colour, stats_pass_, p * 4); srv(b, colour, stats_pass_, p * 4 + 1); srv(flow, DXGI_FORMAT_R16G16_FLOAT, stats_pass_, p * 4 + 2);
        uav(stats_.Get(), DXGI_FORMAT_R32G32B32A32_FLOAT, stats_pass_, p * 4 + 3);
    }
    for (unsigned g = 0; g < desc.generatedCount; ++g) {
        const unsigned p = g % 2, base = g * 7;
        srv(generatorInput(1 - p), colour, finalize_, base); srv(generatorInput(p), colour, finalize_, base + 1);
        srv(desc.real[1 - p], colour, finalize_, base + 2); srv(desc.real[p], colour, finalize_, base + 3);
        srv(score_.Get(), DXGI_FORMAT_R16_FLOAT, finalize_, base + 4); srv(stats_.Get(), DXGI_FORMAT_R32G32B32A32_FLOAT, finalize_, base + 5);
        uav(desc.generated[g].Get(), colour, finalize_, base + 6);
    }
    D3D12_QUERY_HEAP_DESC qd{};
    qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qd.Count = kSlots * kQueriesPerSlot;
    if (FAILED(device->CreateQueryHeap(&qd, IID_PPV_ARGS(&queries_)))) return false;
    UINT64 frequency = 0;
    if (desc.queue && SUCCEEDED(desc.queue->GetTimestampFrequency(&frequency)) && frequency) ticksPerMs_ = double(frequency) / 1000.0;
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = kStatsBytes + kQueriesPerSlot * 8; bd.Height = 1; bd.DepthOrArraySize = 1;
    bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    for (auto& slot : slots_)
        if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&slot.readback))))
            return false;
    viewMask_ = GetEnvironmentVariableW(L"VEYRA_TEST_FG_GUARD_VIEW", nullptr, 0) > 0;
    created_ = true;
    log::info("fg-guard", std::format("on {}x{} overlays={} motion={} flow={}", w, h, overlays_, motion_, desc.flow != nullptr));
    return true;
}

void FgGuard::timestamp(ID3D12GraphicsCommandList* list, unsigned index)
{
    if (recording_) list->EndQuery(queries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, current_ * kQueriesPerSlot + index);
}

void FgGuard::analyze(ID3D12GraphicsCommandList* list, StateTracker& tracker, unsigned parity, bool historyValid, bool haveFlow)
{
    if (!created_) return;
    collect();
    recording_ = false;
    for (unsigned i = 0; i < kSlots; ++i) {
        const unsigned k = (current_ + 1 + i) % kSlots;
        if (slots_[k].state == FrameSlot::State::Free) { current_ = k; recording_ = true; break; }
    }
    if (recording_) { slots_[current_].state = FrameSlot::State::Recording; slots_[current_].finalizes = 0; }
    timestamp(list, 0);
    const uint32_t w = desc_.width, h = desc_.height;
    auto* a = desc_.real[1 - parity];
    auto* b = desc_.real[parity];
    tracker.transition(list, a, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    tracker.transition(list, b, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    auto heapAt = [](const ComputePass& pass, UINT slot) { return pass.heap->GetGPUDescriptorHandleForHeapStart().ptr + slot * pass.increment; };
    const bool flowValid = historyValid && haveFlow && desc_.flow;
    if (flowValid) tracker.transition(list, desc_.flow, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    // Overlays are looked for whenever either half is on: the hold shows real frames, overlays and all.
    tracker.transition(list, score_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const uint32_t cw = w / 5, ch = h / 5;
    const float dc[8] = {bits(w), bits(h), bits((w - cw) / 2), bits((h - ch) / 2), bits((w + cw) / 2), bits((h + ch) / 2),
                         bits(historyValid ? 0u : 1u), bits(flowValid ? 1u : 0u)};
    detect_.bind(list, dc, heapAt(detect_, parity * 4), heapAt(detect_, parity * 4 + 3));
    list->Dispatch((w + 15) / 16, (h + 15) / 16, 1);
    tracker.uavBarrier(list, score_.Get());
    tracker.transition(list, score_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (overlays_) {
        tracker.transition(list, hudless_[parity].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        const float ic[8] = {bits(w), bits(h), 0, 0, 0, 0, 0, 0};
        inpaint_.bind(list, ic, heapAt(inpaint_, parity * 4), heapAt(inpaint_, parity * 4 + 3));
        list->Dispatch((w + 15) / 16, (h + 15) / 16, 1);
        tracker.uavBarrier(list, hudless_[parity].Get());
        tracker.transition(list, hudless_[parity].Get(), D3D12_RESOURCE_STATE_COMMON);
    }
    tracker.transition(list, stats_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const uint32_t flags = (motion_ && historyValid ? 3u : 0u) | (flowValid ? 4u : 0u);
    const float sc[8] = {bits(w), bits(h), bits(flags), kMinSpeed1080 * float(h) / 1080.0f, kExplainLimit, 0, 0, 0};
    stats_pass_.bind(list, sc, heapAt(stats_pass_, parity * 4), heapAt(stats_pass_, parity * 4 + 3));
    list->Dispatch(1, 1, 1);
    tracker.uavBarrier(list, stats_.Get());
    tracker.transition(list, stats_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    // The presentation queue takes real frames in COMMON.
    tracker.transition(list, a, D3D12_RESOURCE_STATE_COMMON);
    tracker.transition(list, b, D3D12_RESOURCE_STATE_COMMON);
    timestamp(list, 1);
}

void FgGuard::finalize(ID3D12GraphicsCommandList* list, StateTracker& tracker, unsigned slot, unsigned parity, float t)
{
    if (!created_ || slot >= desc_.generatedCount) return;
    auto& frame = slots_[current_];
    const unsigned index = recording_ && frame.finalizes < 8 ? 2 + 2 * frame.finalizes++ : 0;
    if (index) timestamp(list, index);
    const uint32_t w = desc_.width, h = desc_.height;
    auto* generated = desc_.generated[slot].Get();
    for (auto* r : {generatorInput(0), generatorInput(1), desc_.real[0], desc_.real[1]}) tracker.transition(list, r, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    tracker.transition(list, generated, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const float fc[8] = {bits(w), bits(h), t, bits((overlays_ ? 1u : 0u) | (motion_ ? 2u : 0u) | (viewMask_ ? 4u : 0u)), kAgreeLimit, 0, 0, 0};
    const auto base = finalize_.heap->GetGPUDescriptorHandleForHeapStart().ptr + slot * 7 * finalize_.increment;
    finalize_.bind(list, fc, base, base + 6 * finalize_.increment);
    list->Dispatch((w + 15) / 16, (h + 15) / 16, 1);
    tracker.uavBarrier(list, generated);
    for (auto* r : {generatorInput(0), generatorInput(1), desc_.real[0], desc_.real[1]}) tracker.transition(list, r, D3D12_RESOURCE_STATE_COMMON);
    (void)parity;
    if (index) timestamp(list, index + 1);
}

void FgGuard::endFrame(ID3D12GraphicsCommandList* list, StateTracker& tracker)
{
    if (!created_ || !recording_) return;
    auto& frame = slots_[current_];
    tracker.transition(list, stats_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = frame.readback.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R32G32B32A32_FLOAT, 2, 1, 1, kStatsBytes};
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = stats_.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    tracker.transition(list, stats_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    list->ResolveQueryData(queries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, current_ * kQueriesPerSlot, 2 + 2 * frame.finalizes, frame.readback.Get(), kStatsBytes);
    frame.state = FrameSlot::State::Resolved;
    recording_ = false;
}

void FgGuard::submitted(ID3D12Fence* fence, uint64_t value)
{
    for (auto& slot : slots_)
        if (slot.state == FrameSlot::State::Resolved) { slot.fence = fence; slot.value = value; slot.state = FrameSlot::State::Submitted; }
}

void FgGuard::collect()
{
    for (auto& slot : slots_) {
        if (slot.state == FrameSlot::State::Recording) slot.state = FrameSlot::State::Free;  // never resolved (no FG ran)
        if (slot.state != FrameSlot::State::Submitted || slot.fence->GetCompletedValue() < slot.value) continue;
        void* data = nullptr;
        D3D12_RANGE range{0, kStatsBytes + kQueriesPerSlot * 8};
        if (SUCCEEDED(slot.readback->Map(0, &range, &data))) {
            float s[8];
            std::memcpy(s, data, sizeof(s));
            uint64_t ticks[kQueriesPerSlot];
            std::memcpy(ticks, static_cast<const uint8_t*>(data) + kStatsBytes, sizeof(ticks));
            D3D12_RANGE none{0, 0};
            slot.readback->Unmap(0, &none);
            const unsigned decision = unsigned(s[6]);
            ++pairs_; ++totalPairs_;
            holds_ += decision & 1u; totalHolds_ += decision & 1u;
            globals_ += (decision >> 1) & 1u; totalGlobals_ += (decision >> 1) & 1u;
            speed_ += s[7];
            if (ticksPerMs_ > 0 && ticks[1] > ticks[0]) {
                double ms = double(ticks[1] - ticks[0]);
                for (unsigned f = 0; f < slot.finalizes; ++f)
                    if (ticks[3 + 2 * f] > ticks[2 + 2 * f]) ms += double(ticks[3 + 2 * f] - ticks[2 + 2 * f]);
                gpuMs_ += ms / ticksPerMs_;
                ++timed_;
            }
            if (log::verboseFrameLogs())
                log::info("fg-guard", std::format("pair global=({:.1f},{:.1f}) share={:.2f} byGlobal={:.2f} byFlow={:.2f} changed={:.2f} decision={}",
                                                  s[0], s[1], s[2], s[3], s[4], s[5], decision));
        }
        slot.state = FrameSlot::State::Free;
    }
    if (pairs_ >= 120) {
        log::info("fg-guard", std::format("pairs={} holds={} globalBlend={} meanSpeedPx={:.1f} gpuMsPerRealFrame={:.3f} (session pairs={} holds={} globalBlend={})",
                                          pairs_, holds_, globals_, speed_ / double(pairs_), timed_ ? gpuMs_ / double(timed_) : 0.0,
                                          totalPairs_, totalHolds_, totalGlobals_));
        pairs_ = holds_ = globals_ = timed_ = 0;
        gpuMs_ = speed_ = 0;
    }
}

}  // namespace veyra::pipeline
