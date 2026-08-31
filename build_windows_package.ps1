param(
    [string] $BuildDir = "build-package-windows64",
    [string] $QtDir = "",
    [string] $MingwDir = "",
    [string] $DepsDir = "",
    [string] $ReferenceDir = "",
    [switch] $RefreshGeodata,
    [switch] $SkipGoBuild,
    [switch] $SkipGuiBuild,
    [int] $Parallel = [Environment]::ProcessorCount
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$Root = $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($Root)) {
    $Root = (Get-Location).Path
}
. (Join-Path $Root "tools\path_safety.ps1")
$Root = Assert-PathOutsideProtectedProduction $Root "Repository root"
$SafeTempRoot = Assert-PathOutsideProtectedProduction ([IO.Path]::GetFullPath([IO.Path]::GetTempPath())) "Windows package temporary directory"
$RouteFluentSingBoxVersion = "1.13.12-routefluent-anytls-client.7"
$RouteFluentSingBoxPatchId = "routefluent-anytls-client-dns-resolver-group-check-v1"
$RouteFluentSingBoxFeatures = @(
    "anytls_outbound_client_field",
    "routefluent_dns_resolver_group",
    "routefluent_dns_check_start_validation"
)
$GoBuildTags = "with_clash_api,with_gvisor,with_quic,with_wireguard,with_utls"
$RouteFluentCoreTags = $GoBuildTags -replace ",", " "

function Write-Step([string] $Message) {
    Write-Host ""
    Write-Host "==> $Message" -ForegroundColor Cyan
}

function Fail([string] $Message) {
    throw $Message
}

function Invoke-Checked {
    param(
        [string] $FilePath,
        [string[]] $Arguments,
        [string] $Label = $FilePath
    )

    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        Fail "$Label failed with exit code $LASTEXITCODE"
    }
}

function Invoke-ExpectedFailure {
    param(
        [string] $FilePath,
        [string[]] $Arguments,
        [string] $Label = $FilePath
    )

    & $FilePath @Arguments
    if ($LASTEXITCODE -eq 0) {
        Fail "$Label unexpectedly succeeded"
    }
}

function Get-FullPath([string] $Path) {
    if ([System.IO.Path]::IsPathRooted($Path)) {
        return [System.IO.Path]::GetFullPath($Path)
    }
    return [System.IO.Path]::GetFullPath((Join-Path $Root $Path))
}

function Require-File([string] $Path, [string] $Name) {
    if (!(Test-Path -LiteralPath $Path -PathType Leaf)) {
        Fail "$Name not found: $Path"
    }
    return (Resolve-Path -LiteralPath $Path).Path
}

function Require-Directory([string] $Path, [string] $Name) {
    if (!(Test-Path -LiteralPath $Path -PathType Container)) {
        Fail "$Name not found: $Path"
    }
    return (Resolve-Path -LiteralPath $Path).Path
}

