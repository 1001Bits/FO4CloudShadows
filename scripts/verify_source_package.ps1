[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string]$ArchivePath,
    [Parameter(Mandatory)]
    [string]$ArchiveHashPath,
    [Parameter(Mandatory)]
    [string]$ProjectRoot,
    [string]$CommonLibSource,
    [Parameter(Mandatory)]
    [string]$GitExecutable,
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
$resolvedArchive = (Resolve-Path -LiteralPath $ArchivePath).Path
$resolvedArchiveHash = (Resolve-Path -LiteralPath $ArchiveHashPath).Path
$resolvedProject = (Resolve-Path -LiteralPath $ProjectRoot).Path
$resolvedGit = (Resolve-Path -LiteralPath $GitExecutable).Path

$actualArchiveHash = (Get-FileHash -LiteralPath $resolvedArchive -Algorithm SHA256).Hash.ToLowerInvariant()
$hashEvidence = [IO.File]::ReadAllText($resolvedArchiveHash)
$hashMatch = [regex]::Match(
    $hashEvidence,
    '^([0-9A-Fa-f]{64})  ([^\r\n]+)\r?\n?$')
if (-not $hashMatch.Success -or
    $hashMatch.Groups[1].Value.ToLowerInvariant() -ne $actualArchiveHash -or
    $hashMatch.Groups[2].Value -ne [IO.Path]::GetFileName($resolvedArchive)) {
    throw "The Corresponding Source archive checksum evidence is invalid or stale."
}

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
        throw "$Label contains tracked modifications."
    }
}
if ($CommonLibSource) {
    $resolvedCommonLib = (Resolve-Path -LiteralPath $CommonLibSource).Path
    $sharedSource = Join-Path $resolvedCommonLib "lib/commonlib-shared"
    Assert-PinnedCheckout $resolvedCommonLib $ExpectedCommonLibCommit "CommonLibF4"
    Assert-PinnedCheckout $sharedSource $ExpectedCommonLibSharedCommit "commonlib-shared"
}

Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::OpenRead($resolvedArchive)
try {
    $rootName = "FO4CloudShadows-$Version-$BuildId-Source"
    $rootPrefix = "$rootName/"
    $manifestArchivePath = "${rootPrefix}SOURCE-MANIFEST.sha256"
    $payloadEntries = New-Object 'Collections.Generic.Dictionary[string,object]' `
        ([StringComparer]::Ordinal)
    $manifestEntry = $null
    foreach ($entry in $archive.Entries) {
        if ([string]::IsNullOrEmpty($entry.Name)) {
            throw "The source ZIP contains an unexpected directory entry '$($entry.FullName)'."
        }
        if ($entry.FullName.Contains('\') -or
            -not $entry.FullName.StartsWith($rootPrefix, [StringComparison]::Ordinal)) {
            throw "The source ZIP contains an entry outside '$rootName': '$($entry.FullName)'."
        }
        $relativeEntryPath = $entry.FullName.Substring($rootPrefix.Length)
        if ([string]::IsNullOrWhiteSpace($relativeEntryPath) -or
            $relativeEntryPath -match '(^|/)\.\.(/|$)' -or
            $relativeEntryPath -match '^[A-Za-z]:') {
            throw "The source ZIP contains an unsafe entry '$($entry.FullName)'."
        }
        if ($entry.FullName -eq $manifestArchivePath) {
            if ($manifestEntry) { throw "The source ZIP contains duplicate manifests." }
            $manifestEntry = $entry
        }
        elseif ($payloadEntries.ContainsKey($relativeEntryPath)) {
            throw "The source ZIP contains duplicate path '$relativeEntryPath'."
        }
        else {
            $payloadEntries.Add($relativeEntryPath, $entry)
        }
    }
    if (-not $manifestEntry) { throw "The source ZIP has no SOURCE-MANIFEST.sha256." }

    $reader = New-Object IO.StreamReader($manifestEntry.Open(), [Text.Encoding]::UTF8)
    try { $manifestText = $reader.ReadToEnd() }
    finally { $reader.Dispose() }

    $manifest = New-Object 'Collections.Generic.Dictionary[string,string]' `
        ([StringComparer]::Ordinal)
    foreach ($line in $manifestText -split "`n") {
        if ([string]::IsNullOrWhiteSpace($line)) { continue }
        $lineMatch = [regex]::Match($line.TrimEnd("`r"), '^([0-9a-f]{64})  (.+)$')
        if (-not $lineMatch.Success) {
            throw "Malformed SOURCE-MANIFEST.sha256 line: '$line'."
        }
        $path = $lineMatch.Groups[2].Value
        if ($path.Contains('\') -or $path -match '(^|/)\.\.(/|$)' -or
            [IO.Path]::IsPathRooted($path)) {
            throw "Unsafe source manifest path '$path'."
        }
        if ($manifest.ContainsKey($path)) {
            throw "Duplicate source manifest path '$path'."
        }
        $manifest.Add($path, $lineMatch.Groups[1].Value)
    }
    if ($manifest.Count -ne $payloadEntries.Count) {
        throw "Source ZIP payload count does not match its manifest: archive=$($payloadEntries.Count), manifest=$($manifest.Count)."
    }
    $sha256 = [Security.Cryptography.SHA256]::Create()
    try {
        foreach ($path in $manifest.Keys) {
            if (-not $payloadEntries.ContainsKey($path)) {
                throw "The source ZIP payload is missing manifest input '$path'."
            }
            $entryStream = $payloadEntries[$path].Open()
            try {
                $entryHash = [BitConverter]::ToString(
                    $sha256.ComputeHash($entryStream)).Replace('-', '').ToLowerInvariant()
            }
            finally { $entryStream.Dispose() }
            if ($entryHash -ne $manifest[$path]) {
                throw "The source ZIP payload is stale for '$path'."
            }
        }
    }
    finally { $sha256.Dispose() }
}
finally { $archive.Dispose() }

$expectedFiles = @{}
function Add-ExpectedFile([string]$Path, [string]$PackagePath) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "A current Corresponding Source input is missing: '$Path'."
    }
    $normalized = $PackagePath.Replace([IO.Path]::DirectorySeparatorChar, '/')
    if ($expectedFiles.ContainsKey($normalized)) {
        throw "Duplicate current source package path '$normalized'."
    }
    $expectedFiles[$normalized] = $Path
}

