#pragma once
// Fork, partage: remote guests watch the stream BooVI presents, from a browser or their own
// BooVI, over WebRTC; one of them plays with their own pad.
//
// - Several viewers at once (RoomModel.h): each has its own WebRTC connection, its own NVENC
//   encoder (GuestVideoSet) and its own bitrate control (GuestQuality.h), so each one gets what
//   its link and decoder take; the host's upload (`envoi max`) is shared among them
//   (shareUpload). The room decides who holds the controller: only that viewer's pad reaches
//   the PS5, and only they get the rumble.
// - Picture: what each viewer receives: every real picture, taken before the host's frame
//   generation and pacing on a live source, or the presented stream with its generated frames
//   (fluide 60, fluide 120); at 1080p or the host's own (super-resolved) size; encoded by NVENC
//   in its low-latency mode (CBR, one-frame VBV, no periodic IDR), sent as H.264 RTP with NACK
//   retransmission and a PLI-driven IDR. The host sets the most it allows; each viewer picks
//   within it, by hand or automatique from its link test.
// - Link test: on a page's request the host sends a rising ramp of padding on its "probe" data
//   channel (unordered, no retransmission), over the same ICE pair as the media.
// - Sound: what the player renders, Opus 10 ms, one encoder for every viewer.
// - Data channels: "input" (unordered, no retransmission) carries the viewer's pad
//   (GuestProtocol.h), "feedback" the rumble, "stats" the clock-sync pings, per-frame timings
//   and the page's latency reports, "room" the room's snapshot and the viewer's commands (ask,
//   cancel, release, roll).
// - Signaling: the guest's offer/answer (/session, /answer), guarded by a random token per
//   share session, or by a friend's long-lived access (GuestAccess.h) once the AccessDesk lets
//   their visit in: until then /session answers 202 with the visit's state. Wrong credentials
//   lock out their source address only (SourceLimiter). A full room answers 409. A page that
//   reconnects (same `gid`) replaces its own previous connection and keeps its place in the room.
// - Reach::Broker (default): BooVI's broker (GuestBroker.h, broker/) serves the page over
//   https from Cloudflare's edge and relays the signaling through this PC's room, which the
//   host opens outbound. Media stay peer to peer, found with STUN; the relay when the direct
//   path fails is Cloudflare TURN, lent per guest by the broker to the owner key's holder, or
//   the user's own TURN key; without either, direct only.
// - Reach::Tailnet: a small HTTP server on loopback and the Tailscale address serves the page
//   and the signaling (GuestHttp.h).
//
// The input thread (RemotePlayInput.cpp) reads guestState() every 1 ms and merges it with
// the host's pad per mode(); hostPad() tells the room when the owner plays himself. The engine
// hands capture() each picture (PresentCapture.h, EarlyCapture).
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "veyra/engine/PresentCapture.h"
#include "veyra/guest/GuestProtocol.h"
#include "veyra/guest/GuestQuality.h"
#include "veyra/guest/RoomModel.h"
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
// The Discord webhook (GuestNotify.h), DPAPI-sealed in <user data>\guest\discord.dpapi; empty
// when none is set. Never logged.
std::string loadDiscordWebhook();
bool saveDiscordWebhook(const std::string& url);
void clearDiscordWebhook();

// The room's rules for a share session, changed with setRoom() while it runs.
struct RoomRules {
    RoomSettings room;            // capacity, turn length, dice allowed
    int idleMinutes = 20;         // the holder's pad still this long, with someone waiting: it passes on
    bool ownerPriority = true;    // a button on the host's own pad takes the controller back
    bool historyToGuests = false; // the snapshot carries the last plays
    Seat defaultSeat = Seat::Player;
};

struct ShareSettings {
    Limit limit;                 // the most a viewer may receive: feed, size; kbps is the host's whole upload
    Reach reach = Reach::Tailnet;
    uint16_t port = 47800;       // tailnet: the local server's
    std::string webRoot;         // tailnet: the folder holding index.html (the guest page)
    std::string fontFile;        // tailnet: JetBrains Mono, served at /font.ttf
    RoomRules rules;
};

