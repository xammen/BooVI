#pragma once
// Fork, partage, "invités": a named, revocable, long-lived access for a friend ("lucas"), so
// their link wakes the host's PS5 and lets them in while the host is away. The host's PC is the
// gateway: the pairing keys and the login PIN never leave it; the friend only holds a token.
//
// - Access: the friend's name, the SHA-256 of their token (the token itself is shown once, at
//   creation or regeneration, and never stored), an optional end date and daily hours, whether
//   the host is asked each time, the control modes allowed, the most they receive.
// - AccessDesk: the visit, one console session that several friends share. The first friend's
//   page knocks (POST /session with its access token) until the host lets it in; GuestStreamer
//   asks the desk, the host's UI drives the visit (ask, wake the console, connect, play). Later
//   friends join the running visit: at once when their access does not ask, else once the host
//   says yes. The visit ends when the last one has left, or when the host ends it.
// - SourceLimiter: wrong credentials and request rate counted per source address, so one
//   address cannot lock everyone else out.
// Pure but for BCrypt (hash, random): tested in tests/fork/GuestAccessTests.cpp.
#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "veyra/guest/GuestProtocol.h"
#include "veyra/guest/GuestQuality.h"

#pragma comment(lib, "bcrypt.lib")

namespace veyra::guest {

using TokenHash = std::array<uint8_t, 32>;

inline TokenHash hashToken(std::string_view token) {
    TokenHash out{};
    BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char*>(token.data())), ULONG(token.size()), out.data(), ULONG(out.size()));
    return out;
}

inline bool sameHash(const TokenHash& a, const TokenHash& b) {
    unsigned diff = 0;
    for (size_t i = 0; i < a.size(); ++i) diff |= unsigned(a[i] ^ b[i]);
    return diff == 0;
}

