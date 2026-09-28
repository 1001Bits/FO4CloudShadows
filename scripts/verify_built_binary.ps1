[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string]$BinaryPath,
    [Parameter(Mandatory)]
    [string]$DumpbinPath,
    [string]$ExpectedVersion = "1.1.0.0",
    [string]$EvidenceMarkerPath,
    [switch]$RequireReleaseFeatures
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "common.ps1")
$fullEvidencePath = $null
if ($EvidenceMarkerPath) {
    $fullEvidencePath = [IO.Path]::GetFullPath($EvidenceMarkerPath)
    if (Test-Path -LiteralPath $fullEvidencePath -PathType Leaf) {
        Remove-Item -LiteralPath $fullEvidencePath -Force
    }
}
$resolvedBinary = (Resolve-Path -LiteralPath $BinaryPath).Path
$resolvedDumpbin = (Resolve-Path -LiteralPath $DumpbinPath).Path
$expected = [Version]$ExpectedVersion
$versionInfo = (Get-Item -LiteralPath $resolvedBinary).VersionInfo

$fileVersion = Get-FO4CSVersionValue $versionInfo "FileVersionRaw" "FileVersion"
$productVersion = Get-FO4CSVersionValue $versionInfo "ProductVersionRaw" "ProductVersion"
if ($fileVersion -ne $expected) {
    throw "File version is '$fileVersion', expected '$expected'."
}
if ($productVersion -ne $expected) {
    throw "Product version is '$productVersion', expected '$expected'."
}
foreach ($property in @("CompanyName", "FileDescription", "LegalCopyright", "OriginalFilename", "ProductName")) {
    if ([string]::IsNullOrWhiteSpace($versionInfo.$property)) {
        throw "VERSIONINFO field '$property' is empty."
    }
}
if ($versionInfo.OriginalFilename -ne "FO4CloudShadows.dll") {
    throw "OriginalFilename is '$($versionInfo.OriginalFilename)', expected 'FO4CloudShadows.dll'."
}

if ($RequireReleaseFeatures) {
    $binaryText = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($resolvedBinary))
    $releaseMarker = 'FO4CS_BUILD_FEATURES:developer_tools=0;private_profiling=0'
    if (-not $binaryText.Contains($releaseMarker)) {
        throw 'Packaging requires the DLL with developer tools and private profiling compiled OFF.'
    }
    foreach ($debugControl in @('Advanced / debug', 'Capture method (F6)',
            'Preview final shadow mask', 'Run automated cloud-shadow test',
            'Cloud shadows in godrays', 'Named RenderVolume hook installed')) {
        if ($binaryText.Contains($debugControl)) {
            throw "Release DLL still contains a developer control or godray hook: $debugControl"
        }
    }
    Write-Host 'Verified release feature policy: advanced debug controls and godray cloud occlusion are absent.'
}

$exportsText = (& $resolvedDumpbin /nologo /exports $resolvedBinary 2>&1 | Out-String)
if ($LASTEXITCODE -ne 0) { throw "dumpbin /exports failed ($LASTEXITCODE)." }
$exports = @([regex]::Matches(
    $exportsText,
    '(?m)^\s*\d+\s+[0-9A-F]+\s+[0-9A-F]+\s+(\S+)(?:\s+=.*)?\s*$') |
    ForEach-Object { $_.Groups[1].Value } |
    Sort-Object -Unique)
$expectedExports = @("F4SEPlugin_Load", "F4SEPlugin_Query", "F4SEPlugin_Version")
$exportDifference = @(Compare-Object -ReferenceObject $expectedExports -DifferenceObject $exports)
if ($exportDifference.Count -ne 0) {
    throw "Unexpected export contract. Found: $($exports -join ', ')."
}

# Inspect the exported record itself: F4SE reads these flags before Load and
# requires an installed Address Library when either database bit is set.
$versionExport = [regex]::Match($exportsText,
    '(?m)^\s*\d+\s+[0-9A-F]+\s+([0-9A-F]+)\s+F4SEPlugin_Version(?:\s+=.*)?\s*$')
