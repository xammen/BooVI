# Frame generation anti-artefacts: crosshair ghosting and lines in fast motion (2026-11-20)

Goal: remove two artefacts players reported on the PS5 stream, "crosshair ghosting while aiming"
and "lines when I look around really fast", without adding latency or losing smoothness, and
prove it with an objective benchmark.

## Benchmark

`scripts/fork/fgbench`: a synthetic clip with a known 120 fps truth.

- `fgclip.py`: a two-layer panorama (far scene, near layer with 1.7x parallax: trunks, a slatted
  fence), thin poles and wires, text. The camera is still, then pans slow (6 px per 30 fps frame),
  fast (40 px), flicks (180 px for 0.12 s, then near still), strafes while aiming, pans very fast
  (80 px). A static HUD sits on top: a crosshair that fades in and out while aiming, a thin
  outlined reticle, corner text, a timer, an opaque minimap.
- `make_clip.py`: every fourth truth picture as 1080p30 HEVC (crf 12), the stream's format.
- `run-bench.ps1`: plays it as a live source (`VEYRA_TEST_CAPTURE_REPLAY`, `VEYRA_TEST_REPLAY_AS_CARD`),
  frame generation on the source picture (`--preset fgonly`), and dumps the frames handed to
  presentation (`VEYRA_TEST_FG_DUMP`, every second pair). The copied preferences must be silent.
- `measure.py`: PSNR and SSIM per region (full frame, a 96x96 crosshair box, the HUD boxes), and
  "lines", the luma gradient energy the output has and the truth does not, perpendicular to the
  pan (streaks drawn along the motion), over the scene outside the HUD. "Best-matching truth" is
  the same PSNR against the truth picture within 4/120 s that the frame matches best: a real frame
  shown at the wrong time counts as clean, and "off" says how far it is. It also reads the
  dumped FG motion field against the true motion.
- `timing.py`: latency and GPU time from the run's log. `crops.py`: truth | before | after crops.

Same build both ways: the settings off give the old pipeline bit for bit (identical metrics).
Runs are deterministic: two runs of the same build gave the same numbers to 0.01 dB.

## Causes, with evidence

**Crosshair ghosting.** The generator moves screen-static overlays with the scene behind them.
Even in the slow pan the crosshair box drops to 21.95 dB (real frames: 32 dB) and the crops show
the crosshair and reticle doubled. Our optical flow gives the HUD the background's motion (mean
2.9 px on the HUD in the slow pan, 21.8 px fast, 36.8 px very fast). But DLSS-G does not use our
vectors here: with zero motion vectors instead of our flow, every metric came out the same to
0.01 dB. Correcting the vectors alone would therefore change nothing for DLSS.

**Lines when turning fast.** The generator's own motion estimate breaks past about 40 px a frame
and with parallax: thin poles come out wavy, trunks smeared, text doubled (`crops-dlss/veryfast-scene`).
"Lines" grows with speed: 0.50 slow, 0.99 fast, 1.27 very fast, 2.0 during flicks, against 0.2
for real frames. Our own flow is right for the scene as a whole (global motion within 0.3 px of
the truth in every pan) but wrong locally (end-point error 6.5 px far / 12.9 px near layer at
40 px, 24.6 px during flicks).

## What changed

`pipeline/FgGuard.{h,cpp}`, four compute passes at the work extent, around every generator
(DLSS, FSR 3.1, VFG, AMF):

| Pass | When | What |
|---|---|---|
| `FgHudDetect` | each real frame | a pixel whose 3x3 patch is the same in A and B while the scene's motion there (its flow vector, or the fastest within 24 px) does not explain it is screen-static. A score kept across pairs rises 0.34 per such pair; a clear change lowers it. The centre fifth (crosshairs) takes a looser test and lets go at 0.2 a pair instead of 0.5, so a crosshair fading while aiming stays masked. |
| `FgHudInpaint` | each real frame | the generator's input without the overlays: masked pixels (and unchanged pixels 2 px around them) take the scene from 16 directions. |
| `FgStats` | each real frame | one thread group: the pair's global motion (the most common flow vector), the share near it, the share it explains photometrically, the share that changed. Decides: global blend (motion at least 4 px at 1080p, share at least 0.25, explains at least 0.3), or hold (more than half the picture changed, share under 0.15, global motion explains under 0.4). |
| `FgFinalize` | each generated frame, in place | hold: the nearest real frame. Global blend: where A and B agree once moved by the global motion (and agree worse unmoved), their motion-compensated blend replaces the generated pixel. Then the overlay pixels from A and B, blended at t. |

