param(
    [string] $ExecutablePath = "build-package-windows64/nekobox.exe",
    [string] $CorePath = "deployment/windows64/nekobox_core.exe",
    [switch] $Json
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$Root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
. (Join-Path $Root "tools\path_safety.ps1")

$executableFull = if ([IO.Path]::IsPathRooted($ExecutablePath)) {
    [IO.Path]::GetFullPath($ExecutablePath)
} else {
    [IO.Path]::GetFullPath((Join-Path $Root $ExecutablePath))
}
$executableFull = Assert-PathOutsideProtectedProduction $executableFull "Auxiliary-route runtime GUI executable"
$coreFull = if ([IO.Path]::IsPathRooted($CorePath)) {
    [IO.Path]::GetFullPath($CorePath)
} else {
    [IO.Path]::GetFullPath((Join-Path $Root $CorePath))
}
$coreFull = Assert-PathOutsideProtectedProduction $coreFull "Auxiliary-route runtime core executable"
$proxyScript = Join-Path $PSScriptRoot "fixtures\http_proxy_line_server.py"
foreach ($required in @($executableFull, $coreFull, $proxyScript)) {
    if (!(Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Required file not found: $required"
    }
}

$mingwBin = Join-Path $Root "qtsdk\tools\Tools\mingw1310_64\bin"
$qtBin = Join-Path $Root "qtsdk\qt\6.5.3\mingw_64\bin"
$qtPlatformPlugins = Join-Path $Root "qtsdk\qt\6.5.3\mingw_64\plugins\platforms"
foreach ($requiredDirectory in @($mingwBin, $qtBin, $qtPlatformPlugins)) {
    if (!(Test-Path -LiteralPath $requiredDirectory -PathType Container)) {
        throw "Required Qt/MinGW directory not found: $requiredDirectory"
    }
}
$env:PATH = "$mingwBin;$qtBin;$env:PATH"
$env:QT_QPA_PLATFORM_PLUGIN_PATH = $qtPlatformPlugins

$ports = @(18119, 18120, 18121, 18130, 18131, 18132, 18133)
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

function ConvertTo-PemText([string] $Label, [byte[]] $Bytes) {
    $body = [Convert]::ToBase64String(
        $Bytes,
        [Base64FormattingOptions]::InsertLineBreaks)
    "-----BEGIN $Label-----`n$body`n-----END $Label-----`n"
}

function New-LoopbackCertificate([string] $CertificatePath, [string] $KeyPath) {
    $rsa = [Security.Cryptography.RSACng]::new(2048)
    $certificate = $null
    try {
        $request = [Security.Cryptography.X509Certificates.CertificateRequest]::new(
            "CN=localhost",
            $rsa,
            [Security.Cryptography.HashAlgorithmName]::SHA256,
            [Security.Cryptography.RSASignaturePadding]::Pkcs1)
        $san = [Security.Cryptography.X509Certificates.SubjectAlternativeNameBuilder]::new()
        $san.AddDnsName("localhost")
        $san.AddIpAddress([Net.IPAddress]::Loopback)
        $request.CertificateExtensions.Add($san.Build())
        $certificate = $request.CreateSelfSigned(
            [DateTimeOffset]::UtcNow.AddMinutes(-5),
            [DateTimeOffset]::UtcNow.AddDays(1))
        $certificatePem = ConvertTo-PemText `
            "CERTIFICATE" `
            ($certificate.Export([Security.Cryptography.X509Certificates.X509ContentType]::Cert))
        $privateKeyPem = ConvertTo-PemText `
            "PRIVATE KEY" `
            ($rsa.Key.Export([Security.Cryptography.CngKeyBlobFormat]::Pkcs8PrivateBlob))
        [IO.File]::WriteAllText($CertificatePath, $certificatePem, [Text.UTF8Encoding]::new($false))
        [IO.File]::WriteAllText($KeyPath, $privateKeyPem, [Text.UTF8Encoding]::new($false))
    } finally {
        if ($null -ne $certificate) { $certificate.Dispose() }
        $rsa.Dispose()
    }
}

function Invoke-ProfileConfigExport(
    [string] $AppData,
    [int] $ProfileId,
    [string] $OutputPath,
    [string[]] $ExtraArguments = @()
) {
    $stdoutPath = "$OutputPath.stdout.log"
    $stderrPath = "$OutputPath.stderr.log"
    if ([IO.File]::Exists($OutputPath)) { [IO.File]::Delete($OutputPath) }
    $arguments = @(
        "-appdata", $AppData,
        "-flag_export_profile_config", "$ProfileId", $OutputPath
    ) + $ExtraArguments
    $process = Start-Process `
        -FilePath $executableFull `
        -ArgumentList $arguments `
        -WindowStyle Hidden `
        -RedirectStandardOutput $stdoutPath `
        -RedirectStandardError $stderrPath `
        -Wait `
        -PassThru
    [pscustomobject]@{
        exit_code = $process.ExitCode
        output_created = [IO.File]::Exists($OutputPath)
        stderr = if ([IO.File]::Exists($stderrPath)) {
            [IO.File]::ReadAllText($stderrPath)
        } else {
            ""
        }
    }
}

function Invoke-CoreConfigCheck([string] $ConfigPath) {
    $previousErrorActionPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = "Continue"
        $output = (& $coreFull check -c $ConfigPath 2>&1) -join "`n"
        $exitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousErrorActionPreference
    }
    [pscustomobject]@{
        exit_code = $exitCode
        output = $output
    }
}

function New-HttpProfile(
    [int] $Id,
    [string] $Name,
    [int] $ServerPort,
    [int] $GroupId = 0
) {
    [ordered]@{
        type = "http"
        id = $Id
        gid = $GroupId
        yc = 0
        report = ""
        bean = [ordered]@{
            _v = 0
            name = $Name
            addr = "127.0.0.1"
            port = $ServerPort
            c_cfg = ""
            c_out = ""
            server_resolver_doh = ""
            server_resolver_fallback = $false
            inherit_subscription_client = $false
            inherit_subscription_resolver = $false
            v = -80
            username = ""
            password = ""
            stream = [ordered]@{}
        }
        traffic = [ordered]@{}
    }
}

function New-AnyTLSProfile(
    [int] $Id,
    [string] $Name,
    [int] $ServerPort,
    [int] $GroupId
) {
    [ordered]@{
        type = "anytls"
        id = $Id
        gid = $GroupId
        yc = 0
        report = ""
        bean = [ordered]@{
            _v = 0
            name = $Name
            addr = "127.0.0.1"
            port = $ServerPort
            c_cfg = ""
            c_out = ""
            server_resolver_doh = ""
            server_resolver_fallback = $false
            inherit_subscription_client = $false
            inherit_subscription_resolver = $false
            pass = "anytls-loopback-fixture-password"
            idle_chk = ""
            idle_timeout = ""
            min_idle = 0
            anytls_client_mode = "mihomo"
            anytls_client_value = ""
            insecure = $true
            disable_sni = $false
            sni = "localhost"
            alpn = ""
            cert = ""
            utls = ""
            pbk = ""
            sid = ""
        }
        traffic = [ordered]@{}
    }
}

function New-TrojanProfile(
    [int] $Id,
    [string] $Name,
    [int] $ServerPort,
    [int] $GroupId
) {
    [ordered]@{
        type = "trojan"
        id = $Id
        gid = $GroupId
        yc = 0
        report = ""
        bean = [ordered]@{
            _v = 0
            name = $Name
            addr = "127.0.0.1"
            port = $ServerPort
            c_cfg = ""
            c_out = ""
            server_resolver_doh = ""
            server_resolver_fallback = $false
            inherit_subscription_client = $false
            inherit_subscription_resolver = $false
            pass = "trojan-loopback-fixture-password"
            flow = ""
            stream = [ordered]@{
                net = "tcp"
                sec = "tls"
                sni = "localhost"
                insecure = $true
            }
        }
        traffic = [ordered]@{}
    }
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

$appData = Join-Path $tempRoot "appdata"
$runtimeConfig = Join-Path $tempRoot "generated-auxiliary-runtime.json"
$anyTlsServerConfig = Join-Path $tempRoot "anytls-server.json"
$trojanServerConfig = Join-Path $tempRoot "trojan-server.json"
$certificatePath = Join-Path $tempRoot "loopback-certificate.pem"
$privateKeyPath = Join-Path $tempRoot "loopback-private-key.pem"
$utf8NoBom = [Text.UTF8Encoding]::new($false)
$lineAStdout = Join-Path $tempRoot "line-a.stdout.log"
$lineAStderr = Join-Path $tempRoot "line-a.stderr.log"
$lineBStdout = Join-Path $tempRoot "line-b.stdout.log"
$lineBStderr = Join-Path $tempRoot "line-b.stderr.log"
$frontProxyStdout = Join-Path $tempRoot "front-proxy.stdout.log"
$frontProxyStderr = Join-Path $tempRoot "front-proxy.stderr.log"
$anyTlsServerStdout = Join-Path $tempRoot "anytls-server.stdout.log"
$anyTlsServerStderr = Join-Path $tempRoot "anytls-server.stderr.log"
$coreStdout = Join-Path $tempRoot "core.stdout.log"
$coreStderr = Join-Path $tempRoot "core.stderr.log"
$lineAProcess = $null
$lineBProcess = $null
$frontProxyProcess = $null
$anyTlsServerProcess = $null
$coreProcess = $null
$result = $null
$proxyBefore = Get-SystemProxySnapshot

try {
    New-LoopbackCertificate $certificatePath $privateKeyPath

    $initializationOutput = Join-Path $tempRoot "initialization-unused.json"
    $initialization = Invoke-ProfileConfigExport `
        -AppData $appData `
        -ProfileId 999999 `
        -OutputPath $initializationOutput
    if ($initialization.exit_code -eq 0 -or $initialization.output_created) {
        throw "Missing-profile initialization unexpectedly exported a configuration."
    }

    $profileDirectory = Join-Path $appData "config\profiles"
    [IO.Directory]::CreateDirectory($profileDirectory) | Out-Null
    $profiles = @(
        (New-HttpProfile -Id 1 -Name "generated-main-fixture" -ServerPort 18131),
        (New-AnyTLSProfile -Id 2 -Name "generated-line-a-anytls-fixture" -ServerPort 18130 -GroupId 1),
        (New-HttpProfile -Id 3 -Name "generated-line-b-fixture" -ServerPort 18131),
        (New-TrojanProfile -Id 4 -Name "generated-line-a-trojan-front-fixture" -ServerPort 18132 -GroupId 1)
    )
    foreach ($profile in $profiles) {
        $profilePath = Join-Path $profileDirectory "$($profile.id).json"
        [IO.File]::WriteAllText(
            $profilePath,
            ($profile | ConvertTo-Json -Depth 30),
            $utf8NoBom)
    }

    $defaultGroupPath = Join-Path $appData "config\groups\0.json"
    $frontedGroupPath = Join-Path $appData "config\groups\1.json"
    $frontedGroup = [IO.File]::ReadAllText($defaultGroupPath) | ConvertFrom-Json
    $frontedGroup | Add-Member -NotePropertyName "id" -NotePropertyValue 1 -Force
    $frontedGroup | Add-Member -NotePropertyName "name" -NotePropertyValue "generated-front-proxy-fixture" -Force
    $frontedGroup | Add-Member -NotePropertyName "front_proxy_id" -NotePropertyValue 4 -Force
    $frontedGroup | Add-Member -NotePropertyName "order" -NotePropertyValue @(2, 4) -Force
    [IO.File]::WriteAllText(
        $frontedGroupPath,
        ($frontedGroup | ConvertTo-Json -Depth 30),
        $utf8NoBom)

    $serverTls = [ordered]@{
        enabled = $true
        certificate_path = $certificatePath
        key_path = $privateKeyPath
    }
    $anyTlsServer = [ordered]@{
        log = [ordered]@{ level = "error"; timestamp = $false }
        inbounds = @(
            [ordered]@{
                type = "anytls"
                tag = "anytls-loopback-in"
                listen = "127.0.0.1"
                listen_port = 18130
                users = @(
                    [ordered]@{
                        name = "anytls-loopback-fixture"
                        password = "anytls-loopback-fixture-password"
                    }
                )
                tls = $serverTls
            }
        )
        outbounds = @([ordered]@{ type = "direct"; tag = "direct" })
        route = [ordered]@{ final = "direct" }
    }
    $trojanServer = [ordered]@{
        log = [ordered]@{ level = "error"; timestamp = $false }
        inbounds = @(
            [ordered]@{
                type = "trojan"
                tag = "trojan-loopback-in"
                listen = "127.0.0.1"
                listen_port = 18132
                users = @(
                    [ordered]@{
                        name = "trojan-loopback-fixture"
                        password = "trojan-loopback-fixture-password"
                    }
                )
                tls = $serverTls
            }
        )
        outbounds = @([ordered]@{ type = "direct"; tag = "direct" })
        route = [ordered]@{ final = "direct" }
    }
    [IO.File]::WriteAllText(
        $anyTlsServerConfig,
        ($anyTlsServer | ConvertTo-Json -Depth 30),
        $utf8NoBom)
    [IO.File]::WriteAllText(
        $trojanServerConfig,
        ($trojanServer | ConvertTo-Json -Depth 30),
        $utf8NoBom)

    $mainConfigPath = Join-Path $appData "config\groups\nekobox.json"
    $mainConfig = [IO.File]::ReadAllText($mainConfigPath) | ConvertFrom-Json
    $mainConfig | Add-Member -NotePropertyName "inbound_socks_port" -NotePropertyValue 18119 -Force
    $mainConfig | Add-Member `
        -NotePropertyName "aux_profile_ports" `
        -NotePropertyValue @("2:18120", "3:18121") `
        -Force
    [IO.File]::WriteAllText(
        $mainConfigPath,
        ($mainConfig | ConvertTo-Json -Depth 30),
        $utf8NoBom)

    $routePath = Join-Path $appData "config\routes_box\Default"
    $route = [IO.File]::ReadAllText($routePath) | ConvertFrom-Json
    $route.custom = '{"rules":[{"domain_suffix":["blocked.test"],"outbound":"block"},{"inbound":["aux-mixed-2"],"ip_cidr":["127.0.0.1/32"],"outbound":"bypass"}]}'
    [IO.File]::WriteAllText(
        $routePath,
        ($route | ConvertTo-Json -Depth 30),
        $utf8NoBom)

    $export = Invoke-ProfileConfigExport `
        -AppData $appData `
        -ProfileId 1 `
        -OutputPath $runtimeConfig `
        -ExtraArguments @("-flag_export_profile_config_include_auxiliary_audit")
    if ($export.exit_code -ne 0 -or !$export.output_created) {
        throw "Generated auxiliary config export failed: $($export.stderr.Trim())"
    }

    $generatedConfig = [IO.File]::ReadAllText($runtimeConfig) | ConvertFrom-Json
    $mainMixed = @($generatedConfig.inbounds | Where-Object { $_.tag -eq "mixed-in" })
    $lineAMixed = @($generatedConfig.inbounds | Where-Object { $_.tag -eq "aux-mixed-2" })
    $lineBMixed = @($generatedConfig.inbounds | Where-Object { $_.tag -eq "aux-mixed-3" })
    $tunInbounds = @($generatedConfig.inbounds | Where-Object { $_.type -eq "tun" })
    $systemProxyInbounds = @(
        $generatedConfig.inbounds | Where-Object {
            $_.PSObject.Properties.Name -contains "set_system_proxy" -and
            $_.set_system_proxy -eq $true
        }
    )
    if ($mainMixed.Count -ne 1 -or $mainMixed[0].type -ne "mixed" -or
        $mainMixed[0].listen -ne "127.0.0.1" -or [int]$mainMixed[0].listen_port -ne 18119 -or
        $lineAMixed.Count -ne 1 -or $lineAMixed[0].type -ne "mixed" -or
        $lineAMixed[0].listen -ne "127.0.0.1" -or [int]$lineAMixed[0].listen_port -ne 18120 -or
        $lineBMixed.Count -ne 1 -or $lineBMixed[0].type -ne "mixed" -or
        $lineBMixed[0].listen -ne "127.0.0.1" -or [int]$lineBMixed[0].listen_port -ne 18121 -or
        $tunInbounds.Count -ne 0 -or $systemProxyInbounds.Count -ne 0 -or
        @($generatedConfig.route.PSObject.Properties.Name) -contains "auto_detect_interface") {
        throw "Generated auxiliary config did not preserve the isolated listener/OS-side-effect contract."
    }

    $lineATerminalRoutes = @(
        $generatedConfig.route.rules | Where-Object {
            @($_.PSObject.Properties.Name).Count -eq 2 -and
            @($_.PSObject.Properties.Name) -contains "inbound" -and
            @($_.PSObject.Properties.Name) -contains "outbound" -and
            @($_.inbound).Count -eq 1 -and
            @($_.inbound)[0] -eq "aux-mixed-2"
        }
    )
    $lineATerminalOutbounds = @(
        if ($lineATerminalRoutes.Count -eq 1) {
            $generatedConfig.outbounds |
                Where-Object { $_.tag -eq $lineATerminalRoutes[0].outbound }
        }
    )
    $lineAFrontOutbounds = @(
        if (
            $lineATerminalOutbounds.Count -eq 1 -and
            @($lineATerminalOutbounds[0].PSObject.Properties.Name) -contains "detour"
        ) {
            $generatedConfig.outbounds |
                Where-Object { $_.tag -eq $lineATerminalOutbounds[0].detour }
        }
    )
    if ($lineATerminalRoutes.Count -ne 1 -or
        $lineATerminalOutbounds.Count -ne 1 -or
        $lineAFrontOutbounds.Count -ne 1 -or
        $lineATerminalOutbounds[0].type -ne "anytls" -or
        $lineATerminalOutbounds[0].server -ne "127.0.0.1" -or
        [int]$lineATerminalOutbounds[0].server_port -ne 18130 -or
        $lineATerminalOutbounds[0].client -ne "mihomo/1.19.28" -or
        $lineATerminalOutbounds[0].tls.enabled -ne $true -or
        $lineATerminalOutbounds[0].tls.insecure -ne $true -or
        $lineAFrontOutbounds[0].type -ne "trojan" -or
        $lineAFrontOutbounds[0].server -ne "127.0.0.1" -or
        [int]$lineAFrontOutbounds[0].server_port -ne 18132 -or
        $lineAFrontOutbounds[0].tls.enabled -ne $true -or
        $lineAFrontOutbounds[0].tls.insecure -ne $true) {
        throw "Generated auxiliary line A did not contain one complete two-hop detour chain."
    }

    $clientCheck = Invoke-CoreConfigCheck $runtimeConfig
    $anyTlsServerCheck = Invoke-CoreConfigCheck $anyTlsServerConfig
    $trojanServerCheck = Invoke-CoreConfigCheck $trojanServerConfig
    if ($clientCheck.exit_code -ne 0) {
        throw "Generated auxiliary two-line config failed core schema validation: $($clientCheck.output)"
    }
    if ($anyTlsServerCheck.exit_code -ne 0) {
        throw "Loopback AnyTLS server config failed core schema validation: $($anyTlsServerCheck.output)"
    }
    if ($trojanServerCheck.exit_code -ne 0) {
        throw "Loopback Trojan server config failed core schema validation: $($trojanServerCheck.output)"
    }
    $checkExitCode = $clientCheck.exit_code

    $python = (Get-Command python -ErrorAction Stop).Source
    $lineAProcess = Start-Process `
        -FilePath $python `
        -ArgumentList @("-I", $proxyScript, "--port", "18133", "--status", "210", "--line", "line-a-origin") `
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
    Wait-OwnedListener $lineAProcess 18133 "Line A HTTP origin fixture"
    Wait-OwnedListener $lineBProcess 18131 "Line B HTTP proxy fixture"

    $anyTlsServerProcess = Start-Process `
        -FilePath $coreFull `
        -ArgumentList "run -c `"$anyTlsServerConfig`"" `
        -WorkingDirectory $tempRoot `
        -WindowStyle Hidden `
        -RedirectStandardOutput $anyTlsServerStdout `
        -RedirectStandardError $anyTlsServerStderr `
        -PassThru
    $frontProxyProcess = Start-Process `
        -FilePath $coreFull `
        -ArgumentList "run -c `"$trojanServerConfig`"" `
        -WorkingDirectory $tempRoot `
        -WindowStyle Hidden `
        -RedirectStandardOutput $frontProxyStdout `
        -RedirectStandardError $frontProxyStderr `
        -PassThru
    Wait-OwnedListener $anyTlsServerProcess 18130 "Line A AnyTLS server fixture"
    Wait-OwnedListener $frontProxyProcess 18132 "Line A Trojan front proxy fixture"

    $coreProcess = Start-Process `
        -FilePath $coreFull `
        -ArgumentList "run -c `"$runtimeConfig`"" `
        -WorkingDirectory $tempRoot `
        -WindowStyle Hidden `
        -RedirectStandardOutput $coreStdout `
        -RedirectStandardError $coreStderr `
        -PassThru
    Wait-OwnedListener $coreProcess 18119 "Generated primary Mixed listener"
    Wait-OwnedListener $coreProcess 18120 "Auxiliary line A Mixed listener"
    Wait-OwnedListener $coreProcess 18121 "Auxiliary line B Mixed listener"

    $lineA = Invoke-ProxyRequest 18120 "http://127.0.0.1:18133/probe"
    $lineB = Invoke-ProxyRequest 18121 "http://line-b.test/probe"
    $crossLine = Invoke-ProxyRequest 18120 "http://127.0.0.1:18133/must-stay-a"
    $lineAOriginWasUsed = (Get-HitCount $lineAStdout) -ge 2

    $lineAHitsBeforeReject = Get-HitCount $lineAStdout
    $lineBHitsBeforeReject = Get-HitCount $lineBStdout
    $blocked = Invoke-ProxyRequest 18120 "http://blocked.test/must-reject"
    Start-Sleep -Milliseconds 250
    $rejectDidNotReachUpstream =
        (Get-HitCount $lineAStdout) -eq $lineAHitsBeforeReject -and
        (Get-HitCount $lineBStdout) -eq $lineBHitsBeforeReject

    Stop-Process -Id $frontProxyProcess.Id -Force
    $frontProxyProcess.WaitForExit(5000) | Out-Null
    $failureDeadline = [DateTime]::UtcNow.AddSeconds(5)
    while ([DateTime]::UtcNow -lt $failureDeadline -and
           @(Get-ListeningConnections @(18132)).Count -gt 0) {
        Start-Sleep -Milliseconds 100
    }

    $lineAFailed = Invoke-ProxyRequest 18120 "http://127.0.0.1:18133/front-proxy-down"
    $lineBAfterFailure = Invoke-ProxyRequest 18121 "http://line-b.test/still-running"
    $lineATerminalStillAvailable =
        !$anyTlsServerProcess.HasExited -and
        @(Get-ListeningConnections @(18130) |
            Where-Object { $_.OwningProcess -eq $anyTlsServerProcess.Id }).Count -eq 1 -and
        !$lineAProcess.HasExited -and
        @(Get-ListeningConnections @(18133) |
            Where-Object { $_.OwningProcess -eq $lineAProcess.Id }).Count -eq 1
    $coreStillRunning = !$coreProcess.HasExited -and
                        @(Get-ListeningConnections @(18119, 18120, 18121) |
                            Where-Object { $_.OwningProcess -eq $coreProcess.Id }).Count -eq 3

    $result = [pscustomobject]@{
        config_source = "profile-manager-config-builder"
        generated_config = [pscustomobject]@{
            schema_check_exit_code = $checkExitCode
            anytls_server_schema_check_exit_code = $anyTlsServerCheck.exit_code
            trojan_server_schema_check_exit_code = $trojanServerCheck.exit_code
            main_mixed_port = 18119
            line_a_profile_id = 2
            line_a_mixed_port = 18120
            line_a_protocol = "anytls"
            line_a_client = "mihomo/1.19.28"
            line_a_front_proxy_profile_id = 4
            line_a_front_proxy_port = 18132
            line_a_front_proxy_protocol = "trojan"
            line_b_profile_id = 3
            line_b_mixed_port = 18121
        }
        passed = [bool](
            $lineA.exit_code -eq 0 -and $lineA.http_code -eq 210 -and
            $lineAOriginWasUsed -and
            $lineB.exit_code -eq 0 -and $lineB.http_code -eq 211 -and
            $crossLine.exit_code -eq 0 -and $crossLine.http_code -eq 210 -and
            $blocked.http_code -ne 210 -and $blocked.http_code -ne 211 -and
            $rejectDidNotReachUpstream -and
            $lineAFailed.http_code -ne 210 -and $lineAFailed.http_code -ne 211 -and
            $lineATerminalStillAvailable -and
            $lineBAfterFailure.exit_code -eq 0 -and $lineBAfterFailure.http_code -eq 211 -and
            $coreStillRunning
        )
        initial_mapping = [pscustomobject]@{
            line_a_status = $lineA.http_code
            line_a_exit_code = $lineA.exit_code
            line_a_origin_used = [bool]$lineAOriginWasUsed
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
            failed_component = "line-a-front-proxy"
            failed_line_status = $lineAFailed.http_code
            failed_line_exit_code = $lineAFailed.exit_code
            anytls_server_and_origin_still_available = [bool]$lineATerminalStillAvailable
            surviving_line_status = $lineBAfterFailure.http_code
            surviving_line_exit_code = $lineBAfterFailure.exit_code
            core_and_all_generated_inbounds_still_running = [bool]$coreStillRunning
        }
    }
} finally {
    foreach ($process in @(
        $coreProcess,
        $frontProxyProcess,
        $anyTlsServerProcess,
        $lineAProcess,
        $lineBProcess
    )) {
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
    $result.config_source -eq "profile-manager-config-builder" -and
    $result.side_effect_checks.system_proxy_state_unchanged -and
    $result.side_effect_checks.all_fixture_ports_released
)

if ($Json) {
    $result | ConvertTo-Json -Depth 6
} else {
    $result | Format-List
    $result.generated_config | Format-List
    $result.initial_mapping | Format-List
    $result.cross_line_guard | Format-List
    $result.reject_guard | Format-List
    $result.line_failure_isolation | Format-List
    $result.side_effect_checks | Format-List
}

if (!$result.passed) { exit 1 }
exit 0
