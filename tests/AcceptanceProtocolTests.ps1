$ErrorActionPreference = 'Stop'
$scriptPath = Join-Path $PSScriptRoot '..\scripts\run_ingame_acceptance.ps1'
$tokens = $null
$parseErrors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile(
    $scriptPath, [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count) { throw 'Acceptance script has syntax errors.' }
# Load only the protocol readers. Never execute the game/process request body.
foreach ($name in @('Read-BoundedJson', 'Assert-UtcTimestamp', 'Write-AtomicUtf8Json')) {
    $definition = $ast.Find({
        param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
            $node.Name -eq $name
    }, $false)
    if (-not $definition) { throw ('Missing function: ' + $name) }
    . ([scriptblock]::Create($definition.Extent.Text))
}
$testPath = Join-Path ([IO.Path]::GetTempPath()) (
    'FO4CloudShadows.AcceptanceProtocol.' + [guid]::NewGuid().ToString('N') + '.json')
try {
    $valid = '{"ReadyUtc":"2026-09-06T12:35:21.891Z","Ready":true,"ProcessId":29636,"Nested":{"CompletedUtc":"2026-09-06T12:35:22Z"}}'
    [IO.File]::WriteAllText($testPath, $valid, [Text.UTF8Encoding]::new($false))
    $document = Read-BoundedJson -LiteralPath $testPath -MaximumBytes 65536
    if ($document.ReadyUtc -isnot [string] -or
        $document.ReadyUtc -cne '2026-09-06T12:35:21.891Z' -or
        $document.Nested.CompletedUtc -isnot [string] -or
        $document.Ready -isnot [bool] -or
        ($document.ProcessId -isnot [int64] -and $document.ProcessId -isnot [int32])) {
        throw 'Protocol parsing changed timestamp strings or JSON scalar types.'
    }
    Assert-UtcTimestamp $document.ReadyUtc 'ReadyUtc'
    Assert-UtcTimestamp $document.Nested.CompletedUtc 'CompletedUtc'
    Write-AtomicUtf8Json -Destination $testPath -Temporary ($testPath + '.tmp') -Json $valid
    if ([IO.File]::ReadAllText($testPath) -cne $valid -or
        (Test-Path -LiteralPath ($testPath + '.tmp'))) {
        throw 'Atomic replacement did not publish the exact request.'
    }
    foreach ($invalid in @('2026-09-06T12:35:21+00:00', '2026-09-06',
            '2026-99-06T12:35:21Z', 123, $true)) {
        $rejected = $false
        try { Assert-UtcTimestamp $invalid 'ReadyUtc' }
        catch { $rejected = $true }
        if (-not $rejected) { throw ('Invalid timestamp accepted: ' + $invalid) }
    }
    $rejected = $false
    try { Read-BoundedJson -LiteralPath $testPath -MaximumBytes 8 | Out-Null }
    catch { $rejected = $true }
    if (-not $rejected) { throw 'Oversized protocol file accepted.' }
    [IO.File]::WriteAllBytes($testPath, [byte[]]@(0x7B, 0xFF, 0x7D))
    $rejected = $false
    try { Read-BoundedJson -LiteralPath $testPath -MaximumBytes 65536 | Out-Null }
    catch { $rejected = $true }
    if (-not $rejected) { throw 'Invalid UTF-8 protocol file accepted.' }
    Write-Output ('Acceptance protocol checks passed on PowerShell ' + $PSVersionTable.PSVersion)
}
finally {
    # One exact temporary file; no recursive filesystem operation.
    if (Test-Path -LiteralPath $testPath) { Remove-Item -LiteralPath $testPath }
    if (Test-Path -LiteralPath ($testPath + '.tmp')) { Remove-Item -LiteralPath ($testPath + '.tmp') }
}
