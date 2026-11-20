# Runs one real PS5 session with a staged fork build and prints its connection timeline:
# app start, Play, wake, session start, login PIN, first decoded frame, controller timing.
# Usage: scripts\fork\session-test.ps1 -App <app dir> [-Seconds 45] [-PinFile <file>] [-Env NAME=VALUE,...]
#        [-Extra <app flags>] [-Shot <png> [-ShotAfter <log regex>] [-ShotDelay 10]]
# -Env sets variables for the app only, for example -Env VEYRA_TEST_UI_STALL=200,2000 is
# written as -Env 'VEYRA_TEST_UI_STALL=200,2000'.
# -Shot saves a PrintWindow PNG of the app ShotDelay seconds after the first log line that
# matches ShotAfter (default: the decoder opening on the first frame).
param([Parameter(Mandatory)][string]$App, [int]$Seconds = 45, [string]$PinFile = '', [string[]]$Env = @(),
      [string[]]$Extra = @(), [string]$Shot = '', [string]$ShotAfter = 'D3D12VA decoder opened', [int]$ShotDelay = 10)
$log = Join-Path $App 'logs\veyra-qml.log'
$before = if (Test-Path $log) { (Get-Item $log).Length } else { 0 }
$args = @('--play-last', '--exit-after', ($Seconds * 1000), '--size', '1280x800')
if ($PinFile) { $args += @('--login-pin-file', $PinFile) }
$args += $Extra
# ProcessStartInfo, not Start-Process: a shell-executed child did not get these variables.
$psi = New-Object Diagnostics.ProcessStartInfo (Join-Path $App 'BooVI.exe'), ($args -join ' ')
$psi.UseShellExecute = $false; $psi.WorkingDirectory = $App
foreach ($pair in $Env) { $name, $value = $pair -split '=', 2; $psi.EnvironmentVariables[$name] = $value }
$p = [Diagnostics.Process]::Start($psi)
function Read-NewLog { $fs = [IO.File]::Open($log, 'Open', 'Read', 'ReadWrite'); [void]$fs.Seek($before, 'Begin'); $t = (New-Object IO.StreamReader($fs)).ReadToEnd(); $fs.Close(); $t }
if ($Shot) {
    Add-Type -AssemblyName System.Drawing
    Add-Type @'
using System; using System.Runtime.InteropServices;
public static class SessionShot {
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
}
'@
    $seen = $false
    while (-not $p.HasExited -and -not $seen) { Start-Sleep -Milliseconds 250; $seen = (Read-NewLog) -match $ShotAfter }
    if ($seen) {
        Start-Sleep -Seconds $ShotDelay
        $p.Refresh(); $h = $p.MainWindowHandle
        $r = New-Object SessionShot+RECT; [void][SessionShot]::GetWindowRect($h, [ref]$r)
        $bmp = New-Object System.Drawing.Bitmap ($r.R - $r.L), ($r.B - $r.T)
        $g = [System.Drawing.Graphics]::FromImage($bmp); $dc = $g.GetHdc()
        $ok = [SessionShot]::PrintWindow($h, $dc, 2); $g.ReleaseHdc($dc); $g.Dispose()
        $bmp.Save($Shot, [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
        "shot printed=$ok at $(Get-Date -Format 'HH:mm:ss.ff') out=$Shot"
    } else { "shot skipped: '$ShotAfter' never logged" }
}
[void]$p.WaitForExit(($Seconds + 60) * 1000)
$new = Read-NewLog
$pattern = 'QML session started|ui-resume-source|remoteplay-wake|remoteplay\] session started|login PIN|D3D12VA decoder opened|source-state session=\d+ transport=2|remoteplay-input|input-timing|ERROR|recovery stopped|goto bed|sleep-ps5'
$new -split "`n" | Where-Object { $_ -cmatch $pattern } | ForEach-Object { $_.Substring(0, [Math]::Min(190, $_.Length)) }
exit 0
