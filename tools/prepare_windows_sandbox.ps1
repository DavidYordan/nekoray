[CmdletBinding()]
param(
    [string] $PackageDir = "",
    [string] $OutputRoot = "",
    [switch] $Launch
)

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
. (Join-Path $PSScriptRoot "path_safety.ps1")

function Write-Utf8Json {
    param(
        [Parameter(Mandatory = $true)] [string] $Path,
        [Parameter(Mandatory = $true)] $Value,
        [int] $Depth = 8
    )
    $json = $Value | ConvertTo-Json -Depth $Depth
    [IO.File]::WriteAllText($Path, $json, [Text.UTF8Encoding]::new($false))
}

function Copy-AllowlistedPackage {
    param(
        [Parameter(Mandatory = $true)] [string] $Source,
        [Parameter(Mandatory = $true)] [string] $Destination
    )

    $allowedFiles = @(
        "geoip.db",
        "geosite.db",
        "nekobox.exe",
        "nekobox.png",
        "nekobox_core.exe",
        "qtbase_zh_CN.qm",
        "routefluent-sing-box-manifest.json",
        "updater.exe"
    )
    $allowedDirectories = @(
        "generic",
        "iconengines",
        "imageformats",
        "networkinformation",
        "platforms",
        "styles",
        "tls"
    )

    $sourceRoot = (Resolve-Path -LiteralPath $Source).Path.TrimEnd('\', '/')
    Assert-DirectoryTreeHasNoReparsePoints $sourceRoot "Windows Sandbox package source"
    foreach ($requiredFile in @("nekobox.exe", "nekobox_core.exe")) {
        if (!(Test-Path -LiteralPath (Join-Path $sourceRoot $requiredFile) -PathType Leaf)) {
            throw "Package source is missing required file: $requiredFile"
        }
    }

    New-Item -ItemType Directory -Path $Destination -ErrorAction Stop | Out-Null
    $copied = [Collections.Generic.List[string]]::new()
    $skipped = [Collections.Generic.List[string]]::new()

    foreach ($item in Get-ChildItem -LiteralPath $sourceRoot -Force) {
        $isAllowedFile = !$item.PSIsContainer -and
            ($allowedFiles -contains $item.Name -or $item.Extension.Equals(".dll", [StringComparison]::OrdinalIgnoreCase))
        $isAllowedDirectory = $item.PSIsContainer -and $allowedDirectories -contains $item.Name
        if (!$isAllowedFile -and !$isAllowedDirectory) {
            $skipped.Add($item.Name)
            continue
        }

        $target = Join-Path $Destination $item.Name
        Copy-Item -LiteralPath $item.FullName -Destination $target -Recurse:$item.PSIsContainer -Force:$false
        if ($item.PSIsContainer) {
            foreach ($file in Get-ChildItem -LiteralPath $target -File -Recurse -Force) {
                $copied.Add($file.FullName.Substring($Destination.Length).TrimStart('\', '/').Replace('\', '/'))
            }
        } else {
            $copied.Add($item.Name)
        }
    }

    return [ordered]@{
        source = $sourceRoot
        copied_files = @($copied | Sort-Object)
        skipped_top_level_entries = @($skipped | Sort-Object)
    }
}

function Get-InputManifest {
    param([Parameter(Mandatory = $true)] [string] $InputRoot)

    $entries = foreach ($file in Get-ChildItem -LiteralPath $InputRoot -File -Recurse -Force |
        Where-Object { $_.Name -ne "expected-input-manifest.json" }) {
        [ordered]@{
            path = $file.FullName.Substring($InputRoot.Length).TrimStart('\', '/').Replace('\', '/')
            length = $file.Length
            sha256 = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    }
    return [ordered]@{
        schema = "nekoray.windows_sandbox_input.v1"
        entries = @($entries | Sort-Object path)
    }
}

if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
    $runId = "{0}-{1}" -f (Get-Date -Format "yyyyMMdd-HHmmss"), ([Guid]::NewGuid().ToString("N").Substring(0, 8))
    $OutputRoot = Join-Path $repoRoot "artifacts\windows-sandbox\$runId"
}
$runRoot = Assert-PathOutsideProtectedProduction ([IO.Path]::GetFullPath($OutputRoot)) "Windows Sandbox run root"
if (Test-Path -LiteralPath $runRoot) {
    throw "Windows Sandbox run root already exists; refusing to overwrite it: $runRoot"
}

$inputRoot = Join-Path $runRoot "input"
$resultsRoot = Join-Path $runRoot "results"
New-Item -ItemType Directory -Path $inputRoot -Force | Out-Null
New-Item -ItemType Directory -Path $resultsRoot -Force | Out-Null

$runnerSource = Join-Path $PSScriptRoot "run_windows_sandbox_validation.ps1"
if (!(Test-Path -LiteralPath $runnerSource -PathType Leaf)) {
    throw "Sandbox runner is missing: $runnerSource"
}
Copy-Item -LiteralPath $runnerSource -Destination (Join-Path $inputRoot "run_windows_sandbox_validation.ps1")

$packageCopy = $null
if (![string]::IsNullOrWhiteSpace($PackageDir)) {
    $resolvedPackageDir = (Resolve-Path -LiteralPath $PackageDir).Path
    if ((Test-PathInsideRoot $runRoot $resolvedPackageDir) -or
        (Test-PathInsideRoot $resolvedPackageDir $runRoot)) {
        throw "Package source and Windows Sandbox run root must not contain one another."
    }
    $packageCopy = Copy-AllowlistedPackage $resolvedPackageDir (Join-Path $inputRoot "package")
}

$inputManifest = Get-InputManifest $inputRoot
Write-Utf8Json (Join-Path $inputRoot "expected-input-manifest.json") $inputManifest 6

$escapedInput = [Security.SecurityElement]::Escape($inputRoot)
$escapedResults = [Security.SecurityElement]::Escape($resultsRoot)
$wsbPath = Join-Path $runRoot "nekoray-offline-validation.wsb"
$wsb = @"
<Configuration>
  <VGpu>Disable</VGpu>
  <Networking>Disable</Networking>
  <AudioInput>Disable</AudioInput>
  <VideoInput>Disable</VideoInput>
  <PrinterRedirection>Disable</PrinterRedirection>
  <ClipboardRedirection>Disable</ClipboardRedirection>
  <MemoryInMB>8192</MemoryInMB>
  <MappedFolders>
    <MappedFolder>
      <HostFolder>$escapedInput</HostFolder>
      <SandboxFolder>C:\NekoRayInput</SandboxFolder>
      <ReadOnly>true</ReadOnly>
    </MappedFolder>
    <MappedFolder>
      <HostFolder>$escapedResults</HostFolder>
      <SandboxFolder>C:\NekoRayResults</SandboxFolder>
      <ReadOnly>false</ReadOnly>
    </MappedFolder>
  </MappedFolders>
  <LogonCommand>
    <Command>powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File C:\NekoRayInput\run_windows_sandbox_validation.ps1 -InputRoot C:\NekoRayInput -ResultsRoot C:\NekoRayResults</Command>
  </LogonCommand>
</Configuration>
"@
[IO.File]::WriteAllText($wsbPath, $wsb, [Text.UTF8Encoding]::new($false))

$featureState = "Unknown"
try {
    $featureState = (Get-WindowsOptionalFeature -Online -FeatureName "Containers-DisposableClientVM").State.ToString()
} catch {
    $featureState = "Unavailable: $($_.Exception.Message)"
}
$sandboxExecutable = Join-Path $env:SystemRoot "System32\WindowsSandbox.exe"
$sandboxExecutablePresent = Test-Path -LiteralPath $sandboxExecutable -PathType Leaf
$rebootPending = (Test-Path -LiteralPath "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Component Based Servicing\RebootPending") -or
    (Test-Path -LiteralPath "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\WindowsUpdate\Auto Update\RebootRequired")
$sandboxReady = $featureState -eq "Enabled" -and $sandboxExecutablePresent

$preparation = [ordered]@{
    schema = "nekoray.windows_sandbox_preparation.v1"
    created_utc = [DateTime]::UtcNow.ToString("o")
    run_root = $runRoot
    package_staged = $null -ne $packageCopy
    package_copy = $packageCopy
    feature_state = $featureState
    sandbox_executable_present = $sandboxExecutablePresent
    reboot_pending = $rebootPending
    sandbox_ready = $sandboxReady
    networking = "disabled"
    input_mapping = "read-only"
    results_mapping = "read-write"
    wsb_path = $wsbPath
}
Write-Utf8Json (Join-Path $runRoot "host-preparation.json") $preparation 8

if ($Launch) {
    if (!$sandboxReady) {
        throw "Windows Sandbox is not ready. Complete the pending Windows restart, then rerun this command with -Launch. Prepared run: $runRoot"
    }
    Start-Process -FilePath $wsbPath | Out-Null
}

$preparation | ConvertTo-Json -Depth 8
