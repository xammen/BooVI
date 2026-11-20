# Builds BooVI's portable release zip, BooVI-<version>-win64-portable.zip, from a staged app
# folder (scripts\fork\build.ps1 or stage-app.ps1), and writes its SHA-256 beside it.
# Usage: scripts\fork\package.ps1 -App <staged folder> [-OutDir <folder>] [-Dev <deps root>]
#
# Kept: BooVI.exe, the Qt, FFmpeg, OpenSSL and MSVC runtime DLLs, Qt plugins and QML, shaders,
# the guest page, the NVIDIA, AMD and Intel runtimes, models, licenses, THIRD_PARTY_NOTICES.md,
# LICENSE and a README.txt.
# Left out: logs, crash dumps, user data and secrets, debug symbols, the base package's own docs,
# assets, startup music and file manifests, and three runtimes whose redistribution is not cleared:
#  - nr-ampere\nvngx_dlssnr.dll: a community-modified NVIDIA binary, unsigned. NVIDIA's SDK
#    licence forbids handing on modified runtimes.
#  - nr-original\nvngx_dlssnr.dll: an NVIDIA experimental runtime that is not in a public SDK.
#  - dlssg-lite\nvngx_dlssg.dll: taken from the NVIDIA app's OTA cache, not from the DLSS SDK.
# The official DLSS SDK runtimes next to them (nvngx_dlss 310.7, nvngx_dlssg 310.9.1 and the
# 310.7 fallback) stay: the DLSS SDK licence lets an application ship them.
param([Parameter(Mandatory)][string]$App,
      [string]$OutDir = (Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'dist'),
      [string]$Dev = (Join-Path $env:USERPROFILE 'dev'))
$ErrorActionPreference = 'Stop'
$fork = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$App = (Resolve-Path $App).Path
if (-not (Test-Path "$App\BooVI.exe")) { throw "no BooVI.exe in $App" }

$cmake = Get-Content -Raw "$fork\CMakeLists.txt"
if ($cmake -notmatch 'project\(veyra VERSION ([0-9]+\.[0-9]+\.[0-9]+)') { throw 'no version in CMakeLists.txt' }
$version = $Matches[1]
$exeVersion = (Get-Item "$App\BooVI.exe").VersionInfo.ProductVersion
if ($exeVersion -ne $version) { throw "BooVI.exe says $exeVersion, CMakeLists.txt says ${version}: rebuild first" }
$name = "BooVI-$version-win64-portable"

$work = Join-Path ([IO.Path]::GetTempPath()) "boovi-package-$([guid]::NewGuid().ToString('N'))"
$stage = Join-Path $work 'BooVI'
try {
    $excludeDirs = @('logs', 'crashdumps', 'docs', 'assets', 'media', 'runtime_local',
                     'runtime\experimental\nr-ampere', 'runtime\experimental\nr-original', 'runtime\experimental\dlssg-lite') |
                   ForEach-Object { Join-Path $App $_ }
    $excludeFiles = @('*.pdb', '*.ilk', '*.log', '*.dmp', '*.dpapi', '*.fork-backup', 'pids.txt', 'link*.txt',
                      'veyra_qml_ui.exe', 'README*', 'RELEASE_*', 'GPU-PACKAGE.txt', 'LISEZ-MOI*',
                      'package-manifest.json', 'vfg-runtime-manifest.json')
    robocopy $App $stage /E /NFL /NDL /NJH /NJS /NP /XD @excludeDirs /XF @excludeFiles | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "robocopy failed ($LASTEXITCODE)" }
    Remove-Item "$stage\release-runtime-manifest.json" -ErrorAction SilentlyContinue   # the app reads runtime\experimental's
    # runtime_local holds the AMD FSR and Intel XeSS runtimes next to the user's data: only those two.
    foreach ($vendor in 'amd', 'intel') {
        if (Test-Path "$App\runtime_local\$vendor") {
            robocopy "$App\runtime_local\$vendor" "$stage\runtime_local\$vendor" /E /NFL /NDL /NJH /NJS /NP /XF *.log *.pdb | Out-Null
            if ($LASTEXITCODE -ge 8) { throw "robocopy $vendor failed ($LASTEXITCODE)" }
        }
    }

    # The diagnostics page lists the runtime manifest: only what the zip ships.
    $manifestPath = "$stage\runtime\experimental\release-runtime-manifest.json"
    if (Test-Path $manifestPath) {
        $manifest = Get-Content -Raw $manifestPath | ConvertFrom-Json
        $manifest.files = @($manifest.files | Where-Object { Test-Path (Join-Path $stage $_.path) })
        $manifest | ConvertTo-Json -Depth 20 | Set-Content -Encoding utf8 $manifestPath
    }

    # Licenses: the base package's, the repository's on top, and the FFmpeg build's own.
    robocopy "$fork\licenses" "$stage\licenses" /E /NFL /NDL /NJH /NJS /NP | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "robocopy licenses failed ($LASTEXITCODE)" }
    $ffmpegLicense = Get-ChildItem "$Dev\deps\ffmpeg\*\LICENSE.txt" | Select-Object -First 1
    if (-not $ffmpegLicense) { throw "no FFmpeg build license under $Dev\deps\ffmpeg" }
    Copy-Item $ffmpegLicense.FullName "$stage\licenses\FFMPEG-BTBN-LGPL-BUILD.txt"
    Copy-Item "$fork\LICENSE", "$fork\THIRD_PARTY_NOTICES.md" $stage
    $readme = @"
