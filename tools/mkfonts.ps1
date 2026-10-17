# Regenerates the LVGL fonts in main/ui/fonts/ from the TTFs in assets/fonts/ (docs/ui.md section 1.3).
#   .\tools\mkfonts.ps1              # uses the committed TTFs
#   .\tools\mkfonts.ps1 -Download    # first re-fetches the TTFs from the official releases (hash-checked)
# Needs Node.js; lv_font_conv is run through npx at the version pinned in tools/package.json.
param([switch]$Download)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$src = Join-Path $root 'assets\fonts'
$out = Join-Path $root 'main\ui\fonts'
New-Item -ItemType Directory -Force $out | Out-Null

if ($Download) {
    $tmp = Join-Path ([IO.Path]::GetTempPath()) ("mkfonts_" + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Force $tmp | Out-Null
    $inter = @{ url = 'https://github.com/rsms/inter/releases/download/v4.1/Inter-4.1.zip'
                sha = '9883fdd4a49d4fb66bd8177ba6625ef9a64aa45899767dde3d36aa425756b11e' }
    $fa = @{ url = 'https://github.com/FortAwesome/Font-Awesome/releases/download/5.15.4/fontawesome-free-5.15.4-web.zip'
             sha = 'b9554077f70ac13c2e687e30d0fef15d5b64852b04b2a87fdb35f3edec395ae7' }
    foreach ($z in @($inter, $fa)) {
        $file = Join-Path $tmp (Split-Path $z.url -Leaf)
        Invoke-WebRequest -UseBasicParsing -Uri $z.url -OutFile $file
        $h = (Get-FileHash -Algorithm SHA256 $file).Hash.ToLower()
        if ($h -ne $z.sha) { throw "hash mismatch for $($z.url): $h" }
        Expand-Archive -Force $file (Join-Path $tmp ([IO.Path]::GetFileNameWithoutExtension($file)))
    }
    Copy-Item -Force (Join-Path $tmp 'Inter-4.1\extras\ttf\Inter-Medium.ttf') $src
    Copy-Item -Force (Join-Path $tmp 'Inter-4.1\extras\ttf\Inter-SemiBold.ttf') $src
    Copy-Item -Force (Join-Path $tmp 'Inter-4.1\LICENSE.txt') (Join-Path $src 'Inter-OFL.txt')
    $faDir = Join-Path $tmp 'fontawesome-free-5.15.4-web\fontawesome-free-5.15.4-web'
    Copy-Item -Force (Join-Path $faDir 'webfonts\fa-solid-900.ttf') $src
    Copy-Item -Force (Join-Path $faDir 'LICENSE.txt') (Join-Path $src 'FontAwesome-LICENSE.txt')
    Remove-Item -Recurse -Force $tmp
}

# Glyph sets (ui.md 1.3). TEXT: Latin-1 + Latin Ext-A, Romanian, whole Cyrillic block, punctuation, currency, arrows.
$R_TEXT  = '0x20-0x7E,0xA0-0x17F,0x218-0x21B,0x400-0x4FF,0x2010-0x2027,0x2030,0x2039-0x203A,0x20AC,0x20B4,0x20B8,0x20BD,0x2116,0x2122,0x2190-0x2193,0x2212'
$R_TITLE = '0x20-0x7E,0xA0-0xFF,0x400-0x45F,0x490-0x491,0x2013-0x2014,0x2018-0x201E,0x2022,0x2026,0x2116'
$R_DIG   = '0x20,0x25,0x2B-0x3A,0x2212'
# Font Awesome 5 Free Solid icons merged into the text fonts (ui_sym.h names them).
$I_FULL = '0xF00C,0xF00D,0xF011,0xF013,0xF023,0xF028,0xF029,0xF04B,0xF04D,0xF053,0xF054,0xF05A,0xF06E,0xF070,0xF071,0xF078,0xF084,0xF0AC,0xF0E7,0xF11C,0xF130,0xF131,0xF185,0xF1AB,0xF1DA,0xF1EB,0xF1F8,0xF240-0xF244,0xF2F1,0xF362,0xF3CD,0xF4AD,0xF544,0xF55A,0xF6A9'
$I_16   = '0xF00C,0xF023,0xF053,0xF054,0xF071,0xF0E7,0xF1EB,0xF240-0xF244,0xF6A9'
$I_28   = '0xF00C,0xF00D,0xF028,0xF04D,0xF053,0xF071,0xF0AC,0xF130,0xF185,0xF1EB,0xF55A,0xF6A9'

$pkg = Get-Content -Raw (Join-Path $PSScriptRoot 'package.json') | ConvertFrom-Json
$ver = $pkg.devDependencies.lv_font_conv
# Relative paths keep the generated file headers machine-independent
$medium = 'assets/fonts/Inter-Medium.ttf'
$semi = 'assets/fonts/Inter-SemiBold.ttf'
$fa = 'assets/fonts/fa-solid-900.ttf'

function Invoke-FontConv([string]$name, [int]$size, [string]$ttf, [string]$ranges, [string]$icons) {
    $a = @('-y', "lv_font_conv@$ver", '--bpp', '4', '--size', "$size", '--no-compress', '--format', 'lvgl',
           '--lv-include', 'lvgl.h', '--lv-font-name', $name, '--font', $ttf, '-r', $ranges)
    if ($icons) { $a += @('--font', $fa, '-r', $icons) }
    $a += @('-o', "main/ui/fonts/$name.c")
    Write-Host "lv_font_conv $name ($size px)"
    # npm prints notices on stderr; Windows PowerShell 5.1 must not treat that as fatal
    $ErrorActionPreference = 'Continue'
    & npx.cmd @a 2>&1 | ForEach-Object { "$_" }
    $rc = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    if ($rc -ne 0) { throw "lv_font_conv failed for $name" }
}

Push-Location $root
try {
    Invoke-FontConv 'ui_16' 16 $medium $R_TEXT $I_16
    Invoke-FontConv 'ui_20' 20 $medium $R_TEXT $I_FULL
    Invoke-FontConv 'ui_28' 28 $semi $R_TITLE $I_28
    Invoke-FontConv 'ui_48' 48 $semi $R_DIG ''
} finally {
    Pop-Location
}

Copy-Item -Force (Join-Path $src 'Inter-OFL.txt') (Join-Path $out 'OFL.txt')
Copy-Item -Force (Join-Path $src 'FontAwesome-LICENSE.txt') (Join-Path $out 'FontAwesome-LICENSE.txt')
Get-ChildItem $out -Filter 'ui_*.c' | ForEach-Object { '{0,-10} {1,9} bytes of source' -f $_.Name, $_.Length }
