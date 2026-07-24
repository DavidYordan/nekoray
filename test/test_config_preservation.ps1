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
$ExecutablePath = Assert-PathOutsideProtectedProduction $ExecutablePath "Config-preservation GUI executable"
Assert-DirectoryTreeHasNoReparsePoints (Split-Path -Parent $ExecutablePath) "Config-preservation GUI directory tree"

$mingwBin = Join-Path $repoRoot "qtsdk\tools\Tools\mingw1310_64\bin"
$qtBin = Join-Path $repoRoot "qtsdk\qt\6.5.3\mingw_64\bin"
$env:PATH = "$mingwBin;$qtBin;$env:PATH"
$env:QT_QPA_PLATFORM_PLUGIN_PATH = Join-Path $repoRoot "qtsdk\qt\6.5.3\mingw_64\plugins\platforms"

$tempRoot = Assert-PathOutsideProtectedProduction `
    ([IO.Path]::GetFullPath([IO.Path]::GetTempPath())) `
    "Config-preservation temporary root"

function New-TestLab {
    $path = Join-Path $tempRoot ("nekoray-config-preservation-" + [Guid]::NewGuid().ToString("N"))
    [IO.Directory]::CreateDirectory($path) | Out-Null
    return [IO.Path]::GetFullPath($path)
}

