# Several friends on one share (BooVI 1.1, 2026-11-21)

Goal: a group of friends uses the owner's PS5 together and unattended. About four people join
one link; one holds the PS5's controller, the others watch live with sound. The host sets who
may play, passes the controller, and keeps a queue; a dice roll drawn on the host settles who
plays next. The PC can run it alone from the tray, and tells a Discord channel what happens.

## Where the state lives

The host app owns the room. The broker stays a relay: it already holds up to 8 guest sockets
per room, and a guest's socket closes once its answer is in, so nothing in-session goes
through it. In-session messages ride a new reliable data channel, `room`, on each guest's own
WebRTC connection. An old page ignores the channel; an old host never offers it. **Nothing has
to be deployed to the broker.**

```
guest page / BooVI client ── "room" channel ─▶ GuestStreamer ─▶ Room (RoomModel.h, pure)
      ◀── snapshot {t:"room", …} on every change ──┘                 │ events
                                                     GuestShareModel ◀┘ (UI thread, 500 ms)
                                                       ├─ play history (guest-plays.dpapi)
                                                       └─ Discord (GuestNotify.h gate, webhook in guest\discord.dpapi)
```

### The room model (`include/veyra/guest/RoomModel.h`, tested in `tests/fork/RoomModelTests.cpp`)

| Field | Meaning |
|---|---|
| `members` | join order; each: public id (6 chars, not the peer's signaling id), name, seat, client, the friend's access id |
| `seat` | set by the host: `Player` (may hold the controller, queue, roll) or `Watcher` (watch only) |
| `holder` | the member whose pad reaches the PS5, or none |
| `ownerHolds` | the owner took the controller back by playing on his own pad; the holder waits at the queue's front |
| `queue` | members waiting for the controller, in order |
| `dice` | the last roll: who launched it, every round of d6 per participant, the winner, when it applies |
| `turnUs` | the host's turn length; 0 none |

Operations are a reducer over that state: `join`, `leave`, `ask`, `cancel`, `release`, `give`
(host), `setSeat` (host), `ownerTake` / `ownerRelease`, `roll`, `tick`. Each change bumps a
revision; each emits typed events (`Joined`, `Left`, `PadTaken{why}`, `PadReleased{heldUs}`,
`Queued`, `DiceRolled`, `OwnerTook`, …) that the UI drains for history and Discord.

Rules:
- The controller is never handed to a `Watcher`, except by the host's own `give`.
- A free controller goes to the queue's head, else stays free until someone asks. The first
  player to join a free room gets it at once, as a single guest always did.
- The holder leaving, releasing, going idle (`gatewayIdleMinutes` without a pad change) or
  being made a watcher passes it to the queue's head. A turn timer moves the holder to the
  queue's back when someone waits.
- Owner priority: a button on the host's own pad while a guest holds the controller in
  `l'invité joue` takes it back. The guest goes to the queue's front and gets it back after
  two minutes without the owner's pad, or when the owner hands it back from the panel.
- Dice: any player (or the host) rolls when at least two players are in the room, at most
  one roll per 20 s, when the host allows it. Each player throws a d6 drawn on the host with
  BCryptGenRandom (rejection sampling, no modulo bias); the highest wins, ties throw again
  among themselves. Every round goes to every guest, which animates it for 3 s; the winner
  takes the controller when the animation ends; a holder who lost waits at the queue's back.
  By symmetry each player wins with probability 1/n; the test checks it over 60,000 rolls.

### Bitrate per viewer, and the host's upload

Each viewer has its own NVENC session and its own `RateControl` (GuestQuality.h), so each one
gets what its link and its decoder take, and its own link test. `envoi max`
(`guestBitrateMbps`) is now the host's whole upload for the share: `shareUpload` (RoomModel.h)
gives the holder what is left after a reserve for the spectators (half the upload split among
them, 4 to 10 Mb/s each), and the spectators split the rest; an upload too small for 4 Mb/s
each is split evenly, and the shares never add up past the upload. Every viewer's ceiling is set
through its `Limit.kbps`. With one guest nothing changes.

Encoders: `GuestVideoSet` is the engine's one `EarlyCapture` and fans out to one
`GuestVideo` per viewer (at most 6). A viewer on real pictures takes the early tap, a viewer on
a fluide feed the presented one, both in the same frame: `PresentCapture::Frame` now says which
tap calls (`early`, `live`), and each encoder picks its own.

### Protocol (`room` channel, JSON, both directions)

Host → guest, on every change and on connect: `{"t":"room","rev":n,"you":"<id>",
"members":[{"id","name","seat","native"}],"holder":"<id>|","owner":0|1,"held":s,"queue":[…],
"turn":s,"left":s,"dice":{"n","by","rounds":[[{"id","v"}]],"winner","in":ms}|null,
"allowDice":0|1,"max":n,"history":[{"name","at":unix,"s":s}]}`.

Guest → host: `{"t":"ask"}`, `{"t":"cancel"}`, `{"t":"release"}`, `{"t":"roll"}`. Every
command is checked against the sender's seat and the room's state on the host; the guest's
claims are never trusted. `/session` takes an optional `name` (24 characters, cleaned) for a
share-link guest; a friend's access brings its own name.

### Access, headless

- `AccessDesk` lets several friends in: the first one's knock starts the console (asking,
  waking, connecting as before, but each page connects as soon as the host said yes); later
  friends join the running console at once when their access does not ask, else after the
  host's approval. The visit ends when the last one has left, or when nobody's pad has moved
  for the idle delay.
