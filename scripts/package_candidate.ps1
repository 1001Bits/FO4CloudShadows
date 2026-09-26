[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$ProjectRoot,
    [Parameter(Mandatory)][string]$BuildRoot,
    [Parameter(Mandatory)][ValidatePattern('^\d+\.\d+\.\d+$')][string]$Version,
    [Parameter(Mandatory)][string]$CMakeExecutable,
    [Parameter(Mandatory)][string]$GitExecutable,
    [Parameter(Mandatory)][string]$DumpbinPath,
    [Parameter(Mandatory)][string]$VcpkgRoot,
    [Parameter(Mandatory)][string]$VcpkgInstalledRoot,
    [Parameter(Mandatory)][string]$VcpkgDownloadsRoot,
    [Parameter(Mandatory)][string]$VcpkgTriplet
)
$ErrorActionPreference = 'Stop'
$ProjectRoot = (Resolve-Path -LiteralPath $ProjectRoot).Path
$BuildRoot = (Resolve-Path -LiteralPath $BuildRoot).Path
$binary = Join-Path $BuildRoot 'bin/Release/FO4CloudShadows.dll'
& (Join-Path $PSScriptRoot 'verify_built_binary.ps1') -BinaryPath $binary `
    -DumpbinPath $DumpbinPath -ExpectedVersion "$Version.0" -RequireReleaseFeatures
$identity = @(Get-Content -LiteralPath (Join-Path $BuildRoot 'FO4CloudShadows.build-id.txt'))
if ($identity.Count -ne 2 -or $identity[0] -notmatch ('^' + [regex]::Escape($Version) + '-([0-9a-f]{16})$')) {
    throw 'Missing or invalid build identity.'
}
$buildId = $Matches[1]
if ($identity[1] -notmatch '^[0-9a-f]{64}$' -or -not $identity[1].StartsWith($buildId)) {
    throw 'Invalid full build fingerprint.'
}
if (-not [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($binary)).Contains($buildId)) {
    throw 'The DLL does not contain the configured build identity; rebuild before packaging.'
}
$packageLabel = "$Version-$buildId"
$gitArgs = @('-c', "safe.directory=$($ProjectRoot.Replace('\', '/'))", '-C', $ProjectRoot)
$revision = (& $GitExecutable @gitArgs rev-parse --verify HEAD | Out-String).Trim()
if ($LASTEXITCODE -ne 0 -or $revision -notmatch '^[0-9a-f]{40}$') { throw 'Cannot identify the source revision.' }
$dirty = @(& $GitExecutable @gitArgs status --porcelain --untracked-files=all)
if ($LASTEXITCODE -ne 0 -or $dirty.Count) { throw 'Commit all source and release tooling before packaging.' }
$sourceArgs = @{
    ProjectRoot = $ProjectRoot
    Version = $Version
    BuildId = $buildId
    GitExecutable = $GitExecutable
    VcpkgRoot = $VcpkgRoot
    VcpkgInstalledRoot = $VcpkgInstalledRoot
    VcpkgDownloadsRoot = $VcpkgDownloadsRoot
    VcpkgTriplet = $VcpkgTriplet
    VcpkgSourceLock = Join-Path $ProjectRoot 'config/vcpkg-source-lock.json'
}
& (Join-Path $PSScriptRoot 'package_source.ps1') @sourceArgs `
    -BuildRoot $BuildRoot -CMakeExecutable $CMakeExecutable
$sourceZip = Join-Path $BuildRoot "FO4CloudShadows-$packageLabel-Source.zip"

