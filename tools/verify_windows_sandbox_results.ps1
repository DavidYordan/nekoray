[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $RunRoot,
    [switch] $Json
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "path_safety.ps1")
$safeRunRoot = Assert-PathOutsideProtectedProduction ([IO.Path]::GetFullPath($RunRoot)) "Windows Sandbox result root"
Assert-DirectoryTreeHasNoReparsePoints $safeRunRoot "Windows Sandbox result tree"

$expectedPath = Join-Path $safeRunRoot "input\expected-input-manifest.json"
$actualPath = Join-Path $safeRunRoot "results\actual-input-manifest.json"
$summaryPath = Join-Path $safeRunRoot "results\sandbox-summary.json"
$completionPath = Join-Path $safeRunRoot "results\completed.marker"
foreach ($required in @($expectedPath, $actualPath, $summaryPath, $completionPath)) {
    if (!(Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Windows Sandbox result is incomplete; missing: $required"
    }
}

$expected = Get-Content -LiteralPath $expectedPath -Raw -Encoding UTF8 | ConvertFrom-Json
$actual = Get-Content -LiteralPath $actualPath -Raw -Encoding UTF8 | ConvertFrom-Json
$summary = Get-Content -LiteralPath $summaryPath -Raw -Encoding UTF8 | ConvertFrom-Json

$failures = [Collections.Generic.List[string]]::new()
$expectedByPath = @{}
foreach ($entry in @($expected.entries)) {
    $expectedByPath[$entry.path] = $entry
}
$actualByPath = @{}
foreach ($entry in @($actual.entries)) {
    $actualByPath[$entry.path] = $entry
}
$hostInputRoot = Join-Path $safeRunRoot "input"
$hostByPath = @{}
foreach ($file in Get-ChildItem -LiteralPath $hostInputRoot -File -Recurse -Force |
    Where-Object { $_.Name -ne "expected-input-manifest.json" }) {
    $relativePath = $file.FullName.Substring($hostInputRoot.Length).TrimStart('\', '/').Replace('\', '/')
    $hostByPath[$relativePath] = [ordered]@{
        length = $file.Length
        sha256 = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}
foreach ($path in @($expectedByPath.Keys)) {
    if (!$actualByPath.ContainsKey($path)) {
        $failures.Add("sandbox input missing: $path")
    } else {
        $expectedEntry = $expectedByPath[$path]
        $actualEntry = $actualByPath[$path]
        if ($expectedEntry.length -ne $actualEntry.length -or $expectedEntry.sha256 -ne $actualEntry.sha256) {
            $failures.Add("sandbox input hash mismatch: $path")
        }
    }
    if (!$hostByPath.ContainsKey($path)) {
        $failures.Add("host staging input missing after Sandbox run: $path")
    } else {
        $expectedEntry = $expectedByPath[$path]
        $hostEntry = $hostByPath[$path]
        if ($expectedEntry.length -ne $hostEntry.length -or $expectedEntry.sha256 -ne $hostEntry.sha256) {
            $failures.Add("host staging input changed after Sandbox run: $path")
        }
    }
}
foreach ($path in @($actualByPath.Keys)) {
    if (!$expectedByPath.ContainsKey($path)) {
        $failures.Add("unexpected sandbox input: $path")
    }
}
foreach ($path in @($hostByPath.Keys)) {
    if (!$expectedByPath.ContainsKey($path)) {
        $failures.Add("unexpected host staging input after Sandbox run: $path")
    }
}

if ($summary.networking_contract -ne "disabled_by_wsb") {
    $failures.Add("sandbox did not report the offline networking contract")
}
if ($summary.up_network_adapter_count -ne 0) {
    $failures.Add("sandbox has an Up network adapter despite Networking=Disable")
}
if (!$summary.success) {
    $failures.Add("sandbox runner reported failure: $($summary.error)")
}

$result = [ordered]@{
    schema = "nekoray.windows_sandbox_verification.v1"
    passed = $failures.Count -eq 0
    run_root = $safeRunRoot
    package_present = $summary.package_present
    os = $summary.os
    elevated = $summary.elevated
    up_network_adapter_count = $summary.up_network_adapter_count
    input_file_count = $expectedByPath.Count
    failures = @($failures)
}

if ($Json) {
    $result | ConvertTo-Json -Depth 8
} else {
    $result | Format-List
}
if ($failures.Count -ne 0) {
    exit 1
}
