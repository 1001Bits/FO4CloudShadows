function Get-FO4CSVcpkgSourceBundleInputs {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)]
        [string]$VcpkgRoot,
        [Parameter(Mandatory)]
        [string]$VcpkgInstalledRoot,
        [Parameter(Mandatory)]
        [string]$VcpkgDownloadsRoot,
        [Parameter(Mandatory)]
        [string]$VcpkgTriplet,
        [Parameter(Mandatory)]
        [string]$SourceLockPath,
        [Parameter(Mandatory)]
        [string]$GitExecutable
    )

    $resolvedVcpkg = (Resolve-Path -LiteralPath $VcpkgRoot).Path
    $resolvedInstalled = (Resolve-Path -LiteralPath $VcpkgInstalledRoot).Path
    if (-not (Test-Path -LiteralPath $VcpkgDownloadsRoot -PathType Container)) {
        throw "The vcpkg downloads directory is missing: '$VcpkgDownloadsRoot'. Re-run vcpkg without an asset-source read cache, or set FO4CS_VCPKG_DOWNLOADS_ROOT/VCPKG_DOWNLOADS to the populated downloads directory."
    }
    $resolvedDownloads = (Resolve-Path -LiteralPath $VcpkgDownloadsRoot).Path
    $resolvedLock = (Resolve-Path -LiteralPath $SourceLockPath).Path
    $resolvedGitForBundle = (Resolve-Path -LiteralPath $GitExecutable).Path
    $lock = [IO.File]::ReadAllText($resolvedLock) | ConvertFrom-Json

    if ($lock.SchemaVersion -ne 1) {
        throw "Unsupported vcpkg source lock schema '$($lock.SchemaVersion)'."
    }
    if ($lock.VcpkgBaseline -notmatch '^[0-9a-f]{40}$') {
        throw "The vcpkg source lock has no valid baseline commit."
    }
    if ($lock.Triplet -ne $VcpkgTriplet) {
        throw "The vcpkg source lock targets '$($lock.Triplet)', not '$VcpkgTriplet'."
    }

    $vcpkgHead = (& $resolvedGitForBundle -C $resolvedVcpkg rev-parse --verify HEAD 2>$null |
        Out-String).Trim()
    if ($LASTEXITCODE -ne 0 -or $vcpkgHead -ne $lock.VcpkgBaseline) {
        throw "vcpkg HEAD '$vcpkgHead' does not match source lock baseline '$($lock.VcpkgBaseline)'."
    }

    $packages = @($lock.Packages)
    if ($packages.Count -eq 0) {
        throw "The vcpkg source lock contains no packages."
    }
    $packageNames = @($packages | ForEach-Object { [string]$_.Name })
    if (($packageNames | Sort-Object -Unique).Count -ne $packageNames.Count) {
        throw "The vcpkg source lock contains duplicate package names."
    }

    $tripletRelative = "triplets/$VcpkgTriplet.cmake"
    $gitInputPaths = @($tripletRelative)
    $gitInputPaths += @($packageNames | ForEach-Object { "ports/$_" })
    & $resolvedGitForBundle -C $resolvedVcpkg diff --quiet HEAD -- @gitInputPaths
    if ($LASTEXITCODE -ne 0) {
        throw "The vcpkg port/triplet recipes used by this build contain tracked modifications."
    }
    $untrackedInputs = @(& $resolvedGitForBundle -C $resolvedVcpkg ls-files `
        --others --exclude-standard -- @gitInputPaths)
    if ($LASTEXITCODE -ne 0 -or $untrackedInputs.Count -ne 0) {
        throw "The vcpkg port/triplet recipes used by this build contain untracked inputs."
    }

    $inputs = New-Object Collections.Generic.List[object]
    function Add-BundleInput([string]$SourcePath, [string]$PackagePath) {
        if (-not (Test-Path -LiteralPath $SourcePath -PathType Leaf)) {
            throw "A required vcpkg source-bundle input is missing: '$SourcePath'."
        }
        $normalizedPackagePath = $PackagePath.Replace('\', '/')
        if ($normalizedPackagePath.StartsWith('/') -or
            $normalizedPackagePath -eq '..' -or
            $normalizedPackagePath.StartsWith('../')) {
            throw "Invalid vcpkg source-bundle path '$normalizedPackagePath'."
        }
        $inputs.Add([pscustomobject]@{
            SourcePath = (Resolve-Path -LiteralPath $SourcePath).Path
            PackagePath = $normalizedPackagePath
        })
    }

    $tripletPath = Join-Path $resolvedVcpkg $tripletRelative
    $tripletHash = (Get-FileHash -LiteralPath $tripletPath -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($tripletHash -ne ([string]$lock.TripletSHA256).ToLowerInvariant()) {
        throw "The '$VcpkgTriplet' triplet does not match the source lock."
    }
    Add-BundleInput $tripletPath "third-party/vcpkg/$tripletRelative"
    Add-BundleInput (Join-Path $resolvedVcpkg "LICENSE.txt") `
        "third-party/vcpkg/LICENSE.txt"

    $tripletInstalledRoot = Join-Path $resolvedInstalled $VcpkgTriplet
    foreach ($package in $packages) {
        $name = [string]$package.Name
        if ($name -notmatch '^[a-z0-9-]+$') {
            throw "Unsafe vcpkg package name '$name' in the source lock."
        }
        foreach ($requiredProperty in @(
            'PortVersion', 'SourcePackage', 'SourceRevision',
            'Archive', 'ArchiveSHA256')) {
            if ([string]::IsNullOrWhiteSpace([string]$package.$requiredProperty)) {
                throw "The vcpkg source lock entry '$name' has no '$requiredProperty'."
            }
        }
        if ($package.ArchiveSHA256 -notmatch '^[0-9a-f]{64}$') {
            throw "The vcpkg source lock entry '$name' has an invalid SHA-256 value."
        }

        $archiveName = [string]$package.Archive
        if ([IO.Path]::GetFileName($archiveName) -ne $archiveName) {
            throw "Unsafe vcpkg source archive name '$archiveName'."
        }
        $archivePath = Join-Path $resolvedDownloads $archiveName
        $archiveHash = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($archiveHash -ne ([string]$package.ArchiveSHA256).ToLowerInvariant()) {
            throw "The source archive for '$name' does not match the source lock."
        }

        $portRoot = Join-Path $resolvedVcpkg "ports/$name"
        $metadataRoot = Join-Path $tripletInstalledRoot "share/$name"
        $abiInfoPath = Join-Path $metadataRoot "vcpkg_abi_info.txt"
        $spdxPath = Join-Path $metadataRoot "vcpkg.spdx.json"
        $copyrightPath = Join-Path $metadataRoot "copyright"
        foreach ($requiredPath in @($portRoot, $metadataRoot)) {
            if (-not (Test-Path -LiteralPath $requiredPath -PathType Container)) {
                throw "The installed vcpkg evidence for '$name' is incomplete: '$requiredPath'."
            }
        }

        $abiHashes = @{}
        foreach ($line in Get-Content -LiteralPath $abiInfoPath) {
            $match = [regex]::Match($line, '^([^\s]+)\s+([0-9a-f]{64})$')
            if ($match.Success) {
                $abiHashes[$match.Groups[1].Value] = $match.Groups[2].Value
            }
        }
        $portPrefix = $portRoot.TrimEnd(
            [IO.Path]::DirectorySeparatorChar,
            [IO.Path]::AltDirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
        $portFiles = @(Get-ChildItem -LiteralPath $portRoot -Recurse -File)
        if ($portFiles.Count -eq 0) {
            throw "The vcpkg port recipe for '$name' contains no files."
        }
        foreach ($portFile in $portFiles) {
            $relativePortPath = $portFile.FullName.Substring($portPrefix.Length).Replace('\', '/')
            $abiKey = if ($abiHashes.ContainsKey($relativePortPath)) {
                $relativePortPath
            } else {
                $portFile.Name
            }
            $portHash = (Get-FileHash -LiteralPath $portFile.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
            if (-not $abiHashes.ContainsKey($abiKey) -or $abiHashes[$abiKey] -ne $portHash) {
                throw "The vcpkg port input '$name/$relativePortPath' does not match installed ABI evidence."
            }
            Add-BundleInput $portFile.FullName "third-party/vcpkg/ports/$name/$relativePortPath"
        }

        $spdx = [IO.File]::ReadAllText($spdxPath) | ConvertFrom-Json
        $portRecord = @($spdx.packages | Where-Object { $_.name -eq $name }) | Select-Object -First 1
        $binaryRecord = @($spdx.packages | Where-Object { $_.name -eq "$name`:$VcpkgTriplet" }) |
            Select-Object -First 1
        $sourceRecord = @($spdx.packages | Where-Object { $_.name -eq $package.SourcePackage }) |
            Select-Object -First 1
        if (-not $portRecord -or $portRecord.versionInfo -ne $package.PortVersion) {
            throw "Installed vcpkg port version for '$name' does not match the source lock."
        }
        if (-not $binaryRecord -or $binaryRecord.versionInfo -notmatch '^[0-9a-f]{64}$') {
            throw "Installed vcpkg package ABI evidence for '$name' is missing or invalid."
        }
        if (-not $sourceRecord -or
            -not ([string]$sourceRecord.downloadLocation).EndsWith("@$($package.SourceRevision)")) {
            throw "Installed vcpkg source revision for '$name' does not match the source lock."
        }

        Add-BundleInput $archivePath "third-party/vcpkg/sources/$archiveName"
        Add-BundleInput $abiInfoPath "third-party/vcpkg/installed-metadata/$name/vcpkg_abi_info.txt"
        Add-BundleInput $spdxPath "third-party/vcpkg/installed-metadata/$name/vcpkg.spdx.json"
        Add-BundleInput $copyrightPath "third-party/vcpkg/installed-metadata/$name/copyright"
    }

    $duplicatePaths = @($inputs | Group-Object PackagePath | Where-Object Count -ne 1)
    if ($duplicatePaths.Count -ne 0) {
        throw "Duplicate vcpkg source-bundle output path '$($duplicatePaths[0].Name)'."
    }
    return $inputs.ToArray()
}
