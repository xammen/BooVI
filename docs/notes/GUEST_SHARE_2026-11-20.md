# Partage: a remote friend watches and plays the PS5 stream (2026-11-20)

Goal: a friend anywhere on the internet sees the PS5 stream BooVI presents and plays it with
their own pad, from a browser, with as little latency as possible.

## How it works

```
PS5 ─chiaki─▶ BooVI (decode, SR, frame generation, present) ─▶ host screen
                     │ each real picture, taken after SR and before frame generation and pacing
                     ▼
              NV12 1080p (GuestNv12.hlsl) ─▶ NVENC P2 ultra-low-latency, CBR, 1-frame VBV
                     ▼
              H.264 RTP + NACK + PLI→IDR, Opus 10 ms ──WebRTC (libdatachannel)──▶ browser
browser pad (Gamepad API, rAF + 4 ms) ─"input" data channel (unordered, 0 retransmit)─▶
              PS5 input thread (1 ms) ─combine(mode)─▶ PS5 session
PS5 rumble ─▶ "feedback" channel ─▶ vibrationActuator
```

- `include/veyra/guest/GuestStreamer.h`, `src/guest/*`: encoder, WebRTC peer, HTTP server for
  the page and its offer/answer (loopback, plus the Tailscale address for `tailnet`; never 0.0.0.0).
- `src/guest/GuestTunnel.*`: cloudflared for the share session (named tunnel or quick tunnel), in a
  kill-on-close job object. `src/guest/GuestCloudflare.*`: the DPAPI secrets and Cloudflare TURN
  credentials. `include/veyra/guest/CloudflaredLog.h`: what is read from cloudflared's output
  (tested in `tests/fork/CloudflaredLogTests.cpp`).
- `include/veyra/guest/GuestProtocol.h`: the 28-byte input message, standard gamepad → DualSense,
  the three control modes. Tested in `tests/fork/GuestProtocolTests.cpp`.
- `web/guest/index.html`: the guest page (BooVI sage palette, JetBrains Mono, French). Its `lab`
  block holds the optional canvas stage, camera anticipation and the simulated game.
- `src/ui/GuestShareModel.cpp` (`vyGuest`), `qml/Veyra/SharePanel.qml`, `GuestChip.qml`, the
  `partage` settings section. Preferences: `guestBitrateMbps`, `guestResolution`, `guestReach`
  (`tunnel`, `quick`, `tailnet`; `tunnel` by default once a tunnel is configured), `guestMode`.
- Security: a 144-bit random token per share session, compared in constant time; 20 wrong
  tokens in a minute lock the endpoint for that minute; kick and stop change or drop the token.
  One guest at a time (a new connection with the token replaces the old one). The page gets the
  picture, the sound and the mode, nothing from PSN. The token is never logged.
- A guest that goes silent for 3 s (closed tab) is dropped, the host's pad takes over.

## Through Cloudflare (default): the friend installs nothing

```
friend's browser ──https──▶ Cloudflare ──tunnel──▶ cloudflared (child of BooVI) ──▶ 127.0.0.1:47800
                     page + POST /session, /answer only (ingress path allowlist, else 404)
friend's browser ◀══ WebRTC media: direct UDP (STUN) when possible, else Cloudflare TURN relay ══▶ BooVI
```

- `accès` → `<named tunnel>` (hostname `play.example.com`): `démarrer le
  partage` starts `cloudflared tunnel run` with the tunnel token in the child's environment
  (`TUNNEL_TOKEN`, never on its command line); the link is `https://play.example.com/?t=<token>`.
  The tunnel's ingress (set on Cloudflare) only forwards `/`, `/index.html`, `/font.ttf`,
  `/session` and `/answer` to `http://127.0.0.1:47800`; everything else is a 404 at the edge.
  With this reach the server listens on port 47800 only (the ingress points there).
- `accès` → `lien rapide`: `cloudflared tunnel --url http://127.0.0.1:<port>`, no account; the
  `https://*.trycloudflare.com` URL is read from cloudflared's output. No edge allowlist there; the
  server's own routes answer 404 for any other path.
- TURN: at share start BooVI asks Cloudflare (`rtc.live.cloudflare.com`, key `boovi`) for
  credentials valid 4 h and hands them to the page in the token-gated `/session` answer, after a
  STUN server. The host's own peer uses STUN and the UDP TURN server too. ICE keeps a direct pair
  when one works; the relay is only the fallback. The panel shows `direct` or `relais cloudflare`.
- Secrets: the tunnel token, the TURN key id and its API token live in
  `%LOCALAPPDATA%\Veyra\guest\cloudflare.dpapi`, DPAPI-encrypted for the Windows user, written by
  `scripts\fork\guest-cf-secrets.ps1` (JSON on stdin). Nothing in git, the logs or the prefs.
