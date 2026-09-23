#requires -Version 7.2
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '../scripts/flat_performance_metrics.ps1')
. (Join-Path $PSScriptRoot '../scripts/flat_performance_control.ps1')

function Assert-Near($Actual, $Expected, [string]$Message) {
    if ([Math]::Abs($Actual - $Expected) -gt 0.00001) {
        throw "$Message : expected $Expected, got $Actual"
    }
}
function Assert-Throws([scriptblock]$Action, [string]$Message) {
    $threw = $false
    try { & $Action | Out-Null } catch { $threw = $true }
    if (-not $threw) { throw "Expected rejection: $Message" }
}
$testRoot = Join-Path ([IO.Path]::GetTempPath()) ('FO4CS-FpsTests-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testRoot | Out-Null
try {
    $path = Join-Path $testRoot 'frames.csv'
    $rows = [Collections.Generic.List[object]]::new()
    $time = 0.0
    for ($i=0; $i -le 1000; ++$i) {
        $duration = if ($i -eq 1000) { 1000.0 } else { 10.0 }
        if ($i -gt 0) { $time += $duration / 1000 }
        $rows.Add([pscustomobject]@{
            Application='Fallout4.exe'; ProcessID='123'; SwapChainAddress='0xABC'
            TimeInSeconds=$time.ToString('F6', [Globalization.CultureInfo]::InvariantCulture)
            msBetweenPresents=$duration.ToString([Globalization.CultureInfo]::InvariantCulture)
            Dropped= $(if ($i -eq 900) { '1' } else { '0' })
            SyncInterval='0'; msGPUActive='4.5'
        })
    }
    $rows | Export-Csv -LiteralPath $path -NoTypeInformation
    $stats = Read-FlatFrameCapture $path 123 -MinimumSeconds 10 -MinimumFrames 1000
    Assert-Near $stats.average_fps (1000000.0 / 10990) 'Average uses total frame time'
    Assert-Near $stats.one_percent_low_fps (10000.0 / 1090) '1% low includes the one-second hitch'
    Assert-Near $stats.p99_frame_ms 10 'P99 is separate from slowest-1% mean'
    Assert-Near $stats.max_frame_ms 1000 'Hitches must not be filtered'
    Assert-Near $stats.dropped_frames 1 'Dropped application frame is retained'
    Assert-Near $stats.frames 1000 'Only the first unpaired interval is excluded'
    Assert-Near $stats.mean_gpu_active_ms 4.5 'GPU active time'

    Assert-Throws { Read-FlatFrameCapture $path 456 -MinimumSeconds 1 } 'wrong process'
    Assert-Throws { Read-FlatFrameCapture $path 123 -MinimumSeconds 60 } 'short capture'
    $rows[500].SwapChainAddress='0xDEF'
    $rows | Export-Csv -LiteralPath $path -NoTypeInformation
    $singleSecondary = Read-FlatFrameCapture $path 123 -MinimumSeconds 1
    Assert-Near $singleSecondary.other_swap_chain_rows 1 'Tiny secondary stream is reported'
    for ($i=100; $i -lt 200; ++$i) { $rows[$i].SwapChainAddress='0xDEF' }
    $rows | Export-Csv -LiteralPath $path -NoTypeInformation
    Assert-Throws { Read-FlatFrameCapture $path 123 -MinimumSeconds 1 } 'substantial second swap chain'
    foreach ($row in $rows) { $row.SwapChainAddress='0xABC'; $row.msGPUActive='NA'; $row.SyncInterval='1' }
    $rows | Export-Csv -LiteralPath $path -NoTypeInformation
    $withoutGpu = Read-FlatFrameCapture $path 123 -MinimumSeconds 1
    if ($null -ne $withoutGpu.mean_gpu_active_ms) { throw 'Missing GPU data was fabricated.' }
    if ($withoutGpu.warnings.Count -lt 2) { throw 'Expected VSync and missing-GPU caveats.' }
    $rows[500].msBetweenPresents='NaN'
    $rows | Export-Csv -LiteralPath $path -NoTypeInformation
    Assert-Throws { Read-FlatFrameCapture $path 123 -MinimumSeconds 1 } 'NaN interval'
    $rows[500].msBetweenPresents='0'
    $rows | Export-Csv -LiteralPath $path -NoTypeInformation
    Assert-Throws { Read-FlatFrameCapture $path 123 -MinimumSeconds 1 } 'zero internal interval'
    $rows[500].msBetweenPresents='10'
    $rows[500].TimeInSeconds='0'
    $rows | Export-Csv -LiteralPath $path -NoTypeInformation
    Assert-Throws { Read-FlatFrameCapture $path 123 -MinimumSeconds 1 } 'nonmonotonic timestamp'

    $runs = @()
    foreach ($state in @('OFF','ON','ON','OFF')) {
        $fps = if ($state -eq 'OFF') { 100.0 } else { 90.0 }
        $gpu = if ($state -eq 'OFF') { 8.0 } else { 9.0 }
        $runs += [pscustomobject]@{ state=$state; metrics=[pscustomobject]@{
            average_fps=$fps; mean_frame_ms=1000.0/$fps; mean_gpu_active_ms=$gpu } }
    }
    $comparison = Compare-FlatFrameCaptures $runs
    Assert-Near $comparison.fps_loss 10 'FPS delta'
    Assert-Near $comparison.fps_loss_percent 10 'FPS percentage'
    Assert-Near $comparison.added_mean_frame_ms (1000.0/90-10) 'Frame time delta'
    Assert-Near $comparison.added_mean_gpu_active_ms 1 'GPU active delta'
    if ($comparison.repeats_disagree_on_direction) { throw 'Stable repeat pairs marked inconsistent.' }
    $runs[2].metrics.average_fps = 101
    $comparison = Compare-FlatFrameCaptures $runs
    if (-not $comparison.repeats_disagree_on_direction) { throw 'Opposing repeat deltas were not flagged.' }
    Assert-Throws { Compare-FlatFrameCaptures @($runs[0],$runs[1]) } 'one pair is insufficient'

    $settingsPath = Join-Path $testRoot 'CloudShadows.json'
    $originalSettings = [Text.Encoding]::UTF8.GetBytes("{`r`n  `"ProjectionModelVersion`": 4, `"Enabled`": true, `"Opacity`": 2.0, `"CloudHeight`": 10000.0`r`n}`r`n")
    [IO.File]::WriteAllBytes($settingsPath, $originalSettings)
    $control = New-CloudSettingsControl $settingsPath $testRoot
    [void](Set-CloudBenchmarkState $control $false)
    $changed = Get-Content $settingsPath -Raw | ConvertFrom-Json
    if ($changed.Enabled -ne $false -or $changed.CloudHeight -ne 10000 -or $changed.Opacity -ne 2) {
        throw 'Explicit OFF changed physical shadow settings.'
    }
    [void](Set-CloudBenchmarkState $control $false) # idempotent, never toggles back ON
    if ((Get-Content $settingsPath -Raw | ConvertFrom-Json).Enabled) { throw 'OFF was not idempotent.' }
    Restore-CloudBenchmarkSettings $control
    if ((Get-CloudSettingsHash ([IO.File]::ReadAllBytes($settingsPath))) -ne (Get-CloudSettingsHash $originalSettings)) {
        throw 'Cancellation did not restore original bytes.'
    }
    [IO.File]::WriteAllText($settingsPath, '{"Enabled":true,"Opacity":1.23}')
    Assert-Throws { Restore-CloudBenchmarkSettings $control } 'preserve outside settings edits'
    [IO.File]::WriteAllBytes($settingsPath, $originalSettings)
    # Interrupt a real child using the production settings controller, then
    # recover with the production HUD code in an independent process.
    $childScript = Join-Path $testRoot 'interrupted-worker.ps1'
    [IO.File]::WriteAllText($childScript, @'
param($Module, $Settings, $Recovery)
$ErrorActionPreference = 'Stop'
. $Module
$control = New-CloudSettingsControl $Settings $Recovery
[void](Set-CloudBenchmarkState $control $false)
[Environment]::Exit(87)
'@)
    $info = [Diagnostics.ProcessStartInfo]::new((Get-Command pwsh.exe).Source)
    $info.UseShellExecute = $false; $info.CreateNoWindow = $true
    foreach ($argument in @('-NoProfile','-File',$childScript,'-Module',
            (Join-Path $PSScriptRoot '../scripts/flat_performance_control.ps1'),
            '-Settings',$settingsPath,'-Recovery',$testRoot)) { $info.ArgumentList.Add($argument) }
    $child = [Diagnostics.Process]::Start($info)
    if (-not $child.WaitForExit(10000) -or $child.ExitCode -ne 87) { throw 'Interruption fixture failed.' }
    $child.Dispose()
    if ((Get-Content $settingsPath -Raw | ConvertFrom-Json).Enabled) { throw 'Interrupted fixture did not leave OFF to recover.' }
    & "$env:WINDIR/System32/WindowsPowerShell/v1.0/powershell.exe" -NoProfile -STA -File `
        (Join-Path $PSScriptRoot '../scripts/start_flat_performance_overlay.ps1') -SelfTest `
        -RecoveryDirectory $testRoot -RecoverySettingsPath $settingsPath
    if ($LASTEXITCODE -ne 0 -or (Get-CloudSettingsHash ([IO.File]::ReadAllBytes($settingsPath))) -ne $control.original_hash) {
        throw 'HUD did not restore the interrupted worker settings.'
    }

    foreach ($iniEncoding in @([Text.Encoding]::UTF8, [Text.Encoding]::Unicode)) {
        $iniPath = Join-Path $testRoot 'mcm.ini'
        $iniText = "[Other]`r`nbEnabled=0`r`n[CloudShadows]`r`nbEnabled=1`r`nfOpacity=2.0`r`nfCloudHeight=10000`r`n"
        $iniOriginal = [byte[]]($iniEncoding.GetPreamble() + $iniEncoding.GetBytes($iniText))
        [IO.File]::WriteAllBytes($iniPath, $iniOriginal)
        $iniControl = New-CloudSettingsControl $iniPath $testRoot
        [void](Set-CloudBenchmarkState $iniControl $false)
        $iniChanged = [IO.File]::ReadAllText($iniPath)
        if ($iniChanged -ne $iniText.Replace("[CloudShadows]`r`nbEnabled=1", "[CloudShadows]`r`nbEnabled=0")) {
            throw 'MCM benchmark changed settings other than the cloud master switch.'
        }
        Restore-CloudBenchmarkSettings $iniControl
        if ((Get-CloudSettingsHash ([IO.File]::ReadAllBytes($iniPath))) -ne (Get-CloudSettingsHash $iniOriginal)) {
            throw 'MCM benchmark recovery did not preserve exact bytes.'
        }
    }

    $runner = Join-Path $PSScriptRoot '../scripts/measure_flat_performance.ps1'
    $tokens = $null
    $errors = $null
    [void][Management.Automation.Language.Parser]::ParseFile($runner, [ref]$tokens, [ref]$errors)
    if ($errors.Count -ne 0) { throw ($errors | Out-String) }
    # Compile the actual native input helper without calling SendInput or
    # interacting with any window. This also checks the x64 structure code.
    $text = [IO.File]::ReadAllText($runner)
    $nativeSource = [regex]::Match($text, "(?s)Add-Type -TypeDefinition @'\r?\n(.*?)\r?\n'@").Groups[1].Value
    if (-not $nativeSource) { throw 'Native helper not found.' }
    Add-Type -TypeDefinition $nativeSource
    Write-Host 'Flat performance tests passed (metrics, stream validation, explicit state, cancellation, interrupted-worker HUD recovery, runner parse/native compile).'
} finally {
    # Only direct files in the unique test directory; no recursive deletion.
    Get-ChildItem -LiteralPath $testRoot -File | ForEach-Object { Remove-Item -LiteralPath $_.FullName -Force }
    Remove-Item -LiteralPath $testRoot -Force
}
