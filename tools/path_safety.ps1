if (-not ('RouteFluentPathSafetyNative' -as [type])) {
    Add-Type -TypeDefinition @'
using System.Runtime.InteropServices;
using System.Text;

public static class RouteFluentPathSafetyNative {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern uint QueryDosDevice(
        string deviceName,
        StringBuilder targetPath,
        int maximumLength);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    public static extern uint GetDriveType(string rootPathName);
}
'@
}

function Get-LocalDosDeviceTarget {
    param(
        [Parameter(Mandatory = $true)]
        [string] $Drive,
        [switch] $AllowMissing
    )

    $target = [Text.StringBuilder]::new(32768)
    $length = [RouteFluentPathSafetyNative]::QueryDosDevice(
        $Drive,
        $target,
        $target.Capacity)
    if ($length -eq 0) {
        if ($AllowMissing) { return "" }
        $errorCode = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
        throw "Unable to resolve DOS device $Drive (Win32 error $errorCode)."
    }
    return $target.ToString()
}

function Test-PathInsideRoot {
    param([string] $Candidate, [string] $Root)
    $rootValue = $Root.TrimEnd('\', '/')
    return $Candidate.Equals($rootValue, [StringComparison]::OrdinalIgnoreCase) -or
        $Candidate.StartsWith(
            $rootValue + [IO.Path]::DirectorySeparatorChar,
            [StringComparison]::OrdinalIgnoreCase)
}

function Assert-PathOutsideProtectedProduction {
    [CmdletBinding()]
    [OutputType([string])]
    param(
        [Parameter(Mandatory = $true)]
        [string] $Path,
        [string] $Purpose = "path"
    )

    if ([string]::IsNullOrWhiteSpace($Path)) {
        throw "$Purpose must not be empty."
    }
    $rawPathWithoutDrivePrefix = if ($Path -match '^[A-Za-z]:') {
        $Path.Substring(2)
    } else {
        $Path
    }
    if ($rawPathWithoutDrivePrefix.Contains(':')) {
        throw "$Purpose rejects alternate data stream/path namespace syntax: $Path"
    }

    $candidate = [IO.Path]::GetFullPath($Path)
    $candidateRoot = [IO.Path]::GetPathRoot($candidate)
    if ($candidate.Length -gt $candidateRoot.Length) {
        $candidate = $candidate.TrimEnd('\', '/')
    }
    $protected = [IO.Path]::GetFullPath('D:\Program Files\nekoray').TrimEnd('\', '/')
    if (Test-PathInsideRoot $candidate $protected) {
        throw "$Purpose must never target the protected production NekoRay installation: $candidate"
    }

    $pathRoot = [IO.Path]::GetPathRoot($candidate)
    if ([string]::IsNullOrWhiteSpace($pathRoot) -or
        $pathRoot -notmatch '^[A-Za-z]:\\$') {
        throw "$Purpose requires a local drive-letter path; UNC/device paths are forbidden: $candidate"
    }
    if ([RouteFluentPathSafetyNative]::GetDriveType($pathRoot) -ne 3) {
        throw "$Purpose requires a fixed local drive; mapped/network/removable roots are forbidden: $pathRoot"
    }

    $candidateDrive = $pathRoot.Substring(0, 2)
    $candidateDevice = Get-LocalDosDeviceTarget $candidateDrive
    if ($candidateDevice.StartsWith('\??\', [StringComparison]::OrdinalIgnoreCase)) {
        throw "$Purpose rejects SUBST/DOS-device aliases: $candidateDrive -> $candidateDevice"
    }
    if ($candidateDevice -match '^\\Device\\[^\\]+\\') {
        throw "$Purpose rejects DOS-device subpath aliases: $candidateDrive -> $candidateDevice"
    }
    $protectedDevice = Get-LocalDosDeviceTarget 'D:' -AllowMissing
    if (![string]::IsNullOrWhiteSpace($protectedDevice) -and
        $candidateDevice.Equals($protectedDevice, [StringComparison]::OrdinalIgnoreCase)) {
        $candidateOnProtectedVolume = [IO.Path]::GetFullPath(
            'D:\' + $candidate.Substring($pathRoot.Length))
        if (Test-PathInsideRoot $candidateOnProtectedVolume $protected) {
            throw "$Purpose resolves to the protected production NekoRay volume/path: $candidate"
        }
    }

    $current = $pathRoot
    foreach ($component in ($candidate.Substring($pathRoot.Length) -split '[\\/]')) {
        if ([string]::IsNullOrWhiteSpace($component)) { continue }
        if ($component.Contains(':')) {
            throw "$Purpose rejects alternate data stream/path namespace components: $candidate"
        }
        if ($component.EndsWith(' ', [StringComparison]::Ordinal) -or
            $component.EndsWith('.', [StringComparison]::Ordinal)) {
            throw "$Purpose rejects Win32-normalized trailing space/dot components: $candidate"
        }
        if ($component.Contains('~')) {
            throw "$Purpose rejects ambiguous Windows short-name components: $candidate"
        }
        $parent = $current
        if (!(Test-Path -LiteralPath $parent -PathType Container)) { break }
        $matches = @(Get-ChildItem -LiteralPath $parent -Force -ErrorAction Stop |
            Where-Object { $_.Name.Equals($component, [StringComparison]::OrdinalIgnoreCase) })
        if ($matches.Count -eq 0) { break }
        if ($matches.Count -ne 1) {
            throw "$Purpose rejects ambiguous case-sensitive directory entries: $candidate"
        }
        $item = $matches[0]
        $current = $item.FullName
        $canonicalItemPath = [IO.Path]::GetFullPath($item.FullName)
        if (Test-PathInsideRoot $canonicalItemPath $protected) {
            throw "$Purpose resolves through an existing component to the protected production NekoRay installation: $candidate"
        }
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "$Purpose rejects reparse/junction components: $current"
        }
    }

    return $candidate
}

function Assert-NewFileOutsideProtectedProduction {
    [CmdletBinding()]
    [OutputType([string])]
    param(
        [Parameter(Mandatory = $true)]
        [string] $Path,
        [string] $Purpose = "output file"
    )

    $candidate = Assert-PathOutsideProtectedProduction $Path $Purpose
    if (Test-Path -LiteralPath $candidate) {
        throw "$Purpose refuses to overwrite an existing path: $candidate"
    }
    return $candidate
}

function Assert-DirectoryTreeHasNoReparsePoints {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [string] $Path,
        [string] $Purpose = "directory tree"
    )

    $safeRoot = Assert-PathOutsideProtectedProduction ([IO.Path]::GetFullPath($Path)) $Purpose
    if (!(Test-Path -LiteralPath $safeRoot -PathType Container)) {
        return
    }

    $pending = [Collections.Generic.Stack[string]]::new()
    $pending.Push($safeRoot)
    while ($pending.Count -gt 0) {
        $currentDirectory = $pending.Pop()
        foreach ($item in Get-ChildItem -LiteralPath $currentDirectory -Force) {
            if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "$Purpose rejects reparse/junction descendants: $($item.FullName)"
            }
            if ($item.PSIsContainer) {
                $pending.Push($item.FullName)
            }
        }
    }
}
