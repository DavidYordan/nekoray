param(
    [string] $PackageDir = "",
    [string] $ProductionDir = "D:\Program Files\nekoray",
    [int] $MonitorSeconds = 0,
    [int] $PollMilliseconds = 250,
    [int] $ExpectedProxyPort = 0,
    [switch] $StrictProjectProxy,
    [switch] $ExpectTunStable,
    [string] $OutputPath = "",
    [switch] $Json
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$Root = [System.IO.Path]::GetFullPath((Join-Path $ScriptDir ".."))
. (Join-Path $ScriptDir "path_safety.ps1")
if ([string]::IsNullOrWhiteSpace($PackageDir)) {
    $PackageDir = Join-Path $Root "deployment\windows64"
}
$PackageDir = [System.IO.Path]::GetFullPath($PackageDir)
$PackageDir = Assert-PathOutsideProtectedProduction $PackageDir "Fail-closed audit package directory"
$ProductionDir = [System.IO.Path]::GetFullPath($ProductionDir)

function Write-Step([string] $Message) {
    if (!$Json) {
        Write-Host ""
        Write-Host "==> $Message" -ForegroundColor Cyan
    }
}

function Read-ProjectConfig([string] $Dir) {
    $configPath = Join-Path $Dir "config\groups\nekobox.json"
    $result = [ordered]@{
        path = $configPath
        exists = $false
        inbound_socks_port = $null
        inbound_address = $null
        core_box_clash_api = $null
        vpn_internal_tun = $null
        remember_enable = $null
        remember_id = $null
        remember_spmode = @()
    }

    if (!(Test-Path -LiteralPath $configPath -PathType Leaf)) {
        return [pscustomobject]$result
    }

    $result["exists"] = $true
    try {
        $config = Get-Content -LiteralPath $configPath -Raw -Encoding UTF8 | ConvertFrom-Json
        foreach ($name in @("inbound_socks_port", "inbound_address", "core_box_clash_api", "vpn_internal_tun", "remember_enable", "remember_id")) {
            if ($config.PSObject.Properties.Name -contains $name) {
                $result[$name] = $config.$name
            }
        }
        if ($config.PSObject.Properties.Name -contains "spmode2" -and $null -ne $config.spmode2) {
            $result["remember_spmode"] = @($config.spmode2)
        }
    } catch {
        $result["error"] = $_.Exception.Message
    }

    return [pscustomobject]$result
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

function Get-SystemProxySnapshot {
    $key = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Internet Settings"
    $item = $null
    try {
        $item = Get-ItemProperty -LiteralPath $key -ErrorAction Stop
    } catch {
        return [pscustomobject]@{
            key = $key
            error = $_.Exception.Message
            proxy_enable = $null
            proxy_server = ""
            auto_config_url = ""
            proxy_override = ""
        }
    }

    return [pscustomobject]@{
        key = $key
        proxy_enable = [int](Get-PropertyValue $item "ProxyEnable" 0)
        proxy_server = [string](Get-PropertyValue $item "ProxyServer" "")
        auto_config_url = [string](Get-PropertyValue $item "AutoConfigURL" "")
        proxy_override = [string](Get-PropertyValue $item "ProxyOverride" "")
    }
}

function Get-WinHttpProxySnapshot {
    try {
        return ((& netsh winhttp show proxy) -join "`n").Trim()
    } catch {
        return "netsh winhttp show proxy failed: $($_.Exception.Message)"
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
        Select-Object @{
            Name = "name"
            Expression = { $_.Name }
        }, @{
            Name = "pid"
            Expression = { [int]$_.ProcessId }
        }, @{
            Name = "path"
            Expression = { $_.ExecutablePath }
        }, @{
            Name = "command_line"
            Expression = { $_.CommandLine }
        })
}

function Get-ProjectProcessSnapshot {
    $processes = @(Get-ProcessInfosUnder $PackageDir)
    $externalTun = @($processes | Where-Object {
            $_.path -and
            [System.IO.Path]::GetFileName($_.path).Equals("nekobox_core.exe", [System.StringComparison]::OrdinalIgnoreCase) -and
            $_.command_line -match "sing-box-vpn\.json"
        })
    $mainCore = @($processes | Where-Object {
            $_.path -and
            [System.IO.Path]::GetFileName($_.path).Equals("nekobox_core.exe", [System.StringComparison]::OrdinalIgnoreCase) -and
            $_.command_line -match "\bnekobox\b" -and
            $_.command_line -match "\b-port\b"
        })
    $gui = @($processes | Where-Object {
            $_.path -and
            [System.IO.Path]::GetFileName($_.path).Equals("nekobox.exe", [System.StringComparison]::OrdinalIgnoreCase)
        })

    return [pscustomobject]@{
        package_dir = $PackageDir
        processes = $processes
        gui_pids = @($gui | ForEach-Object { $_.pid })
        main_core_pids = @($mainCore | ForEach-Object { $_.pid })
        external_tun_core_pids = @($externalTun | ForEach-Object { $_.pid })
    }
}

function Get-PortListeners([int[]] $Ports) {
    $validPorts = @($Ports | Where-Object { $_ -gt 0 } | Sort-Object -Unique)
    if ($validPorts.Count -eq 0) {
        return @()
    }

    try {
        @(Get-NetTCPConnection -State Listen -ErrorAction Stop |
            Where-Object { $validPorts -contains [int]$_.LocalPort } |
            Sort-Object LocalPort, LocalAddress |
            Select-Object @{
                Name = "local_address"
                Expression = { $_.LocalAddress }
            }, @{
                Name = "local_port"
                Expression = { [int]$_.LocalPort }
            }, @{
                Name = "owning_process"
                Expression = { [int]$_.OwningProcess }
            })
    } catch {
        @([pscustomobject]@{ error = $_.Exception.Message })
    }
}

function Get-NetworkSnapshot {
    $tunRegex = "tun|tap|wintun|wireguard|nekobox|nekoray|sing-box|singbox"
    $adapters = @()
    try {
        $adapters = @(Get-NetAdapter -IncludeHidden -ErrorAction Stop |
            Where-Object { $_.Name -match $tunRegex -or $_.InterfaceDescription -match $tunRegex } |
            Sort-Object ifIndex |
            Select-Object @{
                Name = "name"
                Expression = { $_.Name }
            }, @{
                Name = "description"
                Expression = { $_.InterfaceDescription }
            }, @{
                Name = "status"
                Expression = { $_.Status }
            }, @{
                Name = "if_index"
                Expression = { [int]$_.ifIndex }
            })
    } catch {
        $adapters = @([pscustomobject]@{ error = $_.Exception.Message })
    }

    $defaultRoutes = @()
    try {
        $defaultRoutes = @(Get-NetRoute -AddressFamily IPv4 -DestinationPrefix "0.0.0.0/0" -ErrorAction Stop |
            Sort-Object RouteMetric, InterfaceMetric, InterfaceIndex |
            Select-Object @{
                Name = "interface_alias"
                Expression = { $_.InterfaceAlias }
            }, @{
                Name = "interface_index"
                Expression = { [int]$_.InterfaceIndex }
            }, @{
                Name = "next_hop"
                Expression = { $_.NextHop }
            }, @{
                Name = "route_metric"
                Expression = { [int]$_.RouteMetric }
            }, @{
                Name = "interface_metric"
                Expression = { [int]$_.InterfaceMetric }
            }, @{
                Name = "policy_store"
                Expression = { $_.PolicyStore }
            })
    } catch {
        $defaultRoutes = @([pscustomobject]@{ error = $_.Exception.Message })
    }

    return [pscustomobject]@{
        tun_like_adapters = $adapters
        default_ipv4_routes = $defaultRoutes
    }
}

function Test-ProxyServerUsesPort([string] $ProxyServer, [int] $Port) {
    if ($Port -le 0 -or [string]::IsNullOrWhiteSpace($ProxyServer)) {
        return $false
    }
    $escapedPort = [regex]::Escape([string]$Port)
    $pattern = "(?i)(^|[=;\s])(?:127\.0\.0\.1|localhost|\[::1\]|::1):$escapedPort($|[;\s])"
    return $ProxyServer -match $pattern
}

function New-Sample([int[]] $Ports) {
    return [pscustomobject]@{
        timestamp = (Get-Date).ToString("o")
        system_proxy = Get-SystemProxySnapshot
        winhttp_proxy = Get-WinHttpProxySnapshot
        project = Get-ProjectProcessSnapshot
        production_processes = @(Get-ProcessInfosUnder $ProductionDir)
        listeners = @(Get-PortListeners $Ports)
        network = Get-NetworkSnapshot
    }
}

function Array-Of-Ints($Value) {
    @($Value | ForEach-Object { [int]$_ })
}

function Analyze-Samples($Config, $Samples, [int] $ProxyPort) {
    $issues = New-Object System.Collections.Generic.List[string]
    $warnings = New-Object System.Collections.Generic.List[string]
    $baseline = $Samples[0]
    $baselineProxy = $baseline.system_proxy
    $baselineExternalTunPids = @(Array-Of-Ints $baseline.project.external_tun_core_pids)
    $baselineProductionPids = @(Array-Of-Ints ($baseline.production_processes | ForEach-Object { $_.pid }))

    if ($baselineProxy.proxy_enable -eq 1) {
        foreach ($sample in $Samples) {
            if ($sample.system_proxy.proxy_enable -ne 1) {
                $issues.Add("System proxy was enabled at baseline but became disabled at $($sample.timestamp).")
            }
            if ([string]$sample.system_proxy.proxy_server -ne [string]$baselineProxy.proxy_server) {
                $issues.Add("System proxy server changed at $($sample.timestamp): '$($baselineProxy.proxy_server)' -> '$($sample.system_proxy.proxy_server)'.")
            }
            if ([string]$sample.system_proxy.auto_config_url -ne [string]$baselineProxy.auto_config_url) {
                $issues.Add("System proxy AutoConfigURL changed at $($sample.timestamp).")
            }
        }
    } else {
        $warnings.Add("System proxy was disabled at baseline; proxy fail-closed behavior cannot be verified from this run.")
    }

    if ($ProxyPort -gt 0) {
        $baselineUsesExpectedPort = Test-ProxyServerUsesPort $baselineProxy.proxy_server $ProxyPort
        if (!$baselineUsesExpectedPort) {
            $message = "Baseline system proxy does not point to expected project port $ProxyPort. Current value: '$($baselineProxy.proxy_server)'."
            if ($StrictProjectProxy) {
                $issues.Add($message)
            } else {
                $warnings.Add($message)
            }
        } elseif ($baselineProxy.proxy_enable -eq 1) {
            foreach ($sample in $Samples) {
                if (!(Test-ProxyServerUsesPort $sample.system_proxy.proxy_server $ProxyPort)) {
                    $issues.Add("System proxy stopped pointing to expected project port $ProxyPort at $($sample.timestamp).")
                }
            }
        }
    }

    if ($baselineExternalTunPids.Count -gt 0) {
        foreach ($sample in $Samples) {
            $samplePids = @(Array-Of-Ints $sample.project.external_tun_core_pids)
            foreach ($processId in $baselineExternalTunPids) {
                if ($samplePids -notcontains $processId) {
                    $issues.Add("Project external Tun core process $processId disappeared at $($sample.timestamp).")
                }
            }
        }
    } else {
        $warnings.Add("No project external Tun core process was detected at baseline.")
    }

    if ($ExpectTunStable) {
        $baselineAdapters = @($baseline.network.tun_like_adapters | Where-Object { -not ($_.PSObject.Properties.Name -contains "error") } | ForEach-Object { $_.name })
        $baselineRoutesJson = ($baseline.network.default_ipv4_routes | ConvertTo-Json -Depth 8 -Compress)
        foreach ($sample in $Samples) {
            $sampleAdapters = @($sample.network.tun_like_adapters | Where-Object { -not ($_.PSObject.Properties.Name -contains "error") } | ForEach-Object { $_.name })
            foreach ($adapterName in $baselineAdapters) {
                if ($sampleAdapters -notcontains $adapterName) {
                    $issues.Add("Tun-like adapter '$adapterName' disappeared at $($sample.timestamp).")
                }
            }
            $sampleRoutesJson = ($sample.network.default_ipv4_routes | ConvertTo-Json -Depth 8 -Compress)
            if ($sampleRoutesJson -ne $baselineRoutesJson) {
                $issues.Add("Default IPv4 route set changed at $($sample.timestamp).")
            }
        }
    }

    if ($baselineProductionPids.Count -gt 0) {
        foreach ($sample in $Samples) {
            $sampleProductionPids = @(Array-Of-Ints ($sample.production_processes | ForEach-Object { $_.pid }))
            foreach ($processId in $baselineProductionPids) {
                if ($sampleProductionPids -notcontains $processId) {
                    $warnings.Add("Production process $processId from '$ProductionDir' disappeared during sampling; investigate manually because this script does not stop production processes.")
                }
            }
        }
    }

    return [pscustomobject]@{
        ok = $issues.Count -eq 0
        issues = @($issues)
        warnings = @($warnings | Sort-Object -Unique)
    }
}

if (!(Test-Path -LiteralPath $PackageDir -PathType Container)) {
    throw "PackageDir not found: $PackageDir"
}

$config = Read-ProjectConfig $PackageDir
if ($ExpectedProxyPort -le 0 -and $null -ne $config.inbound_socks_port) {
    $ExpectedProxyPort = [int]$config.inbound_socks_port
}

$ports = @($ExpectedProxyPort)
if ($null -ne $config.core_box_clash_api -and [int]$config.core_box_clash_api -gt 0) {
    $ports += [int]$config.core_box_clash_api
}
$ports = @($ports | Where-Object { $_ -gt 0 } | Sort-Object -Unique)

if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    $auditDir = Join-Path $PackageDir "fail_closed_audit"
    $OutputPath = Join-Path $auditDir ("fail_closed_{0}_{1}.json" -f (Get-Date -Format "yyyyMMdd_HHmmss_fff"), ([System.Guid]::NewGuid().ToString("N").Substring(0, 8)))
}
$OutputPath = Assert-NewFileOutsideProtectedProduction $OutputPath "Fail-closed audit report"
$outputDir = Split-Path -Parent $OutputPath
New-Item -ItemType Directory -Force -Path $outputDir | Out-Null

Write-Step "Collect baseline"
$samples = New-Object System.Collections.Generic.List[object]
$samples.Add((New-Sample $ports))

if ($MonitorSeconds -gt 0) {
    Write-Step "Monitor for $MonitorSeconds second(s)"
    $deadline = (Get-Date).AddSeconds($MonitorSeconds)
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds ([Math]::Max(100, $PollMilliseconds))
        $samples.Add((New-Sample $ports))
    }
}

