# Host test of the CJK fallback fonts (main/ui/ui_font.c, docs/ui.md 1.3): real LVGL 9.5 from managed_components,
# the generated Inter fonts and the storage pack of the last ESP-IDF build. Runs with the pack, without it and with a
# corrupted one, and leaves 2x PNG screenshots in main\ui\host_test\out\ (mixed.png, bubbles.png, reader.png, ...).
# Needs the MSVC Build Tools (cl.exe), a Windows 10/11 SDK and one ESP-IDF build first (managed_components, build\storage.bin).
#   powershell -NoProfile -ExecutionPolicy Bypass -File main\ui\host_test\build.ps1     -> "N checks, 0 failures" x3
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = (Resolve-Path (Join-Path $here '..\..\..')).Path
$vs = @('C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools', 'C:\Program Files\Microsoft Visual Studio\2022\BuildTools',
        'C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools') | Where-Object { Test-Path "$_\VC\Tools\MSVC" } |
      Select-Object -First 1
if (-not $vs) { throw 'MSVC Build Tools not found' }
$vc = (Get-ChildItem "$vs\VC\Tools\MSVC" | Sort-Object Name | Select-Object -Last 1).FullName
$sdk = 'C:\Program Files (x86)\Windows Kits\10'
$sv = (Get-ChildItem "$sdk\Include" | Where-Object { Test-Path "$($_.FullName)\ucrt" } | Sort-Object Name |
       Select-Object -Last 1).Name
$env:INCLUDE = "$vc\include;$sdk\Include\$sv\ucrt;$sdk\Include\$sv\um;$sdk\Include\$sv\shared"
$env:LIB = "$vc\lib\x64;$sdk\Lib\$sv\ucrt\x64;$sdk\Lib\$sv\um\x64"
$cl = "$vc\bin\Hostx64\x64\cl.exe"
$libexe = "$vc\bin\Hostx64\x64\lib.exe"
$lv = "$repo\managed_components\lvgl__lvgl"
if (-not (Test-Path "$lv\lvgl.h")) { throw 'run the ESP-IDF build once first (fetches lvgl/lvgl)' }
$pack = "$repo\build\storage.bin"
if (-not (Test-Path $pack)) { throw 'run the ESP-IDF build once first (makes build\storage.bin)' }
$out = Join-Path $here 'out'
$obj = Join-Path $out 'obj'
New-Item -ItemType Directory -Force $obj | Out-Null
$flags = @('/nologo', '/std:c11', '/experimental:c11atomics', '/utf-8', '/O2', '/W0', '/DLV_CONF_INCLUDE_SIMPLE',
           "/FI$here\stubs\host_shim.h", "/I$here", "/I$here\stubs", "/I$lv", "/I$repo\main\ui",
           "/I$repo\components\assets", "/I$repo\components\devconsole")

# LVGL once (about a minute): every core source except the GPU, OS and display drivers this build does not use
$lvlib = Join-Path $out 'lvgl.lib'
if (-not (Test-Path $lvlib)) {
    $skip = 'vg_lite|thorvg|nema|dave2d|nxp|renesas|stm32|sdl|wayland|x11|evdev|libinput|uefi|opengles|drm|fbdev|nuttx|qnx|glfw|windows'
    $rsp = Join-Path $out 'lvgl_src.rsp'
    Get-ChildItem -Recurse "$lv\src" -Filter *.c | Where-Object { $_.FullName -notmatch $skip } |
        ForEach-Object { '"' + $_.FullName + '"' } | Set-Content -Encoding ascii $rsp
    & $cl @flags /c /MP /Fo"$obj\\" "@$rsp" | Select-String -Pattern ' error ' | Select-Object -First 20
    $orsp = Join-Path $out 'lvgl_obj.rsp'
    Get-ChildItem $obj -Filter *.obj | ForEach-Object { '"' + $_.FullName + '"' } | Set-Content -Encoding ascii $orsp
    & $libexe /nologo /OUT:$lvlib "@$orsp"
    if ($LASTEXITCODE -ne 0) { throw 'lvgl.lib failed' }
}

$src = @("$here\t_font.c", "$repo\main\ui\ui_font.c", "$repo\main\ui\ui_str.c", "$repo\components\assets\assets.c",
         "$repo\main\ui\fonts\ui_16.c", "$repo\main\ui\fonts\ui_20.c", "$repo\main\ui\fonts\ui_28.c",
         "$repo\main\ui\fonts\ui_48.c")
& $cl @flags $src $lvlib /Fo"$out\\" /Fe"$out\t_font.exe" /link /nologo user32.lib
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

[Console]::OutputEncoding = [Text.Encoding]::UTF8
$rc = 0
foreach ($run in @(@($pack), @('none'), @($pack, 'corrupt'))) {
    & "$out\t_font.exe" $run[0] $out @($run | Select-Object -Skip 1)
    if ($LASTEXITCODE -ne 0) { $rc = 1 }
}
exit $rc