# Use an empty, uniquely named tree. Never recursively remove a caller's path.
$stage = Join-Path $BuildRoot ('candidate-stage/' + [Guid]::NewGuid().ToString('N'))
& $CMakeExecutable --install $BuildRoot --config Release --component Runtime --prefix $stage
if ($LASTEXITCODE -ne 0) { throw 'Candidate install failed' }
& $CMakeExecutable "-DSTAGE_ROOT=$stage" "-DSOURCE_ROOT=$ProjectRoot" `
    "-DEXPECTED_VERSION=$Version" -P (Join-Path $ProjectRoot 'cmake/VerifyRuntimeAssets.cmake')
if ($LASTEXITCODE -ne 0) { throw 'Candidate runtime assets failed verification' }
$defaults = Get-Content -LiteralPath (Join-Path $stage 'Data/Shaders/Features/CloudShadows.json') -Raw | ConvertFrom-Json
$mcmDefaults = Get-Content -LiteralPath (Join-Path $stage 'Data/MCM/Config/FO4CloudShadows/settings.ini') -Raw
if ($defaults.Opacity -ne 2.0 -or $mcmDefaults -notmatch '(?m)^fOpacity=2(?:\.0+)?\r?$') {
    throw 'Release default opacity must be 2.0 in both JSON and MCM.'
}
$release = [ordered]@{
    Version = $Version
    BuildId = $buildId
    BuildFingerprint = $identity[1]
    SourceCommit = $revision
    SourceURL = "https://github.com/1001Bits/FO4CloudShadows/tree/$revision"
    SourceTag = "build-$buildId"
    License = 'GPL-3.0-only'
    DefaultOpacity = 2.0
    DllSHA256 = (Get-FileHash -LiteralPath $binary -Algorithm SHA256).Hash.ToLowerInvariant()
    SourceArchive = [IO.Path]::GetFileName($sourceZip)
    SourceArchiveSHA256 = (Get-FileHash -LiteralPath $sourceZip -Algorithm SHA256).Hash.ToLowerInvariant()
}
[IO.File]::WriteAllText((Join-Path $stage 'RELEASE.json'), (($release | ConvertTo-Json) + "`n"))

# Install scripts use Data/; mod-manager archives put those directories at root.
$payload = [Collections.Generic.SortedDictionary[string,string]]::new([StringComparer]::Ordinal)
foreach ($file in Get-ChildItem -LiteralPath $stage -Recurse -File -Force) {
    $relative = $file.FullName.Substring($stage.Length + 1).Replace('\', '/')
    if ($relative.StartsWith('Data/')) { $relative = $relative.Substring(5) }
    if ($relative -match '(?i)(^MCM/Settings/|\.(pdb|dxbc|cso|exe|log)$)') { throw "Unexpected runtime payload: $relative" }
    $payload.Add($relative, $file.FullName)
}
$candidate = Join-Path $BuildRoot "FO4CloudShadows-$packageLabel-Candidate.zip"
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$stream = [IO.File]::Open($candidate, [IO.FileMode]::Create)
try {
    $archive = [IO.Compression.ZipArchive]::new($stream, [IO.Compression.ZipArchiveMode]::Create, $false)
    try {
        foreach ($item in $payload.GetEnumerator()) {
            $entry = $archive.CreateEntry($item.Key, [IO.Compression.CompressionLevel]::Optimal)
            $entry.LastWriteTime = [DateTimeOffset]::Parse('2000-01-01T00:00:00Z')
            $inputStream = [IO.File]::OpenRead($item.Value)
            $outputStream = $entry.Open()
            try { $inputStream.CopyTo($outputStream) }
            finally { $outputStream.Dispose(); $inputStream.Dispose() }
        }
    }
    finally { $archive.Dispose() }
}
finally { $stream.Dispose() }

# Reopen the actual ZIP and compare every entry with its verified staged file.
$archive = [IO.Compression.ZipFile]::OpenRead($candidate)
$sha = [Security.Cryptography.SHA256]::Create()
try {
    if ($archive.Entries.Count -ne $payload.Count) { throw 'Runtime ZIP has an unexpected file count.' }
    foreach ($entry in $archive.Entries) {
        if (-not $payload.ContainsKey($entry.FullName)) { throw "Unexpected ZIP entry: $($entry.FullName)" }
        $entryStream = $entry.Open()
        try { $entryHash = [BitConverter]::ToString($sha.ComputeHash($entryStream)).Replace('-', '') }
        finally { $entryStream.Dispose() }
        if ($entryHash -ne (Get-FileHash -LiteralPath $payload[$entry.FullName] -Algorithm SHA256).Hash) {
            throw "Runtime ZIP hash mismatch: $($entry.FullName)"
        }
        $payload.Remove($entry.FullName) | Out-Null
    }
    if ($payload.Count) { throw 'Runtime ZIP is missing staged files.' }
}
finally { $sha.Dispose(); $archive.Dispose() }
$hash = (Get-FileHash -LiteralPath $candidate -Algorithm SHA256).Hash.ToLowerInvariant()
[IO.File]::WriteAllText("$candidate.sha256", "$hash  $([IO.Path]::GetFileName($candidate))`n")
Write-Host "Candidate and matching source prepared: $candidate"
Write-Host 'Publication still requires the in-game visual, compatibility and performance matrix.'
