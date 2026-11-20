#pragma once
// Frame-generation guard ("anti-artefacts", réglages → image), around any generator that makes
// frames between two real frames A and B (DLSS, FSR, VFG, AMF):
//  - overlays: screen-static pixels (crosshair, HUD text, minimap) are found from A and B
//    (FgHudDetect), removed from the generator's input (FgHudInpaint) and put back on every
//    generated frame from A and B (FgFinalize), so the generator never moves them;
//  - motion: the pair's global motion (FgStats) replaces the generated pixels where the two real
//    frames agree once moved by it (a camera pan), and when nothing explains a pair (a flick past
//    the flow's reach, a cut) the nearest real frame is shown instead ("safe hold").
// All on the GPU, no CPU wait: decisions are read back later only for the log.
#include "veyra/pipeline/GpuPassUtils.h"

#include <array>
#include <cstdint>

namespace veyra::pipeline {

class FgGuard {
public:
    struct Desc {
        ID3D12Device* device = nullptr;
        DescriptorStager* stager = nullptr;
        ID3D12CommandQueue* queue = nullptr;     // timestamp frequency
        uint32_t width = 0, height = 0;
        DXGI_FORMAT colorFormat = DXGI_FORMAT_UNKNOWN;
        std::array<ID3D12Resource*, 2> real{};    // the graph's two real frames (A/B by parity)
        ID3D12Resource* flow = nullptr;          // work-extent current->previous motion, R16G16_FLOAT
        const ComPtr<ID3D12Resource>* generated = nullptr;
        unsigned generatedCount = 0;
        bool overlays = true, motion = true;
    };
    bool create(const Desc& desc);
    bool active() const { return created_; }

    // After real frame `parity` is written (COMMON), before any generator reads it.
    void analyze(ID3D12GraphicsCommandList* list, StateTracker& tracker, unsigned parity, bool historyValid, bool haveFlow);
    // What the generator reads for real frame `parity`.
    ID3D12Resource* generatorInput(unsigned parity) const { return overlays_ ? hudless_[parity].Get() : desc_.real[parity]; }
    // After the generator wrote `slot` (its pair's B is `parity`); t in (0,1).
    void finalize(ID3D12GraphicsCommandList* list, StateTracker& tracker, unsigned slot, unsigned parity, float t);
    // In the frame's last list: resolves its timings and decision for the log.
    void endFrame(ID3D12GraphicsCommandList* list, StateTracker& tracker);
    // After that list is submitted.
    void submitted(ID3D12Fence* fence, uint64_t value);

private:
    struct FrameSlot {
        ComPtr<ID3D12Resource> readback;    // 2 stats texels, then the timestamps
        ID3D12Fence* fence = nullptr;
        uint64_t value = 0;
        unsigned finalizes = 0;
        enum class State { Free, Recording, Resolved, Submitted } state = State::Free;
    };
    static constexpr unsigned kSlots = 8, kQueriesPerSlot = 2 + 2 * 8, kStatsBytes = 256;
    void collect();
    void timestamp(ID3D12GraphicsCommandList* list, unsigned index);

    Desc desc_{};
    bool created_ = false, overlays_ = false, motion_ = false, viewMask_ = false;
    ComPtr<ID3D12Resource> score_, stats_, dummyFlow_;
    ComPtr<ID3D12Resource> hudless_[2];
    ComputePass detect_, inpaint_, stats_pass_, finalize_;
    ComPtr<ID3D12QueryHeap> queries_;
    std::array<FrameSlot, kSlots> slots_{};
    unsigned current_ = 0;
    bool recording_ = false;
    double ticksPerMs_ = 0;
    // Log totals since the last line.
    uint64_t pairs_ = 0, holds_ = 0, globals_ = 0, timed_ = 0, totalPairs_ = 0, totalHolds_ = 0, totalGlobals_ = 0;
    double gpuMs_ = 0, speed_ = 0;
};

}  // namespace veyra::pipeline
