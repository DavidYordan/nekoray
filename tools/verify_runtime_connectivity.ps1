param(
    [string] $PackageDir = "",
    [string] $TestUrl = "",
    [int] $TimeoutSeconds = 10,
    [switch] $ExpectRunning,
    [switch] $ExpectSystemProxy,
    [switch] $ExpectAuxiliary,
    [switch] $SkipCurl,
    [string] $OutputPath = "",
    [switch] $Json
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$Root = [System.IO.Path]::GetFullPath((Join-Path $ScriptDir ".."))
if ([string]::IsNullOrWhiteSpace($PackageDir)) {
    $PackageDir = Join-Path $Root "deployment\windows64"
}
$PackageDir = [System.IO.Path]::GetFullPath($PackageDir)

function Write-Step([string] $Message) {
    if (!$Json) {
        Write-Host ""
        Write-Host "==> $Message" -ForegroundColor Cyan
    }
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

function Test-ValidPort($Port) {
    return ($Port -is [int] -or $Port -is [long]) -and $Port -ge 1 -and $Port -le 65535
}

function Read-JsonFile([string] $Path) {
    if (!(Test-Path -LiteralPath $Path -PathType Leaf)) {
        return $null
    }
    try {
        return Get-Content -LiteralPath $Path -Raw -Encoding UTF8 | ConvertFrom-Json
    } catch {
        return [pscustomobject]@{ __error = $_.Exception.Message }
    }
}

function Read-ProjectConfig([string] $Dir) {
    $configPath = Join-Path $Dir "config\groups\nekobox.json"
    $raw = Read-JsonFile $configPath
    $inboundPort = [int](Get-PropertyValue $raw "inbound_socks_port" 12080)
    $inboundAddress = [string](Get-PropertyValue $raw "inbound_address" "127.0.0.1")
    $clashApi = [int](Get-PropertyValue $raw "core_box_clash_api" -19090)
    $configuredTestUrl = [string](Get-PropertyValue $raw "test_url" "http://cp.cloudflare.com/")
    $auxEntries = @()
    $entries = Get-PropertyValue $raw "aux_profile_ports" @()
    foreach ($entry in @($entries)) {
        if ($null -eq $entry) {
            continue
        }
        $parts = ([string]$entry).Split(":")
        if ($parts.Count -ne 2) {
            continue
        }
        $profileId = 0
        $port = 0
        if ([int]::TryParse($parts[0].Trim(), [ref]$profileId) -and [int]::TryParse($parts[1].Trim(), [ref]$port) -and (Test-ValidPort $port)) {
            $auxEntries += [pscustomobject]@{
                profile_id = $profileId
                port = $port
            }
        }
    }

    return [pscustomobject]@{
        path = $configPath
        exists = (Test-Path -LiteralPath $configPath -PathType Leaf)
        error = [string](Get-PropertyValue $raw "__error" "")
        inbound_socks_port = $inboundPort
        inbound_address = $inboundAddress
        core_box_clash_api = $clashApi
        clash_api_enabled = $clashApi -gt 0
        clash_api_port = [Math]::Abs($clashApi)
        clash_api_secret_configured = -not [string]::IsNullOrEmpty([string](Get-PropertyValue $raw "core_box_clash_api_secret" ""))
        test_url = $configuredTestUrl
        aux_profile_ports = @($auxEntries)
    }
}

function Read-Profiles([string] $Dir) {
    $profileDir = Join-Path $Dir "config\profiles"
    $result = @{}
    if (!(Test-Path -LiteralPath $profileDir -PathType Container)) {
        return $result
    }
    foreach ($file in Get-ChildItem -LiteralPath $profileDir -Filter "*.json" -File) {
        $raw = Read-JsonFile $file.FullName
        if ($null -eq $raw) {
            continue
        }
        $id = [int](Get-PropertyValue $raw "id" -1)
        if ($id -lt 0) {
            continue
        }
        $bean = Get-PropertyValue $raw "bean" $null
        $result[$id] = [pscustomobject]@{
            id = $id
            gid = [int](Get-PropertyValue $raw "gid" -1)
            type = [string](Get-PropertyValue $raw "type" "")
            name = [string](Get-PropertyValue $bean "name" "")
            address = [string](Get-PropertyValue $bean "addr" "")
        }
    }
    return $result
}

function Get-SystemProxySnapshot {
    $key = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Internet Settings"
    try {
        $item = Get-ItemProperty -LiteralPath $key -ErrorAction Stop
        return [pscustomobject]@{
            proxy_enable = [int](Get-PropertyValue $item "ProxyEnable" 0)
            proxy_server = [string](Get-PropertyValue $item "ProxyServer" "")
            auto_config_url = [string](Get-PropertyValue $item "AutoConfigURL" "")
            proxy_override = [string](Get-PropertyValue $item "ProxyOverride" "")
        }
    } catch {
        return [pscustomobject]@{
            error = $_.Exception.Message
            proxy_enable = $null
            proxy_server = ""
            auto_config_url = ""
            proxy_override = ""
        }
    }
}

function Get-ProcessInfosUnder([string] $Dir) {
    $rootPath = [System.IO.Path]::GetFullPath($Dir).TrimEnd('\')
    if (!(Test-Path -LiteralPath $rootPath -PathType Container)) {
        return @()
    }
    @(Get-CimInstance Win32_Process |
        Where-Object {
            $_.ExecutablePath -and
            [System.IO.Path]::GetFullPath($_.ExecutablePath).StartsWith($rootPath + '\', [System.StringComparison]::OrdinalIgnoreCase)
        } |
        ForEach-Object {
            [pscustomobject]@{
                pid = [int]$_.ProcessId
                name = [string]$_.Name
                executable = [string]$_.ExecutablePath
                command_line = [string]$_.CommandLine
            }
        })
}

function Test-TcpPort([string] $HostName, [int] $Port, [int] $TimeoutMs) {
    $client = New-Object System.Net.Sockets.TcpClient
    try {
        $async = $client.BeginConnect($HostName, $Port, $null, $null)
        if (!$async.AsyncWaitHandle.WaitOne($TimeoutMs, $false)) {
            return $false
        }
        $client.EndConnect($async)
        return $true
    } catch {
        return $false
    } finally {
        $client.Close()
    }
}

function Get-PortOwner([int] $Port) {
    try {
        $conn = Get-NetTCPConnection -State Listen -LocalPort $Port -ErrorAction Stop |
            Where-Object { $_.LocalAddress -in @("127.0.0.1", "0.0.0.0", "::", "::1") } |
            Select-Object -First 1
        if ($null -eq $conn) {
            return $null
        }
        $proc = $null
        try {
            $proc = Get-Process -Id $conn.OwningProcess -ErrorAction Stop
        } catch {
        }
        return [pscustomobject]@{
            pid = [int]$conn.OwningProcess
            process_name = if ($null -eq $proc) { "" } else { [string]$proc.ProcessName }
            local_address = [string]$conn.LocalAddress
        }
    } catch {
        return $null
    }
}

function Invoke-Curl([string[]] $Arguments) {
    if (!(Get-Command curl.exe -ErrorAction SilentlyContinue)) {
        return [pscustomobject]@{
            skipped = $true
            ok = $false
            error = "curl.exe not found"
            exit_code = $null
            output = ""
        }
    }
    $output = @()
    try {
        $output = & curl.exe @Arguments 2>&1
        $exitCode = $LASTEXITCODE
        return [pscustomobject]@{
            skipped = $false
            ok = ($exitCode -eq 0)
            error = if ($exitCode -eq 0) { "" } else { ($output -join "`n") }
            exit_code = $exitCode
            output = ($output -join "`n")
        }
    } catch {
        return [pscustomobject]@{
            skipped = $false
            ok = $false
            error = $_.Exception.Message
            exit_code = $null
            output = ($output -join "`n")
        }
    }
}

function Test-ProxyPort([string] $Name, [int] $Port, [string] $Url, [int] $TimeoutSeconds, [switch] $SkipCurl) {
    $listen = Test-TcpPort "127.0.0.1" $Port 1500
    $owner = Get-PortOwner $Port
    $socks = $null
    $http = $null
    if (!$SkipCurl -and $listen) {
        $writeOut = "http=%{http_code} time=%{time_total} remote=%{remote_ip}"
        $baseArgs = @("--silent", "--show-error", "--max-time", [string]$TimeoutSeconds, "--connect-timeout", [string][Math]::Min(5, $TimeoutSeconds), "--output", "NUL", "--write-out", $writeOut)
        $socks = Invoke-Curl (@("--proxy", "socks5h://127.0.0.1:$Port") + $baseArgs + @($Url))
        $http = Invoke-Curl (@("--proxy", "http://127.0.0.1:$Port") + $baseArgs + @($Url))
    }
    return [pscustomobject]@{
        name = $Name
        port = $Port
        listen = $listen
        owner = $owner
        curl_socks5h = $socks
        curl_http = $http
    }
}

function Test-ClashApi([int] $Port, [string] $Secret, [int] $TimeoutSeconds, [switch] $SkipCurl) {
    $listen = Test-TcpPort "127.0.0.1" $Port 1500
    $owner = Get-PortOwner $Port
    $version = $null
    if (!$SkipCurl -and $listen) {
        $args = @("--silent", "--show-error", "--max-time", [string]$TimeoutSeconds, "--connect-timeout", [string][Math]::Min(5, $TimeoutSeconds))
        if (![string]::IsNullOrEmpty($Secret)) {
            $args += @("-H", "Authorization: Bearer $Secret")
        }
        $args += @("http://127.0.0.1:$Port/version")
        $version = Invoke-Curl $args
    }
    return [pscustomobject]@{
        port = $Port
        listen = $listen
        owner = $owner
        version = $version
    }
}

function SystemProxyTargetsPort($Snapshot, [int] $Port) {
    if ($null -eq $Snapshot -or $Snapshot.proxy_enable -ne 1) {
        return $false
    }
    $server = [string]$Snapshot.proxy_server
    return $server -match "127\.0\.0\.1:$Port" -or $server -match "localhost:$Port"
}

Write-Step "Collect runtime state"
$config = Read-ProjectConfig $PackageDir
$profiles = Read-Profiles $PackageDir
if ([string]::IsNullOrWhiteSpace($TestUrl)) {
    $TestUrl = $config.test_url
}
if ([string]::IsNullOrWhiteSpace($TestUrl)) {
    $TestUrl = "http://cp.cloudflare.com/"
}

$processes = @(Get-ProcessInfosUnder $PackageDir)
$systemProxy = Get-SystemProxySnapshot
$ports = @()
$ports += Test-ProxyPort "main" $config.inbound_socks_port $TestUrl $TimeoutSeconds -SkipCurl:$SkipCurl
$auxProfilePorts = @($config.aux_profile_ports)
foreach ($aux in $auxProfilePorts) {
    $profile = $null
    if ($profiles.ContainsKey([int]$aux.profile_id)) {
        $profile = $profiles[[int]$aux.profile_id]
    }
    $name = if ($null -eq $profile -or [string]::IsNullOrWhiteSpace($profile.name)) {
        "aux profile #$($aux.profile_id)"
    } else {
        "aux #$($aux.profile_id) $($profile.name)"
    }
    $ports += Test-ProxyPort $name ([int]$aux.port) $TestUrl $TimeoutSeconds -SkipCurl:$SkipCurl
}

$clashApi = $null
if ($config.clash_api_enabled) {
    $raw = Read-JsonFile $config.path
    $secret = [string](Get-PropertyValue $raw "core_box_clash_api_secret" "")
    $clashApi = Test-ClashApi $config.clash_api_port $secret $TimeoutSeconds -SkipCurl:$SkipCurl
}

$warnings = New-Object System.Collections.Generic.List[string]
$failures = New-Object System.Collections.Generic.List[string]

if ($processes.Count -eq 0) {
    $warnings.Add("No project process was found under $PackageDir.")
}
if ($ExpectRunning -and !$ports[0].listen) {
    $failures.Add("Main proxy port $($config.inbound_socks_port) is not listening.")
} elseif (!$ports[0].listen) {
    $warnings.Add("Main proxy port $($config.inbound_socks_port) is not listening; start a main profile before runtime connectivity validation.")
}
if ($ExpectAuxiliary -and $auxProfilePorts.Count -eq 0) {
    $failures.Add("No persisted auxiliary proxy port is configured.")
} elseif ($auxProfilePorts.Count -eq 0) {
    $warnings.Add("No persisted auxiliary proxy ports were found.")
}
foreach ($portResult in @($ports | Select-Object -Skip 1)) {
    if (!$portResult.listen) {
        $message = "Auxiliary proxy port $($portResult.port) is not listening."
        if ($ExpectAuxiliary) {
            $failures.Add($message)
        } else {
            $warnings.Add($message)
        }
    }
}
if ($ExpectSystemProxy -and !(SystemProxyTargetsPort $systemProxy $config.inbound_socks_port)) {
    $failures.Add("Windows system proxy is not enabled for project main port $($config.inbound_socks_port).")
}
if ($config.clash_api_enabled -and $null -ne $clashApi -and !$clashApi.listen) {
    $warnings.Add("Clash API is enabled in config but port $($config.clash_api_port) is not listening.")
}
if (!$SkipCurl) {
    foreach ($portResult in $ports) {
        if ($portResult.listen) {
            if ($null -ne $portResult.curl_socks5h -and !$portResult.curl_socks5h.ok) {
                $failures.Add("$($portResult.name) socks5h curl failed on port $($portResult.port): $($portResult.curl_socks5h.error)")
            }
            if ($null -ne $portResult.curl_http -and !$portResult.curl_http.ok) {
                $failures.Add("$($portResult.name) http curl failed on port $($portResult.port): $($portResult.curl_http.error)")
            }
        }
    }
    if ($null -ne $clashApi -and $clashApi.listen -and $null -ne $clashApi.version -and !$clashApi.version.ok) {
        $failures.Add("Clash API /version request failed on port $($config.clash_api_port): $($clashApi.version.error)")
    }
}

$status = if ($failures.Count -eq 0) { "OK" } else { "FAILED" }
$report = [ordered]@{
    schema = "nekoray.runtime_connectivity_audit.v1"
    timestamp = (Get-Date).ToString("o")
    package_dir = $PackageDir
    test_url = $TestUrl
    status = $status
    config = $config
    system_proxy = $systemProxy
    project_processes = @($processes)
    proxy_ports = @($ports)
    clash_api = $clashApi
    warnings = @($warnings)
    failures = @($failures)
}

if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    $auditDir = Join-Path $PackageDir "runtime_audit"
    New-Item -ItemType Directory -Force -Path $auditDir | Out-Null
    $stamp = Get-Date -Format "yyyyMMdd_HHmmss_fff"
    $OutputPath = Join-Path $auditDir "runtime_$stamp.json"
}

$jsonText = $report | ConvertTo-Json -Depth 8
Set-Content -LiteralPath $OutputPath -Value $jsonText -Encoding UTF8

if ($Json) {
    $jsonText
} else {
    Write-Step "Result"
    if ($status -eq "OK") {
        Write-Host "OK: runtime connectivity audit completed." -ForegroundColor Green
    } else {
        Write-Host "FAILED: runtime connectivity audit found failures." -ForegroundColor Red
    }
    Write-Host "Report: $OutputPath"
    Write-Host "Package: $PackageDir"
    Write-Host "Main proxy: 127.0.0.1:$($config.inbound_socks_port) listen=$($ports[0].listen)"
    if ($auxProfilePorts.Count -gt 0) {
        foreach ($portResult in @($ports | Select-Object -Skip 1)) {
            Write-Host "Aux proxy:  127.0.0.1:$($portResult.port) listen=$($portResult.listen) name=$($portResult.name)"
        }
    }
    if ($config.clash_api_enabled) {
        Write-Host "Clash API:  127.0.0.1:$($config.clash_api_port) listen=$($clashApi.listen)"
    } else {
        Write-Host "Clash API:  disabled in config"
    }
    Write-Host "System proxy enabled: $($systemProxy.proxy_enable)"
    Write-Host "System proxy server:  $($systemProxy.proxy_server)"
    if ($warnings.Count -gt 0) {
        Write-Host ""
        Write-Host "Warnings:"
        foreach ($warning in $warnings) {
            Write-Host "- $warning"
        }
    }
    if ($failures.Count -gt 0) {
        Write-Host ""
        Write-Host "Failures:"
        foreach ($failure in $failures) {
            Write-Host "- $failure"
        }
    }
}

if ($failures.Count -gt 0) {
    exit 1
}
