# Partage: the guest's latency, stage by stage, and what "negative latency" can mean (2026-11-20)

Goal: a friend plays the host's PS5 from a browser (`GUEST_SHARE_2026-11-20.md`). Cut what can be
cut between the console and the friend's eyes and hands, say what cannot, and try the speculative
ideas behind switches, each with a number.

Setup for every number measured here: RTX 5080, a 360 Hz screen, Edge 157 headless on the same PC
over loopback, a 1080p clip shared with `--share`. "Live" figures come from the friend's real
session on 2026-11-20 (through Cloudflare's TURN relay), medians of the last 60 page reports.
The PS5 was in use, so nothing here was run against it.

## What changed

| Change | Where |
|---|---|
| A live source hands each real picture to the guest's encoder right after the graph submitted it, not at its present | `EarlyCapture` in `PresentCapture.h`, `EngineController.cpp`, `EnhanceGraph::realFrameRead` |
| The frame cap is a two-picture bucket (live pictures arrive unevenly) | `GuestVideo.cpp` |
| "decoded → sent" on the host, "decoded → display" on the page, from the picture's decode on this PC | `guest-latency` log line, `f` message field `d`, page row `hôte → écran` |
| The page reports its screen's refresh rate, the time between pad reads, a real pad's sample age | `lab` block in `web/guest/index.html`, `guest-report` fields `hz`, `pollgap`, `padage`, `padstep` |
| `anticipation` toggle (off by default): camera anticipation on a WebGL stage | `lab` block, `?warp=1` |
| A simulated game and a bench for it | `?sim=1`, `?bench=1` |
| One command for a measured run | `scripts/fork/guest-latency.ps1` |

## The frame generation hold, removed for the guest

The guest's encoder read each real picture inside its present list. Frame generation shows the
interpolated pictures first and the real one last, so at 4X on a 30 fps game the real picture is
presented 32 ms after its decode (`FLUIDITE_PS5_CADENCE_2026-11-20.md`; live that night:
`callbackToPresentReturnP95Ms=33.6`, `schedulingWaitP95Ms=26.7`). The guest waited all of it and never
saw a generated frame.

Now the engine hands the picture over as soon as `EnhanceGraph::process` returns: one command list on
the graph's queue converts it to NV12, and the frame's presentation waits for that read. The host's
frames are untouched; the guest sees a real picture about 25 ms before the host's own screen does.

A/B on one build (`VEYRA_TEST_GUEST_TAP=present` keeps the old tap), a 30 fps clip replayed as a live
source, DLSS frame generation 2X (30 real, 60 shown), 20 s each, medians of 5 s windows:

| | tap at present | tap after the graph |
|---|---|---|
| picture decoded → RTP sent (host) | 24.9, 25.0, 30.4 ms | 6.6, 6.5, 6.3 ms |
| picture decoded → guest display (page) | 42.0 ms (35.3–48.3) | 11.2 ms (10.3–11.8) |
| tap → NVENC done | 2.0 ms | 4.8–4.9 ms |
| host callback → present P95 | 30.0 ms | 29.4 ms |
| host slot waits | 0 | 0 |

The tap-to-encoded time grows because the conversion now queues behind the picture's own GPU work
(super resolution, flow), which the present-time tap found already done. The guest's total is what
counts: 18 to 24 ms less at 2X. At the live session's 4X the hold is 32 ms, so about 28 ms is expected
there (not measured: the PS5 was busy). `decodedToSent` in the `guest-latency` line shows it directly.

## Budget, glass to glass for the friend

A 30 fps game, the host in fluidité 4X, the friend through the relay. "Before" is the live session.

| Stage | Before | After | Source |
|---|---|---|---|
| Pad → the browser's sample of it | not measured | same, now reported (`manette → page`) | — |
| Wait for the page's next read | 0–4 ms | same | 4 ms timer; reads 1.7 ms apart (p95 3.0) on a 360 Hz screen |
| Pad read → host | 3.1 ms | same | live |
| Host → handed to the PS5 session | 0.6 ms | same | live |
| chiaki → PS5, the game's own input to picture, PS5 encode, PS5 → host | not measured | same | a 30 fps game alone is commonly 70–130 ms |
| Host: packet → decoded picture | 2.0 ms | same | live, `readAgeMs` |
| Host: decoded → RTP sent | about 35 ms (32 hold + 2.6) | 6.3–6.6 ms | hold from the fluidité doc and the live P95; after measured on the replay rig at 2X |
| Network, one way | 2.75 ms | same | live |
| Received → displayed (jitter buffer 0.7, decode 0.3, the screen's refresh) | 14.7 ms (10.3–16.5) | same screen: same. 4.1–5.0 ms at 360 Hz | live; this PC |
| The screen itself | not measured | same | — |
| **Host decoded → friend's screen** | **about 52 ms** | **about 24 ms** | sums of the rows above |
| Camera: stick → picture starts to turn | the whole loop | one screen refresh, with `anticipation` | simulated game only, below |

Received → displayed is one refresh interval of the compositor plus the wait for the next one:
4.1–5.0 ms here at 360 Hz (2.8 ms a refresh). The friend's 10.2–16.1 ms fits a screen near 120 Hz
(8.3 + 0–8.3 ms, plus 1 ms of buffer and decode), not 60 Hz (which would read 18–34 ms). The page now
shows the refresh rate it measures (`écran`), so the next session settles it.

## Tried, with numbers

| Idea | Result | Verdict |
|---|---|---|
| Tap after the graph instead of at the present | 6.3–6.6 ms against 24.9–30.4 ms decoded → sent | adopted |
| Tap one step earlier, between the picture's list and the frame generation lists | 6.2 ms against 6.3–6.6 ms | no difference, not kept |
| Tap before super resolution (the decoded 1080p picture) | at most the 1.7 ms of SR plus 1.0 ms of flow would go; needs a second conversion (P010 PQ → NV12) on decoder textures | not built: 2.7 ms for a second path |
| NVENC P1 instead of P2 | encode 1.87 and 2.49 ms against 1.92–2.21 ms | no difference, P2 kept |
| NVENC slices sent as they complete | Chromium decodes whole frames; the whole encode is 2 ms | not built |
| Intra refresh instead of an IDR on request | changes recovery after a loss, not the steady state; the session had 0 % loss | not built |
| 60 pictures a second to the guest on a 30 fps game | the extra pictures are copies; each new one already leaves 6.5 ms after its decode | not built |
| RTP pacing | 47.5 ms against 19.1 ms (`GUEST_SHARE_2026-11-20.md`) | stays off |
| `<canvas>` from track frames (WebGL, `desynchronized`) instead of `<video>` | 7.8 ms (7.7–7.9) against 7.6 ms (7.1–7.9) host → screen; frame by frame the same refresh | no gain; kept as the stage for anticipation |
| WebCodecs `VideoDecoder` on insertable streams | 16.9 ms (14.5–17.5): the transform holds each frame 8.8 ms before any decoder; the decode itself is 0.5 ms against 0.3 ms | rejected |
| `jitterBufferTarget = 0`, `playoutDelayHint = 0` | already set; the buffer measures 0.5–0.8 ms | nothing left |
| A faster pad loop than rAF + 4 ms | the wait is 2 ms on average, less on a fast screen | not built; the browser's own sampling is the unknown, now reported |

The canvas figure is the page's estimate (the refresh after the draw, plus one), the `<video>`'s is
Chromium's own `expectedDisplayTime`. Neither is a photon measurement, and a headless browser has no
real screen: a camera on the friend's screen would be the way to compare the two for real.

## A BooVI client's session, read from the host's log (evening of 2026-11-20)

The friend played through the native client on an AMD RX 9070 XT. Three numbers looked worse
than the browser's. None was the path being slow.

| Number | Live | Cause | Change |
|---|---|---|---|
| `input recvToHandedPs5` p50 | 3.9–5.1 ms (browser 0.5–0.6) | The client repeats an unchanged pad every 8 ms; the PS5 thread hands an unchanged state only at its own 8 ms keepalive, so each repeat waited 0–8 ms. A change goes out on the next 1 ms pass for both clients | The host times each change from its arrival, not each message (`changes=` in the line); the acks the client turns into `inps5` follow |
| `encode` p50 | 6.3–7.5 ms (earlier 2.7–2.9) | The 2.7–2.9 ms sessions were `feed=120`, tapped at the present once the GPU work is done. `feed=real` taps right after the graph submits, so `encode` includes the picture's own GPU work: 5.6–8.2 ms all evening, before the 4X session too | None. Moving the tap ahead of the frame generation lists: 5.3–8.5 ms before, 5.6–9.7 ms after (5 s windows, 2X, the friend's session sharing the GPU), no gain, not kept |
| client `dec` | 4.35 ms (browser 0.1–0.3) | The same client on an RTX 5080: 0.19–0.22 ms with D3D12VA. 20× slower fits the software decoder (inferred: the client did not report its decoder) | The client logs, shows (stats card, detailed rows) and reports its decoder and GPU vendor (`decoder`, `gpu` in `guest-report`). When D3D12VA fails it tries D3D11VA, the browsers' path, before software: 0.34–0.44 ms with D3D12VA refused (`VEYRA_TEST_D3D12VA_REFUSE=1`) |

FSR 4 ML frame generation and FSR 4 upscaling (`FSR 3.1 / 4`) were already offered and the default
for a new profile on an RX 9000 (`defaultFrameGenerationBackend`, `effectGpu().rx9000`), and the
friend's package carries the FidelityFX runtimes.

## "Negative latency"

Stadia's term meant two things: a server that could run ahead of the player (predict the next inputs,
render those futures, send the one that came true), and a frame rate high enough to hide the
network. Both need the game itself to cooperate. Here the game runs on a PS5 that sends one finished
video stream: nothing upstream of the picture can be predicted, rolled back or rendered twice. The
loop pad → PS5 → game → encode → host → friend cannot go below the game's own input-to-picture time
plus the encoders and the wire. No technology on the host or in the page changes that.

