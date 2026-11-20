#include "veyra/pipeline/StreamPictureDiff.h"

#include <bit>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext_d3d12va.h>
#include <libavutil/pixfmt.h>
}

namespace veyra::pipeline {
namespace {
// 3 levels in 255 are noise, 4 are a difference (PictureReference::kBand8), on 8 and 10 bit samples.
constexpr float kBand = 3.2f / 255.0f;
constexpr UINT kTotalBytes = 16;

ID3D12Resource* lumaTexture(const AVFrame* frame, DXGI_FORMAT& lumaFormat) {
    if (!frame || frame->format != AV_PIX_FMT_D3D12 || !frame->data[0]) return nullptr;
    auto* texture = reinterpret_cast<AVD3D12VAFrame*>(frame->data[0])->texture;
    if (!texture) return nullptr;
    const auto desc = texture->GetDesc();
    if (desc.DepthOrArraySize != 1) return nullptr;
    if (desc.Format == DXGI_FORMAT_NV12) lumaFormat = DXGI_FORMAT_R8_UNORM;
    else if (desc.Format == DXGI_FORMAT_P010) lumaFormat = DXGI_FORMAT_R16_UNORM;
    else return nullptr;
    return texture;
}

ComPtr<ID3D12Resource> makeBuffer(ID3D12Device* device, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES properties{}; properties.Type = heap;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; desc.Width = kTotalBytes; desc.Height = 1; desc.DepthOrArraySize = 1;
    desc.MipLevels = 1; desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; desc.Flags = flags;
    ComPtr<ID3D12Resource> buffer;
    device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&buffer));
    return buffer;
}

void transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource; barrier.Transition.StateBefore = from; barrier.Transition.StateAfter = to;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(1, &barrier);
}
} // namespace

