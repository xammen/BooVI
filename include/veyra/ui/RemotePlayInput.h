#pragma once
// Fork: the PS5 stream's controller, keyboard and mouse, read and sent on their own thread
// every 1 ms, so a stalled UI thread no longer delays or drops input. The thread owns the SDL
// pad reader (initialized, polled and shut down there), the keyboard/mouse mapper and a
// message-only window that receives raw keyboard and mouse input while the capture is on.
// The UI thread only starts and stops it and exchanges the few values below.
#include <windows.h>

#include <atomic>
#include <mutex>
#include <stop_token>
#include <thread>

#include "veyra/remoteplay/ControllerInput.h"
#include "veyra/remoteplay/KbmMapper.h"

namespace veyra::engine { class EngineController; }
namespace veyra::guest { class GuestStreamer; }

namespace veyra::ui {

class RemotePlayInput {
public:
    explicit RemotePlayInput(engine::EngineController& engine) : engine_(engine) {}
    ~RemotePlayInput() { stop(); }
    RemotePlayInput(const RemotePlayInput&) = delete;
    RemotePlayInput& operator=(const RemotePlayInput&) = delete;

    void start();   // no-op while running
    void stop();    // sends a neutral state, then joins
    bool running() const { return thread_.joinable(); }

    // Set by the UI thread.
    void setBackground(bool on) { background_ = on; }      // read the pad without the focus
    void setKbmWindow(HWND topLevel) { kbmWindow_ = topLevel; } // captured window, null when not
    void setKbmSettings(const remoteplay::KbmSettings&);
    void calibrate() { calibrate_ = true; }
    void tapPs() { tapPs_ = true; }   // press PS once, for PsTap::kHold (PadPolicy.h)
    // Fork, partage: a remote guest's pad joins the state sent to the PS5 per the host's
    // mode (GuestProtocol.h combine), and the PS5's rumble also goes to the guest.
    void setGuest(guest::GuestStreamer* guest) { guest_ = guest; }

    // Published by the thread.
    bool focused() const { return focused_; }
    remoteplay::ControllerInput::Capabilities capabilities() const;

private:
    void run(std::stop_token stop);

    engine::EngineController& engine_;
    std::atomic<bool> background_{false}, focused_{true}, calibrate_{false}, tapPs_{false};
    std::atomic<HWND> kbmWindow_{nullptr};
    std::atomic<guest::GuestStreamer*> guest_{nullptr};
    mutable std::mutex mutex_;   // guards the three below
    remoteplay::KbmSettings settings_;
    unsigned settingsVersion_ = 0;
    remoteplay::ControllerInput::Capabilities capabilities_;
    std::jthread thread_;        // last: joined before the members it reads
};

} // namespace veyra::ui
