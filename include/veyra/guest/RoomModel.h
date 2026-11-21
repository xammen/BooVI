#pragma once
// Fork, partage à plusieurs: the room the host's BooVI keeps for a share. Several friends watch
// one link; one of them holds the PS5's controller. The host sets who may play (seat), passes the
// controller, and the room keeps a queue, a turn timer and a dice roll for the next player.
// GuestStreamer owns one Room and is its only writer; guests send commands on their "room" data
// channel and get a snapshot (snapshotJson) after every change. Events feed the play history and
// the Discord notices (GuestNotify.h).
// Pure: tested in tests/fork/RoomModelTests.cpp. Design: docs/notes/MULTI_GUEST_2026-11-21.md.
#include <algorithm>
#include <cstdint>
#include <deque>
#include <format>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace veyra::guest {

// Who may take the controller. The host sets it; a guest never does.
enum class Seat : uint8_t { Player, Watcher };

constexpr std::string_view seatName(Seat s) { return s == Seat::Player ? "player" : "watcher"; }
inline std::optional<Seat> parseSeat(std::string_view v) {
    if (v == "player") return Seat::Player;
    if (v == "watcher") return Seat::Watcher;
    return std::nullopt;
}

struct Member {
    std::string id;          // public, 6 characters: never the peer's signaling id
    std::string name;
    Seat seat = Seat::Player;
    bool native = false;     // joined from the BooVI app
    std::string accessId;    // the friend's access, empty for the share link
    int64_t joinedUs = 0;
    int64_t orphanUs = 0;    // since when no connection stands behind it (sweep), 0 while one does
};

// Why the controller changed hands.
enum class Handoff : uint8_t { Free, Queue, Host, Dice, Turn, Owner };
constexpr std::string_view handoffName(Handoff h) {
    switch (h) {
    case Handoff::Free: return "free";
    case Handoff::Queue: return "queue";
    case Handoff::Host: return "host";
    case Handoff::Dice: return "dice";
    case Handoff::Turn: return "turn";
    default: return "owner";
    }
}
// Why a holder stopped holding it.
enum class Release : uint8_t { Gave, Left, Idle, Seat, Host, Turn, Dice, Owner };

struct RoomEvent {
    enum class Kind : uint8_t {
        Joined, Left, PadTaken, PadReleased, PadFree, Queued, Unqueued, DiceRolled, OwnerTook, OwnerReleased, SeatChanged,
    };
    Kind kind;
    std::string name;        // the member concerned
    std::string other;       // DiceRolled: who launched it ("" the host)
    Handoff handoff = Handoff::Free;
    Release release = Release::Gave;
    int64_t heldUs = 0;      // PadReleased: how long they held it
    int position = 0;        // Queued: 1-based place
    Seat seat = Seat::Player;
};

struct DiceThrow { std::string id; int value = 0; };
struct DiceRoll {
    uint32_t n = 0;
    std::string by;                               // member id, "" the host
    std::vector<std::vector<DiceThrow>> rounds;   // every round's throws; ties throw again
    std::string winner;                           // member id
    int64_t atUs = 0, applyUs = 0;
    bool applied = false;
};

enum class RoomRefusal : uint8_t { None, Unknown, Watcher, Full, NotAllowed, TooSoon, TooFew, Busy };
constexpr std::string_view refusalName(RoomRefusal r) {
    switch (r) {
    case RoomRefusal::None: return "";
    case RoomRefusal::Unknown: return "unknown";
    case RoomRefusal::Watcher: return "watcher";
    case RoomRefusal::Full: return "full";
    case RoomRefusal::NotAllowed: return "not-allowed";
    case RoomRefusal::TooSoon: return "too-soon";
    case RoomRefusal::TooFew: return "too-few";
    default: return "busy";
    }
}

struct RoomSettings {
    int maxMembers = 4;
    int64_t turnUs = 0;          // 0: no turn timer
    bool diceAllowed = true;
};

class Room {
public:
    static constexpr int64_t kDiceShowUs = 3'000'000;      // the guests' animation, then the result applies
    static constexpr int64_t kDiceCooldownUs = 20'000'000; // between two rolls
    static constexpr int64_t kOwnerHoldUs = 120'000'000;   // the owner's pad silent this long: the guest gets it back
    // A uniform integer in [0, bound). The host passes BCryptGenRandom's; tests a seeded one.
    using Random = std::function<uint32_t(uint32_t bound)>;

