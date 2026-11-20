#pragma once
// Fork, partage: one remote guest watches the stream BooVI presents and plays with their
// own pad, from a browser, over WebRTC.
//
// - Picture: what the guest receives (GuestQuality.h): every real picture, taken before the
//   host's frame generation and pacing on a live source, or the presented stream with its
//   generated frames (fluide 60, fluide 120); at 1080p or the host's own (super-resolved)
//   size; encoded by NVENC in its low-latency mode (CBR, one-frame VBV, no periodic IDR),
//   sent as H.264 RTP with NACK retransmission and a PLI-driven IDR. The host sets the most
//   it allows; the page picks within it, by hand or automatique from its link test.
// - Link test: on the page's request the host sends a rising ramp of padding on the
//   "probe" data channel (unordered, no retransmission), over the same ICE pair as the media.
// - Sound: what the player renders, Opus 10 ms.
// - Data channels: "input" (unordered, no retransmission) carries the guest's pad
//   (GuestProtocol.h), "feedback" the rumble, "stats" the clock-sync pings, per-frame
//   timings and the page's latency reports.
// - Signaling: the guest's offer/answer (/session, /answer), guarded by a random token per
//   share session, or by a friend's long-lived access (GuestAccess.h) once the AccessDesk lets
//   their visit in: until then /session answers 202 with the visit's state. Wrong
//   credentials lock out their source address only (SourceLimiter). One guest at a time: a
//   new guest with the token replaces the previous connection; kick() disconnects and
//   changes the token.
// - Reach::Broker (default): BooVI's broker (GuestBroker.h, broker/) serves the page over
//   https from Cloudflare's edge and relays the signaling through this PC's room, which the
//   host opens outbound. Media stay peer to peer, found with STUN; the relay when the direct
//   path fails is Cloudflare TURN, lent per guest by the broker to the owner key's holder, or
//   the user's own TURN key; without either, direct only.
// - Reach::Tailnet: a small HTTP server on loopback and the Tailscale address serves the page
//   and the signaling (GuestHttp.h).
//
// The input thread (RemotePlayInput.cpp) reads guestState() every 1 ms and merges it with
// the host's pad per mode(); the engine hands capture() each real picture, before frame
// generation and pacing on a live source (PresentCapture.h, EarlyCapture).
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "veyra/engine/PresentCapture.h"
#include "veyra/guest/GuestProtocol.h"
#include "veyra/guest/GuestQuality.h"
#include "veyra/remoteplay/Types.h"

