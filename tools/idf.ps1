# Runs idf.py inside the ESP-IDF v6.1 environment installed by EIM.
#   .\tools\idf.ps1 build
#   .\tools\idf.ps1 -p COM7 flash monitor
# Rebuild the toolchain on a new PC with:  eim install --config eim_config.toml
$ErrorActionPreference = 'Stop'
$activate = 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1'
if (-not (Test-Path $activate)) { throw "ESP-IDF v6.1 not found ($activate). Install it with: eim install --config eim_config.toml" }
. $activate *> $null
# idf.py prints notes on stderr; in Windows PowerShell 5.1 that must not be treated as fatal
$ErrorActionPreference = 'Continue'
Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue
Push-Location (Split-Path $PSScriptRoot -Parent)
try {
    idf.py @args
    exit $LASTEXITCODE
} finally {
    Pop-Location
}
