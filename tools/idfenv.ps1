# Dot-source from the other tools\*.ps1 scripts: loads the ESP-IDF v6.1 environment (EIM) quietly
# and sets $IdfPython (venv python with esptool/pyserial) and $IdfPath.
#   . "$PSScriptRoot\idfenv.ps1"
$activate = 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1'
if (-not (Test-Path $activate)) { throw "ESP-IDF v6.1 not found ($activate). Install it with: eim install --config eim_config.toml" }
$ErrorActionPreference = 'Stop'
# note: the profile declares [switch]$e, which leaks into the caller: never name a variable $e
. $activate *> $null
# idf.py and esptool print notes on stderr; in Windows PowerShell 5.1 that must not be fatal
$ErrorActionPreference = 'Continue'
Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue
$IdfPath = $env:IDF_PATH
$IdfPython = if ($env:IDF_PYTHON_ENV_PATH) { Join-Path $env:IDF_PYTHON_ENV_PATH 'Scripts\python.exe' } else { 'C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe' }
if (-not (Test-Path $IdfPython)) { $IdfPython = 'C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe' }
$RepoRoot = Split-Path $PSScriptRoot -Parent

function Invoke-Checked {
    # Runs a native command (first argument) and throws when it fails, so scripts stop at the
    # first error. A simple function on purpose: "-p"-style arguments are passed through as-is.
    $exe = $args[0]
    $rest = @($args | Select-Object -Skip 1)
    & $exe @rest
    if ($LASTEXITCODE -ne 0) { throw "$exe $($rest -join ' ') failed (exit $LASTEXITCODE)" }
}
