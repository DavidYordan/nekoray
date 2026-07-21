param(
    [switch] $Json
)

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$failures = [System.Collections.Generic.List[string]]::new()

function Add-Failure([string] $Message) {
    $script:failures.Add($Message)
}

function Invoke-Git([string[]] $Arguments) {
    $previousErrorActionPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $output = @(& git -C $repoRoot @Arguments 2>&1)
        $exitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousErrorActionPreference
    }
    if ($exitCode -ne 0) {
        throw "git $($Arguments -join ' ') failed: $($output -join [Environment]::NewLine)"
    }
    return $output
}

$tracked = @(Invoke-Git @("ls-files"))
$forbiddenTracked = @(
    $tracked | Where-Object {
        $path = $_ -replace "\\", "/"
        $path -match '(^|/)(remote\.env|\.env(?:\.|$))' -or
        ($path -match '(^|/)(config|groups|routes_box|recovery)/') -or
        ($path -match '\.(pem|key|pfx|p12|jks|keystore|exe|dll|dmp|zip|7z|log)$') -or
        ($path -match '^(build(?:-|/)|deployment/|qtsdk/|artifacts/|dist/|packages/|release/|test-results/)')
    }
)
$forbiddenTracked = @($forbiddenTracked | Where-Object { $_ -ne ".env.example" })
foreach ($path in $forbiddenTracked) {
    Add-Failure "tracked sensitive/generated path: $path"
}

$requiredIgnorePatterns = @(
    "remote.env",
    "*.pem",
    "*.key",
    "/config/",
    "/recovery/",
    "__pycache__/",
    "/build-*",
    "/deployment",
    "/qtsdk"
)
$gitignore = [IO.File]::ReadAllText((Join-Path $repoRoot ".gitignore"))
foreach ($pattern in $requiredIgnorePatterns) {
    if ($gitignore.IndexOf($pattern, [StringComparison]::Ordinal) -lt 0) {
        Add-Failure "missing required .gitignore pattern: $pattern"
    }
}

$workflowFiles = @(
    Get-ChildItem -LiteralPath (Join-Path $repoRoot ".github\workflows") -File -ErrorAction SilentlyContinue |
        Where-Object { $_.Extension -in @(".yml", ".yaml") }
)
$actionReferences = 0
foreach ($file in $workflowFiles) {
    $content = [IO.File]::ReadAllText($file.FullName)
    foreach ($match in [regex]::Matches($content, '(?m)^\s*uses:\s*([^#\s]+)')) {
        $reference = $match.Groups[1].Value
        if ($reference.StartsWith("./") -or $reference.StartsWith("docker://")) { continue }
        $actionReferences++
        $separator = $reference.LastIndexOf('@')
        $selector = if ($separator -ge 0) { $reference.Substring($separator + 1) } else { "" }
        if ($selector -notmatch '^[0-9a-fA-F]{40}$') {
            Add-Failure "GitHub Action is not pinned to a full commit SHA in $($file.FullName): $reference"
        }
    }
}

$powershellFiles = @(
    Get-ChildItem -LiteralPath $repoRoot -File -Filter *.ps1
    Get-ChildItem -LiteralPath (Join-Path $repoRoot "test") -Recurse -File -Filter *.ps1 -ErrorAction SilentlyContinue
    Get-ChildItem -LiteralPath (Join-Path $repoRoot "tools") -Recurse -File -Filter *.ps1 -ErrorAction SilentlyContinue
)
foreach ($file in $powershellFiles) {
    $tokens = $null
    $errors = $null
    [Management.Automation.Language.Parser]::ParseFile($file.FullName, [ref] $tokens, [ref] $errors) | Out-Null
    foreach ($parseError in $errors) {
        Add-Failure "$($file.FullName):$($parseError.Extent.StartLineNumber): $($parseError.Message)"
    }
}

$jsonFixtures = @(
    Get-ChildItem -LiteralPath (Join-Path $repoRoot "test\fixtures") -File -Filter *.json -ErrorAction SilentlyContinue
)
foreach ($file in $jsonFixtures) {
    try {
        [IO.File]::ReadAllText($file.FullName) | ConvertFrom-Json -ErrorAction Stop | Out-Null
    } catch {
        Add-Failure "invalid JSON fixture $($file.FullName): $($_.Exception.Message)"
    }
}

$markdownFiles = @((Get-Item (Join-Path $repoRoot "README.md"))) + @(
    Get-ChildItem -LiteralPath (Join-Path $repoRoot "docs") -Recurse -File -Filter *.md
)
$linkCount = 0
foreach ($file in $markdownFiles) {
    $text = [IO.File]::ReadAllText($file.FullName)
    foreach ($match in [regex]::Matches($text, '!?(?<!\\)\[[^\]]*\]\(([^)]+)\)')) {
        $target = $match.Groups[1].Value.Trim()
        if ($target.StartsWith("<") -and $target.EndsWith(">")) {
            $target = $target.Substring(1, $target.Length - 2)
        }
        if ($target -match '^(https?://|mailto:|#|data:)') { continue }
        $target = ($target -split '#', 2)[0]
        if ([string]::IsNullOrWhiteSpace($target)) { continue }
        $linkCount++
        $decoded = [Uri]::UnescapeDataString($target)
        $candidate = if ([IO.Path]::IsPathRooted($decoded)) {
            $decoded
        } else {
            Join-Path $file.DirectoryName $decoded
        }
        if (-not (Test-Path -LiteralPath $candidate)) {
            Add-Failure "missing local Markdown link in $($file.FullName): $target"
        }
    }
}

$moduleFiles = @(
    Join-Path $repoRoot "go\cmd\nekobox_core\go.mod"
    Join-Path $repoRoot "go\grpc_server\go.mod"
)
foreach ($file in $moduleFiles) {
    $content = [IO.File]::ReadAllText($file) -replace "\\", "/"
    if ($content -notmatch 'third_party/libneko') {
        Add-Failure "Go module is not pinned to third_party/libneko: $file"
    }
    if ($content -match '(?m)=>\s+(?:\.\./){3,}libneko\s*$') {
        Add-Failure "Go module still references repository-external libneko: $file"
    }
}

$submoduleStatus = @(Invoke-Git @("submodule", "status", "--recursive"))
foreach ($line in $submoduleStatus) {
    if ($line -match '^[-+U]') {
        Add-Failure "submodule is missing, conflicted, or at an unrecorded commit: $line"
    }
}

$previousErrorActionPreference = $ErrorActionPreference
$ErrorActionPreference = "Continue"
try {
    $diffCheck = @(& git -C $repoRoot diff --check HEAD 2>&1)
    $diffCheckExitCode = $LASTEXITCODE
} finally {
    $ErrorActionPreference = $previousErrorActionPreference
}
if ($diffCheckExitCode -ne 0) {
    foreach ($line in $diffCheck) { Add-Failure "git diff --check: $line" }
}

$result = [ordered]@{
    passed = $failures.Count -eq 0
    tracked_files = $tracked.Count
    powershell_files = $powershellFiles.Count
    json_fixtures = $jsonFixtures.Count
    markdown_files = $markdownFiles.Count
    local_markdown_links = $linkCount
    workflow_action_references = $actionReferences
    submodules = $submoduleStatus.Count
    failures = @($failures)
}

if ($Json) {
    $result | ConvertTo-Json -Depth 5
} else {
    $result.GetEnumerator() | Where-Object { $_.Key -ne "failures" } | ForEach-Object {
        Write-Host "$($_.Key): $($_.Value)"
    }
    foreach ($failure in $failures) { Write-Error $failure }
}

if (-not $result.passed) { exit 1 }
