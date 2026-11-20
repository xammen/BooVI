# Deploys the broker Worker "boovi" (wrangler.jsonc) on a custom domain of a zone in your account.
# The domain is never written in the repository: pass it here. Prints nothing secret.
# Usage: broker\scripts\deploy.ps1 -Domain go.example.com [-Var TURN_BUDGET_GB:900 ...]
param([Parameter(Mandatory)][string]$Domain, [string[]]$Var = @(), [switch]$DryRun)
$ErrorActionPreference = 'Stop'
$broker = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'account.ps1')
$env:CLOUDFLARE_ACCOUNT_ID = Get-ZoneAccount ($Domain -replace '^[^.]+\.', '')
Push-Location $broker
try {
    node scripts/assets.mjs
    $wargs = @('wrangler', 'deploy', '--domain', $Domain)
    foreach ($v in $Var) { $wargs += @('--var', $v) }
    if ($DryRun) { $wargs += '--dry-run' }
    # wrangler names the account in its output: only its own outcome lines are shown.
    & npx @wargs 2>&1 | ForEach-Object { "$_" } | Where-Object { $_ -match 'Uploaded|Deployed|Current Version|custom domain|error|ERROR|✘|Total Upload|Your Worker has access|env\.' } |
        ForEach-Object { $_ -replace '[0-9a-f]{32}', '<id>' -replace '[\w.+-]+@[\w-]+\.[\w.]+', '<email>' }
    if ($LASTEXITCODE) { throw "wrangler deploy failed ($LASTEXITCODE)" }
} finally { Pop-Location }