- cloudflared stops with the share, and with BooVI even on a crash (job object). The panel shows
  `tunnel cloudflare connecté` / `en connexion…` from cloudflared's registered connections.
- Set up once in the Cloudflare account (with the `cf` CLI): tunnel `boovi` (remotely managed),
  its ingress, a proxied CNAME `play` → `<tunnel id>.cfargotunnel.com`, TURN key `boovi`.
  To set up another PC: `cf tunnels token get <tunnel id>` gives the token again; the TURN API
  token is only shown at creation, so a new PC takes a new TURN key.

## User steps (tailnet)

Host (once): in the Tailscale admin console, share this PC with the friend (Machines → this PC →
Share), or invite them to the tailnet. Then for each session:

1. Play the PS5 in BooVI, open `partager` in the stream toolbar, `démarrer le partage`.
2. `activer https (tailscale serve)`: runs `tailscale serve --bg --https=8443 http://127.0.0.1:47800`.
   Browsers only read a pad over https. The first time, Tailscale gets a certificate for the
   PC's `*.ts.net` name (that name then appears in public certificate transparency logs).
3. `copier`, send the link privately. Pick `l'invité joue`, `à deux` or `regarder seulement`.
4. `couper l'invité` disconnects them and changes the link; `arrêter le partage` closes the
   server and turns off the `serve`/`funnel` it opened.

Friend: install Tailscale and accept the share, open the link in Chrome or Edge, `rejoindre`,
plug a pad and press a button. Keys: `f` full screen, `h` stats, `m` sound.

The Tailscale Funnel option is gone: `<named tunnel>` and `lien rapide` replace it, with a TURN relay.

## Latency, measured

`GUEST_LATENCY_2026-11-20.md` has the budget stage by stage, the live session's numbers and what
changed since this table: on a live source the guest's picture no longer waits for the host's
present (decoded → sent 6.5 ms instead of 25–30 ms with frame generation on). The table below was
taken with the tap at the present and frame generation's hold outside its first row.

One PC only (RTX 5080, Edge 157 headless on the same PC). "Tailscale address" is the browser
joining `http://100.64.x.y:47800`: Windows routes it locally, so it does not cross the network.
Medians of 2 s (guest) or 5 s (host) windows; ranges are across windows and runs.

| Leg | Loopback | Tailscale address | Source |
|---|---|---|---|
| host present → NVENC done | 1.9–2.1 ms | 1.9–2.1 ms | host log `guest-latency` |
| host present → RTP handed to the transport | 2.6–3.0 ms | 2.5–3.0 ms | host log |
| sent → received (clock-synced) | 0.0–0.2 ms | 0.1–0.2 ms | page, `stats` channel |
| decode (`processingDuration`) | 0.3 ms | 0.3–0.5 ms | page, `requestVideoFrameCallback` |
| received → displayed | 2.2–3.9 ms | 4.3–5.3 ms | page |
| **host present → guest display** | **4.8–6.9 ms** | **7.2–8.7 ms** | page, clock-synced |
| pad read → host received | 0.16–0.19 ms | 0.15–0.25 ms | page + acks |
| **pad read → handed to the PS5 session** | **0.3–0.7 ms** | **0.4–0.8 ms** | page + acks |
| session queue (handed → chiaki) | p50 0.01 ms, p99 ≤ 0.3 ms | same | `remoteplay-input-timing` |

