param(
    [string] $CorePath = "deployment/windows64/nekobox_core.exe",
    [switch] $Json
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$Root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
. (Join-Path $Root "tools\path_safety.ps1")

$coreFull = if ([IO.Path]::IsPathRooted($CorePath)) {
    [IO.Path]::GetFullPath($CorePath)
} else {
    [IO.Path]::GetFullPath((Join-Path $Root $CorePath))
}
$coreFull = Assert-PathOutsideProtectedProduction $coreFull "Auxiliary-route runtime core executable"
$runtimeFixture = Join-Path $PSScriptRoot "fixtures\auxiliary-two-line-runtime.json"
$proxyScript = Join-Path $PSScriptRoot "fixtures\http_proxy_line_server.py"
foreach ($required in @($coreFull, $runtimeFixture, $proxyScript)) {
    if (!(Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Required file not found: $required"
    }
}

$ports = @(18120, 18121, 18130, 18131)
function Get-ListeningConnections([int[]] $LocalPorts) {
    @(
        foreach ($port in $LocalPorts) {
            Get-NetTCPConnection -State Listen -LocalPort $port -ErrorAction SilentlyContinue
        }
    )
}
if (@(Get-ListeningConnections $ports).Count -gt 0) {
    throw "One or more auxiliary-route fixture ports are already occupied: $($ports -join ', ')"
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

function Wait-OwnedListener(
    [Diagnostics.Process] $Process,
    [int] $Port,
    [string] $Label
) {
    $deadline = [DateTime]::UtcNow.AddSeconds(10)
    while ([DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 100
        if ($Process.HasExited) { break }
        $owned = @(
            Get-NetTCPConnection -State Listen -LocalPort $Port -ErrorAction SilentlyContinue |
                Where-Object {
                    $_.LocalAddress -eq "127.0.0.1" -and
                    $_.OwningProcess -eq $Process.Id
                }
        )
        if ($owned.Count -eq 1) { return }
    }
    throw "$Label did not acquire 127.0.0.1:$Port with PID $($Process.Id)."
}

function Invoke-ProxyRequest([int] $Port, [string] $Url) {
    $previousErrorActionPreference = $ErrorActionPreference
    $previousNoProxy = [Environment]::GetEnvironmentVariable("NO_PROXY", "Process")
    $previousNoProxyLower = [Environment]::GetEnvironmentVariable("no_proxy", "Process")
    try {
        $ErrorActionPreference = "Continue"
        [Environment]::SetEnvironmentVariable("NO_PROXY", "", "Process")
        [Environment]::SetEnvironmentVariable("no_proxy", "", "Process")
        $output = (& curl.exe `
            --silent --show-error --max-time 5 `
            --proxy "http://127.0.0.1:$Port" `
            --output NUL `
            --write-out "NEKO_HTTP=%{http_code}" `
            $Url 2>$null) -join ""
        $exitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousErrorActionPreference
        [Environment]::SetEnvironmentVariable("NO_PROXY", $previousNoProxy, "Process")
        [Environment]::SetEnvironmentVariable("no_proxy", $previousNoProxyLower, "Process")
    }
    [pscustomobject]@{
        exit_code = $exitCode
        http_code = if ($output -match 'NEKO_HTTP=(\d{3})') { [int]$Matches[1] } else { 0 }
    }
}

function Get-HitCount([string] $Path) {
    if (!(Test-Path -LiteralPath $Path -PathType Leaf)) { return 0 }
    return @(
        Get-Content -LiteralPath $Path -Encoding utf8 |
            Where-Object { ![string]::IsNullOrWhiteSpace($_) }
    ).Count
}

$safeTempRoot = Assert-PathOutsideProtectedProduction `
    ([IO.Path]::GetFullPath([IO.Path]::GetTempPath())) `
    "Auxiliary-route runtime temporary root"
$tempRoot = Assert-PathOutsideProtectedProduction `
    (Join-Path $safeTempRoot "nekoray-aux-route-test-$([Guid]::NewGuid().ToString('N'))") `
    "Auxiliary-route runtime temporary directory"
$expectedPrefix = $safeTempRoot.TrimEnd('\') + "\nekoray-aux-route-test-"
if (!$tempRoot.StartsWith($expectedPrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Unsafe temporary path: $tempRoot"
}
New-Item -ItemType Directory -Path $tempRoot | Out-Null

$lineAStdout = Join-Path $tempRoot "line-a.stdout.log"
$lineAStderr = Join-Path $tempRoot "line-a.stderr.log"
$lineBStdout = Join-Path $tempRoot "line-b.stdout.log"
$lineBStderr = Join-Path $tempRoot "line-b.stderr.log"
$coreStdout = Join-Path $tempRoot "core.stdout.log"
$coreStderr = Join-Path $tempRoot "core.stderr.log"
$lineAProcess = $null
$lineBProcess = $null
$coreProcess = $null
$result = $null
$proxyBefore = Get-SystemProxySnapshot

try {
    $previousErrorActionPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = "Continue"
        $checkOutput = (& $coreFull check -c $runtimeFixture 2>&1) -join "`n"
        $checkExitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousErrorActionPreference
    }
    if ($checkExitCode -ne 0) {
        throw "Auxiliary two-line runtime fixture failed core schema validation: $checkOutput"
    }

    $python = (Get-Command python -ErrorAction Stop).Source
    $lineAProcess = Start-Process `
        -FilePath $python `
        -ArgumentList @("-I", $proxyScript, "--port", "18130", "--status", "210", "--line", "line-a") `
        -WorkingDirectory $tempRoot `
        -WindowStyle Hidden `
        -RedirectStandardOutput $lineAStdout `
        -RedirectStandardError $lineAStderr `
        -PassThru
    $lineBProcess = Start-Process `
        -FilePath $python `
        -ArgumentList @("-I", $proxyScript, "--port", "18131", "--status", "211", "--line", "line-b") `
        -WorkingDirectory $tempRoot `
        -WindowStyle Hidden `
        -RedirectStandardOutput $lineBStdout `
        -RedirectStandardError $lineBStderr `
        -PassThru
    Wait-OwnedListener $lineAProcess 18130 "Line A HTTP proxy fixture"
    Wait-OwnedListener $lineBProcess 18131 "Line B HTTP proxy fixture"

    $coreProcess = Start-Process `
        -FilePath $coreFull `
        -ArgumentList "run -c `"$runtimeFixture`"" `
        -WorkingDirectory $tempRoot `
        -WindowStyle Hidden `
        -RedirectStandardOutput $coreStdout `
        -RedirectStandardError $coreStderr `
        -PassThru
    Wait-OwnedListener $coreProcess 18120 "Auxiliary line A Mixed listener"
    Wait-OwnedListener $coreProcess 18121 "Auxiliary line B Mixed listener"

    $lineA = Invoke-ProxyRequest 18120 "http://line-a.test/probe"
    $lineB = Invoke-ProxyRequest 18121 "http://line-b.test/probe"
    $crossLine = Invoke-ProxyRequest 18120 "http://crossline.test/must-stay-a"

    $lineAHitsBeforeReject = Get-HitCount $lineAStdout
    $lineBHitsBeforeReject = Get-HitCount $lineBStdout
    $blocked = Invoke-ProxyRequest 18120 "http://blocked.test/must-reject"
    Start-Sleep -Milliseconds 250
    $rejectDidNotReachUpstream =
        (Get-HitCount $lineAStdout) -eq $lineAHitsBeforeReject -and
        (Get-HitCount $lineBStdout) -eq $lineBHitsBeforeReject

    Stop-Process -Id $lineAProcess.Id -Force
    $lineAProcess.WaitForExit(5000) | Out-Null
    $failureDeadline = [DateTime]::UtcNow.AddSeconds(5)
    while ([DateTime]::UtcNow -lt $failureDeadline -and
           @(Get-ListeningConnections @(18130)).Count -gt 0) {
        Start-Sleep -Milliseconds 100
    }

    $lineAFailed = Invoke-ProxyRequest 18120 "http://line-a.test/upstream-down"
    $lineBAfterFailure = Invoke-ProxyRequest 18121 "http://line-b.test/still-running"
    $coreStillRunning = !$coreProcess.HasExited -and
                        @(Get-ListeningConnections @(18120, 18121) |
                            Where-Object { $_.OwningProcess -eq $coreProcess.Id }).Count -eq 2

    $result = [pscustomobject]@{
        passed = [bool](
            $lineA.exit_code -eq 0 -and $lineA.http_code -eq 210 -and
            $lineB.exit_code -eq 0 -and $lineB.http_code -eq 211 -and
            $crossLine.exit_code -eq 0 -and $crossLine.http_code -eq 210 -and
            $blocked.http_code -ne 210 -and $blocked.http_code -ne 211 -and
            $rejectDidNotReachUpstream -and
            $lineAFailed.http_code -ne 210 -and $lineAFailed.http_code -ne 211 -and
            $lineBAfterFailure.exit_code -eq 0 -and $lineBAfterFailure.http_code -eq 211 -and
            $coreStillRunning
        )
        initial_mapping = [pscustomobject]@{
            line_a_status = $lineA.http_code
            line_a_exit_code = $lineA.exit_code
            line_b_status = $lineB.http_code
            line_b_exit_code = $lineB.exit_code
        }
        cross_line_guard = [pscustomobject]@{
            requested_through_line_a_status = $crossLine.http_code
            exit_code = $crossLine.exit_code
        }
        reject_guard = [pscustomobject]@{
            client_status = $blocked.http_code
            client_exit_code = $blocked.exit_code
            upstream_hit_count_unchanged = [bool]$rejectDidNotReachUpstream
        }
        line_failure_isolation = [pscustomobject]@{
            failed_line_status = $lineAFailed.http_code
            failed_line_exit_code = $lineAFailed.exit_code
            surviving_line_status = $lineBAfterFailure.http_code
            surviving_line_exit_code = $lineBAfterFailure.exit_code
            core_and_both_inbounds_still_running = [bool]$coreStillRunning
        }
    }
} finally {
    foreach ($process in @($coreProcess, $lineAProcess, $lineBProcess)) {
        if ($null -ne $process -and !$process.HasExited) {
            Stop-Process -Id $process.Id -Force
            $process.WaitForExit(5000) | Out-Null
        }
    }
    if (Test-Path -LiteralPath $tempRoot -PathType Container) {
        $resolved = [IO.Path]::GetFullPath($tempRoot)
        if (!$resolved.StartsWith($expectedPrefix, [StringComparison]::OrdinalIgnoreCase)) {
            throw "Refusing unsafe cleanup: $resolved"
        }
        Assert-DirectoryTreeHasNoReparsePoints $resolved "Auxiliary-route runtime cleanup tree"
        Get-ChildItem -LiteralPath $resolved -Recurse -Force -ErrorAction SilentlyContinue |
            ForEach-Object {
                try { $_.Attributes = "Normal" } catch {}
            }
        [IO.Directory]::Delete($resolved, $true)
    }
}

$proxyAfter = Get-SystemProxySnapshot
$portsReleased = @(Get-ListeningConnections $ports).Count -eq 0
$result | Add-Member -NotePropertyName side_effect_checks -NotePropertyValue ([pscustomobject]@{
    system_proxy_state_unchanged = $proxyBefore -eq $proxyAfter
    all_fixture_ports_released = $portsReleased
})
$result.passed = [bool](
    $result.passed -and
    $result.side_effect_checks.system_proxy_state_unchanged -and
    $result.side_effect_checks.all_fixture_ports_released
)

if ($Json) {
    $result | ConvertTo-Json -Depth 6
} else {
    $result | Format-List
    $result.initial_mapping | Format-List
    $result.cross_line_guard | Format-List
    $result.reject_guard | Format-List
    $result.line_failure_isolation | Format-List
    $result.side_effect_checks | Format-List
}

if (!$result.passed) { exit 1 }
exit 0
