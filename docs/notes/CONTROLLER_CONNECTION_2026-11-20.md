# Controller connection during a PS5 stream (2026-11-20)

Goal: the PC's controller connects without fuss during a PS5 Remote Play stream. A DualSense
left paired with the PS5 is the common case: the PC never sees it, or the PS5 takes it back
when PS is pressed. BooVI must say so briefly, stop saying it the moment a pad shows up, and
treat keyboard and mouse as a normal way to play.

## What was there

- `ControllerInput` (SDL3, `src/remoteplay/ControllerInput.cpp`) runs on the PS5 input thread
  (`RemotePlayInput`, 1 ms loop). It notices a lost pad on every poll and rescans once a second
  while it reads no pad or a pad that is not a DualSense. `choosePad` (`PadPolicy.h`) reads a
  DualSense when one is connected, otherwise keeps the current pad, otherwise the first one.
  Hot-plug already worked without restarting the stream, with up to 1 s of delay.
- The thread publishes the pad's capabilities every 100 passes. The UI reads them through
  `veyra.ps5` (`pad`, `controller`), but `tickPs5` only emitted `ps5Changed` for a focus or
  status change, so the stream overlay's pad chip could keep showing "aucune manette" after a
  pad came in. Nothing told the user why there was no pad.
- chiaki-ng tells the PS5 once, at stream start, that a controller is connected
  (`stream_connection_send_controller_connection`). The PS5 sends nothing back about its own
  local controllers, so BooVI cannot know whether the DualSense is on the console. The hint
  says "peut-être" and nothing more is invented.
- `Ps5InputChips.qml` is not instantiated anywhere; `StreamOverlay.qml` holds the live chips.

## State model

`PadPresence` (`include/veyra/remoteplay/PadPresence.h`) is pure: the UI thread feeds it every
tick with four facts and a clock, and tests drive it with made-up times.

Inputs:
- `live`: a PS5 picture streams and the PC sends input (not view-only).
- `kbm`: keyboard and mouse are captured.
- `pad`: the name of the pad the input thread reads, empty for none.
- `now` in ms.

States:

| state     | meaning                                                     | UI                   |
|-----------|-------------------------------------------------------------|----------------------|
| `Off`     | no PS5 stream that sends input                              | nothing              |
| `Waiting` | live, no pad for less than `kGrace` (3 s)                   | chip: aucune manette |
| `Missing` | live, no pad for `kGrace` or more                           | chip + hint card     |
| `Quiet`   | live, no pad, the user closed the hint or took the keyboard | chip                 |
| `Reading` | live, a pad is read                                         | chip with its name   |

Transitions:

```
Off ──live, no pad──▶ Waiting ──kGrace──▶ Missing ──close / kbm──▶ Quiet
 │                      │                   │                       │
 └──live, pad──▶ Reading ◀────pad───────────┴───────────────────────┘
                   │  └─ another pad ─▶ Reading (toast)
                   └──pad gone──▶ Waiting (the grace starts again)
any ──not live──▶ Off
```

- A pad that comes in while the stream already runs without one, at least `kSettle` (1 s)
  after the picture started, counts an arrival: the overlay shows a short toast with its name.
  A pad that was there when the picture started does not, and neither does the input thread's
  first poll.
- A switch to another pad while one is read (a DualSense switched on next to a virtual pad,
  `choosePad`) also counts an arrival, so the toast names the pad now read.
- `Quiet` lasts until a pad is read. A pad lost after that brings the hint back, because a
  DualSense taken by the PS5 is exactly that case.
- The grace keeps a Bluetooth blip or the gap of a pad switch from flashing the hint.
- Keyboard and mouse only: taking the keyboard closes the hint for the rest of the stream (or
  until a pad comes and goes). It is a normal way to play, not an error.

## Pairing gesture (checked)

Sony's page for PC, Mac and mobile: with the player indicator off, "while pressing and holding
the create button, press and hold the PS button until the light bar blinks", then pick the
controller in the device's Bluetooth settings
(playstation.com/support/hardware/pair-dualsense-controller-bluetooth). The button is Create,
not Share (Share is the DualShock 4). The hint gives that gesture and USB as the other way.

## What changed

- `ControllerInput`: a `SDL_EVENT_GAMEPAD_ADDED` event rescans on the next pass, so a pad
  plugged in mid-stream is read within a few ms instead of up to 1 s.
- `Capabilities` compares with `==`; `tickPs5` emits `ps5Changed` when the read pad changes,
  so every pad chip follows a hot-plug.
- `PadPresence` drives two new `veyra.ps5` keys: `padHint` (the hint card shows) and
  `padArrivals` (a counter; each change shows the toast). `veyra.ps5PadHintClose()` closes the hint.
- `PadNotice.qml`, placed under the pad chip in `StreamOverlay.qml`: the hint card and the
  arrival toast, in the existing Theme tokens, with the pad glyph and no status lights.
- No new setting.

## Verified

- `scripts\fork\run-tests.cmd`: all fork tests pass, including the 26 checks of
  `tests/fork/PadPresenceTests.cpp` (grace, hint gone on a pad, keyboard/mouse calm, close,
  switch toast, no toast for the first poll, per-stream reset, nameless pad).
- `python -B scripts/i18n/extract_fork.py --check`: 0 missing English, 0 placeholder problems.
- `scripts\fork\build.ps1` built `veyra_qml_ui` and staged it to a test folder.
- `PadNotice.qml` rendered offscreen with Qt's `qml` tool (hint card, then arrival toast).
- The staged app ran offscreen on `--page stream-preview-nopad`, muted, and logged no QML error.
- Not run: a live PS5 session with a real hot-plug. The input thread change is one line that
  only moves an existing rescan earlier.
