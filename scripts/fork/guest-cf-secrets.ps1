# Partage: stores this PC's sharing secrets, encrypted with DPAPI for the current Windows user, in
# %LOCALAPPDATA%\BooVI\guest\cloudflare.dpapi (BOOVI_USER_DATA's guest folder when set; read by
# src\guest\GuestCloudflare.h). Either or both:
#   ownerKey                   the broker's owner key: the broker lends its TURN relay to this PC
#   turnKeyId, turnApiToken    your own Cloudflare TURN key, used when there is no owner key
# Reads one JSON object on stdin, never argv, or the owner key alone from -OwnerKeyFile (the file
# broker\scripts\put-secrets.ps1 sealed). Keys already stored and not given are kept; keys of
# older versions (hostname, tunnelToken) are dropped. Prints nothing secret.
# Usage: <json> | powershell -NoProfile -File scripts\fork\guest-cf-secrets.ps1
#        powershell -NoProfile -File scripts\fork\guest-cf-secrets.ps1 -OwnerKeyFile <file>
param([string]$OwnerKeyFile = '')
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Security
function Unseal([string]$path) {
    $plain = [Security.Cryptography.ProtectedData]::Unprotect([IO.File]::ReadAllBytes($path), $null, 'CurrentUser')
    try { return [Text.Encoding]::UTF8.GetString($plain) } finally { [Array]::Clear($plain, 0, $plain.Length) }
}
$root = if ($env:BOOVI_USER_DATA) { $env:BOOVI_USER_DATA } else { Join-Path $env:LOCALAPPDATA 'BooVI' }
$file = Join-Path $root 'guest\cloudflare.dpapi'
$given = if ($OwnerKeyFile) { @{ ownerKey = Unseal $OwnerKeyFile } } else { [Console]::In.ReadToEnd() | ConvertFrom-Json }
$stored = if (Test-Path $file) { Unseal $file | ConvertFrom-Json } else { $null }
$config = [ordered]@{}
foreach ($key in 'ownerKey', 'turnKeyId', 'turnApiToken') {
    $value = if ($given.$key) { $given.$key } elseif ($stored -and $stored.$key) { $stored.$key } else { $null }
    if ($value) { $config[$key] = [string]$value }
}
if (-not $config.ownerKey -and -not ($config.turnKeyId -and $config.turnApiToken)) { throw 'give ownerKey, or turnKeyId and turnApiToken' }
New-Item -ItemType Directory -Force (Split-Path -Parent $file) | Out-Null
$plain = [Text.Encoding]::UTF8.GetBytes(($config | ConvertTo-Json -Compress))
$sealed = [Security.Cryptography.ProtectedData]::Protect($plain, $null, 'CurrentUser')
[Array]::Clear($plain, 0, $plain.Length)
[IO.File]::WriteAllBytes($file, $sealed)
"stored: $(@($config.Keys) -join ', ') ($($sealed.Length) bytes, dpapi current user)"
