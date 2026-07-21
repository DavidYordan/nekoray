param(
    [Parameter(Mandatory = $true)]
    [string] $ConfigPath,
    [string] $CorePath = "",
    [string] $InboundTag = "mixed-in",
    [string] $TestUrl = "http://cp.cloudflare.com/",
    [string] $ConnectTestUrl = "https://cp.cloudflare.com/",
    [ValidateRange(100, 599)]
    [int] $ExpectedHttpStatus = 204,
    [ValidateRange(1, 60)]
    [int] $TimeoutSeconds = 15,
    [ValidateSet("preserve", "native", "mihomo")]
    [string] $AnyTLSClientOverride = "preserve",
    [ValidateSet("preserve", "h2-http1", "none")]
    [string] $AnyTLSAlpnOverride = "preserve",
    [ValidateSet("preserve", "none", "chrome")]
    [string] $AnyTLSUtlsOverride = "preserve",
    [switch] $RemoveAnyTLSDetour,
    [switch] $ForceAutoDetectInterface,
    [switch] $AllowDirectTestOutbound,
    [switch] $KeepLogs,
    [switch] $Json
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$Root = [System.IO.Path]::GetFullPath((Join-Path $ScriptDir ".."))

if ($InboundTag -ne "mixed-in") {
    throw "This connectivity probe supports only the primary 'mixed-in' -> 'proxy' path. It is not an auxiliary-port mapping contract tester."
}

function Resolve-InputPath([string] $Path) {
    if ([System.IO.Path]::IsPathRooted($Path)) {
        return [System.IO.Path]::GetFullPath($Path)
    }
    return [System.IO.Path]::GetFullPath((Join-Path $Root $Path))
}

function Require-File([string] $Path, [string] $Label) {
    if (!(Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Label not found: $Path"
    }
    return (Resolve-Path -LiteralPath $Path).Path
}

function Get-PropertyValue($Object, [string] $Name, $Default = $null) {
    if ($null -eq $Object) {
        return $Default
    }
    if ($Object.PSObject.Properties.Name -contains $Name) {
        return $Object.$Name
    }
    return $Default
}

function Resolve-LoopbackAddress([string] $Address) {
    if ([string]::IsNullOrWhiteSpace($Address)) {
        throw "Mixed inbound must explicitly declare a loopback listen address."
    }
    if ($Address -eq "localhost") {
        return "127.0.0.1"
    }
    $parsed = $null
    if (![System.Net.IPAddress]::TryParse($Address, [ref]$parsed) -or ![System.Net.IPAddress]::IsLoopback($parsed)) {
        throw "Refusing to probe a non-loopback mixed inbound: $Address"
    }
    return $parsed.ToString()
}

function Get-ListeningConnections([int] $Port) {
    return @(Get-NetTCPConnection -State Listen -LocalPort $Port -ErrorAction SilentlyContinue)
}

function Get-StrictOutboundClosure($Outbounds, [string] $RootTag, [bool] $AllowDirect) {
    $byTag = @{}
    foreach ($outbound in @($Outbounds)) {
        $tag = [string](Get-PropertyValue $outbound "tag" "")
        if ([string]::IsNullOrWhiteSpace($tag)) {
            throw "Every diagnostic outbound must have a non-empty string tag."
        }
        if ($byTag.ContainsKey($tag)) {
            throw "Duplicate diagnostic outbound tag: $tag"
        }
        $byTag[$tag] = $outbound
    }

    $closure = [System.Collections.ArrayList]::new()
    $visited = @{}
    $current = $RootTag
    while (![string]::IsNullOrWhiteSpace($current)) {
        if ($visited.ContainsKey($current)) {
            throw "Outbound detour cycle at '$current'."
        }
        $visited[$current] = $true
        if (!$byTag.ContainsKey($current)) {
            throw "Outbound detour target '$current' does not exist exactly once."
        }
        $outbound = $byTag[$current]
        $type = ([string](Get-PropertyValue $outbound "type" "")).Trim().ToLowerInvariant()
        if ($type -in @("selector", "urltest", "url-test", "block") -or
            (!$AllowDirect -and $type -eq "direct")) {
            throw "Strict line probe refuses outbound type '$type' at '$current'."
        }
        [void]$closure.Add($outbound)
        if (!($outbound.PSObject.Properties.Name -contains "detour")) {
            break
        }
        if ($outbound.detour -isnot [string]) {
            throw "Outbound '$current' has a non-string detour."
        }
        $current = ([string]$outbound.detour).Trim()
    }
    return @($closure)
}

function ConvertTo-CurlConfigValue([string] $Value) {
    if ($Value -match "[\x00\r\n]") {
        throw "Inbound credentials containing control characters are not supported by this probe."
    }
    return $Value.Replace("\", "\\").Replace('"', '\"')
}

function Invoke-ProxyProbe(
    [string] $Scheme,
    [string] $HostName,
    [int] $Port,
    [string] $Url,
    [int] $Timeout,
    [int] $ExpectedStatus,
    [string] $Username,
    [string] $Password,
    [bool] $ProxyTunnel = $false
) {
    $proxyHost = if ($HostName -eq "::1") { "[::1]" } else { $HostName }
    $arguments = @(
        "--silent",
        "--show-error",
        "--max-time", [string]$Timeout,
        "--connect-timeout", [string][Math]::Min(5, $Timeout),
        "--output", "NUL",
        "--write-out", "HTTP_CODE=%{http_code};CONNECT=%{time_connect};TOTAL=%{time_total}",
        "--proxy", "${Scheme}://${proxyHost}:$Port"
    )
    if ($ProxyTunnel) {
        $arguments += "--proxytunnel"
    }
    $arguments += $Url

    $previousErrorActionPreference = $ErrorActionPreference
    $previousOutputEncoding = $OutputEncoding
    $previousNoProxy = [Environment]::GetEnvironmentVariable("NO_PROXY", "Process")
    $previousNoProxyLower = [Environment]::GetEnvironmentVariable("no_proxy", "Process")
    try {
        # Windows PowerShell turns native stderr into ErrorRecord objects. Keep
        # curl failures as probe results instead of aborting the cleanup path.
        $ErrorActionPreference = "Continue"
        $OutputEncoding = [System.Text.UTF8Encoding]::new($false)
        [Environment]::SetEnvironmentVariable("NO_PROXY", "", "Process")
        [Environment]::SetEnvironmentVariable("no_proxy", "", "Process")
        if (![string]::IsNullOrEmpty($Username) -or ![string]::IsNullOrEmpty($Password)) {
            # Keep credentials off the process command line. curl reads this
            # one-line config from stdin and it is never written to disk.
            $credential = ConvertTo-CurlConfigValue "${Username}:${Password}"
            $curlConfig = "proxy-user = `"$credential`""
            $output = ($curlConfig | & curl.exe --config - @arguments 2>&1) -join "`n"
        } else {
            $output = (& curl.exe @arguments 2>&1) -join "`n"
        }
        $exitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousErrorActionPreference
        $OutputEncoding = $previousOutputEncoding
        [Environment]::SetEnvironmentVariable("NO_PROXY", $previousNoProxy, "Process")
        [Environment]::SetEnvironmentVariable("no_proxy", $previousNoProxyLower, "Process")
    }
    $httpCode = 0
    if ($output -match "HTTP_CODE=(\d+)") {
        $httpCode = [int]$Matches[1]
    }
    return [pscustomobject]@{
        scheme = $Scheme
        exit_code = $exitCode
        http_code = $httpCode
        success = $exitCode -eq 0 -and $httpCode -eq $ExpectedStatus
        summary = ($output -replace "\r?\n", " | ")
    }
}

$configFull = Require-File (Resolve-InputPath $ConfigPath) "core config"
if ([string]::IsNullOrWhiteSpace($CorePath)) {
    $CorePath = Join-Path $Root "deployment\windows64\nekobox_core.exe"
}
$coreFull = Require-File (Resolve-InputPath $CorePath) "nekobox_core.exe"
$config = Get-Content -LiteralPath $configFull -Raw -Encoding UTF8 | ConvertFrom-Json
$runConfigFull = $configFull
$variantConfigPath = ""
$inbounds = @(Get-PropertyValue $config "inbounds" @())
$outbounds = @(Get-PropertyValue $config "outbounds" @())
$originalInboundCount = $inbounds.Count

if (@($inbounds | Where-Object { $_.type -eq "tun" }).Count -gt 0) {
    throw "Refusing to start a config that contains a TUN inbound. Export a non-TUN runtime config for this probe."
}

$ntp = Get-PropertyValue $config "ntp" $null
if ($null -ne $ntp -and (Get-PropertyValue $ntp "write_to_system" $false) -eq $true) {
    throw "Refusing ntp.write_to_system=true."
}
$config.PSObject.Properties.Remove("ntp")
$endpoints = @(Get-PropertyValue $config "endpoints" @())
if ($endpoints.Count -gt 0) {
    throw "Refusing a config with top-level endpoints."
}
$config.PSObject.Properties.Remove("endpoints")

$target = @($inbounds | Where-Object { $_.tag -eq $InboundTag })
if ($target.Count -ne 1) {
    throw "Expected exactly one inbound tagged '$InboundTag', found $($target.Count)."
}
$target = $target[0]
if ($target.type -ne "mixed") {
    throw "Inbound '$InboundTag' is '$($target.type)', not 'mixed'."
}

$listenAddress = Resolve-LoopbackAddress ([string](Get-PropertyValue $target "listen" ""))
$target.listen = $listenAddress
$target.PSObject.Properties.Remove("set_system_proxy")
$listenPort = [int]$target.listen_port
if ($listenPort -lt 1 -or $listenPort -gt 65535) {
    throw "Invalid mixed inbound port: $listenPort"
}

$existingListeners = @(Get-ListeningConnections $listenPort)
if ($existingListeners.Count -gt 0) {
    throw "Refusing to start because inbound port $listenPort is already listening."
}

# Always run a constrained temporary copy. Other inbounds, controllers and
# file-backed logging are outside this probe's scope and may have side effects.
$config.inbounds = @($target)
$config.PSObject.Properties.Remove("experimental")
$config.PSObject.Properties.Remove("services")
$diagnosticLog = Get-PropertyValue $config "log" $null
if ($null -ne $diagnosticLog) {
    $diagnosticLog.PSObject.Properties.Remove("output")
}

$username = ""
$password = ""
$users = @(Get-PropertyValue $target "users" @())
if ($users.Count -gt 0) {
    $username = [string]$users[0].username
    $password = [string]$users[0].password
}

$hasAnyTLSOverride = $AnyTLSClientOverride -ne "preserve" -or $AnyTLSAlpnOverride -ne "preserve" -or $AnyTLSUtlsOverride -ne "preserve" -or $RemoveAnyTLSDetour
if ($hasAnyTLSOverride) {
    $mainOutbound = @($outbounds | Where-Object { (Get-PropertyValue $_ "tag" "") -eq "proxy" })
    if ($mainOutbound.Count -ne 1 -or (Get-PropertyValue $mainOutbound[0] "type" "") -ne "anytls") {
        throw "AnyTLS diagnostic overrides require exactly one main outbound tagged 'proxy' with type 'anytls'."
    }
    if ($AnyTLSClientOverride -eq "native") {
        $mainOutbound[0].PSObject.Properties.Remove("client")
    } elseif ($AnyTLSClientOverride -eq "mihomo") {
        if ($mainOutbound[0].PSObject.Properties.Name -contains "client") {
            $mainOutbound[0].client = "mihomo/1.19.28"
        } else {
            $mainOutbound[0] | Add-Member -NotePropertyName "client" -NotePropertyValue "mihomo/1.19.28"
        }
    }
    if ($RemoveAnyTLSDetour) {
        $mainOutbound[0].PSObject.Properties.Remove("detour")
    }
    if ($AnyTLSAlpnOverride -ne "preserve") {
        $tls = Get-PropertyValue $mainOutbound[0] "tls" $null
        if ($null -eq $tls) {
            throw "AnyTLS ALPN override requires a TLS object on the main outbound."
        }
        if ($AnyTLSAlpnOverride -eq "none") {
            $tls.PSObject.Properties.Remove("alpn")
        } elseif ($tls.PSObject.Properties.Name -contains "alpn") {
            $tls.alpn = @("h2", "http/1.1")
        } else {
            $tls | Add-Member -NotePropertyName "alpn" -NotePropertyValue @("h2", "http/1.1")
        }
    }
    if ($AnyTLSUtlsOverride -ne "preserve") {
        $tls = Get-PropertyValue $mainOutbound[0] "tls" $null
        if ($null -eq $tls) {
            throw "AnyTLS uTLS override requires a TLS object on the main outbound."
        }
        if ($AnyTLSUtlsOverride -eq "none") {
            $tls.PSObject.Properties.Remove("utls")
        } else {
            $utls = [pscustomobject]@{
                enabled = $true
                fingerprint = "chrome"
            }
            if ($tls.PSObject.Properties.Name -contains "utls") {
                $tls.utls = $utls
            } else {
                $tls | Add-Member -NotePropertyName "utls" -NotePropertyValue $utls
            }
        }
    }
}
$config.outbounds = @(Get-StrictOutboundClosure $outbounds "proxy" ([bool]$AllowDirectTestOutbound))
if ($ForceAutoDetectInterface) {
    $diagnosticRoute = Get-PropertyValue $config "route" $null
    if ($null -eq $diagnosticRoute) {
        $config | Add-Member -NotePropertyName "route" -NotePropertyValue ([pscustomobject]@{})
        $diagnosticRoute = $config.route
    }
    if ($diagnosticRoute.PSObject.Properties.Name -contains "auto_detect_interface") {
        $diagnosticRoute.auto_detect_interface = $true
    } else {
        $diagnosticRoute | Add-Member -NotePropertyName "auto_detect_interface" -NotePropertyValue $true
    }
}
$variantConfigPath = Join-Path ([System.IO.Path]::GetTempPath()) "nekoray-mixed-probe-$([Guid]::NewGuid().ToString('N')).json"
$jsonText = $config | ConvertTo-Json -Depth 100
[System.IO.File]::WriteAllText($variantConfigPath, $jsonText, [System.Text.UTF8Encoding]::new($false))
$runConfigFull = $variantConfigPath

$probeId = [Guid]::NewGuid().ToString("N")
$stdoutPath = Join-Path ([System.IO.Path]::GetTempPath()) "nekoray-mixed-$probeId.stdout.log"
$stderrPath = Join-Path ([System.IO.Path]::GetTempPath()) "nekoray-mixed-$probeId.stderr.log"
$process = $null
$listenerReady = $false
$httpResult = $null
$httpConnectResult = $null
$socksResult = $null
$plainLog = @()
$coreAliveWhenListening = $false
$coreAliveAfterProbes = $false
$listenerOwnedByCore = $false
$listenerOwnedAfterProbes = $false
$coreExitCode = $null
$stoppedByProbe = $false

try {
    $process = Start-Process `
        -FilePath $coreFull `
        -ArgumentList "run -c `"$runConfigFull`"" `
        -WorkingDirectory (Split-Path -Parent $coreFull) `
        -WindowStyle Hidden `
        -RedirectStandardOutput $stdoutPath `
        -RedirectStandardError $stderrPath `
        -PassThru

    $deadline = [DateTime]::UtcNow.AddSeconds([Math]::Min(10, $TimeoutSeconds))
    while ([DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 200
        if ($process.HasExited) {
            break
        }
        $ownedListeners = @(Get-ListeningConnections $listenPort | Where-Object {
            $_.OwningProcess -eq $process.Id -and $_.LocalAddress -eq $listenAddress
        })
        if ($ownedListeners.Count -gt 0) {
            $listenerReady = $true
            $listenerOwnedByCore = $true
            $coreAliveWhenListening = !$process.HasExited
            break
        }
    }

    if ($listenerReady) {
        $httpResult = Invoke-ProxyProbe "http" $listenAddress $listenPort $TestUrl $TimeoutSeconds $ExpectedHttpStatus $username $password
        $httpConnectResult = Invoke-ProxyProbe "http" $listenAddress $listenPort $ConnectTestUrl $TimeoutSeconds $ExpectedHttpStatus $username $password $true
        $socksResult = Invoke-ProxyProbe "socks5h" $listenAddress $listenPort $TestUrl $TimeoutSeconds $ExpectedHttpStatus $username $password
        Start-Sleep -Milliseconds 500
        $coreAliveAfterProbes = !$process.HasExited
        $listenerOwnedAfterProbes = $coreAliveAfterProbes -and @(
            Get-ListeningConnections $listenPort | Where-Object {
                $_.OwningProcess -eq $process.Id -and $_.LocalAddress -eq $listenAddress
            }
        ).Count -gt 0
    }
} finally {
    if ($null -ne $process) {
        if ($process.HasExited) {
            $coreExitCode = $process.ExitCode
        } else {
            $stoppedByProbe = $true
            Stop-Process -Id $process.Id -Force
            $process.WaitForExit(5000) | Out-Null
        }
    }

    foreach ($path in @($stdoutPath, $stderrPath)) {
        if (Test-Path -LiteralPath $path -PathType Leaf) {
            $plainLog += Get-Content -LiteralPath $path -Encoding UTF8 | ForEach-Object {
                $_ -replace "\x1b\[[0-9;]*m", ""
            }
        }
    }
    if (![string]::IsNullOrEmpty($variantConfigPath) -and (Test-Path -LiteralPath $variantConfigPath -PathType Leaf)) {
        Remove-Item -LiteralPath $variantConfigPath -Force
    }
}

[object[]] $retainedLogPaths = @()
if ($KeepLogs) {
    $retainedLogPaths = @($stdoutPath, $stderrPath)
}

$result = [pscustomobject]@{
    config = $configFull
    core = $coreFull
    inbound_tag = $InboundTag
    anytls_client_override = $AnyTLSClientOverride
    anytls_alpn_override = $AnyTLSAlpnOverride
    anytls_utls_override = $AnyTLSUtlsOverride
    anytls_detour_removed = [bool]$RemoveAnyTLSDetour
    auto_detect_interface_forced = [bool]$ForceAutoDetectInterface
    listen_address = $listenAddress
    listen_port = $listenPort
    expected_http_status = $ExpectedHttpStatus
    original_inbound_count = $originalInboundCount
    diagnostic_inbound_count = 1
    diagnostic_outbound_count = @($config.outbounds).Count
    removed_other_inbound_count = [Math]::Max(0, $originalInboundCount - 1)
    process_spawned = $null -ne $process
    core_pid = if ($null -ne $process) { $process.Id } else { $null }
    core_alive_when_listening = $coreAliveWhenListening
    core_alive_after_probes = $coreAliveAfterProbes
    core_exit_code = $coreExitCode
    stopped_by_probe = $stoppedByProbe
    listener_ready = $listenerReady
    listener_owned_by_core = $listenerOwnedByCore
    listener_owned_after_probes = $listenerOwnedAfterProbes
    http = $httpResult
    http_connect = $httpConnectResult
    socks5h = $socksResult
    evidence = [pscustomobject]@{
        mixed_events = @($plainLog | Where-Object { $_ -match "inbound/mixed\[$([Regex]::Escape($InboundTag))\]" }).Count
        error_lines = @($plainLog | Where-Object { $_ -match "ERROR|FATAL|panic|failed" }).Count
        anytls_eof = @($plainLog | Where-Object { $_ -match "anytls.*EOF|failed to create session: EOF" }).Count
        anytls_session_error = @($plainLog | Where-Object { $_ -match "outbound/anytls|failed to create session" -and $_ -match "ERROR|failed|EOF" }).Count
        tls_handshake_error = @($plainLog | Where-Object { $_ -match "TLS handshake|handshake failed|failed to handshake|remote error: tls|tls:" }).Count
        tcp_dial_error = @($plainLog | Where-Object { $_ -match "dial tcp|connectex|connection refused|connection timed out|i/o timeout|network is unreachable" }).Count
        inbound_auth_error = @($plainLog | Where-Object { $_ -match "inbound/mixed" -and $_ -match "auth|authentication" -and $_ -match "ERROR|failed|denied" }).Count
        resolver_unavailable = @($plainLog | Where-Object { $_ -match "no available primary DoH resolver" }).Count
        startup_error = @($plainLog | Where-Object { $_ -match "start service:|initialize inbound|listen tcp.*failed" }).Count
        selected_interface = @($plainLog | Where-Object { $_ -match "network: updated default interface" }).Count
        proxy_outbound_events = @($plainLog | Where-Object { $_ -match "outbound/[^\[]+\[proxy\]" }).Count
    }
    log_paths = $retainedLogPaths
}

if (!$KeepLogs) {
    foreach ($path in @($stdoutPath, $stderrPath)) {
        if (Test-Path -LiteralPath $path -PathType Leaf) {
            Remove-Item -LiteralPath $path -Force
        }
    }
}
if ($Json) {
    $result | ConvertTo-Json -Depth 8
} else {
    $result | Format-List
    if ($null -ne $httpResult) {
        $httpResult | Format-Table -AutoSize
    }
    if ($null -ne $httpConnectResult) {
        $httpConnectResult | Format-Table -AutoSize
    }
    if ($null -ne $socksResult) {
        $socksResult | Format-Table -AutoSize
    }
    $result.evidence | Format-List
}

if (!$listenerReady -or !$listenerOwnedByCore -or !$listenerOwnedAfterProbes -or !$coreAliveAfterProbes) {
    exit 2
}
if (!$httpResult.success -or !$httpConnectResult.success -or !$socksResult.success -or
    $result.evidence.startup_error -gt 0 -or $result.evidence.mixed_events -lt 3 -or $result.evidence.proxy_outbound_events -lt 1) {
    exit 1
}
exit 0