if (-not $versionExport.Success) { throw 'Cannot locate F4SEPlugin_Version RVA.' }
$versionRva = [Convert]::ToUInt32($versionExport.Groups[1].Value, 16)
$imageBytes = [IO.File]::ReadAllBytes($resolvedBinary)
$ntOffset = [BitConverter]::ToInt32($imageBytes, 0x3C)
if ($ntOffset -lt 0 -or $ntOffset + 24 -gt $imageBytes.Length -or
    [BitConverter]::ToUInt32($imageBytes, $ntOffset) -ne 0x4550) {
    throw 'Invalid PE header while reading F4SE metadata.'
}
$sectionCount = [BitConverter]::ToUInt16($imageBytes, $ntOffset + 6)
$optionalSize = [BitConverter]::ToUInt16($imageBytes, $ntOffset + 20)
$sectionTable = $ntOffset + 24 + $optionalSize
$versionOffset = -1L
for ($index = 0; $index -lt $sectionCount; ++$index) {
    $sectionOffset = $sectionTable + 40 * $index
    if ($sectionOffset + 40 -gt $imageBytes.Length) { throw 'Truncated PE section table.' }
    $sectionRva = [BitConverter]::ToUInt32($imageBytes, $sectionOffset + 12)
    $rawSize = [BitConverter]::ToUInt32($imageBytes, $sectionOffset + 16)
    $rawOffset = [BitConverter]::ToUInt32($imageBytes, $sectionOffset + 20)
    $relative = [long]$versionRva - $sectionRva
    if ($relative -ge 0 -and $relative + 1116 -le $rawSize) {
        $versionOffset = [long]$rawOffset + $relative
        break
    }
}
if ($versionOffset -lt 0 -or $versionOffset + 1116 -gt $imageBytes.Length) {
    throw 'F4SE metadata is not fully backed by the DLL file.'
}
if ([BitConverter]::ToUInt32($imageBytes, $versionOffset) -ne 1) {
    throw 'Unexpected F4SE metadata schema.'
}
$packedPluginVersion = ($expected.Major -shl 24) -bor ($expected.Minor -shl 16) -bor
    ($expected.Build -shl 4) -bor $expected.Revision
if ([BitConverter]::ToUInt32($imageBytes, $versionOffset + 4) -ne $packedPluginVersion) {
    throw "F4SE plugin version does not match $ExpectedVersion."
}
$addressFlags = [BitConverter]::ToUInt32($imageBytes, $versionOffset + 520)
if ($addressFlags -ne 0) {
    throw "F4SE metadata must use exact versions with no Address Library flags; found $addressFlags."
}
# OG 163, NG 984, AE 221/240, VR 72, and the exact F4SEVR runtime proxy.
$compatibleVersions = @(0x010A0A30, 0x010A3D80, 0x010B0DD0, 0x010B0F00, 0x01020480, 0x010A08A0)
for ($index = 0; $index -lt 16; ++$index) {
    $actualVersion = [BitConverter]::ToUInt32($imageBytes, $versionOffset + 528 + 4 * $index)
    $expectedRuntime = if ($index -lt $compatibleVersions.Count) { $compatibleVersions[$index] } else { 0 }
    if ($actualVersion -ne $expectedRuntime) {
        throw "Unexpected F4SE compatible version at index $index."
    }
}
Write-Host 'Verified F4SE metadata: exact supported versions, no Address Library requirement.'

$headersText = (& $resolvedDumpbin /nologo /headers $resolvedBinary 2>&1 | Out-String)
if ($LASTEXITCODE -ne 0) { throw "dumpbin /headers failed ($LASTEXITCODE)." }
foreach ($requirement in @(
    @{ Pattern = '(?im)\b8664\s+machine\s+\(x64\)'; Description = 'x64 machine type' },
    @{ Pattern = '(?im)^\s*DLL\s*$'; Description = 'DLL image characteristic' },
    @{ Pattern = '(?im)Dynamic base'; Description = 'ASLR/DYNAMICBASE' },
    @{ Pattern = '(?im)NX compatible'; Description = 'DEP/NXCOMPAT' },
    @{ Pattern = '(?im)High Entropy Virtual Addresses'; Description = 'HIGHENTROPYVA' })) {
    if ($headersText -notmatch $requirement.Pattern) {
        throw "PE contract is missing $($requirement.Description)."
    }
}

$dependentsText = (& $resolvedDumpbin /nologo /dependents $resolvedBinary 2>&1 | Out-String)
if ($LASTEXITCODE -ne 0) { throw "dumpbin /dependents failed ($LASTEXITCODE)." }
if ($dependentsText -match '(?im)\b(?:MSVCP\d+D|VCRUNTIME\d+(?:_\d+)?D|ucrtbased)\.dll\b') {
    throw "The Release DLL imports a debug C/C++ runtime: '$($Matches[0])'."
}

if ($fullEvidencePath) {
    $markerParent = Split-Path -Parent $fullEvidencePath
    if ($markerParent) {
        New-Item -ItemType Directory -Path $markerParent -Force | Out-Null
    }
    $evidence = [ordered]@{
        SchemaVersion = 1
        Version = $expected.ToString()
        BinarySHA256 = (Get-FileHash -LiteralPath $resolvedBinary -Algorithm SHA256).Hash
        VerifiedUTC = [DateTime]::UtcNow.ToString("o")
    }
    $tempEvidence = "$fullEvidencePath.$([guid]::NewGuid().ToString('N')).tmp"
    try {
        $utf8NoBom = New-Object Text.UTF8Encoding($false)
        [IO.File]::WriteAllText(
            $tempEvidence,
            ($evidence | ConvertTo-Json),
            $utf8NoBom)
        Move-Item -LiteralPath $tempEvidence -Destination $fullEvidencePath -Force
    }
    finally {
        if (Test-Path -LiteralPath $tempEvidence -PathType Leaf) {
            Remove-Item -LiteralPath $tempEvidence -Force
        }
    }
}

Write-Host "Verified PE version, exports, hardening flags, architecture, and runtime imports." -ForegroundColor Green
