# Plays the benchmark clip as a live source (the capture-card scheduler a console stream uses) with
# frame generation on the source picture, dumps the frames handed to presentation and keeps the log.
# Usage: run-bench.ps1 -App <app dir> -Clip <fgbench30.mp4> -Out <run dir> [-Backend dlss|fsr3|vfg] [-Motion flow|zero]
#        [-Guard on|off|hud|fast] [-Seconds 22] [-Every 2] [-NoDump] [-Env NAME=VALUE,...]
# -Guard sets réglages → image → anti-artefacts in the copied preferences before the launch.
# The app's copied preferences must be silent (volume 0, intro music off): checked before launch.
param([Parameter(Mandatory)][string]$App, [Parameter(Mandatory)][string]$Clip, [Parameter(Mandatory)][string]$Out,
      [string]$Backend = 'dlss', [string]$Motion = 'flow', [ValidateSet('on','off','hud','fast')][string]$Guard = 'on', [int]$Seconds = 22, [int]$Every = 2, [switch]$NoDump, [string[]]$Env = @())
$ErrorActionPreference = 'Stop'
$prefs = Join-Path $App 'runtime_local\user-data-2.0.0\qml-preferences.v1.json'
$p = Get-Content $prefs -Raw | ConvertFrom-Json
if ($p.volume -ne 0 -or $p.introMusic -ne $false) { throw "test prefs are not silent: volume=$($p.volume) introMusic=$($p.introMusic)" }
$p | Add-Member -Force fgHudGuard ($Guard -in 'on', 'hud')
$p | Add-Member -Force fgFastGuard ($Guard -in 'on', 'fast')
[IO.File]::WriteAllText($prefs, ($p | ConvertTo-Json -Depth 8), (New-Object Text.UTF8Encoding $false))   # no BOM: Qt's JSON reader
New-Item -ItemType Directory -Force $Out | Out-Null
$log = Join-Path $App 'logs\veyra-qml.log'
$before = if (Test-Path $log) { (Get-Item $log).Length } else { 0 }
$psi = New-Object Diagnostics.ProcessStartInfo (Join-Path $App 'BooVI.exe'), "`"$Clip`" --preset fgonly --fg-backend $Backend --fg-motion $Motion --size 1280x800 --exit-after $($Seconds * 1000)"
$psi.UseShellExecute = $false; $psi.WorkingDirectory = $App
$psi.RedirectStandardOutput = $true; $psi.RedirectStandardError = $true
$psi.EnvironmentVariables['VEYRA_TEST_CAPTURE_REPLAY'] = '1'
$psi.EnvironmentVariables['VEYRA_TEST_REPLAY_AS_CARD'] = '1'
$psi.EnvironmentVariables['VEYRA_TEST_FG_FORCE_MULTIPLIER'] = '1'
if (-not $NoDump) {
    $dump = Join-Path $Out 'frames'
    if (Test-Path $dump) { Remove-Item -Recurse -Force $dump }
    $psi.EnvironmentVariables['VEYRA_TEST_FG_DUMP'] = $dump
    $psi.EnvironmentVariables['VEYRA_TEST_FG_DUMP_EVERY'] = "$Every"
}
foreach ($pair in $Env) { $name, $value = $pair -split '=', 2; $psi.EnvironmentVariables[$name] = $value }
$proc = [Diagnostics.Process]::Start($psi)
$proc.BeginOutputReadLine(); $proc.BeginErrorReadLine()
"pid $($proc.Id)"
if (-not $proc.WaitForExit(($Seconds + 40) * 1000)) { Stop-Process -Id $proc.Id -Force; 'killed after timeout' }
$fs = [IO.File]::Open($log, 'Open', 'Read', 'ReadWrite'); if ($fs.Length -lt $before) { $before = 0 }   # the log rotated
[void]$fs.Seek($before, 'Begin')
$new = (New-Object IO.StreamReader($fs)).ReadToEnd(); $fs.Close()
$new | Set-Content -Path (Join-Path $Out 'run.log') -Encoding utf8
$new -split "`n" | Where-Object { $_ -cmatch 'fg-dump|FG motion|fg-guard|ui-preset|ERROR|frame generation|fg-backend: Create' } | ForEach-Object { $_.Trim().Substring(0, [Math]::Min(220, $_.Trim().Length)) } | Select-Object -First 30
