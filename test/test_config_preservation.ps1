param(
    [string]$ExecutablePath = ""
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$repoRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($ExecutablePath)) {
    $ExecutablePath = Join-Path $repoRoot "build-package-windows64\nekobox.exe"
}
$ExecutablePath = (Resolve-Path -LiteralPath $ExecutablePath).Path

$mingwBin = Join-Path $repoRoot "qtsdk\tools\Tools\mingw1310_64\bin"
$qtBin = Join-Path $repoRoot "qtsdk\qt\6.5.3\mingw_64\bin"
$env:PATH = "$mingwBin;$qtBin;$env:PATH"
$env:QT_QPA_PLATFORM_PLUGIN_PATH = Join-Path $repoRoot "qtsdk\qt\6.5.3\mingw_64\plugins\platforms"

$tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')

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

$utf8NoBom = [Text.UTF8Encoding]::new($false)
$cases = @()

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
        $script:cases += [ordered]@{
            name = $Name
            passed = ($firstRun.exit_code -ne 0 -and $secondRun.exit_code -ne 0 -and -not $secondRun.output_created -and $before -eq $after)
            exit_code = $secondRun.exit_code
            hash_unchanged = ($before -eq $after)
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
    $cases += [ordered]@{
        name = "invalid_existing_main_config"
        passed = ($run.exit_code -ne 0 -and -not $run.output_created -and $before -eq $after)
        exit_code = $run.exit_code
        hash_unchanged = ($before -eq $after)
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
    $cases += [ordered]@{
        name = "invalid_existing_route_config"
        passed = ($firstRun.exit_code -ne 0 -and $secondRun.exit_code -ne 0 -and -not $secondRun.output_created -and $before -eq $after)
        exit_code = $secondRun.exit_code
        hash_unchanged = ($before -eq $after)
    }
} finally {
    Remove-TestLab $invalidRouteLab
}

$result = [ordered]@{
    passed = (@($cases | Where-Object { -not $_.passed }).Count -eq 0)
    cases = $cases
}
$result | ConvertTo-Json -Depth 5
if (-not $result.passed) { exit 1 }