    // A new turn length starts the holder's turn over.
    void configure(const RoomSettings& s) {
        if (s.turnUs != settings_.turnUs && holder_) heldSinceUs_ = lastNowUs_;
        settings_ = s;
        bump();
    }
    const RoomSettings& settings() const { return settings_; }

    // ---- members ----
    RoomRefusal join(Member m, int64_t nowUs) {
        lastNowUs_ = nowUs;
        if (int(members_.size()) >= settings_.maxMembers) return RoomRefusal::Full;
        m.name = uniqueName(m.name.empty() ? std::string("invité") : m.name);
        m.joinedUs = nowUs;
        members_.push_back(m);
        record({RoomEvent::Kind::Joined, m.name});
        if (m.seat == Seat::Player && !holder_ && !ownerHolds_ && queue_.empty()) take(m.id, Handoff::Free, nowUs);
        bump();
        return RoomRefusal::None;
    }
    void leave(const std::string& id, int64_t nowUs) {
        lastNowUs_ = nowUs;
        const Member* m = find(id);
        if (!m) return;
        const std::string name = m->name;
        dequeue(id, false);
        const bool held = holder_ == id;
        if (held) drop(Release::Left, nowUs);
        if (ownerWaiting_ == id) ownerWaiting_.clear();
        std::erase_if(members_, [&](const Member& x) { return x.id == id; });
        record({RoomEvent::Kind::Left, name});
        if (held) advance(nowUs);
        bump();
    }

    // Members with no connection behind them (`backed`: the ids that have one) leave once that
    // has lasted `graceUs`: a page whose reconnection failed. As with leave(), a controller they
    // held passes on. Returns the ids that left.
    std::vector<std::string> sweep(const std::vector<std::string>& backed, int64_t nowUs, int64_t graceUs) {
        std::vector<std::string> gone;
        for (auto& m : members_) {
            if (std::find(backed.begin(), backed.end(), m.id) != backed.end()) m.orphanUs = 0;
            else if (!m.orphanUs) m.orphanUs = nowUs;
            else if (nowUs - m.orphanUs >= graceUs) gone.push_back(m.id);
        }
        for (const auto& id : gone) leave(id, nowUs);
        return gone;
    }

    // ---- a guest's commands ----
    RoomRefusal ask(const std::string& id, int64_t nowUs) {
        lastNowUs_ = nowUs;
        const Member* m = find(id);
        if (!m) return RoomRefusal::Unknown;
        if (m->seat != Seat::Player) return RoomRefusal::Watcher;
        if (holder_ == id || inQueue(id)) return RoomRefusal::None;
        if (!holder_ && !ownerHolds_ && queue_.empty()) { take(id, Handoff::Free, nowUs); bump(); return RoomRefusal::None; }
        queue_.push_back(id);
        record({RoomEvent::Kind::Queued, m->name, {}, Handoff::Free, Release::Gave, 0, int(queue_.size())});
        bump();
        return RoomRefusal::None;
    }
    void cancel(const std::string& id) { if (dequeue(id, true)) bump(); }
    void release(const std::string& id, int64_t nowUs, Release why = Release::Gave) {
        lastNowUs_ = nowUs;
        if (holder_ != id) return;
        drop(why, nowUs);
        advance(nowUs);
        bump();
    }
    // Any player (or the host, by = "") rolls for the controller among every player in the room.
    RoomRefusal roll(const std::string& by, int64_t nowUs, const Random& random) {
        lastNowUs_ = nowUs;
        if (!settings_.diceAllowed && !by.empty()) return RoomRefusal::NotAllowed;
        if (!by.empty()) {
            const Member* m = find(by);
            if (!m) return RoomRefusal::Unknown;
            if (m->seat != Seat::Player) return RoomRefusal::Watcher;
        }
        if (dice_ && !dice_->applied) return RoomRefusal::Busy;
        if (dice_ && nowUs - dice_->atUs < kDiceCooldownUs) return RoomRefusal::TooSoon;
        std::vector<std::string> players;
        for (const auto& m : members_) if (m.seat == Seat::Player) players.push_back(m.id);
        if (players.size() < 2) return RoomRefusal::TooFew;
        DiceRoll d;
        d.n = ++diceSeq_;
        d.by = by;
        d.atUs = nowUs;
        d.applyUs = nowUs + kDiceShowUs;
        std::vector<std::string> left = players;
        for (int round = 0; left.size() > 1 && round < 64; ++round) {
            std::vector<DiceThrow> throws;
            int best = 0;
            for (const auto& id : left) {
                const int v = int(random(6)) + 1;
                throws.push_back({id, v});
                best = std::max(best, v);
            }
            left.clear();
            for (const auto& t : throws) if (t.value == best) left.push_back(t.id);
            d.rounds.push_back(std::move(throws));
        }
        d.winner = left.size() == 1 ? left.front() : left[random(uint32_t(left.size()))];
        const Member* w = find(d.winner);
        record({RoomEvent::Kind::DiceRolled, w ? w->name : std::string(), by.empty() ? std::string() : find(by)->name});
        dice_ = std::move(d);
        bump();
        return RoomRefusal::None;
    }

