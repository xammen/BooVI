#pragma once
// Fork, partage: what a remote guest's browser sends on the "input" data channel
// (web/guest/index.html) and how it becomes the DualSense state the PS5 receives.
// Pure functions, read by the PS5 input thread (RemotePlayInput.cpp) and tested in
// tests/fork/GuestProtocolTests.cpp.
//
// Input message, 28 bytes, little-endian, one full pad state per message:
//   0      'P'
//   1      flags: bit 0 synthetic (the page's test sweep), bit 1 a pad is connected
//   2..3   u16 sequence number
//   4..11  f64 the page's performance.now() when it read the pad, in ms
//   12..15 u32 buttons, bit i = button i of the browser's standard gamepad layout
//   16..23 i16 left x, left y, right x, right y (-32767..32767, +y down as in SDL)
//   24, 25 u8 l2, r2 (analog, 0..255)
//   26..27 reserved (0)
//
// Extended message, version 1, sent by a BooVI client (GuestClientSource): the same 28 bytes,
// then the DualSense's touchpad and motion. 76 + 32 n bytes.
//   1      flags: bit 2 motion valid
//   26     u8 version (1); 0 is the browser's message, which is exactly 28 bytes
//   27     u8 n, motion samples read since the previous message (0..8)
//   28..39 two touches: i8 id (-1 none), u8 0, u16 x (0..1919), u16 y (0..1079)
//   40..51 f32 gyro x, y, z (rad/s, SDL frame); 52..63 f32 accel x, y, z (g)
//   64..71 u64 motion timestamp, µs
//   72..75 reserved
//   76..   n samples of 32 bytes: f32 gyro[3], f32 accel[3], u64 timestamp µs
// A later version may append fields: a reader takes the version 1 part of a longer message.
//
// Feedback, host to a BooVI client on the "feedback" channel (the browser gets JSON rumble):
//   0 'F', 1 version (1), 2 flags (bit 0 rumble, bit 1 triggers, bit 2 motion reset),
//   3, 4 u8 rumble left, right, 5..15 left trigger effect, 16..26 right trigger effect,
//   27 reserved, 28..29 u16 n haptic samples, 30.. n i16 (stereo PCM 3 kHz, n <= 600).
#include <algorithm>
#include <cctype>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "veyra/remoteplay/KbmMapper.h"
#include "veyra/remoteplay/Types.h"

