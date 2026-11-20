# The id of the Cloudflare account that holds a zone, read with the cf CLI and never printed.
function Get-ZoneAccount([Parameter(Mandatory)][string]$Zone) {
    $out = (& cf zones list --name $Zone 2>$null) -join "`n"
    $json = $out.Substring($out.IndexOf('['))
    $zones = $json | ConvertFrom-Json
    if (-not $zones -or -not $zones[0].account.id) { throw "no zone $Zone in this login" }
    return $zones[0].account.id
}
