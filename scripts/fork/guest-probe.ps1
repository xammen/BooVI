# Opens the partage guest page in a headless Edge (its own profile, no window, no focus taken),
# lets it join, prints the page's own numbers every 2 s and saves a screenshot.
# Driven over the DevTools protocol with .NET's WebSocket: nothing is installed.
# Usage: scripts\fork\guest-probe.ps1 -Url <guest link> [-Seconds 30] [-Shot <png>] [-Test] [-ShotAt 20] [-Eval <js>] [-Quiet]
#   -Eval   a JavaScript expression printed once at the end (the page's own measurements)
#   -Quiet  no line every 2 s
#   -Test   the page's synthetic pad sweep (the right stick turns; no button is pressed)
#   -Relay  the page gathers relay candidates only (iceTransportPolicy "relay"): proves the TURN fallback
#   -Init <js>       runs before the page's own script (scripts\fork\guest-pad-fake.js fakes a pad; pick its scenario with a #fake=… on -Url)
#   -ShotWhen <js>   the screenshot is taken once this expression is true, instead of at -ShotAt
param([Parameter(Mandatory)][string]$Url, [int]$Seconds = 30, [string]$Shot = '', [switch]$Test, [switch]$Relay, [int]$ShotAt = 0, [string]$Init = '', [string]$ShotWhen = '', [string]$Eval = '', [switch]$Quiet,
      [string]$Profile = (Join-Path $env:TEMP 'boovi-guest-probe'), [int]$Port = 9333,
      [string]$Edge = 'C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe', [string[]]$Flags = @())
$ErrorActionPreference = 'Stop'
$hash = if ($Url.Contains('#')) { $Url.Substring($Url.IndexOf('#')) } else { '' }
$base = if ($hash) { $Url.Substring(0, $Url.IndexOf('#')) } else { $Url }
$target = $base + $(if ($base.Contains('?')) { '&' } else { '?' }) + 'autojoin=1' + $(if ($Test) { '&test=1' } else { '' }) + $(if ($Relay) { '&relay=1' } else { '' }) + $hash
$args = @('--headless=new', "--remote-debugging-port=$Port", "--user-data-dir=$Profile", '--no-first-run', '--no-default-browser-check',
          '--autoplay-policy=no-user-gesture-required', '--window-size=1280,720', '--mute-audio') + $Flags + @('about:blank')
$edgeProc = Start-Process -FilePath $Edge -ArgumentList $args -PassThru -WindowStyle Hidden
try {
    $page = $null
    for ($i = 0; $i -lt 50 -and -not $page; $i++) {
        Start-Sleep -Milliseconds 200
        try { $page = (Invoke-RestMethod "http://127.0.0.1:$Port/json") | Where-Object { $_.type -eq 'page' } | Select-Object -First 1 } catch {}
    }
    if (-not $page) { throw 'edge devtools did not answer' }
    $ws = New-Object Net.WebSockets.ClientWebSocket
    $ws.ConnectAsync([Uri]$page.webSocketDebuggerUrl, [Threading.CancellationToken]::None).Wait()
    $script:id = 0
    function Invoke-Cdp([string]$method, [hashtable]$params = @{}) {
        $script:id++
        $msg = @{ id = $script:id; method = $method; params = $params } | ConvertTo-Json -Depth 6 -Compress
        $bytes = [Text.Encoding]::UTF8.GetBytes($msg)
        $ws.SendAsync([ArraySegment[byte]]::new($bytes), 'Text', $true, [Threading.CancellationToken]::None).Wait()
        $buffer = New-Object byte[] (1 -shl 20)
        while ($true) {
            $sb = New-Object Text.StringBuilder
            do {
                $r = $ws.ReceiveAsync([ArraySegment[byte]]::new($buffer), [Threading.CancellationToken]::None).Result
                [void]$sb.Append([Text.Encoding]::UTF8.GetString($buffer, 0, $r.Count))
            } while (-not $r.EndOfMessage)
            $reply = $sb.ToString() | ConvertFrom-Json
            if ($reply.id -eq $script:id) { return $reply }
        }
    }
    function Eval([string]$expr) { (Invoke-Cdp 'Runtime.evaluate' @{ expression = $expr; returnByValue = $true; awaitPromise = $true }).result.result.value }
    [void](Invoke-Cdp 'Page.enable')
    if ($Init) { [void](Invoke-Cdp 'Page.addScriptToEvaluateOnNewDocument' @{ source = [IO.File]::ReadAllText($Init) }) }
    [void](Invoke-Cdp 'Page.navigate' @{ url = $target })
    $start = Get-Date
    $shotDone = $false
    while (((Get-Date) - $start).TotalSeconds -lt $Seconds) {
        Start-Sleep -Seconds 2
        $t = [int]((Get-Date) - $start).TotalSeconds
        $state = Eval "JSON.stringify({gate: document.getElementById('gateText').innerText, hud: document.getElementById('hudState').innerText, pad: document.getElementById('padText').innerText, fake: window.__fakeStatus, kv: kv})"
        if (-not $Quiet) { "t=$t $state" }
        if ($Shot -and -not $shotDone -and (($ShotWhen -and (Eval "!!($ShotWhen)")) -or (-not $ShotWhen -and $ShotAt -gt 0 -and $t -ge $ShotAt))) {
            $png = (Invoke-Cdp 'Page.captureScreenshot' @{ format = 'png' }).result.data
            [IO.File]::WriteAllBytes($Shot, [Convert]::FromBase64String($png)); "shot $Shot"; $shotDone = $true
        }
    }
    if ($Shot -and -not $shotDone) {
        $png = (Invoke-Cdp 'Page.captureScreenshot' @{ format = 'png' }).result.data
        [IO.File]::WriteAllBytes($Shot, [Convert]::FromBase64String($png)); "shot $Shot"
    }
    if ($Eval) { "eval $(Eval $Eval)" }
    try { [void](Invoke-Cdp 'Browser.close') } catch {}
} finally {
    Start-Sleep -Milliseconds 500
    if (-not $edgeProc.HasExited) { Stop-Process -Id $edgeProc.Id -Force -ErrorAction SilentlyContinue }
}