- Tray, start with Windows (`--gateway-tray`), PS5 wake and rest were already there
  (GUEST_ACCESS_2026-11-20.md). Added: the PC stays awake while a guest is connected
  (`SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED)`), owner priority, and a
  watchdog that restarts a share whose broker requests are past their deadline
  (GuestBroker.cpp) with nobody connected, at most once a minute.

### Discord

`GuestNotify.h` (pure): events to French one-liners, a per-event switch from the settings
(`discordEvents`), and a gate that sends at most one message per 3 s and 20 per minute,
merging what waits into one message, and honours Discord's `retry_after`. The webhook URL is
typed in réglages → partage, checked to be a `discord.com` webhook, sealed with DPAPI in
`<user data>\guest\discord.dpapi`, shown back only as "configuré", never logged.

## Added from the perf audit of 2026-11-21

- **The player's automatique stops at real pictures** (GuestQuality.h `latencyFirst`), on a live
  source only: the fluide feeds wait for the host's frame generation. Spectators keep the fluide
  rungs; a manual choice stays. Each page shows the measured cost on the fluide buttons
  (`fluideMs` in the host's `st`: fluide minus real, decoded → sent, both measured in the last two
  minutes by any viewer). A file source keeps the old ladder: there real pictures come at their
  present, where frame generation can leave none (0 a second on 1.0.2 as well).
- **A friend connects while the console wakes**: from the host's yes on, the desk lets their
  `/session` through, so the TURN lease, ICE and the answer overlap the PS5's start; the page
  says "la ps5 se réveille" until the first picture. A visit with under 30 s of pictures (or none)
  keeps the console 90 s, so a retry finds it streaming.
- `guest-latency` gives each tap's last value with its age (`real X age N s`), the dated and
  undated frame counts of the window, and bitstreams that found no viewer or no open track. The
  two silent `/session` failures (peer not made, peer replaced while gathering) are logged.

## The 2026-11-20 23:02 wedge

After the guest went silent, four `/session` calls stopped at "new peer" and the UI thread
stopped (the PS5 input thread then ran two hours past its session, since only the UI tick stops
it). In 1.0.2 `status()` ran on the UI thread every tick and called the dying peer's `rtt()`
(`usrsctp_getsockopt`), and its local copy of the peer could be the last one, so
`~PeerConnection`, which waits in impl's `mProcessor.join()`, could run on the UI thread. The log
has no stack, so the exact lock is inferred, not proven.

Now no libdatachannel call runs on the UI thread or under one of our locks: a retired peer goes to
a teardown thread of its own (`PeerReaper`), which says goodbye, closes it and destroys it once
nobody else holds it; `status()` reads an rtt sampled by the control thread; mode and limit
changes only flag the control thread. Left as before: the PS5 input thread sends the holder's
rumble itself, and `stop()` joins the audio encoder, whose thread sends on the tracks.

## Measured (2026-11-21, one PC, RTX 5080, loopback or the tailnet address)

Host: a muted test instance sharing `clip60.mp4` (1080p60), own user data, never focused, ended by
`--exit-after`. Viewers: headless Edge 157 (`guest-probe.ps1`), and once a second instance of the
same build as a BooVI client. No PS5, no TURN lease.

