[CmdletBinding()]
param(
    [string] $InputRoot = "C:\NekoRayInput",
    [string] $ResultsRoot = "C:\NekoRayResults"
)

$ErrorActionPreference = "Stop"
$utf8NoBom = [Text.UTF8Encoding]::new($false)

function Get-Manifest {
    param([Parameter(Mandatory = $true)] [string] $Root)

    $entries = foreach ($file in Get-ChildItem -LiteralPath $Root -File -Recurse -Force |
        Where-Object { $_.Name -ne "expected-input-manifest.json" }) {
        [ordered]@{
            path = $file.FullName.Substring($Root.Length).TrimStart('\', '/').Replace('\', '/')
            length = $file.Length
            sha256 = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    }
    return [ordered]@{
        schema = "nekoray.windows_sandbox_input.v1"
        entries = @($entries | Sort-Object path)
    }
}

function Invoke-ExactProcess {
    param(
        [Parameter(Mandatory = $true)] [string] $FilePath,
        [string] $Arguments = "",
        [Parameter(Mandatory = $true)] [string] $WorkingDirectory,
        [int] $TimeoutMilliseconds = 30000
    )

    $startInfo = [Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $FilePath
    $startInfo.Arguments = $Arguments
    $startInfo.WorkingDirectory = $WorkingDirectory
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    if (!$process.Start()) {
        throw "Unable to start sandbox-owned process: $FilePath"
    }

    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()
    $timedOut = !$process.WaitForExit($TimeoutMilliseconds)
    if ($timedOut) {
        $process.Kill()
    }
    $process.WaitForExit()
    $stdout = $stdoutTask.Result
    $stderr = $stderrTask.Result
    $exitCode = if ($timedOut) { $null } else { $process.ExitCode }
    $process.Dispose()

    return [ordered]@{
        exit_code = $exitCode
        timed_out = $timedOut
        stdout = if ($stdout.Length -gt 16384) { $stdout.Substring(0, 16384) } else { $stdout }
        stderr = if ($stderr.Length -gt 16384) { $stderr.Substring(0, 16384) } else { $stderr }
    }
}

if (!(Test-Path -LiteralPath $InputRoot -PathType Container)) {
    throw "Sandbox input mapping is missing: $InputRoot"
}
if (!(Test-Path -LiteralPath $ResultsRoot -PathType Container)) {
    throw "Sandbox results mapping is missing: $ResultsRoot"
}

$summaryPath = Join-Path $ResultsRoot "sandbox-summary.json"
$actualManifestPath = Join-Path $ResultsRoot "actual-input-manifest.json"
$completionPath = Join-Path $ResultsRoot "completed.marker"
foreach ($output in @($summaryPath, $actualManifestPath, $completionPath)) {
    if (Test-Path -LiteralPath $output) {
        throw "Sandbox runner refuses to overwrite an existing result: $output"
    }
}

$startedUtc = [DateTime]::UtcNow
$summary = [ordered]@{
    schema = "nekoray.windows_sandbox_result.v1"
    started_utc = $startedUtc.ToString("o")
    completed_utc = $null
    success = $false
    networking_contract = "disabled_by_wsb"
    up_network_adapter_count = $null
    network_adapters = @()
    os = $null
    elevated = $false
    package_present = $false
    core_version = $null
    gui_transaction_report = $null
    error = $null
}

try {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    $summary.elevated = $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

    $os = Get-CimInstance Win32_OperatingSystem
    $summary.os = [ordered]@{
        caption = $os.Caption
        version = $os.Version
        build_number = $os.BuildNumber
        architecture = $os.OSArchitecture
    }

    $adapters = @()
    try {
        $adapters = @(Get-NetAdapter -IncludeHidden -ErrorAction Stop | Select-Object Name, InterfaceDescription, Status)
    } catch {
        $summary.error = "Unable to inventory sandbox network adapters: $($_.Exception.Message)"
        throw
    }
    $summary.network_adapters = @($adapters | ForEach-Object {
        [ordered]@{
            name = $_.Name
            description = $_.InterfaceDescription
            status = $_.Status.ToString()
        }
    })
    $summary.up_network_adapter_count = @($adapters | Where-Object { $_.Status -eq "Up" }).Count

    $actualManifest = Get-Manifest $InputRoot
    [IO.File]::WriteAllText(
        $actualManifestPath,
        ($actualManifest | ConvertTo-Json -Depth 6),
        $utf8NoBom)

    $packageRoot = Join-Path $InputRoot "package"
    $summary.package_present = Test-Path -LiteralPath $packageRoot -PathType Container
    if ($summary.package_present) {
        $workRoot = "C:\NekoRayWork"
        $workPackage = Join-Path $workRoot "package"
        $appData = Join-Path $workRoot "appdata"
        New-Item -ItemType Directory -Path $workPackage -Force | Out-Null
        New-Item -ItemType Directory -Path $appData -Force | Out-Null
        foreach ($item in Get-ChildItem -LiteralPath $packageRoot -Force) {
            Copy-Item -LiteralPath $item.FullName -Destination $workPackage -Recurse -Force
        }

        $corePath = Join-Path $workPackage "nekobox_core.exe"
        $guiPath = Join-Path $workPackage "nekobox.exe"
        $summary.core_version = Invoke-ExactProcess $corePath "version" $workPackage
        $summary.gui_transaction_report = Invoke-ExactProcess `
            $guiPath "-appdata C:\NekoRayWork\appdata -flag_config_transaction_report" $workPackage
    }

    $processChecksPassed = !$summary.package_present -or
        (!$summary.core_version.timed_out -and $summary.core_version.exit_code -eq 0 -and
         !$summary.gui_transaction_report.timed_out -and $summary.gui_transaction_report.exit_code -eq 0)
    $summary.success = $summary.up_network_adapter_count -eq 0 -and $processChecksPassed
} catch {
    if ([string]::IsNullOrWhiteSpace($summary.error)) {
        $summary.error = $_.Exception.Message
    }
} finally {
    $summary.completed_utc = [DateTime]::UtcNow.ToString("o")
    [IO.File]::WriteAllText($summaryPath, ($summary | ConvertTo-Json -Depth 8), $utf8NoBom)
    [IO.File]::WriteAllText($completionPath, "completed`r`n", $utf8NoBom)
}
