#pragma once
// Live recording (LiveRecorder) reads the presented frame inside the
// presentation command list, after the presenter's own reads. The consumer
// fence the graph waits on before reusing the texture then covers the
// recording too, and no second queue ever touches the graph's textures.
// Engine thread only. Kept free of the presenter's headers: EngineController.h
// is included after Qt, whose `slots` macro breaks diagnostics/GpuTimer.h.
#include <d3d12.h>
#include <array>
#include <atomic>
#include <cstdint>

namespace veyra::engine {

class PresentCapture {
public:
    struct Frame {
        ID3D12Resource* texture=nullptr; // graph output; COMMON on entry, must be left in COMMON
        DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN;
        uint32_t width=0,height=0;
        uint32_t hdrMode=0;              // 0 SDR, 1 scRGB (linear BT.709), 2 HDR10 (PQ BT.2020)
        float ditherStep=0;              // graph output dither step (RgbToNv12 reserved.y)
        bool generated=false;
        int64_t sourceHost100ns=0;       // steady clock when a live source's picture reached this PC decoded; 0 unknown
        bool early=false;                // the guest's early tap (EarlyCapture), before frame generation and pacing
        bool live=false;                 // a live session: its real pictures come through the early tap, not here
    };
    virtual ~PresentCapture()=default;
    // true: commands were recorded into `list`; submitted() or discarded() follows.
    virtual bool record(ID3D12GraphicsCommandList* list,const Frame& frame)=0;
    // The list was submitted; this fence point covers the recorded commands.
    virtual void submitted(ID3D12Fence* fence,uint64_t value)=0;
    // The list was not submitted (failed present); nothing will execute.
    virtual void discarded()=0;
};

// The fork's remote guests. Real pictures: a live source (console stream, capture card) hands
// each one over right after the graph submitted it, on the graph's queue (Frame::early), so frame
// generation and pacing hold that picture for the host's screen only (up to 30 ms at 4X).
// The fluide feeds take the presented stream instead, generated frames included, at its
// present. A file always hands its pictures over at their present. Several guests may want
// both in the same frame: each consumer reads Frame::early and Frame::live and takes its own.
class EarlyCapture : public PresentCapture {
public:
    // Engine thread, cheap: a consumer on real pictures would take an early frame now. No command
    // list is opened otherwise.
    virtual bool wanted() const=0;
};

// The presenter has one capture; a recording, the instant replay buffer and the fork's
// remote guest (GuestStreamer.h) all take the presented frame through this. Consumers are
// added, never removed, and must outlive the engine thread.
class PresentCaptureFanout final : public PresentCapture {
public:
    PresentCaptureFanout(PresentCapture& first,PresentCapture& second){add(first);add(second);}
    // Any thread; false when full. The engine thread sees the new consumer from its next frame.
    bool add(PresentCapture& capture){
        const size_t n=count_.load(std::memory_order_relaxed);
        if(n>=kMax)return false;
        consumers_[n]=&capture;
        count_.store(n+1,std::memory_order_release);
        return true;
    }
    bool record(ID3D12GraphicsCommandList* list,const Frame& frame) override {
        bool any=false;
        for(size_t i=0,n=count_.load(std::memory_order_acquire);i<n;++i)any|=consumers_[i]->record(list,frame);
        return any;
    }
    // Each one acts only on what it recorded itself.
    void submitted(ID3D12Fence* fence,uint64_t value) override {for(size_t i=0,n=count_.load(std::memory_order_acquire);i<n;++i)consumers_[i]->submitted(fence,value);}
    void discarded() override {for(size_t i=0,n=count_.load(std::memory_order_acquire);i<n;++i)consumers_[i]->discarded();}
private:
    static constexpr size_t kMax=4;
    std::array<PresentCapture*,kMax> consumers_{};
    std::atomic<size_t> count_{0};
};

} // namespace veyra::engine
