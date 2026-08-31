param(
    [string]$ExecutablePath = "",
    [string]$CorePath = ""
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
if (![string]::IsNullOrWhiteSpace($CorePath)) {
    $CorePath = (Resolve-Path -LiteralPath $CorePath).Path
    $CorePath = Assert-PathOutsideProtectedProduction $CorePath "Config-guard core executable"
    Assert-DirectoryTreeHasNoReparsePoints (Split-Path -Parent $CorePath) "Config-guard core directory tree"
}

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
    [int]$ExpectedPrimaryMixedPort = -1,
    [switch]$AssertPrimaryUsesNativeRouting,
    [switch]$AssertGeneratedAuxiliaryContract,
    [switch]$AssertAuxiliaryOmitted,
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

        if ($AssertGeneratedAuxiliaryContract -or $AssertAuxiliaryOmitted) {
            $auxProfile = [ordered]@{
                type = "chain"
                id = 2
                gid = 0
                yc = 0
                report = ""
                bean = [ordered]@{
                    _v = 0
                    name = "final-config-guard-auxiliary-chain-fixture"
                    list = @(3, 4)
                }
                traffic = [ordered]@{}
            }
            $auxProfilePath = Join-Path $profileDir "2.json"
            [IO.File]::WriteAllText($auxProfilePath, ($auxProfile | ConvertTo-Json -Depth 30), $utf8NoBom)
            $auxiliaryHops = @(
                [ordered]@{ id = 3; addr = "192.0.2.2"; port = 1081 },
                [ordered]@{ id = 4; addr = "192.0.2.3"; port = 1082 }
            )
            foreach ($hop in $auxiliaryHops) {
                $hopProfile = [ordered]@{
                    type = "socks"
                    id = $hop.id
                    gid = 0
                    yc = 0
                    report = ""
                    bean = [ordered]@{
                        _v = 0
                        name = "final-config-guard-auxiliary-hop-$($hop.id)"
                        addr = $hop.addr
                        port = $hop.port
                        c_cfg = ""
                        c_out = ""
                        server_resolver_doh = ""
                        server_resolver_fallback = $false
                        inherit_subscription_client = $false
                        inherit_subscription_resolver = $false
                        v = 5
                        username = ""
                        password = ""
                        stream = [ordered]@{}
                    }
                    traffic = [ordered]@{}
                }
                $hopProfilePath = Join-Path $profileDir "$($hop.id).json"
                [IO.File]::WriteAllText($hopProfilePath, ($hopProfile | ConvertTo-Json -Depth 30), $utf8NoBom)
            }

            $mainConfigPath = Join-Path $lab "config\groups\nekobox.json"
            $mainConfig = [IO.File]::ReadAllText($mainConfigPath) | ConvertFrom-Json
            $mainConfig | Add-Member -NotePropertyName "aux_profile_ports" -NotePropertyValue @("2:12100") -Force
            [IO.File]::WriteAllText($mainConfigPath, ($mainConfig | ConvertTo-Json -Depth 30), $utf8NoBom)
        }

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
        if ($ExpectedPrimaryMixedPort -ge 0 -and $run.output_created) {
            $output = [IO.File]::ReadAllText((Join-Path $lab "export.json")) | ConvertFrom-Json
            $primaryMixed = @($output.inbounds | Where-Object { $_.tag -eq "mixed-in" })
            if ($primaryMixed.Count -ne 1 -or
                [int]$primaryMixed[0].listen_port -ne $ExpectedPrimaryMixedPort) {
                $outputAssertionPassed = $false
            }
        }
        if ($AssertPrimaryUsesNativeRouting -and $run.output_created) {
            $output = [IO.File]::ReadAllText((Join-Path $lab "export.json")) | ConvertFrom-Json
            $nativeRouteFound = $false
            $unconditionalPrimaryBindingFound = $false
            foreach ($rule in @($output.route.rules)) {
                $propertyNames = @($rule.PSObject.Properties.Name)
                $inboundTags = @()
                if ($propertyNames -contains "inbound") { $inboundTags += @($rule.inbound) }
                $domainSuffixes = @()
                if ($propertyNames -contains "domain_suffix") { $domainSuffixes += @($rule.domain_suffix) }
                $outbound = ""
                if ($propertyNames -contains "outbound") { $outbound = [string]$rule.outbound }
                if ($inboundTags.Count -eq 1 -and
                    $inboundTags[0] -eq "mixed-in" -and
                    $domainSuffixes -contains "native-routing.example" -and
                    $outbound -eq "bypass") {
                    $nativeRouteFound = $true
                }

                $extraTerminalFields = @(
                    $propertyNames | Where-Object { $_ -notin @("inbound", "outbound", "action") }
                )
                $action = if ($propertyNames -contains "action") { [string]$rule.action } else { "" }
                if ($inboundTags.Count -eq 1 -and
                    $inboundTags[0] -eq "mixed-in" -and
                    $outbound -eq "proxy" -and
                    $extraTerminalFields.Count -eq 0 -and
                    ([string]::IsNullOrWhiteSpace($action) -or $action -eq "route")) {
                    $unconditionalPrimaryBindingFound = $true
                }
            }
            if (-not $nativeRouteFound -or $unconditionalPrimaryBindingFound) {
                $outputAssertionPassed = $false
            }
        }
        $coreCheckExitCode = $null
        if ($AssertGeneratedAuxiliaryContract -and $run.output_created) {
            $outputPath = Join-Path $lab "export.json"
            $output = [IO.File]::ReadAllText($outputPath) | ConvertFrom-Json
            $auxiliaryTag = "aux-mixed-2"
            $auxiliaryMixed = @($output.inbounds | Where-Object { $_.tag -eq $auxiliaryTag })
            $tunInbounds = @($output.inbounds | Where-Object { $_.type -eq "tun" })
            $systemProxyInbounds = @(
                $output.inbounds | Where-Object {
                    $_.PSObject.Properties.Name -contains "set_system_proxy" -and
                    $_.set_system_proxy -eq $true
                }
            )
            if ($auxiliaryMixed.Count -ne 1 -or
                $auxiliaryMixed[0].type -ne "mixed" -or
                $auxiliaryMixed[0].listen -ne "127.0.0.1" -or
                [int]$auxiliaryMixed[0].listen_port -ne 12100 -or
                $tunInbounds.Count -ne 0 -or
                $systemProxyInbounds.Count -ne 0 -or
                @($output.route.PSObject.Properties.Name) -contains "auto_detect_interface") {
                $outputAssertionPassed = $false
            }

            $terminalIndex = -1
            $terminalOutbound = ""
            $routeRules = @($output.route.rules)
            for ($index = 0; $index -lt $routeRules.Count; $index++) {
                $rule = $routeRules[$index]
                $propertyNames = @($rule.PSObject.Properties.Name)
                $inboundTags = @()
                if ($propertyNames -contains "inbound") { $inboundTags += @($rule.inbound) }
                if ($inboundTags.Count -eq 1 -and
                    $inboundTags[0] -eq $auxiliaryTag -and
                    $propertyNames -contains "outbound" -and
                    $propertyNames.Count -eq 2) {
                    $terminalIndex = $index
                    $terminalOutbound = [string]$rule.outbound
                    break
                }
            }
            if ($terminalIndex -lt 0 -or [string]::IsNullOrWhiteSpace($terminalOutbound)) {
                $outputAssertionPassed = $false
            } else {
                $matchingOutbounds = @($output.outbounds | Where-Object { $_.tag -eq $terminalOutbound })
                if ($matchingOutbounds.Count -ne 1 -or $matchingOutbounds[0].type -ne "socks") {
                    $outputAssertionPassed = $false
                } else {
                    $terminalDetour = if ($matchingOutbounds[0].PSObject.Properties.Name -contains "detour") {
                        [string]$matchingOutbounds[0].detour
                    } else {
                        ""
                    }
                    $detourOutbounds = @($output.outbounds | Where-Object { $_.tag -eq $terminalDetour })
                    if ([string]::IsNullOrWhiteSpace($terminalDetour) -or
                        $detourOutbounds.Count -ne 1 -or
                        $detourOutbounds[0].type -ne "socks" -or
                        $matchingOutbounds[0].server -ne "192.0.2.3" -or
                        $detourOutbounds[0].server -ne "192.0.2.2") {
                        $outputAssertionPassed = $false
                    }
                }

                for ($index = 0; $index -lt $terminalIndex; $index++) {
                    if (@($routeRules[$index].PSObject.Properties.Name) -contains "outbound") {
                        $outputAssertionPassed = $false
                    }
                }
            }

            $scopedRejectFound = $false
            $postTerminalRedirectFound = $false
            for ($index = 0; $index -lt $routeRules.Count; $index++) {
                $rule = $routeRules[$index]
                $propertyNames = @($rule.PSObject.Properties.Name)
                if ($index -lt $terminalIndex -and
                    $propertyNames -contains "type" -and
                    $propertyNames -contains "mode" -and
                    $propertyNames -contains "action" -and
                    $rule.type -eq "logical" -and
                    $rule.mode -eq "and" -and
                    $rule.action -eq "reject") {
                    $conditions = @($rule.rules)
                    if ($conditions.Count -eq 2) {
                        $firstConditionNames = @($conditions[0].PSObject.Properties.Name)
                        $secondConditionNames = @($conditions[1].PSObject.Properties.Name)
                        if ($firstConditionNames -contains "inbound" -and
                            $secondConditionNames -contains "domain_suffix" -and
                            @($conditions[0].inbound).Count -eq 1 -and
                            @($conditions[0].inbound)[0] -eq $auxiliaryTag -and
                            @($conditions[1].domain_suffix) -contains "blocked.generated.test") {
                            $scopedRejectFound = $true
                        }
                    }
                }
                $inboundTags = @()
                if ($propertyNames -contains "inbound") { $inboundTags += @($rule.inbound) }
                if ($index -gt $terminalIndex -and
                    $propertyNames -contains "domain_suffix" -and
                    $propertyNames -contains "outbound" -and
                    $inboundTags.Count -eq 1 -and
                    $inboundTags[0] -eq $auxiliaryTag -and
                    @($rule.domain_suffix) -contains "must-not-redirect.generated.test" -and
                    $rule.outbound -eq "bypass") {
                    $postTerminalRedirectFound = $true
                }
            }
            if (-not $scopedRejectFound -or -not $postTerminalRedirectFound) {
                $outputAssertionPassed = $false
            }

            if (![string]::IsNullOrWhiteSpace($CorePath)) {
                $previousErrorActionPreference = $ErrorActionPreference
                try {
                    $ErrorActionPreference = "Continue"
                    $null = (& $CorePath check -c $outputPath 2>&1) -join "`n"
                    $coreCheckExitCode = $LASTEXITCODE
                } finally {
                    $ErrorActionPreference = $previousErrorActionPreference
                }
                if ($coreCheckExitCode -ne 0) {
                    $outputAssertionPassed = $false
                }
            }
        }
        if ($AssertAuxiliaryOmitted -and $run.output_created) {
            $output = [IO.File]::ReadAllText((Join-Path $lab "export.json")) | ConvertFrom-Json
            $auxiliaryRouteJson = @($output.route.rules) | ConvertTo-Json -Depth 30 -Compress
            $auxiliaryOutbounds = @(
                $output.outbounds | Where-Object {
                    $_.PSObject.Properties.Name -contains "server" -and
                    $_.server -in @("192.0.2.2", "192.0.2.3")
                }
            )
            if (@($output.inbounds | Where-Object { $_.tag -eq "aux-mixed-2" }).Count -ne 0 -or
                $auxiliaryOutbounds.Count -ne 0 -or
                $auxiliaryRouteJson.IndexOf("aux-mixed-2", [StringComparison]::Ordinal) -ge 0) {
                $outputAssertionPassed = $false
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
            core_check_exit_code = $coreCheckExitCode
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
    -Name "primary_mixed_preserves_native_routing" `
    -CoreConfig '{}' `
    -ShouldSucceed $true `
    -FixtureType "socks" `
    -RoutingCustom '{"rules":[{"inbound":["mixed-in"],"domain_suffix":["native-routing.example"],"outbound":"bypass"}]}' `
    -ExpectedPrimaryMixedPort 2080 `
    -AssertPrimaryUsesNativeRouting

Add-Case `
    -Name "generated_auxiliary_line_is_strict_and_main_stays_native" `
    -CoreConfig '{}' `
    -ShouldSucceed $true `
    -FixtureType "socks" `
    -RoutingCustom '{"rules":[{"inbound":["mixed-in"],"domain_suffix":["native-routing.example"],"outbound":"bypass"},{"domain_suffix":["blocked.generated.test"],"outbound":"block"},{"inbound":["aux-mixed-2"],"domain_suffix":["must-not-redirect.generated.test"],"outbound":"bypass"}]}' `
    -ExtraArguments @("-flag_export_profile_config_include_auxiliary_audit") `
    -ExpectedPrimaryMixedPort 2080 `
    -AssertPrimaryUsesNativeRouting `
    -AssertGeneratedAuxiliaryContract

Add-Case `
    -Name "normal_export_remains_primary_only" `
    -CoreConfig '{}' `
    -ShouldSucceed $true `
    -FixtureType "socks" `
    -RoutingCustom '{"rules":[{"inbound":["mixed-in"],"domain_suffix":["native-routing.example"],"outbound":"bypass"}]}' `
    -ExpectedPrimaryMixedPort 2080 `
    -AssertPrimaryUsesNativeRouting `
    -AssertAuxiliaryOmitted

Add-Case `
    -Name "reject_auxiliary_audit_in_test_mode" `
    -CoreConfig '{}' `
    -ShouldSucceed $false `
    -ExpectedError "Auxiliary-line audit is only available for the side-effect-free standard export mode." `
    -ExtraArguments @("-flag_export_profile_config_for_test", "-flag_export_profile_config_include_auxiliary_audit") `
    -FixtureType "socks"

Add-Case `
    -Name "native_domain_without_provider_doh" `
    -CoreConfig '{}' `
    -ShouldSucceed $true `
    -FixtureType "socks" `
    -ServerAddress "native-node.example" `
    -ExpectedPrimaryMixedPort 2080 `
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
