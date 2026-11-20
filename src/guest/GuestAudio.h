#pragma once
// Fork, partage: the guest's sound. Takes the PCM the player renders (RecordingAudioTap.h,
// before volume and mute, so muting BooVI does not mute the guest), downmixes it to stereo
// and encodes Opus in 10 ms frames on its own thread. Every packet goes to the sink with
// its RTP timestamp (48 kHz samples).
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace veyra::guest {

class GuestAudio {
public:
    using Sink = std::function<void(const std::vector<uint8_t>& packet, uint32_t rtpTimestamp)>;
    explicit GuestAudio(Sink sink);
    ~GuestAudio();
    GuestAudio(const GuestAudio&) = delete;
    GuestAudio& operator=(const GuestAudio&) = delete;
    void start();   // any thread; no-op while running
    void stop();    // any thread; joins
    uint64_t packets() const;
private:
    struct Impl;
    std::shared_ptr<Impl> p_;
};

} // namespace veyra::guest
