param(
    [string]$ExecutablePath = ""
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$repoRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $repoRoot "tools\path_safety.ps1")
if ([string]::IsNullOrWhiteSpace($ExecutablePath)) {
    $ExecutablePath = Join-Path $repoRoot "build-package-windows64\nekobox.exe"
}
$ExecutablePath = (Resolve-Path -LiteralPath $ExecutablePath).Path
$ExecutablePath = Assert-PathOutsideProtectedProduction $ExecutablePath "Config-guard GUI executable"
Assert-DirectoryTreeHasNoReparsePoints (Split-Path -Parent $ExecutablePath) "Config-guard GUI directory tree"

$mingwBin = Join-Path $repoRoot "qtsdk\tools\Tools\mingw1310_64\bin"
$qtBin = Join-Path $repoRoot "qtsdk\qt\6.5.3\mingw_64\bin"
$env:PATH = "$mingwBin;$qtBin;$env:PATH"
$env:QT_QPA_PLATFORM_PLUGIN_PATH = Join-Path $repoRoot "qtsdk\qt\6.5.3\mingw_64\plugins\platforms"

$tempRoot = Assert-PathOutsideProtectedProduction `
    ([IO.Path]::GetFullPath([IO.Path]::GetTempPath())) `
    "Config-guard temporary root"
$utf8NoBom = [Text.UTF8Encoding]::new($false)
$cases = @()

function New-TestLab {
    $path = Join-Path $tempRoot ("nekoray-final-config-guards-" + [Guid]::NewGuid().ToString("N"))
    [IO.Directory]::CreateDirectory($path) | Out-Null
    return [IO.Path]::GetFullPath($path)
}

