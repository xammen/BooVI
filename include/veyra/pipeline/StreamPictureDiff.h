#pragma once
// How much a hardware-decoded stream picture differs from the one before it (2026-11-20).
//
// Content dedup compares pictures on the CPU (PictureCompare.h); a D3D12VA picture never
// reaches the CPU, so the PS5 stream ran with every picture "blind" and kept. This counts,
// on the GPU, the luma samples beyond a band of 3 levels in 255 between two consecutive
// decoded pictures and reads the one number back. StreamCadence turns the sequence of
// counts into keep / drop.
//
// The compare runs on the engine's own queue behind a GPU-side wait on the decoder's
// fence, so the answer arrives when the decode has finished: work the enhancement of this
// picture had to wait for anyway. The caller waits on event() while it keeps presenting.
#include <cstdint>
#include <memory>
#include <optional>

#include "veyra/pipeline/GpuPassUtils.h"

struct AVFrame;

namespace veyra::pipeline {

class StreamPictureDiff {
public:
    struct Result { uint64_t changed = 0, total = 0; };

    ~StreamPictureDiff() { close(); }
    bool initialize(ID3D12Device* device, ID3D12CommandQueue* queue);
    void close();
    // Forgets the previous picture (decoder reset, new session): the next one has no count.
    void reset() { previous_.reset(); }
    // Starts the compare of `frame` with the picture before it. False: no compare is running
    // (first picture, a CPU frame, another extent); `frame` still becomes the previous picture.
    bool submit(const AVFrame* frame);
    bool pending() const { return pending_; }
    // Signalled when a submitted compare has finished on the GPU.
    HANDLE event() const { return event_; }
    // The count of the submitted compare once the GPU finished it; nullopt while it runs.
    std::optional<Result> take();
    // Gives up on a compare the caller no longer waits for: the next submit starts clean.
    void abandon();

private:
    ID3D12Device* device_ = nullptr;
    ID3D12CommandQueue* queue_ = nullptr;
    ComputePass pass_;
    DescriptorStager stager_;
    ComPtr<ID3D12CommandAllocator> allocator_;
    ComPtr<ID3D12GraphicsCommandList> list_;
    ComPtr<ID3D12Fence> fence_;
    ComPtr<ID3D12Resource> total_, readback_;
    std::shared_ptr<AVFrame> previous_, compared_;
    HANDLE event_ = nullptr;
    uint64_t fenceValue_ = 0, pendingTotal_ = 0;
    uint32_t lastTotal_ = 0;
    bool pending_ = false;
};

} // namespace veyra::pipeline
