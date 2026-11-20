#include "veyra/guest/GuestProtocol.h"
#include "veyra/guest/GuestRoom.h"
#include <cstdio>
#include <vector>

using veyra::guest::ControlMode;
using veyra::guest::GuestPad;
using veyra::remoteplay::ControllerState;

static int failures = 0;
static void expect(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

// The bytes the page's encodeInput() writes (web/guest/index.html).
static std::vector<std::byte> message(uint8_t flags, uint16_t seq, double ms, uint32_t buttons,
                                      std::array<int16_t, 4> axes, uint8_t l2, uint8_t r2) {
    std::vector<std::byte> out(veyra::guest::kInputMessageSize);
    out[0] = std::byte('P');
    out[1] = std::byte(flags);
    std::memcpy(out.data() + 2, &seq, 2);
    std::memcpy(out.data() + 4, &ms, 8);
    std::memcpy(out.data() + 12, &buttons, 4);
    std::memcpy(out.data() + 16, axes.data(), 8);
    out[24] = std::byte(l2);
    out[25] = std::byte(r2);
    return out;
}

int main() {
    const auto bytes = message(2, 513, 1234.5, (1u << 0) | (1u << 16), {100, -200, 32767, -32768}, 10, 0);
    const auto pad = veyra::guest::parseInputMessage(bytes);
    expect(pad.has_value(), "parse: a full message is read");
    expect(pad && pad->seq == 513 && pad->guestMs == 1234.5, "parse: sequence and page time");
    expect(pad && pad->connected && !pad->synthetic, "parse: flags");
    expect(pad && pad->axes[0] == 100 && pad->axes[1] == -200 && pad->axes[2] == 32767, "parse: axes");
    expect(pad && pad->axes[3] == -32767, "parse: -32768 is clamped to the symmetric range");
    expect(pad && pad->l2 == 10 && pad->r2 == 0, "parse: triggers");

    auto shorter = bytes; shorter.pop_back();
    expect(!veyra::guest::parseInputMessage(shorter), "parse: a short message is refused");
    auto longer = bytes; longer.push_back(std::byte(0));
    expect(!veyra::guest::parseInputMessage(longer), "parse: a long message is refused");
    auto wrongKind = bytes; wrongKind[0] = std::byte('Q');
    expect(!veyra::guest::parseInputMessage(wrongKind), "parse: another kind is refused");
    expect(!veyra::guest::parseInputMessage(message(2, 1, std::nan(""), 0, {}, 0, 0)), "parse: a NaN time is refused");
    expect(!veyra::guest::parseInputMessage(message(2, 1, -5, 0, {}, 0, 0)), "parse: a negative time is refused");
    expect(veyra::guest::parseInputMessage(message(3, 1, 1, 0, {}, 0, 0))->synthetic, "parse: the test sweep flag");

    // Standard layout -> DualSense, one button at a time.
    const struct { int index; uint32_t button; const char* name; } map[] = {
        {0, ControllerState::Cross, "a/cross"}, {1, ControllerState::Circle, "b/circle"},
        {2, ControllerState::Square, "x/square"}, {3, ControllerState::Triangle, "y/triangle"},
        {4, ControllerState::L1, "lb/l1"}, {5, ControllerState::R1, "rb/r1"},
        {8, ControllerState::Share, "view/create"}, {9, ControllerState::Options, "menu/options"},
        {10, ControllerState::L3, "ls/l3"}, {11, ControllerState::R3, "rs/r3"},
        {12, ControllerState::Up, "dpad up"}, {13, ControllerState::Down, "dpad down"},
        {14, ControllerState::Left, "dpad left"}, {15, ControllerState::Right, "dpad right"},
        {16, ControllerState::PS, "home/ps"}, {17, ControllerState::Touchpad, "touchpad click"}};
    for (const auto& m : map) {
        GuestPad p; p.connected = true; p.buttons = 1u << m.index;
        const auto s = veyra::guest::toControllerState(p);
        char what[96]; std::snprintf(what, sizeof(what), "map: %s", m.name);
        expect(s.buttons == m.button && s.inputActive, what);
    }
    {
        GuestPad p; p.connected = true; p.buttons = (1u << 6) | (1u << 7); p.r2 = 40;
        const auto s = veyra::guest::toControllerState(p);
        expect(s.buttons == 0, "map: l2/r2 are not buttons");
        expect(s.l2 == 255, "map: a trigger reported only as pressed pulls fully");
        expect(s.r2 == 40, "map: an analog trigger keeps its value");
    }
    {
        GuestPad p; p.connected = true; p.axes = {1000, -2000, 3000, -4000};
        const auto s = veyra::guest::toControllerState(p);
        expect(s.leftX == 1000 && s.leftY == -2000 && s.rightX == 3000 && s.rightY == -4000, "map: sticks pass through");
    }
    {
        GuestPad p; p.connected = false; p.buttons = 1; p.axes = {5000, 0, 0, 0};
        const auto s = veyra::guest::toControllerState(p);
        expect(!s.inputActive && s.buttons == 0 && s.leftX == 0, "map: no pad on the page is a released pad");
    }

    ControllerState host; host.inputActive = true; host.buttons = ControllerState::Square; host.leftX = 100;
    ControllerState guest; guest.inputActive = true; guest.buttons = ControllerState::Cross; guest.leftX = 20000;
    using veyra::guest::combine;
    expect(combine(ControlMode::GuestPlays, host, std::nullopt) == host, "combine: no guest, the host plays");
    expect(combine(ControlMode::Together, host, std::nullopt) == host, "combine: no guest in à deux, the host plays");
    expect(combine(ControlMode::GuestPlays, host, guest) == guest, "combine: l'invité joue, the guest's pad only");
    expect(combine(ControlMode::WatchOnly, host, guest) == host, "combine: regarder seulement ignores the guest");
    const auto both = combine(ControlMode::Together, host, guest);
    expect(both.buttons == (ControllerState::Square | ControllerState::Cross), "combine: à deux merges the buttons");
    expect(both.leftX == 20000, "combine: à deux keeps the stick pushed furthest");
    ControllerState stale;  // released: inputActive false
    expect(combine(ControlMode::GuestPlays, host, stale) == stale, "combine: a silent guest in l'invité joue sends a released pad");

    // ---- extended message: a BooVI client's DualSense ----
    using veyra::guest::encodeInputMessage;
    using veyra::guest::parseInputMessage;
    ControllerState ds;
    ds.inputActive = true;
    ds.buttons = ControllerState::Cross | ControllerState::Touchpad | ControllerState::PS | ControllerState::Left;
    ds.leftX = -1200; ds.leftY = 300; ds.rightX = 32767; ds.rightY = -32767; ds.l2 = 7; ds.r2 = 255;
    ds.touches[0] = {3, 1919, 0}; ds.touches[1] = {4, 100, 1079};
    ds.motionValid = true; ds.gyro = {0.5f, -1.25f, 2.0f}; ds.accel = {0.01f, 0.98f, -0.1f}; ds.motionTimestampUs = 123456789;
    for (int i = 0; i < 3; ++i) ds.motionSamples[i] = {{float(i), 0.1f, 0.2f}, {0, 1, 0}, uint64_t(1000 + i)};
    ds.motionSampleCount = 3;
    const auto extBytes = encodeInputMessage(veyra::guest::fromControllerState(ds, 77, 42.25));
    expect(extBytes.size() == 76 + 3 * 32, "extended: 76 bytes plus 32 per motion sample");
    const auto ext = parseInputMessage(extBytes);
    expect(ext && ext->version == 1 && ext->seq == 77 && ext->guestMs == 42.25, "extended: version, sequence, time");
    const auto back = ext ? veyra::guest::toControllerState(*ext) : ControllerState{};
    expect(back.buttons == ds.buttons, "extended: every DualSense button survives the round trip");
    expect(back.leftX == ds.leftX && back.rightY == ds.rightY && back.l2 == 7 && back.r2 == 255, "extended: sticks and triggers");
    expect(back.touches == ds.touches, "extended: both touch points reach the PS5 state");
    expect(back.motionValid && back.gyro == ds.gyro && back.accel == ds.accel && back.motionTimestampUs == ds.motionTimestampUs, "extended: gyro, accel and timestamp reach the PS5 state");
    expect(back.motionSampleCount == 3 && back.motionSamples[2] == ds.motionSamples[2], "extended: the batched motion samples arrive in order");

    auto noMotion = ds; noMotion.motionValid = false;
    const auto still = parseInputMessage(encodeInputMessage(veyra::guest::fromControllerState(noMotion, 1, 1)));
    expect(still && !still->motionValid && !veyra::guest::toControllerState(*still).motionValid, "extended: a pad without sensors sends no motion");
    // The host times a guest's input from its first message that changes the PS5's state:
    // the client's 8 ms keepalive repeats the pad and must read as no change.
    const auto keepalive = parseInputMessage(encodeInputMessage(veyra::guest::fromControllerState(noMotion, 2, 9)));
    expect(still && keepalive && veyra::guest::toControllerState(*keepalive) == veyra::guest::toControllerState(*still), "extended: a keepalive hands the PS5 the same state");
    auto nudged = noMotion; nudged.rightX -= 1;
    const auto moved = parseInputMessage(encodeInputMessage(veyra::guest::fromControllerState(nudged, 3, 17)));
    expect(still && moved && veyra::guest::toControllerState(*moved) != veyra::guest::toControllerState(*still), "extended: a stick step is a change");

    // Classic messages are untouched by the extension.
    expect(parseInputMessage(bytes) && parseInputMessage(bytes)->version == 0 && !parseInputMessage(bytes)->motionValid, "extended: the browser's 28 bytes still parse as version 0");
    const auto page = message(3, 9, 77.5, (1u << 3) | (1u << 17), {1, -2, 3, -32767}, 0, 200);
    expect(encodeInputMessage(*parseInputMessage(page)) == page, "extended: version 0 encodes the browser's exact bytes");

    auto truncated = extBytes; truncated.pop_back();
    expect(!parseInputMessage(truncated), "extended: a short extended message is refused");
    auto padded = extBytes; padded.push_back(std::byte(0));
    expect(!parseInputMessage(padded), "extended: version 1 with trailing bytes is refused");
    auto future = extBytes; future[26] = std::byte(2); future.push_back(std::byte(9));
    expect(parseInputMessage(future) && parseInputMessage(future)->gyro == ds.gyro, "extended: a later version's appended fields are skipped");
    auto noVersion = extBytes; noVersion[26] = std::byte(0);
    expect(!parseInputMessage(noVersion), "extended: more than 28 bytes without a version is refused");
    auto tooMany = extBytes; tooMany[27] = std::byte(9);
    expect(!parseInputMessage(tooMany), "extended: more than 8 motion samples is refused");
    auto nanGyro = extBytes; const float nan = std::nanf(""); std::memcpy(nanGyro.data() + 40, &nan, 4);
    const auto nanPad = parseInputMessage(nanGyro);
    expect(nanPad && !nanPad->motionValid && nanPad->buttons == ext->buttons, "extended: a NaN gyro drops the motion, keeps the buttons");
    auto wide = extBytes; const uint16_t outside = 60000; std::memcpy(wide.data() + 30, &outside, 2);
    expect(parseInputMessage(wide)->touches[0].x == 1919, "extended: a touch outside the pad is clamped");

    // ---- feedback, host -> client ----
    veyra::remoteplay::ControllerFeedback fb;
    fb.rumble = true; fb.left = 200; fb.right = 15; fb.triggers = true;
    fb.leftTrigger = {0x21, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10}; fb.rightTrigger[0] = 0x26;
    fb.haptics = {100, -100, 32767, -32768};
    const auto fbBytes = veyra::guest::encodeFeedbackMessage(fb);
    const auto fbBack = veyra::guest::parseFeedbackMessage(fbBytes);
    expect(fbBack && fbBack->rumble && fbBack->left == 200 && fbBack->right == 15, "feedback: rumble");
    expect(fbBack && fbBack->triggers && fbBack->leftTrigger == fb.leftTrigger && fbBack->rightTrigger == fb.rightTrigger, "feedback: adaptive trigger effects");
    expect(fbBack && fbBack->haptics == fb.haptics, "feedback: haptic PCM");
    auto fbShort = fbBytes; fbShort.pop_back();
    expect(!veyra::guest::parseFeedbackMessage(fbShort), "feedback: a short message is refused");

    // ---- the link a friend pastes ----
    using veyra::guest::parseShareLink;
    const auto cf = parseShareLink("  https://play.example.com/?t=AbC-d_EfGh1234567890xyz \n");
    expect(cf && cf->https && cf->host == "play.example.com" && cf->port == 443 && cf->token == "AbC-d_EfGh1234567890xyz", "link: the cloudflare share link");
    const auto local = parseShareLink("http://127.0.0.1:47801/?x=1&t=tokentoken#fake=remap");
    expect(local && !local->https && local->host == "127.0.0.1" && local->port == 47801 && local->token == "tokentoken", "link: a local link with a port, other parameters and a fragment");
    const auto bare = parseShareLink("Play.Example.Com?t=tokentoken");
    expect(bare && bare->https && bare->host == "play.example.com", "link: no scheme means https");
    expect(!parseShareLink("https://play.example.com/"), "link: no token is refused");
    expect(!parseShareLink("https://user@play.example.com/?t=tokentoken"), "link: user info is refused");
    expect(!parseShareLink("ftp://play.example.com/?t=tokentoken"), "link: another scheme is refused");
    expect(!parseShareLink("https://play.example.com/?t=bad<token>!"), "link: a token outside base64url is refused");
    expect(!parseShareLink("https://play.example.com:99999/?t=tokentoken"), "link: a port past 65535 is refused");
    const auto access = parseShareLink("https://play.example.com/?a=AbC-d_EfGh1234567890xyzAbC-d_EfGh1234567890xy");
    expect(access && access->access && access->token.size() == 45 && access->host == "play.example.com", "link: a friend's access link");
    expect(cf && !cf->access && cf->room.empty(), "link: a share link is not an access, and an old one names no room");
    const auto room = parseShareLink("https://go.example.com/BD0Yl_nIfZ9Outt9sJj5xG#t=AbC-d_EfGh1234567890xyz");
    expect(room && room->room == "BD0Yl_nIfZ9Outt9sJj5xG" && room->token == "AbC-d_EfGh1234567890xyz" && !room->access, "link: a broker room, its token in the fragment");
    const auto roomAccess = parseShareLink("go.example.com/BD0Yl_nIfZ9Outt9sJj5xG/#a=AbC-d_EfGh1234567890xyzAbC");
    expect(roomAccess && roomAccess->access && roomAccess->room == "BD0Yl_nIfZ9Outt9sJj5xG", "link: a friend's access to a broker room");
    expect(!parseShareLink("https://go.example.com/BD0Yl_nIfZ9Outt9sJj5xG"), "link: a room with no secret is refused");
    expect(!parseShareLink("https://go.example.com/not-a-room#t=AbC-d_EfGh1234567890xyz"), "link: a path that is no room id is refused");
    const auto tail = parseShareLink("http://100.64.1.2:47800/#t=tokentoken");
    expect(tail && tail->room.empty() && tail->token == "tokentoken" && !tail->https, "link: a tailnet link with its token in the fragment");

    // ---- the broker room's id, derived as broker/src/ids.ts does (same vector in broker/test/ids.test.ts) ----
    expect(veyra::guest::roomIdOf("BooVI-room-key-test-vector-0123456789abcdefg") == "BD0Yl_nIfZ9Outt9sJj5xG", "room: id from the key");
    expect(veyra::guest::isRoomKey(veyra::guest::newRoomKey()), "room: a new key is 44 base64url characters");
    expect(veyra::guest::newRoomKey() != veyra::guest::newRoomKey(), "room: keys are random");

    if (failures) { std::printf("%d FAILED\n", failures); return 1; }
    std::printf("all passed\n");
    return 0;
}
