[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string]$ProjectRoot,
    [string]$CommonLibSource,
    [Parameter(Mandatory)]
    [string]$BuildRoot,
    [Parameter(Mandatory)]
    [string]$GitExecutable,
    [Parameter(Mandatory)]
    [string]$CMakeExecutable,
    [Parameter(Mandatory)]
    [string]$Version,
    [Parameter(Mandatory)]
    [ValidatePattern('^[0-9a-f]{16}$')]
    [string]$BuildId,
    [string]$ExpectedCommonLibCommit,
    [string]$ExpectedCommonLibSharedCommit,
    [Parameter(Mandatory)]
    [string]$VcpkgRoot,
    [Parameter(Mandatory)]
    [string]$VcpkgInstalledRoot,
    [Parameter(Mandatory)]
    [string]$VcpkgDownloadsRoot,
    [Parameter(Mandatory)]
    [string]$VcpkgTriplet,
    [Parameter(Mandatory)]
    [string]$VcpkgSourceLock
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "vcpkg_source_bundle.ps1")
$resolvedProject = (Resolve-Path -LiteralPath $ProjectRoot).Path
$resolvedBuild = (Resolve-Path -LiteralPath $BuildRoot).Path
$resolvedGit = (Resolve-Path -LiteralPath $GitExecutable).Path

function Assert-PinnedCheckout(
    [string]$Path,
    [string]$ExpectedCommit,
    [string]$Label
) {
    $head = (& $resolvedGit -C $Path rev-parse --verify HEAD 2>$null | Out-String).Trim()
    if ($LASTEXITCODE -ne 0 -or $head -ne $ExpectedCommit) {
        throw "$Label HEAD '$head' does not match '$ExpectedCommit'."
    }
    & $resolvedGit -C $Path diff --quiet HEAD --
    if ($LASTEXITCODE -ne 0) {
        throw "$Label contains tracked modifications; refusing a non-reproducible source archive."
    }
}

if ($CommonLibSource) {
    # Retained for historical builds. The universal renderer compiles its own
    # runtime bridge and has no CommonLib build dependency.
    $resolvedCommonLib = (Resolve-Path -LiteralPath $CommonLibSource).Path
    $sharedSource = Join-Path $resolvedCommonLib "lib/commonlib-shared"
    Assert-PinnedCheckout $resolvedCommonLib $ExpectedCommonLibCommit "CommonLibF4"
    Assert-PinnedCheckout $sharedSource $ExpectedCommonLibSharedCommit "commonlib-shared"
}

$packageLabel = "$Version-$BuildId"
$stageParent = Join-Path $resolvedBuild ('source-package-stage/' + [Guid]::NewGuid().ToString('N'))
$sourceRoot = Join-Path $stageParent "FO4CloudShadows-$packageLabel-Source"
$buildPrefix = $resolvedBuild.TrimEnd(
    [IO.Path]::DirectorySeparatorChar,
    [IO.Path]::AltDirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
$stageFullPath = [IO.Path]::GetFullPath($stageParent)
if (-not $stageFullPath.StartsWith($buildPrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing to stage source outside the configured build directory."
}
New-Item -ItemType Directory -Path $sourceRoot -Force | Out-Null

$projectFiles = @(
    ".gitignore",
    "CMakeLists.txt",
    "CMakePresets.json",
    "EXCEPTIONS.md",
    "LICENSE",
    "README.md",
    "SOURCE_DELIVERY.md",
    "THIRD_PARTY_NOTICES.md",
    "vcpkg.json",
    "tools/extract_fxp_dxbc.py"
)
$projectDirectories = @(
    ".github",
    "assets",
    "cmake",
    "config",
    "docs",
    "extern/imgui",
    "licenses",
    "scripts",
    "shaders",
    "src",
    "tests"
)
foreach ($relativePath in $projectFiles) {
    $source = Join-Path $resolvedProject $relativePath
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
        throw "Required project source file is missing: '$relativePath'."
    }
    $destination = Join-Path $sourceRoot $relativePath
    New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
    Copy-Item -LiteralPath $source -Destination $destination -Force
}
foreach ($relativePath in $projectDirectories) {
    $source = Join-Path $resolvedProject $relativePath
    if (-not (Test-Path -LiteralPath $source -PathType Container)) {
        throw "Required project source directory is missing: '$relativePath'."
    }
    $destination = Join-Path $sourceRoot $relativePath
    New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
    Copy-Item -LiteralPath $source -Destination $destination -Recurse -Force
}

function Copy-TrackedFiles([string]$Checkout, [string]$Destination) {
    $trackedFiles = @(& $resolvedGit -C $Checkout ls-files)
    if ($LASTEXITCODE -ne 0 -or $trackedFiles.Count -eq 0) {
        throw "Could not enumerate tracked source files in '$Checkout'."
    }
    foreach ($relativePath in $trackedFiles) {
        $source = Join-Path $Checkout $relativePath
        if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
            # A gitlink is represented by the nested checkout copied separately.
            continue
        }
        $target = Join-Path $Destination $relativePath
        New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
        Copy-Item -LiteralPath $source -Destination $target -Force
    }
}

if ($CommonLibSource) {
    $commonLibDestination = Join-Path $sourceRoot "extern/CommonLibF4"
    Copy-TrackedFiles $resolvedCommonLib $commonLibDestination
    Copy-TrackedFiles $sharedSource (Join-Path $commonLibDestination "lib/commonlib-shared")
}

