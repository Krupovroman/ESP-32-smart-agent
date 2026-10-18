# Flashes the current build over USB, waits for the board to boot and prints "info".
#   .\tools\flash.ps1                 # bootloader + partition table + otadata + ota_0
#   .\tools\flash.ps1 -AppOnly        # ota_0 only (no effect while otadata selects ota_1)
#   .\tools\flash.ps1 -Storage        # the asset pack build\storage.bin (CJK font, 5 MB) at 0x820000; once, and
#                                     # again only when the font changes (idf.py storage-flash)
#   .\tools\flash.ps1 -Port COM9 -NoWait
param(
    [string]$Port = $(if ($env:RA_PORT) { $env:RA_PORT } else { 'COM7' }),
    [switch]$AppOnly,
    [switch]$Storage,
    [switch]$NoWait
)
. "$PSScriptRoot\idfenv.ps1"

$target = if ($Storage) { 'storage-flash' } elseif ($AppOnly) { 'app-flash' } else { 'flash' }
& "$PSScriptRoot\idf.ps1" -p $Port $target
if ($LASTEXITCODE -ne 0) { Write-Host "flash failed (exit $LASTEXITCODE). Close any monitor/dev.py on $Port, or use download mode: hold BOOT, tap RESET, release BOOT."; exit $LASTEXITCODE }
if ($AppOnly -and -not $Storage) { Write-Host "note: app-flash writes ota_0 only; if 'info' shows part=ota_1, run .\tools\slot.ps1 -Boot 0 or a full flash." }
if ($NoWait) { exit 0 }

& $IdfPython "$PSScriptRoot\dev.py" --port $Port --no-logs wait-ready 15
if ($LASTEXITCODE -ne 0) { Write-Host "board did not report READY within 15 s"; exit $LASTEXITCODE }
& $IdfPython "$PSScriptRoot\dev.py" --port $Port --no-logs info
exit $LASTEXITCODE
