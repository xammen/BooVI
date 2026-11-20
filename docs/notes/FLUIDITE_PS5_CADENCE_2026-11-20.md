# Fluidité on the PS5 stream: real pictures only, a learned phase, event wakes (2026-11-20)

Goal: in fluidité mode (super resolution plus frame generation) the PS5 stream should show 60 or
120 pictures a second made from the game's real pictures, with as little added delay as possible.

Setup for every number here: RTX 5080, PS5 Remote Play 1080p60 HEVC HDR, D3D12VA decode, RTX Video SR
to 1440p, DLSS frame generation, 360 Hz display, presentation "automatic". Latency is CPU-observed:
decode callback of a picture to the return of its Present. Scan-out is DXGI frame statistics. The
network and the console's encoder are before the first point this PC can time.

## What was wrong

PS5 Remote Play sends 60 pictures a second whatever the game renders. A 30 fps game arrives twice per
picture. Frame generation at 2X then showed, per 33 ms: a copy, an interpolation of two equal pictures,
one blend, one new picture. Half of the 120 pictures a second carried no new motion.

Content dedup existed but was blind here: it compares on the CPU, a D3D12VA picture never gets there
(`[content-dedup] blind=60 observed=60`). Fed software-decoded pictures it was worse than nothing on a
drifting menu: 10 to 20 irregular pictures a second, `callbackToPresentReturnP95Ms=354`.

## What changed

| Change | Where |
|---|---|
| GPU count of luma samples that differ between two consecutive decoded pictures | `StreamPictureDiff`, `shaders/StreamPictureDiff.hlsl` |
| Cadence detector: drops second copies only while changed / unchanged alternates | `StreamCadence.h`, 27 unit checks, two measured sequences among them |
| Multiplier doubles at half rate (DLSS takes its frame count per evaluation) | `EnhanceGraph::fgPreviewCeiling`, `EngineController.cpp` |
| Choice for 30 fps games: fluide (2X becomes 4X) or réactif (multiplier kept) | `PresentationSettings::halfRateKeepsMultiplier`, réglages > image |
| The stream learns its pair phase like capture cards do | `adaptiveStreamPhase` |
| A video packet and a decoded picture wake their readers (were 1 ms looks) | `SessionInbox::setVideoWake`, `RemotePlaySessionSource::frameEvent` |
| Stats card: images réelles, délai total, délai gpu | `StreamOverlay.qml`, bridge `contentFps` / `liveDelayMs` / `scanoutDelayMs` |

The stream really duplicates, but not bit for bit: the console's encoder keeps refining a copy. Two
scenes of one 30 fps game, luma samples changed against the picture before (of 2 073 600):

| | copies | new pictures |
|---|---|---|
| quiet scene | 0 to 171, mostly under 20 | 3 800 to 120 000 |
| busy scene | 800 to 2 300, now and then 5 600 | 17 000 to 1 000 000 |

Packet size does not separate them (quiet scene: copies 2 to 5 KB, new pictures 5 to 10 KB), and chiaki's
per-picture data (frame index, frames lost, reference recovered) has no repeat flag.

## Before and after

"Before" is commit 3f19a31, or the new build with `VEYRA_TEST_STREAM_CADENCE=off`,
`VEYRA_TEST_LEGACY_STREAM_PHASE=1` and `VEYRA_TEST_STREAM_NO_WAKE=1`, which measured the same
(20.11 against 20.12, 20.07, 19.92 ms). Each cell is one 40 to 60 s session; a range is several sessions.

### 60 fps content (a menu), 2X

| | before | after |
|---|---|---|
| content rate detected | none, 60 assumed | full mode, every picture kept |
| presented a second | 119.9 | 119.9 |
| present interval p50 / p95 / p99 | 8.33 / 9.21-9.29 / 9.80-9.88 ms | 8.33 / 9.10-9.12 / 9.35-9.38 ms |
| real picture, callback to Present, p50 (p95) | 19.92-20.12 (20.8-22.1) ms | 14.31-14.40 (14.9-15.2) ms |
| first picture of a pair on screen | 11.25-11.46 ms | 5.59-5.62 ms |
| Present to scan-out p50 | 6.98-7.34 ms | 6.64-6.66 ms |
| GPU per source picture | 4.0-4.4 ms | 3.8-4.2 ms |

