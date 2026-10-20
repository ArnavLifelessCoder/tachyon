param(
    [ValidateSet('debug', 'asan', 'release')]
    [string]$Preset = 'debug',
    [switch]$SkipBuild
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
Set-Location -LiteralPath $root
New-Item -ItemType Directory -Force (Join-Path $root '.tmp') | Out-Null
$env:TEMP = Join-Path $root '.tmp'
$env:TMP = $env:TEMP
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    $installation = & $vswhere -latest -version '[17.0,18.0)' -products '*' `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $installation) { throw 'Visual Studio 2022 C++ tools are required.' }
    Import-Module (Join-Path $installation 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
    Enter-VsDevShell -VsInstallPath $installation -SkipAutomaticLocation `
        -DevCmdArguments '-arch=x64 -host_arch=x64'
}
$python = Join-Path $root '.venv/Scripts/python.exe'
if (-not (Test-Path -LiteralPath $python)) {
    throw 'Create .venv and install requirements-dev.txt first.'
}
New-Item -ItemType Directory -Force (Join-Path $root '.local/review') | Out-Null
if (-not $SkipBuild) {
    & cmake --preset $Preset "-DPython3_EXECUTABLE=$python"
    if ($LASTEXITCODE) { throw "Configure failed: $LASTEXITCODE" }
    & cmake --build --preset $Preset
    if ($LASTEXITCODE) { throw "Build failed: $LASTEXITCODE" }
}
$previousAsanOptions = $env:ASAN_OPTIONS
$previousParserTrace = $env:ITCH_PARSE_TRACE
try {
    $env:ITCH_PARSE_TRACE = '1'
    if ($Preset -eq 'asan') {
        # CTest executables run in the build tree; pytest children run at the root.
        # A relative prefix avoids drive-letter parsing in ASAN_OPTIONS.
        New-Item -ItemType Directory -Force (Join-Path $root 'build/asan/.local/review') | Out-Null
        $asanLogPrefix = ".local/review/asan-$((Get-Date).ToString('yyyyMMdd-HHmmss'))"
        $diagnosticOptions = "verbosity=1:log_path=$asanLogPrefix"
        $env:ASAN_OPTIONS = if ($previousAsanOptions) {
            "${previousAsanOptions}:$diagnosticOptions"
        } else {
            $diagnosticOptions
        }
        Write-Output "ASan diagnostics: $asanLogPrefix.<pid> under the root and build/asan"
    }
    & ctest --preset $Preset --verbose 2>&1 |
        Tee-Object -FilePath (Join-Path $root ".local/review/$Preset-tests.txt")
    if ($LASTEXITCODE) { throw "Tests failed: $LASTEXITCODE" }
} finally {
    $env:ASAN_OPTIONS = $previousAsanOptions
    $env:ITCH_PARSE_TRACE = $previousParserTrace
}
