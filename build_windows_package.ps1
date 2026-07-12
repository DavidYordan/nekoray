param(
    [string] $BuildDir = "build-package-windows64",
    [string] $QtDir = "",
    [string] $MingwDir = "",
    [string] $DepsDir = "",
    [string] $ReferenceDir = "D:\Program Files\nekoray",
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
    $full = [System.IO.Path]::GetFullPath($Path)
    $allowed = [System.IO.Path]::GetFullPath($AllowedRoot).TrimEnd('\')
    if (!$full.StartsWith($allowed + '\', [System.StringComparison]::OrdinalIgnoreCase)) {
        Fail "Refusing to remove directory outside allowed root: $full"
    }
    if (Test-Path -LiteralPath $full) {
        Remove-Item -LiteralPath $full -Recurse -Force
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

    $target = Join-Path $TargetDir $Name
    if (!$Refresh -and (Test-Path -LiteralPath $target) -and ((Get-Item -LiteralPath $target).Length -gt 0)) {
        return
    }

    $tmp = "$target.download"
    if (Test-Path -LiteralPath $tmp) {
        Remove-Item -LiteralPath $tmp -Force
    }

    try {
        Write-Host "Downloading $Name"
        Invoke-WebRequest -Uri $Url -OutFile $tmp -UseBasicParsing
        if (!(Test-Path -LiteralPath $tmp) -or ((Get-Item -LiteralPath $tmp).Length -le 0)) {
            Fail "Downloaded file is empty: $Name"
        }
        Move-Item -LiteralPath $tmp -Destination $target -Force
        return
    } catch {
        if (Test-Path -LiteralPath $tmp) {
            Remove-Item -LiteralPath $tmp -Force
        }
        $fallback = Join-Path $FallbackDir $Name
        if (![string]::IsNullOrWhiteSpace($FallbackDir) -and (Test-Path -LiteralPath $fallback)) {
            Write-Warning "Download failed for $Name, using reference copy: $fallback"
            Copy-Item -LiteralPath $fallback -Destination $target -Force
            return
        }
        throw
    }
}

function Copy-IfExists([string] $Source, [string] $DestinationDir) {
    if (Test-Path -LiteralPath $Source -PathType Leaf) {
        Copy-Item -LiteralPath $Source -Destination $DestinationDir -Force
    }
}

function Require-PackageFile([string] $RelativePath) {
    $path = Join-Path $PackageDir $RelativePath
    if (!(Test-Path -LiteralPath $path -PathType Leaf) -or ((Get-Item -LiteralPath $path).Length -le 0)) {
        Fail "Package is missing required file: $RelativePath"
    }
}

Push-Location $Root
try {
    Write-Step "Resolve local toolchain"

    $QtDir = Find-QtDir $QtDir
    $MingwDir = Find-MingwDir $MingwDir
    $QtBin = Join-Path $QtDir "bin"
    $MingwBin = Join-Path $MingwDir "bin"
    $DepsDir = if ([string]::IsNullOrWhiteSpace($DepsDir)) {
        Join-Path $Root "libs\deps\built"
    } else {
        Get-FullPath $DepsDir
    }
    Require-Directory $DepsDir "CMake dependency prefix" | Out-Null

    $CMake = Find-CommandPath "cmake.exe"
    if ([string]::IsNullOrWhiteSpace($CMake)) {
        Fail "cmake.exe was not found."
    }
    $Go = Find-CommandPath "go.exe"
    if ([string]::IsNullOrWhiteSpace($Go)) {
        Fail "go.exe was not found."
    }

    $Ninja = Find-CommandPath "ninja.exe"
    if (![string]::IsNullOrWhiteSpace($Ninja)) {
        $Generator = "Ninja"
        $MakeProgram = $Ninja
    } else {
        $Generator = "MinGW Makefiles"
        $MakeProgram = Require-File (Join-Path $MingwBin "mingw32-make.exe") "mingw32-make.exe"
    }

    $Version = (Get-Content -LiteralPath (Join-Path $Root "nekoray_version.txt") -Raw).Trim()
    $VersionStandalone = "nekoray-$Version"
    $BuildDirFull = Get-FullPath $BuildDir
    $DeployRoot = Join-Path $Root "deployment"
    $PackageDir = Join-Path $DeployRoot "windows64"
    $PublicResDir = Join-Path $DeployRoot "public_res"
    $ZipStageRoot = Join-Path $DeployRoot "zip-stage"
    $ZipStagePackage = Join-Path $ZipStageRoot "nekoray"
    $ZipPath = Join-Path $DeployRoot "$VersionStandalone-windows64.zip"

    Write-Host "Root:       $Root"
    Write-Host "Qt:         $QtDir"
    Write-Host "MinGW:      $MingwDir"
    Write-Host "Deps:       $DepsDir"
    Write-Host "Generator:  $Generator"
    Write-Host "Build dir:  $BuildDirFull"
    Write-Host "Package:    $PackageDir"

    $oldPath = $env:PATH
    $oldGoos = $env:GOOS
    $oldGoarch = $env:GOARCH
    $oldCgo = $env:CGO_ENABLED
    $oldQtPluginPath = $env:QT_PLUGIN_PATH
    $oldQtQpaPlatformPluginPath = $env:QT_QPA_PLATFORM_PLUGIN_PATH
    $oldQml2ImportPath = $env:QML2_IMPORT_PATH
    $env:PATH = "$MingwBin;$QtBin;$(Join-Path $DepsDir 'bin');$oldPath"
    $env:QT_PLUGIN_PATH = Join-Path $QtDir "plugins"
    $env:QT_QPA_PLATFORM_PLUGIN_PATH = Join-Path $QtDir "plugins\platforms"
    $env:QML2_IMPORT_PATH = Join-Path $QtDir "qml"
    $env:GOOS = "windows"
    $env:GOARCH = "amd64"
    $env:CGO_ENABLED = "0"

    New-Item -ItemType Directory -Force -Path $DeployRoot | Out-Null
    Remove-SafeDirectory $PackageDir $DeployRoot
    Remove-SafeDirectory $ZipStageRoot $DeployRoot
    New-Item -ItemType Directory -Force -Path $PackageDir | Out-Null

    if (!$SkipGuiBuild) {
        Write-Step "Configure and build GUI"
        New-Item -ItemType Directory -Force -Path $BuildDirFull | Out-Null
        $cmakeArgs = @(
            "-S", $Root,
            "-B", $BuildDirFull,
            "-G", $Generator,
            "-Wno-dev",
            "-DQT_VERSION_MAJOR=6",
            "-DCMAKE_BUILD_TYPE=Release",
            "-DCMAKE_POLICY_VERSION_MINIMUM=3.5",
            "-DCMAKE_PREFIX_PATH=$QtDir",
            "-DNKR_LIBS=$DepsDir",
            "-DCMAKE_C_COMPILER=$(Join-Path $MingwBin 'gcc.exe')",
            "-DCMAKE_CXX_COMPILER=$(Join-Path $MingwBin 'g++.exe')",
            "-DCMAKE_MAKE_PROGRAM=$MakeProgram"
        )
        Invoke-Checked $CMake $cmakeArgs "CMake configure"
        Invoke-Checked $CMake @("--build", $BuildDirFull, "--config", "Release", "--parallel", "$Parallel") "CMake build"
    } else {
        Write-Step "Skip GUI build"
    }

    $GuiExe = Require-File (Join-Path $BuildDirFull "nekobox.exe") "nekobox.exe"
    Copy-Item -LiteralPath $GuiExe -Destination (Join-Path $PackageDir "nekobox.exe") -Force

    Write-Step "Deploy Qt runtime"
    Invoke-Checked (Join-Path $QtBin "windeployqt.exe") @(
        (Join-Path $PackageDir "nekobox.exe"),
        "--force",
        "--no-compiler-runtime",
        "--no-system-d3d-compiler",
        "--no-opengl-sw",
        "--no-translations",
        "--qtpaths", (Join-Path $QtBin "qtpaths.exe"),
        "--verbose", "1"
    ) "windeployqt"

    foreach ($unneeded in @("translations", "d3dcompiler_47.dll", "opengl32sw.dll", "libEGL.dll", "libGLESv2.dll", "Qt6Pdf.dll")) {
        $path = Join-Path $PackageDir $unneeded
        if (Test-Path -LiteralPath $path) {
            Remove-Item -LiteralPath $path -Recurse -Force
        }
    }

    Write-Step "Copy compiler runtime and public assets"
    foreach ($runtimeDll in @("libgcc_s_seh-1.dll", "libstdc++-6.dll", "libwinpthread-1.dll")) {
        Copy-Item -LiteralPath (Require-File (Join-Path $MingwBin $runtimeDll) $runtimeDll) -Destination $PackageDir -Force
    }
    foreach ($opensslDll in @("libcrypto-3-x64.dll", "libssl-3-x64.dll", "libcrypto-1_1-x64.dll", "libssl-1_1-x64.dll")) {
        Copy-IfExists (Join-Path $QtBin $opensslDll) $PackageDir
        Copy-IfExists (Join-Path $MingwBin $opensslDll) $PackageDir
    }

    New-Item -ItemType Directory -Force -Path $PublicResDir | Out-Null
    Copy-Item -Path (Join-Path $Root "res\public\*") -Destination $PublicResDir -Force
    $fallback = if (Test-Path -LiteralPath $ReferenceDir -PathType Container) { $ReferenceDir } else { "" }
    Download-Or-Fallback "geoip.dat" "https://github.com/Loyalsoldier/v2ray-rules-dat/releases/latest/download/geoip.dat" $PublicResDir $fallback -Refresh:$RefreshGeodata
    Download-Or-Fallback "geosite.dat" "https://github.com/v2fly/domain-list-community/releases/latest/download/dlc.dat" $PublicResDir $fallback -Refresh:$RefreshGeodata
    Download-Or-Fallback "geoip.db" "https://github.com/SagerNet/sing-geoip/releases/latest/download/geoip.db" $PublicResDir $fallback -Refresh:$RefreshGeodata
    Download-Or-Fallback "geosite.db" "https://github.com/SagerNet/sing-geosite/releases/latest/download/geosite.db" $PublicResDir $fallback -Refresh:$RefreshGeodata
    Copy-Item -Path (Join-Path $PublicResDir "*") -Destination $PackageDir -Force

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
            $ldflags = "-w -s -X github.com/matsuridayo/libneko/neko_common.Version_neko=$VersionStandalone"
            $tags = "with_clash_api,with_gvisor,with_quic,with_wireguard,with_utls"
            Invoke-Checked $Go @("build", "-v", "-o", (Join-Path $PackageDir "nekobox_core.exe"), "-trimpath", "-ldflags", $ldflags, "-tags", $tags) "go build nekobox_core"
        } finally {
            Pop-Location
        }
    } else {
        Write-Step "Skip Go build"
        Require-File (Join-Path $PackageDir "updater.exe") "updater.exe" | Out-Null
        Require-File (Join-Path $PackageDir "nekobox_core.exe") "nekobox_core.exe" | Out-Null
    }

    Write-Step "Validate package contents"
    foreach ($required in @(
        "nekobox.exe",
        "nekobox_core.exe",
        "updater.exe",
        "nekobox.png",
        "qtbase_zh_CN.qm",
        "geoip.dat",
        "geosite.dat",
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

    foreach ($forbidden in @("nekoray.exe", "nekoray_core.exe", "nekoray.png", "d3dcompiler_47.dll", "opengl32sw.dll")) {
        if (Test-Path -LiteralPath (Join-Path $PackageDir $forbidden)) {
            Fail "Package contains forbidden legacy or optional file: $forbidden"
        }
    }
    if (Test-Path -LiteralPath (Join-Path $PackageDir "config")) {
        Fail "Package must not include user config directory."
    }

    Write-Step "Smoke-check nekobox_core"
    Invoke-Checked (Join-Path $PackageDir "nekobox_core.exe") @("version") "nekobox_core version"
    $checkConfig = Join-Path ([System.IO.Path]::GetTempPath()) "nekoray-anytls-package-check.json"
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
        Invoke-Checked (Join-Path $PackageDir "nekobox_core.exe") @("check", "-c", $checkConfig) "nekobox_core check"
    } finally {
        Remove-Item -LiteralPath $checkConfig -Force -ErrorAction SilentlyContinue
    }

    Write-Step "Create formal zip with nekoray root folder"
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

    Write-Step "Done"
    Write-Host "Folder: $PackageDir"
    Write-Host "Zip:    $ZipPath"
} finally {
    if (Get-Variable -Name oldPath -Scope Local -ErrorAction SilentlyContinue) { $env:PATH = $oldPath }
    if (Get-Variable -Name oldGoos -Scope Local -ErrorAction SilentlyContinue) { $env:GOOS = $oldGoos }
    if (Get-Variable -Name oldGoarch -Scope Local -ErrorAction SilentlyContinue) { $env:GOARCH = $oldGoarch }
    if (Get-Variable -Name oldCgo -Scope Local -ErrorAction SilentlyContinue) { $env:CGO_ENABLED = $oldCgo }
    if (Get-Variable -Name oldQtPluginPath -Scope Local -ErrorAction SilentlyContinue) { $env:QT_PLUGIN_PATH = $oldQtPluginPath }
    if (Get-Variable -Name oldQtQpaPlatformPluginPath -Scope Local -ErrorAction SilentlyContinue) { $env:QT_QPA_PLATFORM_PLUGIN_PATH = $oldQtQpaPlatformPluginPath }
    if (Get-Variable -Name oldQml2ImportPath -Scope Local -ErrorAction SilentlyContinue) { $env:QML2_IMPORT_PATH = $oldQml2ImportPath }
    Pop-Location
}