namespace veyra::guest {

class AccessDesk;

enum class Phase : uint8_t { Off, Waiting, Connecting, Connected };
// How the guest reaches this PC. Broker: https://<broker>/<room id>, the guest installs
// nothing. Tailnet: the guest is in (or shares) the host's tailnet, media bind the Tailscale
// address.
enum class Reach : uint8_t { Broker, Tailnet };
enum class BrokerPhase : uint8_t { Off, Connecting, Connected };
// Where a guest's relay comes from when the direct path fails.
enum class Relay : uint8_t { None, Owner, OwnKey };

// The broker this build shares through (BOOVI_BROKER_URL), or VEYRA_GUEST_BROKER's; empty: none.
std::string brokerUrl();
// The broker link of this PC's room (https://<broker>/<room id>, the room key made on first
// use), empty when this build has no broker. A friend's access link adds #a=<token> to it.
std::string brokerRoomLink();
// What a share through the broker would relay with (the config in GuestCloudflare.h).
Relay configuredRelay();

struct ShareSettings {
    Limit limit;                 // the most the guest may receive: feed, size, the host's upload
    Reach reach = Reach::Tailnet;
    uint16_t port = 47800;       // tailnet: the local server's
    std::string webRoot;         // tailnet: the folder holding index.html (the guest page)
    std::string fontFile;        // tailnet: JetBrains Mono, served at /font.ttf
};

struct ShareStatus {
    Phase phase = Phase::Off;
    ControlMode mode = ControlMode::GuestPlays;
    std::string token;           // empty while off
    uint16_t port = 0;
    std::string tailscaleIp;     // empty when Tailscale is not up
    Reach reach = Reach::Tailnet;
    BrokerPhase broker = BrokerPhase::Off;
    std::string publicUrl;       // broker: https://<broker>/<room id>
    Relay relay = Relay::None;
    std::string remote;          // guest's address as ICE selected it
    std::string candidate;       // remote candidate type: "host", "srflx", "prflx", "relay"
    bool relayed = false;        // either end of the selected pair is a TURN relay
    std::string encoder;
    std::string error;
    uint32_t width = 0, height = 0;
    uint32_t sourceHeight = 0;   // the tap's own height: what "native" sends
    uint32_t targetKbps = 0;
    Quality quality;             // what the guest receives now
    Limit limit;                 // the host's maximum
    bool automatic = true;       // the guest's choice: automatique, or manuel
    uint32_t linkKbps = 0;       // the page's link test, kb/s; 0 until tested
    double linkRttMs = -1, linkLossPercent = -1;
    double displayHz = -1;       // guest report: the guest's screen refresh
    double guestFps = -1;        // guest report: pictures its page showed a second
    double sentMbps = 0, fps = 0, rttMs = -1;
    uint64_t frames = 0, inputs = 0;
    bool padConnected = false, synthetic = false;
    bool padSilent = false;      // the guest's pad has sent states for 15 s without any button
    bool nativeClient = false;   // the guest joined from the BooVI app (GuestClientSource), not a browser
    std::string accessName;      // the friend's access ("lucas") when they came in with one, else empty
    double idleSeconds = 0;      // since the guest's pad last changed (a button, a trigger, a stick)
    std::string padName;         // guest report: the pad as the page names it, empty without one
    std::string padMapping;      // guest report: "standard", "remap", "non-standard", "steam-hid", "none"
    // Latency, ms, median of the last window; -1 until measured.
    double presentToSentMs = -1;     // host: picture handed over -> NVENC done -> RTP handed to the transport
    double encodeMs = -1;            // host: GPU conversion + NVENC
    double decodedToSentMs = -1;     // host, live source: picture decoded here -> RTP handed to the transport
    double decodedToSentRealMs = -1;   // the same, last measured on real pictures (the early tap)
    double decodedToSentSmoothMs = -1; // the same, last measured in a fluide feed (the present tap)
    double presentToDisplayMs = -1;  // guest report: picture handed over -> guest compositor (clock-synced)
    double decodedToDisplayMs = -1;  // guest report, live source: picture decoded on the host -> guest compositor
    double receiveToDisplayMs = -1;  // guest report: packet received -> displayed
    double decodeMs = -1;            // guest report: processingDuration
    double inputToHostMs = -1;       // guest report: pad read -> host received (clock-synced)
    double inputToPs5Ms = -1;        // guest report: pad read -> handed to the PS5 session
    double lossPercent = 0;
};

class GuestStreamer {
public:
    GuestStreamer();
    ~GuestStreamer();
    GuestStreamer(const GuestStreamer&) = delete;
    GuestStreamer& operator=(const GuestStreamer&) = delete;

    // The engine's consumer; register once with EngineController::setEarlyCapture.
    engine::EarlyCapture& capture();

    // UI thread.
    bool start(const ShareSettings& settings, std::string& error);
    void stop();
    bool active() const;
    void kick();                         // disconnect the guest and change the token
    void setMode(ControlMode mode);
    ControlMode mode() const;
    void setLimit(const Limit& limit);
    ShareStatus status() const;
    // The friends' accesses; null turns them off. Must outlive this streamer's share sessions.
    void setAccessDesk(AccessDesk* desk);

    // PS5 input thread. Empty while no guest is connected; a released pad when the guest
    // has been silent past kGuestStale.
    std::optional<remoteplay::ControllerState> guestState(remoteplay::HostTime now);
    // The state just handed to the PS5 session included guestState()'s latest input; the guest's
    // oldest change not yet handed is timed from its arrival.
    void handedToPs5(const remoteplay::ControllerState& sent);
    void feedback(const remoteplay::ControllerFeedback& feedback);

private:
    struct Impl;
    std::shared_ptr<Impl> p_;
};

} // namespace veyra::guest