Where the 5.8 ms went: learned phase 3.6, event wakes 1.6, no wait for the picture count 0.6.

### 30 fps game, same scene, sessions back to back (real30-old-2, real30-new-2)

| | before | after, fluide (4X) |
|---|---|---|
| content rate detected | none | 29.9, half mode 54 of 56 s |
| presented a second | 119.8 | 119.6 |
| pictures shown without new motion | 2 of every 4 | 17 kept stills in 54 s |
| present interval p50 / p95 / p99 | 8.33 / 14.70 / 15.84 ms | 8.34 / 8.85 / 9.70 ms |
| intervals over 12.5 ms | 16.06 % | 0.11 % |
| real picture, callback to Present, p50 (p95) | 20.11 (20.76) ms | 32.24 (36.80) ms |
| first picture of a group on screen | 11.46 ms | 6.76 ms |
| GPU | 4.42 ms x 60 = 265 ms/s | 6.07 ms x 30 = 182 ms/s |

The uneven intervals before come from the network: a new picture is large and takes about 7 ms longer
to arrive than its copy, so arrivals alternated 9.6 and 23.6 ms and every pair was anchored on its own.
Kept first copies arrive 32.2 to 34.7 ms apart.

The real picture waits 12 ms longer in fluide: four even steps across 33 ms put it three quarters of
the interval after the first one. réactif keeps 2X, measured with every second picture dropped
(`VEYRA_TEST_STREAM_CADENCE=half`):

| | fluide 4X | réactif 2X |
|---|---|---|
| presented a second | 119.9 | 59.8 |
| present interval p50 / p99 | 8.34 / 9.16 ms | 16.67 / 18.16 ms |
| real picture, callback to Present | 31.80 ms | 23.96 ms |
| GPU | 6.0 ms x 30 | 4.41 ms x 30 = 132 ms/s |

## Tried and not adopted

| Idea | Result | Verdict |
|---|---|---|
| Turn the existing auto dedup on for streams | drifting menu: 10-20 irregular fps, P95 delay 354 ms | rejected, replaced by the alternation detector |
| Packet size as the repeat signal | overlaps in a quiet scene | rejected |
| Tearing instead of automatic | 14.25 against 14.31-14.40 ms, inside the run-to-run spread | no difference |
| Low queue off | 14.42 ms, scan-out 6.83 | no difference |
| NVIDIA low latency off | 14.44 ms, scan-out 6.77 | no difference |
| Explicit VSync | 14.67 ms and scan-out 7.47 against 6.65 | worse by 1.2 ms |
| DLSS FG with zero motion instead of optical flow | first picture 5.74 against 6.14 ms, GPU 5.06 against 5.42 ms, one run each | not default: 0.4 ms, picture quality not judged |
| Keep a first copy without waiting for its count | saves the compare wait (0.7-1.0 ms at P95) | rejected: a late game frame would be shown as two short pairs |

Not measured: VRR (G-SYNC), which needs an exclusive session on the user's screen. Scan-out read
6.6 ms windowed and 6.6 ms in the 12 s the window was fullscreen; older logs read 6.7 ms at 120 Hz and
7.1 ms at 360 Hz, so the refresh rate did not move it either.

## Limits

- Half mode needs the PS5 source, D3D12VA decode and frame generation on. Software decode, PC and
  Xbox streams keep every picture. The multiplier doubles only with DLSS; other backends behave as réactif.
- Only half rate is recognised. A 20 fps or 40 fps game keeps every picture, as before.
- Entering half mode is one 25 ms gap, leaving it one catch-up step. A session had 2 to 6 such gaps.
- The new réglages and stats strings are French source text; `i18n/catalog.json` is unchanged.

## Reproduce

`scripts/fork/session-test.ps1` with `-Env VEYRA_VERBOSE_FRAME_LOGS=1`, then
`py scripts/fork/fluid-stats.py <log>`. Logs and screenshots of every run named here are in
`%USERPROFILE%\dev\veyra-fork-app-fluid\shots`.
