# Invités: a friend's own link wakes the PS5 and plays it (2026-11-20)

Goal: the host gives a friend a personal, revocable access. When the friend connects, the PS5
wakes, its login PIN is entered and the friend plays, without the host doing anything. The
host's PC is the gateway: the pairing keys and the PIN never leave it.

## How it works

```
friend's page or BooVI ── POST /session {access} ──▶ GuestStreamer ──▶ AccessDesk (one visit at a time)
        ◀── 202 {state: asking | waking | connecting}            │
        ◀── 200 offer, then WebRTC as for a share link            ▼
                                                  GuestAccessModel (vyAccess, UI thread)
                                                  ask the host (prompt, tray, 60 s) → PS5 Play flow
                                                  (probe, wake, remembered PIN, PS tap) → ready → playing
```

- `include/veyra/guest/GuestAccess.h`: the access (name, SHA-256 of a 264-bit token, end date,
  daily hours, ask or auto, allowed modes, most it receives), `matchAccess` (constant time over
  every access), `modeFor`, `SourceLimiter` (per source address), `AccessDesk`. Tested in
  `tests/fork/GuestAccessTests.cpp`.
- The waiting goes through `/session` (202 until admitted), so the named tunnel's existing
  ingress allowlist (`/`, `/session`, `/answer`, …) needs no change.
- `src/ui/GuestAccessModel.cpp`: the store (`<data dir>/guest-access.dpapi`, DPAPI, hashes only;
  a link is shown once), the gateway (share session held open with no stream, tray icon,
  minimize to tray, `HKCU\...\Run` with `--gateway-tray` only when ticked), the visit.
- A visit ends when the friend leaves (15 s without their peer), idles (`gatewayIdleMinutes`,
  20), is cut by the host, or their access is revoked or regenerated. A session this gateway
  started is stopped; when the visit woke the console it is kept `gatewayRestMinutes` (10), then
  `goto_bed` (skipped if the host's window is active), so a friend coming back finds it awake.
- Busy console (the host plays): an access allowing "à deux" or "regarder" joins after a host
  prompt; otherwise "la console est occupée". A second friend sees "occupé".
- Native client: `?a=` links in "rejoindre un partage"; it knocks the same way and, once it has
  played, keeps the link (DPAPI, `<data dir>/guest-saved-access.dpapi`) as "ps5 de <host>".
- Security: wrong tokens lock their source address (10 a minute, Cf-Connecting-Ip behind
  cloudflared), 240 requests a minute per address; logs name the access, never the token, and
  mask the address's last part.

## Limits

- If the PS5 asks for its login PIN and "remember the PIN" is off, the visit fails with a message.
- The rest delay keeps the Remote Play session open meanwhile (goto_bed needs a live session).
- Not run against the PS5 or through play.example.com; the native client's `?a=` path was not run.

## Reproduce (no PS5)

`VEYRA_TEST_ACCESS_SOURCE=<clip>` plays a file instead of the PS5 Play flow;
`VEYRA_TEST_ACCESS_FILE=<file>` creates an access "test" and a revoked one and writes their links;
`VEYRA_TEST_ACCESS_APPROVE` set makes "test" ask, `=1` approves after a second. With
`VEYRA_GUEST_REACH=tailnet` and `VEYRA_GUEST_PORT=47830`, open the link in a headless Edge with
`scripts\fork\guest-probe.ps1`.
