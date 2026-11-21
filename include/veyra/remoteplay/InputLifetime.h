#pragma once

namespace veyra::remoteplay {

// The engine's latest session, as the PS5 input thread (RemotePlayInput) sees it.
struct InputSession {
    bool remotePlay = false;    // a PS5 stream
    bool guestClient = false;   // a share joined as a BooVI client
    bool running = false;
    bool opening = false;
    bool stopping = false;
};

// Whether the input thread still has a session to feed: a PS5 stream that is opening or running,
// or a joined share that is opening, running or stopping. A PS5 session that fails keeps
// `running` until the engine's teardown ends, then this turns false.
// Field log 2026-11-20: the UI tick that applied this rule stopped running, and the thread kept
// polling the pad at 1 kHz for 2 h after the PS5 quit (quitReason=12). The thread now applies it
// itself (RemotePlayInput::run), and the UI tick still does for its own state.
constexpr bool inputWanted(const InputSession& s) {
    return (s.remotePlay && (s.running || s.opening)) || (s.guestClient && (s.running || s.opening || s.stopping));
}

} // namespace veyra::remoteplay
