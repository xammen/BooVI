#include "veyra/engine/AnticipationStage.h"

#include <cstring>

namespace veyra::engine {
namespace {
void transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource; barrier.Transition.StateBefore = from; barrier.Transition.StateAfter = to;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(1, &barrier);
}
} // namespace

bool AnticipationStage::ensure(ID3D12Device* device, const D3D12_RESOURCE_DESC& like) {
    if (failed_) return false;
    if (ready() && desc_.Width == like.Width && desc_.Height == like.Height && desc_.Format == like.Format) return true;
    close();
    auto fail = [&](const char* what, HRESULT hr) {
        log::warn("anticipation", std::format("stage unavailable: {} hr=0x{:X}; pictures are presented as before", what, unsigned(hr)));
        close(); failed_ = true; return false;
    };
    D3D12_RESOURCE_DESC desc = like;
    desc.MipLevels = 1; desc.DepthOrArraySize = 1; desc.Alignment = 0;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{}; rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; rtvDesc.NumDescriptors = kTextures;
    D3D12_DESCRIPTOR_HEAP_DESC viewDesc{}; viewDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; viewDesc.NumDescriptors = kTextures; viewDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    HRESULT hr = device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&rtvs_));
    if (SUCCEEDED(hr)) hr = device->CreateDescriptorHeap(&viewDesc, IID_PPV_ARGS(&blitHeap_));
    if (FAILED(hr)) return fail("descriptor heaps", hr);
    rtvIncrement_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    viewIncrement_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    std::vector<uint8_t> shader;
    // Luma pass heap: one source view per stage texture, then the luma target.
    if (!pipeline::loadShaderBytes("AnticipationLuma.dxil", shader) || !lumaPass_.create(device, shader, kTextures + 1, 1, 1, 8, 0, true)) return fail("luma pass", E_FAIL);
    for (unsigned i = 0; i < kTextures; ++i) {
        hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, kRest, nullptr, IID_PPV_ARGS(&textures_[i]));
        if (FAILED(hr)) return fail("picture texture", hr);
        textures_[i]->SetName(L"Veyra anticipation stage");
        device->CreateRenderTargetView(textures_[i].Get(), nullptr, rtv(i));
        pipeline::makeSrv(device, textures_[i].Get(), desc.Format, {blitHeap_->GetCPUDescriptorHandleForHeapStart().ptr + size_t(i) * viewIncrement_});
        pipeline::makeSrv(device, textures_[i].Get(), desc.Format, pipeline::cpuHandleOf(lumaPass_, i));
    }
    luma_ = pipeline::makeTexture(device, kMotionWidth, kMotionHeight, DXGI_FORMAT_R32_FLOAT, true);
    if (!luma_) return fail("luma texture", E_OUTOFMEMORY);
    pipeline::makeUav(device, luma_.Get(), DXGI_FORMAT_R32_FLOAT, pipeline::cpuHandleOf(lumaPass_, kTextures));
    const auto lumaDesc = luma_->GetDesc();
    UINT64 bytes = 0;
    device->GetCopyableFootprints(&lumaDesc, 0, 1, 0, &footprint_, nullptr, nullptr, &bytes);
    D3D12_HEAP_PROPERTIES readHeap{}; readHeap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; buffer.Width = bytes; buffer.Height = 1; buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1; buffer.SampleDesc.Count = 1; buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    hr = device->CreateCommittedResource(&readHeap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback_));
    if (FAILED(hr)) return fail("luma readback", hr);
    desc_ = desc; current_ = 0; readbackBytes_ = bytes;
    log::info("anticipation", std::format("stage {}x{} format={} textures={} luma={}x{} readback={} bytes", desc.Width, desc.Height, unsigned(desc.Format), kTextures, kMotionWidth, kMotionHeight, bytes));
    return true;
}

void AnticipationStage::close() {
    for (auto& texture : textures_) texture.Reset();
    rtvs_.Reset(); blitHeap_.Reset(); luma_.Reset(); readback_.Reset(); lumaPass_ = {};
    desc_ = {}; footprint_ = {}; current_ = 0; readbackBytes_ = 0; failed_ = false;
}

void AnticipationStage::recordLuma(ID3D12GraphicsCommandList* list, unsigned index, bool linear) {
    transition(list, luma_.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const float constants[8] = {1.0f / kMotionWidth, 1.0f / kMotionHeight, linear ? 1.0f : 0.0f};
    lumaPass_.bind(list, constants, pipeline::gpuHandleOf(lumaPass_, index).ptr, pipeline::gpuHandleOf(lumaPass_, kTextures).ptr);
    list->Dispatch((kMotionWidth + 7) / 8, (kMotionHeight + 7) / 8, 1);
    transition(list, luma_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION from{}, to{};
    from.pResource = luma_.Get(); from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    to.pResource = readback_.Get(); to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; to.PlacedFootprint = footprint_;
    list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    transition(list, luma_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
}

bool AnticipationStage::readProfiles(MotionProfiles& out) {
    if (!readback_) return false;
    void* mapped = nullptr;
    const D3D12_RANGE read{0, SIZE_T(readbackBytes_)};
    if (FAILED(readback_->Map(0, &read, &mapped)) || !mapped) return false;
    out = motionProfiles(static_cast<const float*>(mapped), footprint_.Footprint.RowPitch / sizeof(float));
    const D3D12_RANGE written{};
    readback_->Unmap(0, &written);
    return true;
}

} // namespace veyra::engine
