function Get-FO4CSVersionValue {
    param(
        [Parameter(Mandatory)]
        [Diagnostics.FileVersionInfo]$VersionInfo,
        [Parameter(Mandatory)]
        [string]$RawProperty,
        [Parameter(Mandatory)]
        [string]$TextProperty
    )

    $raw = $VersionInfo.PSObject.Properties[$RawProperty]
    if ($raw -and $null -ne $raw.Value) {
        return [Version]$raw.Value
    }
    $text = [string]$VersionInfo.$TextProperty
    $match = [regex]::Match($text, '(?<!\d)\d+\.\d+\.\d+\.\d+(?!\d)')
    if (-not $match.Success) {
        throw "Could not read a four-part version from '$TextProperty' ('$text')."
    }
    return [Version]$match.Value
}

function Get-FO4CSFileVersion {
    param(
        [Parameter(Mandatory)]
        [string]$LiteralPath
    )

    $info = (Get-Item -LiteralPath $LiteralPath).VersionInfo
    return Get-FO4CSVersionValue $info "FileVersionRaw" "FileVersion"
}
