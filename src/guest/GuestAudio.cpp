#include "GuestAudio.h"

#include "veyra/Log.h"
#include "veyra/sink/RecordingAudioTap.h"

#include <opus/opus.h>
#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <format>
#include <mutex>
#include <thread>

namespace veyra::guest {
namespace {
constexpr int kRate = 48000;
constexpr int kFrame = kRate / 100;            // 10 ms
constexpr size_t kMaxQueued = size_t(kRate) / 5; // 200 ms: beyond it the oldest sound goes
}

// The tap holds a shared_ptr to the sink side; the encoder thread lives in the same object.
struct GuestAudio::Impl final : sink::RecordingAudioSink {
    Sink sink;
    std::mutex m;
    std::condition_variable cv;
    std::deque<float> queue;                   // interleaved stereo
    const void* renderer = nullptr;            // the first endpoint heard; others are ignored
    bool running = false;
    std::thread thread;
    std::atomic<uint64_t> sent{0};

    void renderedPcm(const float* pcm, size_t frames, const sink::AudioFormat& format, int64_t, const void* from) override {
        if (!pcm || !frames || !format.channels) return;
        {
            std::lock_guard lock(m);
            if (!running) return;
            if (!renderer) renderer = from;
            if (from != renderer) return;
            const unsigned ch = format.channels;
            for (size_t i = 0; i < frames; ++i) {
                const float* f = pcm + i * ch;
                queue.push_back(f[0]);
                queue.push_back(ch > 1 ? f[1] : f[0]);
            }
            while (queue.size() > kMaxQueued * 2) queue.pop_front();
        }
        cv.notify_one();
    }

    void run() {
        SetThreadDescription(GetCurrentThread(), L"Guest audio");
        int error = 0;
        OpusEncoder* encoder = opus_encoder_create(kRate, 2, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &error);
        if (!encoder) { veyra::log::error("guest-audio", std::format("opus encoder error={}", error)); return; }
        opus_encoder_ctl(encoder, OPUS_SET_BITRATE(128000));
        opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(5));
        std::vector<float> frame(kFrame * 2);
        std::vector<uint8_t> packet(1500);
        uint32_t timestamp = 0;
        veyra::log::info("guest-audio", "opus 48000 Hz stereo 10 ms started");
        for (;;) {
            {
                std::unique_lock lock(m);
                cv.wait(lock, [&] { return !running || queue.size() >= frame.size(); });
                if (!running) break;
                std::copy_n(queue.begin(), frame.size(), frame.begin());
                queue.erase(queue.begin(), queue.begin() + ptrdiff_t(frame.size()));
            }
            const int bytes = opus_encode_float(encoder, frame.data(), kFrame, packet.data(), int(packet.size()));
            if (bytes > 0) {
                sink(std::vector<uint8_t>(packet.begin(), packet.begin() + bytes), timestamp);
                ++sent;
            }
            timestamp += kFrame;
        }
        opus_encoder_destroy(encoder);
    }
};

GuestAudio::GuestAudio(Sink sink) : p_(std::make_shared<Impl>()) { p_->sink = std::move(sink); }
GuestAudio::~GuestAudio() { stop(); }

void GuestAudio::start() {
    {
        std::lock_guard lock(p_->m);
        if (p_->running) return;
        p_->running = true; p_->renderer = nullptr; p_->queue.clear();
    }
    p_->thread = std::thread([p = p_.get()] { p->run(); });
    sink::addRecordingAudioSink(p_);
}

void GuestAudio::stop() {
    sink::removeRecordingAudioSink(p_.get());
    { std::lock_guard lock(p_->m); p_->running = false; }
    p_->cv.notify_all();
    if (p_->thread.joinable()) p_->thread.join();
}

uint64_t GuestAudio::packets() const { return p_->sent.load(); }

} // namespace veyra::guest
