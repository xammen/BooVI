# Partage: stores the Cloudflare secrets BooVI needs to share through a named tunnel and a TURN
# relay, encrypted with DPAPI for the current Windows user, in %LOCALAPPDATA%\BooVI\guest\cloudflare.dpapi
# (read by include\veyra\guest\GuestCloudflare.h). Reads one JSON object on stdin, never argv:
#   {"hostname": "play.example.com", "tunnelToken": "...", "turnKeyId": "...", "turnApiToken": "..."}
# Prints nothing secret. Usage: <json> | powershell -NoProfile -File scripts\fork\guest-cf-secrets.ps1
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Security
$text = [Console]::In.ReadToEnd()
$json = $text | ConvertFrom-Json
foreach ($key in 'hostname', 'tunnelToken', 'turnKeyId', 'turnApiToken') {
    if (-not $json.$key) { throw "missing $key" }
}
$root = if ($env:BOOVI_USER_DATA) { $env:BOOVI_USER_DATA } else { Join-Path $env:LOCALAPPDATA 'BooVI' }
$dir = Join-Path $root 'guest'
New-Item -ItemType Directory -Force $dir | Out-Null
$plain = [Text.Encoding]::UTF8.GetBytes($text.Trim())
$sealed = [Security.Cryptography.ProtectedData]::Protect($plain, $null, 'CurrentUser')
[Array]::Clear($plain, 0, $plain.Length)
[IO.File]::WriteAllBytes((Join-Path $dir 'cloudflare.dpapi'), $sealed)
"stored for $($json.hostname) ($($sealed.Length) bytes, dpapi current user)"