The decision and the GPU time go back to the CPU later through a readback ring (no wait), for
the log: `[fg-guard] pairs= holds= globalBlend= gpuMsPerRealFrame=`.

Settings: `EnhancementSettings::fgHudGuard` ("protéger le viseur et le hud") and `fgFastGuard`
("mouvements rapides : privilégier la netteté"), on by default, in réglages → image →
anti-artefacts while frame generation is on, kept in the preferences. Export keeps the frames as
generated.

Test switches added: `--fg-backend`, `--fg-motion`, `--preset fgonly`, `VEYRA_TEST_FG_DUMP`
(`_EVERY`, `_PERIOD`), `VEYRA_TEST_FG_GUARD_VIEW` (paints the overlay mask magenta).

## Results

RTX 5080, 1080p work extent, no SR. Generated frames only; real frames sit at 30.7 dB / SSIM 0.993
(the floor set by the stream's coding). "between" is the near-still time between flicks.

### DLSS FG x4, before → after

| segment | frames | PSNR full | PSNR crosshair | PSNR HUD | SSIM full | SSIM crosshair | SSIM HUD | lines | PSNR vs best-matching truth | off (1/120 s) |
|---|---|---|---|---|---|---|---|---|---|---|
| slow | 114 | 25.34 → 26.70 | 21.95 → 23.54 | 26.58 → 26.86 | 0.883 → 0.900 | 0.869 → 0.922 | 0.950 → 0.955 | 0.50 → 0.39 | 25.34 → 26.70 | 0.00 → 0.00 |
| fast | 111 | 19.42 → 20.52 | 17.42 → 22.40 | 23.91 → 25.37 | 0.722 → 0.802 | 0.650 → 0.862 | 0.884 → 0.908 | 0.99 → 0.78 | 19.42 → 20.52 | 0.00 → 0.00 |
| flick | 28 | 14.25 → 13.27 | 14.18 → 13.63 | 17.77 → 16.71 | 0.427 → 0.356 | 0.511 → 0.522 | 0.744 → 0.721 | 2.03 → 3.22 | 16.59 → 26.01 | 1.43 → 1.39 |
| between | 86 | 28.15 → 28.69 | 27.83 → 29.14 | 30.13 → 30.37 | 0.950 → 0.955 | 0.963 → 0.974 | 0.975 → 0.979 | 0.27 → 0.25 | 28.18 → 28.81 | 0.03 → 0.03 |
| aim | 111 | 20.54 → 20.82 | 17.67 → 22.78 | 27.42 → 28.04 | 0.811 → 0.827 | 0.778 → 0.920 | 0.952 → 0.957 | 0.71 → 0.67 | 20.48 → 20.72 | 0.21 → 0.16 |
| veryfast | 66 | 17.97 → 20.15 | 15.17 → 18.78 | 21.38 → 22.94 | 0.628 → 0.779 | 0.550 → 0.769 | 0.843 → 0.890 | 1.27 → 0.80 | 18.01 → 20.15 | 0.06 → 0.00 |

### FSR 3.1 x2, before → after

| segment | frames | PSNR full | PSNR crosshair | PSNR HUD | SSIM full | SSIM crosshair | SSIM HUD | lines | PSNR vs best-matching truth | off (1/120 s) |
|---|---|---|---|---|---|---|---|---|---|---|
| slow | 38 | 20.35 → 23.40 | 25.49 → 26.38 | 29.73 → 29.37 | 0.824 → 0.850 | 0.932 → 0.936 | 0.975 → 0.967 | 0.87 → 0.65 | 20.35 → 23.40 | 0.00 → 0.00 |
| fast | 37 | 20.88 → 21.21 | 17.81 → 23.08 | 23.02 → 25.67 | 0.838 → 0.838 | 0.694 → 0.886 | 0.910 → 0.933 | 0.89 → 0.86 | 20.88 → 21.21 | 0.00 → 0.00 |
| flick | 10 | 15.31 → 13.43 | 14.92 → 13.58 | 17.13 → 16.43 | 0.497 → 0.348 | 0.506 → 0.503 | 0.709 → 0.709 | 2.25 → 3.21 | 15.53 → 24.24 | 0.60 → 1.90 |
| between | 28 | 29.47 → 29.48 | 29.55 → 29.94 | 33.02 → 32.82 | 0.979 → 0.979 | 0.985 → 0.986 | 0.992 → 0.992 | 0.26 → 0.26 | 29.47 → 29.48 | 0.00 → 0.00 |
| aim | 37 | 20.85 → 21.14 | 19.50 → 22.78 | 26.42 → 28.08 | 0.853 → 0.858 | 0.861 → 0.912 | 0.958 → 0.934 | 0.82 → 0.79 | 20.85 → 21.14 | 0.00 → 0.00 |
| veryfast | 22 | 21.69 → 21.86 | 16.25 → 19.84 | 21.34 → 23.02 | 0.860 → 0.866 | 0.631 → 0.822 | 0.865 → 0.902 | 0.76 → 0.75 | 21.69 → 21.86 | 0.00 → 0.00 |

During flicks the hold shows real frames: clean (PSNR against the best-matching truth 16.6 → 26.0
dB with DLSS), but up to 1.4/120 s early or late, so against the true time PSNR and "lines" get
worse. That is the trade the setting names: sharpness over motion during a flick. 19 of 240
pairs held, all inside flicks; none elsewhere in the clip.

### Cost (DLSS x4, no dump, steady 1 s windows, median of the windows' P95)

| | off | on |
|---|---|---|
| callback → Present return P95, ms | 32.74 / 32.79 / 32.84 / 32.85 / 33.63 / 35.69 (6 runs) | 32.68 / 32.89 (final build); 32.86 to 34.05 (5 runs of the build before the last threshold change) |
| GPU ready P95, ms | 1.69 to 2.24 | 2.20 to 2.60 |
| guard GPU time per real frame (timestamps), ms | - | 0.50 to 0.54 (DLSS x4: analysis + 3 finalizes); 0.37 to 0.49 with FSR x2 |
| generated frames expired | 0 | 0 |

The replay scheduler's latency is not a console's (the file plays like a capture card), but both
builds run the same path. The difference is inside the run-to-run spread.

Crops (truth | before | after), one per segment and region: `%USERPROFILE%\dev\fgart-bench\crops-dlss`
and `crops-fsr`. Per-frame metrics: `metrics.json` in each run folder there.

## Tried and not kept

| Idea | Result |
|---|---|
| Zero the FG motion on the HUD / better vectors | no effect on DLSS-G: zero vectors and our flow give the same frames |
| Hold whenever the per-pixel flow explains little | never fired: on smooth scenery a wrong vector still matches |
| Overlay test "static while the surroundings change" (no flow) | masked smooth moving scenery; the composite doubled it (HUD SSIM 0.95 → 0.82) |
| Inpaint a 5x5 band but restore only the 3x3 mask | holes in glyphs; inpaint and composite now cover the same pixels |
| Global blend from 2 px of motion | softened FSR's near-still frames (SSIM 0.979 → 0.944); now from 4 px |
| Nearest real frame instead of DLSS in every fast pan (offline) | worse than DLSS on every metric; global-motion blend better |

## Limits

- The stream's coding: real frames already sit at 30.7 dB against the truth; a PS5 stream at a
  lower bitrate adds more noise, which the overlay test (patch difference under 0.03 + 0.25 of
  the moved difference) and the hold's 0.08 colour step must clear. Not measured on a real stream.
- Overlays that move or are translucent over a moving scene (a minimap that rotates, a
  semi-transparent HUD, damage numbers, hit markers) change between real frames: they are not
  masked and the generator still moves them. Large opaque static panels are masked at their
  edges only; their inside is left to the generator, which keeps it in place (the blend refuses
  pixels that agree better unmoved).
- The guard needs the optical flow: with VFG (its own motion) or DLSS with "motion: zero", the
  flow is not computed and the guard does nothing (VFG run: 0 global blends, 0 holds).
- The global blend fits camera turns; parallax and moving objects keep the generator's frame.
- AMF FRC, FSR 4 ML and an RX 9000 were not run. DLSS-G's own UI inputs (HUD-less colour,
  UI colour and alpha) were not tried: the guard does the same job for every backend.