function Remove-SafeDirectory([string] $Path, [string] $AllowedRoot) {
    $full = Assert-PathOutsideProtectedProduction ([System.IO.Path]::GetFullPath($Path)) "Recursive removal target"
    $allowed = (Assert-PathOutsideProtectedProduction ([System.IO.Path]::GetFullPath($AllowedRoot)) "Recursive removal boundary").TrimEnd('\')
    if (!$full.StartsWith($allowed + '\', [System.StringComparison]::OrdinalIgnoreCase)) {
        Fail "Refusing to remove directory outside allowed root: $full"
    }
    if (Test-Path -LiteralPath $full) {
        Assert-DirectoryTreeHasNoReparsePoints $full "Recursive removal target"
        Remove-Item -LiteralPath $full -Recurse -Force
    }
}

function Get-PackageProcessInfos([string] $PackageDir) {
    $packageRoot = [System.IO.Path]::GetFullPath($PackageDir).TrimEnd('\')
    $knownProcessNames = @(
        "nekobox.exe",
        "nekobox_core.exe",
        "nekoray.exe",
        "nekoray_core.exe",
        "launcher.exe",
        "updater.exe"
    )

    @(Get-CimInstance Win32_Process |
        Where-Object {
            $_.ExecutablePath -and
            $knownProcessNames -contains [System.IO.Path]::GetFileName($_.ExecutablePath).ToLowerInvariant() -and
            [System.IO.Path]::GetFullPath($_.ExecutablePath).StartsWith($packageRoot + '\', [System.StringComparison]::OrdinalIgnoreCase)
        })
}

function Assert-PackageNotRunning([string] $PackageDir) {
    $processInfos = @(Get-PackageProcessInfos $PackageDir)
    if ($processInfos.Count -eq 0) {
        return
    }

    Write-Step "Running package instance detected"
    $processInfos |
        Select-Object @{Name = "ProcessName"; Expression = { $_.Name } }, ProcessId, ExecutablePath |
        Format-Table -AutoSize
    Fail (
        "Refusing to build over a running package instance. " +
        "The build script never closes GUI/core processes because that could change proxy/TUN state. " +
        "Stop the exact package instance manually, then rerun the build. PID(s): " +
        ($processInfos.ProcessId -join ', ')
    )
}

function Backup-PackageConfig([string] $PackageConfigDir, [string] $PackageConfigBackupDir) {
    if (Test-Path -LiteralPath $PackageConfigBackupDir) {
        Fail "Stale package config backup exists and must be reviewed manually before building: $PackageConfigBackupDir"
    }
    if (!(Test-Path -LiteralPath $PackageConfigDir -PathType Container)) {
        Write-Host "No existing package config to preserve."
        return $false
    }

    Assert-DirectoryTreeHasNoReparsePoints $PackageConfigDir "Package config backup source"
    try {
        Copy-Item -LiteralPath $PackageConfigDir -Destination $PackageConfigBackupDir -Recurse -Force
    } catch {
        if (Test-Path -LiteralPath $PackageConfigBackupDir) {
            Remove-SafeDirectory $PackageConfigBackupDir (Split-Path -Parent $PackageConfigBackupDir)
        }
        throw
    }
    $fileCount = (Get-ChildItem -LiteralPath $PackageConfigBackupDir -Recurse -File -Force | Measure-Object).Count
    Write-Host "Preserved package config: $PackageConfigBackupDir ($fileCount file(s))"
    return $true
}

function Restore-PackageConfig([string] $PackageConfigDir, [string] $PackageConfigBackupDir, [string] $PackageDir) {
    if (!(Test-Path -LiteralPath $PackageConfigBackupDir -PathType Container)) {
        Fail "Owned package config backup is missing or is not a directory: $PackageConfigBackupDir"
    }

    Assert-DirectoryTreeHasNoReparsePoints $PackageConfigBackupDir "Package config restore source"
    New-Item -ItemType Directory -Force -Path $PackageDir | Out-Null
    Remove-SafeDirectory $PackageConfigDir $PackageDir
    Copy-Item -LiteralPath $PackageConfigBackupDir -Destination $PackageConfigDir -Recurse -Force
    $fileCount = (Get-ChildItem -LiteralPath $PackageConfigDir -Recurse -File -Force | Measure-Object).Count
    Write-Host "Restored package config: $PackageConfigDir ($fileCount file(s))"
}

function Backup-PackageBinaries([string] $PackageDir, [string] $PackageBinaryBackupDir) {
    if (Test-Path -LiteralPath $PackageBinaryBackupDir) {
        Fail "Stale package binary backup exists and must be reviewed manually before building: $PackageBinaryBackupDir"
    }
    Assert-DirectoryTreeHasNoReparsePoints $PackageDir "Package binary backup source"
    $sources = @()
    foreach ($name in @("updater.exe", "nekobox_core.exe")) {
        $source = Assert-PathOutsideProtectedProduction (Join-Path $PackageDir $name) "Package binary backup source file"
        if (Test-Path -LiteralPath $source -PathType Leaf) {
            $sources += [pscustomobject]@{ name = $name; path = $source }
        }
    }
    if ($sources.Count -eq 0) {
        Write-Warning "SkipGoBuild requested but no existing Go binaries were found under $PackageDir."
        return $false
    }

    New-Item -ItemType Directory -Path $PackageBinaryBackupDir | Out-Null
    try {
        foreach ($sourceInfo in $sources) {
            $destination = Assert-NewFileOutsideProtectedProduction (Join-Path $PackageBinaryBackupDir $sourceInfo.name) "Package binary backup destination"
            Copy-Item -LiteralPath $sourceInfo.path -Destination $destination
        }
    } catch {
        Remove-SafeDirectory $PackageBinaryBackupDir (Split-Path -Parent $PackageBinaryBackupDir)
        throw
    }
    Write-Host "Preserved package Go binaries: $PackageBinaryBackupDir ($($sources.Count) file(s))"
    return $true
}

function Restore-PackageBinaries([string] $PackageDir, [string] $PackageBinaryBackupDir) {
    if (!(Test-Path -LiteralPath $PackageBinaryBackupDir -PathType Container)) {
        Fail "Owned package binary backup is missing or is not a directory: $PackageBinaryBackupDir"
    }
    Assert-DirectoryTreeHasNoReparsePoints $PackageBinaryBackupDir "Package binary restore source"
    $PackageDir = Assert-PathOutsideProtectedProduction $PackageDir "Package binary restore directory"
    New-Item -ItemType Directory -Force -Path $PackageDir | Out-Null
    foreach ($name in @("updater.exe", "nekobox_core.exe")) {
        $source = Assert-PathOutsideProtectedProduction (Join-Path $PackageBinaryBackupDir $name) "Package binary restore source file"
        if (Test-Path -LiteralPath $source -PathType Leaf) {
            $destination = Initialize-NewBuildFile (Join-Path $PackageDir $name) "Package binary restore destination"
            Copy-Item -LiteralPath $source -Destination $destination
        }
    }
}

function Find-QtDir {
    param([string] $Requested)
    if (![string]::IsNullOrWhiteSpace($Requested)) {
        $candidate = Get-FullPath $Requested
        Require-File (Join-Path $candidate "bin\windeployqt.exe") "windeployqt.exe" | Out-Null
        return (Resolve-Path -LiteralPath $candidate).Path
    }

    $candidates = @()
    $qtRoot = Join-Path $Root "qtsdk\qt"
    if (Test-Path -LiteralPath $qtRoot) {
        $candidates += Get-ChildItem -LiteralPath $qtRoot -Directory -Recurse |
            Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName "bin\windeployqt.exe") } |
            Sort-Object FullName -Descending
    }
    if ($candidates.Count -eq 0) {
        Fail "Project-local Qt was not found. Expected something like qtsdk\qt\<version>\mingw_64 with bin\windeployqt.exe."
    }
    return $candidates[0].FullName
}

function Find-MingwDir {
    param([string] $Requested)
    if (![string]::IsNullOrWhiteSpace($Requested)) {
        $candidate = Get-FullPath $Requested
        Require-File (Join-Path $candidate "bin\g++.exe") "g++.exe" | Out-Null
        return (Resolve-Path -LiteralPath $candidate).Path
    }

    $toolsRoot = Join-Path $Root "qtsdk\tools\Tools"
    $candidates = @()
    if (Test-Path -LiteralPath $toolsRoot) {
        $candidates += Get-ChildItem -LiteralPath $toolsRoot -Directory |
            Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName "bin\g++.exe") } |
            Sort-Object FullName -Descending
    }
    if ($candidates.Count -eq 0) {
        Fail "Project-local MinGW was not found. Expected something like qtsdk\tools\Tools\mingw*_64 with bin\g++.exe."
    }
    return $candidates[0].FullName
}

function Find-CommandPath([string] $Name) {
    $cmd = Get-Command $Name -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -eq $cmd) {
        return ""
    }
    return $cmd.Source
}

