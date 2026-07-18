param(
    [Parameter(Mandatory = $true)]
    [int] $ProfileId,
    [string] $PackageDir = "deployment/windows64",
    [string] $OutputPath = "",
    [switch] $ForTest,
    [switch] $ForShare,
    [switch] $Check,
    [switch] $Json
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$Root = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($Root)) {
    $Root = (Get-Location).Path
}

function Get-FullPath([string] $Path) {
    if ([System.IO.Path]::IsPathRooted($Path)) {
        return [System.IO.Path]::GetFullPath($Path)
    }
    return [System.IO.Path]::GetFullPath((Join-Path $Root $Path))
}

function Require-File([string] $Path, [string] $Name) {
    if (!(Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Name not found: $Path"
    }
    return (Resolve-Path -LiteralPath $Path).Path
}

$packageFull = Get-FullPath $PackageDir
$nekobox = Require-File (Join-Path $packageFull "nekobox.exe") "nekobox.exe"
$core = Require-File (Join-Path $packageFull "nekobox_core.exe") "nekobox_core.exe"

if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    $auditDir = Join-Path $packageFull "runtime_audit"
    New-Item -ItemType Directory -Path $auditDir -Force | Out-Null
    $stamp = Get-Date -Format "yyyyMMdd_HHmmss"
    $OutputPath = Join-Path $auditDir "profile_${ProfileId}_core_config_$stamp.json"
}
$outputFull = Get-FullPath $OutputPath
$outputDir = Split-Path -Parent $outputFull
New-Item -ItemType Directory -Path $outputDir -Force | Out-Null

$argsList = @("-flag_export_profile_config", [string] $ProfileId, $outputFull)
if ($ForTest) {
    $argsList += "-flag_export_profile_config_for_test"
}
if ($ForShare) {
    $argsList += "-flag_export_profile_config_for_share"
}

$exportProcess = Start-Process -FilePath $nekobox -ArgumentList $argsList -WorkingDirectory $packageFull -WindowStyle Hidden -Wait -PassThru
if ($exportProcess.ExitCode -ne 0) {
    throw "profile config export failed with exit code $($exportProcess.ExitCode)"
}

$checkExitCode = $null
if ($Check) {
    & $core check -c $outputFull
    $checkExitCode = $LASTEXITCODE
}

$config = Get-Content -Raw -Path $outputFull | ConvertFrom-Json
$outboundSummary = @()
foreach ($outbound in @($config.outbounds)) {
    $domainResolver = $null
    if ($outbound.PSObject.Properties.Name -contains "domain_resolver") {
        $domainResolver = $outbound.domain_resolver
    }
    $client = $null
    if ($outbound.PSObject.Properties.Name -contains "client") {
        $client = $outbound.client
    }
    $detour = $null
    if ($outbound.PSObject.Properties.Name -contains "detour") {
        $detour = $outbound.detour
    }
    $outboundSummary += [pscustomobject]@{
        tag = $outbound.tag
        type = $outbound.type
        client = $client
        detour = $detour
        domain_resolver = $domainResolver
    }
}

$dnsServers = @($config.dns.servers)
$routefluentResolverGroups = @($dnsServers | Where-Object { $_.type -eq "routefluent_resolver_group" })
$dohServers = @($dnsServers | Where-Object { $_.type -eq "https" -or $_.type -eq "h3" })

$result = [pscustomobject]@{
    profile_id = $ProfileId
    output_path = $outputFull
    check_exit_code = $checkExitCode
    inbound_count = @($config.inbounds).Count
    outbound_count = @($config.outbounds).Count
    dns_server_count = $dnsServers.Count
    routefluent_resolver_group_count = $routefluentResolverGroups.Count
    doh_server_count = $dohServers.Count
    outbounds = $outboundSummary
}

if ($Json) {
    $result | ConvertTo-Json -Depth 8
} else {
    $result | Format-List
    $outboundSummary | Format-Table -AutoSize
}
