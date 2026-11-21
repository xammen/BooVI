#pragma once
// Fork, "rejoindre un partage": a friend's BooVI joins the host's share (GuestStreamer.h) as a
// native client instead of a browser. It speaks the page's signaling (POST /session, /answer,
// marked as a BooVI client), receives the host's H.264 and Opus over WebRTC (libdatachannel,
// the host's ICE servers, Cloudflare TURN included), decodes with the shared D3D12VA decoder and
// feeds the engine as a live stream, so this PC's own super resolution, fluidité preset and frame
// generation run on the host's real pictures.
//
// Input: the engine routes the PS5 input thread's pad (SDL3, a DualSense with gyro, accel and
// touchpad) here; it leaves on the "input" channel as the extended message (GuestProtocol.h).
// The host's rumble, trigger effects and haptics come back on "feedback" for takeFeedback().
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "veyra/remoteplay/Types.h"
#include "veyra/sink/CaptureAudioSession.h"
#include "veyra/source/IFrameSource.h"

struct AVFrame;
struct ID3D12Device;
struct ID3D12CommandQueue;

namespace veyra::source {

struct GuestClientDesc {
    std::string link;                 // the share link, token included; never logged
    std::string name;                 // what the host's room shows for this friend (a share link; an access brings its own)
    std::shared_ptr<ID3D12Device> decodeDevice;
    std::shared_ptr<ID3D12CommandQueue> decodeQueue;
    const std::atomic<bool>* stopRequested = nullptr;
};

struct GuestClientStats {
    enum class State : uint8_t { Idle, Connecting, Streaming, Ended, Failed };
    State state = State::Idle;
    std::wstring message;             // progress while connecting; the reason when Ended or Failed
    std::string host;                 // the host's name from the link, no token
    std::string hostName;             // a friend's access: the name the host gave their gateway, may be empty
    uint32_t width = 0, height = 0;
    bool hardwareDecode = false;
    std::string decoder;              // "d3d12va", "software", "software(after d3d12va)"; empty before the first picture
    std::string gpu;                  // the decoding GPU's vendor: "amd", "nvidia", "intel"
    bool relayed = false;             // the selected pair goes through a TURN relay
    bool iceFailed = false;           // Failed because no network path was found: the browser may still get through
    bool extendedInput = false;       // the host takes the extended pad message
    std::string controlMode;          // the host's choice: "guest", "together", "watch"
    std::string room;                 // the host's last room snapshot (RoomModel.h snapshotJson), JSON; empty from an older host
    std::string roomNotice;           // the host refused a room command: "watcher", "too-soon"…
    uint64_t units = 0, decoded = 0, dropped = 0, decodeErrors = 0, keyframeRequests = 0, inputsSent = 0;
    double receivedFps = 0, decodedFps = 0, videoMbps = 0, decodeMs = 0, rttMs = 0;
    double hostToScreenMs = -1;       // picture decoded on the host -> presented here, clock-synced median
    double lossPercent = 0;           // pictures the host sent that never arrived, last window
    // Camera anticipation on this client; the engine fills it in its snapshot.
    struct Anticipation {
        int mode = 0;                 // 0 off, 1 anticipation alone, 2 with interpolation
        bool running = false, learnt = false, active = false;
        double confidence = 0, lagMs = 0, presentsPerSecond = 0;
    } anticipation;
};

class GuestClientSource final : public IFrameSource {
public:
    GuestClientSource();
    ~GuestClientSource() override;
    GuestClientSource(const GuestClientSource&) = delete;
    GuestClientSource& operator=(const GuestClientSource&) = delete;

    // Signaling and ICE; blocks until media flow or failure (stats().message says why).
    bool connect(GuestClientDesc desc);

    bool open(const SourceOpenDesc&) override { return false; }
    const SourceInfo& info() const override { return info_; }
    SourceReadStatus read(pipeline::FramePacket&, const AVFrame**) override;
    bool seek(const pipeline::Rational&) override { return false; }
    void close() noexcept override;

    // Signalled when a decoded picture is ready (the engine's live loop waits on it).
    void* frameEvent() const { return frameEvent_; }
    GuestClientStats stats() const;
    uint64_t skipped() const;
    uint64_t unitsReceived() const;
    sink::CaptureAudioState audioState() const { return audio_.snapshot(); }
    void setAudioGain(float value) { audio_.setGain(value); }
    void setAudioSync(unsigned mode, int offset) { audio_.setSync(mode, offset); }
    void videoPresented(double ptsMs, int64_t host100ns, bool sourceFrame);
    void videoReset(bool resetAudio = true) { audio_.videoReset(resetAudio); }
    // When the host had the picture of this time stamp, on this PC's clock (host 100 ns): the
    // host's own time for it through the stats channel's clock sync, else its arrival less the
    // usual transit. A generated picture's stamp lies between two real ones. -1: not known yet.
    int64_t pictureHostTime100ns(int64_t pts100ns);

    // The PS5 input thread's pad, every pass the send gate lets through.
    void controller(const remoteplay::ControllerState& state);
    // A room command for the host (RoomModel.h RoomCommand: "ask", "cancel", "release", "roll").
    // Any thread; sent from a thread of its own, never the caller's.
    void roomCommand(const std::string& command);
    remoteplay::ControllerFeedback takeFeedback();

private:
    struct Impl;
    void run(GuestClientDesc desc);
    void decodeLoop();
    void publish(std::shared_ptr<AVFrame> frame, const pipeline::FramePacket& packet, const SourceInfo& info);
    void fail(std::wstring message);

    std::unique_ptr<Impl> p_;
    mutable std::mutex mutex_;
    std::condition_variable ready_;
    std::atomic<bool> cancel_{false};
    bool initialized_ = false, started_ = false;
    SourceInfo info_, publishedInfo_;
    struct Frame { std::shared_ptr<AVFrame> frame; pipeline::FramePacket packet; SourceInfo info; };
    std::optional<Frame> latest_;
    std::shared_ptr<AVFrame> view_;
    uint64_t sequence_ = 0, skipped_ = 0;
    bool openFlagPending_ = true;
    GuestClientStats stats_;
    sink::CaptureAudioSession audio_;
    void* frameEvent_ = nullptr;
    std::jthread owner_;
};

} // namespace veyra::source
