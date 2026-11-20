#pragma once
// Test only: copies the frames the graph hands to presentation (real and generated) to disk,
// for the frame-generation benchmark (scripts/fork/fgbench). VEYRA_TEST_FG_DUMP=<dir> turns it
// on; VEYRA_TEST_FG_DUMP_EVERY=<n> keeps the pairs whose real frame's time is a multiple of n source
// periods (VEYRA_TEST_FG_DUMP_PERIOD, 100 ns units, default 1/30 s), so runs dump the same pairs. Copies go to readback
// buffers and are written by a thread once their fence passes: no CPU wait in the graph.
#include <d3d12.h>
#include <wrl/client.h>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace veyra::pipeline {

class FgFrameDump {
public:
    // subdir: a folder of its own under the dump directory (the FG motion field, say).
    static std::unique_ptr<FgFrameDump> fromEnvironment(ID3D12Device* device, uint32_t width, uint32_t height, DXGI_FORMAT format,
                                                        const wchar_t* subdir = nullptr, uint32_t slots = 32);
    ~FgFrameDump();
    bool wantPair(int64_t b100ns) const { return (b100ns + period_ / 2) / period_ % every_ == 0; }
    // The texture must be in COPY_SOURCE. Returns false when every buffer is still in flight.
    bool record(ID3D12GraphicsCommandList* list, ID3D12Resource* texture, std::string name);
    // Every record since the last call completes with this fence value.
    void submitted(ID3D12Fence* fence, uint64_t value);
    void poll();

private:
    struct Slot {
        Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
        std::string name;
        ID3D12Fence* fence = nullptr;
        uint64_t value = 0;
        enum class State { Free, Recorded, Submitted } state = State::Free;
    };
    FgFrameDump() = default;
    void writer();

    std::filesystem::path dir_;
    uint32_t every_ = 2, width_ = 0, height_ = 0;
    int64_t period_ = 333333;
    DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint_{};
    uint64_t bytes_ = 0, skipped_ = 0, written_ = 0;
    std::vector<Slot> slots_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::pair<std::string, std::vector<uint8_t>>> queue_;
    bool stop_ = false;
    std::thread thread_;
};

}  // namespace veyra::pipeline