# GPL Corresponding Source also covers every non-system module statically
# linked into the DLL. Preserve the exact upstream source archives, audited
# vcpkg port/patch recipes, static-md triplet, installed ABI/SBOM evidence, and
# vcpkg license that produced Detours, spdlog/fmt, and nlohmann-json.
$vcpkgInputs = @(Get-FO4CSVcpkgSourceBundleInputs `
    -VcpkgRoot $VcpkgRoot `
    -VcpkgInstalledRoot $VcpkgInstalledRoot `
    -VcpkgDownloadsRoot $VcpkgDownloadsRoot `
    -VcpkgTriplet $VcpkgTriplet `
    -SourceLockPath $VcpkgSourceLock `
    -GitExecutable $resolvedGit)
foreach ($inputFile in $vcpkgInputs) {
    $target = Join-Path $sourceRoot $inputFile.PackagePath
    New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
    Copy-Item -LiteralPath $inputFile.SourcePath -Destination $target -Force
}

$forbiddenArtifacts = @(Get-ChildItem -LiteralPath $sourceRoot -Recurse -File -Force |
    Where-Object { $_.Extension -in @(".cso", ".dll", ".dxbc", ".exe", ".lib", ".obj", ".pdb") })
if ($forbiddenArtifacts.Count -ne 0) {
    throw "The source stage contains a binary or game-owned artifact: '$($forbiddenArtifacts[0].FullName)'."
}

$sourcePrefix = $sourceRoot.TrimEnd(
    [IO.Path]::DirectorySeparatorChar,
    [IO.Path]::AltDirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
$relativePaths = [string[]]@(Get-ChildItem -LiteralPath $sourceRoot -Recurse -File -Force |
    ForEach-Object { $_.FullName.Substring($sourcePrefix.Length) })
[Array]::Sort($relativePaths, [StringComparer]::Ordinal)
$manifestLines = foreach ($relativePath in $relativePaths) {
    $hash = (Get-FileHash -LiteralPath (Join-Path $sourceRoot $relativePath) -Algorithm SHA256).Hash.ToLowerInvariant()
    "$hash  $($relativePath.Replace([IO.Path]::DirectorySeparatorChar, '/'))"
}
$utf8NoBom = New-Object Text.UTF8Encoding($false)
[IO.File]::WriteAllText(
    (Join-Path $sourceRoot "SOURCE-MANIFEST.sha256"),
    (($manifestLines -join "`n") + "`n"),
    $utf8NoBom)

# Write entries in ordinal order with a fixed timestamp so identical source
# inputs produce identical ZIP bytes on the same .NET compression runtime.
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archivePath = Join-Path $resolvedBuild "FO4CloudShadows-$packageLabel-Source.zip"
$archiveHashPath = "$archivePath.sha256"
foreach ($oldOutput in @($archivePath, $archiveHashPath)) {
    if (Test-Path -LiteralPath $oldOutput -PathType Leaf) {
        Remove-Item -LiteralPath $oldOutput -Force
    }
}
$allFiles = [string[]]@(Get-ChildItem -LiteralPath $sourceRoot -Recurse -File -Force |
    ForEach-Object { $_.FullName.Substring($stageParent.Length + 1) })
[Array]::Sort($allFiles, [StringComparer]::Ordinal)
$archiveStream = [IO.File]::Open($archivePath, [IO.FileMode]::CreateNew)
try {
    $archive = New-Object IO.Compression.ZipArchive(
        $archiveStream,
        [IO.Compression.ZipArchiveMode]::Create,
        $false)
    try {
        foreach ($relativePath in $allFiles) {
            $entryName = $relativePath.Replace([IO.Path]::DirectorySeparatorChar, '/')
            $entry = $archive.CreateEntry($entryName, [IO.Compression.CompressionLevel]::Optimal)
            $entry.LastWriteTime = [DateTimeOffset]::Parse("2000-01-01T00:00:00Z")
            $inputStream = [IO.File]::OpenRead((Join-Path $stageParent $relativePath))
            $entryStream = $entry.Open()
            try { $inputStream.CopyTo($entryStream) }
            finally {
                $entryStream.Dispose()
                $inputStream.Dispose()
            }
        }
    }
    finally { $archive.Dispose() }
}
finally { $archiveStream.Dispose() }

$archiveHash = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash.ToLowerInvariant()
[IO.File]::WriteAllText(
    $archiveHashPath,
    "$archiveHash  $([IO.Path]::GetFileName($archivePath))`n",
    $utf8NoBom)
# Verify the finished archive, its manifest and every current input. This
# supersedes the old staging-only CMake check, whose required producer list
# predates the current cloud-geometry renderer.
$verifyArgs = @{
    ArchivePath = $archivePath
    ArchiveHashPath = $archiveHashPath
    ProjectRoot = $resolvedProject
    GitExecutable = $resolvedGit
    Version = $Version
    BuildId = $BuildId
    VcpkgRoot = $VcpkgRoot
    VcpkgInstalledRoot = $VcpkgInstalledRoot
    VcpkgDownloadsRoot = $VcpkgDownloadsRoot
    VcpkgTriplet = $VcpkgTriplet
    VcpkgSourceLock = $VcpkgSourceLock
}
if ($CommonLibSource) {
    $verifyArgs.CommonLibSource = $CommonLibSource
    $verifyArgs.ExpectedCommonLibCommit = $ExpectedCommonLibCommit
    $verifyArgs.ExpectedCommonLibSharedCommit = $ExpectedCommonLibSharedCommit
}
& (Join-Path $PSScriptRoot 'verify_source_package.ps1') @verifyArgs
Write-Host "Created Corresponding Source archive: $archivePath" -ForegroundColor Green
Write-Host "Source archive SHA-256: $archiveHash" -ForegroundColor Green