function Remove-TestLab([string]$Path) {
    $resolved = [IO.Path]::GetFullPath($Path)
    $parent = [IO.Path]::GetDirectoryName($resolved.TrimEnd('\'))
    $leaf = [IO.Path]::GetFileName($resolved.TrimEnd('\'))
    if ($parent -ne $tempRoot -or -not $leaf.StartsWith("nekoray-final-config-guards-", [StringComparison]::Ordinal)) {
        throw "Refusing unsafe test cleanup: $resolved"
    }
    if ([IO.Directory]::Exists($resolved)) {
        [IO.Directory]::Delete($resolved, $true)
    }
}

function Invoke-Export([string]$Lab, [int]$ProfileId, [string[]]$ExtraArguments = @()) {
    $outputPath = Join-Path $Lab "export.json"
    $stdoutPath = Join-Path $Lab "stdout.log"
    $stderrPath = Join-Path $Lab "stderr.log"
    if ([IO.File]::Exists($outputPath)) { [IO.File]::Delete($outputPath) }
    $arguments = @("-appdata", $Lab, "-flag_export_profile_config", "$ProfileId", $outputPath) + $ExtraArguments
    $process = Start-Process `
        -FilePath $ExecutablePath `
        -ArgumentList $arguments `
        -WindowStyle Hidden `
        -RedirectStandardOutput $stdoutPath `
        -RedirectStandardError $stderrPath `
        -Wait `
        -PassThru
    return [ordered]@{
        exit_code = $process.ExitCode
        output_created = [IO.File]::Exists($outputPath)
        stderr = if ([IO.File]::Exists($stderrPath)) { [IO.File]::ReadAllText($stderrPath) } else { "" }
    }
}

function Add-Case(
    [string]$Name,
    [string]$CoreConfig,
    [bool]$ShouldSucceed,
    [string]$ExpectedError = "",
    [string[]]$ExtraArguments = @(),
    [ValidateSet("internal-full", "socks")]
    [string]$FixtureType = "internal-full",
    [string]$CustomConfig = "",
    [string]$CustomOutbound = "",
    [string]$RoutingCustom = "",
    [switch]$AssertNoNullRuleFields,
    [string]$ServerAddress = "192.0.2.1",
    [string]$ResolverDoh = "",
    [int]$ExpectedProviderDohCount = -1,
    [switch]$AssertNativeBootstrap,
    [switch]$UseSubscriptionGroupResolver,
    [int]$GroupResolverPolicyVersion = 1,
    [string]$GroupResolverOrigin = "nameserver"
) {
    $lab = New-TestLab
    try {
        # A missing-profile export creates the normal isolated store structure
        # without launching a profile or touching any OS network mode.
        $null = Invoke-Export -Lab $lab -ProfileId 999999
        $profileDir = Join-Path $lab "config\profiles"
        [IO.Directory]::CreateDirectory($profileDir) | Out-Null
        $bean = if ($FixtureType -eq "socks") {
            [ordered]@{
                _v = 0
                name = "final-config-guard-socks-fixture"
                addr = $ServerAddress
                port = 1080
                c_cfg = $CustomConfig
                c_out = $CustomOutbound
                server_resolver_doh = if ($UseSubscriptionGroupResolver) { "" } else { $ResolverDoh }
                server_resolver_fallback = $false
                inherit_subscription_client = $false
                inherit_subscription_resolver = [bool]$UseSubscriptionGroupResolver
                v = 5
                username = ""
                password = ""
                stream = [ordered]@{}
            }
        } else {
            [ordered]@{
                _v = 0
                name = "final-config-guard-fixture"
                core = "internal-full"
                cs = $CoreConfig
            }
        }
        $profile = [ordered]@{
            type = if ($FixtureType -eq "socks") { "socks" } else { "custom" }
            id = 1
            gid = 0
            yc = 0
            report = ""
            bean = $bean
            traffic = [ordered]@{}
        }
        $profilePath = Join-Path $profileDir "1.json"
        [IO.File]::WriteAllText($profilePath, ($profile | ConvertTo-Json -Depth 30), $utf8NoBom)

        if ($UseSubscriptionGroupResolver) {
            $groupPath = Join-Path $lab "config\groups\0.json"
            $group = [IO.File]::ReadAllText($groupPath) | ConvertFrom-Json
            $group | Add-Member -NotePropertyName "source_type" -NotePropertyValue "clash" -Force
            $group | Add-Member -NotePropertyName "default_server_resolver_source" -NotePropertyValue "subscription" -Force
            $group | Add-Member -NotePropertyName "default_server_resolver_doh" -NotePropertyValue $ResolverDoh -Force
            $group | Add-Member -NotePropertyName "default_server_resolver_origin" -NotePropertyValue $GroupResolverOrigin -Force
            $group | Add-Member -NotePropertyName "default_server_resolver_policy_version" -NotePropertyValue $GroupResolverPolicyVersion -Force
            [IO.File]::WriteAllText($groupPath, ($group | ConvertTo-Json -Depth 30), $utf8NoBom)
        }

        if (![string]::IsNullOrWhiteSpace($RoutingCustom)) {
            $routePath = Join-Path $lab "config\routes_box\Default"
            $route = [IO.File]::ReadAllText($routePath) | ConvertFrom-Json
            $route.custom = $RoutingCustom
            [IO.File]::WriteAllText($routePath, ($route | ConvertTo-Json -Depth 30), $utf8NoBom)
        }

        $run = Invoke-Export -Lab $lab -ProfileId 1 -ExtraArguments $ExtraArguments
        $errorMatched = [string]::IsNullOrEmpty($ExpectedError) -or
            $run.stderr.IndexOf($ExpectedError, [StringComparison]::Ordinal) -ge 0
        $outputAssertionPassed = $true
        if ($AssertNoNullRuleFields -and $run.output_created) {
            $output = [IO.File]::ReadAllText((Join-Path $lab "export.json")) | ConvertFrom-Json
            foreach ($rule in @($output.route.rules)) {
                if (($rule.PSObject.Properties.Name -contains "rules" -and $null -eq $rule.rules) -or
                    ($rule.PSObject.Properties.Name -contains "outbound" -and $null -eq $rule.outbound)) {
                    $outputAssertionPassed = $false
                }
            }
        }
        if ($ExpectedProviderDohCount -ge 0 -and $run.output_created) {
            $output = [IO.File]::ReadAllText((Join-Path $lab "export.json")) | ConvertFrom-Json
            $providerDoh = @($output.dns.servers | Where-Object { $_.tag -like "rf-doh-*" })
            if ($providerDoh.Count -ne $ExpectedProviderDohCount) {
                $outputAssertionPassed = $false
            }
            if ($AssertNativeBootstrap) {
                $nativeBootstrap = @($output.dns.servers | Where-Object { $_.tag -eq "dns-local" })
                if ($nativeBootstrap.Count -ne 1 -or $nativeBootstrap[0].type -ne "local") {
                    $outputAssertionPassed = $false
                }
                foreach ($server in $providerDoh) {
                    if ($server.domain_resolver.server -ne "dns-local" -or
                        $server.tls.server_name -ne $server.server -or
                        ($server.domain_resolver.PSObject.Properties.Name -contains "strategy")) {
                        $outputAssertionPassed = $false
                    }
                }
            }
        }
        $passed = if ($ShouldSucceed) {
            $run.exit_code -eq 0 -and $run.output_created -and $outputAssertionPassed
        } else {
            $run.exit_code -ne 0 -and -not $run.output_created -and $errorMatched
        }
        $script:cases += [ordered]@{
            name = $Name
            passed = $passed
            exit_code = $run.exit_code
            output_created = $run.output_created
            error_matched = $errorMatched
            output_assertion_passed = $outputAssertionPassed
            stderr = $run.stderr.Trim()
        }
    } finally {
        Remove-TestLab $lab
    }
}

Add-Case `
    -Name "safe_internal_full_export" `
    -CoreConfig '{"inbounds":[],"outbounds":[]}' `
    -ShouldSucceed $true

Add-Case `
    -Name "reject_unrequested_tun" `
    -CoreConfig '{"inbounds":[{"type":"tun","tag":"unowned"}],"outbounds":[]}' `
    -ShouldSucceed $false `
    -ExpectedError "Tun inbound is present without an explicit product Tun enable action"

Add-Case `
    -Name "reject_inbound_system_proxy" `
    -CoreConfig '{"inbounds":[{"type":"mixed","tag":"mixed","listen":"127.0.0.1","listen_port":19081,"set_system_proxy":true}],"outbounds":[]}' `
    -ShouldSucceed $false `
    -ExpectedError "set_system_proxy=true"

Add-Case `
    -Name "reject_wireguard_system_endpoint_export" `
    -CoreConfig '{"inbounds":[],"outbounds":[],"endpoints":[{"type":"wireguard","tag":"wg-system","system":true}]}' `
    -ShouldSucceed $false `
    -ExpectedError "unmanaged wireguard system interface"

Add-Case `
    -Name "reject_test_top_level_custom_config_before_launch" `
    -CoreConfig '{}' `
    -ShouldSucceed $false `
    -ExpectedError "temporary core must remain an exact bounded generated configuration" `
    -ExtraArguments @("-flag_export_profile_config_for_test") `
    -FixtureType "socks" `
    -CustomConfig '{"ntp":{"enabled":true,"write_to_system":true}}'

Add-Case `
    -Name "reject_managed_mixed_inbound_replacement_export" `
    -CoreConfig '{}' `
    -ShouldSucceed $false `
    -ExpectedError "Managed Mixed inbound 'mixed-in' changed after custom_config merge" `
    -FixtureType "socks" `
    -CustomConfig '{"inbounds":[{"tag":"mixed-in","type":"mixed","listen":"127.0.0.1","listen_port":12080,"detour":"direct"}]}'

Add-Case `
    -Name "reject_profile_level_outbound_detour_export" `
    -CoreConfig '{}' `
    -ShouldSucceed $false `
    -ExpectedError "Profile-level custom outbound settings may not add or change detour" `
    -FixtureType "socks" `
    -CustomOutbound '{"detour":"direct"}'

Add-Case `
    -Name "safe_standard_custom_route_without_null_fields" `
    -CoreConfig '{}' `
    -ShouldSucceed $true `
    -FixtureType "socks" `
    -RoutingCustom '{"rules":[{"domain_suffix":["example.test"],"outbound":"proxy"}]}' `
    -AssertNoNullRuleFields

Add-Case `
    -Name "native_domain_without_provider_doh" `
    -CoreConfig '{}' `
    -ShouldSucceed $true `
    -FixtureType "socks" `
    -ServerAddress "native-node.example" `
    -ExpectedProviderDohCount 0

Add-Case `
    -Name "provider_domain_doh_uses_native_bootstrap" `
    -CoreConfig '{}' `
    -ShouldSucceed $true `
    -FixtureType "socks" `
    -ServerAddress "provider-node.example" `
    -ResolverDoh "https://resolver.example/dns-query/provider" `
    -ExpectedProviderDohCount 1 `
    -AssertNativeBootstrap `
    -UseSubscriptionGroupResolver

Add-Case `
    -Name "reject_obsolete_subscription_resolver_metadata" `
    -CoreConfig '{}' `
    -ShouldSucceed $false `
    -FixtureType "socks" `
    -ServerAddress "provider-node.example" `
    -ResolverDoh "https://stale.example/dns-query" `
    -UseSubscriptionGroupResolver `
    -GroupResolverPolicyVersion 0 `
    -GroupResolverOrigin "" `
    -ExpectedError "obsolete import policy"

Add-Case `
    -Name "reject_invalid_provider_doh" `
    -CoreConfig '{}' `
    -ShouldSucceed $false `
    -FixtureType "socks" `
    -ServerAddress "provider-node.example" `
    -ResolverDoh "http://resolver.example/dns-query" `
    -ExpectedError "DoH URL must use https scheme"

Add-Case `
    -Name "reject_native_bootstrap_replacement" `
    -CoreConfig '{}' `
    -ShouldSucceed $false `
    -FixtureType "socks" `
    -ServerAddress "provider-node.example" `
    -ResolverDoh "https://resolver.example/dns-query" `
    -CustomConfig '{"dns":{"servers":[{"tag":"dns-local","type":"udp","server":"8.8.8.8"}]}}' `
    -ExpectedError "Native bootstrap resolver 'dns-local'"

Add-Case `
    -Name "reject_tailscale_system_interface_share_export" `
    -CoreConfig '{"inbounds":[],"outbounds":[],"endpoints":[{"type":"tailscale","tag":"ts-system","system_interface":true}]}' `
    -ShouldSucceed $false `
    -ExpectedError "unmanaged tailscale system interface" `
    -ExtraArguments @("-flag_export_profile_config_for_share")

Add-Case `
    -Name "reject_ntp_system_clock_write" `
    -CoreConfig '{"inbounds":[],"outbounds":[],"ntp":{"enabled":true,"write_to_system":true}}' `
    -ShouldSucceed $false `
    -ExpectedError "ntp.write_to_system=true"

$result = [ordered]@{
    passed = (@($cases | Where-Object { -not $_.passed }).Count -eq 0)
    cases = $cases
}
$result | ConvertTo-Json -Depth 5
if (-not $result.passed) { exit 1 }