function Download-Or-Fallback {
    param(
        [string] $Name,
        [string] $Url,
        [string] $TargetDir,
        [string] $FallbackDir,
        [switch] $Refresh
    )

    $target = Assert-PathOutsideProtectedProduction (Join-Path $TargetDir $Name) "Downloaded package resource"
    if (!$Refresh -and (Test-Path -LiteralPath $target) -and ((Get-Item -LiteralPath $target).Length -gt 0)) {
        return
    }

    $tmp = Initialize-NewBuildFile "$target.download" "Package resource download staging file"

    try {
        Write-Host "Downloading $Name"
        Invoke-WebRequest -Uri $Url -OutFile $tmp -UseBasicParsing
        if (!(Test-Path -LiteralPath $tmp) -or ((Get-Item -LiteralPath $tmp).Length -le 0)) {
            Fail "Downloaded file is empty: $Name"
        }
        $newTarget = Initialize-NewBuildFile $target "Downloaded package resource"
        Move-Item -LiteralPath $tmp -Destination $newTarget
        return
    } catch {
        if (Test-Path -LiteralPath $tmp) {
            Remove-Item -LiteralPath $tmp -Force
        }
        $fallback = if ([string]::IsNullOrWhiteSpace($FallbackDir)) {
            ""
        } else {
            Join-Path $FallbackDir $Name
        }
        if (![string]::IsNullOrWhiteSpace($fallback) -and (Test-Path -LiteralPath $fallback -PathType Leaf)) {
            Write-Warning "Download failed for $Name, using reference copy: $fallback"
            Copy-BuildFile $fallback $target "Fallback package resource"
            return
        }
        throw
    }
}

function Copy-IfExists([string] $Source, [string] $DestinationDir) {
    if (Test-Path -LiteralPath $Source -PathType Leaf) {
        Copy-BuildFile $Source (Join-Path $DestinationDir ([IO.Path]::GetFileName($Source))) "Optional package dependency"
    }
}

function Initialize-NewBuildFile([string] $Path, [string] $Purpose) {
    $safePath = Assert-PathOutsideProtectedProduction ([IO.Path]::GetFullPath($Path)) $Purpose
    if (Test-Path -LiteralPath $safePath) {
        if (Test-Path -LiteralPath $safePath -PathType Container) {
            Fail "$Purpose expected a file but found a directory: $safePath"
        }
        Remove-Item -LiteralPath $safePath -Force
    }
    return Assert-NewFileOutsideProtectedProduction $safePath $Purpose
}

function Copy-BuildFile([string] $Source, [string] $Destination, [string] $Purpose) {
    $safeSource = Assert-PathOutsideProtectedProduction ([IO.Path]::GetFullPath($Source)) "$Purpose source"
    if (!(Test-Path -LiteralPath $safeSource -PathType Leaf)) {
        Fail "$Purpose source file not found: $safeSource"
    }
    $safeDestination = Initialize-NewBuildFile $Destination "$Purpose destination"
    Copy-Item -LiteralPath $safeSource -Destination $safeDestination
}

function Require-PackageFile([string] $RelativePath) {
    $path = Join-Path $PackageDir $RelativePath
    if (!(Test-Path -LiteralPath $path -PathType Leaf) -or ((Get-Item -LiteralPath $path).Length -le 0)) {
        Fail "Package is missing required file: $RelativePath"
    }
}

function Find-Python {
    foreach ($name in @("python.exe", "python3.exe")) {
        $candidate = Find-CommandPath $name
        if (![string]::IsNullOrWhiteSpace($candidate)) {
            return $candidate
        }
    }
    Fail "python.exe was not found. RouteFluent patched sing-box source preparation requires Python 3.9+."
}

function Assert-RouteFluentManifest([string] $ManifestPath) {
    $manifest = Get-Content -LiteralPath $ManifestPath -Raw | ConvertFrom-Json
    if ($manifest.version_name -ne $RouteFluentSingBoxVersion) {
        Fail "Unexpected RouteFluent sing-box version: $($manifest.version_name)"
    }
    if ($manifest.patch_id -ne $RouteFluentSingBoxPatchId) {
        Fail "Unexpected RouteFluent sing-box patch id: $($manifest.patch_id)"
    }
    foreach ($feature in $RouteFluentSingBoxFeatures) {
        if ($manifest.features -notcontains $feature) {
            Fail "RouteFluent sing-box manifest is missing feature: $feature"
        }
    }
    foreach ($tag in $RouteFluentCoreTags.Split(" ", [System.StringSplitOptions]::RemoveEmptyEntries)) {
        if ($manifest.tags -notcontains $tag) {
            Fail "RouteFluent sing-box manifest is missing build tag: $tag"
        }
    }
}

