#include "veyra/remoteplay/ControllerQueue.h"
#include <cstdio>

using namespace veyra::remoteplay;

static int failures = 0;
static void expect(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

static ControllerState pad(uint32_t buttons, int16_t rightX, uint8_t samples = 0, float firstGyro = 0) {
    ControllerState s;
    s.inputActive = true;
    s.buttons = buttons;
    s.rightX = rightX;
    s.motionSampleCount = samples;
    for (uint8_t i = 0; i < samples; ++i) s.motionSamples[i].gyro[0] = firstGyro + i;
    return s;
}

int main() {
    {
        ControllerQueue q;
        for (int16_t x = 0; x < 50; ++x) pushCoalesced(q, x, pad(0, x));
        expect(q.size() == 1, "50 stick-only updates during a stall: one queued state");
        expect(q.back().second.rightX == 49 && q.back().first == 49, "the queued state is the newest stick position");
    }
    {
        ControllerQueue q;
        pushCoalesced(q, 1, pad(0, 10));
        pushCoalesced(q, 2, pad(ControllerState::Cross, 11));
        pushCoalesced(q, 3, pad(ControllerState::Cross, 12));
        pushCoalesced(q, 4, pad(0, 13));
        pushCoalesced(q, 5, pad(0, 14));
        expect(q.size() == 3, "press then release: idle, pressed, released are all queued");
        expect(q[1].second.buttons == ControllerState::Cross && q[1].second.rightX == 12, "the pressed state carries the newest stick while held");
        expect(q[2].second.buttons == 0 && q[2].second.rightX == 14, "the release carries the newest stick");
    }
    {
        ControllerQueue q;
        ControllerState touched = pad(0, 0);
        touched.touches[0].id = 3;
        pushCoalesced(q, 1, pad(0, 0));
        pushCoalesced(q, 2, touched);
        expect(q.size() == 2, "a new touch contact is an edge and is queued");
    }
    {
        ControllerQueue q;
        ControllerState neutral;
        pushCoalesced(q, 1, pad(0, 5));
        pushCoalesced(q, 2, neutral);
        expect(q.size() == 2, "losing focus (inputActive false) is an edge");
    }
    {
        ControllerQueue q;
        pushCoalesced(q, 1, pad(0, 0, 10, 100));
        pushCoalesced(q, 2, pad(0, 0, 10, 200));
        const auto& s = q.back().second;
        expect(s.motionSampleCount == 16, "merged motion keeps the newest 16 samples");
        expect(s.motionSamples[0].gyro[0] == 104 && s.motionSamples[15].gyro[0] == 209, "motion samples stay in order, oldest dropped first");
    }
    std::printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
