# A/B slot tools over USB (otatool): try a build in the other slot without losing the current one.
#   .\tools\slot.ps1 -Write build\round_assistant.bin -Slot 1 -Boot   # write ota_1 and boot it
#   .\tools\slot.ps1 -Boot 0                                          # back to ota_0
#   .\tools\slot.ps1 -Info                                            # read otadata
# Images written this way are not PENDING_VERIFY: there is no automatic rollback (use dev.py ota push
# to exercise the real OTA path). -Boot erases otadata first, because otatool only rewrites the
# sequence number and a stale ABORTED/INVALID state would make the bootloader ignore the switch.
param(
    [Parameter(Position = 0)][ValidateRange(0, 1)][int]$Slot = -1,
    [string]$Write,
    [switch]$Boot,
    [switch]$Info,
    [string]$Port = $(if ($env:RA_PORT) { $env:RA_PORT } else { 'COM7' })
)
. "$PSScriptRoot\idfenv.ps1"
$ErrorActionPreference = 'Stop'
$ota = Join-Path $IdfPath 'components\app_update\otatool.py'

if ($Info) {
    Invoke-Checked $IdfPython $ota -p $Port read_otadata
    exit 0
}
if (-not $Write -and -not $Boot) { throw 'nothing to do: use -Write <bin> -Slot 0|1 [-Boot], -Boot 0|1 or -Info' }
if ($Slot -lt 0) { throw 'give the slot: -Slot 0|1 (or -Boot 0|1)' }

if ($Write) {
    if (-not (Test-Path $Write)) { throw "missing $Write" }
    if ((Get-Item $Write).Length -gt 0x400000) { throw "$Write is larger than a 4 MB slot" }
    Invoke-Checked $IdfPython $ota -p $Port write_ota_partition --slot $Slot --input (Resolve-Path $Write).Path
    Write-Host "written to ota_$Slot"
}
if ($Boot) {
    Invoke-Checked $IdfPython $ota -p $Port erase_otadata
    Invoke-Checked $IdfPython $ota -p $Port switch_ota_partition --slot $Slot
    Write-Host "next boot: ota_$Slot (check with: python tools\dev.py info)"
}
