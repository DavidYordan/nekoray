param(
    [string] $CorePath = "deployment/windows64/nekobox_core.exe",
    [switch] $Json
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$Root = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
. (Join-Path $Root "tools\path_safety.ps1")
$probeScript = Join-Path $Root "tools\verify_mixed_inbound.ps1"
$coreFull = if ([System.IO.Path]::IsPathRooted($CorePath)) {
    [System.IO.Path]::GetFullPath($CorePath)
} else {
    [System.IO.Path]::GetFullPath((Join-Path $Root $CorePath))
}
$coreFull = Assert-PathOutsideProtectedProduction $coreFull "Mixed-probe core executable"
if (!(Test-Path -LiteralPath $probeScript -PathType Leaf)) { throw "Probe script not found: $probeScript" }
if (!(Test-Path -LiteralPath $coreFull -PathType Leaf)) { throw "Core not found: $coreFull" }

function Get-ListenerSnapshot([int[]] $Ports) {
    return @(
        Get-NetTCPConnection -State Listen -LocalPort $Ports -ErrorAction SilentlyContinue |
            Sort-Object LocalPort, LocalAddress, OwningProcess |
            ForEach-Object { "$($_.LocalAddress):$($_.LocalPort):$($_.OwningProcess)" }
    )
}

function Get-SystemProxySnapshot {
    $path = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Internet Settings"
    $value = Get-ItemProperty -Path $path -ErrorAction SilentlyContinue
    return [pscustomobject]@{
        ProxyEnable = if ($null -ne $value -and $value.PSObject.Properties.Name -contains "ProxyEnable") { $value.ProxyEnable } else { $null }
        ProxyServer = if ($null -ne $value -and $value.PSObject.Properties.Name -contains "ProxyServer") { $value.ProxyServer } else { $null }
        AutoConfigURL = if ($null -ne $value -and $value.PSObject.Properties.Name -contains "AutoConfigURL") { $value.AutoConfigURL } else { $null }
    } | ConvertTo-Json -Compress
}

function Invoke-SuccessFixture([string] $Name, [int] $Port) {
    if (@(Get-ListenerSnapshot @($Port)).Count -gt 0) {
        throw "Fixture port is already occupied: $Port"
    }
    $fixture = Join-Path $PSScriptRoot "fixtures\$Name"
    $raw = (& powershell -NoProfile -ExecutionPolicy Bypass -File $probeScript `
        -ConfigPath $fixture -CorePath $coreFull `
        -TestUrl $script:OriginUrl -ConnectTestUrl $script:OriginUrl `
        -TimeoutSeconds 15 -AllowDirectTestOutbound -Json 2>&1) -join "`n"
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 0) {
        throw "Fixture failed ($Name, exit $exitCode): $raw"
    }
    $result = $raw | ConvertFrom-Json
    $passed = $result.listener_owned_by_core -and
              $result.listener_owned_after_probes -and
              $result.http.success -and
              $result.http_connect.success -and
              $result.socks5h.success -and
              $result.evidence.startup_error -eq 0 -and
              $result.evidence.proxy_outbound_events -ge 1 -and
              @(Get-ListenerSnapshot @($Port)).Count -eq 0
    return [pscustomobject]@{
        fixture = $Name
        expected = "success"
        exit_code = $exitCode
        passed = [bool]$passed
    }
}

function Invoke-RejectionFixture([string] $Name, [int] $Port, [string] $Pattern) {
    if (@(Get-ListenerSnapshot @($Port)).Count -gt 0) {
        throw "Fixture port is already occupied: $Port"
    }
    $fixture = Join-Path $PSScriptRoot "fixtures\$Name"
    $previousErrorActionPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = "Continue"
        $raw = (& powershell -NoProfile -ExecutionPolicy Bypass -File $probeScript `
            -ConfigPath $fixture -CorePath $coreFull -Json 2>&1) -join "`n"
        $exitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousErrorActionPreference
    }
    return [pscustomobject]@{
        fixture = $Name
        expected = "rejected"
        exit_code = $exitCode
        passed = $exitCode -ne 0 -and $raw -match $Pattern -and @(Get-ListenerSnapshot @($Port)).Count -eq 0
    }
}

$forbiddenLog = Join-Path (Split-Path -Parent $coreFull) "must-not-be-created.log"
if (Test-Path -LiteralPath $forbiddenLog) { throw "Forbidden fixture log already exists: $forbiddenLog" }
$originPort = 18090
$originScript = Join-Path $PSScriptRoot "fixtures\http_204_server.py"
if (!(Test-Path -LiteralPath $originScript -PathType Leaf)) { throw "Origin fixture not found: $originScript" }
if (@(Get-ListenerSnapshot @($originPort)).Count -gt 0) { throw "Origin fixture port is already occupied: $originPort" }
$tempRoot = Assert-PathOutsideProtectedProduction `
    ([IO.Path]::GetFullPath([IO.Path]::GetTempPath())) `
    "Mixed-probe fixture temporary root"
$originId = [Guid]::NewGuid().ToString("N")
$originStdout = Assert-NewFileOutsideProtectedProduction `
    (Join-Path $tempRoot "nekoray-origin-$originId.stdout.log") `
    "Mixed-probe fixture stdout path"
$originStderr = Assert-NewFileOutsideProtectedProduction `
    (Join-Path $tempRoot "nekoray-origin-$originId.stderr.log") `
    "Mixed-probe fixture stderr path"
$originProcess = $null
$script:OriginUrl = "http://127.0.0.1:$originPort/health"
$extraBefore = @(Get-ListenerSnapshot @(18082, 18083))
$proxyBefore = Get-SystemProxySnapshot

try {
    $originProcess = Start-Process `
        -FilePath "python" `
        -ArgumentList @("-I", $originScript, "--port", "$originPort") `
        -WindowStyle Hidden `
        -RedirectStandardOutput $originStdout `
        -RedirectStandardError $originStderr `
        -PassThru
    $originDeadline = [DateTime]::UtcNow.AddSeconds(10)
    $originReady = $false
    while ([DateTime]::UtcNow -lt $originDeadline) {
        Start-Sleep -Milliseconds 100
        if ($originProcess.HasExited) { break }
        if (@(Get-ListenerSnapshot @($originPort) | Where-Object { $_ -match ":$($originProcess.Id)$" }).Count -gt 0) {
            $originReady = $true
            break
        }
    }
    if (!$originReady) {
        $detail = if (Test-Path -LiteralPath $originStderr) { Get-Content -LiteralPath $originStderr -Raw -Encoding UTF8 } else { "" }
        throw "Loopback origin did not become ready: $detail"
    }

    $results = @(
        Invoke-SuccessFixture "mixed-direct-sanitization.json" 18081
        Invoke-SuccessFixture "mixed-auth-direct.json" 18084
        Invoke-RejectionFixture "mixed-reject-non-loopback.json" 18085 "non-loopback"
        Invoke-RejectionFixture "mixed-reject-tun.json" 18086 "TUN inbound"
        Invoke-RejectionFixture "mixed-reject-ntp-system-write.json" 18087 "ntp.write_to_system"
        Invoke-RejectionFixture "mixed-reject-system-endpoint.json" 18088 "top-level endpoints"
        Invoke-RejectionFixture "mixed-reject-dynamic-outbound.json" 18089 "refuses outbound type"
    )
} finally {
    if ($null -ne $originProcess -and !$originProcess.HasExited) {
        Stop-Process -Id $originProcess.Id -Force
        $originProcess.WaitForExit(5000) | Out-Null
    }
    foreach ($path in @($originStdout, $originStderr)) {
        if ([IO.File]::Exists($path)) { [IO.File]::Delete($path) }
    }
}

$extraAfter = @(Get-ListenerSnapshot @(18082, 18083))
$proxyAfter = Get-SystemProxySnapshot
$sideEffectsPassed = ($extraBefore | ConvertTo-Json -Compress) -eq ($extraAfter | ConvertTo-Json -Compress) -and
                     $proxyBefore -eq $proxyAfter -and
                     !(Test-Path -LiteralPath $forbiddenLog)
$allPassed = @($results | Where-Object { !$_.passed }).Count -eq 0 -and $sideEffectsPassed

$summary = [pscustomobject]@{
    passed = $allPassed
    cases = $results
    side_effect_checks = [pscustomobject]@{
        extra_listener_state_unchanged = ($extraBefore | ConvertTo-Json -Compress) -eq ($extraAfter | ConvertTo-Json -Compress)
                               system_proxy_state_unchanged = $proxyBefore -eq $proxyAfter
                               forbidden_log_not_created = !(Test-Path -LiteralPath $forbiddenLog)
                               loopback_origin_released = @(Get-ListenerSnapshot @($originPort)).Count -eq 0
                           }
}

if ($Json) {
    $summary | ConvertTo-Json -Depth 6
} else {
    $summary | Format-List
    $results | Format-Table -AutoSize
    $summary.side_effect_checks | Format-List
}

if (!$allPassed) { exit 1 }
exit 0
