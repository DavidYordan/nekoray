param(
    [string] $CorePath = "deployment/windows64/nekobox_core.exe",
    [switch] $Json
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$Root = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$auditScript = Join-Path $Root "tools\verify_runtime_connectivity.ps1"
$coreFull = if ([System.IO.Path]::IsPathRooted($CorePath)) {
    [System.IO.Path]::GetFullPath($CorePath)
} else {
    [System.IO.Path]::GetFullPath((Join-Path $Root $CorePath))
}
$runtimeFixture = Join-Path $PSScriptRoot "fixtures\runtime-connectivity-direct.json"
$groupsFixture = Join-Path $PSScriptRoot "fixtures\runtime-connectivity-groups.json"
$listenPort = 18087

foreach ($required in @($auditScript, $coreFull, $runtimeFixture, $groupsFixture)) {
    if (!(Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Required file not found: $required"
    }
}
if (@(Get-NetTCPConnection -State Listen -LocalPort $listenPort -ErrorAction SilentlyContinue).Count -gt 0) {
    throw "Fixture port is already occupied: $listenPort"
}

function Get-SystemProxySnapshot {
    $key = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Internet Settings"
    $value = Get-ItemProperty -LiteralPath $key -ErrorAction SilentlyContinue
    [pscustomobject]@{
        ProxyEnable = if ($null -ne $value -and $value.PSObject.Properties.Name -contains "ProxyEnable") { $value.ProxyEnable } else { $null }
        ProxyServer = if ($null -ne $value -and $value.PSObject.Properties.Name -contains "ProxyServer") { $value.ProxyServer } else { $null }
        AutoConfigURL = if ($null -ne $value -and $value.PSObject.Properties.Name -contains "AutoConfigURL") { $value.AutoConfigURL } else { $null }
        ProxyOverride = if ($null -ne $value -and $value.PSObject.Properties.Name -contains "ProxyOverride") { $value.ProxyOverride } else { $null }
    } | ConvertTo-Json -Compress
}

function Invoke-Audit([int] $ExpectedStatus, [string] $OutputPath) {
    $previousErrorActionPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = "Continue"
        $raw = (& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $auditScript `
            -PackageDir $tempRoot `
            -TestUrl $script:RuntimeOriginUrl `
            -ExpectedHttpStatus $ExpectedStatus `
            -ExpectRunning `
            -OutputPath $OutputPath `
            -Json 2>&1) -join "`n"
        $exitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousErrorActionPreference
    }
    [pscustomobject]@{
        exit_code = $exitCode
        report = $raw | ConvertFrom-Json
    }
}

$tempRoot = Join-Path ([System.IO.Path]::GetTempPath()) "nekoray-runtime-connectivity-test-$([Guid]::NewGuid().ToString('N'))"
$tempRoot = [System.IO.Path]::GetFullPath($tempRoot)
$expectedPrefix = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath()).TrimEnd('\') + "\nekoray-runtime-connectivity-test-"
if (!$tempRoot.StartsWith($expectedPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Unsafe temporary path: $tempRoot"
}

$process = $null
$originProcess = $null
$originPort = 18091
$originScript = Join-Path $PSScriptRoot "fixtures\http_204_server.py"
if (!(Test-Path -LiteralPath $originScript -PathType Leaf)) { throw "Origin fixture not found: $originScript" }
if (@(Get-NetTCPConnection -State Listen -LocalPort $originPort -ErrorAction SilentlyContinue).Count -gt 0) {
    throw "Origin fixture port is already occupied: $originPort"
}
$script:RuntimeOriginUrl = "http://127.0.0.1:$originPort/health"
$proxyBefore = Get-SystemProxySnapshot
$result = $null
try {
    $groupsDir = Join-Path $tempRoot "config\groups"
    New-Item -ItemType Directory -Path $groupsDir -Force | Out-Null
    Copy-Item -LiteralPath $coreFull -Destination (Join-Path $tempRoot "nekobox_core.exe") -Force
    Copy-Item -LiteralPath $groupsFixture -Destination (Join-Path $groupsDir "nekobox.json") -Force

    $originProcess = Start-Process `
        -FilePath "python" `
        -ArgumentList @("-I", $originScript, "--port", "$originPort") `
        -WorkingDirectory $tempRoot `
        -WindowStyle Hidden `
        -RedirectStandardOutput (Join-Path $tempRoot "origin.stdout.log") `
        -RedirectStandardError (Join-Path $tempRoot "origin.stderr.log") `
        -PassThru
    $originDeadline = [DateTime]::UtcNow.AddSeconds(10)
    $originReady = $false
    while ([DateTime]::UtcNow -lt $originDeadline) {
        Start-Sleep -Milliseconds 100
        if ($originProcess.HasExited) { break }
        $originOwned = @(Get-NetTCPConnection -State Listen -LocalPort $originPort -ErrorAction SilentlyContinue |
            Where-Object { $_.LocalAddress -eq "127.0.0.1" -and $_.OwningProcess -eq $originProcess.Id })
        if ($originOwned.Count -gt 0) {
            $originReady = $true
            break
        }
    }
    if (!$originReady) { throw "Loopback origin did not acquire its port." }

    $process = Start-Process `
        -FilePath (Join-Path $tempRoot "nekobox_core.exe") `
        -ArgumentList "run -c `"$runtimeFixture`"" `
        -WorkingDirectory $tempRoot `
        -WindowStyle Hidden `
        -RedirectStandardOutput (Join-Path $tempRoot "core.stdout.log") `
        -RedirectStandardError (Join-Path $tempRoot "core.stderr.log") `
        -PassThru

    $ready = $false
    $deadline = [DateTime]::UtcNow.AddSeconds(10)
    while ([DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 200
        if ($process.HasExited) { break }
        $owned = @(Get-NetTCPConnection -State Listen -LocalPort $listenPort -ErrorAction SilentlyContinue |
            Where-Object { $_.LocalAddress -eq "127.0.0.1" -and $_.OwningProcess -eq $process.Id })
        if ($owned.Count -gt 0) {
            $ready = $true
            break
        }
    }
    if (!$ready) { throw "Direct runtime fixture did not acquire its loopback port." }

    $expected204 = Invoke-Audit 204 (Join-Path $tempRoot "audit-204.json")
    $expected200 = Invoke-Audit 200 (Join-Path $tempRoot "audit-200.json")

    $port204 = @($expected204.report.proxy_ports)[0]
    $pass204 = $expected204.exit_code -eq 0 -and
               $expected204.report.status -eq "OK" -and
               $port204.owner.pid -eq $process.Id -and
               $port204.curl_http.http_code -eq 204 -and $port204.curl_http.ok -and
               $port204.curl_socks5h.http_code -eq 204 -and $port204.curl_socks5h.ok
    $reject200 = $expected200.exit_code -ne 0 -and
                 $expected200.report.status -eq "FAILED" -and
                 @($expected200.report.failures).Count -ge 2

    $result = [pscustomobject]@{
        passed = [bool]($pass204 -and $reject200)
        expected_204 = [pscustomobject]@{
            passed = [bool]$pass204
            exit_code = $expected204.exit_code
            http_code = $port204.curl_http.http_code
            socks5h_code = $port204.curl_socks5h.http_code
            listener_owned_by_fixture_pid = $port204.owner.pid -eq $process.Id
        }
        expected_200_mismatch = [pscustomobject]@{
            rejected = [bool]$reject200
            exit_code = $expected200.exit_code
            failure_count = @($expected200.report.failures).Count
        }
    }
} finally {
    if ($null -ne $process -and !$process.HasExited) {
        Stop-Process -Id $process.Id -Force
        $process.WaitForExit(5000) | Out-Null
    }
    if ($null -ne $originProcess -and !$originProcess.HasExited) {
        Stop-Process -Id $originProcess.Id -Force
        $originProcess.WaitForExit(5000) | Out-Null
    }
    if (Test-Path -LiteralPath $tempRoot -PathType Container) {
        $resolved = [System.IO.Path]::GetFullPath($tempRoot)
        if (!$resolved.StartsWith($expectedPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
            throw "Refusing unsafe cleanup: $resolved"
        }
        Get-ChildItem -LiteralPath $resolved -Recurse -Force -ErrorAction SilentlyContinue | ForEach-Object {
            try { $_.Attributes = "Normal" } catch {}
        }
        [System.IO.Directory]::Delete($resolved, $true)
    }
}

$proxyAfter = Get-SystemProxySnapshot
$portReleased = @(Get-NetTCPConnection -State Listen -LocalPort $listenPort -ErrorAction SilentlyContinue).Count -eq 0
$originReleased = @(Get-NetTCPConnection -State Listen -LocalPort $originPort -ErrorAction SilentlyContinue).Count -eq 0
$sideEffectsPassed = $proxyBefore -eq $proxyAfter -and $portReleased -and $originReleased
$result | Add-Member -NotePropertyName side_effect_checks -NotePropertyValue ([pscustomobject]@{
    system_proxy_state_unchanged = $proxyBefore -eq $proxyAfter
    fixture_port_released = $portReleased
    loopback_origin_released = $originReleased
})
$result.passed = [bool]($result.passed -and $sideEffectsPassed)

if ($Json) {
    $result | ConvertTo-Json -Depth 6
} else {
    $result | Format-List
    $result.expected_204 | Format-List
    $result.expected_200_mismatch | Format-List
    $result.side_effect_checks | Format-List
}

if (!$result.passed) { exit 1 }
exit 0
