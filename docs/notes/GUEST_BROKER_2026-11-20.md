# Sharing through a broker, without cloudflared (2026-11-20)

Replaces the named tunnel and the quick tunnel (GUEST_SHARE_2026-11-20.md) with a Cloudflare
Worker that serves the guest page and relays signaling. User-facing summary: docs/SHARING.md.

## Design

- Worker `boovi` (`broker/`), static assets = `web/guest`, two SQLite Durable Object classes:
  `Room` (one per host, hibernating WebSockets) and `TurnLedger` (one, the month's TURN budget).
- The host opens `wss://<broker>/api/host/<room>` outbound, `Authorization: Bearer <room key>`.
  Room key: 33 random bytes, base64url, DPAPI in `guest\room.dpapi`. Room id: first 132 bits of
  `SHA-256("boovi room v1\n" + key)`, so a link never lets anyone open the host's room.
- A guest (page or BooVI client) opens `wss://<broker>/api/room/<room>` and sends
  `{id, path: "/session" | "/answer", body}`; the room forwards it to the host with the guest's IP,
  the host runs the same handler the tailnet HTTP server runs and answers `{id, status, body}`.
- Secrets ride in the fragment (`#t=`, `#a=`) and inside messages. The Worker never parses a
  message body. The host keeps every check it had: one-shot token, hashed access, approval card
  with its 1.2 s guard, per-IP lockout (it now sees the real guest IP from `CF-Connecting-IP`).
- Rate limits (Workers rate-limit binding, per IP, per location): 30 joins/min, 10 room
  openings/min. A room holds 8 guests, 64 KiB per message, 240 messages/min per guest; it closes
  with its host (code 4404) and at `ROOM_TTL_S` = 12 h.
- The page builds its `RTCPeerConnection` and opens the room socket at load (a cold
  `new RTCPeerConnection()` costs about a second in Edge); the offer's ICE servers are applied
  with `setConfiguration`. The room socket closes once the answer's reply is in.

## TURN entitlement and the monthly cap

- `OWNER_KEY` (Worker secret, 33 random bytes) vs `X-Boovi-Owner` on the host socket, compared in
  constant time. An entitled host asks `{t: "turn"}` per guest and gets a lease: Cloudflare TURN
  credentials made with the owner's TURN key (`TURN_KEY_ID`, `TURN_KEY_API_TOKEN`, Worker secrets).
  It hands the lease back (`turn-end`) as soon as the path is direct or the guest leaves; the
  ledger revokes it at Cloudflare and settles it.
- Other hosts get STUN only, or use their own TURN key from their DPAPI config.
- **Cap: reservation, not analytics.** A credential is charged its whole TTL at the most a share
  can send (`TURN_RATE_MBPS` = 120) before it is created; a revoked one is refunded down to its
  real time plus `TURN_GRACE_S` = 60 s, and only after Cloudflare confirms the revocation (204).
  Past `TURN_BUDGET_GB` = 900 per UTC month, no credential is issued; near the end the TTL
  shrinks to what is left, and under `TURN_TTL_MIN_S` = 600 s none is issued.
  Why not GraphQL analytics: they lag about 30 s, are sampled, need an extra account token, and
  Cloudflare documents no Realtime spend cap. A reservation is an upper bound, since Cloudflare
  stops billing a credential at its expiry (TURN FAQ). Worst case is bounded by the budget even if
  every revocation fails; the cost of being safe is that a 4 h lease holds 216 GB until settled.
- Vars fail closed: an unreadable budget is 0, an unreadable rate is 1000 Mb/s.
- Tests: `broker/test/budget.test.ts`, `ledger.test.ts` (real SQLite, fake TURN API), mutation
  checked (refund without revoke, no grace, reserve after the call all fail a test).

## Cloudflare free tier, as read on 2026-11-20

| Product | Free allowance | Source |
|---|---|---|
| Workers | 100,000 requests/day, 10 ms CPU/request, static asset requests free and unlimited | developers.cloudflare.com/workers/platform/limits/, /workers/platform/pricing/ |
| Durable Objects | SQLite backend only; 100,000 requests/day (incoming WebSocket messages billed 20:1); 13,000 GB-s/day, none while hibernating; 5 M rows read, 100 k rows written/day; 5 GB stored | developers.cloudflare.com/durable-objects/platform/pricing/, /limits/ |
| Realtime TURN | $0.05/GB egress; first 1,000 GB/month free, shared with the SFU; no spend cap documented; credentials up to 48 h; billing stops at credential expiry | developers.cloudflare.com/realtime/sfu/platform/pricing/, /realtime/turn/faq/ |
| Rate limiting binding | period 10 or 60 s, counts per location (approximate) | developers.cloudflare.com/workers/runtime-apis/bindings/rate-limit/ |

A share costs a few dozen Worker requests and DO messages, so the daily limits allow hundreds
of shares a day. The 900 GB budget sits under the 1,000 GB free TURN allowance.

## Measurements (same PC, same host build, headless Edge, fresh profile per run)

Host: muted test instance sharing a 10-minute local clip, no PS5. Times from navigation start.

| Flow | Before (quick tunnel, old app) | After (broker) |
|---|---|---|
| link opened with autojoin → first frame, median of 5 | 1.63 s | 1.15 s |
| friend clicks after 3 s → first frame, median of 5 | 1.16 s (one run stalled 10 s in the tunnel) | 0.42 s |
| owner host (TURN lease per guest), default ICE, median of 5 | n/a | 1.85 s (min 1.17) |
| owner host, forced relay, median of 3 | n/a | 2.83 s (min 1.53) |
| native BooVI client, process start → first picture decoded | n/a | 4.7 s (includes app start-up) |

The page itself comes from Cloudflare's edge in 40–60 ms once cached (270–530 ms cold). Host ICE
gathering takes 10–110 ms; a TURN allocation that does not answer still holds the offer to the
3 s gathering cap (seen in 2 of 13 owner runs, as before the change).

In-game latency: the media path is unchanged (same encoder, same WebRTC transport, same data
channels); the broker carries only signaling and the room socket closes after the answer. Host
`presentToSent` p50 2.7 ms, `encode` p50 2.2 ms during the after runs.

## Verified on the deployed broker

`broker-live` checks (Node client against the deployed Worker): page served at a room link with
CSP and `no-referrer`; another room's key refused; non-owner host opens its room, not entitled,
TURN refused (`not-entitled`); guest text reaches the host unread with the guest's IP; reply
reaches the guest; room closes with its host (4404) and later guests get `offline`; owner host
entitled; real TURN lease in 219 ms, charged 216 GB up front, revoked and settled to 0.92 GB;
with a budget under one lease, `budget` refusal; join and room-creation rate limits trip.

End to end: browser guest direct (5/5), owner relay forced (3/3, `candidate=relay`), native
BooVI client direct (first picture 1920x1080, d3d12va), a wrong `#t=` refused by the host (403,
logged with the guest's /24).
