// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// Keyboard and mouse capture for a Moonlight session. While captured, the app window's keyboard
// and mouse messages go to the host instead of the UI: the native event filter sees them before Qt
// does. The mouse is read as raw relative movement, the cursor is hidden and kept inside the
// window. Ctrl+Alt+Shift+Z releases the capture, +Q asks to end the session, +S toggles the stats.
// Not captured: the Windows key, Alt+Tab and Ctrl+Alt+Del (the system keeps them; losing the focus
// releases the capture and everything held on the host).
#include <QAbstractNativeEventFilter>
#include <functional>
#include <QObject>
#include <windows.h>

#include "veyra/moonlight/InputRouter.h"

namespace veyra::engine { class EngineController; }

namespace veyra::ui {

class MoonlightInputCapture final : public QObject, public QAbstractNativeEventFilter, private moonlight::InputSink {
    Q_OBJECT
public:
#ifdef VEYRA_ENABLE_MOONLIGHT
    explicit MoonlightInputCapture(engine::EngineController& engine, QObject* parent = nullptr);
#endif
    // Fork: the capture for a reader on another thread that registers raw input itself (the
    // PS5 input thread). This object then hides and clips the cursor, keeps the window's key and
    // mouse messages from the UI, and handles the hotkeys and focus loss; it forwards nothing.
    explicit MoonlightInputCapture(QObject* parent = nullptr);
    ~MoonlightInputCapture() override;

    void setWindow(HWND topLevel) { window_ = topLevel; }
    bool captured() const { return captured_; }
    // Returns false when it cannot start (no window, or the window is not in the foreground).
    bool capture(bool on);

    bool nativeEventFilter(const QByteArray& eventType, void* message, qintptr* result) override;

signals:
    void capturedChanged(bool captured);
    void releaseRequested();   // Ctrl+Alt+Shift+Z was pressed (capture is already released)
    void quitRequested();      // Ctrl+Alt+Shift+Q
    void statsRequested();     // Ctrl+Alt+Shift+S
    void keysRequested();      // Ctrl+M, PS5 capture only: show or hide the bindings overlay

private:
    void key(uint32_t virtualKey, uint32_t scanCode, bool extended, bool down) override;
    void mouseMove(int dx, int dy) override;
    void mouseButton(int button, bool down) override;
    void scroll(int delta, bool horizontal) override;
    void init();
    void clipToWindow();

    engine::EngineController* engine_ = nullptr; // null: input is read elsewhere
    std::function<void()> releaseInput_;
    moonlight::InputRouter router_;
    HWND window_ = nullptr;
    bool captured_ = false;
};

} // namespace veyra::ui