Stream: 1920×1080, 57–60 fps (the PS5's real frames; the host showed 120 with FG), 17–19 Mb/s
at a 20 Mb/s target, 0 loss. Synthetic right-stick sweep (page test mode) reached the PS5 send
path (`last rx=… ry=…` turning in `guest-latency`).

`received → displayed` depends on where the frame lands in the browser's 60 Hz frame clock:
three back-to-back runs on the same video gave 6.7, 8.4 and 19.1 ms host→display, steady within
each run. Expect 0–16.7 ms of it on a 60 Hz screen.

libdatachannel's `PacingHandler` (opt-in, `VEYRA_GUEST_PACING=1`) measured 47.5 ms host→display
against 19.1 ms right before it (sent→received 7.4 ms against 0.07 ms): it drains on a thread-pool
timer. Off by default; the one-frame VBV already keeps each frame to its share of the link.

## Through Cloudflare, measured (2026-11-20)

Same PC, Edge 157 headless opening `https://play.example.com/?t=…` (page and signaling through the
named tunnel), a local video shared with `--share`, 1080p60, 0 loss, Opus ~129 kb/s received.
Medians of 2 s windows over 30 s per run.

| | path (page's selected pair) | host present → guest display | sent → received | ping |
|---|---|---|---|---|
| default ICE | `direct` (host/host) | 15.7 ms (8.4–16.3) | 0.1 ms | 0 ms |
| `relay=1` (relay only) | `relais cloudflare` (relay/prflx) | 11.5 ms (7.3–17.5) | 1.6 ms | 3 ms |

The relay run crosses Cloudflare's TURN server and back (3 ms RTT from here); the display figure
mostly reflects where frames land in the 60 Hz refresh (see above), not the path. Edge routes:
`/` and `/font.ttf` 200, `/favicon.ico`, `/admin`, `/index.html/../secret` 404, a wrong token 403.
cloudflared received the token through its environment (its command line has none) and was gone
once BooVI closed. No token or key in `logs/veyra-qml.log` or the prefs.

## Estimated for a real friend (not measured)

- Picture: what the host sees + ~3 ms (capture, NVENC, send) + one-way network (Tailscale direct:
  RTT/2; through a DERP relay: more) + 1–3 ms hardware decode + 0–16.7 ms screen refresh.
- Pad: browser poll (≤ 4 ms) + one-way network + ~0.5 ms host + chiaki to the PS5 (unchanged).
- The panel and the page show the real ping, path (`direct` or `relayé · derp …`) and both
  latencies once a friend is connected.

## What the guest receives

The host sets the most the guest may receive (Réglages → partage, and the share panel during a
share): `images réelles`, `fluide 60` or `fluide 120` (`guestFeed`), `1080p` or `native`, the
host's super-resolved output (`guestResolution`), and its upload (`envoi max`, `guestBitrateMbps`,
50 Mb/s by default). The guest picks within it on the page (`qualité`, key `q`):

- `images réelles`: the early tap, before frame generation (lowest latency), at most 60 a second.
- `fluide 60` / `fluide 120`: the presented stream, generated frames included, decimated on an
  even grid of 60 or 120 a second by present time (a frame is taken from a quarter interval before
  its slot). It waits for the host's frame generation like the host's own screen does.
- `native` keeps the tap's own size (2560×1440 here): no extra copy, the NV12 pass just doesn't scale.
- H.264 offered at level 5.2 (`640c34`); NVENC's frame rate follows the mode without a new IDR;
  a size change opens a new encoder.

On connection the page runs a link test: the host sends a ramp of 1100-byte messages on an
unordered, no-retransmission data channel (`probe`, same ICE pair as the picture), 300 ms per step
from 4 Mb/s up to 1.5× the host's upload. Per step the page measures arrival rate, loss and queueing
(one-way delay over its lowest); two congested steps stop the ramp. The result (fastest step plus the
picture received meanwhile) goes back to the host and shows as `connexion : 91 Mb/s · ping 1 ms ·
direct`, with the mode that fits it with a 25 % margin. `retester la connexion` runs it again.

`automatique` (default) starts on real pictures, takes the best rung that fits the test, then follows
the page's 2 s reports (`GuestQuality.h`, tested in `tests/fork/GuestQualityTests.cpp`): loss above
1 % or the pictures' network time 25 ms over its 30 s floor cuts the bitrate (−10 %, −25 % past 3 % or
60 ms) and lowers the link estimate; a rung starved below 70 % of its bitrate steps down (fluide 120 →
60 → réelles at 1080p; native → 1080p). Clean reports climb back 10 % per 6 s up to the tested link,
30 s after the last loss or step. `manuel`: the guest's feed, size and a bitrate cap up to the host's
upload; loss still lowers the bitrate.

Measured 2026-11-20, loopback, headless Edge 157 on this PC (360 Hz), a clip replayed as a live
source with DLSS FG 2X and SR to 2560×1440. Medians of 5 s windows over 25–30 s.

| | real · 1080p | fluide 60 · 1080p | fluide 120 · 1440p |
|---|---|---|---|
| source (clip, FG 2X) | 30 → 60 | 30 → 60 | 60 → 120 |
| pictures shown by the page (`presentedFrames`), dropped | 30.0, 0 | 60.0, 0 | 120.0, 0 |
| host decoded → sent | 6.5–7.5 ms | 24.6–25.0 ms | 20.3–27.2 ms |
| host tap → NVENC done | 4.8–5.2 ms | 2.0–2.2 ms | 3.2–5.2 ms |
| decoded on the host → page display | 11.6 ms | 29.0 ms | 30.5 ms |
| CBR target / sent | 20 / 6.6–7.8 Mb/s | 25 / 18–19.7 Mb/s | 50 / 30–35 Mb/s |

