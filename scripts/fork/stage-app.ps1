# Assembles a runnable fork build: the Qt runtime, NVIDIA runtime files and assets come from
# an existing Veyra 2.1.9 portable folder (never its logs or runtime_local user data), then
# the fork's exe, shaders, QML, FFmpeg and OpenSSL DLLs go on top.
# Usage: scripts\fork\stage-app.ps1 -Base <Veyra-2.1.9 portable dir> -Out <new app dir>
param([Parameter(Mandatory)][string]$Base, [Parameter(Mandatory)][string]$Out,
      [string]$Build = (Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'out\build\fork'),
      [string]$Dev = (Join-Path $env:USERPROFILE 'dev'))
$ErrorActionPreference = 'Stop'
$fork = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
function Copy-Tree($from, $to, [string[]]$excludeDirs = @(), [string[]]$excludeFiles = @()) {
    $args = @($from, $to, '/E', '/NFL', '/NDL', '/NJH', '/NJS', '/NP')
    if ($excludeDirs) { $args += '/XD'; $args += $excludeDirs }
    if ($excludeFiles) { $args += '/XF'; $args += $excludeFiles }
    robocopy @args | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "robocopy $from failed ($LASTEXITCODE)" }
}
if (-not (Test-Path "$Build\BooVI.exe")) { throw "no build at $Build" }
# veyra_qml_ui.exe is the exe's name before BooVI, present when the base is an older staged app.
# The base's own QML and guest page are replaced whole, so no file this tree deleted survives.
Copy-Tree $Base $Out @("$Base\logs", "$Base\runtime_local", "$Base\qml\Veyra", "$Base\web\guest") @('startup-music.mp3', 'veyra_qml_ui.exe')
# runtime_local holds the AMD FSR and Intel XeSS runtimes next to the user data: copy those two only.
foreach ($vendor in 'amd', 'intel') {
    if (Test-Path "$Base\runtime_local\$vendor") { Copy-Tree "$Base\runtime_local\$vendor" "$Out\runtime_local\$vendor" }
}
Copy-Item "$Build\BooVI.exe" "$Out\BooVI.exe" -Force
Copy-Tree "$Build\shaders" "$Out\shaders"
Copy-Tree "$fork\qml\Veyra" "$Out\qml\Veyra"
# Fork, partage: the page a remote guest opens (GuestStreamer.h serves it).
Copy-Tree "$fork\web\guest" "$Out\web\guest"
Copy-Item "$fork\i18n\catalog.json" "$Out\i18n\catalog.json" -Force
$ff = Get-ChildItem "$Dev\deps\ffmpeg" -Directory | Select-Object -First 1
Copy-Item "$($ff.FullName)\bin\*.dll" $Out -Force
Copy-Item "$Dev\Qt\Tools\OpenSSLv3\Win_x64\bin\libcrypto-3-x64.dll", "$Dev\Qt\Tools\OpenSSLv3\Win_x64\bin\libssl-3-x64.dll" $Out -Force
Write-Host "staged $Out"
exit 0
