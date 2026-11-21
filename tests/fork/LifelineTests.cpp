// Callbacks that outlive their owner (Lifeline.h), as libdatachannel's outlive the streamer.
#include "veyra/guest/Lifeline.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <optional>
#include <thread>

using veyra::guest::Lifeline;

static int failures = 0;
static void expect(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

struct Owner {
    int calls = 0;
    std::atomic<bool> inside{false}, finished{false};
};

int main() {
    {
        Owner owner;
        Lifeline<Owner> life(owner);
        std::function<void(int)> callback = life.guard([](Owner& o, int n) { o.calls += n; });
        callback(2);
        expect(owner.calls == 2, "a callback reaches its owner while it lives");
        life.cut();
        callback(5);
        expect(owner.calls == 2, "once cut, a callback does nothing");
    }
    {
        int reached = 0;
        std::function<void()> late;
        {
            auto owner = std::make_unique<Owner>();
            std::optional<Lifeline<Owner>> life(std::in_place, *owner);
            late = life->guard([&reached](Owner& o) { ++o.calls; ++reached; });
            late();
            life.reset();
            owner.reset();
        }
        late();
        expect(reached == 1, "a callback run after its owner and its lifeline were destroyed does not reach the owner");
    }
    {
        Owner owner;
        Lifeline<Owner> life(owner);
        auto slow = life.guard([](Owner& o) {
            o.inside = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            o.finished = true;
        });
        std::thread t(slow);
        while (!owner.inside) std::this_thread::yield();
        life.cut();
        expect(owner.finished, "cut waits for a callback running on another thread");
        t.join();
    }
    std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