BooVI $version

FRANÇAIS
1. Décompresse ce dossier où tu veux, sauf dans Program Files (par exemple sur le Bureau).
2. Lance BooVI.exe. Windows SmartScreen peut demander « Exécuter quand même ».
3. PS5 : consoles, PS5, puis associe-la une fois avec le code à 8 chiffres de la PS5
   (Paramètres, Système, Lecture à distance, Associer un appareil).
   Déconnecte d'abord ta DualSense de la PS5, puis branche-la au PC.

Les réglages restent dans ce dossier. Les associations et les secrets de partage sont chiffrés
pour ton compte Windows dans %LOCALAPPDATA%\BooVI.

ENGLISH
1. Extract this folder anywhere except Program Files (the Desktop works).
2. Run BooVI.exe. Windows SmartScreen may ask you to "Run anyway".
3. PS5: consoles, PS5, then pair it once with the PS5's 8-digit code
   (Settings, System, Remote Play, Pair Device).
   Disconnect your DualSense from the PS5 first, then connect it to the PC.

Settings stay in this folder. Pairings and sharing secrets are encrypted for your Windows
account in %LOCALAPPDATA%\BooVI.

BooVI is free software under the GNU GPL v3 (LICENSE). Based on Veyra
(https://github.com/Likely7/Veyra-NRVideo), GPLv3. Third-party components and their licenses:
THIRD_PARTY_NOTICES.md and the licenses folder.
"@
    [IO.File]::WriteAllText("$stage\README.txt", $readme.Replace("`r`n", "`n").Replace("`n", "`r`n"), [Text.UTF8Encoding]::new($true))

    # What must never ship.
    $bad = Get-ChildItem $stage -Recurse -File | Where-Object {
        $_.Extension -in '.pdb', '.ilk', '.log', '.dmp', '.dpapi' -or $_.Name -eq 'veyra_qml_ui.exe' -or
        $_.FullName -match '\\(user-data[^\\]*|logs|crashdumps|nr-ampere|nr-original|dlssg-lite)\\' -or $_.Name -match 'qml-preferences'
    }
    if ($bad) { throw "would ship: $($bad.FullName -join ', ')" }
    foreach ($need in 'BooVI.exe', 'Qt6Core.dll', 'platforms\qwindows.dll', 'qml\Veyra\qmldir', 'shaders', 'web\guest\index.html',
                      'LICENSE', 'THIRD_PARTY_NOTICES.md', 'README.txt', 'licenses\qt', 'licenses\remoteplay\CHIAKI_AGPL3_OPENSSL.txt') {
        if (-not (Test-Path (Join-Path $stage $need))) { throw "missing $need" }
    }

    New-Item -ItemType Directory -Force $OutDir | Out-Null
    $zip = Join-Path (Resolve-Path $OutDir).Path "$name.zip"
    Remove-Item $zip, "$zip.sha256" -ErrorAction SilentlyContinue
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [IO.Compression.ZipFile]::CreateFromDirectory($stage, $zip, [IO.Compression.CompressionLevel]::Optimal, $true)
    $hash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLowerInvariant()
    Set-Content -Encoding ascii "$zip.sha256" "$hash  $name.zip"
    $files = (Get-ChildItem $stage -Recurse -File).Count
    Write-Host ("{0}  {1:N0} bytes  {2} files  sha256 {3}" -f $zip, (Get-Item $zip).Length, $files, $hash)
} finally {
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}
exit 0   # robocopy leaves 1-3 in $LASTEXITCODE on success
