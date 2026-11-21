#pragma once
// Fork, partage à plusieurs: what the host's BooVI tells a Discord channel (a webhook the owner
// pasted in réglages → partage, kept sealed in <user data>\guest\discord.dpapi). Each kind of
// event has its own switch (`discordEvents`, a bit mask). NotifyGate keeps the channel calm:
// lines wait 1.5 s so a burst goes out as one message, at most one message per 3 s and 20 a
// minute, and Discord's own retry_after is honoured.
// Pure: tested in tests/fork/GuestNotifyTests.cpp.
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <deque>
#include <format>
#include <optional>
#include <string>
#include <string_view>

#include "veyra/guest/RoomModel.h"

namespace veyra::guest {

enum class Notice : uint8_t { ShareStarted, ShareStopped, ShareStuck, Joined, Left, PadTaken, PadFree, Queue, Dice };
constexpr uint32_t noticeBit(Notice n) { return 1u << uint32_t(n); }
// "ps5 libre", connected, controller, dice and stuck by default; the share's start and stop and
// the queue's moves only when asked.
constexpr uint32_t kDefaultNotices = noticeBit(Notice::ShareStuck) | noticeBit(Notice::Joined) | noticeBit(Notice::PadTaken) |
                                     noticeBit(Notice::PadFree) | noticeBit(Notice::Dice);
constexpr uint32_t kAllNotices = (1u << 9) - 1;

constexpr std::string_view noticeName(Notice n) {
    switch (n) {
    case Notice::ShareStarted: return "share-started";
    case Notice::ShareStopped: return "share-stopped";
    case Notice::ShareStuck: return "share-stuck";
    case Notice::Joined: return "joined";
    case Notice::Left: return "left";
    case Notice::PadTaken: return "pad";
    case Notice::PadFree: return "free";
    case Notice::Queue: return "queue";
    default: return "dice";
    }
}

// A name as Discord shows it: no mention, no markdown.
inline std::string discordName(std::string_view name) {
    std::string out;
    for (const char c : cleanGuestName(name)) {
        if (std::string_view("*_~`|>#[]():").find(c) != std::string_view::npos) out += '\\';
        out += c;
    }
    return out.empty() ? std::string("un invité") : out;
}

struct NoticeLine { Notice kind; std::string text; };

// The line a room event becomes, when it is one Discord hears about.
inline std::optional<NoticeLine> noticeFor(const RoomEvent& e) {
    const std::string n = discordName(e.name);
    switch (e.kind) {
    case RoomEvent::Kind::Joined: return NoticeLine{Notice::Joined, std::format("{} est connecté(e)", n)};
    case RoomEvent::Kind::Left: return NoticeLine{Notice::Left, std::format("{} est parti(e)", n)};
    case RoomEvent::Kind::PadTaken: {
        const char* how = e.handoff == Handoff::Dice ? " · gagné aux dés" : e.handoff == Handoff::Turn ? " · à son tour"
                        : e.handoff == Handoff::Host ? " · passée par l'hôte" : e.handoff == Handoff::Queue ? " · c'était son tour dans la file" : "";
        return NoticeLine{Notice::PadTaken, std::format("🎮 {} a la manette{}", n, how)};
    }
    case RoomEvent::Kind::PadFree: return NoticeLine{Notice::PadFree, "ps5 libre : personne n'a la manette"};
    case RoomEvent::Kind::Queued: return NoticeLine{Notice::Queue, std::format("{} attend la manette (n° {} dans la file)", n, e.position)};
    case RoomEvent::Kind::Unqueued: return NoticeLine{Notice::Queue, std::format("{} quitte la file", n)};
    case RoomEvent::Kind::DiceRolled:
        return NoticeLine{Notice::Dice, e.other.empty() ? std::format("🎲 {} gagne aux dés", n) : std::format("🎲 {} gagne aux dés (lancés par {})", n, discordName(e.other))};
    case RoomEvent::Kind::OwnerTook: return NoticeLine{Notice::PadTaken, "🎮 l'hôte reprend la manette"};
    default: return std::nullopt;
    }
}

inline std::string noticeLine(Notice n) {
    switch (n) {
    case Notice::ShareStarted: return "partage ouvert";
    case Notice::ShareStopped: return "partage arrêté";
    case Notice::ShareStuck: return "⚠️ le partage ne répond plus, il redémarre";
    default: return {};
    }
}

// The webhook's JSON body: no mention can ping anyone, whatever a name holds.
inline std::string webhookBody(std::string_view content) {
    return std::format(R"({{"username":"BooVI","content":{},"allowed_mentions":{{"parse":[]}}}})", jsonText(content));
}

// A Discord webhook URL: https://discord.com/api/webhooks/<digits>/<token> (also discordapp.com,
// ptb., canary.). `allowLocal` (tests only) also takes http://127.0.0.1:<port>/<path>.
inline bool webhookUrlOk(std::string_view url, bool allowLocal = false) {
    if (allowLocal && url.starts_with("http://127.0.0.1:") && url.size() < 200) return true;
    std::string_view rest;
    for (std::string_view host : {"https://discord.com/api/webhooks/", "https://discordapp.com/api/webhooks/", "https://ptb.discord.com/api/webhooks/",
                                  "https://canary.discord.com/api/webhooks/"})
        if (url.starts_with(host)) { rest = url.substr(host.size()); break; }
    const size_t slash = rest.find('/');
    if (rest.empty() || slash == std::string_view::npos || slash == 0 || slash > 24) return false;
    for (size_t i = 0; i < slash; ++i) if (rest[i] < '0' || rest[i] > '9') return false;
    const std::string_view token = rest.substr(slash + 1);
    if (token.size() < 20 || token.size() > 100) return false;
    for (const char c : token) if (!(std::isalnum(uint8_t(c)) || c == '-' || c == '_')) return false;
    return true;
}

class NotifyGate {
public:
    static constexpr int64_t kGatherMs = 1500;     // a line waits this long for the ones of the same burst
    static constexpr int64_t kSpacingMs = 3000;    // between two messages
    static constexpr int kPerMinute = 20;
    static constexpr size_t kMaxLines = 12;        // in one message; the rest is counted
    static constexpr size_t kMaxPending = 60;

