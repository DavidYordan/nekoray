param(
    [string] $Protoc = ""
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$GeneratorDir = $PSScriptRoot
$RepositoryRoot = (Resolve-Path (Join-Path $GeneratorDir "..\..\..")).Path
. (Join-Path $RepositoryRoot "tools\path_safety.ps1")
$RepositoryRoot = Assert-PathOutsideProtectedProduction $RepositoryRoot "Repository root"
$SafeTempRoot = Assert-PathOutsideProtectedProduction ([IO.Path]::GetFullPath([IO.Path]::GetTempPath())) "Proto generator temporary directory"
$BundledProtoc = Join-Path $RepositoryRoot "libs\deps\built\bin\protoc.exe"
$BundledMingwBin = Join-Path $RepositoryRoot "qtsdk\tools\Tools\mingw1310_64\bin"
$ToolDir = Join-Path $SafeTempRoot "nekoray-proto-go-v1.28.1-grpc-v1.2.0"
$ToolDir = Assert-PathOutsideProtectedProduction $ToolDir "Pinned proto generator tool cache"
$GenerationStageRoot = Assert-PathOutsideProtectedProduction `
    (Join-Path $RepositoryRoot "build-proto-generation") `
    "Proto generation staging root"
$GoPlugin = Join-Path $ToolDir "protoc-gen-go.exe"
$GrpcPlugin = Join-Path $ToolDir "protoc-gen-go-grpc.exe"
$GeneratorDir = Assert-PathOutsideProtectedProduction $GeneratorDir "Proto source/generation directory"
Assert-DirectoryTreeHasNoReparsePoints $GeneratorDir "Proto source/generation tree"
Assert-DirectoryTreeHasNoReparsePoints $ToolDir "Pinned proto generator tool cache"
Assert-DirectoryTreeHasNoReparsePoints $GenerationStageRoot "Proto generation staging root"
$staleOutputTransactions = @(Get-ChildItem -LiteralPath $GeneratorDir -File -Force |
    Where-Object { $_.Name -match '^\.libcore(?:_grpc)?\.pb\.go\.[0-9a-f]{32}\.(?:new|bak)$' })
if ($staleOutputTransactions.Count -gt 0) {
    throw "Stale proto output transaction files require manual review: $($staleOutputTransactions.FullName -join ', ')"
}
if ((Test-Path -LiteralPath $GenerationStageRoot -PathType Container) -and
    @(Get-ChildItem -LiteralPath $GenerationStageRoot -Force).Count -gt 0) {
    throw "Stale proto generation staging data requires manual review: $GenerationStageRoot"
}

function Invoke-Checked {
    param([string] $FilePath, [string[]] $Arguments)
    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$FilePath failed with exit code $LASTEXITCODE"
    }
}

function Get-ToolVersionOrEmpty([string] $Path) {
    if (!(Test-Path -LiteralPath $Path -PathType Leaf)) {
        return ""
    }
    try {
        return (& $Path --version | Out-String).Trim()
    } catch {
        return ""
    }
}

function Remove-GeneratorFileForReplacement([string] $Path, [string] $Purpose) {
    $safePath = Assert-PathOutsideProtectedProduction ([IO.Path]::GetFullPath($Path)) $Purpose
    if (Test-Path -LiteralPath $safePath) {
        if (Test-Path -LiteralPath $safePath -PathType Container) {
            throw "$Purpose expected a file but found a directory: $safePath"
        }
        Remove-Item -LiteralPath $safePath -Force
    }
    return Assert-NewFileOutsideProtectedProduction $safePath $Purpose
}

function Get-SafeApplicationPath([string] $Name, [string] $Purpose) {
    $command = Get-Command -Name $Name -CommandType Application -ErrorAction Stop |
        Select-Object -First 1
    return Assert-PathOutsideProtectedProduction $command.Source $Purpose
}

if ([string]::IsNullOrWhiteSpace($Protoc)) {
    $Protoc = if (Test-Path -LiteralPath $BundledProtoc -PathType Leaf) {
        $BundledProtoc
    } else {
        "protoc"
    }
}
$Protoc = Get-SafeApplicationPath $Protoc "protobuf compiler executable"

