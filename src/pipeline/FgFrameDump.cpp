#include "veyra/pipeline/FgFrameDump.h"

#include "veyra/Log.h"

#include <cstdio>
#include <cstring>
#include <format>
#include <fstream>

namespace veyra::pipeline {

std::unique_ptr<FgFrameDump> FgFrameDump::fromEnvironment(ID3D12Device* device, uint32_t width, uint32_t height, DXGI_FORMAT format,
                                                          const wchar_t* subdir, uint32_t slots)
{
    wchar_t dir[512]{};
    if (!GetEnvironmentVariableW(L"VEYRA_TEST_FG_DUMP", dir, 512) || !dir[0]) return nullptr;
    std::unique_ptr<FgFrameDump> dump(new FgFrameDump());
    dump->dir_ = dir;
    if (subdir) dump->dir_ /= subdir;
    std::error_code ec;
    std::filesystem::create_directories(dump->dir_, ec);
    wchar_t every[16]{};
    if (GetEnvironmentVariableW(L"VEYRA_TEST_FG_DUMP_EVERY", every, 16)) dump->every_ = std::max(1, _wtoi(every));
    if (GetEnvironmentVariableW(L"VEYRA_TEST_FG_DUMP_PERIOD", every, 16)) dump->period_ = std::max(1, _wtoi(every));
    dump->width_ = width; dump->height_ = height; dump->format_ = format;
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width = width; td.Height = height; td.DepthOrArraySize = 1;
    td.MipLevels = 1; td.Format = format; td.SampleDesc.Count = 1;
    device->GetCopyableFootprints(&td, 0, 1, 0, &dump->footprint_, nullptr, nullptr, &dump->bytes_);
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = dump->bytes_; bd.Height = 1; bd.DepthOrArraySize = 1;
    bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    dump->slots_.resize(slots);
    for (auto& slot : dump->slots_) {
        if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                   IID_PPV_ARGS(&slot.buffer)))) {
            log::error("fg-dump", "readback allocation failed");
            return nullptr;
        }
    }
    std::ofstream(dump->dir_ / "index.csv", std::ios::app)
        << std::format("# width={} height={} format={} pitch={} every={}\n", width, height, unsigned(format),
                       dump->footprint_.Footprint.RowPitch, dump->every_);
    dump->thread_ = std::thread([d = dump.get()] { d->writer(); });
    log::info("fg-dump", std::format("dumping {}x{} format={} every {} pair(s)", width, height, unsigned(format), dump->every_));
    return dump;
}

FgFrameDump::~FgFrameDump()
{
    poll();
    { std::lock_guard lock(mutex_); stop_ = true; }
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
    log::info("fg-dump", std::format("written={} skipped={}", written_, skipped_));
}

bool FgFrameDump::record(ID3D12GraphicsCommandList* list, ID3D12Resource* texture, std::string name)
{
    poll();
    for (auto& slot : slots_) {
        if (slot.state != Slot::State::Free) continue;
        D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource = slot.buffer.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = footprint_;
        D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource = texture;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex = 0;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        slot.name = std::move(name);
        slot.state = Slot::State::Recorded;
        return true;
    }
    ++skipped_;
    return false;
}

void FgFrameDump::submitted(ID3D12Fence* fence, uint64_t value)
{
    for (auto& slot : slots_)
        if (slot.state == Slot::State::Recorded) { slot.fence = fence; slot.value = value; slot.state = Slot::State::Submitted; }
}

void FgFrameDump::poll()
{
    for (auto& slot : slots_) {
        if (slot.state != Slot::State::Submitted || slot.fence->GetCompletedValue() < slot.value) continue;
        void* data = nullptr;
        D3D12_RANGE range{0, size_t(bytes_)};
        if (SUCCEEDED(slot.buffer->Map(0, &range, &data))) {
            std::vector<uint8_t> copy(static_cast<size_t>(bytes_));
            std::memcpy(copy.data(), data, copy.size());
            D3D12_RANGE none{0, 0};
            slot.buffer->Unmap(0, &none);
            { std::lock_guard lock(mutex_); queue_.emplace_back(std::move(slot.name), std::move(copy)); }
            wake_.notify_one();
            ++written_;
        }
        slot.state = Slot::State::Free;
    }
}

void FgFrameDump::writer()
{
    for (;;) {
        std::pair<std::string, std::vector<uint8_t>> item;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [&] { return stop_ || !queue_.empty(); });
            if (queue_.empty()) return;
            item = std::move(queue_.front());
            queue_.pop_front();
        }
        std::ofstream(dir_ / (item.first + ".raw"), std::ios::binary)
            .write(reinterpret_cast<const char*>(item.second.data()), std::streamsize(item.second.size()));
        std::ofstream(dir_ / "index.csv", std::ios::app) << item.first << '\n';
    }
}

}  // namespace veyra::pipeline
