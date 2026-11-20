# Building BooVI

Windows 10 or 11 x64, PowerShell, Python 3 (`py`), Git. No Visual Studio install and no admin
rights: the scripts put everything under `%USERPROFILE%\dev` (`-Dev` changes it).

## 1. Toolchain and SDKs

```powershell
scripts\fork\setup-dev.ps1      # CMake, Ninja, Qt 6.8.3, portable MSVC 2022, SDL3, FFmpeg 9.0 (LGPL),
                                # FidelityFX SDK 2.3.0 and 1.1.4, DLSS SDK 310.7
scripts\fork\build-rp-deps.ps1  # Opus, json-c, miniupnpc, libevent, nlohmann_json, libdatachannel,
                                # moonlight-common-c; OpenSSL comes with Qt's tools
```

Both are safe to rerun. One header set is manual: clone
[nv-codec-headers](https://github.com/FFmpeg/nv-codec-headers) at tag `n13.0.19.0` into
`%USERPROFILE%\dev\deps\nv-codec-headers-13.0`.

## 2. Runtime base

The NVIDIA (DLSS, DLSS-G, Video Effects), AMD FidelityFX and Intel XeSS runtime DLLs, the Qt
runtime and the shared assets are not built here: they come from a Veyra 2.1.9 NVIDIA portable
folder, downloaded from [Veyra's releases](https://github.com/Likely7/Veyra-NRVideo/releases/tag/v2.1.9)
and extracted anywhere. The build never writes to it, and never copies its `logs` or user data.

## 3. Build and stage

```powershell
scripts\fork\build.ps1 -Base <Veyra 2.1.9 portable folder> [-Out <app folder>] [-Repo <owner/name>]
```

It fetches and patches chiaki-ng at the pinned commit on first run, configures
`out\build\fork`, builds `BooVI.exe` and stages a runnable folder (`out\app` by default).
`-Repo` names BooVI's GitHub repository for update checks; without it the build checks nothing.

The version is set once, in `project()` at the top of `CMakeLists.txt`.

## 4. Tests

```bat
scripts\fork\run-tests.cmd
```

Builds and runs the fork's standalone tests in `tests\fork` (into `build-fork\`), plus the guest
page's controller parser when Node.js is installed.

## 5. Release zip

```powershell
scripts\fork\package.ps1 -App <staged folder> [-OutDir <folder>]
```

Writes `BooVI-<version>-win64-portable.zip` and its SHA-256. It keeps the exe, runtime DLLs, Qt
plugins and QML, shaders, licenses, `THIRD_PARTY_NOTICES.md` and a `README.txt`, and leaves out
logs, user data, `.pdb` files and the developer-only runtimes. See the script's header for the
exact list.

## Where things live at run time

| What | Where |
|---|---|
| Settings, presets, logs | next to `BooVI.exe` (`runtime_local\`, `logs\`) |
| PS5, Moonlight and Xbox pairings, sharing secrets, QML cache | `%LOCALAPPDATA%\BooVI` (`include/veyra/UserDataRoot.h`) |

`BOOVI_USER_DATA` points the second folder somewhere else, for tests and side-by-side copies.
A first start copies the Veyra-era `%LOCALAPPDATA%\Veyra` stores once, without touching them.

## Sharing through your own domain

See [SHARING.md](SHARING.md).
