# Sets the broker's secrets, each read here and handed to `wrangler secret put` on its stdin:
#   TURN_KEY_ID, TURN_KEY_API_TOKEN  the owner's Cloudflare TURN key, from a sharing config that
#                                    scripts\fork\guest-cf-secrets.ps1 wrote (DPAPI, this Windows user)
#   OWNER_KEY                        the key that entitles a host to the broker's TURN relay: the one
#                                    sealed in -OwnerKeyFile, or a new one sealed there
# -OwnerKeyFile is what `scripts\fork\guest-cf-secrets.ps1 -OwnerKeyFile` merges into a PC's
# sharing config. Prints nothing secret.
# Usage: broker\scripts\put-secrets.ps1 -Zone example.com -OwnerKeyFile <path> [-Config <cloudflare.dpapi>] [-OwnerOnly]
param([Parameter(Mandatory)][string]$Zone, [Parameter(Mandatory)][string]$OwnerKeyFile, [string]$Config = '', [switch]$OwnerOnly)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Security
$broker = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'account.ps1')
$env:CLOUDFLARE_ACCOUNT_ID = Get-ZoneAccount $Zone

function Unseal([string]$path) {
    $plain = [Security.Cryptography.ProtectedData]::Unprotect([IO.File]::ReadAllBytes($path), $null, 'CurrentUser')
    try { return [Text.Encoding]::UTF8.GetString($plain) } finally { [Array]::Clear($plain, 0, $plain.Length) }
}
function Seal([string]$path, [string]$text) {
    $plain = [Text.Encoding]::UTF8.GetBytes($text)
    $sealed = [Security.Cryptography.ProtectedData]::Protect($plain, $null, 'CurrentUser')
    [Array]::Clear($plain, 0, $plain.Length)
    New-Item -ItemType Directory -Force (Split-Path -Parent $path) | Out-Null
    [IO.File]::WriteAllBytes($path, $sealed)
}
function Put([string]$name, [string]$value) {
    if (-not $value) { throw "no value for $name" }
    $psi = New-Object Diagnostics.ProcessStartInfo 'cmd.exe', "/c npx wrangler secret put $name"
    $psi.WorkingDirectory = $broker
    $psi.UseShellExecute = $false
    $psi.RedirectStandardInput = $true; $psi.RedirectStandardOutput = $true; $psi.RedirectStandardError = $true
    # The child's stdin takes Console.InputEncoding, whose UTF-8 would start with a byte order mark.
    $encoding = [Console]::InputEncoding
    [Console]::InputEncoding = New-Object Text.UTF8Encoding $false
    try { $p = [Diagnostics.Process]::Start($psi) } finally { [Console]::InputEncoding = $encoding }
    $p.StandardInput.Write($value)
    $p.StandardInput.Close()
    $out = $p.StandardOutput.ReadToEnd() + $p.StandardError.ReadToEnd()
    $p.WaitForExit()
    if ($p.ExitCode -ne 0 -or $out -notmatch 'Success') { throw "wrangler secret put $name failed ($($p.ExitCode))" }
    "secret $name set"
}

# The owner key: the sealed one, or a new one (33 random bytes, base64url).
if (Test-Path $OwnerKeyFile) { $owner = Unseal $OwnerKeyFile }
else {
    $bytes = New-Object byte[] 33
    [Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($bytes)
    $owner = [Convert]::ToBase64String($bytes).Replace('+', '-').Replace('/', '_').TrimEnd('=')
    [Array]::Clear($bytes, 0, $bytes.Length)
    Seal $OwnerKeyFile $owner
    "new owner key sealed in $OwnerKeyFile"
}
Put 'OWNER_KEY' $owner
if ($OwnerOnly) { return }

# The TURN key: from the sharing config (BooVI's, else the one Veyra left before BooVI copied it).
if (-not $Config) {
    $Config = @((Join-Path $env:LOCALAPPDATA 'BooVI\guest\cloudflare.dpapi'), (Join-Path $env:LOCALAPPDATA 'Veyra\guest\cloudflare.dpapi')) |
        Where-Object { Test-Path $_ } | Select-Object -First 1
}
if (-not $Config) { throw 'no sharing config with a TURN key on this PC' }
$json = (Unseal $Config) | ConvertFrom-Json
Put 'TURN_KEY_ID' $json.turnKeyId
Put 'TURN_KEY_API_TOKEN' $json.turnApiToken