$sampleArray = @($samples.ToArray())
$analysis = Analyze-Samples -Config $config -Samples $sampleArray -ProxyPort $ExpectedProxyPort
$report = [pscustomobject]@{
    schema = "nekoray.fail_closed_restart_audit.v1"
    created_at = (Get-Date).ToString("o")
    root = $Root
    package_dir = $PackageDir
    production_dir = $ProductionDir
    monitor_seconds = $MonitorSeconds
    poll_milliseconds = $PollMilliseconds
    expected_proxy_port = $ExpectedProxyPort
    strict_project_proxy = [bool]$StrictProjectProxy
    expect_tun_stable = [bool]$ExpectTunStable
    config = $config
    analysis = $analysis
    samples = $sampleArray
}

$reportJson = $report | ConvertTo-Json -Depth 16
$reportJson | Set-Content -LiteralPath $OutputPath -Encoding UTF8

if ($Json) {
    $reportJson
} else {
    Write-Step "Result"
    if ($analysis.ok) {
        Write-Host "OK: no fail-open transition detected." -ForegroundColor Green
    } else {
        Write-Host "FAILED: fail-open risk detected." -ForegroundColor Red
    }

    $baseline = $samples[0]
    Write-Host "Report: $OutputPath"
    Write-Host "Package: $PackageDir"
    Write-Host "System proxy enabled: $($baseline.system_proxy.proxy_enable)"
    Write-Host "System proxy server:  $($baseline.system_proxy.proxy_server)"
    Write-Host "Expected proxy port:  $ExpectedProxyPort"
    Write-Host "Project GUI PIDs:     $(@($baseline.project.gui_pids) -join ', ')"
    Write-Host "Main core PIDs:       $(@($baseline.project.main_core_pids) -join ', ')"
    Write-Host "External Tun PIDs:    $(@($baseline.project.external_tun_core_pids) -join ', ')"

    if ($analysis.warnings.Count -gt 0) {
        Write-Host ""
        Write-Host "Warnings:" -ForegroundColor Yellow
        foreach ($warning in $analysis.warnings) {
            Write-Host "- $warning"
        }
    }
    if ($analysis.issues.Count -gt 0) {
        Write-Host ""
        Write-Host "Issues:" -ForegroundColor Red
        foreach ($issue in $analysis.issues) {
            Write-Host "- $issue"
        }
    }
}

if (!$analysis.ok) {
    exit 1
}
