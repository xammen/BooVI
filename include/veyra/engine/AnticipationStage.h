#pragma once
// Fork, camera anticipation: the GPU side's resources (docs/CAMERA_ANTICIPATION_2026-11-20.md).
//
// The presenter draws a picture many times, shifted a little more each screen refresh, while
// the graph already writes the next one into its own pool. So each presented picture is first
// copied to one of two textures here ("the stage"), and a 160x90 luma copy of it is read back
// for the global motion estimate. Everything runs on the presentation queue, in its order.
#include <array>
#include <cstdint>

#include "veyra/engine/CameraAnticipation.h"
#include "veyra/pipeline/GpuPassUtils.h"

namespace veyra::engine {

class AnticipationStage {
public:
    static constexpr unsigned kTextures = 2;
    // The stage's textures rest in this state: sampled by the blit and by the luma pass.
    static constexpr D3D12_RESOURCE_STATES kRest = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;

    // Textures like `like` (the graph's output). False when they cannot be made.
    bool ensure(ID3D12Device* device, const D3D12_RESOURCE_DESC& like);
    void close();
    bool ready() const { return bool(textures_[0]); }

    // The texture the next picture goes to: never the one being drawn from.
    unsigned next() const { return (current_ + 1) % kTextures; }
    unsigned current() const { return current_; }
    void setCurrent(unsigned index) { current_ = index; }
    ID3D12Resource* texture(unsigned index) const { return textures_[index].Get(); }
    D3D12_CPU_DESCRIPTOR_HANDLE rtv(unsigned index) const { return {rtvs_->GetCPUDescriptorHandleForHeapStart().ptr + size_t(index) * rtvIncrement_}; }
    // The blit's source: a heap of its own with one view per texture.
    ID3D12DescriptorHeap* blitHeap() const { return blitHeap_.Get(); }
    D3D12_GPU_DESCRIPTOR_HANDLE blitSource(unsigned index) const { return {blitHeap_->GetGPUDescriptorHandleForHeapStart().ptr + size_t(index) * viewIncrement_}; }

    // Records the luma copy of texture `index` and its readback. linear: scRGB values.
    void recordLuma(ID3D12GraphicsCommandList* list, unsigned index, bool linear);
    // The profiles of the last recorded luma copy; call once its list has finished on the GPU.
    bool readProfiles(MotionProfiles& out);

private:
    std::array<pipeline::ComPtr<ID3D12Resource>, kTextures> textures_;
    pipeline::ComPtr<ID3D12DescriptorHeap> rtvs_, blitHeap_;
    pipeline::ComPtr<ID3D12Resource> luma_, readback_;
    pipeline::ComputePass lumaPass_;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint_{};
    D3D12_RESOURCE_DESC desc_{};
    UINT64 readbackBytes_ = 0;
    UINT rtvIncrement_ = 0, viewIncrement_ = 0;
    unsigned current_ = 0;
    bool failed_ = false;
};

} // namespace veyra::engine