$packageConfigBackupOwned = $false
$packageBinaryBackupOwned = $false
$packageReplacementStarted = $false
Push-Location $Root
try {
    Write-Step "Resolve local toolchain"

    $QtDir = Find-QtDir $QtDir
    $MingwDir = Find-MingwDir $MingwDir
    $QtDir = Assert-PathOutsideProtectedProduction $QtDir "Qt toolchain directory"
    $MingwDir = Assert-PathOutsideProtectedProduction $MingwDir "MinGW toolchain directory"
    $QtBin = Assert-PathOutsideProtectedProduction (Join-Path $QtDir "bin") "Qt executable directory"
    $MingwBin = Assert-PathOutsideProtectedProduction (Join-Path $MingwDir "bin") "MinGW executable directory"
    $WinDeployQt = Assert-PathOutsideProtectedProduction (Require-File (Join-Path $QtBin "windeployqt.exe") "windeployqt.exe") "windeployqt executable"
    $QtPaths = Assert-PathOutsideProtectedProduction (Require-File (Join-Path $QtBin "qtpaths.exe") "qtpaths.exe") "qtpaths executable"
    $Gcc = Assert-PathOutsideProtectedProduction (Require-File (Join-Path $MingwBin "gcc.exe") "gcc.exe") "GCC executable"
    $Gxx = Assert-PathOutsideProtectedProduction (Require-File (Join-Path $MingwBin "g++.exe") "g++.exe") "G++ executable"
    $DepsDir = if ([string]::IsNullOrWhiteSpace($DepsDir)) {
        Join-Path $Root "libs\deps\built"
    } else {
        Get-FullPath $DepsDir
    }
    $DepsDir = Assert-PathOutsideProtectedProduction $DepsDir "CMake dependency prefix"
    Require-Directory $DepsDir "CMake dependency prefix" | Out-Null
    $DepsBin = Assert-PathOutsideProtectedProduction (Join-Path $DepsDir "bin") "Dependency executable directory"

    $CMake = Find-CommandPath "cmake.exe"
    if ([string]::IsNullOrWhiteSpace($CMake)) {
        Fail "cmake.exe was not found."
    }
    $CMake = Assert-PathOutsideProtectedProduction $CMake "CMake executable"
    $Go = Find-CommandPath "go.exe"
    if ([string]::IsNullOrWhiteSpace($Go)) {
        Fail "go.exe was not found."
    }
    $Go = Assert-PathOutsideProtectedProduction $Go "Go executable"
    $Python = Find-Python
    $Python = Assert-PathOutsideProtectedProduction $Python "Python executable"
    $Git = Find-CommandPath "git.exe"
    if ([string]::IsNullOrWhiteSpace($Git)) {
        Fail "git.exe was not found. RouteFluent patched sing-box source preparation requires Git."
    }
    $Git = Assert-PathOutsideProtectedProduction $Git "Git executable"

    $Ninja = Find-CommandPath "ninja.exe"
    if (![string]::IsNullOrWhiteSpace($Ninja)) {
        $Ninja = Assert-PathOutsideProtectedProduction $Ninja "Ninja executable"
        $Generator = "Ninja"
        $MakeProgram = $Ninja
    } else {
        $Generator = "MinGW Makefiles"
        $MakeProgram = Assert-PathOutsideProtectedProduction (Require-File (Join-Path $MingwBin "mingw32-make.exe") "mingw32-make.exe") "MinGW Make executable"
    }

    $Version = (Get-Content -LiteralPath (Join-Path $Root "nekoray_version.txt") -Raw).Trim()
    $VersionStandalone = "nekoray-$Version"
    $BuildDirFull = Get-FullPath $BuildDir
    $BuildDirFull = Assert-PathOutsideProtectedProduction $BuildDirFull "Windows package build directory"
    $DeployRoot = Assert-PathOutsideProtectedProduction (Join-Path $Root "deployment") "Windows package deployment root"
    $PackageDir = Assert-PathOutsideProtectedProduction (Join-Path $DeployRoot "windows64") "Windows package directory"
    $PackageConfigDir = Assert-PathOutsideProtectedProduction (Join-Path $PackageDir "config") "Preserved package config directory"
    $PackageConfigBackupDir = Assert-PathOutsideProtectedProduction (Join-Path $DeployRoot "windows64-config-preserve") "Package config backup directory"
    $PackageBinaryBackupDir = Assert-PathOutsideProtectedProduction (Join-Path $DeployRoot "windows64-binary-preserve") "Package binary backup directory"
    $PublicResDir = Assert-PathOutsideProtectedProduction (Join-Path $DeployRoot "public_res") "Package public resource directory"
    $RouteFluentWorkDir = Assert-PathOutsideProtectedProduction (Join-Path $Root "third_party\routefluent-sing-box\work") "RouteFluent source work directory"
    $RouteFluentCoreBuildDir = Assert-PathOutsideProtectedProduction (Join-Path $Root "build-routefluent-sing-box") "RouteFluent core build directory"
    $RouteFluentCoreExe = Assert-PathOutsideProtectedProduction (Join-Path $RouteFluentCoreBuildDir "sing-box-windows-amd64.exe") "RouteFluent core output"
    $RouteFluentCoreManifest = Assert-PathOutsideProtectedProduction (Join-Path $RouteFluentCoreBuildDir "sing-box-windows-amd64.routefluent-anytls-client.json") "RouteFluent manifest output"
    $ZipStageRoot = Assert-PathOutsideProtectedProduction (Join-Path $DeployRoot "zip-stage") "Windows package zip staging root"
    $ZipStagePackage = Assert-PathOutsideProtectedProduction (Join-Path $ZipStageRoot "nekoray") "Windows package zip staging directory"
    $ZipPath = Assert-PathOutsideProtectedProduction (Join-Path $DeployRoot "$VersionStandalone-windows64.zip") "Windows package archive"

    Assert-DirectoryTreeHasNoReparsePoints $DeployRoot "Windows package deployment tree"
    Assert-DirectoryTreeHasNoReparsePoints $BuildDirFull "Windows GUI build tree"
    Assert-DirectoryTreeHasNoReparsePoints $RouteFluentWorkDir "RouteFluent source work tree"
    Assert-DirectoryTreeHasNoReparsePoints $RouteFluentCoreBuildDir "RouteFluent core build tree"
    foreach ($staleBackup in @($PackageConfigBackupDir, $PackageBinaryBackupDir)) {
        if (Test-Path -LiteralPath $staleBackup) {
            Fail "Stale package backup exists. Review and recover it manually before building; this run will not delete or restore it: $staleBackup"
        }
    }

    Write-Host "Root:       $Root"
    Write-Host "Qt:         $QtDir"
    Write-Host "MinGW:      $MingwDir"
    Write-Host "Deps:       $DepsDir"
    Write-Host "Generator:  $Generator"
    Write-Host "Build dir:  $BuildDirFull"
    Write-Host "Package:    $PackageDir"
    Write-Host "RF core:    $RouteFluentSingBoxVersion"

    $oldPath = $env:PATH
    $oldGoos = $env:GOOS
    $oldGoarch = $env:GOARCH
    $oldCgo = $env:CGO_ENABLED
    $oldQtPluginPath = $env:QT_PLUGIN_PATH
    $oldQtQpaPlatformPluginPath = $env:QT_QPA_PLATFORM_PLUGIN_PATH
    $oldQml2ImportPath = $env:QML2_IMPORT_PATH
    $env:PATH = "$MingwBin;$QtBin;$DepsBin;$oldPath"
    $env:QT_PLUGIN_PATH = Join-Path $QtDir "plugins"
    $env:QT_QPA_PLATFORM_PLUGIN_PATH = Join-Path $QtDir "plugins\platforms"
    $env:QML2_IMPORT_PATH = Join-Path $QtDir "qml"
    $env:GOOS = "windows"
    $env:GOARCH = "amd64"
    $env:CGO_ENABLED = "0"

    New-Item -ItemType Directory -Force -Path $DeployRoot | Out-Null
    Assert-PackageNotRunning $PackageDir
    $packageConfigBackupOwned = [bool](Backup-PackageConfig $PackageConfigDir $PackageConfigBackupDir)
    if ($SkipGoBuild) {
        $packageBinaryBackupOwned = [bool](Backup-PackageBinaries $PackageDir $PackageBinaryBackupDir)
    }
    $packageReplacementStarted = $true
    Remove-SafeDirectory $PackageDir $DeployRoot
    Remove-SafeDirectory $ZipStageRoot $DeployRoot
    New-Item -ItemType Directory -Force -Path $PackageDir | Out-Null

    Write-Step "Prepare RouteFluent patched sing-box"
    Invoke-Checked $Git @("submodule", "update", "--init", "--recursive", "third_party/routefluent-sing-box") "RouteFluent sing-box submodule init"
    $RouteFluentSingBoxScript = Require-File (Join-Path $Root "third_party\routefluent-sing-box\build_routefluent_sing_box.py") "RouteFluent sing-box build script"
    $RouteFluentCoreExe = Initialize-NewBuildFile $RouteFluentCoreExe "RouteFluent core output"
    $RouteFluentCoreManifest = Initialize-NewBuildFile $RouteFluentCoreManifest "RouteFluent manifest output"
    Invoke-Checked $Python @(
        $RouteFluentSingBoxScript,
        "--goos", "windows",
        "--goarch", "amd64",
        "--tags", $RouteFluentCoreTags,
        "--output", $RouteFluentCoreExe,
        "--manifest", $RouteFluentCoreManifest
    ) "RouteFluent sing-box source preparation"
    Require-Directory (Join-Path $RouteFluentWorkDir "src\sing-box-1.13.12") "RouteFluent patched sing-box source" | Out-Null
    Assert-RouteFluentManifest $RouteFluentCoreManifest

    if (!$SkipGuiBuild) {
        Write-Step "Reset, configure, and build GUI"
        # A formal package must not inherit untracked objects, manually built
        # diagnostics, or a CMake cache from an earlier source/protocol state.
        # The guarded helper rejects the repository root, production paths,
        # aliases, and reparse trees before removing this exact build subtree.
        Remove-SafeDirectory $BuildDirFull $Root
        New-Item -ItemType Directory -Force -Path $BuildDirFull | Out-Null
        $cmakeArgs = @(
            "-S", $Root,
            "-B", $BuildDirFull,
            "-G", $Generator,
            "-Wno-dev",
            "-DQT_VERSION_MAJOR=6",
            "-DBUILD_TESTING=ON",
            "-DCMAKE_BUILD_TYPE=Release",
            "-DCMAKE_POLICY_VERSION_MINIMUM=3.5",
            "-DCMAKE_PREFIX_PATH=$QtDir",
            "-DNKR_LIBS=$DepsDir",
            "-DCMAKE_C_COMPILER=$Gcc",
            "-DCMAKE_CXX_COMPILER=$Gxx",
            "-DCMAKE_MAKE_PROGRAM=$MakeProgram"
        )
        Invoke-Checked $CMake $cmakeArgs "CMake configure"
        Invoke-Checked $CMake @("--build", $BuildDirFull, "--config", "Release", "--parallel", "$Parallel") "CMake build"
    } else {
        Write-Step "Skip GUI build"
    }

    $GuiExe = Require-File (Join-Path $BuildDirFull "nekobox.exe") "nekobox.exe"
    Copy-BuildFile $GuiExe (Join-Path $PackageDir "nekobox.exe") "Windows GUI package binary"

    Write-Step "Deploy Qt runtime"
    Invoke-Checked $WinDeployQt @(
        (Join-Path $PackageDir "nekobox.exe"),
        "--force",
        "--no-compiler-runtime",
        "--no-system-d3d-compiler",
        "--no-opengl-sw",
        "--no-translations",
        "--qtpaths", $QtPaths,
        "--verbose", "1"
    ) "windeployqt"

    foreach ($unneeded in @("translations", "d3dcompiler_47.dll", "opengl32sw.dll", "libEGL.dll", "libGLESv2.dll", "Qt6Pdf.dll")) {
        $path = Assert-PathOutsideProtectedProduction (Join-Path $PackageDir $unneeded) "Optional package cleanup target"
        if (Test-Path -LiteralPath $path) {
            if (Test-Path -LiteralPath $path -PathType Container) {
                Assert-DirectoryTreeHasNoReparsePoints $path "Optional package cleanup target"
            }
            Remove-Item -LiteralPath $path -Recurse -Force
        }
    }

    Write-Step "Copy compiler runtime and public assets"
    foreach ($runtimeDll in @("libgcc_s_seh-1.dll", "libstdc++-6.dll", "libwinpthread-1.dll")) {
        Copy-BuildFile (Require-File (Join-Path $MingwBin $runtimeDll) $runtimeDll) (Join-Path $PackageDir $runtimeDll) "Compiler runtime $runtimeDll"
    }
    foreach ($opensslDll in @("libcrypto-3-x64.dll", "libssl-3-x64.dll", "libcrypto-1_1-x64.dll", "libssl-1_1-x64.dll")) {
        Copy-IfExists (Join-Path $QtBin $opensslDll) $PackageDir
        Copy-IfExists (Join-Path $MingwBin $opensslDll) $PackageDir
    }

    New-Item -ItemType Directory -Force -Path $PublicResDir | Out-Null
    foreach ($publicAsset in Get-ChildItem -LiteralPath (Join-Path $Root "res\public") -File) {
        Copy-BuildFile $publicAsset.FullName (Join-Path $PublicResDir $publicAsset.Name) "Public package asset"
    }
    $fallback = if (![string]::IsNullOrWhiteSpace($ReferenceDir)) {
        $referenceCandidate = Get-FullPath $ReferenceDir
        # Reject the protected path before even probing its existence.
        $referenceCandidate = Assert-PathOutsideProtectedProduction $referenceCandidate "Reference geodata directory"
        if (Test-Path -LiteralPath $referenceCandidate -PathType Container) {
            $resolvedReference = (Resolve-Path -LiteralPath $referenceCandidate).Path
            $resolvedReference = Assert-PathOutsideProtectedProduction $resolvedReference "Resolved reference geodata directory"
            $resolvedReference
        } else {
            ""
        }
    } else {
        ""
    }
    Download-Or-Fallback "geoip.db" "https://github.com/SagerNet/sing-geoip/releases/latest/download/geoip.db" $PublicResDir $fallback -Refresh:$RefreshGeodata
    Download-Or-Fallback "geosite.db" "https://github.com/SagerNet/sing-geosite/releases/latest/download/geosite.db" $PublicResDir $fallback -Refresh:$RefreshGeodata
    foreach ($publicResource in Get-ChildItem -LiteralPath $PublicResDir -File |
        Where-Object { $_.Name -notin @("geoip.dat", "geosite.dat") }) {
        Copy-BuildFile $publicResource.FullName (Join-Path $PackageDir $publicResource.Name) "Prepared package resource"
    }

    if (!$SkipGoBuild) {
        Write-Step "Build updater.exe"
        Push-Location (Join-Path $Root "go\cmd\updater")
        try {
            Invoke-Checked $Go @("build", "-o", (Join-Path $PackageDir "updater.exe"), "-trimpath", "-ldflags", "-w -s") "go build updater"
        } finally {
            Pop-Location
        }

        Write-Step "Build nekobox_core.exe"
        Push-Location (Join-Path $Root "go\cmd\nekobox_core")
        try {
            $ldflags = "-w -s -X github.com/matsuridayo/libneko/neko_common.Version_neko=$VersionStandalone -X github.com/sagernet/sing-box/constant.Version=$RouteFluentSingBoxVersion"
            Invoke-Checked $Go @("build", "-v", "-o", (Join-Path $PackageDir "nekobox_core.exe"), "-trimpath", "-ldflags", $ldflags, "-tags", $GoBuildTags) "go build nekobox_core"
        } finally {
            Pop-Location
        }
    } else {
        Write-Step "Skip Go build"
        if ($packageBinaryBackupOwned) {
            Restore-PackageBinaries $PackageDir $PackageBinaryBackupDir
            $packageBinaryBackupOwned = $false
            Remove-SafeDirectory $PackageBinaryBackupDir $DeployRoot
        }
        Require-File (Join-Path $PackageDir "updater.exe") "updater.exe" | Out-Null
        Require-File (Join-Path $PackageDir "nekobox_core.exe") "nekobox_core.exe" | Out-Null
    }

    if (!$SkipGuiBuild -and !$SkipGoBuild) {
        Write-Step "Verify authenticated core Exit ACK and exact process finish"
        Assert-DirectoryTreeHasNoReparsePoints `
            $BuildDirFull `
            "core Exit integration build tree"
        $CoreExitIntegrationTestPath = Assert-PathOutsideProtectedProduction `
            (Join-Path $BuildDirFull "core_exit_integration_test.exe") `
            "core Exit integration test executable"
        $CoreExitIntegrationTest = Require-File `
            $CoreExitIntegrationTestPath `
            "core Exit integration test"
        $RuntimeTransitionTestPath = Assert-PathOutsideProtectedProduction `
            (Join-Path $BuildDirFull "runtime_transition_test.exe") `
            "runtime transition test executable"
        $RuntimeTransitionTest = Require-File `
            $RuntimeTransitionTestPath `
            "runtime transition test"
        $ShareFormatTestPath = Assert-PathOutsideProtectedProduction `
            (Join-Path $BuildDirFull "share_format_test.exe") `
            "share format test executable"
        $ShareFormatTest = Require-File `
            $ShareFormatTestPath `
            "share format test"
        $ResolverPolicyTestPath = Assert-PathOutsideProtectedProduction `
            (Join-Path $BuildDirFull "resolver_policy_test.exe") `
            "resolver policy test executable"
        $ResolverPolicyTest = Require-File `
            $ResolverPolicyTestPath `
            "resolver policy test"
        $CoreExitIntegrationWorkRoot = Assert-PathOutsideProtectedProduction `
            (Join-Path $BuildDirFull "core-exit-integration-work") `
            "core Exit integration work root"
        if (Test-Path -LiteralPath $CoreExitIntegrationWorkRoot) {
            Assert-DirectoryTreeHasNoReparsePoints `
                $CoreExitIntegrationWorkRoot `
                "core Exit integration work root"
        } else {
            New-Item -ItemType Directory -Path $CoreExitIntegrationWorkRoot | Out-Null
        }
        Assert-DirectoryTreeHasNoReparsePoints `
            $PackageDir `
            "core Exit integration package tree"
        $CoreExitIntegrationCorePath = Require-File `
            (Assert-PathOutsideProtectedProduction `
                (Join-Path $PackageDir "nekobox_core.exe") `
                "core Exit integration core executable") `
            "current package core"
        $CoreExitIntegrationWorkRoot = (Resolve-Path -LiteralPath $CoreExitIntegrationWorkRoot).Path
        $CoreExitIntegrationCoreSha256 = `
            (Get-FileHash -LiteralPath $CoreExitIntegrationCorePath -Algorithm SHA256).Hash.ToLowerInvariant()
        $CoreExitEnvironment = @{
            "ROUTEFLUENT_CORE_EXIT_TEST_AUTHORIZATION" = "build_windows_package.ps1:v1"
            "ROUTEFLUENT_CORE_EXIT_TEST_CORE_PATH" = $CoreExitIntegrationCorePath
            "ROUTEFLUENT_CORE_EXIT_TEST_CORE_SHA256" = $CoreExitIntegrationCoreSha256
            "ROUTEFLUENT_CORE_EXIT_TEST_WORK_ROOT" = $CoreExitIntegrationWorkRoot
        }
        $CoreExitPreviousEnvironment = @{}
        try {
            Invoke-Checked `
                $RuntimeTransitionTest `
                @() `
                "runtime transition tracker test before core Exit integration"
            Invoke-Checked `
                $ShareFormatTest `
                @() `
                "share format test before core Exit integration"
            Invoke-Checked `
                $ResolverPolicyTest `
                @() `
                "resolver policy test before core Exit integration"
            foreach ($entry in $CoreExitEnvironment.GetEnumerator()) {
                $CoreExitPreviousEnvironment[$entry.Key] = [Environment]::GetEnvironmentVariable(
                    $entry.Key,
                    [EnvironmentVariableTarget]::Process)
                [Environment]::SetEnvironmentVariable(
                    $entry.Key,
                    $entry.Value,
                    [EnvironmentVariableTarget]::Process)
            }
            Invoke-Checked $CoreExitIntegrationTest @(
                $CoreExitIntegrationCorePath,
                $CoreExitIntegrationWorkRoot
            ) "core Exit integration test"
        } finally {
            foreach ($entry in $CoreExitPreviousEnvironment.GetEnumerator()) {
                [Environment]::SetEnvironmentVariable(
                    $entry.Key,
                    $entry.Value,
                    [EnvironmentVariableTarget]::Process)
            }
        }
    } else {
        Write-Step "Skip core Exit integration test (requires current GUI tests and core)"
    }
    Copy-BuildFile $RouteFluentCoreManifest (Join-Path $PackageDir "routefluent-sing-box-manifest.json") "RouteFluent package manifest"

    Write-Step "Validate package contents"
    foreach ($required in @(
        "nekobox.exe",
        "nekobox_core.exe",
        "updater.exe",
        "routefluent-sing-box-manifest.json",
        "nekobox.png",
        "qtbase_zh_CN.qm",
        "geoip.db",
        "geosite.db",
        "Qt6Core.dll",
        "Qt6Gui.dll",
        "Qt6Network.dll",
        "Qt6Svg.dll",
        "Qt6Widgets.dll",
        "libgcc_s_seh-1.dll",
        "libstdc++-6.dll",
        "libwinpthread-1.dll",
        "platforms\qwindows.dll",
        "imageformats\qsvg.dll",
        "iconengines\qsvgicon.dll",
        "styles\qwindowsvistastyle.dll",
        "tls\qschannelbackend.dll"
    )) {
        Require-PackageFile $required
    }

    foreach ($forbidden in @("nekoray.exe", "nekoray_core.exe", "nekoray.png", "d3dcompiler_47.dll", "opengl32sw.dll", "geoip.dat", "geosite.dat")) {
        if (Test-Path -LiteralPath (Join-Path $PackageDir $forbidden)) {
            Fail "Package contains forbidden legacy or optional file: $forbidden"
        }
    }
    if (Test-Path -LiteralPath (Join-Path $PackageDir "config")) {
        Fail "Package must not include user config directory."
    }

    Write-Step "Smoke-check nekobox_core"
    $CoreExe = Join-Path $PackageDir "nekobox_core.exe"
    $versionOutput = & $CoreExe version
    if ($LASTEXITCODE -ne 0) {
        Fail "nekobox_core version failed with exit code $LASTEXITCODE"
    }
    $versionOutputText = ($versionOutput | Out-String).Trim()
    Write-Host $versionOutputText
    if ($versionOutputText -notmatch [Regex]::Escape($RouteFluentSingBoxVersion)) {
        Fail "nekobox_core version does not contain RouteFluent patched sing-box version: $RouteFluentSingBoxVersion"
    }
    $checkConfig = Join-Path $SafeTempRoot ("nekoray-anytls-package-check-{0}.json" -f [Guid]::NewGuid().ToString('N'))
    $checkConfig = Assert-NewFileOutsideProtectedProduction $checkConfig "Package smoke-check configuration"
    @'
{
  "log": { "level": "error" },
  "dns": {
    "servers": [
      { "type": "local", "tag": "dns-direct" }
    ],
    "rules": [
      { "query_type": [32, 33], "action": "predefined", "rcode": "NOERROR" },
      { "domain_suffix": ".lan", "action": "predefined", "rcode": "NOERROR" }
    ],
    "independent_cache": true
  },
  "inbounds": [
    { "type": "mixed", "tag": "mixed-in", "listen": "127.0.0.1", "listen_port": 0 }
  ],
  "outbounds": [
    {
      "type": "anytls",
      "tag": "anytls-out",
      "server": "example.com",
      "server_port": 443,
      "password": "secret",
      "client": "mihomo/1.19.28",
      "tls": {
        "enabled": true,
        "server_name": "example.com",
        "alpn": ["h2", "http/1.1"],
        "utls": { "enabled": true, "fingerprint": "chrome" }
      }
    },
    { "type": "direct", "tag": "direct" },
    { "type": "direct", "tag": "bypass" }
  ],
  "route": {
    "rules": [
      { "protocol": "dns", "action": "hijack-dns" },
      { "network": "udp", "port": [135, 137, 138, 139, 5353], "action": "reject" }
    ],
    "default_domain_resolver": { "server": "dns-direct" },
    "final": "anytls-out"
    }
}
'@ | Set-Content -LiteralPath $checkConfig -Encoding ASCII
    try {
        Invoke-Checked $CoreExe @("check", "-c", $checkConfig) "nekobox_core check"
    } finally {
        Remove-Item -LiteralPath $checkConfig -Force -ErrorAction SilentlyContinue
    }
    $RouteFluentTestData = Join-Path $Root "third_party\routefluent-sing-box\testdata"
    foreach ($validFixture in @(
        "anytls-client-check.json",
        "routefluent-dns-resolver-group-check.json",
        "routefluent-dns-doh-bootstrap-check.json"
    )) {
        Invoke-Checked $CoreExe @("check", "-c", (Join-Path $RouteFluentTestData $validFixture)) "RouteFluent valid fixture $validFixture"
    }
    foreach ($invalidFixture in @(
        "routefluent-dns-invalid-primary-local.json",
        "routefluent-dns-invalid-fallback-https.json",
        "routefluent-dns-invalid-fallback-missing-probes.json"
    )) {
        Invoke-ExpectedFailure $CoreExe @("check", "-c", (Join-Path $RouteFluentTestData $invalidFixture)) "RouteFluent invalid fixture $invalidFixture"
    }

    if ($SkipGuiBuild -or $SkipGoBuild) {
        Write-Warning (
            "SkipGuiBuild/SkipGoBuild are diagnostic-only. " +
            "The package directory was validated, but no formal archive will be created or overwritten."
        )
        if (Test-Path -LiteralPath $ZipPath) {
            Write-Warning "Existing formal archive was left untouched: $ZipPath"
        }
        if ($packageConfigBackupOwned) {
            Restore-PackageConfig $PackageConfigDir $PackageConfigBackupDir $PackageDir
            $packageConfigBackupOwned = $false
            Remove-SafeDirectory $PackageConfigBackupDir $DeployRoot
        }
        Write-Step "Diagnostic package directory ready; formal archive skipped"
        Write-Host "Folder: $PackageDir"
        return
    }

    Write-Step "Create formal zip with nekoray root folder"
    Assert-DirectoryTreeHasNoReparsePoints $PackageDir "Windows package archive source"
    New-Item -ItemType Directory -Force -Path $ZipStagePackage | Out-Null
    Copy-Item -Path (Join-Path $PackageDir "*") -Destination $ZipStagePackage -Recurse -Force
    if (Test-Path -LiteralPath $ZipPath) {
        Remove-Item -LiteralPath $ZipPath -Force
    }

    $SevenZip = Find-CommandPath "7z.exe"
    if ([string]::IsNullOrWhiteSpace($SevenZip)) {
        foreach ($candidate in @(
            "C:\Program Files\7-Zip\7z.exe",
            "C:\Program Files (x86)\7-Zip\7z.exe"
        )) {
            if (Test-Path -LiteralPath $candidate) {
                $SevenZip = $candidate
                break
            }
        }
    }
    if (![string]::IsNullOrWhiteSpace($SevenZip)) {
        $SevenZip = Assert-PathOutsideProtectedProduction $SevenZip "7-Zip executable"
        Push-Location $ZipStageRoot
        try {
            Invoke-Checked $SevenZip @("a", "-tzip", $ZipPath, ".\nekoray") "7z"
        } finally {
            Pop-Location
        }
    } else {
        Compress-Archive -LiteralPath $ZipStagePackage -DestinationPath $ZipPath -CompressionLevel Optimal
    }

    Remove-SafeDirectory $ZipStageRoot $DeployRoot
    if ($packageConfigBackupOwned) {
        Restore-PackageConfig $PackageConfigDir $PackageConfigBackupDir $PackageDir
        $packageConfigBackupOwned = $false
        Remove-SafeDirectory $PackageConfigBackupDir $DeployRoot
    }

    Write-Step "Done"
    Write-Host "Folder: $PackageDir"
    Write-Host "Zip:    $ZipPath"
} finally {
    if ($packageConfigBackupOwned) {
        if (!$packageReplacementStarted) {
            $packageConfigBackupOwned = $false
            try {
                Remove-SafeDirectory $PackageConfigBackupDir $DeployRoot
            } catch {
                Write-Warning "Could not remove this run's unused package config backup: $($_.Exception.Message)"
            }
        } else {
            try {
                Restore-PackageConfig $PackageConfigDir $PackageConfigBackupDir $PackageDir
                $packageConfigBackupOwned = $false
                Remove-SafeDirectory $PackageConfigBackupDir $DeployRoot
            } catch {
                Write-Warning "Could not restore preserved package config: $($_.Exception.Message)"
            }
        }
    }
    if ($packageBinaryBackupOwned) {
        if (!$packageReplacementStarted) {
            $packageBinaryBackupOwned = $false
            try {
                Remove-SafeDirectory $PackageBinaryBackupDir $DeployRoot
            } catch {
                Write-Warning "Could not remove this run's unused package binary backup: $($_.Exception.Message)"
            }
        } else {
            try {
                Restore-PackageBinaries $PackageDir $PackageBinaryBackupDir
                $packageBinaryBackupOwned = $false
                Remove-SafeDirectory $PackageBinaryBackupDir $DeployRoot
            } catch {
                Write-Warning "Could not restore preserved Go binaries: $($_.Exception.Message)"
            }
        }
    }
    if (Get-Variable -Name oldPath -Scope Local -ErrorAction SilentlyContinue) { $env:PATH = $oldPath }
    if (Get-Variable -Name oldGoos -Scope Local -ErrorAction SilentlyContinue) { $env:GOOS = $oldGoos }
    if (Get-Variable -Name oldGoarch -Scope Local -ErrorAction SilentlyContinue) { $env:GOARCH = $oldGoarch }
    if (Get-Variable -Name oldCgo -Scope Local -ErrorAction SilentlyContinue) { $env:CGO_ENABLED = $oldCgo }
    if (Get-Variable -Name oldQtPluginPath -Scope Local -ErrorAction SilentlyContinue) { $env:QT_PLUGIN_PATH = $oldQtPluginPath }
    if (Get-Variable -Name oldQtQpaPlatformPluginPath -Scope Local -ErrorAction SilentlyContinue) { $env:QT_QPA_PLATFORM_PLUGIN_PATH = $oldQtQpaPlatformPluginPath }
    if (Get-Variable -Name oldQml2ImportPath -Scope Local -ErrorAction SilentlyContinue) { $env:QML2_IMPORT_PATH = $oldQml2ImportPath }
    Pop-Location
}