// `bytes` random bytes (a multiple of 3) as base64url without padding.
inline std::string randomBase64Url(size_t bytes) {
    std::vector<uint8_t> raw(bytes);
    BCryptGenRandom(nullptr, raw.data(), ULONG(raw.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    for (size_t i = 0; i + 2 < raw.size(); i += 3) {
        const uint32_t v = (uint32_t(raw[i]) << 16) | (uint32_t(raw[i + 1]) << 8) | raw[i + 2];
        for (int k = 3; k >= 0; --k) out.push_back(alphabet[(v >> (6 * k)) & 63]);
    }
    SecureZeroMemory(raw.data(), raw.size());
    return out;
}

// 264 random bits, 44 characters.
inline std::string newAccessToken() { return randomBase64Url(33); }

enum AllowedMode : uint8_t { AllowGuest = 1, AllowTogether = 2, AllowWatch = 4 };

struct Access {
    std::string id;                     // 12 hex characters, names the access in the UI and the history
    std::string name;                   // "lucas"
    TokenHash hash{};
    int64_t created = 0;                // unix seconds
    int64_t expires = 0;                // unix seconds; 0: no end
    int fromMinute = -1, toMinute = -1; // local daily window [from, to), wrapping past midnight; -1: any time
    bool ask = true;                    // "demander mon accord" each time, else "autoriser automatiquement"
    uint8_t modes = AllowGuest;
    Feed feed = Feed::Smooth120;        // the most this friend receives, within the host's own maximum
};

inline bool withinHours(int fromMinute, int toMinute, int minuteOfDay) {
    if (fromMinute < 0 || toMinute < 0 || fromMinute == toMinute) return true;
    if (fromMinute < toMinute) return minuteOfDay >= fromMinute && minuteOfDay < toMinute;
    return minuteOfDay >= fromMinute || minuteOfDay < toMinute;
}

enum class AccessVerdict : uint8_t { Ok, Unknown, Expired, OutsideHours };
struct AccessMatch {
    AccessVerdict verdict = AccessVerdict::Unknown;
    int index = -1;
};

// Every access is compared, in constant time, whatever matches first.
inline AccessMatch matchAccess(const std::vector<Access>& list, std::string_view token, int64_t nowUnix, int minuteOfDay) {
    if (token.size() < 16 || token.size() > 128) return {};
    const TokenHash h = hashToken(token);
    int found = -1;
    for (size_t i = 0; i < list.size(); ++i)
        if (sameHash(list[i].hash, h) && found < 0) found = int(i);
    if (found < 0) return {};
    const Access& a = list[size_t(found)];
    if (a.expires > 0 && nowUnix >= a.expires) return {AccessVerdict::Expired, found};
    if (!withinHours(a.fromMinute, a.toMinute, minuteOfDay)) return {AccessVerdict::OutsideHours, found};
    return {AccessVerdict::Ok, found};
}

// The mode a friend gets: alone at a free console ("l'invité joue" first), next to the host at a
// busy one ("à deux", then "regarder seulement"); nothing when the access allows neither.
inline std::optional<ControlMode> modeFor(uint8_t allowed, bool consoleBusy) {
    if (!consoleBusy) {
        if (allowed & AllowGuest) return ControlMode::GuestPlays;
        if (allowed & AllowTogether) return ControlMode::Together;
        if (allowed & AllowWatch) return ControlMode::WatchOnly;
        return std::nullopt;
    }
    if (allowed & AllowTogether) return ControlMode::Together;
    if (allowed & AllowWatch) return ControlMode::WatchOnly;
    return std::nullopt;
}

// Per source address: a request rate, and wrong credentials. kMaxFailures wrong ones within
// kWindowUs lock that address for kWindowUs; other addresses are not affected.
class SourceLimiter {
public:
    static constexpr int kMaxFailures = 10;
    static constexpr int kMaxRequests = 240;
    static constexpr int64_t kWindowUs = 60'000'000;
    static constexpr size_t kMaxSources = 4096;

    // Counts the request; false when the address is locked out or past its rate.
    bool admit(const std::string& source, int64_t nowUs) {
        std::lock_guard lock(m_);
        Entry& e = entry(source, nowUs);
        if (e.lockedUntil > nowUs) return false;
        if (nowUs - e.windowStart >= kWindowUs) { e.windowStart = nowUs; e.requests = 0; }
        return ++e.requests <= kMaxRequests;
    }
    // A wrong token or access from this address. True when it just locked the address.
    bool failed(const std::string& source, int64_t nowUs) {
        std::lock_guard lock(m_);
        Entry& e = entry(source, nowUs);
        while (!e.failures.empty() && nowUs - e.failures.front() >= kWindowUs) e.failures.pop_front();
        e.failures.push_back(nowUs);
        if (int(e.failures.size()) < kMaxFailures || e.lockedUntil > nowUs) return false;
        e.lockedUntil = nowUs + kWindowUs;
        e.failures.clear();
        return true;
    }
    bool locked(const std::string& source, int64_t nowUs) const {
        std::lock_guard lock(m_);
        const auto it = map_.find(source);
        return it != map_.end() && it->second.lockedUntil > nowUs;
    }

private:
    struct Entry {
        int64_t windowStart = 0, lockedUntil = 0, lastUs = 0;
        int requests = 0;
        std::deque<int64_t> failures;
    };
    Entry& entry(const std::string& source, int64_t nowUs) {
        if (map_.size() >= kMaxSources && !map_.contains(source)) {
            std::erase_if(map_, [&](const auto& kv) { return kv.second.lockedUntil <= nowUs && nowUs - kv.second.lastUs >= kWindowUs; });
            if (map_.size() >= kMaxSources) map_.erase(map_.begin());
        }
        Entry& e = map_[source];
        e.lastUs = nowUs;
        return e;
    }
    mutable std::mutex m_;
    std::unordered_map<std::string, Entry> map_;
};

// What made a viewer's connection at /session: the hash of the credential shown (share token or
// access token) and, for a share-link viewer, its address. Its /answer must show the same.
struct PeerCredential {
    TokenHash hash{};
    bool shareLink = false;
    std::string source;
    bool admits(const PeerCredential& shown) const {
        // Not the address: a laptop changing network between /session and /answer is the same guest.
        return sameHash(hash, shown.hash) && shareLink == shown.shareLink;
    }
};

// A visit, from the friend's first knock to their leaving.
enum class VisitPhase : uint8_t { Asking, Waking, Connecting, Ready, Playing };

constexpr std::string_view visitPhaseName(VisitPhase p) {
    switch (p) {
    case VisitPhase::Asking: return "asking";
    case VisitPhase::Waking: return "waking";
    case VisitPhase::Connecting: return "connecting";
    case VisitPhase::Ready: return "ready";
    default: return "playing";
    }
}

struct Visit {
    std::string accessId, name;   // the friend who started it
    TokenHash hash{};
    VisitPhase phase = VisitPhase::Asking;
    bool consoleBusy = false;   // the host was playing: the friend joins their stream, no console start
    ControlMode mode = ControlMode::GuestPlays;
    Feed feed = Feed::Smooth120;
    int64_t startedUs = 0, knockUs = 0;
};

// One friend in the visit. The starter's approval is the visit's own (its phase past Asking).
struct VisitGuest {
    std::string accessId, name;
    TokenHash hash{};
    bool approved = false;
    bool watchOnly = false;     // their access allows watching only
    int64_t knockUs = 0, sinceUs = 0;
};

// What a knock gets. Admitted: create the peer. Otherwise the HTTP status and the state the page
// shows: "asking", "waking", "connecting" (keep knocking), "busy", "refused", "failed",
// "expired", "hours", "closed" (stop), "invalid" (a wrong token: counted against the address).
struct Knock {
    bool admitted = false;
    bool invalid = false;
    bool started = false;       // this knock began a visit or joined one: the host's side acts on it
    bool watchOnly = false;     // admitted: their access allows watching only
    int status = 202;
    std::string state, accessId, name, host, detail;
};

// A friend the visit lets in: their access, by its id (names are only shown).
struct Admission {
    std::string accessId, name;
    bool watchOnly = false;
};

class AccessDesk {
public:
    static constexpr int64_t kOutcomeUs = 30'000'000;   // a refusal or failure stays readable this long

    // Called without the desk's lock after a knock that began a visit or added a friend to it (HTTP thread).
    std::function<void()> onVisit;

    // Revoked or regenerated: that friend leaves at once; the starter takes the visit with them.
    // Returns the friends who left, for the host's side to cut their connections.
    std::vector<VisitGuest> setAccesses(std::vector<Access> list) {
        std::lock_guard lock(m_);
        accesses_ = std::move(list);
        std::vector<VisitGuest> gone;
        if (visit_ && !known(visit_->accessId, visit_->hash)) {
            visit_.reset();
            return std::exchange(guests_, {});
        }
        std::erase_if(guests_, [&](const VisitGuest& g) {
            if (known(g.accessId, g.hash)) return false;
            gone.push_back(g);
            return true;
        });
        return gone;
    }
    void setHostName(std::string name) { std::lock_guard lock(m_); host_ = std::move(name); }
    std::string hostName() const { std::lock_guard lock(m_); return host_; }
    void setOpen(bool open) { std::lock_guard lock(m_); open_ = open; }
    bool open() const { std::lock_guard lock(m_); return open_; }
    void setConsoleBusy(bool busy) { std::lock_guard lock(m_); consoleBusy_ = busy; }
    // How many friends one visit holds (the room's size).
    void setCapacity(int n) { std::lock_guard lock(m_); capacity_ = std::max(1, n); }

    Knock knock(std::string_view token, int64_t nowUs, int64_t nowUnix, int minuteOfDay) {
        Knock out;
        {
            std::lock_guard lock(m_);
            out.host = host_;
            const auto match = matchAccess(accesses_, token, nowUnix, minuteOfDay);
            if (match.verdict == AccessVerdict::Unknown) { out.invalid = true; out.status = 403; out.state = "invalid"; return out; }
            const Access& a = accesses_[size_t(match.index)];
            out.accessId = a.id;
            out.name = a.name;
            if (match.verdict == AccessVerdict::Expired) { out.status = 403; out.state = "expired"; return out; }
            if (match.verdict == AccessVerdict::OutsideHours) { out.status = 403; out.state = "hours"; return out; }
            if (!open_) { out.status = 503; out.state = "closed"; return out; }
            if (VisitGuest* g = guest(a.hash)) {
                g->knockUs = nowUs;
                if (sameHash(visit_->hash, a.hash)) visit_->knockUs = nowUs;
                if (!g->approved || visit_->phase == VisitPhase::Asking) { out.state = "asking"; return out; }
                // Let in while the console still wakes: the WebRTC set-up runs meanwhile, the
                // picture comes once the console streams.
                out.admitted = true; out.status = 200; out.state = visitPhaseName(visit_->phase); out.watchOnly = g->watchOnly;
                return out;
            }
            if (outcome(a.hash, nowUs, out)) return out;
            if (visit_) {
                // Another friend joins the running visit, as a player when their access lets them play.
                if (int(guests_.size()) >= capacity_) { out.status = 409; out.state = "busy"; return out; }
                const VisitGuest g{a.id, a.name, a.hash, !a.ask, (a.modes & (AllowGuest | AllowTogether)) == 0, nowUs, nowUs};
                guests_.push_back(g);
                out.state = g.approved ? visitPhaseName(visit_->phase) : "asking";
                out.watchOnly = g.watchOnly;
                if (g.approved && visit_->phase != VisitPhase::Asking) { out.admitted = true; out.status = 200; }
            } else {
                const auto mode = modeFor(a.modes ? a.modes : uint8_t(AllowGuest), consoleBusy_);
                if (!mode) { out.status = 409; out.state = "busy"; return out; }
                Visit v;
                v.accessId = a.id; v.name = a.name; v.hash = a.hash; v.consoleBusy = consoleBusy_; v.mode = *mode; v.feed = a.feed;
                // Joining the host's own game always asks them first.
                v.phase = a.ask || consoleBusy_ ? VisitPhase::Asking : VisitPhase::Waking;
                v.startedUs = v.knockUs = nowUs;
                visit_ = v;
                guests_.assign(1, VisitGuest{a.id, a.name, a.hash, v.phase != VisitPhase::Asking, *mode == ControlMode::WatchOnly, nowUs, nowUs});
                out.state = visitPhaseName(v.phase);
                out.watchOnly = guests_.front().watchOnly;
                if (v.phase != VisitPhase::Asking) { out.admitted = true; out.status = 200; }
            }
            out.started = true;
        }
        if (onVisit) onVisit();
        return out;
    }

    // /answer: the friend while their visit lets them in (from the host's yes on, the console
    // waking included).
    std::optional<Admission> admitted(std::string_view token) const {
        if (token.size() < 16 || token.size() > 128) return std::nullopt;
        const TokenHash h = hashToken(token);
        std::lock_guard lock(m_);
        if (!visit_ || visit_->phase == VisitPhase::Asking) return std::nullopt;
        for (const auto& g : guests_) if (g.approved && sameHash(g.hash, h)) return Admission{g.accessId, g.name, g.watchOnly};
        return std::nullopt;
    }

    std::optional<Visit> visit() const { std::lock_guard lock(m_); return visit_; }
    std::vector<VisitGuest> guests() const { std::lock_guard lock(m_); return guests_; }
    // The starter's approval moves the visit on; a later friend's (approveGuest) lets just them in.
    void setPhase(VisitPhase phase) {
        std::lock_guard lock(m_);
        if (!visit_) return;
        visit_->phase = phase;
        if (phase == VisitPhase::Asking) return;
        for (auto& g : guests_) if (g.accessId == visit_->accessId) g.approved = true;
    }
    void approveGuest(const std::string& accessId) {
        std::lock_guard lock(m_);
        for (auto& g : guests_) if (g.accessId == accessId) g.approved = true;
    }
    // One friend leaves the visit (gone, refused, cut); a refusal stays readable on their page.
    void leave(const std::string& accessId, int64_t nowUs, std::string state = {}, std::string detail = {}) {
        std::lock_guard lock(m_);
        for (auto it = guests_.begin(); it != guests_.end(); ++it) {
            if (it->accessId != accessId) continue;
            if (!state.empty()) remember(Outcome{it->hash, std::move(state), std::move(detail), nowUs + kOutcomeUs}, nowUs);
            guests_.erase(it);
            return;
        }
    }
    // Ends the visit and lets every friend in it go. A refusal or failure stays readable by the
    // starter's page for kOutcomeUs.
    std::optional<Visit> end(int64_t nowUs, std::string state = {}, std::string detail = {}) {
        std::lock_guard lock(m_);
        auto ended = std::move(visit_);
        visit_.reset();
        guests_.clear();
        if (ended && !state.empty()) remember(Outcome{ended->hash, std::move(state), std::move(detail), nowUs + kOutcomeUs}, nowUs);
        return ended;
    }

private:
    struct Outcome { TokenHash hash; std::string state, detail; int64_t untilUs; };
    bool known(const std::string& id, const TokenHash& h) const {
        for (const auto& a : accesses_) if (a.id == id && sameHash(a.hash, h)) return true;
        return false;
    }
    VisitGuest* guest(const TokenHash& h) {
        for (auto& g : guests_) if (sameHash(g.hash, h)) return &g;
        return nullptr;
    }
    // Each friend's own last refusal or cut, so two in a row do not reopen the first one's door.
    void remember(Outcome o, int64_t nowUs) {
        std::erase_if(outcomes_, [&](const Outcome& x) { return nowUs >= x.untilUs || sameHash(x.hash, o.hash); });
        if (outcomes_.size() >= 32) outcomes_.erase(outcomes_.begin());
        outcomes_.push_back(std::move(o));
    }
    bool outcome(const TokenHash& h, int64_t nowUs, Knock& out) const {
        for (const auto& o : outcomes_) {
            if (!sameHash(o.hash, h) || nowUs >= o.untilUs) continue;
            out.status = 403; out.state = o.state; out.detail = o.detail;
            return true;
        }
        return false;
    }
    mutable std::mutex m_;
    std::vector<Access> accesses_;
    std::optional<Visit> visit_;
    std::vector<VisitGuest> guests_;
    std::vector<Outcome> outcomes_;
    std::string host_;
    bool open_ = false, consoleBusy_ = false;
    int capacity_ = 4;
};

} // namespace veyra::guest