    // ---- the host ----
    void give(const std::string& id, int64_t nowUs) {
        lastNowUs_ = nowUs;
        if (!find(id) || holder_ == id) return;
        if (ownerHolds_) { ownerHolds_ = false; record({RoomEvent::Kind::OwnerReleased, {}}); }
        if (holder_) drop(Release::Host, nowUs);
        dequeue(id, false);
        take(id, Handoff::Host, nowUs);
        bump();
    }
    void setSeat(const std::string& id, Seat seat, int64_t nowUs) {
        lastNowUs_ = nowUs;
        Member* m = member(id);
        if (!m || m->seat == seat) return;
        m->seat = seat;
        record({RoomEvent::Kind::SeatChanged, m->name, {}, Handoff::Free, Release::Gave, 0, 0, seat});
        if (seat == Seat::Watcher) {
            dequeue(id, false);
            if (holder_ == id) { drop(Release::Seat, nowUs); advance(nowUs); }
        } else if (!holder_ && !ownerHolds_ && queue_.empty()) take(id, Handoff::Free, nowUs);
        bump();
    }
    // The owner plays on his own pad: the holder waits at the queue's front.
    void ownerTake(int64_t nowUs) {
        lastNowUs_ = nowUs;
        ownerSeenUs_ = nowUs;
        if (ownerHolds_) return;
        ownerHolds_ = true;
        if (holder_) {
            const std::string id = *holder_;
            drop(Release::Owner, nowUs);
            queue_.push_front(id);
            ownerWaiting_ = id;
        }
        record({RoomEvent::Kind::OwnerTook, {}});
        bump();
    }
    void ownerActive(int64_t nowUs) { if (ownerHolds_) ownerSeenUs_ = nowUs; }
    void ownerRelease(int64_t nowUs) {
        lastNowUs_ = nowUs;
        if (!ownerHolds_) return;
        ownerHolds_ = false;
        ownerWaiting_.clear();
        record({RoomEvent::Kind::OwnerReleased, {}});
        advance(nowUs);
        bump();
    }

    // The control loop, every 100 ms: the dice's result, the turn timer, the owner's silence.
    void tick(int64_t nowUs) {
        lastNowUs_ = nowUs;
        if (dice_ && !dice_->applied && nowUs >= dice_->applyUs) {
            dice_->applied = true;
            const std::string winner = dice_->winner;
            if (find(winner) && holder_ != winner) {
                if (ownerHolds_) { ownerHolds_ = false; ownerWaiting_.clear(); record({RoomEvent::Kind::OwnerReleased, {}}); }
                std::optional<std::string> loser = holder_;
                if (holder_) drop(Release::Dice, nowUs);
                dequeue(winner, false);
                take(winner, Handoff::Dice, nowUs);
                if (loser && find(*loser) && find(*loser)->seat == Seat::Player) queue_.push_back(*loser);
            }
            bump();
        }
        if (ownerHolds_ && nowUs - ownerSeenUs_ >= kOwnerHoldUs) ownerRelease(nowUs);
        if (settings_.turnUs > 0 && holder_ && nowUs - heldSinceUs_ >= settings_.turnUs && nextInQueue()) {
            const std::string id = *holder_;
            drop(Release::Turn, nowUs);
            queue_.push_back(id);
            advance(nowUs, Handoff::Turn);
            bump();
        }
    }

