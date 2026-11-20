#pragma once
// Fork, partage: the guest's video. The engine's EarlyCapture consumer: it converts each
// picture it takes to NV12 at the guest's stream size in the command list the engine hands it,
// then encodes it with NVENC in its low-latency mode on its own thread, one frame in flight,
// and hands every bitstream to the sink at once. NVIDIA only: NVENC waits for the conversion
// on the GPU.
// - Real pictures (the default): generated frames are skipped; a live source hands each
//   picture over before presentation, a file at its present. At most 60 a second.
// - Presented (the fluide feeds): every presented frame, generated ones included, at its
//   present, kept on an even grid of `fps` a second by their times.
#include <d3d12.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "veyra/engine/PresentCapture.h"

namespace veyra::guest {

struct EncodedFrame {
    std::vector<uint8_t> data;   // Annex B access unit
    bool key = false;
    int64_t captureUs = 0;       // sink::recordingClockUs() when the engine handed the picture over
    int64_t sourceUs = 0;        // same clock, when a live source's picture was decoded here; 0 unknown
    int64_t encodedUs = 0;       // when NVENC handed the bitstream back
    uint32_t width = 0, height = 0;
    bool presented = false;      // taken from the presented stream (a fluide feed)
};

struct VideoStats {
    uint64_t encoded = 0;        // frames handed to the sink
    uint64_t skipped = 0;        // real frames skipped (every slot busy, encoder opening)
    uint32_t width = 0, height = 0;
    uint32_t sourceHeight = 0;   // the picture the tap gives
    std::string encoder;
    std::string error;
};

class GuestVideo final : public engine::EarlyCapture {
public:
    using Sink = std::function<void(EncodedFrame&&)>;
    explicit GuestVideo(Sink sink);
    ~GuestVideo() override;
    GuestVideo(const GuestVideo&) = delete;
    GuestVideo& operator=(const GuestVideo&) = delete;

    // Any thread. Stream height (the width follows the picture; a new height opens a new
    // encoder), pictures a second, real pictures or the presented stream, CBR target.
    void setQuality(uint32_t height, uint32_t fps, bool presented, uint32_t kbps);
    // Any thread: a new target bitrate for the running encoder.
    void setBitrate(uint32_t kbps);
    // Any thread. Off closes the encoder; frames are only converted while on.
    void setEnabled(bool on);
    // Any thread: the next frame is an IDR (a PLI, or a new guest).
    void requestKeyframe();
    VideoStats stats() const;

    // EarlyCapture: engine thread.
    bool wanted() const override;
    bool presentedFeed() const override;
    bool record(ID3D12GraphicsCommandList* list, const Frame& frame) override;
    void submitted(ID3D12Fence* fence, uint64_t value) override;
    void discarded() override;

    struct Pipe;
private:
    struct State;
    std::unique_ptr<State> s_;
};

} // namespace veyra::guest
