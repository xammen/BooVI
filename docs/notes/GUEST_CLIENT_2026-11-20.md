# Partage: a friend joins from their own BooVI (2026-11-20)

Goal: the friend who joins a share runs BooVI and does the frame generation and super
resolution on their own GPU, matched to their own screen. The browser page stays the
no-install way in.

## How it works

```
host BooVI ── real pictures (H.264, NVENC), Opus ── WebRTC ──▶ friend's BooVI
   /session {client:"boovi", input:1}, /answer             GuestClientSource: D3D12VA decode
                                                            ─▶ engine as a live stream ("guest:")
                                                            ─▶ SR, fluidité, DLSS / FSR 3.1 / VFG FG
friend's pad (SDL3, PS5 input thread, 1 ms) ── "input": extended message ──▶ host ─▶ PS5
host's rumble, trigger effects, haptics ── "feedback": binary 'F' message ──▶ friend's pad
```

- `include/veyra/source/GuestClientSource.h`, `src/source/GuestClientSource.cpp`: signaling over
  WinHTTP (http or https), the host's ICE servers (STUN, Cloudflare TURN over UDP), H.264 and Opus
  receive, the shared decoder, clock sync on the "stats" channel (ping/pong, best round trip of
  10 s) and the page's report (`rep`: loss, decode, host → screen) so the host's panel and its
  bitrate control work as for a browser.
- Engine: `EngineController::openGuestClient`; the source runs like the PS5 stream (live
  scheduling, decoded-picture wake, stream cadence and learnt phase). The PS5 input thread's pad
  reaches it through `remotePlayController`, the host's feedback through `remotePlayFeedback`.
- `include/veyra/guest/GuestProtocol.h`: the extended input message (version 1: the browser's 28
  bytes, then two touch points, gyro, accel, timestamp and up to 8 motion samples) and the binary
  feedback message. A host reads both; a host that does not offer `input` gets the 28 bytes.
  Tested in `tests/fork/GuestProtocolTests.cpp`.
- Host, additive: `/session` marks a BooVI client (`client=boovi` in the log, `nativeClient` in
  the status, "invité : application BooVI" in the panel), offers `input: 1`, and sends a BooVI
  client the whole DualSense feedback. The guest's pictures are the host's real pictures (the tap
  before frame generation): the friend generates locally.
- UI: `veyra.join` (`GuestJoinModel`), the home row "rejoindre un partage", the connect page's
  "partage" flow (`SetupJoin.qml`), the stream page as for the PS5 (stats card with loss and
  "hôte → écran", presets, keyboard/mouse, fullscreen). The pasted link (its token) is kept in
  memory only, never in the preferences or the log.

## Measured, one PC, loopback (2026-11-20)

Host: a 1080p60 clip shared with `--share`, preset performance (no SR, no FG), reach tailnet,
port 47810. Client: `--join-file`, preset fluidité (SR to 1440p, DLSS frame generation x2) on an
RTX 5080 and a 360 Hz screen, the synthetic pad (`VEYRA_TEST_GUEST_CLIENT_SYNTH=1`). 45 s run,
both instances at volume 0.

| | value |
|---|---|
| connect (offer to media) | 37 ms, path `direct` |
| client: decode | D3D12VA, 0.20 ms median |
| client: real pictures / shown | 60 / 120 a second (`gpuCompletedFps`, `presentSubmitFps`) |
| client: graph | `initialized src=1920x1080 work=2560x1440 sr=1 nr=0 fg=1` |
| host: picture handed over → RTP sent | 2.2–3.7 ms (p50 per 5 s) |
| host → client screen (clock-synced, capture on the host to Present here) | 19.7 ms median (17.7–24.0) |
| packet received → presented on the client (FG hold included) | 16.0 ms median |
| loss | 0 % |
| pad messages reaching the host | about 225 a second, extended v1, gyro, accel and touch in the host's `guest-latency` log |

The host here played a file, so there was no PS5 session to hand the pad to: the host's
`recvToHandedPs5` stays empty. The mapping to the PS5's state (motion and touch included) is the
tested `toControllerState`.

## Reproduce

```
scripts\fork\build.ps1 -Base <Veyra 2.1.9 portable> -Out <client app>
scripts\fork\stage-app.ps1 -Base <...> -Out <host app> -Build out\build\fork
# both apps: volume 0 in runtime_local\user-data-2.0.0\qml-preferences.v1.json; the host's
# guestReach "tailnet" so no tunnel starts.
VEYRA_GUEST_PORT=47810 VEYRA_GUEST_LINK_FILE=<f>  <host>\veyra_qml_ui.exe --share --preset performance <clip>
VEYRA_TEST_GUEST_CLIENT_SYNTH=1  <client>\veyra_qml_ui.exe --join-file <f> --preset fluid
```

## For the friend

`scripts\fork\package-guest.ps1 -App <staged app>` writes a portable zip (449 MiB) without user
data, logs, link files or secrets, with a `LISEZ-MOI-AMI.txt`. It leaves out three runtimes this
fork may not hand on: the community-modified unsigned `nr-ampere\nvngx_dlssnr.dll`, the
non-public `nr-original\nvngx_dlssnr.dll` and `dlssg-lite\nvngx_dlssg.dll` from the NVIDIA app's
OTA cache. The official DLSS SDK runtimes, RTX Video, FidelityFX 2.3 and XeSS 3.0 stay (their
licences allow shipping them with an application). Still to check: the NVIDIA Video Effects
runtimes (`runtime\nvidia-vfg`, `publicDistributionAudited: false` in its manifest), the
community DLSSG-Transfusion PTX kernels (MIT, derived from NVIDIA's) and Magewell's
`LibMWCapture.dll`; the public 2.1.9 package already shipped them.

## Limits

- Not run through Cloudflare (the play.example.com tunnel was not started) nor against a PS5 (it
  was in use). WinHTTP posts as "BooVI": a Cloudflare bot rule on the hostname could challenge it.
- No receive-side NACK: a lost packet costs a keyframe request (PLI) instead of a retransmit.
- Opus is placed on the picture's clock at the first packet; a long stall can leave them apart
  until the next join.
- If the host's encoder gains other feeds (generated pictures for browsers), a BooVI client
  should keep real pictures: `/session` already says `client: "boovi"` and `feed: "real"`.