    void push(std::string line, int64_t nowMs) {
        if (line.empty()) return;
        if (!pending_.empty() && pending_.back() == line) return;   // the same line twice in a row
        if (pending_.empty()) firstMs_ = nowMs;
        if (pending_.size() >= kMaxPending) { pending_.pop_front(); ++dropped_; }
        pending_.push_back(std::move(line));
    }
    // The message to send now, if one is due.
    std::optional<std::string> take(int64_t nowMs) {
        while (!sent_.empty() && nowMs - sent_.front() >= 60000) sent_.pop_front();
        if (pending_.empty() || nowMs - firstMs_ < kGatherMs || nowMs < notBeforeMs_) return std::nullopt;
        if (!sent_.empty() && nowMs - sent_.back() < kSpacingMs) return std::nullopt;
        if (int(sent_.size()) >= kPerMinute) return std::nullopt;
        std::string out;
        size_t n = 0;
        while (!pending_.empty() && n < kMaxLines) {
            if (out.size() + pending_.front().size() + 1 > 1900) break;
            out += (n ? "\n" : "") + pending_.front();
            pending_.pop_front();
            ++n;
        }
        const size_t more = pending_.size() + dropped_;
        if (pending_.size() > 0 && n == kMaxLines) { out += std::format("\n… et {} de plus", more); pending_.clear(); }
        else if (dropped_) out += std::format("\n… et {} de plus", dropped_);
        dropped_ = 0;
        firstMs_ = nowMs;
        sent_.push_back(nowMs);
        return out;
    }
    // Discord said 429: nothing before retry_after; the message comes back to the front.
    void rateLimited(const std::string& message, int64_t retryAfterMs, int64_t nowMs) {
        notBeforeMs_ = nowMs + std::clamp<int64_t>(retryAfterMs, 1000, 600000);
        if (!sent_.empty()) sent_.pop_back();
        pending_.push_front(message);
        firstMs_ = nowMs - kGatherMs;
    }
    size_t pending() const { return pending_.size(); }

private:
    std::deque<std::string> pending_;
    std::deque<int64_t> sent_;
    int64_t firstMs_ = 0, notBeforeMs_ = 0;
    size_t dropped_ = 0;
};

} // namespace veyra::guest
