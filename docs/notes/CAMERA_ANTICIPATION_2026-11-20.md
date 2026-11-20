# Partage: camera anticipation in the native client (2026-11-20)

Goal: a friend who joins a share from their own BooVI turns the camera and sees it turn at once,
at their screen's refresh rate, instead of one whole loop later and at the game's 30 pictures a
second. The guest page's prototype (`GUEST_LATENCY_2026-11-20.md`, `?warp=1`) is now a present
mode of the native client.

## How it works

```
right stick (input thread, as it leaves for the host) ──▶ StickHistory
each presented picture ─▶ copy to the stage ─▶ 160x90 luma ─▶ global motion against the picture before
                                   │                              │
                                   │                 CameraModel: loop delay, speed per deflection, confidence
                                   ▼                              ▼
every screen refresh:  draw the stage's picture displaced by what the stick has asked for
                       between (picture's host time − learnt delay) and the refresh that shows the draw
```

- The model (`include/veyra/engine/CameraAnticipation.h`, no GPU in it) is the page's: per new
  picture its global shift (column and row profiles of the luma copy) is correlated with the
  stick's history at 20 delays (0 to 380 ms); the best one is the loop delay, the speed per
  deflection is a curve of five bands per axis. Confidence is the share of the measured motion the
  stick explains: under 25 % nothing is shifted, from 55 % all of it. An axis learns only while its
  stick moved, and only from measurements whose profiles line up (score 0.5 or more).
- New here: the shift has two limits, 8 % of the picture for the loop's delay and 6 % for what
  the stick adds while one picture stays on screen, so a fast turn whose delay part is at its
  limit still moves between two pictures. When a new picture replaces the one being shifted, the
  difference between the motion the model expected and the picture's own goes to a residual
  (3 % at most) that fades over 80 ms: the screen sees no step. Content that moves with the
  stick at rest is never held back.
- The picture's time is the host's own for it (the stats channel's `f` message through the clock
  sync), on this PC's clock. The stick's time is the moment the pad state leaves for the host.
- GPU (`AnticipationStage`, `VideoPresenterAnticipation.cpp`): plain D3D12 on the presentation
  queue. Per picture one copy to a stage of two textures, one 20x12 compute dispatch
  (`AnticipationLuma.hlsl`) and a 69 KB readback, waited for at most 2 ms. Per refresh one
  `PresentBlit` draw with a displacement (flag 512): past the picture's edge the border is
  stretched, blurred along the edge and dimmed. No vendor API.
- The redraws ("ticks") run on the engine's owner thread, between its other work. Their grid
  keeps one phase against the screen's refreshes (DXGI frame statistics), half a period before
  the last moment a Present still makes a refresh, and each shift aims at the refresh its draw
  reaches. The thread sleeps to 0.6 ms before a tick, then spins.

## The choice

réglages > image > "anticipation caméra · partage rejoint"
(`PresentationSettings::cameraAnticipation`, saved with the presentation preferences):

| | pictures | what it costs |
|---|---|---|
| automatique (default) | anticipation alone on a joined share shown at 100 Hz or more, else as before | — |
| désactivée | as before | — |
| anticipation seule | no frame generation: a picture is shown as soon as it is decoded, and redrawn every refresh | objects, characters and the HUD move at the game's own rate |
| anticipation + interpolation | frame generation as chosen, each real and generated picture redrawn every refresh | the real picture waits for the next one again (half a picture to one picture) |

Automatic does not look at the game's rate: the stream's rate is not known when the session
starts, and switching frame generation on and off rebuilds the graph. A 60 fps game in automatic
therefore loses its interpolation; "anticipation + interpolation" or "désactivée" keeps it.

Stats card of a joined share: `anticipation : active · confiance 0,99 · gain appris` (also
`apprend`, `prête` with the stick at rest, `inactive`); "affiché" counts the draws of every refresh.

## Measured

Setup: RTX 5080, two instances on this PC over loopback (`scripts` below). The host shares a
1080p clip at 30 pictures a second, preset performance, port 47817, reach tailnet. The client
joins in fluidité (super resolution to 1440p), a 1280x720 window on the 360 Hz screen, composed
by the desktop. The friend's live session ran on the same GPU during every run and could not be
stopped: "without" and "with" alternate inside each bench run.

The game is simulated on the client (`VEYRA_TEST_ANTICIPATION_SIM=100`): each received picture is
drawn as a panorama whose camera follows the right stick 100 ms late through a dead zone and a
power curve. The measure, the model and the draw then run on real network pictures. The bench
(`VEYRA_TEST_ANTICIPATION_BENCH=1`) scripts the stick: a sweep of 20 holds, then four blocks
(without, with, without, with) of 8 steps from rest to 0.8.

### The stick to the screen

One run each, 16 steps a side. "On screen" is the refresh DXGI's statistics give for that
Present, not a photon measurement; the camera shown is what the draw was told to shift by, not
read back from pixels.

| | without | anticipation alone | anticipation + interpolation |
|---|---|---|---|
| stick step → the Present that moves the picture, median (p90) | 131.4 ms (141.1) | 2.3 ms (3.1) | 1.9 ms (2.7) |
| stick step → on screen | 137.4 ms | 9.0 ms | 8.2 ms |
| camera behind the stick in a held turn | 129.1 ms, 7.77 % of the picture's width | 1.0 ms, 0.06 % | 3.2 ms ahead, 0.20 % |
| learnt delay, confidence | — | 100.2 ms, 0.99 | 100.1 ms, 0.98 |

