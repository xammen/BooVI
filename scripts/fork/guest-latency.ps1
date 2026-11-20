# Shares a local video from a staged fork build, joins it with a headless Edge over loopback
# (guest-probe.ps1) and prints the host's and the page's latency lines for that run.
# Usage: scripts\fork\guest-latency.ps1 -App <app dir> -Video <file> [-Seconds 30] [-Port 47810]
#        [-Live] [-Env NAME=VALUE,...] [-Query 'render=canvas'] [-Eval <js>] [-Shot <png>] [-ShotAt <s>] [-Out <log copy>]
#   -Live   the file runs the live scheduler like a capture card (VEYRA_TEST_CAPTURE_REPLAY and
#           VEYRA_TEST_REPLAY_AS_CARD): frame generation then holds each real picture as it does
#           for a console stream
#   -Query  added to the guest link (the page's own switches)
# The share always runs in the tailnet reach (VEYRA_GUEST_REACH=tailnet): never the named tunnel.
# Only the processes started here are stopped.
param([Parameter(Mandatory)][string]$App, [Parameter(Mandatory)][string]$Video, [int]$Seconds = 30, [int]$Port = 47810,
      [switch]$Live, [string[]]$Env = @(), [string]$Query = '', [string]$Eval = '', [string]$Shot = '', [int]$ShotAt = 0, [string]$Out = '', [int]$DevtoolsPort = 9343)
$ErrorActionPreference = 'Stop'
$log = Join-Path $App 'logs\veyra-qml.log'
$before = if (Test-Path $log) { (Get-Item $log).Length } else { 0 }
$linkFile = Join-Path $env:TEMP "boovi-guest-link-$PID.txt"
Remove-Item $linkFile -ErrorAction SilentlyContinue
$psi = New-Object Diagnostics.ProcessStartInfo (Join-Path $App 'BooVI.exe'), "--share `"$Video`" --size 1280x800 --exit-after $(($Seconds + 25) * 1000)"
$psi.UseShellExecute = $false; $psi.WorkingDirectory = $App
$psi.RedirectStandardOutput = $true; $psi.RedirectStandardError = $true   # the app also logs to its console
$psi.EnvironmentVariables['VEYRA_GUEST_LINK_FILE'] = $linkFile
$psi.EnvironmentVariables['VEYRA_GUEST_PORT'] = "$Port"
$psi.EnvironmentVariables['VEYRA_GUEST_REACH'] = 'tailnet'
if ($Live) { $psi.EnvironmentVariables['VEYRA_TEST_CAPTURE_REPLAY'] = '1'; $psi.EnvironmentVariables['VEYRA_TEST_REPLAY_AS_CARD'] = '1' }
foreach ($pair in $Env) { $name, $value = $pair -split '=', 2; $psi.EnvironmentVariables[$name] = $value }
$p = [Diagnostics.Process]::Start($psi)
$p.BeginOutputReadLine(); $p.BeginErrorReadLine()
try {
    for ($i = 0; $i -lt 100 -and -not (Test-Path $linkFile); $i++) { Start-Sleep -Milliseconds 200 }
    if (-not (Test-Path $linkFile)) { throw 'the app wrote no guest link' }
    $link = @(Get-Content $linkFile)[0].Trim()   # first line: the loopback link
    if ($link -notmatch '^http://127\.0\.0\.1:') { throw 'no loopback link' }
    if ($Query) { $link += '&' + $Query }
    Start-Sleep -Seconds 3
    $probe = @{ Url = $link; Seconds = $Seconds; Port = $DevtoolsPort; Test = $true }
    if ($Eval) { $probe.Eval = $Eval }
    if ($Shot) { $probe.Shot = $Shot; $probe.ShotAt = $ShotAt }
    # The link carries the share token: the probe's output shows the page's numbers only.
    & (Join-Path $PSScriptRoot 'guest-probe.ps1') @probe
} finally {
    Remove-Item $linkFile -ErrorAction SilentlyContinue
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
}
$fs = [IO.File]::Open($log, 'Open', 'Read', 'ReadWrite'); [void]$fs.Seek($before, 'Begin')
$new = (New-Object IO.StreamReader($fs)).ReadToEnd(); $fs.Close()
if ($Out) { $new -replace '([?&]t=)[A-Za-z0-9_-]+', '$1<token>' | Set-Content -Path $Out -Encoding utf8 }
$new -split "`n" | Where-Object { $_ -cmatch '\[guest-tap\]|\[guest-latency\]|\[guest-video\]|\[capture-timing\]|\[player-timing\]|ERROR' } |
    ForEach-Object { $line = $_.Trim() -replace 'gpuColorP95Ms.*', ''; $line.Substring(0, [Math]::Min(360, $line.Length)) } | Select-Object -Last 12
