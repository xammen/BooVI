#include "veyra/guest/RoomModel.h"
#include <cstdio>
#include <random>

using namespace veyra::guest;

static int failures = 0;
static void expect(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

static Member member(const char* id, const char* name, Seat seat = Seat::Player) { return Member{id, name, seat}; }
static bool has(const std::vector<RoomEvent>& events, RoomEvent::Kind kind, const char* name = nullptr) {
    for (const auto& e : events) if (e.kind == kind && (!name || e.name == name)) return true;
    return false;
}

int main() {
    const int64_t s = 1000000;
    std::mt19937 engine(7);
    const Room::Random fair = [&](uint32_t bound) { return std::uniform_int_distribution<uint32_t>(0, bound - 1)(engine); };

    {
        Room r;
        r.join(member("a", "lucas"), 0);
        expect(r.holder() == "a", "the first player into a free room takes the controller");
        r.join(member("b", "sam"), s);
        expect(r.holder() == "a", "a second player does not take it");
        expect(r.ask("b", 2 * s) == RoomRefusal::None && r.queuePosition("b") == 1, "asking while it is held queues, first place");
        r.join(member("c", "zoé", Seat::Watcher), 3 * s);
        expect(r.ask("c", 3 * s) == RoomRefusal::Watcher && r.queuePosition("c") == 0, "a watcher cannot ask");
        r.leave("a", 10 * s);
        expect(r.holder() == "b" && r.queue().empty(), "the holder leaving passes it to the queue's head");
        const auto events = r.drainEvents();
        bool released = false;
        for (const auto& e : events) released |= e.kind == RoomEvent::Kind::PadReleased && e.name == "lucas" && e.release == Release::Left && e.heldUs == 10 * s;
        expect(released, "the play time ends when the holder leaves (10 s)");
        expect(has(events, RoomEvent::Kind::PadTaken, "sam"), "sam takes it");
    }
    {
        Room r;
        r.join(member("a", "lucas"), 0);
        r.join(member("b", "lucas"), 0);
        expect(r.members()[1].name == "lucas 2", "two guests with one name are told apart");
        expect(r.join(member("c", "c"), 0) == RoomRefusal::None && r.join(member("d", "d"), 0) == RoomRefusal::None, "four fit");
        expect(r.join(member("e", "e"), 0) == RoomRefusal::Full && r.members().size() == 4, "a fifth is refused");
    }
    {
        Room r;
        r.join(member("a", "a"), 0);
        r.join(member("b", "b"), 0);
        r.release("a", s);
        expect(!r.holder(), "released with nobody waiting: the controller is free");
        expect(has(r.drainEvents(), RoomEvent::Kind::PadFree), "and that is said");
        r.ask("b", 2 * s);
        expect(r.holder() == "b", "a free controller goes to whoever asks");
        r.setSeat("b", Seat::Watcher, 3 * s);
        expect(!r.holder(), "made a watcher, the holder loses it");
        r.give("b", 4 * s);
        expect(r.holder() == "b", "the host can still hand it to a watcher");
        r.ask("a", 4 * s);
        r.cancel("a");
        expect(r.queue().empty(), "cancel leaves the queue");
    }
    {
        Room r;
        r.configure({4, 60 * s, true});
        r.join(member("a", "a"), 0);
        r.join(member("b", "b"), 0);
        r.tick(120 * s);
        expect(r.holder() == "a", "the turn timer does nothing while nobody waits");
        r.ask("b", 121 * s);
        r.tick(121 * s);
        expect(r.holder() == "b" && r.queuePosition("a") == 1, "past the turn with someone waiting: rotate, the holder to the back");
        r.tick(121 * s + 59 * s);
        expect(r.holder() == "b", "the new holder gets a full turn");
        r.tick(121 * s + 60 * s);
        expect(r.holder() == "a" && r.queuePosition("b") == 1, "and then it rotates back");
    }
    {
        Room r;
        r.join(member("a", "a"), 0);
        r.join(member("b", "b"), 0);
        r.ask("b", 0);
        r.ownerTake(s);
        expect(!r.holder() && r.ownerHolds() && r.queue().front() == "a", "the owner plays: the holder waits at the queue's front");
        expect(r.ask("a", 2 * s) == RoomRefusal::None && r.queuePosition("a") == 1, "asking again keeps its place");
        r.ownerActive(60 * s);
        r.tick(60 * s + Room::kOwnerHoldUs - 1);
        expect(r.ownerHolds(), "the owner keeps it while his pad is used");
        r.tick(60 * s + Room::kOwnerHoldUs);
        expect(!r.ownerHolds() && r.holder() == "a" && r.queuePosition("b") == 1, "two minutes without the owner's pad: back to the guest");
        r.ownerTake(200 * s);
        r.ownerRelease(201 * s);
        expect(r.holder() == "a", "the owner hands it back from the panel");
    }
    {
        Room r;
        r.join(member("a", "a"), 0);
        expect(r.roll("a", 0, fair) == RoomRefusal::TooFew, "dice need two players");
        r.join(member("b", "b"), 0);
        r.join(member("w", "w", Seat::Watcher), 0);
        expect(r.roll("w", 0, fair) == RoomRefusal::Watcher, "a watcher cannot roll");
        expect(r.roll("a", s, fair) == RoomRefusal::None, "a player rolls");
        const auto d = *r.dice();
        bool onlyPlayers = true;
        for (const auto& t : d.rounds.front()) onlyPlayers &= t.id != "w" && t.value >= 1 && t.value <= 6;
        expect(onlyPlayers && d.rounds.front().size() == 2, "every player throws a d6, watchers do not");
        expect(r.roll("b", s, fair) == RoomRefusal::Busy, "one roll at a time");
        r.tick(s + Room::kDiceShowUs - 1);
        expect(r.holder() == "a", "the result waits for the animation");
        r.tick(s + Room::kDiceShowUs);
        expect(r.holder() == d.winner, "then the winner holds the controller");
        if (d.winner == "b") expect(r.queuePosition("a") == 1, "the holder who lost waits at the queue's back");
        expect(r.roll("a", s + Room::kDiceShowUs, fair) == RoomRefusal::TooSoon, "a cooldown between rolls");
        r.configure({4, 0, false});
        expect(r.roll("a", 100 * s, fair) == RoomRefusal::NotAllowed, "the host can turn the dice off");
        expect(r.roll("", 100 * s, fair) == RoomRefusal::None, "the host still rolls");
    }
    {
        // Ties throw again: a fixed sequence 6, 6, then 2, 5.
        Room r;
        r.join(member("a", "a"), 0);
        r.join(member("b", "b"), 0);
        std::vector<uint32_t> seq{5, 5, 1, 4};
        size_t i = 0;
        r.roll("", 0, [&](uint32_t) { return seq[i++ % seq.size()]; });
        const auto d = *r.dice();
        expect(d.rounds.size() == 2 && d.winner == "b", "a tie on 6 throws again; 5 beats 2");
        const std::string json = snapshotJson(r, "a", 0);
        expect(json.find(R"("rounds":[[{"id":"a","v":6},{"id":"b","v":6}],[{"id":"a","v":2},{"id":"b","v":5}]])") != std::string::npos, "the snapshot carries every round");
    }
    {
        // Fairness: each of four players wins a quarter of the rolls (60,000 rolls, 3 sigma ~ 0.5 %).
        int wins[4]{};
        for (int k = 0; k < 60000; ++k) {
            Room r;
            r.configure({4, 0, true});
            const char* ids[] = {"a", "b", "c", "d"};
            for (const char* id : ids) r.join(member(id, id), 0);
            r.roll("", 0, fair);
            ++wins[r.dice()->winner[0] - 'a'];
        }
        bool even = true;
        for (int w : wins) even &= w > 15000 - 600 && w < 15000 + 600;
        std::printf("     wins %d %d %d %d\n", wins[0], wins[1], wins[2], wins[3]);
        expect(even, "dice: every player wins about 1/4, whoever joined first");
    }
    {
        // A page reconnected, then its new connection failed: its member has nothing behind it.
        Room r;
        r.join(member("a", "lucas"), 0);
        r.join(member("b", "sam"), 0);
        r.ask("b", s);
        const std::vector<std::string> onlySam{"b"};
        expect(r.sweep(onlySam, 2 * s, 20 * s).empty() && r.members().size() == 2, "sweep: a member just left without a connection stays (grace)");
        expect(r.sweep(onlySam, 21 * s, 20 * s).empty(), "sweep: still within the grace");
        const auto gone = r.sweep(onlySam, 22 * s, 20 * s);
        expect(gone == std::vector<std::string>{"a"} && r.members().size() == 1, "sweep: past the grace it leaves");
        expect(r.holder() == "b", "sweep: the controller it held goes to who waited");
        Room back;
        back.join(member("a", "lucas"), 0);
        back.sweep({}, s, 20 * s);
        back.sweep({"a"}, 10 * s, 20 * s);
        expect(back.sweep({}, 25 * s, 20 * s).empty() && back.members().size() == 1, "sweep: a connection back in time starts the grace over");
    }
    {
        expect(shareUpload(50000, 50000, 1, 0) == std::vector<uint32_t>{50000}, "one viewer: the whole upload, as before");
        const auto four = shareUpload(50000, 50000, 4, 2);
        expect(four[2] >= 25000 && four[2] <= 25010 && four[0] == 8333 && four[1] == four[3], "four viewers: the holder 25, the others about 8.3 each");
        uint32_t sum = 0; for (auto k : four) sum += k;
        expect(sum <= 50000, "never more than the host's upload");
        const auto fits = shareUpload(20000, 20000, 3, 0);
        expect(fits[1] >= 4000, "a spectator gets 4 Mb/s when the upload holds that for everyone");
        const auto low = shareUpload(10000, 10000, 4, 0);
        expect(low[0] == 2500 && low[1] == 2500 && low[3] == 2500, "too small for 4 Mb/s each: an even split");
        const auto six = shareUpload(20000, 20000, 6, 1);
        uint32_t sixSum = 0; for (auto k : six) sixSum += k;
        expect(sixSum <= 20000, "20 Mb/s among 6 viewers stays within 20 Mb/s");
        const auto none = shareUpload(30000, 50000, 3, -1);
        expect(none[0] == 10000 && none[2] == 10000, "nobody holds it: an even split");
        bool within = true;
        for (uint32_t total = 0; total <= 120000; total += 700)
            for (uint32_t most : {3000u, 8000u, 25000u, 200000u})
                for (size_t n = 0; n <= 6; ++n)
                    for (int h = -1; h < int(n); ++h) {
                        uint64_t got = 0;
                        for (auto k : shareUpload(total, most, n, h)) { got += k; within &= k <= most; }
                        within &= got <= total;
                    }
        expect(within, "any upload, any count, any holder: the shares never pass the upload or the per-viewer most");
    }
    {
        Room r;
        r.join(member("a", "lu\"cas"), 0);
        const std::string json = snapshotJson(r, "a", 5 * 1000000, {{"sam", 1700000000, 600}});
        expect(json.find(R"("name":"lu\"cas")") != std::string::npos, "names are escaped");
        expect(json.find(R"("holder":"a")") != std::string::npos && json.find(R"("held":5)") != std::string::npos, "holder and time held");
        expect(json.find(R"("history":[{"name":"sam","at":1700000000,"s":600}])") != std::string::npos, "history when shown");
        expect(roomCommandOf("roll") == RoomCommand::Roll && roomCommandOf("give") == RoomCommand::None, "a guest cannot send host commands");
        expect(cleanGuestName("  <b>lucas</b>  le   grand ") == "blucas/b le grand", "names lose markup and extra spaces");
        expect(cleanGuestName("zoé@x") == "zoéx", "accents kept, @ dropped (no Discord mention)");
        expect(cleanGuestName("abcdefghijklmnopqrstuvwxyz").size() == 24, "24 characters at most");
    }
    std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
