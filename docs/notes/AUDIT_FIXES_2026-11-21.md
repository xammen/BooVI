# Audit fixes, 2026-11-21

Fixes for items 3, 4, 5 and the non-guest part of item 9 of the log audit of 2026-11-21
(`perf-audit-2026-11-21.md`, from the field logs of 2026-11-20). Guest and share code is not
touched here.

| Item | Commit | Change |
|---|---|---|
| 3 | `5a053310` | The PS5 input thread ends with its session. The UI thread gets a stall reporter. |
| 4 | `859e6574` | Moved logs keep a UTC timestamp, and the newest five stay. |
| 5 | `c81ee4f9` | Every audio re-anchor logs its cause. No re-anchor at the latency floor. |
| 9 | `74fd0789` | Clock resets and 1 s maxima, DXGI stats age, composition query, quit reason and rest state, screen A/V skew. |

## 3. The PS5 input thread outlived its session

**Root cause (from the log).** No flag stayed set. At 23:22:00 the PS5 quit (quitReason=12), the
source set `failed`, the engine tore down and set `running=false`. The UI tick logs both changes
unconditionally (`qml-window source-state` and `ui-live-failed`). Neither line exists, and no
interface line of any kind follows until the log ends at 01:21. So the UI tick did not run after
23:22. `tickPs5` was the only place that stopped the input thread. The thread `t=38892` then
polled the pad at 1 kHz until the app was closed.

Why the UI thread stopped is not known. One lead belongs to the guest code: after the guest went
silent at 23:01:54, four `/session` requests logged a TURN lease and then nothing. They logged no
`peer ... offered` and no `offer after`, so `createPeer` never returned. The UI thread calls
`GuestStreamer::status()` every tick.

**Change.**
- `InputLifetime.h` holds the one rule for "the session still wants input". The UI tick and the
  thread both use it.
- `RemotePlayInput` reads `EngineController::inputSession()` every 100 ms and ends by itself.
  `start()` joins a thread that ended and starts a new one. Both run under the thread's lock, so a
  reconnect at that moment never lands on a dead thread.
- The bridge runs the engine's stall reporter on its 16 ms tick (`ui-hang`, 15 s). It logs the
  blocked stack and writes one minidump beside the log. The watchdog clock now excludes system
  sleep, so a resume is not reported as a stall.

**Verified.** `InputLifetimeTests` walks the field log's states: opening, playing, keyframe
recovery, recovery stopped with the teardown still running, teardown complete. Four app runs of this
build logged no `ui-hang` line.

**Not verified.** The thread ending by itself in a real PS5 session. A real UI stall report.

## 4. Log retention

Moved logs are now `veyra-qml.<UTC yyyyMMdd-HHmmss>[-n].log`. The newest five stay and older ones are
deleted. The live file stays `logs/veyra-qml.log`. The comment now says 64 MB is about 2.7 h of a PS5
stream with frame generation and a guest.

The per-second tags stay per second. The audit's own analyses need them (item 8's per-second present
p99, item 6's bitrate buckets), and five files cover about 13 h.

**Verified.** `veyra_logger_tests` passes all 14 checks: rotation keeps five, the oldest are deleted, two
restarts keep both earlier logs. Four app runs with `VEYRA_TEST_LOG_ROTATE_BYTES=40000` moved the
first run's 116 KB log at start, kept every file across restarts and pruned to five:

```
[log] earlier sessions moved to veyra-qml.20261121-025157.log (116646 bytes; the newest 5 moved logs are kept)
[log] rotated reason=size; the earlier part of this session is in veyra-qml.20261121-025246.log (40669 bytes)
```

## 5. Host audio re-anchors

**Cause (measured).** The drift loop's error can be rebuilt from each `live-audio-sync` line:
`skew + videoPts - videoHost + ingressMap + target`. In the field log:
- 22 of the 26 re-anchors fired at about -60 ms with the target clamped to 0 to 5 ms.
- Each one came back to -42 to -55 ms within seconds.

With video ahead of audio the automatic target is 0. The played audio then sits at its latency floor
(capture block, queue reserve, endpoint), about 50 ms behind, and arrival jitter crossed the 60 ms
limit every 30 s or so. A re-anchor cannot remove that floor, so each one was a 30 ms fade for
nothing. The other 3 followed a jump in the video delay. The last one was the underrun before the
quit.

**Change.**
- Every reset logs `capture-audio-reanchor-cause`. The reason is one of `endpoint-start`,
  `source-discontinuity`, `video-reset`, `input-overflow`, `queue-overflow`, `sync-setting`,
  `pcm-timeline-jump` with the gap, `underrun`, or `drift`. A drift reset adds the error, the filtered
  error, what the anchor reached, the target and the queues.
- `captureDriftReanchor`: audio ahead past 60 ms re-anchors as before. Late audio re-anchors only when
  it is also 20 ms later than the last anchor reached.
- `live-audio-sync` gains `driftHeld`, the drift checks the old rule would have acted on.

**Verified.** `CaptureAudioDriftTests` uses the field values. `veyra_capture_audio_tests
--latency-floor 25` drives the real session for 40 s, muted, with 10 ms blocks arriving 0 to 25 ms
late and video 10 ms ahead. It reproduced the field state: target 0 to 8 ms, skew -41 to -66 ms. The
old rule would have acted on 33 checks (`driftHeld=33`). The new one re-anchored 0 times after the
start.

**Not verified.** A PS5 evening. That session should show:
- far fewer `capture-audio-reanchor-cause reason=drift` lines than the 22 in 73 min seen before;
- `driftHeld` growing in `live-audio-sync`;
- `skewMs` staying in the same -40 to -65 ms band.

## 9. Instrumentation

- `remoteplay-clock`: each reset of the arrival estimate is logged with the arrival gap and index
  gap. The 10 s line adds `max1sMs=[...]`, the largest arrival minus estimated PTS of each second,
  and `resets=`.
- `display-stats`: `statsAgeMs`, the age of DXGI's last frame statistics. A `dxgiPresentCount=0`
  with a large age means DXGI stopped updating them, not that nothing showed. This run (60 Hz,
  window in the background) showed 57 to 63 a second and an age of 15 to 80 ms. The field's 360 Hz
  zeros are still unexplained.
- `display-latency`: `compositionQuery`. Every run showed `0x80004002` (E_NOINTERFACE). The D3D12
  swap chain has no `IDXGISwapChainMedia`, so `compositionMode=-1` was never a measurement. Present
  mode needs PresentMon or ETW.
- `remoteplay-quit`: chiaki's quit reason name and the console's own text, with addresses removed.
  `quitReason=12` is `STREAM_CONNECTION_REMOTE_SHUTDOWN`: the console ended the stream, which is why
  recovery did not retry. After such a stop, one discovery probe 3 s later logs whether the console
  answers as resting, awake, or not at all. No address or console id is logged.
- `av-skew`: every 10 s for live sources, the skew against the picture's Present and against the
  picture on screen (Present plus the measured scanout p50). It leaves out the audio output latency
  after the device position.

Not done here: the guest items of 9 (stale "real" in `guest-latency`, the dated-frame count, the
silent `/session` returns). They belong to the guest code.

**Not verified.** The clock, quit and A/V lines need a PS5 session. The file replay used here does not
go through the remote source or the live audio session.