    // ---- reading ----
    const std::vector<Member>& members() const { return members_; }
    const Member* find(const std::string& id) const {
        for (const auto& m : members_) if (m.id == id) return &m;
        return nullptr;
    }
    std::optional<std::string> holder() const { return holder_; }
    int64_t heldSinceUs() const { return heldSinceUs_; }
    bool ownerHolds() const { return ownerHolds_; }
    const std::deque<std::string>& queue() const { return queue_; }
    int queuePosition(const std::string& id) const {
        for (size_t i = 0; i < queue_.size(); ++i) if (queue_[i] == id) return int(i) + 1;
        return 0;
    }
    const std::optional<DiceRoll>& dice() const { return dice_; }
    uint64_t revision() const { return rev_; }
    bool full() const { return int(members_.size()) >= settings_.maxMembers; }
    std::vector<RoomEvent> drainEvents() { return std::exchange(events_, {}); }

private:
    Member* member(const std::string& id) {
        for (auto& m : members_) if (m.id == id) return &m;
        return nullptr;
    }
    bool inQueue(const std::string& id) const { return std::find(queue_.begin(), queue_.end(), id) != queue_.end(); }
    bool dequeue(const std::string& id, bool announce) {
        const auto it = std::find(queue_.begin(), queue_.end(), id);
        if (it == queue_.end()) return false;
        queue_.erase(it);
        if (announce) if (const Member* m = find(id)) record({RoomEvent::Kind::Unqueued, m->name});
        return true;
    }
    std::optional<std::string> nextInQueue() const {
        for (const auto& id : queue_) if (const Member* m = find(id); m && m->seat == Seat::Player) return id;
        return std::nullopt;
    }
    void take(const std::string& id, Handoff why, int64_t nowUs) {
        holder_ = id;
        heldSinceUs_ = nowUs;
        if (const Member* m = find(id)) record({RoomEvent::Kind::PadTaken, m->name, {}, why});
    }
    void drop(Release why, int64_t nowUs) {
        if (!holder_) return;
        if (const Member* m = find(*holder_)) record({RoomEvent::Kind::PadReleased, m->name, {}, Handoff::Free, why, nowUs - heldSinceUs_});
        holder_.reset();
    }
    void advance(int64_t nowUs, Handoff why = Handoff::Queue) {
        if (ownerHolds_ || holder_) return;
        while (!queue_.empty()) {
            const std::string id = queue_.front();
            queue_.pop_front();
            if (const Member* m = find(id); m && m->seat == Seat::Player) { take(id, why, nowUs); return; }
        }
        record({RoomEvent::Kind::PadFree, {}});
    }
    std::string uniqueName(std::string name) const {
        auto taken = [&](const std::string& n) { return std::any_of(members_.begin(), members_.end(), [&](const Member& m) { return m.name == n; }); };
        if (!taken(name)) return name;
        for (int i = 2;; ++i) if (std::string n = std::format("{} {}", name, i); !taken(n)) return n;
    }
    void record(RoomEvent e) { if (events_.size() < 256) events_.push_back(std::move(e)); }
    void bump() { ++rev_; }

    RoomSettings settings_;
    std::vector<Member> members_;
    std::optional<std::string> holder_;
    int64_t heldSinceUs_ = 0, ownerSeenUs_ = 0, lastNowUs_ = 0;
    bool ownerHolds_ = false;
    std::string ownerWaiting_;
    std::deque<std::string> queue_;
    std::optional<DiceRoll> dice_;
    uint32_t diceSeq_ = 0;
    uint64_t rev_ = 0;
    std::vector<RoomEvent> events_;
};

// ---- the host's upload, shared among the viewers ----
// The holder gets what is left after a reserve for the spectators (half the upload split among
// them, 4 to 10 Mb/s each); the spectators split the rest. An upload too small for 4 Mb/s each
// is split evenly. Each share is also held to the per-viewer most, and together they never pass
// the upload. `holder`: index of the holder in the viewers, -1 none.
inline std::vector<uint32_t> shareUpload(uint32_t totalKbps, uint32_t perViewerMaxKbps, size_t viewers, int holder) {
    constexpr uint32_t kFloor = 4000, kSpectatorCap = 10000;
    std::vector<uint32_t> out(viewers, 0);
    if (viewers == 0) return out;
    const uint32_t n = uint32_t(viewers);
    if (holder < 0 || size_t(holder) >= viewers || uint64_t(kFloor) * n > totalKbps) {
        std::fill(out.begin(), out.end(), std::min(perViewerMaxKbps, totalKbps / n));
        return out;
    }
    const uint32_t others = n - 1;
    const uint32_t reserveEach = others ? std::clamp(totalKbps / 2 / others, kFloor, kSpectatorCap) : 0;
    const uint32_t holderKbps = std::min(perViewerMaxKbps, totalKbps - reserveEach * others);
    const uint32_t each = others ? std::min(perViewerMaxKbps, (totalKbps - holderKbps) / others) : 0;
    for (size_t i = 0; i < viewers; ++i) out[i] = int(i) == holder ? holderKbps : each;
    return out;
}

