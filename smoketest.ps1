# ==============================================================================
# DRAWBRIDGE — an ENGAGE product
# Windows host-binary smoke test (pure PowerShell 5+/7, no dependencies)
#
# Starts SipServer.exe, probes the HTTP API on :8080 (checks ported from
# tests/http/test_api.sh) and sends a SIP OPTIONS over UDP to :5060.
# Exit code 0 only if every check passes.
# ==============================================================================
[CmdletBinding()]
param(
    [string]$ExePath
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path

# ── Locate the binary ─────────────────────────────────────────────────────────
if (-not $ExePath) {
    $candidates = @(
        (Join-Path $repoRoot 'build-audit\Release\SipServer.exe'),
        (Join-Path $repoRoot 'build\Release\SipServer.exe')
    )
    $ExePath = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
}
if (-not $ExePath -or -not (Test-Path $ExePath)) {
    Write-Host 'ERROR: SipServer.exe not found.' -ForegroundColor Red
    Write-Host '  Searched: build-audit\Release\SipServer.exe and build\Release\SipServer.exe'
    Write-Host '  Build it first (with IDF_PATH unset):'
    Write-Host '      cmake -B build -S .'
    Write-Host '      cmake --build build --config Release'
    exit 2
}

$HostIp  = '127.0.0.1'
$SipPort = 5060
$WebPort = 8080
$BaseUrl = "http://${HostIp}:${WebPort}"

Write-Host '======================================================================' -ForegroundColor Cyan
Write-Host '       DRAWBRIDGE — an ENGAGE product :: Windows smoke test'            -ForegroundColor Cyan
Write-Host '======================================================================' -ForegroundColor Cyan
Write-Host "  Binary : $ExePath"
Write-Host "  Target : $BaseUrl (HTTP), ${HostIp}:${SipPort} (SIP/UDP)"
Write-Host ''

# ── Check bookkeeping ─────────────────────────────────────────────────────────
$script:Passed = 0
$script:Failed = 0
function Report([string]$Name, [bool]$Ok, [string]$Detail = '') {
    if ($Ok) {
        Write-Host '  [PASS] ' -ForegroundColor Green -NoNewline
        $script:Passed++
    } else {
        Write-Host '  [FAIL] ' -ForegroundColor Red -NoNewline
        $script:Failed++
    }
    Write-Host $Name -NoNewline
    if ($Detail) { Write-Host "  ($Detail)" -ForegroundColor DarkGray } else { Write-Host '' }
}

# HTTP GET that never throws on non-2xx; returns @{ Code; Body }.
function Http-Get([string]$Url) {
    try {
        $resp = Invoke-WebRequest -Uri $Url -UseBasicParsing -TimeoutSec 5
        return @{ Code = [int]$resp.StatusCode; Body = [string]$resp.Content }
    } catch {
        $r = $_.Exception.Response
        if ($r -and $r.StatusCode) {
            $code = [int]$r.StatusCode
            $body = ''
            try {
                $stream = $r.GetResponseStream()
                if ($stream) { $body = (New-Object System.IO.StreamReader($stream)).ReadToEnd() }
            } catch {}
            return @{ Code = $code; Body = $body }
        }
        return @{ Code = 0; Body = $_.Exception.Message }
    }
}

# ── Start the server ──────────────────────────────────────────────────────────
$stamp  = Get-Date -Format 'yyyyMMdd-HHmmss'
$outLog = Join-Path $env:TEMP "drawbridge-smoke-$stamp-out.log"
$errLog = Join-Path $env:TEMP "drawbridge-smoke-$stamp-err.log"

Write-Host "Starting server (logs: $outLog)" -ForegroundColor Yellow
$proc = Start-Process -FilePath $ExePath `
    -ArgumentList '--ip', $HostIp, '--port', $SipPort, '--web', $WebPort `
    -RedirectStandardOutput $outLog -RedirectStandardError $errLog `
    -PassThru -WindowStyle Hidden

try {
    # ── Check 1: TCP 8080 comes up within 10 s ────────────────────────────────
    $up = $false
    $deadline = (Get-Date).AddSeconds(10)
    while ((Get-Date) -lt $deadline) {
        if ($proc.HasExited) { break }
        $tcp = New-Object System.Net.Sockets.TcpClient
        try {
            $async = $tcp.BeginConnect($HostIp, $WebPort, $null, $null)
            if ($async.AsyncWaitHandle.WaitOne(500) -and $tcp.Connected) { $up = $true }
        } catch {} finally { $tcp.Close() }
        if ($up) { break }
        Start-Sleep -Milliseconds 250
    }
    Report "Web server listening on TCP $WebPort within 10s" $up `
        $(if ($proc.HasExited) { "process exited early (code $($proc.ExitCode))" } else { '' })

    if ($up) {
        Write-Host ''
        Write-Host 'HTTP API checks (ported from tests/http/test_api.sh):' -ForegroundColor Cyan

        # ── Check 2: TC-HP-01 — landing dashboard ────────────────────────────
        $r = Http-Get "$BaseUrl/"
        Report 'GET / -> 200 with HTML dashboard' `
            ($r.Code -eq 200 -and $r.Body -match '(?i)<html') "got $($r.Code)"

        # ── Check 3: TC-HP-02 — /api/status JSON snapshot ────────────────────
        $r = Http-Get "$BaseUrl/api/status"
        $schemaOk = ($r.Body -like '*uptime*') -and ($r.Body -like '*packetsProcessed*') -and ($r.Body -like '*clients*')
        Report 'GET /api/status -> 200 with uptime/packetsProcessed/clients schema' `
            ($r.Code -eq 200 -and $schemaOk) "got $($r.Code)"

        # ── Check 4: TC-OTA-01 — /api/ota/status ungated introspection ───────
        $r = Http-Get "$BaseUrl/api/ota/status"
        Report 'GET /api/ota/status -> 200 with otaSupported + running fields' `
            ($r.Code -eq 200 -and $r.Body -like '*"otaSupported"*' -and $r.Body -like '*"running"*') "got $($r.Code)"

        # ── Check 5: TC-AUTH-01 — /api/admin/status reachable, unprovisioned ─
        $r = Http-Get "$BaseUrl/api/admin/status"
        Report 'GET /api/admin/status -> 200 reporting provisioned:false' `
            ($r.Code -eq 200 -and $r.Body -like '*"provisioned":false*') "got $($r.Code)"

        # ── Check 6: TC-ED-04 — unknown API route -> 404 ─────────────────────
        $r = Http-Get "$BaseUrl/api/invalid-route-name-xyz"
        Report 'GET /api/invalid-route-name-xyz -> 404' ($r.Code -eq 404) "got $($r.Code)"

        # ── Check 7: SIP OPTIONS over UDP -> any SIP/2.0 reply within 3 s ────
        Write-Host ''
        Write-Host 'SIP signaling check:' -ForegroundColor Cyan
        $sipOk = $false
        $detail = ''
        $udp = New-Object System.Net.Sockets.UdpClient
        try {
            $udp.Client.ReceiveTimeout = 3000
            $udp.Connect($HostIp, $SipPort)
            $localPort = ([System.Net.IPEndPoint]$udp.Client.LocalEndPoint).Port
            $branch = 'z9hG4bK' + ([guid]::NewGuid().ToString('N').Substring(0, 12))
            $tag    = ([guid]::NewGuid().ToString('N').Substring(0, 8))
            $callId = ([guid]::NewGuid().ToString('N'))
            $crlf = "`r`n"
            $msg = "OPTIONS sip:${HostIp}:${SipPort} SIP/2.0" + $crlf +
                   "Via: SIP/2.0/UDP ${HostIp}:${localPort};branch=$branch" + $crlf +
                   "Max-Forwards: 70" + $crlf +
                   "From: <sip:smoketest@${HostIp}>;tag=$tag" + $crlf +
                   "To: <sip:${HostIp}:${SipPort}>" + $crlf +
                   "Call-ID: $callId@${HostIp}" + $crlf +
                   "CSeq: 1 OPTIONS" + $crlf +
                   "Contact: <sip:smoketest@${HostIp}:${localPort}>" + $crlf +
                   "User-Agent: drawbridge-smoketest" + $crlf +
                   "Content-Length: 0" + $crlf + $crlf
            $bytes = [System.Text.Encoding]::ASCII.GetBytes($msg)
            [void]$udp.Send($bytes, $bytes.Length)
            $remote = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, 0)
            $replyBytes = $udp.Receive([ref]$remote)
            $reply = [System.Text.Encoding]::ASCII.GetString($replyBytes)
            if ($reply.StartsWith('SIP/2.0')) {
                $sipOk = $true
                $detail = ($reply -split "`r`n")[0]
            } else {
                $detail = 'reply did not start with SIP/2.0'
            }
        } catch {
            $detail = "no SIP response within 3s: $($_.Exception.Message)"
        } finally {
            $udp.Close()
        }
        Report 'SIP OPTIONS over UDP 5060 -> SIP/2.0 response within 3s' $sipOk $detail
    } else {
        Write-Host '  Skipping HTTP and SIP checks (server never came up).' -ForegroundColor Yellow
        if (Test-Path $errLog) {
            Write-Host '  --- stderr tail ---' -ForegroundColor DarkGray
            Get-Content $errLog -Tail 20 | ForEach-Object { Write-Host "  $_" -ForegroundColor DarkGray }
        }
    }
}
finally {
    if ($proc -and -not $proc.HasExited) {
        Write-Host ''
        Write-Host "Stopping server (PID $($proc.Id))..." -ForegroundColor Yellow
        Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
        try { $proc.WaitForExit(5000) | Out-Null } catch {}
    }
}

# ── Summary ───────────────────────────────────────────────────────────────────
Write-Host ''
Write-Host '======================================================================' -ForegroundColor Cyan
Write-Host "  Total: $($script:Passed + $script:Failed)   Passed: " -NoNewline
Write-Host $script:Passed -ForegroundColor Green -NoNewline
Write-Host '   Failed: ' -NoNewline
if ($script:Failed -eq 0) {
    Write-Host '0' -ForegroundColor Green
    Write-Host '>>> SMOKE TEST PASSED' -ForegroundColor Green
    exit 0
} else {
    Write-Host $script:Failed -ForegroundColor Red
    Write-Host '>>> SMOKE TEST FAILED' -ForegroundColor Red
    exit 1
}