Automatique with a test bottleneck (`VEYRA_TEST_GUEST_LIMIT=25000@15`: 25 Mb/s from 15 s after the
guest connects, tail drop, NACKs ignored): the test measured 91 Mb/s, fluide 120 at 1440p; 13 s after
the cap, 1080p at 120; 30 s steady at 17–23 Mb/s, page loss 0.2 % on average.

## The guest's controller

The page reads the pad three ways and sends the same 28-byte state (`GuestProtocol.h`, unchanged):

- Gamepad API, standard layout: as before.
- Gamepad API, any other layout: "configurer ma manette" prompts each DualSense control in turn
  (buttons, triggers read as axes resting at -1, sticks by direction, a d-pad hat read as one axis).
  The mapping is kept in the guest's localStorage per gamepad id, with "refaire" and "réinitialiser".
- A Steam Controller over WebHID ("connecter une manette steam", Chrome or Edge). Without Steam the
  controller is a keyboard and mouse, so the Gamepad API never sees it. The page turns lizard mode off,
  reads its reports and maps them like Steam's default layout: right pad = right stick, left pad click =
  d-pad. 2015 controller wired (0x1102) or through its dongle (0x1142); 2025 controller (0x1302-0x1305).
  Protocol from SDL 3.4.14 (`SDL_hidapi_steam.c`, `SDL_hidapi_steam_triton.c`). The Bluetooth 2015
  controller is not handled. Not tried on a real controller: the parser is tested on built reports
  (`tests/fork/GuestSteamPadTests.mjs`).

With no pad the page says "aucune manette détectée" and offers WebHID or Steam Input (browser added to
Steam as a non-Steam game, or a gamepad desktop layout). It reports the pad name and its mapping
(`pad`, `map` in the stats report). The share panel shows "manette de l'invité : …", warns when the
guest has no pad or when the pad has sent states for 15 s without a button, and warns that "regarder
seulement" ignores the guest. `guest-latency` logs `received buttons=0x…`, what came from the page
before any PS5 session.

## Limits

- The page is public through Cloudflare. The 144-bit token, its constant-time compare, one guest
  at a time and kick stand as before. The wrong-token lockout is global: someone who knows the
  hostname can send 20 wrong tokens and keep the endpoint locked for a minute at a time.
- Browsers expose no gyro, no touchpad surface (Chromium reports its click as button 17, mapped),
  no adaptive triggers, no speaker or LED. Rumble works in Chromium.
- NVIDIA only (NVENC). H.264 for browser compatibility; Edge accepted the offer as profile
  42e02a and decodes NVENC's High stream. Firefox decodes H.264 through OpenH264 (baseline only):
  use Chrome or Edge.
- The bitrate drops on loss reported by the page (>1 %: −10 %, >3 %: −25 %, floor 4 Mb/s) and
  climbs back 10 % per 6 s without loss. Not exercised under real loss.
- Sony's terms do not foresee someone else playing remotely on the account.

## Reproduce

```
scripts\fork\build.ps1 -Base <Veyra 2.1.9 portable> -Out <app>
# no PS5: a local video, a headless Edge joins with the synthetic stick sweep
<app>\veyra_qml_ui.exe --share <video.mp4>          (VEYRA_GUEST_LINK_FILE=<file> gets the link)
scripts\fork\guest-probe.ps1 -Url <link> -Test -Seconds 30 -Shot guest.png
# a fake pad: a non-standard one configured through the page (#fake=remap), or a Steam Controller
# over WebHID (#fake=steam); scripts\fork\guest-pad-fake.js says what the host log should show
scripts\fork\guest-probe.ps1 -Url "<local link>#fake=remap" -Init scripts\fork\guest-pad-fake.js -Seconds 65
# panel and settings screenshots
scripts\fork\shot.ps1 -App <app> -Out panel.png -Page stream-preview -Extra --share-panel
scripts\fork\shot.ps1 -App <app> -Out settings.png -Page set-partage
```

`scripts\fork\guest-latency.ps1` forces the tailnet reach (`VEYRA_GUEST_REACH=tailnet`), so a test
run never opens the named tunnel; `-Query 'auto=0&feed=120&size=native&mbps=52'` picks a mode.

Through Cloudflare: Réglages → partage → accès → `<named tunnel>` (or `lien rapide`), then the same
`--share` run; the link file gets the `https://` link once it is known (`VEYRA_GUEST_PORT` moves
the server off 47800 when another BooVI already shares there; the named tunnel's ingress must point
at the same port). `-Relay` on the probe
forces the TURN relay (`iceTransportPolicy: "relay"`).

With the PS5: take the shared PS5 session lock first, run `--play-last --share`, then the
probe. `--share-kick-after N` exercises the kick.