namespace veyra::guest {

// Who drives the pad while a guest is connected; the host chooses.
enum class ControlMode : uint8_t {
    GuestPlays,  // "l'invité joue": the guest's pad replaces the host's
    Together,    // "à deux": guest, host pad and keyboard/mouse merged (mergeInputs)
    WatchOnly,   // "regarder seulement": the guest's input is ignored
};

inline constexpr size_t kMaxMotionSamples = 8;

struct GuestPad {
    uint16_t seq = 0;
    double guestMs = 0;
    bool synthetic = false;
    bool connected = false;
    uint32_t buttons = 0;              // standard gamepad layout bits
    std::array<int16_t, 4> axes{};     // lx, ly, rx, ry
    uint8_t l2 = 0, r2 = 0;
    // Extended message only (version >= 1).
    uint8_t version = 0;
    std::array<remoteplay::ControllerState::Touch, 2> touches{};
    bool motionValid = false;
    std::array<float, 3> gyro{}, accel{0, 1, 0};
    uint64_t motionTimestampUs = 0;
    std::array<remoteplay::ControllerState::MotionSample, kMaxMotionSamples> motionSamples{};
    uint8_t motionSampleCount = 0;
};

inline constexpr size_t kInputMessageSize = 28;
inline constexpr uint8_t kInputKind = 'P';
inline constexpr uint8_t kExtendedVersion = 1;
inline constexpr size_t kExtendedHeaderSize = 76;
inline constexpr size_t kMotionSampleSize = 32;
inline constexpr uint16_t kTouchMaxX = 1919, kTouchMaxY = 1079;
// A connected guest that has sent nothing for this long is treated as a released pad
// (the page sends a keepalive every 50 ms).
inline constexpr remoteplay::HostTime kGuestStale = 2500000;  // 250 ms in 100 ns

namespace detail {
inline bool finite3(const std::array<float, 3>& v) {
    return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}
} // namespace detail

inline std::optional<GuestPad> parseInputMessage(std::span<const std::byte> bytes) {
    if (bytes.size() < kInputMessageSize || uint8_t(bytes[0]) != kInputKind) return std::nullopt;
    auto read = [&](size_t at, auto& out) { std::memcpy(&out, bytes.data() + at, sizeof(out)); };
    GuestPad pad;
    const uint8_t flags = uint8_t(bytes[1]);
    pad.synthetic = flags & 1;
    pad.connected = flags & 2;
    read(2, pad.seq);
    read(4, pad.guestMs);
    if (!std::isfinite(pad.guestMs) || pad.guestMs < 0) return std::nullopt;
    read(12, pad.buttons);
    for (size_t i = 0; i < 4; ++i) {
        read(16 + 2 * i, pad.axes[i]);
        pad.axes[i] = std::max<int16_t>(pad.axes[i], -32767);
    }
    pad.l2 = uint8_t(bytes[24]);
    pad.r2 = uint8_t(bytes[25]);
    if (bytes.size() == kInputMessageSize) return pad;

    // Extended: a version, then a size that matches it.
    pad.version = uint8_t(bytes[26]);
    const uint8_t count = uint8_t(bytes[27]);
    const size_t needed = kExtendedHeaderSize + kMotionSampleSize * count;
    if (pad.version < kExtendedVersion || count > kMaxMotionSamples || bytes.size() < needed) return std::nullopt;
    if (pad.version == kExtendedVersion && bytes.size() != needed) return std::nullopt;
    for (size_t i = 0; i < 2; ++i) {
        const size_t at = 28 + 6 * i;
        auto& t = pad.touches[i];
        t.id = int8_t(bytes[at]);
        read(at + 2, t.x);
        read(at + 4, t.y);
        if (t.id < 0) t = {};
        t.x = std::min(t.x, kTouchMaxX);
        t.y = std::min(t.y, kTouchMaxY);
    }
    read(40, pad.gyro);
    read(52, pad.accel);
    read(64, pad.motionTimestampUs);
    // Unusable motion drops the motion, never the buttons.
    pad.motionValid = (flags & 4) && detail::finite3(pad.gyro) && detail::finite3(pad.accel);
    for (size_t i = 0; i < count; ++i) {
        const size_t at = kExtendedHeaderSize + kMotionSampleSize * i;
        remoteplay::ControllerState::MotionSample s;
        read(at, s.gyro);
        read(at + 12, s.accel);
        read(at + 24, s.timestampUs);
        if (detail::finite3(s.gyro) && detail::finite3(s.accel)) pad.motionSamples[pad.motionSampleCount++] = s;
    }
    if (!pad.motionValid) { pad.gyro = {}; pad.accel = {0, 1, 0}; pad.motionTimestampUs = 0; pad.motionSampleCount = 0; }
    return pad;
}

// The bytes parseInputMessage reads: the browser's 28 for version 0, else the extended form.
inline std::vector<std::byte> encodeInputMessage(const GuestPad& pad) {
    const uint8_t count = pad.version ? uint8_t(std::min<size_t>(pad.motionSampleCount, kMaxMotionSamples)) : 0;
    std::vector<std::byte> out(pad.version ? kExtendedHeaderSize + kMotionSampleSize * count : kInputMessageSize);
    auto write = [&](size_t at, const auto& value) { std::memcpy(out.data() + at, &value, sizeof(value)); };
    out[0] = std::byte(kInputKind);
    out[1] = std::byte(uint8_t((pad.synthetic ? 1 : 0) | (pad.connected ? 2 : 0) | (pad.version && pad.motionValid ? 4 : 0)));
    write(2, pad.seq);
    write(4, pad.guestMs);
    write(12, pad.buttons);
    for (size_t i = 0; i < 4; ++i) write(16 + 2 * i, pad.axes[i]);
    out[24] = std::byte(pad.l2);
    out[25] = std::byte(pad.r2);
    if (!pad.version) return out;
    out[26] = std::byte(pad.version);
    out[27] = std::byte(count);
    for (size_t i = 0; i < 2; ++i) {
        const auto& t = pad.touches[i];
        out[28 + 6 * i] = std::byte(uint8_t(t.id));
        write(28 + 6 * i + 2, t.x);
        write(28 + 6 * i + 4, t.y);
    }
    write(40, pad.gyro);
    write(52, pad.accel);
    write(64, pad.motionTimestampUs);
    for (size_t i = 0; i < count; ++i) {
        const size_t at = kExtendedHeaderSize + kMotionSampleSize * i;
        write(at, pad.motionSamples[i].gyro);
        write(at + 12, pad.motionSamples[i].accel);
        write(at + 24, pad.motionSamples[i].timestampUs);
    }
    return out;
}

// Standard gamepad button index -> DualSense button. L2/R2 (6, 7) are analog and handled
// apart; 17 is the touchpad click Chromium reports for a DualSense.
inline constexpr std::array<uint32_t, 18> kStandardToDualSense{
    remoteplay::ControllerState::Cross, remoteplay::ControllerState::Circle,
    remoteplay::ControllerState::Square, remoteplay::ControllerState::Triangle,
    remoteplay::ControllerState::L1, remoteplay::ControllerState::R1,
    0, 0,
    remoteplay::ControllerState::Share, remoteplay::ControllerState::Options,
    remoteplay::ControllerState::L3, remoteplay::ControllerState::R3,
    remoteplay::ControllerState::Up, remoteplay::ControllerState::Down,
    remoteplay::ControllerState::Left, remoteplay::ControllerState::Right,
    remoteplay::ControllerState::PS, remoteplay::ControllerState::Touchpad};

inline remoteplay::ControllerState toControllerState(const GuestPad& pad) {
    remoteplay::ControllerState state;
    state.inputActive = pad.connected;
    if (!pad.connected) return state;
    for (size_t i = 0; i < kStandardToDualSense.size(); ++i)
        if (pad.buttons & (1u << i)) state.buttons |= kStandardToDualSense[i];
    state.leftX = pad.axes[0];
    state.leftY = pad.axes[1];
    state.rightX = pad.axes[2];
    state.rightY = pad.axes[3];
    // A trigger the browser reports only as pressed still pulls fully.
    state.l2 = (pad.buttons & (1u << 6)) && !pad.l2 ? 255 : pad.l2;
    state.r2 = (pad.buttons & (1u << 7)) && !pad.r2 ? 255 : pad.r2;
    state.touches = pad.touches;
    if (pad.motionValid) {
        state.motionValid = true;
        state.gyro = pad.gyro;
        state.accel = pad.accel;
        state.motionTimestampUs = pad.motionTimestampUs;
        const size_t count = std::min<size_t>(pad.motionSampleCount, state.motionSamples.size());
        std::copy_n(pad.motionSamples.begin(), count, state.motionSamples.begin());
        state.motionSampleCount = uint8_t(count);
    }
    return state;
}

// A BooVI client's pad as the extended message: the inverse of toControllerState.
inline GuestPad fromControllerState(const remoteplay::ControllerState& state, uint16_t seq, double clientMs) {
    GuestPad pad;
    pad.version = kExtendedVersion;
    pad.seq = seq;
    pad.guestMs = clientMs;
    pad.connected = state.inputActive;
    for (size_t i = 0; i < kStandardToDualSense.size(); ++i)
        if (kStandardToDualSense[i] && (state.buttons & kStandardToDualSense[i])) pad.buttons |= 1u << i;
    pad.axes = {state.leftX, state.leftY, state.rightX, state.rightY};
    pad.l2 = state.l2;
    pad.r2 = state.r2;
    pad.touches = state.touches;
    pad.motionValid = state.motionValid;
    if (state.motionValid) {
        pad.gyro = state.gyro;
        pad.accel = state.accel;
        pad.motionTimestampUs = state.motionTimestampUs;
        // The newest samples when more were read than one message carries.
        const size_t count = std::min<size_t>(state.motionSampleCount, state.motionSamples.size());
        const size_t keep = std::min(count, kMaxMotionSamples);
        std::copy_n(state.motionSamples.begin() + ptrdiff_t(count - keep), keep, pad.motionSamples.begin());
        pad.motionSampleCount = uint8_t(keep);
    }
    return pad;
}

// ---- feedback, host -> BooVI client ----
inline constexpr uint8_t kFeedbackKind = 'F';
inline constexpr size_t kFeedbackHeaderSize = 30;
inline constexpr size_t kMaxHapticSamples = 600;

inline std::vector<std::byte> encodeFeedbackMessage(const remoteplay::ControllerFeedback& f) {
    const size_t n = std::min(f.haptics.size(), kMaxHapticSamples);
    std::vector<std::byte> out(kFeedbackHeaderSize + 2 * n);
    out[0] = std::byte(kFeedbackKind);
    out[1] = std::byte(uint8_t(1));
    out[2] = std::byte(uint8_t((f.rumble ? 1 : 0) | (f.triggers ? 2 : 0) | (f.motionReset ? 4 : 0)));
    out[3] = std::byte(f.left);
    out[4] = std::byte(f.right);
    std::memcpy(out.data() + 5, f.leftTrigger.data(), 11);
    std::memcpy(out.data() + 16, f.rightTrigger.data(), 11);
    const uint16_t count = uint16_t(n);
    std::memcpy(out.data() + 28, &count, 2);
    if (n) std::memcpy(out.data() + kFeedbackHeaderSize, f.haptics.data(), 2 * n);
    return out;
}

inline std::optional<remoteplay::ControllerFeedback> parseFeedbackMessage(std::span<const std::byte> bytes) {
    if (bytes.size() < kFeedbackHeaderSize || uint8_t(bytes[0]) != kFeedbackKind || uint8_t(bytes[1]) < 1) return std::nullopt;
    uint16_t count = 0;
    std::memcpy(&count, bytes.data() + 28, 2);
    if (count > kMaxHapticSamples || bytes.size() < kFeedbackHeaderSize + 2 * size_t(count)) return std::nullopt;
    if (uint8_t(bytes[1]) == 1 && bytes.size() != kFeedbackHeaderSize + 2 * size_t(count)) return std::nullopt;
    remoteplay::ControllerFeedback f;
    const uint8_t flags = uint8_t(bytes[2]);
    f.rumble = flags & 1;
    f.triggers = flags & 2;
    f.motionReset = flags & 4;
    f.left = uint8_t(bytes[3]);
    f.right = uint8_t(bytes[4]);
    std::memcpy(f.leftTrigger.data(), bytes.data() + 5, 11);
    std::memcpy(f.rightTrigger.data(), bytes.data() + 16, 11);
    f.haptics.resize(count);
    if (count) std::memcpy(f.haptics.data(), bytes.data() + kFeedbackHeaderSize, 2 * size_t(count));
    return f;
}

// ---- the link a friend pastes: a share, https://go.example.com/<room>#t=<token>, or their own
// long-lived access, #a=<token> (GuestAccess.h). The room is the host's on BooVI's broker
// (GuestRoom.h); a link without one reaches the host's own server (the tailnet), or a broker's
// legacy room. The secret is read from the fragment, or from the query of an older link. ----
struct ShareLink {
    bool https = true;
    std::string host;      // DNS name or IPv4
    uint16_t port = 443;
    std::string room;      // the broker room's id, empty for a host's own server
    std::string token;     // base64url, never logged
    bool access = false;   // an access (a=): knock until the host lets it in
};

inline std::optional<ShareLink> parseShareLink(std::string_view text) {
    auto isSpace = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!text.empty() && isSpace(text.front())) text.remove_prefix(1);
    while (!text.empty() && isSpace(text.back())) text.remove_suffix(1);
    ShareLink link;
    auto startsWithNoCase = [&](std::string_view prefix) {
        if (text.size() < prefix.size()) return false;
        for (size_t i = 0; i < prefix.size(); ++i)
            if (char(std::tolower(uint8_t(text[i]))) != prefix[i]) return false;
        return true;
    };
    if (startsWithNoCase("https://")) text.remove_prefix(8);
    else if (startsWithNoCase("http://")) { text.remove_prefix(7); link.https = false; link.port = 80; }
    else if (text.find("://") != std::string_view::npos) return std::nullopt;
    const size_t end = std::min(text.find_first_of("/?#"), text.size());
    std::string_view authority = text.substr(0, end);
    if (authority.empty() || authority.find('@') != std::string_view::npos) return std::nullopt;
    if (const size_t colon = authority.rfind(':'); colon != std::string_view::npos) {
        const auto digits = authority.substr(colon + 1);
        if (digits.empty() || digits.size() > 5) return std::nullopt;
        uint32_t port = 0;
        for (char c : digits) { if (c < '0' || c > '9') return std::nullopt; port = port * 10 + uint32_t(c - '0'); }
        if (!port || port > 65535) return std::nullopt;
        link.port = uint16_t(port);
        authority = authority.substr(0, colon);
    }
    if (authority.empty() || authority.size() > 253) return std::nullopt;
    for (char c : authority)
        if (!(std::isalnum(uint8_t(c)) || c == '-' || c == '.')) return std::nullopt;
    link.host.assign(authority);
    for (auto& c : link.host) c = char(std::tolower(uint8_t(c)));
    // The path: a broker room's id, or nothing.
    std::string_view rest = text.substr(end);
    const size_t hash = std::min(rest.find('#'), rest.size());
    const std::string_view fragment = hash < rest.size() ? rest.substr(hash + 1) : std::string_view();
    rest = rest.substr(0, hash);
    const size_t query = std::min(rest.find('?'), rest.size());
    std::string_view path = rest.substr(0, query);
    while (!path.empty() && path.front() == '/') path.remove_prefix(1);
    while (!path.empty() && path.back() == '/') path.remove_suffix(1);
    if (!path.empty()) {
        if (path.size() != 22) return std::nullopt;
        for (char c : path) if (!(std::isalnum(uint8_t(c)) || c == '-' || c == '_')) return std::nullopt;
        link.room.assign(path);
    }
    // The secret: t= or a= in the fragment, else in the query.
    auto take = [&](std::string_view params) {
        while (!params.empty()) {
            const size_t amp = std::min(params.find('&'), params.size());
            const std::string_view param = params.substr(0, amp);
            if (param.starts_with("t=")) { link.token.assign(param.substr(2)); link.access = false; }
            else if (param.starts_with("a=")) { link.token.assign(param.substr(2)); link.access = true; }
            params.remove_prefix(std::min(amp + 1, params.size()));
        }
    };
    if (query < rest.size()) take(rest.substr(query + 1));
    take(fragment);
    if (link.token.size() < 8 || link.token.size() > 128) return std::nullopt;
    for (char c : link.token)
        if (!(std::isalnum(uint8_t(c)) || c == '-' || c == '_')) return std::nullopt;
    return link;
}

// The state the PS5 receives. `guest` is empty while no guest is connected: the host plays
// alone whatever the mode.
inline remoteplay::ControllerState combine(ControlMode mode, const remoteplay::ControllerState& host,
                                           const std::optional<remoteplay::ControllerState>& guest) {
    if (!guest || mode == ControlMode::WatchOnly) return host;
    if (mode == ControlMode::GuestPlays) return *guest;
    return remoteplay::mergeInputs(host, *guest);
}

} // namespace veyra::guest