function Remove-TestLab([string]$Path) {
    $resolved = [IO.Path]::GetFullPath($Path)
    $parent = [IO.Path]::GetDirectoryName($resolved.TrimEnd('\'))
    $leaf = [IO.Path]::GetFileName($resolved.TrimEnd('\'))
    if ($parent -ne $tempRoot -or -not $leaf.StartsWith("nekoray-config-preservation-", [StringComparison]::Ordinal)) {
        throw "Refusing unsafe test cleanup: $resolved"
    }
    if ([IO.Directory]::Exists($resolved)) {
        [IO.Directory]::Delete($resolved, $true)
    }
}

function Invoke-Export([string]$Lab) {
    $outputPath = Join-Path $Lab "should-not-exist.json"
    $stdoutPath = Join-Path $Lab "stdout.log"
    $stderrPath = Join-Path $Lab "stderr.log"
    $process = Start-Process `
        -FilePath $ExecutablePath `
        -ArgumentList @("-appdata", $Lab, "-flag_export_profile_config", "999999", $outputPath) `
        -WindowStyle Hidden `
        -RedirectStandardOutput $stdoutPath `
        -RedirectStandardError $stderrPath `
        -Wait `
        -PassThru
    return [ordered]@{
        exit_code = $process.ExitCode
        output_created = [IO.File]::Exists($outputPath)
        stdout = if ([IO.File]::Exists($stdoutPath)) { [IO.File]::ReadAllText($stdoutPath) } else { "" }
        stderr = if ([IO.File]::Exists($stderrPath)) { [IO.File]::ReadAllText($stderrPath) } else { "" }
    }
}

function Invoke-Maintenance([string]$Lab, [string[]]$Arguments) {
    $stdoutPath = Join-Path $Lab ("maintenance-stdout-" + [Guid]::NewGuid().ToString("N") + ".log")
    $stderrPath = Join-Path $Lab ("maintenance-stderr-" + [Guid]::NewGuid().ToString("N") + ".log")
    $process = Start-Process `
        -FilePath $ExecutablePath `
        -ArgumentList (@("-appdata", $Lab) + $Arguments) `
        -WindowStyle Hidden `
        -RedirectStandardOutput $stdoutPath `
        -RedirectStandardError $stderrPath `
        -Wait `
        -PassThru
    return [ordered]@{
        exit_code = $process.ExitCode
        stdout = if ([IO.File]::Exists($stdoutPath)) { [IO.File]::ReadAllText($stdoutPath) } else { "" }
        stderr = if ([IO.File]::Exists($stderrPath)) { [IO.File]::ReadAllText($stderrPath) } else { "" }
    }
}

$utf8NoBom = [Text.UTF8Encoding]::new($false)
$cases = @()

function Test-QuarantineEvidence(
    [string]$Lab,
    [string]$RelativeSource,
    [string]$ExpectedHash,
    [string]$ExpectedReasonPattern
) {
    $relativeSnapshot = "$RelativeSource.$($ExpectedHash.ToLowerInvariant()).snapshot"
    $snapshotPath = Join-Path (Join-Path $Lab "config\recovery\quarantine") $relativeSnapshot
    $metadataPath = "$snapshotPath.meta.json"
    if (-not [IO.File]::Exists($snapshotPath) -or -not [IO.File]::Exists($metadataPath)) {
        return $false
    }
    if ((Get-FileHash -LiteralPath $snapshotPath -Algorithm SHA256).Hash -ne $ExpectedHash) {
        return $false
    }
    try {
        $metadata = Get-Content -LiteralPath $metadataPath -Raw -Encoding UTF8 | ConvertFrom-Json
    } catch {
        return $false
    }
    $normalizedSource = $RelativeSource.Replace('\', '/')
    return $metadata.schema -eq "nekoray.recovery.quarantine.v1" -and
           $metadata.source_path -eq $normalizedSource -and
           $metadata.sha256 -eq $ExpectedHash.ToLowerInvariant() -and
           @($metadata.reasons | Where-Object { $_ -match $ExpectedReasonPattern }).Count -ge 1
}

function Add-InvalidAuxiliaryMappingCase([string]$Name, [string]$InvalidAuxJson) {
    $lab = New-TestLab
    try {
        $firstRun = Invoke-Export $lab
        $mainConfig = Join-Path $lab "config\groups\nekobox.json"
        if (-not [IO.File]::Exists($mainConfig)) {
            throw "Initial isolated config creation did not produce the main config. Exit=$($firstRun.exit_code) stderr=$($firstRun.stderr)"
        }

        $configObject = Get-Content -LiteralPath $mainConfig -Raw -Encoding UTF8 | ConvertFrom-Json
        $invalidValue = $InvalidAuxJson | ConvertFrom-Json
        $configObject | Add-Member -NotePropertyName "aux_profile_ports" -NotePropertyValue $invalidValue -Force
        $serialized = $configObject | ConvertTo-Json -Depth 50
        [IO.File]::WriteAllText($mainConfig, $serialized, $utf8NoBom)

        $before = (Get-FileHash -LiteralPath $mainConfig -Algorithm SHA256).Hash
        $secondRun = Invoke-Export $lab
        $after = (Get-FileHash -LiteralPath $mainConfig -Algorithm SHA256).Hash
        $quarantineVerified = Test-QuarantineEvidence `
            -Lab $lab `
            -RelativeSource "groups\nekobox.json" `
            -ExpectedHash $before `
            -ExpectedReasonPattern "aux_profile_ports"
        $script:cases += [ordered]@{
            name = $Name
            passed = ($firstRun.exit_code -ne 0 -and $secondRun.exit_code -ne 0 -and -not $secondRun.output_created -and $before -eq $after -and $quarantineVerified)
            exit_code = $secondRun.exit_code
            hash_unchanged = ($before -eq $after)
            quarantine_verified = $quarantineVerified
        }
    } finally {
        Remove-TestLab $lab
    }
}

$invalidMainLab = New-TestLab
try {
    $groupsDir = Join-Path $invalidMainLab "config\groups"
    [IO.Directory]::CreateDirectory($groupsDir) | Out-Null
    $mainConfig = Join-Path $groupsDir "nekobox.json"
    [IO.File]::WriteAllText($mainConfig, '{"broken":', $utf8NoBom)
    $before = (Get-FileHash -LiteralPath $mainConfig -Algorithm SHA256).Hash
    $run = Invoke-Export $invalidMainLab
    $after = (Get-FileHash -LiteralPath $mainConfig -Algorithm SHA256).Hash
    $quarantineVerified = Test-QuarantineEvidence `
        -Lab $invalidMainLab `
        -RelativeSource "groups\nekobox.json" `
        -ExpectedHash $before `
        -ExpectedReasonPattern "unterminated|JSON"
    $cases += [ordered]@{
        name = "invalid_existing_main_config"
        passed = ($run.exit_code -ne 0 -and -not $run.output_created -and $before -eq $after -and $quarantineVerified)
        exit_code = $run.exit_code
        hash_unchanged = ($before -eq $after)
        quarantine_verified = $quarantineVerified
    }
} finally {
    Remove-TestLab $invalidMainLab
}

Add-InvalidAuxiliaryMappingCase `
    -Name "duplicate_auxiliary_profile_mapping" `
    -InvalidAuxJson '["1:12100","1:12101"]'
Add-InvalidAuxiliaryMappingCase `
    -Name "non_string_auxiliary_mapping_entry" `
    -InvalidAuxJson '["1:12100",17]'
Add-InvalidAuxiliaryMappingCase `
    -Name "wrong_type_auxiliary_mapping_field" `
    -InvalidAuxJson '{"bad":1}'

$unsafeRoutingLab = New-TestLab
try {
    $firstRun = Invoke-Export $unsafeRoutingLab
    $mainConfig = Join-Path $unsafeRoutingLab "config\groups\nekobox.json"
    if (-not [IO.File]::Exists($mainConfig)) {
        throw "Initial isolated config creation did not produce the main config. Exit=$($firstRun.exit_code) stderr=$($firstRun.stderr)"
    }
    $configObject = Get-Content -LiteralPath $mainConfig -Raw -Encoding UTF8 | ConvertFrom-Json
    $configObject | Add-Member -NotePropertyName "active_routing" -NotePropertyValue "..\groups\escape.json" -Force
    [IO.File]::WriteAllText($mainConfig, ($configObject | ConvertTo-Json -Depth 50), $utf8NoBom)

    $before = (Get-FileHash -LiteralPath $mainConfig -Algorithm SHA256).Hash
    $secondRun = Invoke-Export $unsafeRoutingLab
    $after = (Get-FileHash -LiteralPath $mainConfig -Algorithm SHA256).Hash
    $quarantineVerified = Test-QuarantineEvidence `
        -Lab $unsafeRoutingLab `
        -RelativeSource "groups\nekobox.json" `
        -ExpectedHash $before `
        -ExpectedReasonPattern "active_routing"
    $cases += [ordered]@{
        name = "unsafe_active_routing_path_rejected"
        passed = ($firstRun.exit_code -ne 0 -and
                  $secondRun.exit_code -ne 0 -and
                  -not $secondRun.output_created -and
                  $before -eq $after -and
                  $quarantineVerified)
        exit_code = $secondRun.exit_code
        hash_unchanged = ($before -eq $after)
        quarantine_verified = $quarantineVerified
    }
} finally {
    Remove-TestLab $unsafeRoutingLab
}

$invalidRouteLab = New-TestLab
try {
    $firstRun = Invoke-Export $invalidRouteLab
    $mainConfig = Join-Path $invalidRouteLab "config\groups\nekobox.json"
    $routesDir = Join-Path $invalidRouteLab "config\routes_box"
    [array]$routeFiles = @()
    if ([IO.Directory]::Exists($routesDir)) {
        $routeFiles = @(Get-ChildItem -LiteralPath $routesDir -File)
    }
    if (-not [IO.File]::Exists($mainConfig) -or $routeFiles.Count -ne 1) {
        throw "Initial isolated config creation did not produce the expected files. Exit=$($firstRun.exit_code) stderr=$($firstRun.stderr)"
    }
    Get-Content -LiteralPath $mainConfig -Raw -Encoding UTF8 | ConvertFrom-Json | Out-Null

    $routeConfig = $routeFiles[0].FullName
    [IO.File]::WriteAllText($routeConfig, '["not-an-object"]', $utf8NoBom)
    $before = (Get-FileHash -LiteralPath $routeConfig -Algorithm SHA256).Hash
    $secondRun = Invoke-Export $invalidRouteLab
    $after = (Get-FileHash -LiteralPath $routeConfig -Algorithm SHA256).Hash
    $routeRelative = "routes_box\$([IO.Path]::GetFileName($routeConfig))"
    $quarantineVerified = Test-QuarantineEvidence `
        -Lab $invalidRouteLab `
        -RelativeSource $routeRelative `
        -ExpectedHash $before `
        -ExpectedReasonPattern "root must be an object"
    $cases += [ordered]@{
        name = "invalid_existing_route_config"
        passed = ($firstRun.exit_code -ne 0 -and $secondRun.exit_code -ne 0 -and -not $secondRun.output_created -and $before -eq $after -and $quarantineVerified)
        exit_code = $secondRun.exit_code
        hash_unchanged = ($before -eq $after)
        quarantine_verified = $quarantineVerified
    }
} finally {
    Remove-TestLab $invalidRouteLab
}

function Add-ProfileRecoveryCase(
    [string]$Name,
    [string]$ProfileJson,
    [string]$ExpectedReasonPattern
) {
    $lab = New-TestLab
    try {
        $firstRun = Invoke-Export $lab
        $profilesDir = Join-Path $lab "config\profiles"
        $profilePath = Join-Path $profilesDir "7.json"
        [IO.File]::WriteAllText($profilePath, $ProfileJson, $utf8NoBom)
        $before = (Get-FileHash -LiteralPath $profilePath -Algorithm SHA256).Hash
        $secondRun = Invoke-Export $lab
        $after = (Get-FileHash -LiteralPath $profilePath -Algorithm SHA256).Hash
        $quarantineVerified = Test-QuarantineEvidence `
            -Lab $lab `
            -RelativeSource "profiles\7.json" `
            -ExpectedHash $before `
            -ExpectedReasonPattern $ExpectedReasonPattern
        $script:cases += [ordered]@{
            name = $Name
            passed = ($firstRun.exit_code -ne 0 -and $secondRun.exit_code -ne 0 -and $before -eq $after -and $quarantineVerified)
            exit_code = $secondRun.exit_code
            hash_unchanged = ($before -eq $after)
            quarantine_verified = $quarantineVerified
        }
    } finally {
        Remove-TestLab $lab
    }
}

Add-ProfileRecoveryCase `
    -Name "unknown_profile_type_quarantined" `
    -ProfileJson '{"type":"future-core","id":7,"gid":0}' `
    -ExpectedReasonPattern "Unsupported or unknown profile type"

Add-ProfileRecoveryCase `
    -Name "dangling_profile_group_quarantined" `
    -ProfileJson '{"type":"socks","id":7,"gid":99,"bean":{}}' `
    -ExpectedReasonPattern "missing or unreadable group 99"

$manualRecoveryLab = New-TestLab
try {
    $configDir = Join-Path $manualRecoveryLab "config"
    $groupsDir = Join-Path $configDir "groups"
    $transactionDir = Join-Path $configDir "recovery\transactions\cli-test"
    $beforeDir = Join-Path $transactionDir "before"
    $afterDir = Join-Path $transactionDir "after"
    [IO.Directory]::CreateDirectory($groupsDir) | Out-Null
    [IO.Directory]::CreateDirectory($beforeDir) | Out-Null
    [IO.Directory]::CreateDirectory($afterDir) | Out-Null

    $targetPath = Join-Path $groupsDir "recovery-cli.json"
    $beforeBytes = [Text.Encoding]::UTF8.GetBytes('{"state":"before"}')
    $afterBytes = [Text.Encoding]::UTF8.GetBytes('{"state":"after"}')
    [IO.File]::WriteAllBytes($targetPath, $afterBytes)
    $beforeSnapshotPath = Join-Path $beforeDir "0000.bin"
    $afterSnapshotPath = Join-Path $afterDir "0000.bin"
    [IO.File]::WriteAllBytes($beforeSnapshotPath, $beforeBytes)
    [IO.File]::WriteAllBytes($afterSnapshotPath, $afterBytes)
    $beforeHash = (Get-FileHash -LiteralPath $beforeSnapshotPath -Algorithm SHA256).Hash.ToLowerInvariant()
    $afterHash = (Get-FileHash -LiteralPath $afterSnapshotPath -Algorithm SHA256).Hash.ToLowerInvariant()
    $manifest = [ordered]@{
        schema = "nekoray.config_transaction.v1"
        id = "cli-test"
        operation = "manual recovery CLI integration test"
        state = "prepared"
        entries = @(
            [ordered]@{
                path = "groups/recovery-cli.json"
                before = [ordered]@{
                    exists = $true
                    sha256 = $beforeHash
                    size = $beforeBytes.Length
                    snapshot = "before/0000.bin"
                }
                after = [ordered]@{
                    exists = $true
                    sha256 = $afterHash
                    size = $afterBytes.Length
                    snapshot = "after/0000.bin"
                }
            }
        )
    } | ConvertTo-Json -Depth 8
    $manifestPath = Join-Path $transactionDir "manifest.json"
    [IO.File]::WriteAllText($manifestPath, $manifest, $utf8NoBom)

    $reportRun = Invoke-Maintenance $manualRecoveryLab @("-flag_config_transaction_report")
    $report = if ($reportRun.exit_code -eq 0) { $reportRun.stdout | ConvertFrom-Json } else { $null }
    $recoverRun = Invoke-Maintenance $manualRecoveryLab @(
        "-flag_config_transaction_recover", "cli-test", "before")
    $malformedRun = Invoke-Maintenance $manualRecoveryLab @(
        "-flag_config_transaction_report", "-flag_config_transaction_report")
    $recoveredManifest = Get-Content -LiteralPath $manifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
    $targetRestored = (Get-FileHash -LiteralPath $targetPath -Algorithm SHA256).Hash.ToLowerInvariant() -eq $beforeHash
    $reportValid = $null -ne $report -and
                   @($report.transactions).Count -eq 1 -and
                   $report.transactions[0].recoverable -eq $true -and
                   $report.transactions[0].entries[0].current -eq "after"
    $cases += [ordered]@{
        name = "explicit_transaction_report_and_rollback"
        passed = ($reportRun.exit_code -eq 0 -and
                  $reportValid -and
                  $recoverRun.exit_code -eq 0 -and
                  $malformedRun.exit_code -eq 2 -and
                  $malformedRun.stderr -match "exactly one" -and
                  $targetRestored -and
                  $recoveredManifest.state -eq "rolled_back" -and
                  $recoveredManifest.recovery_direction -eq "before" -and
                  -not [string]::IsNullOrWhiteSpace($recoveredManifest.recovered_utc))
        report_exit_code = $reportRun.exit_code
        recovery_exit_code = $recoverRun.exit_code
        malformed_exit_code = $malformedRun.exit_code
        report_recoverable = $reportValid
        target_restored = $targetRestored
        final_state = $recoveredManifest.state
    }
} finally {
    Remove-TestLab $manualRecoveryLab
}

$pendingTransactionLab = New-TestLab
try {
    $firstRun = Invoke-Export $pendingTransactionLab
    $mainConfig = Join-Path $pendingTransactionLab "config\groups\nekobox.json"
    if (-not [IO.File]::Exists($mainConfig)) {
        throw "Initial isolated config creation did not produce the main config. Exit=$($firstRun.exit_code) stderr=$($firstRun.stderr)"
    }
    $before = (Get-FileHash -LiteralPath $mainConfig -Algorithm SHA256).Hash

    $transactionDir = Join-Path $pendingTransactionLab "config\recovery\transactions\pending-test"
    [IO.Directory]::CreateDirectory($transactionDir) | Out-Null
    $manifest = [ordered]@{
        schema = "nekoray.config_transaction.v1"
        id = "pending-test"
        operation = "interrupted integration test"
        state = "prepared"
        entries = @()
    } | ConvertTo-Json -Depth 5
    [IO.File]::WriteAllText((Join-Path $transactionDir "manifest.json"), $manifest, $utf8NoBom)

    $secondRun = Invoke-Export $pendingTransactionLab
    $after = (Get-FileHash -LiteralPath $mainConfig -Algorithm SHA256).Hash
    $cases += [ordered]@{
        name = "pending_transaction_blocks_startup"
        passed = ($firstRun.exit_code -ne 0 -and
                  $secondRun.exit_code -ne 0 -and
                  -not $secondRun.output_created -and
                  $before -eq $after -and
                  $secondRun.stderr -match "interrupted configuration transaction")
        exit_code = $secondRun.exit_code
        hash_unchanged = ($before -eq $after)
        recovery_block_reported = ($secondRun.stderr -match "interrupted configuration transaction")
    }
} finally {
    Remove-TestLab $pendingTransactionLab
}

$result = [ordered]@{
    passed = (@($cases | Where-Object { -not $_.passed }).Count -eq 0)
    cases = $cases
}
$result | ConvertTo-Json -Depth 5
if (-not $result.passed) { exit 1 }
