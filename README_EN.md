<p align="center"><img src="assets/boovi.png" width="96" alt=""></p>

# BooVI

[Français](README.md)

BooVI is a Windows game streaming app: PS5 Remote Play, Xbox, PC (Moonlight) and capture cards.
Your graphics card improves the picture it receives (super resolution, frame generation) at low
latency.

## Features

- **PS5**: Remote Play on the local network, DualSense with gyro, touchpad and rumble.
- **Xbox**: streaming from your console (unofficial).
- **PC**: Moonlight client for a Sunshine host.
- **Capture cards**: shows and enhances the HDMI input.
- **Super resolution**: DLSS, FSR, RTX Video.
- **Frame generation**: DLSS Frame Generation, FSR 3.1, NVIDIA Video Frame Generation.
- **Low latency**: presentation paced to the display, G-SYNC and Reflex on NVIDIA.
- **Keyboard and mouse to controller**: play the PS5 without a pad.
- **Recording** of the session.
- **Remote play with a friend**: share your session, your friend joins with a link.

## Requirements

- Windows 10 or 11, 64-bit.
- A DirectX 12 graphics card and a recent driver.
- NVIDIA RTX: DLSS (RTX 20 and later), DLSS Frame Generation (RTX 40 and 50).
- AMD, Intel and others: FSR 3.1 for super resolution and frame generation.

## Download

Get `BooVI-<version>-win64-portable.zip` from [Releases](../../releases). There is no installer.

1. Extract the folder anywhere except `Program Files` (the Desktop works).
2. Run `BooVI.exe`. Windows SmartScreen may ask you to "Run anyway".

Settings stay in the app's folder. Console pairings and sharing secrets are encrypted for your
Windows account in `%LOCALAPPDATA%\BooVI`.

## Quick start: PS5

1. On the PS5: **Settings → System → Remote Play**, turn Remote Play on.
2. In BooVI: **consoles → PS5**. Turn the PS5 on or leave it in rest mode; BooVI finds it on
   your local network. Otherwise, enter its IP address.
3. **Pair, once**: you need your PSN Account ID (**trouver mon id** opens a site that shows it),
   then the 8-digit code the PS5 shows under **Remote Play → Pair Device**.
4. If the PS5 asks for your account's sign-in PIN, BooVI can remember it, encrypted on this PC.
5. **Play**.

Controller:

- **Disconnect your DualSense from the PS5 first** (turn it off by holding the PS button for
  about ten seconds), or it drives the console directly and BooVI never sees it.
- Plug it in over USB or pair it with the PC over Bluetooth.
- Without a controller, the keyboard and mouse drive a virtual one.

## Remote play with a friend

During a session, **partager** makes a link. Your friend pastes it in BooVI (**accueil →
rejoindre un partage**) or opens it in Chrome or Edge. On their side, their graphics card does
the super resolution and frame generation.

The link goes through one of these paths, chosen in **réglages → partage**:

- **Tailscale**: you are both in the same tailnet.
- **Quick link**: an account-less Cloudflare tunnel (install `cloudflared` on the sharing PC).
- **Your own domain**: a named Cloudflare tunnel. See [docs/SHARING.md](docs/SHARING.md).

## Build from source

See [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md).

## License

GPLv3, see [LICENSE](LICENSE). The PS5 part uses [chiaki-ng](https://github.com/streetpea/chiaki-ng)
(AGPL-3.0). Third-party components and their licenses are listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) and `licenses/`.

Based on [Veyra](https://github.com/Likely7/Veyra-NRVideo), GPLv3.

PlayStation, PS5 and DualSense are trademarks of Sony Interactive Entertainment. Xbox is a
trademark of Microsoft. BooVI is not affiliated with either.
