#include "veyra/guest/GuestAccess.h"
#include <cstdio>

using namespace veyra::guest;

static int failures = 0;
static void expect(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

static Access make(const char* id, const char* name, const std::string& token) {
    Access a;
    a.id = id; a.name = name; a.hash = hashToken(token);
    return a;
}

int main() {
    // ---- tokens
    const std::string t1 = newAccessToken(), t2 = newAccessToken();
    expect(t1.size() == 44 && t1 != t2, "token: 44 base64url characters, different each time");
    bool alphabet = true;
    for (char c : t1) alphabet &= (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    expect(alphabet, "token: base64url alphabet only");
    expect(sameHash(hashToken(t1), hashToken(t1)) && !sameHash(hashToken(t1), hashToken(t2)), "hash: same token same hash, other token other hash");
    const TokenHash abc = hashToken("abc");
    expect(abc[0] == 0xba && abc[1] == 0x78 && abc[31] == 0xad, "hash: SHA-256 of \"abc\" (FIPS 180-2 vector)");

    // ---- hours and expiry
    expect(withinHours(-1, -1, 3 * 60), "hours: none set, any time");
    expect(withinHours(20 * 60, 23 * 60, 21 * 60) && !withinHours(20 * 60, 23 * 60, 23 * 60) && !withinHours(20 * 60, 23 * 60, 10 * 60), "hours: 20:00-23:00");
    expect(withinHours(22 * 60, 2 * 60, 23 * 60 + 30) && withinHours(22 * 60, 2 * 60, 60) && !withinHours(22 * 60, 2 * 60, 12 * 60), "hours: 22:00-02:00 wraps past midnight");

    std::vector<Access> list{make("a1", "lucas", t1), make("a2", "zoé", t2)};
    list[1].expires = 1000;
    list[0].fromMinute = 20 * 60; list[0].toMinute = 23 * 60;
    expect(matchAccess(list, t1, 500, 21 * 60).verdict == AccessVerdict::Ok && matchAccess(list, t1, 500, 21 * 60).index == 0, "match: lucas in his hours");
    expect(matchAccess(list, t1, 500, 9 * 60).verdict == AccessVerdict::OutsideHours, "match: lucas outside his hours");
    expect(matchAccess(list, t2, 999, 0).verdict == AccessVerdict::Ok, "match: zoé before her end date");
    expect(matchAccess(list, t2, 1000, 0).verdict == AccessVerdict::Expired, "match: zoé at her end date");
    expect(matchAccess(list, newAccessToken(), 0, 0).verdict == AccessVerdict::Unknown, "match: an unknown token");
    expect(matchAccess(list, "short", 0, 0).verdict == AccessVerdict::Unknown, "match: a short token is never hashed");

    // ---- modes
    expect(modeFor(AllowGuest, false) == ControlMode::GuestPlays && !modeFor(AllowGuest, true), "modes: guest only, busy console refused");
    expect(modeFor(AllowGuest | AllowTogether, true) == ControlMode::Together, "modes: busy console, à deux");
    expect(modeFor(AllowWatch, true) == ControlMode::WatchOnly && modeFor(AllowWatch, false) == ControlMode::WatchOnly, "modes: watch only");

    // ---- per-address limiter
    SourceLimiter limiter;
    int64_t now = 1'000'000'000;
    for (int i = 0; i < SourceLimiter::kMaxFailures - 1; ++i) limiter.failed("203.0.113.9", now + i);
    expect(!limiter.locked("203.0.113.9", now + 100) && limiter.admit("203.0.113.9", now + 100), "limiter: 9 wrong tokens, not locked yet");
    expect(limiter.failed("203.0.113.9", now + 200), "limiter: the 10th wrong token locks the address");
    expect(!limiter.admit("203.0.113.9", now + 300), "limiter: the locked address is refused");
    expect(limiter.admit("127.0.0.1", now + 300) && limiter.admit("100.64.1.2", now + 300), "limiter: other addresses still pass");
    expect(limiter.admit("203.0.113.9", now + 200 + SourceLimiter::kWindowUs), "limiter: unlocked a minute later");
    SourceLimiter rate;
    bool passed = true;
    for (int i = 0; i < SourceLimiter::kMaxRequests; ++i) passed &= rate.admit("198.51.100.1", now + i);
    expect(passed && !rate.admit("198.51.100.1", now + 1000), "limiter: past the request rate, refused");
    expect(rate.admit("198.51.100.2", now + 1000), "limiter: the rate is per address");
    expect(rate.admit("198.51.100.1", now + SourceLimiter::kWindowUs), "limiter: the rate resets each minute");
    SourceLimiter crowd;
    for (size_t i = 0; i < SourceLimiter::kMaxSources + 50; ++i) crowd.admit("s" + std::to_string(i), now);
    expect(crowd.admit("late", now + SourceLimiter::kWindowUs * 2), "limiter: a full table still takes a new address");

    // ---- the desk: one visit at a time
    AccessDesk desk;
    int started = 0;
    desk.onVisit = [&] { ++started; };
    list[0].fromMinute = list[0].toMinute = -1;
    list[1].expires = 0;
    desk.setAccesses(list);
    desk.setHostName("max");
    auto k = desk.knock(t1, 1, 0, 0);
    expect(k.status == 503 && k.state == "closed" && started == 0, "desk: closed while the gateway is off");
    desk.setOpen(true);
    k = desk.knock(newAccessToken(), 1, 0, 0);
    expect(k.invalid && k.status == 403, "desk: an unknown token is invalid");
    k = desk.knock(t1, 10, 0, 0);
    expect(k.started && started == 1 && k.state == "asking" && k.name == "lucas" && k.host == "max", "desk: lucas knocks, the host is asked");
    k = desk.knock(t1, 20, 0, 0);
    expect(!k.started && started == 1 && k.state == "asking" && desk.visit()->knockUs == 20, "desk: knocking again keeps the visit");
    k = desk.knock(t2, 30, 0, 0);
    expect(k.status == 409 && k.state == "busy", "desk: a second friend sees busy");
    expect(!desk.admitted(t1), "desk: not admitted while asking");
    desk.setPhase(VisitPhase::Ready);
    k = desk.knock(t1, 40, 0, 0);
    expect(k.admitted && k.status == 200 && desk.admitted(t1) == std::optional<std::string>("lucas") && !desk.admitted(t2), "desk: ready, lucas is admitted, zoé is not");
    desk.end(50, "refused");
    k = desk.knock(t1, 60, 0, 0);
    expect(k.status == 403 && k.state == "refused" && started == 1, "desk: a refusal stays readable");
    k = desk.knock(t1, 60 + AccessDesk::kOutcomeUs, 0, 0);
    expect(k.started && started == 2, "desk: after it, a new visit");
    desk.setPhase(VisitPhase::Playing);
    list.erase(list.begin());
    desk.setAccesses(list);
    expect(!desk.visit() && !desk.admitted(t1), "desk: revoking lucas ends his visit at once");
    expect(desk.knock(t1, 100, 0, 0).invalid, "desk: a revoked token is invalid");

    // Busy console: only an access that allows à deux or watching gets in, and the host is asked.
    AccessDesk busy;
    std::vector<Access> two{make("b1", "lucas", t1), make("b2", "zoé", t2)};
    two[0].ask = false;
    two[1].ask = false; two[1].modes = AllowGuest | AllowWatch;
    busy.setAccesses(two); busy.setOpen(true);
    expect(busy.knock(t1, 1, 0, 0).state == "waking", "busy desk: auto-approved access starts by waking the console");
    busy.end(2);
    busy.setConsoleBusy(true);
    expect(busy.knock(t1, 3, 0, 0).state == "busy", "busy desk: guest-only access sees busy");
    k = busy.knock(t2, 4, 0, 0);
    expect(k.state == "asking" && busy.visit()->mode == ControlMode::WatchOnly && busy.visit()->consoleBusy, "busy desk: zoé may watch, after the host says yes");

    if (failures) { std::printf("%d FAILED\n", failures); return 1; }
    std::printf("all passed\n");
    return 0;
}