What can change is when the friend *sees a response*. The one input whose effect on the picture is
simple enough to guess is the camera: turning shifts the whole picture. VR headsets (asynchronous
timewarp) and NVIDIA Reflex 2 (Frame Warp) do exactly this: warp the last picture by the newest
input, then let the next real picture correct it.

### The prototype (`anticipation`, off by default)

- The stream is drawn on a WebGL canvas. Every screen refresh the last picture is redrawn shifted by
  the camera motion the stick has asked for and the stream has not shown yet: the stick's history
  from (picture time − loop delay) to now, through the learnt speed curve.
- Learnt while playing, per game: each new picture's global shift (column and row brightness profiles
  of a 160×90 copy, lined up against the previous picture's) is correlated with the stick's history
  at 20 candidate delays (0–380 ms). The best delay is the loop delay; the speed per deflection is a
  curve of five bands per axis. An axis learns only while its stick moved.
- Past the picture's edge the border is stretched, blurred and dimmed. The shift is capped at 10 % of
  the picture, and scaled by how much of the measured motion the stick explains: a menu, a cutscene
  or any picture that does not follow the stick gets no shift (on a plain video with the test sweep:
  confidence 0.6 %, nothing moved).

Measured on the simulated game (`?sim=1&bench=1`: a panorama, the camera follows the right stick 100 ms
late through a dead zone and a power curve, 30 pictures a second, a still HUD), 16 stick steps each
way, two runs:

