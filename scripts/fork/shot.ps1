# Starts a Veyra build, waits, saves a PNG of its main window (PrintWindow, so other windows
# on top do not matter), and lets the app close itself through --exit-after.
# Usage: scripts\fork\shot.ps1 -App <dir with BooVI.exe> -Out <png> [-Page home] [-Delay 5] [-Dialog ps5] [-Size 1920x1080] [-Extra <switch>]
param([Parameter(Mandatory)][string]$App, [Parameter(Mandatory)][string]$Out, [string]$Page = 'home', [int]$Delay = 5, [string]$Dialog = '', [string]$Size = '1280x800', [string[]]$Extra = @())
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System; using System.Runtime.InteropServices;
public static class Shot {
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
}
'@
$p = Start-Process -FilePath (Join-Path $App 'BooVI.exe') -WorkingDirectory $App -PassThru `
     -ArgumentList (@('--exit-after', (($Delay + 4) * 1000), '--page', $Page, '--size', $Size, '--reduced-motion') + $Extra + $(if ($Dialog) { @('--dialog', $Dialog) } else { @() }))
Start-Sleep -Seconds $Delay
$p.Refresh()
$h = $p.MainWindowHandle
if ($h -eq [IntPtr]::Zero) { Write-Error 'no main window'; exit 1 }
$r = New-Object Shot+RECT; [void][Shot]::GetWindowRect($h, [ref]$r)
$bmp = New-Object System.Drawing.Bitmap ($r.R - $r.L), ($r.B - $r.T)
$g = [System.Drawing.Graphics]::FromImage($bmp); $dc = $g.GetHdc()
$ok = [Shot]::PrintWindow($h, $dc, 2); $g.ReleaseHdc($dc); $g.Dispose()
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
[void]$p.WaitForExit(30000)
"printed=$ok size=$($r.R - $r.L)x$($r.B - $r.T) out=$Out"
