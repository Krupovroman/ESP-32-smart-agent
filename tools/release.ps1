# Tags a milestone and archives everything needed to flash it again without building.
#   .\tools\release.ps1 -Tag m3
#   .\tools\release.ps1 -Tag v1.0.0 -Release -Message "first release"
# Requires a clean tree (tracked files). Creates an annotated tag, rebuilds (reconfigure, so the
# image version is the tag), and copies into releases\<tag>\ keeping the build\ layout that
# flash_args refers to: bootloader\bootloader.bin, partition_table\partition-table.bin,
# ota_data_initial.bin, round_assistant.bin/.elf/.map, flash_args, sdkconfig, dependencies.lock,
# SHA256SUMS.txt and manifest.json. Binaries are gitignored; manifest, sums and flash_args are not.
# -DryRun archives the existing build into %TEMP%\ra-release-<tag> without checks, tag or build.
param(
    [Parameter(Mandatory)][string]$Tag,
    [string]$Message = '',
    [switch]$Release,
    [switch]$DryRun
)
. "$PSScriptRoot\idfenv.ps1"
$ErrorActionPreference = 'Stop'
Push-Location $RepoRoot
try {
    if ($Tag -notmatch '^[A-Za-z0-9._-]+$') { throw "bad tag name '$Tag'" }
    if ($DryRun) {
        $dest = Join-Path ([IO.Path]::GetTempPath()) "ra-release-$Tag"
        if (Test-Path $dest) { Remove-Item -Recurse -Force $dest }
    } else {
        $dirty = git status --porcelain --untracked-files=no
        if ($dirty) { throw "the working tree has uncommitted changes:`n$($dirty -join "`n")" }
        if (git tag --list $Tag) { throw "tag $Tag already exists" }
        $dest = Join-Path $RepoRoot "releases\$Tag"
        if (Test-Path $dest) { throw "$dest already exists" }
        if (-not $Message) { $Message = "Release $Tag" }
        Invoke-Checked git tag -a $Tag -m $Message
    }
    $commit = (git rev-parse HEAD).Trim()

    $buildDir = 'build'
    $sdkconfig = Join-Path $RepoRoot 'sdkconfig'
    $idfArgs = @()
    if ($Release) {
        if (-not (Test-Path (Join-Path $RepoRoot 'sdkconfig.defaults.release'))) {
            if (-not $DryRun) { git tag -d $Tag | Out-Null }
            throw 'sdkconfig.defaults.release does not exist yet; build without -Release'
        }
        $buildDir = 'build-rel'
        $sdkconfig = Join-Path $RepoRoot 'build-rel\sdkconfig'
        $idfArgs = @('-B', 'build-rel', '-D', 'SDKCONFIG=build-rel/sdkconfig', '-D', 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.release')
    }
    if (-not $DryRun) {
        # reconfigure: the project version (git describe) is read at configure time
        & "$PSScriptRoot\idf.ps1" @idfArgs reconfigure build
        if ($LASTEXITCODE -ne 0) {
            git tag -d $Tag | Out-Null
            throw "build failed (exit $LASTEXITCODE); tag $Tag removed"
        }
    }
    $build = Join-Path $RepoRoot $buildDir

    # the version string inside the image (esp_app_desc_t at offset 32, version at 48)
    $img = [IO.File]::ReadAllBytes((Join-Path $build 'round_assistant.bin'))
    $magic = [BitConverter]::ToUInt32($img, 32)
    $ver = [Text.Encoding]::ASCII.GetString($img, 48, 32).TrimEnd([char]0)
    # (0xABCD5432 written as a literal would be a negative Int32 in Windows PowerShell)
    if ($magic -ne [Convert]::ToUInt32('ABCD5432', 16)) { Write-Warning 'app descriptor not found in the image' }
    elseif ($ver -ne $Tag) { Write-Warning "image version is '$ver', not '$Tag'" }

    New-Item -ItemType Directory -Force $dest | Out-Null
    $files = New-Object System.Collections.Generic.List[string]
    $flashLines = Get-Content (Join-Path $build 'flash_args')
    $offsets = @{}
    foreach ($l in $flashLines) {
        if ($l -match '^(0x[0-9a-fA-F]+)\s+(\S+)$') { $offsets[$Matches[2]] = $Matches[1]; $files.Add($Matches[2]) }
    }
    foreach ($f in @('flash_args', 'round_assistant.elf', 'round_assistant.map')) { $files.Add($f) }
    foreach ($rel in $files) {
        $src = Join-Path $build $rel
        if (-not (Test-Path $src)) { throw "missing $src" }
        $dst = Join-Path $dest $rel
        New-Item -ItemType Directory -Force (Split-Path $dst -Parent) | Out-Null
        Copy-Item $src $dst
    }
    Copy-Item $sdkconfig (Join-Path $dest 'sdkconfig')
    Copy-Item (Join-Path $RepoRoot 'dependencies.lock') (Join-Path $dest 'dependencies.lock')
    $files.Add('sdkconfig')
    $files.Add('dependencies.lock')

    $sums = @()
    $entries = @()
    foreach ($rel in $files) {
        $p = Join-Path $dest $rel
        $h = (Get-FileHash -Algorithm SHA256 $p).Hash.ToLower()
        $sums += "$h  $($rel -replace '\\', '/')"
        $entry = [ordered]@{ path = ($rel -replace '\\', '/'); bytes = (Get-Item $p).Length; sha256 = $h }
        if ($offsets.ContainsKey($rel)) { $entry.offset = $offsets[$rel] }
        $entries += [pscustomobject]$entry
    }
    $sums | Set-Content -Encoding ascii (Join-Path $dest 'SHA256SUMS.txt')
    $idfVer = [Text.Encoding]::ASCII.GetString($img, 144, 32).TrimEnd([char]0)
    $manifest = [ordered]@{
        tag = $Tag; version = $ver; commit = $commit; created = (Get-Date).ToString('s'); idf = $idfVer
        build = $buildDir; restore = ".\tools\restore.ps1 -Tag $Tag"; files = $entries
    }
    # UTF-8 without BOM, so json.load() and other tools read it as-is
    [IO.File]::WriteAllText((Join-Path $dest 'manifest.json'), ($manifest | ConvertTo-Json -Depth 4),
        (New-Object System.Text.UTF8Encoding $false))

    Write-Host "release $Tag ($ver, $commit) archived in $dest"
    if (-not $DryRun) {
        Write-Host "commit the manifest:  git add releases/$Tag; git commit -m `"releases: $Tag manifest`""
    }
} finally {
    Pop-Location
}
