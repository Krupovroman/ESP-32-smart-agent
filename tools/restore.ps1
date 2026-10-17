# Restores firmware over USB without building.
#   .\tools\restore.ps1 -Tag m2              # flash releases\m2\ (flash_args); NVS (settings) is kept
#   .\tools\restore.ps1 -Tag m2 -EraseNvs    # ... and erase the settings partition afterwards
#   .\tools\restore.ps1 -Factory             # full 16 MB factory image (replaces partitions and NVS)
# If the board does not answer: hold BOOT, tap RESET, release BOOT (ROM download mode), then retry.
param(
    [string]$Tag,
    [switch]$Factory,
    [switch]$EraseNvs,
    [switch]$Yes,
    [string]$Port = $(if ($env:RA_PORT) { $env:RA_PORT } else { 'COM7' })
)
. "$PSScriptRoot\idfenv.ps1"
$ErrorActionPreference = 'Stop'
$FactorySha = '90151CB92E6D224D02B19A6956D8EC9AD5F7ADE0008EE24E97012210F8802AE9'

if ([bool]$Tag -eq [bool]$Factory) { throw 'use exactly one of -Tag <tag> or -Factory' }

if ($Factory) {
    $img = Join-Path $RepoRoot 'backups\factory_full_16MB.bin'
    if (-not (Test-Path $img)) { throw "missing $img (it is gitignored: copy it from the main checkout)" }
    if ((Get-Item $img).Length -ne 16MB) { throw "$img is not 16 MB" }
    Write-Host 'checking SHA-256 of the factory image...'
    $h = (Get-FileHash -Algorithm SHA256 $img).Hash
    if ($h -ne $FactorySha) { throw "factory image hash $h does not match $FactorySha" }
    if (-not $Yes) {
        $answer = Read-Host 'This overwrites the whole flash (partition table, NVS, both slots). Type YES'
        if ($answer -ne 'YES') { throw 'cancelled' }
    }
    Invoke-Checked $IdfPython -m esptool --chip esp32s3 -p $Port write-flash 0 $img
    Write-Host 'factory firmware restored. Back to this project: .\tools\flash.ps1, then push the secrets again.'
    exit 0
}

$dir = Join-Path $RepoRoot "releases\$Tag"
if (-not (Test-Path (Join-Path $dir 'flash_args'))) { throw "no release archive in $dir (see .\tools\release.ps1)" }
$sums = Join-Path $dir 'SHA256SUMS.txt'
if (Test-Path $sums) {
    foreach ($line in Get-Content $sums) {
        if ($line -notmatch '^([0-9a-f]{64})\s+(.+)$') { continue }
        $p = Join-Path $dir $Matches[2]
        if (-not (Test-Path $p)) { throw "missing $p (binaries are not in git: rebuild with git switch --detach $Tag)" }
        if ((Get-FileHash -Algorithm SHA256 $p).Hash.ToLower() -ne $Matches[1]) { throw "checksum mismatch: $p" }
    }
    Write-Host 'checksums OK'
}
Push-Location $dir
try {
    # flash_args holds the flash mode/size line and the offset/file pairs, relative to this folder
    Invoke-Checked $IdfPython -m esptool --chip esp32s3 -p $Port write-flash '@flash_args'
} finally {
    Pop-Location
}
if ($EraseNvs) {
    Invoke-Checked $IdfPython (Join-Path $IdfPath 'components\partition_table\parttool.py') -p $Port erase_partition --partition-name=nvs
    Write-Host 'NVS erased: settings are back to defaults'
}
Write-Host "release $Tag flashed (boots ota_0)."
