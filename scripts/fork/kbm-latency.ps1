# Measures mouse-to-PS5 latency on a live session: starts the staged app on the last PS5 with
# VEYRA_KBM_TRACE on, moves the mouse, then joins the moves with the trace (kbm-latency.py).
# Default: the keyboard/mouse are captured (--ps5-kbm) and KbmSweep.exe injects constant-speed
# sweeps into the window with SendInput, which takes the focus and the cursor while it runs.
# -Synthetic: no capture and no SendInput; the app's input thread moves a model mouse itself
# (VEYRA_KBM_SWEEP), so the focus and the desktop cursor stay with whoever uses the PC.
# Holds the shared console lock (%USERPROFILE%\dev\ps5-session.lock) for the whole session, and
# writes the round trip to the console (never its address) to network.txt.
# Usage: scripts\fork\kbm-latency.ps1 -App <app dir> -Out <dir> [-Seconds 50] [-Sweep '--speeds 500,2000']
#        [-Synthetic] [-Prefs <qml-preferences json to use for this run>]
param([Parameter(Mandatory)][string]$App, [Parameter(Mandatory)][string]$Out, [int]$Seconds = 50,
      [string]$Sweep = '', [switch]$Synthetic, [string]$Prefs = '')
$ErrorActionPreference = 'Stop'
$fork = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$sweepExe = Join-Path $fork 'build-fork\KbmSweep.exe'
$lock = Join-Path $env:USERPROFILE 'dev\ps5-session.lock'
New-Item -ItemType Directory -Force $Out | Out-Null
while ($true) {
    if ((Test-Path $lock) -and ((Get-Date) - (Get-Item $lock).LastWriteTime).TotalMinutes -gt 10) { Remove-Item $lock -Force }
    try { New-Item -ItemType File $lock -Value "kbm $(Get-Date -Format o)" -ErrorAction Stop | Out-Null; break }
    catch { "lock held: $(Get-Content $lock -ErrorAction SilentlyContinue); waiting"; Start-Sleep -Seconds 20 }
}
$prefFile = Join-Path $App 'runtime_local\user-data-2.0.0\qml-preferences.v1.json'
$saved = $null
try {
    if ($Prefs) { $saved = "$prefFile.kbm-latency"; Copy-Item $prefFile $saved -Force; Copy-Item $Prefs $prefFile -Force }
    $log = Join-Path $App 'logs\veyra-qml.log'
    $before = if (Test-Path $log) { (Get-Item $log).Length } else { 0 }
    $trace = Join-Path $Out 'trace.csv'; $moves = Join-Path $Out 'sweep.csv'
    $capture = if ($Synthetic) { '' } else { '--ps5-kbm ' }
    $psi = New-Object Diagnostics.ProcessStartInfo (Join-Path $App 'BooVI.exe'), "--play-last $capture--exit-after $($Seconds * 1000) --size 1280x800"
    $psi.UseShellExecute = $false; $psi.WorkingDirectory = $App
    $psi.EnvironmentVariables['VEYRA_KBM_TRACE'] = $trace
    if ($Synthetic) { $psi.EnvironmentVariables['VEYRA_KBM_SWEEP'] = $moves }
    $p = [Diagnostics.Process]::Start($psi)
    function Read-NewLog { if (-not (Test-Path $log)) { return '' }; $fs = [IO.File]::Open($log, 'Open', 'Read', 'ReadWrite'); [void]$fs.Seek($before, 'Begin'); $t = (New-Object IO.StreamReader($fs)).ReadToEnd(); $fs.Close(); $t }
    if ($Synthetic) { Start-Sleep -Seconds 25 }
    elseif (-not $p.HasExited) { & $sweepExe $p.Id $moves ($Sweep -split ' ' | Where-Object { $_ }) | Out-File (Join-Path $Out 'sweep.txt') -Encoding utf8 }
    # The network leg: round trips to the console the session is connected to (its control
    # connection, TCP 9295). Only the times are written, never the address.
    $console = Get-NetTCPConnection -OwningProcess $p.Id -RemotePort 9295 -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty RemoteAddress
    if ($console) {
        $ping = New-Object Net.NetworkInformation.Ping
        $rtt = 1..50 | ForEach-Object { $r = $ping.Send($console, 1000); if ($r.Status -eq 'Success') { $r.RoundtripTime } }
        if ($rtt) { "console round trip ms: min=$(($rtt | Measure-Object -Minimum).Minimum) avg=$([Math]::Round(($rtt | Measure-Object -Average).Average, 2)) max=$(($rtt | Measure-Object -Maximum).Maximum) n=$($rtt.Count)" | Out-File (Join-Path $Out 'network.txt') -Encoding utf8 }
    }
    [void]$p.WaitForExit(($Seconds + 60) * 1000)
    (Read-NewLog) -split "`n" | Where-Object { $_ -cmatch 'captured for the PS5|capture refused|input-timing|kbm trace|kbm sweep|ERROR' } |
        ForEach-Object { $_.Substring(0, [Math]::Min(260, $_.Length)) } | Out-File (Join-Path $Out 'log.txt') -Encoding utf8
} finally {
    if ($saved) { Move-Item $saved $prefFile -Force }
    Remove-Item $lock -Force -ErrorAction SilentlyContinue
}
if ((Test-Path $trace) -and (Test-Path $moves)) { py -I (Join-Path $PSScriptRoot 'kbm-latency.py') $moves $trace | Out-File (Join-Path $Out 'summary.txt') -Encoding utf8 }