// One viewer, as the host's panel shows it.
struct ViewerStatus {
    std::string tag;             // the room's public id
    std::string name;
    std::string accessId;        // the friend's access when they came in with one, else empty
    bool connected = false;      // media flow (else still connecting)
    bool nativeClient = false;   // joined from the BooVI app (GuestClientSource), not a browser
    Seat seat = Seat::Player;
    bool holder = false;
    int queuePosition = 0;       // 1-based, 0 not waiting
    std::string remote;          // address as ICE selected it
    std::string candidate;       // remote candidate type: "host", "srflx", "prflx", "relay"
    bool relayed = false;        // either end of the selected pair is a TURN relay
    std::string encoder;
    uint32_t width = 0, height = 0;
    uint32_t sourceHeight = 0;   // the tap's own height: what "native" sends
    uint32_t targetKbps = 0;
    uint32_t shareKbps = 0;      // its part of the host's upload
    Quality quality;             // what it receives now
    bool automatic = true;       // its choice: automatique, or manuel
    uint32_t linkKbps = 0;       // its link test, kb/s; 0 until tested
    double linkRttMs = -1, linkLossPercent = -1;
    double displayHz = -1;       // its report: the screen refresh
    double guestFps = -1;        // its report: pictures shown a second
    double sentMbps = 0, fps = 0, rttMs = -1;
    uint64_t frames = 0, inputs = 0;
    bool padConnected = false, synthetic = false;
    bool padSilent = false;      // the pad has sent states for 15 s without any button
    double idleSeconds = 0;      // since the pad last changed (a button, a trigger, a stick)
    std::string padName;         // its report: the pad as the page names it, empty without one
    std::string padMapping;      // its report: "standard", "remap", "non-standard", "steam-hid", "none"
    // Latency, ms, median of the last window; -1 until measured.
    double presentToSentMs = -1;     // host: picture handed over -> NVENC done -> RTP handed to the transport
    double encodeMs = -1;            // host: GPU conversion + NVENC
    double decodedToSentMs = -1;     // host, live source: picture decoded here -> RTP handed to the transport
    double decodedToSentRealMs = -1;   // the same, last measured on real pictures (the early tap)
    double decodedToSentSmoothMs = -1; // the same, last measured in a fluide feed (the present tap)
    double presentToDisplayMs = -1;  // its report: picture handed over -> its compositor (clock-synced)
    double decodedToDisplayMs = -1;  // its report, live source: picture decoded on the host -> its compositor
    double receiveToDisplayMs = -1;  // its report: packet received -> displayed
    double decodeMs = -1;            // its report: processingDuration
    double inputToHostMs = -1;       // its report: pad read -> host received (clock-synced)
    double inputToPs5Ms = -1;        // its report: pad read -> handed to the PS5 session
    double lossPercent = 0;
};

struct RoomStatus {
    std::string holder;          // tag, empty when free
    double heldSeconds = 0;
    bool ownerHolds = false;
    std::vector<std::string> queue;   // tags
    std::optional<DiceRoll> dice;
    double diceAgeSeconds = 0;
    RoomRules rules;
};

struct ShareStatus {
    Phase phase = Phase::Off;    // Connected: at least one viewer's media flow
    ControlMode mode = ControlMode::GuestPlays;
    std::string token;           // empty while off
    uint16_t port = 0;
    std::string tailscaleIp;     // empty when Tailscale is not up
    Reach reach = Reach::Tailnet;
    BrokerPhase broker = BrokerPhase::Off;
    std::string publicUrl;       // broker: https://<broker>/<room id>
    Relay relay = Relay::None;
    std::string error;
    bool stuck = false;          // a request past its deadline, or the control loop stopped
    bool controlAlive = true;    // the control loop ran in the last 5 s
    Limit limit;                 // the host's maximum; kbps: the whole upload
    std::vector<ViewerStatus> viewers;   // in the room's order
    RoomStatus room;
    double sentMbps = 0;         // every viewer together
    double playIdleSeconds = 0;  // since any player's pad last changed, or since the share had a viewer
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
    void kick(const std::string& tag);       // disconnect one viewer; a share-link guest cannot come back from that address
    void renewLink();                        // a new share token: every share-link viewer goes, friends stay
    void dropAccess(const std::string& accessId);   // a friend's visit ended
    void setMode(ControlMode mode);
    ControlMode mode() const;
    void setLimit(const Limit& limit);
    void setRules(const RoomRules& rules);
    // The host's room commands.
    void give(const std::string& tag);
    void takeBack();                         // the owner plays: as when his pad is used
    void handBack();                         // the owner hands the controller back to the queue
    void setSeat(const std::string& tag, Seat seat);
    void roll();
    // What the guests see as the last plays (when the rules say so), newest first.
    void setHistory(std::vector<PlayRecord> plays);
    std::vector<RoomEvent> drainEvents();
    ShareStatus status() const;
    // The friends' accesses; null turns them off. Must outlive this streamer's share sessions.
    void setAccessDesk(AccessDesk* desk);

    // PS5 input thread. Empty while no viewer holds the controller; a released pad when the
    // holder has been silent past kGuestStale.
    std::optional<remoteplay::ControllerState> guestState(remoteplay::HostTime now);
    // The host's own pad, as polled: a new button takes the controller back (ownerPriority).
    void hostPad(const remoteplay::ControllerState& state, remoteplay::HostTime now);
    // The state just handed to the PS5 session included guestState()'s latest input; the
    // holder's oldest change not yet handed is timed from its arrival.
    void handedToPs5(const remoteplay::ControllerState& sent);
    void feedback(const remoteplay::ControllerFeedback& feedback);

private:
    struct Impl;
    std::shared_ptr<Impl> p_;
};

} // namespace veyra::guest
