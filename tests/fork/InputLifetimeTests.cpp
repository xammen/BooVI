// The PS5 input thread's lifetime (InputLifetime.h), on the engine states a session goes through.
#include "veyra/remoteplay/InputLifetime.h"
#include <cstdio>

using veyra::remoteplay::InputSession;
using veyra::remoteplay::inputWanted;

static int failures = 0;
static void expect(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

// The fields the engine publishes (EngineController::inputSession), per transport.
static InputSession ps5(bool running, bool opening, bool stopping = false) { return {true, false, running, opening, stopping}; }
static InputSession share(bool running, bool opening, bool stopping = false) { return {false, true, running, opening, stopping}; }

int main() {
    {
        // Field log 2026-11-20 23:22: keyframe recovery, then "recovery stopped attempts=1
        // quitReason=12 error=2012", teardown, and the engine's run ends.
        expect(inputWanted(ps5(false, true)), "a PS5 stream that is opening wants input");
        expect(inputWanted(ps5(true, false)), "a playing PS5 stream wants input");
        expect(inputWanted(ps5(true, false)), "during keyframe recovery the stream still wants input");
        expect(inputWanted(ps5(true, false)), "once recovery stopped, the engine's teardown still runs (failed, running)");
        expect(!inputWanted(ps5(false, false)), "after the teardown (failed, not running) the thread ends");
    }
    {
        expect(inputWanted(ps5(true, false, true)), "a PS5 stream being stopped keeps input until its teardown");
        expect(!inputWanted(ps5(false, false, false)), "a stopped PS5 stream ends the thread");
        // A reconnect resets the snapshot to opening before the input thread is started again.
        expect(inputWanted(ps5(false, true)), "a reconnect opens a new session that wants input");
    }
    {
        expect(inputWanted(share(false, true)), "a joined share that is opening wants input");
        expect(inputWanted(share(true, false)), "a playing joined share wants input");
        expect(inputWanted(share(true, false, true)), "a joined share being stopped still sends the neutral pad");
        expect(!inputWanted(share(false, false)), "a closed joined share ends the thread");
    }
    {
        expect(!inputWanted(InputSession{false, false, true, false, false}), "a file or capture card playing after the stream ends the thread");
        expect(!inputWanted(InputSession{}), "no session ends the thread");
    }
    if (failures) { std::printf("%d FAILED\n", failures); return 1; }
    std::printf("all passed\n");
    return 0;
}
