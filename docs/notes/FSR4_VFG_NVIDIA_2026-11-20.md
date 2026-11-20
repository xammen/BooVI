# FSR 4 and NVIDIA VFG on an RTX 5080 (2026-11-20)

Goal: two more frame generation / upscaling choices on an RTX 5080, measured on a real
PS5 Remote Play stream (1080p60 in, 1440p work extent, RTX Video SR medium unless noted).

## Result

| Choice | State on RTX 5080 | Why |
|---|---|---|
| NVIDIA VFG (frame generation) | works | The fork build compiled it out: `VEYRA_CUDA_DRIVER_INCLUDE_DIR` was never passed, so `VfgBackend::runtimeAvailable()` returned false. `build.ps1` now passes NVIDIA's public CUDA 12.8 cudart redist headers. VFG uses the Video Effects SDK, not the NVIDIA Optical Flow SDK. |
| FSR 4 INT8 upscaling (new SR choice "FSR 4 INT8 · non officiel", id 6) | works | The FSR 4.1.1 INT8 model ships in AMD's own `amd_fidelityfx_upscaler_dx12.dll` (FSR SDK 2.3.0). Only `ffxProvider_FSR4_Int8::IsSupported` refuses non-AMD adapters. `FsrProviderUnlock` makes it report support while the context is created, then restores it (OptiScaler's `Fsr4ForceEnableInt8` patches the same slot). No extra DLL. |
| FSR 4 ML frame generation | not possible | AMD requires an RX 9000. Forced the same way, the 4.0.1 provider created a context and dispatched on the 5080, but the generated frames were vertical stripes on black (`shots/fsr4-x2.png`). The model is FP8 and needs RDNA 4 matrix hardware. The choice stays greyed out with that reason. |

Provider inventory on the 5080 (`ffxQuery GET_VERSIONS`, SDK 2.3.0 DLLs): upscaler 3.1.5 and 2.3.4,
frame generation 3.1.6. With the provider unlocked: upscaler 4.1.1 too, frame generation 4.0.1 too.

## Measurements (real stream, steady state, median of 1 s windows)

Latency is `callbackToPresentReturnP95Ms`. FG and SR GPU costs are the `capture-timing` p95 columns.

| Run | Output fps | FG GPU ms | SR GPU ms | Latency ms | Image |
|---|---|---|---|---|---|
| DLSS FG x2 (baseline) | 120 | 2.20 | - | 21.5 | clean |
| FSR 3.1 FG x2 | 120 | 1.66 | - | 21.5 | clean |
| VFG x2 | 120 | 3.42 | - | 20.1 | clean |
| FSR 4 ML FG x2 (forced) | 120 | 6.76 | - | 21.7 | broken: stripes |
| VFG x6 (360 Hz display) | 210 | 17.08 | - | 23.2 | clean; GPU-bound, does not reach x6 |
| DLSS FG x6 | 118 | 2.19 | - | 25.0 | display was at 120 Hz in this run, so capped |
| FSR 4 INT8 SR + DLSS x2 | 120 | 2.12 | 2.25 | 28.6 | clean |
| FSR 3.1 SR + DLSS x2 | 120 | 2.15 | 0.36 | 25.4 | clean |

Offline, 1080p to 1440p on the 5080: FSR 4.1.1 INT8 1.4 to 1.7 ms a dispatch, FSR 3.1.5 0.3 ms. Both
have mean absolute error 0.004 against the synthetic source (`shots/offline-fsr31-vs-fsr4int8.png`).

No run logged `backend-recovery` or `ui-fg-rollback`. Logs, screenshots and the
per-run log slices are in `veyra-fork-app-fg\shots`.

## Known limits

- First use of FSR 4 INT8 on a driver install: the engine thread stalled about 40 s inside
  `nvgpucomp64.dll` (NVIDIA's shader compiler) before the first frame. The next run started at once.
  The FSR 4 ML frame generation context took 32 s to create the same way.
- The monitor switched between 360 Hz and 120 Hz during the series; output follows the display.

## Sources

- AMD FSR Frame Generation 4.0.2 limitations, "An AMD Radeon RX 9000 Series GPU or later":
  https://gpuopen.com/manuals/fsr_sdk/techniques/frame-interpolation-ml/
- AMD FSR Upscaling 4.1.2, "requires an AMD 7000 series discrete GPU or AMD 9000 series GPU":
  https://gpuopen.com/manuals/fsr_sdk/techniques/super-resolution-ml/
- OptiScaler 0.9.4 / 0.9.5 release notes, `Fsr4ForceEnableInt8` for Intel/NVIDIA, "only applies to
  the shipped SDK upscaler DLL": https://github.com/optiscaler/OptiScaler/releases
- OptiScaler `OptiScaler/fsr4/FSR4Models.cpp` (patches `IsSupported`, vtable slot 2, of
  `ffxProvider_FSR4_Int8`) and `FSR4DriverProviderDx12.cpp` (FP8 for frame generation only where
  the GPU really has FP8): https://github.com/optiscaler/OptiScaler
- FSR 4 ML frame generation on RDNA 3 works only on Linux through RADV FP8 emulation:
  https://www.techpowerup.com/344284/modder-runs-amd-fsr-4-redstone-frame-generation-on-rdna-3-but-only-on-linux