Without: the game's 100 ms, 6.5 ms from the host to this screen, half a picture interval on
average, 6 ms from Present to scan-out. With: the wait for the next tick (0 to 2.78 ms) and
Present to scan-out, which is the limit left: about two and a half refreshes in a composed
window. Other runs of the same bench: on screen 7.7 and 9.7 ms with, 135.4 and 139.2 ms without;
on the 120 Hz screen (ticks not yet locked that run) 11.1 ms against 137.1 ms.
"Without" in the interpolation column is 144.5 ms: frame generation's own hold.

### Pacing at the screen's refresh

Per-second log lines (`[anticipation]`), medians over 50 to 60 s, all Present calls together.

| | frame generation 2X, no anticipation | anticipation alone | anticipation + interpolation |
|---|---|---|---|
| Present calls a second | 60.0 | 359.2 (lowest second 344.2) | 363.1 (354.7) |
| interval p50 / p95 / p99 | 16.66 / 17.12 / 17.45 ms | 2.78 / 3.43 / 3.83 ms (worst second's p99 6.43) | 2.77 / 3.62 / 4.25 ms |
| picture received → displayed | 22.1 ms | 4.06 ms | 23.9 ms |
| picture decoded on the host → displayed here | 24.4 ms | 6.48 ms | 27.2 ms |

Locking the ticks to the screen: 136 of 13 840 presents were replaced before being shown (1.0 %),
against 783 of 16 528 (4.7 %) with a free-running grid.

### Cost

| | value |
|---|---|
| GPU per refresh (the tick's blit, queue timestamps) | 0.007 ms |
| GPU per picture (copy and luma) | 0.031 ms |
| owner thread per tick | 0.20 to 0.46 ms, the Present call for most of it |
| owner thread per picture, waiting for the measure | 0.17 ms median; 0 missed of 1 800 in the reported run |
| client process, 20 s of streaming | 0.42 cores median (0.35 to 0.68, 7 runs) against 0.55 with frame generation (2 runs) |
| memory | two pictures of the working size (29 MB at 1440p) |

One run was disturbed for 35 s: 236 to 300 Present calls a second, p99 up to 27 ms, measures
timing out at 2 ms (15 to 21 pictures a second shown unmeasured), Present to scan-out 16 ms.
Two reruns, locked and free, were clean. The other session on the GPU is the likely cause; it
was not shown.

## What a real game will show

Not run on a real game nor on the PS5 (in use), and not on AMD hardware. From the mechanism:

- only the camera is anticipated. A jump, a shot, a menu, a character's own motion take the whole
  loop as before;
- the HUD, the weapon and the character are part of the picture: they move with the shift, then
  come back when the stream catches up;
- a turn is a rotation and the shift is a translation: right at the centre, off toward the edges
  and on near objects;
- games add acceleration, smoothing and aim assist to the stick. The curve fits the average; the
  rest shows as the picture drifting a little ahead and settling. The residual hides the step, not
  the drift;
- while turning, the side the camera turns toward shows a stretched, blurred, dimmed band, up to
  14 % of the picture in a fast turn;
- the first seconds of camera movement teach the model. Until then, and in any scene that does
  not follow the stick (menu, cutscene, a locked camera), nothing is shifted;
- alone, the picture between two shifts is the same real picture: objects move 30 times a second.
  Every input also arrives about 18 ms sooner than with frame generation (22.1 against 4.06 ms
  here), camera or not.

## Limits

- The model lives for one session; nothing is saved per game.
- The right stick only. A mouse reaches it through the keyboard and mouse mapping; gyro aim is not modelled.
- One delay for both axes, on a 20 ms grid up to 380 ms.
- Alone, réglages still shows the chosen frame generation; the joined share runs without it.
- Not applied with XeSS frame generation (it owns Present) nor with the HDR copy path.
- Variable refresh was not tested; the ticks follow the screen's reported rate.
- The ticks share the owner thread: a long graph submission delays one.
- Seen during the runs, in the base as well: the host's file playback sometimes ends by itself
  (11 of 27 runs; 1 of 4 more with a host built before this work), and `capture-audio-clock` warns
  about 60 times a second with frame generation on a 30 fps share (3 400 lines in the run without
  anticipation).

## Reproduce

```
scripts\fork\build.ps1 -Base <Veyra 2.1.9 portable> -Out <client app>
scripts\fork\stage-app.ps1 -Base <...> -Out <host app> -Build out\build\fork
# both copies: "volume": 0, "muted": true, "introMusic": false, "gatewayEnabled": false
VEYRA_GUEST_PORT=47817 VEYRA_GUEST_REACH=tailnet VEYRA_GUEST_LINK_FILE=<f>  <host>\veyra_qml_ui.exe --share --preset performance clip30.mp4
VEYRA_TEST_GUEST_CLIENT_SYNTH=1 VEYRA_TEST_ANTICIPATION_SIM=100 VEYRA_TEST_ANTICIPATION_BENCH=1  <client>\veyra_qml_ui.exe --join-file <f> --preset fluid
# the client's log: [anticipation] one line a second, [anticipation-bench] the result after about 60 s
# VEYRA_TEST_ANTICIPATION_TRACE=1: one line per picture (measured and expected motion, residual)
# VEYRA_TEST_ANTICIPATION_FREE_RUN=1: ticks not locked to the screen's refreshes
scripts\fork\run-tests.cmd        # CameraAnticipationTests: the model on a simulated game
```

The runner, logs and shots of every run named here: `%USERPROFILE%\dev\reproj-test`
(`run.ps1 -Sim 100 -Bench -Primary`, `-Anticipation 0..3`).
