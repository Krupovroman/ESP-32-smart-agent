# Host tests of the battery model (components/board/battery.c, docs/ui.md 3.16): battery.c on a simulated cell,
# charger and USB host (t_battery.c); the ADC, USB-Serial-JTAG, clock and FreeRTOS are stand-ins (stubs\).
# Needs the MSVC Build Tools (cl.exe) and a Windows 10/11 SDK; the ESP-IDF build does not see this folder.
#   powershell -NoProfile -ExecutionPolicy Bypass -File components\board\host_test\build.ps1
#   components\board\host_test\out\battery_test.exe [-v [-t]] [name]   -> "N checks, 0 failures", exit code 0
#   (-v: the model's log, -t: every reading too; name: only the tests whose name contains it)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$comp = (Resolve-Path (Join-Path $here '..')).Path
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
$out = Join-Path $here 'out'
New-Item -ItemType Directory -Force $out | Out-Null
# stubs first, so the IDF headers resolve to the stand-ins; t_battery.c includes battery.c to reach its statics
$inc = @("/I$here\stubs", "/I$here", "/I$comp")
$src = @("$here\t_battery.c")
& $cl /nologo /std:c11 /utf-8 /W3 /wd4996 /wd4244 /wd4267 /wd4018 /Od /Zi /D_CRT_SECURE_NO_WARNINGS `
    @inc @src /Fo"$out\\" /Fd"$out\\" /Fe"$out\battery_test.exe" /link /nologo
exit $LASTEXITCODE