| | without | with |
|---|---|---|
| stick step → picture motion on screen | 126.6 and 127.6 ms | 5.2 and 5.5 ms |
| camera behind the stick during a steady turn | 101.5 ms (78 px of 1280) | −1.5 and −0.5 ms (1.2 and 0.5 px) |
| learnt loop delay | — | 100.4 and 100.5 ms |
| cost per picture (readback and fit, main thread) | — | 2.0–2.1 ms |

"On screen" is one refresh after the draw, 2.8 ms on this PC's 360 Hz; on a 60 Hz screen it is up to
33 ms instead of 5. The simulated camera is the easy case. On a real game expect:

- only the camera is anticipated: a jump, a shot, a menu still take the whole loop;
- the HUD, the weapon and the character move with the shift, then snap back: no game data says which
  pixels belong to the world (Frame Warp has it, from inside the engine);
- a real camera turn is a rotation, a shift is its first-order approximation: correct at the centre,
  off toward the edges and near objects (parallax);
- games add acceleration, smoothing and aim assist to the stick: the curve fits the average, the rest
  shows as the picture drifting ahead and settling back;
- the picture's border shows a stretched band while turning.

It has not been tried on a real game. It is a toggle for the friend to judge by feel.

### The same idea for the host's own mouse (a note, nothing built)