| What | Result | Run (test harness, `runs\`) |
|---|---|---|
| 3 browser viewers, one encoder each | 59.5 fps to each; player real native 30 Mb/s; spectators fluide 120 in a 10 Mb/s share each; upload 50 Mb/s split 30/10/10 | smoke3 |
| queue, dice from a guest | zoé queued at 5 s; sam rolled; lucas won and kept it; every page animated the same roll | room1 |
| host commands (VEYRA_TEST_ROOM) | give → sam; seat zoé → watcher; take → free; hand back → sam; kick zoé → page "l'hôte t'a déconnecté"; host roll | room2 |
| native client in the room | "nico" (BooVI, native:1) joined, asked (queued), rolled (lucas won 4–2) | native |
| Discord, fake webhook on 127.0.0.1 | 8 posts in 41 s, bursts merged, ≥ 3 s apart, one 429 honoured (resent 3 s later), `allowed_mentions` empty, URL absent from the log | discord |
| watchdog (broker reach, VEYRA_TEST_GUEST_STALL=1) | stuck `/session` answered 503 at 10.2 s, share restarted 250 ms later, next guest 60 fps | watchdog |
| stuck teardown (VEYRA_TEST_GUEST_STALL_TEARDOWN=1) | UI logged its poll every 2 s through it; next viewer connected in 89 ms, 59 fps; host exited by itself | stall-teardown |

Player feed, live source (`VEYRA_TEST_CAPTURE_REPLAY`, DLSS FG 2X, SR to 1440p), one viewer on
automatique, 35 s each:

| | 1.0.2 (cb006b10) | this branch |
|---|---|---|
| automatique picks | fluide 120 native | real native |
| host decoded → sent | 19.3–26.6 ms (fluide) | 7.7–8.8 ms (real) |
| page host decode → display (`src`) p50 | 41.2 ms (n=10) | 25.6 ms (n=18); 30.1 ms (n=15) in a second run |
| sent | 60 a second | 60 a second |

With a spectator on fluide beside the player, both pages labelled the fluide buttons "+13 ms"
(fluide 21.2 ms against real 7.5 ms decoded → sent). The audit's 4X hold on the PS5 measured 36.7
against 11.85 ms, so its gain should be about 25 ms (inferred, not run here: no PS5).

Access join, the clip standing in for the PS5 (`VEYRA_TEST_ACCESS_SOURCE`), loopback:

| | 1.0.2 | this branch |
|---|---|---|
| visit begins → guest connected | 3.09 s (0.14 s after the console streamed) | 0.09 s (the console still starting) |
| visit begins → playing | 3.45 s | 3.6–3.9 s (console start 3.0–3.4 s, same as before) |
| retry 9 s after a short visit | console restarted (2.5–3.6 s) | console kept: playing 0.3 s after the visit began |

On loopback WebRTC takes 0.1 s, so the overlap barely shows; the 2026-11-20 internet join spent
2.5 s after the PS5 was ready (TURN lease, ICE 1.9 s), which now runs during its 5.3 s start
(inferred, not run over the internet).

## Not verified, or open

- No PS5, no internet friend: the room and the overlap were run on loopback and the tailnet
  address only; the broker reach only for the watchdog (its relay, no TURN).
- A screenshot of the host's panel during a live share: the panel is drawn over the native video
  window, which neither PrintWindow nor `grabWindow` captures here, and the test windows stay
  behind the owner's. The panel was checked with `--page stream-preview --share-panel`, and its
  live data through its bindings (`room`, `viewers`) during runs.
- Owner priority needs the host's own pad: covered by RoomModelTests, not run.
- Start with Windows, tray and PS5 rest were already in 1.0.2 and are unchanged; not re-run.
- Keep-awake: logged ("keeping the PC awake while guests watch"), not checked with powercfg.
- Pre-existing, found here: real pictures from a file with frame generation on reach the guest at
  0–15 a second (1.0.2 too). Live sources use the early tap and are not affected.
- A "partage arrêté" Discord notice queued as the app exits is not sent.

## Review fixes (2026-11-21, after two pre-release reviews)

| # | Commit | What was wrong, what changed |
|---|---|---|
| 8 | `7afb4b6a` | `shareUpload` kept a 4 Mb/s floor that did not fit: 20 Mb/s among 6 viewers gave out 24. The floor applies only when it fits; a sweep test checks sum ≤ upload. |
| 6 | `d8487a3b` | `AccessDesk::setPhase` approved `guests_.front()`: if the starter left during the wake, an "ask first" friend got in without the host. It approves the visit's own access id. |
| 5 | `e4f7ce04` | Friends were told apart by display name (watch-only, `dropAccess`, presence). Knock and `admitted()` return an `Admission` (access id, watch-only); the peer, room member, status and `dropAccess` carry the id. A new access may not reuse a name. |
| 4 | `ff6f121a` | Revoking an access dropped the friend from the desk silently: their connection stayed. `setAccesses` returns who left; `publish()` runs `leaveFriend` (history, `dropAccess`) for each. |
| 9 | `2c918da1` | The panel's disconnect only retired the peer: a friend's page knocked straight back in. For an access viewer it now ends their visit ("visite terminée"); the tip says "couper sa visite". |
| 10 | `77d340c4` | `/answer` accepted any valid credential for any peer. The peer keeps a `PeerCredential` (credential hash, and the address for a share-link viewer); `/answer` must match it. |
| 2 | `24ee0f10` | `onConnected` took an encoder and a room place before `connectedHandled`, racing `retirePeer`: an encoder kept encoding for a retired peer. Both move the peer's life under its `lifeMutex`; the shared Opus encoder follows the live viewers from one place (`syncAudio`). |
| 1 | `bdbf0340` | A reconnection whose new connection failed left the first member in the room for good (holding the controller, taking a place). A page's gid keeps its tag for the session; `Room::sweep` has a member with no connected peer for 25 s leave; `/session` counts room members against the size. |
| 3 | `7155dafe` | libdatachannel callbacks captured `Impl*` and could run after the streamer, from connections the teardown thread still held. They go through a `Lifeline` (Lifeline.h) that `~Impl` cuts. |
| 7 | `7faec735` | The stats channel took any size and rate and logged every report. `InboundBudget` (InboundBudget.h): 4 KB cap, token buckets per kind, room commands 4/s burst 8, a counted warning every 5 s; a repeated refusal is told once per 2 s; reports logged every 10 s, cut at 600 bytes. |
| 12 | `0ec202bd` | The page's `gate()` used `innerHTML` (now text nodes); the BooVI client's room chip shows the host's refusal for 3 s; the six encoders share one 3 s teardown wait. |
| 11 | none | Not a defect: the input thread's self-stop `break` leaves the loop, and the neutral pad (`engine_.remotePlayController({})`) is sent after the loop on every exit path. |

Test hooks: `VEYRA_TEST_GUEST_CONNECT_HOLD_MS` (`7f923672`, holds `onConnected` once it has an
encoder), `VEYRA_TEST_ACCESS_REVOKE_AT` (`7d055667`), and a warning for an encoder that encodes for
no viewer. Not changed (noted by the review): the audio anchor is measured once per anchor
(CaptureAudioSession.cpp).

Runs on the final build (`host11` in the harness, loopback or the tailnet address):

| Run | Result |
|---|---|
| reconnect flap (`repro-flap.ps1`, flap.js, hold 2.5 s), before the fixes | encoder 1 encoded ~300 pictures per 5 s for nobody for 25 s; the room ended with 3 members for 2 viewers ("lucas", "lucas 2") |
| same, after | no idle encoder; the orphan left after 25 s; the page came back under its own tag: 2 members for 2 viewers |
| flood (`repro-flood.ps1`, flood.js) | 25 948 messages in 5 s dropped past the budget with two counted warnings; 2 refusals sent back; 7 report lines in 35 s (≤ 487 characters); the other viewer at 60 fps |
| access visit, kick, revoke (`repro-join.ps1`) | 59.7 fps; kick: peer closed, page "visite terminée", no reconnection; revoke: peer closed 4 ms after the revocation |
| 3 viewers, queue, dice, Discord fake | 60 fps each, upload 30/10/10; zoé queued; sam's roll won by lucas; 7 posts, the 429 honoured, mentions off |
| native client, watchdog, stuck teardown | as before: nico asked, rolled, won; stuck `/session` 503 at 10 s and restart; UI alive through the hung teardown, next viewer 60 fps |

## Reproduce

Scripts in the local test harness (`%USERPROFILE%\dev\multi-test`, not in this repository; `quiet.ps1` starts apps
without focus): `run-multi.ps1` (host + N browser viewers, `-Queries` per viewer such as
`salon=1&ask=5&roll=10`, `-HostEnv @{VEYRA_TEST_ROOM='give:2@20,...'}`), `run-native.ps1`,
`repro-join.ps1 [-Early] [-HostEnv @{...}]`, `repro-watchdog.ps1`, `repro-stall-teardown.ps1`,
`repro-flap.ps1`, `repro-flood.ps1` (most take `-App`), and
`fake_webhook.py` for Discord (seal `http://127.0.0.1:<port>/...` into the test user data's
`guest\discord.dpapi`, set `VEYRA_TEST_DISCORD_LOCAL=1`).
