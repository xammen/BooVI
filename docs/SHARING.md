# Sharing a session through your own domain

BooVI shares a session with a friend over one of three paths, picked in **réglages → partage →
accès**:

| Path | What the sharing PC needs | Link |
|---|---|---|
| Tailscale | Tailscale, both PCs in the same tailnet | `https://<pc>.<tailnet>.ts.net:<port>/?t=…` |
| Lien rapide | `cloudflared` installed, no account | `https://<random>.trycloudflare.com/?t=…` |
| Named tunnel | `cloudflared` and your own Cloudflare tunnel, set up below | `https://<your host>/?t=…` |

Nothing in the source names a domain, a tunnel or a Cloudflare account. Without the settings
below, BooVI offers Tailscale and the quick link only.

## What a named tunnel needs

In your Cloudflare account, once:

1. A remotely managed tunnel, and a public hostname for it (for example `play.example.com`,
   a proxied CNAME to `<tunnel id>.cfargotunnel.com`).
2. Its ingress: forward `/`, `/index.html`, `/font.ttf`, `/session` and `/answer` to
   `http://127.0.0.1:47800`, and answer 404 to everything else.
3. A Realtime TURN key, so a friend behind a strict NAT still connects through Cloudflare's
   relay. Its API token is shown once, at creation.

## Storing the settings on the sharing PC

The tunnel token, the TURN key id and its API token never go in the repository, the logs or
the app's preferences. `scripts\fork\guest-cf-secrets.ps1` reads them as JSON on stdin and writes
`%LOCALAPPDATA%\BooVI\guest\cloudflare.dpapi`, encrypted with DPAPI for the current Windows user:

```powershell
Get-Content my-tunnel.json | powershell -NoProfile -File scripts\fork\guest-cf-secrets.ps1
Remove-Item my-tunnel.json
```

with `my-tunnel.json`:

```json
{"hostname": "play.example.com", "tunnelToken": "…", "turnKeyId": "…", "turnApiToken": "…"}
```

The script prints the hostname and the encrypted size, nothing secret. Restart BooVI: the named
tunnel becomes the default path, and **accès** shows your hostname.

Each Windows user, and each PC, needs its own copy, since DPAPI ties the file to the user.
`cloudflared tunnel token <tunnel id>` gives the tunnel token again on a new PC; the TURN API
token does not come back, so a new PC takes a new TURN key.

## Moving from Veyra

A PC that already shared through Veyra keeps its settings: on its first start, BooVI copies
`%LOCALAPPDATA%\Veyra\guest` (and the PS5, Moonlight and Xbox pairings) to `%LOCALAPPDATA%\BooVI`.
The Veyra folder is left as it was.