The host could do it better than the page. The mouse gives exact motion at 1000 Hz, the KBM mapper
already knows the stick deflection it sends, the frame generation pass already computes motion
vectors for the picture, and the present shader could apply the shift at no extra pass. The hold is
also where it would pay most: in fluide 4X the real picture reaches the host's screen 32 ms after its
decode. The open questions are the same (HUD, parallax, the game's curve) plus one: generated frames
would need the shift too.

## What the friend and the host can do

- Friend: a faster screen is the largest lever left on their side. Received → displayed is 1 to 2
  refresh intervals: about 10–18 ms at 120 Hz, 5–10 ms at 240 Hz, 4–5 ms measured at 360 Hz.
- Friend: Chrome or Edge, the page full screen (`f`), hardware acceleration on, nothing heavy on the
  GPU. A wired pad: the page will show how old a pad sample is when it sees it (`manette → page`).
- Friend and host: wired network. The relay adds little here (5 ms round trip); a direct path
  (`chemin: direct`) saves at most that.
- Host: nothing to set. The guest no longer pays for the host's frame generation, so fluide or
  réactif is the host's own choice again.
- Host, to try with the PS5 free: the Remote Play bitrate. A new picture of a 30 fps game arrives
  about 7 ms later than its copy because it is larger (`FLUIDITE_PS5_CADENCE_2026-11-20.md`); 10 Mb/s
  instead of 15 may bring it in sooner. Not measured.

## Limits

- The early tap was measured on a file replayed as a live source, not on the PS5 stream. The PS5's
  half mode (copies dropped before the graph) runs the same code after `process`.
- The guest's picture is no longer in step with the host's screen: the guest is ahead by the host's
  hold. The guest's sound path is unchanged, so the picture also moved that much earlier against it.
- A capture card or a screen capture shared with a guest takes the early tap too (any live source).
- `anticipation` adds about 2 ms of main-thread work per picture and draws through a canvas; with it
  off the page is the plain `<video>` as before.

## Reproduce

```
scripts\fork\build.ps1 -Base <Veyra 2.1.9 portable> -Out <app>
# in <app>'s preferences: "guestReach": "tailnet" (loopback; the named tunnel is not started)
# the early tap against the old one, a 30 fps clip as a live source
scripts\fork\guest-latency.ps1 -App <app> -Video clip30.mp4 -Live -Env VEYRA_TEST_GUEST_TAP=present
scripts\fork\guest-latency.ps1 -App <app> -Video clip30.mp4 -Live
# the display paths
scripts\fork\guest-latency.ps1 -App <app> -Video clip60.mp4 -Query render=canvas
scripts\fork\guest-latency.ps1 -App <app> -Video clip60.mp4 -Query render=webcodecs
# the anticipation bench: serve web\guest over http, no host needed
scripts\fork\guest-probe.ps1 -Url 'http://127.0.0.1:8765/index.html?sim=1&bench=1' -Seconds 64 -Quiet -Eval 'JSON.stringify(lab.bench.result)'
```

The simulated game also runs by hand with a real pad: `<link>/?sim=1&warp=1` (no token needed).
Logs of the runs named here: `%USERPROFILE%\dev\guestlat-test`.
