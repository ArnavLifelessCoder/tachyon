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
& $python scripts/run_test_policy.py --preset $Preset 2>&1 |
    Tee-Object -FilePath (Join-Path $root ".local/review/$Preset-tests.txt")
if ($LASTEXITCODE) { throw "Test policy failed: $LASTEXITCODE; see .local/review/$Preset-policy-latest.json" }
