// Hardware probe for the fork's controller path: runs the real ControllerInput against the
// pads connected to this PC for a few seconds and prints what it opens and reads.
// Usage: PadProbe.exe [seconds]
#include "veyra/remoteplay/ControllerInput.h"
#include "veyra/Log.h"
#include <SDL3/SDL_gamepad.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace veyra::log {
void info(const char* c, const std::string& m) { std::printf("[INFO ] [%s] %s\n", c, m.c_str()); }
void warn(const char* c, const std::string& m) { std::printf("[WARN ] [%s] %s\n", c, m.c_str()); }
void error(const char* c, const std::string& m) { std::printf("[ERROR] [%s] %s\n", c, m.c_str()); }
}

int main(int argc, char** argv) {
    const int seconds = argc > 1 ? std::atoi(argv[1]) : 3;
    veyra::remoteplay::ControllerInput input;
    if (!input.start()) { std::printf("SDL gamepad init failed: %s\n", SDL_GetError()); return 2; }
    uint32_t lastButtons = 0;
    bool reported = false;
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < end) {
        const auto state = input.poll(true);
        if (!reported && input.connected()) {
            reported = true;
            int count = 0;
            auto* ids = SDL_GetGamepads(&count);
            std::printf("pads seen by SDL: %d\n", count);
            for (int i = 0; i < count; ++i) {
                const char* name = SDL_GetGamepadNameForID(ids[i]);
                std::printf("  id=%u type=%d name=%s\n", ids[i], int(SDL_GetGamepadTypeForID(ids[i])), name ? name : "?");
            }
            SDL_free(ids);
            const auto caps = input.capabilities();
            std::printf("reading: %s gyro=%d touch=%d dualsense=%d\n", caps.name.c_str(), caps.gyro, caps.touch, caps.triggers);
        }
        if (state.inputActive && state.buttons != lastButtons) {
            std::printf("buttons=0x%04X leftX=%d\n", state.buttons, state.leftX);
            lastButtons = state.buttons;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }
    if (!input.connected()) std::printf("no pad opened in %d s\n", seconds);
    input.stop();
    return input.connected() ? 0 : 1;
}
