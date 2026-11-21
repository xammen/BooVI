#pragma once
// Fork: callbacks that another library's threads may run after their owner is gone. libdatachannel
// keeps a viewer's connection, and the callbacks it holds, alive until the teardown thread
// (GuestStreamer.cpp, PeerReaper) destroys it, which can outlive the streamer itself. A callback
// made with guard() reaches its owner only while the owner lives: cut() waits for the ones running
// now, and every later one does nothing.
// Pure: tested in tests/fork/LifelineTests.cpp.
#include <memory>
#include <shared_mutex>
#include <utility>

namespace veyra::guest {

template <class Owner>
class Lifeline {
public:
    explicit Lifeline(Owner& owner) : s_(std::make_shared<State>()) { s_->owner = &owner; }
    ~Lifeline() { cut(); }
    Lifeline(const Lifeline&) = delete;
    Lifeline& operator=(const Lifeline&) = delete;

    // Not from inside a guarded callback.
    void cut() {
        std::unique_lock lock(s_->m);
        s_->owner = nullptr;
    }
    // f(owner, args...) while the owner lives; nothing once cut.
    template <class F>
    auto guard(F f) const {
        return [s = s_, f = std::move(f)](auto&&... args) {
            std::shared_lock lock(s->m);
            if (s->owner) f(*s->owner, std::forward<decltype(args)>(args)...);
        };
    }

private:
    struct State {
        std::shared_mutex m;
        Owner* owner = nullptr;
    };
    std::shared_ptr<State> s_;
};

} // namespace veyra::guest