bool StreamPictureDiff::initialize(ID3D12Device* device, ID3D12CommandQueue* queue) {
    close();
    if (!device || !queue) return false;
    device_ = device; queue_ = queue;
    const auto type = queue->GetDesc().Type;
    std::vector<uint8_t> shader;
    total_ = makeBuffer(device, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    readback_ = makeBuffer(device, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!total_ || !readback_ || !event_ ||
        !pass_.loadShader("StreamPictureDiff.dxil", shader) || !pass_.create(device, shader, 4, 2, 1) || !stager_.initialize(device, 8) ||
        FAILED(device->CreateCommandAllocator(type, IID_PPV_ARGS(&allocator_))) ||
        FAILED(device->CreateCommandList(0, type, allocator_.Get(), nullptr, IID_PPV_ARGS(&list_))) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)))) {
        veyra::log::warn("stream-diff", "GPU picture compare unavailable; the stream keeps every picture");
        close();
        return false;
    }
    D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
    view.Format = DXGI_FORMAT_R32_TYPELESS; view.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    view.Buffer.NumElements = kTotalBytes / 4; view.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    device->CreateUnorderedAccessView(total_.Get(), nullptr, &view, cpuHandleOf(pass_, 2));
    // The running total starts wherever the new buffer's memory does: read it once.
    transition(list_.Get(), total_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list_->CopyBufferRegion(readback_.Get(), 0, total_.Get(), 0, kTotalBytes);
    transition(list_.Get(), total_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    list_->Close();
    ID3D12CommandList* lists[] = {list_.Get()};
    queue_->ExecuteCommandLists(1, lists);
    queue_->Signal(fence_.Get(), ++fenceValue_);
    fence_->SetEventOnCompletion(fenceValue_, event_);
    if (WaitForSingleObject(event_, 2000) != WAIT_OBJECT_0) { close(); return false; }
    pending_ = true; pendingTotal_ = 0;
    (void)take();
    return true;
}

void StreamPictureDiff::close() {
    if (fence_ && fenceValue_ && fence_->GetCompletedValue() < fenceValue_ && event_) {
        fence_->SetEventOnCompletion(fenceValue_, event_);
        WaitForSingleObject(event_, 2000);
    }
    previous_.reset(); compared_.reset();
    pass_ = {}; stager_ = {};
    list_.Reset(); allocator_.Reset(); fence_.Reset(); total_.Reset(); readback_.Reset();
    if (event_) CloseHandle(event_);
    event_ = nullptr; device_ = nullptr; queue_ = nullptr;
    fenceValue_ = 0; pendingTotal_ = 0; lastTotal_ = 0; pending_ = false;
}

bool StreamPictureDiff::submit(const AVFrame* frame) {
    if (!list_ || pending_) return false;
    DXGI_FORMAT format{}, previousFormat{};
    auto* texture = lumaTexture(frame, format);
    if (!texture) { previous_.reset(); return false; }
    std::shared_ptr<AVFrame> current(av_frame_clone(frame), [](AVFrame* value) { av_frame_free(&value); });
    if (!current) { previous_.reset(); return false; }
    auto* before = lumaTexture(previous_.get(), previousFormat);
    const bool comparable = before && previousFormat == format && previous_->width == frame->width && previous_->height == frame->height &&
        frame->width > 0 && frame->height > 0;
    if (!comparable) { previous_ = std::move(current); return false; }
    if (FAILED(allocator_->Reset()) || FAILED(list_->Reset(allocator_.Get(), nullptr))) { previous_ = std::move(current); return false; }
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = format; view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Texture2D.MipLevels = 1; view.Texture2D.PlaneSlice = 0;
    stager_.stageSrv(texture, &view, pass_.heap.Get(), 0);
    stager_.stageSrv(before, &view, pass_.heap.Get(), 1);
    // Both pictures are in COMMON between command lists (the decoder and the graph leave them
    // there), so reading them here needs no barrier and they decay back when the list ends.
    const float constants[8] = {std::bit_cast<float>(uint32_t(frame->width)), std::bit_cast<float>(uint32_t(frame->height)), kBand};
    pass_.bind(list_.Get(), constants, gpuHandleOf(pass_, 0).ptr, gpuHandleOf(pass_, 2).ptr);
    list_->Dispatch((UINT(frame->width) + 31) / 32, (UINT(frame->height) + 31) / 32, 1);
    transition(list_.Get(), total_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list_->CopyBufferRegion(readback_.Get(), 0, total_.Get(), 0, kTotalBytes);
    transition(list_.Get(), total_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (FAILED(list_->Close())) { previous_ = std::move(current); return false; }
    // The decoder writes on its own queue: this queue waits for both pictures on the GPU side.
    for (const AVFrame* picture : {frame, static_cast<const AVFrame*>(previous_.get())}) {
        const auto& sync = reinterpret_cast<AVD3D12VAFrame*>(picture->data[0])->sync_ctx;
        if (sync.fence) queue_->Wait(sync.fence, sync.fence_value);
    }
    ID3D12CommandList* lists[] = {list_.Get()};
    queue_->ExecuteCommandLists(1, lists);
    queue_->Signal(fence_.Get(), ++fenceValue_);
    fence_->SetEventOnCompletion(fenceValue_, event_);
    pending_ = true; pendingTotal_ = uint64_t(frame->width) * uint64_t(frame->height);
    compared_ = std::move(previous_); previous_ = std::move(current);
    return true;
}

void StreamPictureDiff::abandon() {
    if (pending_ && event_ && fence_->GetCompletedValue() < fenceValue_) {
        fence_->SetEventOnCompletion(fenceValue_, event_);
        WaitForSingleObject(event_, 2000);
    }
    // Reading the finished compare keeps the running total in step; one that never finished
    // leaves a list the GPU may still use, so the compare is closed for this session.
    if (pending_ && !take()) { pending_ = false; list_.Reset(); }
    previous_.reset();
}

std::optional<StreamPictureDiff::Result> StreamPictureDiff::take() {
    if (!pending_ || fence_->GetCompletedValue() < fenceValue_) return {};
    pending_ = false; compared_.reset();
    void* mapped = nullptr;
    const D3D12_RANGE range{0, 4};
    if (FAILED(readback_->Map(0, &range, &mapped)) || !mapped) return {};
    const uint32_t total = *static_cast<const uint32_t*>(mapped);
    const D3D12_RANGE none{0, 0};
    readback_->Unmap(0, &none);
    const uint32_t changed = total - lastTotal_;
    lastTotal_ = total;
    return Result{changed, pendingTotal_};
}

} // namespace veyra::pipeline
