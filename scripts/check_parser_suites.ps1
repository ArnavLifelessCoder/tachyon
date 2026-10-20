param(
    [ValidateRange(1, 10)]
    [int]$Runs = 3
)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path $PSScriptRoot -Parent
Set-Location -LiteralPath $taskRoot
$reviewDirectory = Join-Path $taskRoot '.local/review'
New-Item -ItemType Directory -Force $reviewDirectory | Out-Null
$records = @()
$head = (& git rev-parse HEAD).Trim()
$stamp = (Get-Date).ToString('yyyyMMdd-HHmmss')
foreach ($preset in @('debug', 'asan')) {
    for ($run = 1; $run -le $Runs; $run++) {
        $logName = "m2-validation-$stamp-$preset-$run.txt"
        $log = Join-Path $reviewDirectory $logName
        $clock = [System.Diagnostics.Stopwatch]::StartNew()
        $failure = $null
        try {
            & (Join-Path $PSScriptRoot 'test.ps1') -Preset $preset -SkipBuild:($run -gt 1) *> $log
        } catch {
            $failure = $_.Exception.Message
        }
        $clock.Stop()
        $text = Get-Content -LiteralPath $log -Raw
        $match = [regex]::Match($text, 'Total Test time \(real\) =\s*([0-9.]+) sec')
        $seconds = if ($match.Success) {
            [double]::Parse($match.Groups[1].Value, [cultureinfo]::InvariantCulture)
        } else { $null }
        $passed = -not $failure -and $text -match '100% tests passed, 0 tests failed out of \d+'
        $records += [ordered]@{
            preset = $preset
            run = $run
            passed = $passed
            ctest_seconds = $seconds
            configure_build_test_seconds = $clock.Elapsed.TotalSeconds
            failure = $failure
            log = ".local/review/$logName"
        }
        [ordered]@{
            tested_commit = $head
            platform = [System.Runtime.InteropServices.RuntimeInformation]::OSDescription
            requested_runs_per_preset = $Runs
            stop_on_first_failure = $true
            runs = $records
        } | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (
            Join-Path $reviewDirectory 'windows-suite-runs.json'
        ) -Encoding utf8
        Write-Output "$preset run $run : passed=$passed ctest_seconds=$seconds"
        if (-not $passed) { throw "Acceptance stopped; inspect $log" }
    }
}