$PreviousPath = $env:PATH
try {
    if (Test-Path -LiteralPath $BundledMingwBin -PathType Container) {
        # The repository protoc is a MinGW binary. A prepared Windows build
        # tree keeps its runtime DLLs here, but they need not be globally
        # installed or added to the user's persistent PATH.
        $env:PATH = "$BundledMingwBin;$env:PATH"
    }

    $ProtocVersion = (& $Protoc --version | Out-String).Trim()
    if ($LASTEXITCODE -ne 0 -or $ProtocVersion -ne "libprotoc 3.21.4") {
        throw "Expected repository protobuf compiler libprotoc 3.21.4, got: $ProtocVersion. Prepare the Windows toolchain or pass -Protoc explicitly."
    }

    New-Item -ItemType Directory -Force -Path $ToolDir | Out-Null
    try {
        $ToolLock = [IO.File]::Open(
            (Join-Path $ToolDir "generation.lock"),
            [IO.FileMode]::OpenOrCreate,
            [IO.FileAccess]::ReadWrite,
            [IO.FileShare]::None
        )
    } catch {
        throw "Another pinned proto generation is already using $ToolDir. Wait for it to finish and retry."
    }
    try {
        if ((Get-ToolVersionOrEmpty $GoPlugin) -ne "protoc-gen-go.exe v1.28.1") {
            $GoPlugin = Remove-GeneratorFileForReplacement $GoPlugin "Pinned protoc-gen-go executable"
            $GoExecutable = Get-SafeApplicationPath "go" "Go executable for proto generator installation"
            $PreviousGoBin = $env:GOBIN
            try {
                $env:GOBIN = $ToolDir
                Invoke-Checked $GoExecutable @("install", "google.golang.org/protobuf/cmd/protoc-gen-go@v1.28.1")
            } finally {
                $env:GOBIN = $PreviousGoBin
            }
            if ((Get-ToolVersionOrEmpty $GoPlugin) -ne "protoc-gen-go.exe v1.28.1") {
                throw "Pinned protoc-gen-go installation did not produce v1.28.1 at $GoPlugin."
            }
        }
        if ((Get-ToolVersionOrEmpty $GrpcPlugin) -ne "protoc-gen-go-grpc 1.2.0") {
            $GrpcPlugin = Remove-GeneratorFileForReplacement $GrpcPlugin "Pinned protoc-gen-go-grpc executable"
            $GoExecutable = Get-SafeApplicationPath "go" "Go executable for proto generator installation"
            $PreviousGoBin = $env:GOBIN
            try {
                $env:GOBIN = $ToolDir
                Invoke-Checked $GoExecutable @("install", "google.golang.org/grpc/cmd/protoc-gen-go-grpc@v1.2.0")
            } finally {
                $env:GOBIN = $PreviousGoBin
            }
            if ((Get-ToolVersionOrEmpty $GrpcPlugin) -ne "protoc-gen-go-grpc 1.2.0") {
                throw "Pinned protoc-gen-go-grpc installation did not produce v1.2.0 at $GrpcPlugin."
            }
        }

        $env:PATH = "$ToolDir;$env:PATH"
        $GenerationStageRootCreated = !(Test-Path -LiteralPath $GenerationStageRoot)
        $GenerationStage = ""
        try {
            New-Item -ItemType Directory -Force -Path $GenerationStageRoot | Out-Null
            $GenerationStage = Assert-PathOutsideProtectedProduction `
                (Join-Path $GenerationStageRoot ([Guid]::NewGuid().ToString('N'))) `
                "Proto generation staging directory"
            New-Item -ItemType Directory -Path $GenerationStage | Out-Null
            Push-Location $GeneratorDir
            try {
                Invoke-Checked $Protoc @(
                    "-I", ".",
                    "--go_out=$GenerationStage", "--go_opt", "paths=source_relative",
                    "--go-grpc_out=$GenerationStage", "--go-grpc_opt", "paths=source_relative",
                    "libcore.proto"
                )
            } finally {
                Pop-Location
            }

            $replacementId = [Guid]::NewGuid().ToString('N')
            $replacementRecords = @()
            foreach ($generatedName in @("libcore.pb.go", "libcore_grpc.pb.go")) {
                $stagedOutput = Join-Path $GenerationStage $generatedName
                if (!(Test-Path -LiteralPath $stagedOutput -PathType Leaf)) {
                    throw "Pinned proto generation did not create $generatedName."
                }
                $destination = Assert-PathOutsideProtectedProduction `
                    (Join-Path $GeneratorDir $generatedName) `
                    "Generated proto output $generatedName"
                if (Test-Path -LiteralPath $destination -PathType Container) {
                    throw "Generated proto output expected a file but found a directory: $destination"
                }
                $incoming = Assert-NewFileOutsideProtectedProduction `
                    (Join-Path $GeneratorDir ".$generatedName.$replacementId.new") `
                    "Generated proto incoming file $generatedName"
                $backup = Assert-NewFileOutsideProtectedProduction `
                    (Join-Path $GeneratorDir ".$generatedName.$replacementId.bak") `
                    "Generated proto backup file $generatedName"
                $replacementRecords += [pscustomobject]@{
                    name = $generatedName
                    staged = $stagedOutput
                    destination = $destination
                    incoming = $incoming
                    backup = $backup
                    original_exists = (Test-Path -LiteralPath $destination -PathType Leaf)
                    incoming_ready = $false
                    original_moved = $false
                    new_installed = $false
                }
            }

            try {
                foreach ($record in $replacementRecords) {
                    Move-Item -LiteralPath $record.staged -Destination $record.incoming
                    $record.incoming_ready = $true
                }
                foreach ($record in $replacementRecords) {
                    if ($record.original_exists) {
                        Move-Item -LiteralPath $record.destination -Destination $record.backup
                        $record.original_moved = $true
                    }
                }
                foreach ($record in $replacementRecords) {
                    Move-Item -LiteralPath $record.incoming -Destination $record.destination
                    $record.incoming_ready = $false
                    $record.new_installed = $true
                }
            } catch {
                $installError = $_
                $rollbackErrors = [Collections.Generic.List[string]]::new()
                for ($index = $replacementRecords.Count - 1; $index -ge 0; $index--) {
                    $record = $replacementRecords[$index]
                    try {
                        if ($record.new_installed -and (Test-Path -LiteralPath $record.destination)) {
                            Remove-Item -LiteralPath $record.destination -Force
                            $record.new_installed = $false
                        }
                        if ($record.original_moved -and (Test-Path -LiteralPath $record.backup -PathType Leaf)) {
                            Move-Item -LiteralPath $record.backup -Destination $record.destination
                            $record.original_moved = $false
                        }
                        if ($record.incoming_ready -and (Test-Path -LiteralPath $record.incoming -PathType Leaf)) {
                            Remove-Item -LiteralPath $record.incoming -Force
                            $record.incoming_ready = $false
                        }
                    } catch {
                        $rollbackErrors.Add("$($record.name): $($_.Exception.Message)")
                    }
                }
                if ($rollbackErrors.Count -gt 0) {
                    throw "Proto output install failed: $($installError.Exception.Message). Rollback also failed: $($rollbackErrors -join '; ')"
                }
                throw $installError
            }

            foreach ($record in $replacementRecords) {
                if ($record.original_moved -and (Test-Path -LiteralPath $record.backup -PathType Leaf)) {
                    Remove-Item -LiteralPath $record.backup -Force
                    $record.original_moved = $false
                }
            }
        } finally {
            if (![string]::IsNullOrWhiteSpace($GenerationStage) -and
                (Test-Path -LiteralPath $GenerationStage -PathType Container)) {
                Assert-DirectoryTreeHasNoReparsePoints $GenerationStage "Proto generation staging tree"
                Remove-Item -LiteralPath $GenerationStage -Recurse -Force
            }
            if ($GenerationStageRootCreated -and
                (Test-Path -LiteralPath $GenerationStageRoot -PathType Container) -and
                @(Get-ChildItem -LiteralPath $GenerationStageRoot -Force).Count -eq 0) {
                Remove-Item -LiteralPath $GenerationStageRoot -Force
            }
        }
    } finally {
        $ToolLock.Dispose()
    }
} finally {
    $env:PATH = $PreviousPath
}