$projectFiles = @(
    ".gitignore", "CMakeLists.txt", "CMakePresets.json", "EXCEPTIONS.md",
    "LICENSE", "README.md", "SOURCE_DELIVERY.md", "THIRD_PARTY_NOTICES.md",
    "vcpkg.json", "tools/extract_fxp_dxbc.py")
$projectDirectories = @(
    ".github", "assets", "cmake", "config", "docs", "extern/imgui",
    "licenses", "scripts", "shaders", "src", "tests")
foreach ($relativePath in $projectFiles) {
    Add-ExpectedFile (Join-Path $resolvedProject $relativePath) $relativePath
}
$projectPrefix = $resolvedProject.TrimEnd(
    [IO.Path]::DirectorySeparatorChar,
    [IO.Path]::AltDirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
foreach ($relativeDirectory in $projectDirectories) {
    $directory = Join-Path $resolvedProject $relativeDirectory
    foreach ($file in Get-ChildItem -LiteralPath $directory -Recurse -File -Force) {
        Add-ExpectedFile $file.FullName $file.FullName.Substring($projectPrefix.Length)
    }
}

function Add-TrackedFiles([string]$Checkout, [string]$PackagePrefix) {
    $trackedFiles = @(& $resolvedGit -C $Checkout ls-files)
    if ($LASTEXITCODE -ne 0 -or $trackedFiles.Count -eq 0) {
        throw "Could not enumerate tracked source files in '$Checkout'."
    }
    foreach ($relativePath in $trackedFiles) {
        $source = Join-Path $Checkout $relativePath
        if (Test-Path -LiteralPath $source -PathType Leaf) {
            Add-ExpectedFile $source "$PackagePrefix/$($relativePath.Replace('\', '/'))"
        }
    }
}
if ($CommonLibSource) {
    Add-TrackedFiles $resolvedCommonLib "extern/CommonLibF4"
    Add-TrackedFiles $sharedSource "extern/CommonLibF4/lib/commonlib-shared"
}

$vcpkgInputs = @(Get-FO4CSVcpkgSourceBundleInputs `
    -VcpkgRoot $VcpkgRoot `
    -VcpkgInstalledRoot $VcpkgInstalledRoot `
    -VcpkgDownloadsRoot $VcpkgDownloadsRoot `
    -VcpkgTriplet $VcpkgTriplet `
    -SourceLockPath $VcpkgSourceLock `
    -GitExecutable $resolvedGit)
foreach ($inputFile in $vcpkgInputs) {
    Add-ExpectedFile $inputFile.SourcePath $inputFile.PackagePath
}

if ($manifest.Count -ne $expectedFiles.Count) {
    throw "Source manifest input count is stale: archive=$($manifest.Count), current=$($expectedFiles.Count)."
}
foreach ($packagePath in $expectedFiles.Keys) {
    if (-not $manifest.ContainsKey($packagePath)) {
        throw "The source archive is missing current input '$packagePath'."
    }
    $currentHash = (Get-FileHash -LiteralPath $expectedFiles[$packagePath] -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($manifest[$packagePath] -ne $currentHash) {
        throw "The source archive is stale for '$packagePath'."
    }
}

Write-Host "Verified Corresponding Source archive against $($expectedFiles.Count) current inputs." -ForegroundColor Green