// ---- the snapshot a guest gets ----
inline std::string jsonText(std::string_view s) {
    std::string out = "\"";
    for (const char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (uint8_t(c) < 0x20) out += std::format("\\u{:04x}", unsigned(uint8_t(c)));
            else out += c;
        }
    }
    return out + "\"";
}

struct PlayRecord { std::string name; int64_t atUnix = 0; int64_t seconds = 0; };

// `you`: the receiving member's id. `history`: the last plays, newest first; empty when the host
// does not show it to guests.
inline std::string snapshotJson(const Room& room, const std::string& you, int64_t nowUs, const std::vector<PlayRecord>& history = {}) {
    std::string out = std::format(R"({{"t":"room","rev":{},"you":{},"members":[)", room.revision(), jsonText(you));
    bool first = true;
    for (const auto& m : room.members()) {
        out += std::format(R"({}{{"id":{},"name":{},"seat":"{}","native":{},"friend":{}}})", first ? "" : ",", jsonText(m.id), jsonText(m.name),
                           seatName(m.seat), m.native ? 1 : 0, m.accessId.empty() ? 0 : 1);
        first = false;
    }
    const auto holder = room.holder();
    const auto& s = room.settings();
    const int64_t held = holder ? (nowUs - room.heldSinceUs()) / 1000000 : 0;
    const int64_t left = holder && s.turnUs > 0 ? std::max<int64_t>(0, (s.turnUs - (nowUs - room.heldSinceUs())) / 1000000) : -1;
    out += std::format(R"(],"holder":{},"owner":{},"held":{},"queue":[)", jsonText(holder.value_or("")), room.ownerHolds() ? 1 : 0, held);
    first = true;
    for (const auto& id : room.queue()) { out += (first ? "" : ",") + jsonText(id); first = false; }
    out += std::format(R"(],"turn":{},"left":{},"allowDice":{},"max":{},"dice":)", s.turnUs / 1000000, left, s.diceAllowed ? 1 : 0, s.maxMembers);
    if (const auto& d = room.dice()) {
        out += std::format(R"({{"n":{},"by":{},"winner":{},"in":{},"rounds":[)", d->n, jsonText(d->by), jsonText(d->winner),
                           std::max<int64_t>(0, (d->applyUs - nowUs) / 1000));
        for (size_t r = 0; r < d->rounds.size(); ++r) {
            out += r ? ",[" : "[";
            for (size_t i = 0; i < d->rounds[r].size(); ++i)
                out += std::format(R"({}{{"id":{},"v":{}}})", i ? "," : "", jsonText(d->rounds[r][i].id), d->rounds[r][i].value);
            out += "]";
        }
        out += "]}";
    } else out += "null";
    out += R"(,"history":[)";
    for (size_t i = 0; i < history.size(); ++i)
        out += std::format(R"({}{{"name":{},"at":{},"s":{}}})", i ? "," : "", jsonText(history[i].name), history[i].atUnix, history[i].seconds);
    return out + "]}";
}

// A guest's command on the "room" channel: {"t": "ask" | "cancel" | "release" | "roll"}.
enum class RoomCommand : uint8_t { None, Ask, Cancel, Release, Roll };
inline RoomCommand roomCommandOf(std::string_view t) {
    if (t == "ask") return RoomCommand::Ask;
    if (t == "cancel") return RoomCommand::Cancel;
    if (t == "release") return RoomCommand::Release;
    if (t == "roll") return RoomCommand::Roll;
    return RoomCommand::None;
}

// A name a guest typed: printable, no markup characters, at most 24 characters (UTF-8 kept whole).
inline std::string cleanGuestName(std::string_view raw) {
    std::string out;
    size_t chars = 0;
    bool space = false;
    for (size_t i = 0; i < raw.size() && chars < 24;) {
        const uint8_t c = uint8_t(raw[i]);
        const size_t len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (!len || i + len > raw.size()) { ++i; continue; }
        if (len == 1) {
            if (c < 0x20 || c == 0x7F || c == '<' || c == '>' || c == '"' || c == '&' || c == '`' || c == '\\' || c == '@') { ++i; continue; }
            if (c == ' ') { space = !out.empty(); ++i; continue; }
        }
        if (space) { out += ' '; ++chars; space = false; }
        out.append(raw.substr(i, len));
        ++chars;
        i += len;
    }
    return out;
}

} // namespace veyra::guest
