# Sharing a session with a friend

BooVI shares a session over one of two paths, picked in **réglages → partage → accès**:

| Path | What the sharing PC needs | Link |
|---|---|---|
| Broker (default) | nothing: the build names a broker | `https://<broker>/<room>#t=…` |
| Tailscale | Tailscale, both PCs in the same tailnet | `https://<pc>.<tailnet>.ts.net:<port>/?t=…` |

The friend opens the link in Chrome or Edge, or pastes it in BooVI (**accueil → rejoindre un
partage**). Nothing to install on either side.

## How the broker path works

```
 friend's browser / BooVI                 broker (Cloudflare Worker "boovi")          your BooVI
 ───────────────────────                  ──────────────────────────────────          ──────────
 GET https://<broker>/<room> ───────────▶ static guest page (edge cache)
 WebSocket /api/room/<room>  ◀──────────▶ Room (Durable Object) ◀──────────▶ WebSocket /api/host/<room>
   offer / answer / ICE, and the            relays text, reads nothing            (outbound, opened when
   link's secret, inside messages                                                  sharing starts)
                                          TurnLedger (Durable Object)  ◀── owner key: a TURN lease per guest
 media: WebRTC, direct (STUN) ◀════════════════════════════════════════════════════▶ media
        or through Cloudflare TURN when the host is entitled and the direct path fails
```

- **The room.** Your BooVI keeps a 264-bit room key on this PC (`guest\room.dpapi`, DPAPI). The
  room id in links is a hash of it (132 bits), so only your PC can open your room. The Worker
  checks the key, then relays the guest's messages to your BooVI without reading them.
- **The secret stays in the fragment.** `#t=` (a share's one-shot token) and `#a=` (a permanent
  access) are never sent to the broker by the browser. The page carries the secret to your
  BooVI inside the room, and BooVI checks it as it always did: one-shot token, hashed permanent
  access, the approval card, per-IP lockout after wrong secrets.
- **Limits.** The broker limits room joins (30 a minute) and room openings (10 a minute) per IP.
  A room closes when your BooVI leaves, and after 12 hours at most.
- **Media never goes through the Worker.** The picture, sound and controller travel by WebRTC,
  direct when the two networks allow it.

## Several friends on one link

Up to six friends (four by default, **réglages → partage → invités à la fois**) open the same link
or their own access. They all watch, with sound; one holds the PS5's controller. In **partager**
you see who is there, pass the controller, make someone watch-only, take it back, roll the dice or
disconnect one friend. On their page (or in their BooVI) friends ask for the controller, wait in
the queue, give it back or roll the dice, which this PC draws. Optional: a turn length, and the
controller coming back to you when you press your own pad. Each friend gets the picture their link
takes; **envoi max** is your whole upload, shared among them, the player first. Who played and how
long is kept in **réglages → partage → historique des parties**.

A Discord channel can hear about it (ps5 libre, who connects, who has the controller, the queue,
the dice, the share's start, stop or a stuck share): paste a webhook in **réglages → partage →
discord**. The link is checked, encrypted with DPAPI in `%LOCALAPPDATA%\BooVI\guest\discord.dpapi`,
never shown again and never logged. Messages are grouped, at most one every 3 s.

Nothing changes on the broker: everything after the connection travels on each friend's own
WebRTC connection. Design and measurements: [notes/MULTI_GUEST_2026-11-21.md](notes/MULTI_GUEST_2026-11-21.md).

## The relay (TURN)

When the direct path fails (two strict NATs), a TURN relay carries the stream. Two ways to have
one, set with `scripts\fork\guest-cf-secrets.ps1` (DPAPI, never in the repository or the logs):

- **The broker's owner key.** A host that proves it (`ownerKey`) gets a short-lived Cloudflare
  TURN credential per guest, from the broker, within a monthly cap. The broker's own TURN key
  stays a Worker secret.
- **Your own Cloudflare TURN key** (`turnKeyId`, `turnApiToken`), used directly by your BooVI.

Without either, sharing works direct-only (STUN): **accès** says so.

```powershell
# the owner key that broker\scripts\put-secrets.ps1 sealed:
powershell -NoProfile -File scripts\fork\guest-cf-secrets.ps1 -OwnerKeyFile <owner-key.dpapi>
# or your own TURN key, as JSON on stdin:
'{"turnKeyId": "…", "turnApiToken": "…"}' | powershell -NoProfile -File scripts\fork\guest-cf-secrets.ps1
```

Each Windows user, and each PC, needs its own copy, since DPAPI ties the file to the user.

## Running your own broker

`broker/` is the Worker (`boovi`): one Durable Object class per room, one for the TURN ledger,
the guest page as static assets. On a Cloudflare account with a zone:

```powershell
cd broker; npm ci; npm test
powershell -File scripts\deploy.ps1 -Domain go.example.com
powershell -File scripts\put-secrets.ps1 -Zone example.com -OwnerKeyFile <path> -Config <cloudflare.dpapi with your TURN key>
```

Then build BooVI with `scripts\fork\build.ps1 … -Broker https://go.example.com`. The domain is
never written in the repository. The cap and TTLs are Worker vars (`wrangler.jsonc`, or
`deploy.ps1 -Var TURN_BUDGET_GB:500`); `GET /api/turn` with the owner key shows the month's ledger.
Design notes and measurements: [notes/GUEST_BROKER_2026-11-20.md](notes/GUEST_BROKER_2026-11-20.md).

## Moving from Veyra

A PC that already shared through Veyra keeps its settings: on its first start, BooVI copies
`%LOCALAPPDATA%\Veyra\guest` (and the PS5, Moonlight and Xbox pairings) to `%LOCALAPPDATA%\BooVI`.
The Veyra folder is left as it was. A tunnel token stored by an older BooVI is ignored, and
dropped the next time `guest-cf-secrets.ps1` runs.
