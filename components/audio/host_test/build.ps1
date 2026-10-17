# Host tests for the speech gate (components/audio/vad.c) on synthetic clips: fan noise, speech bursts,
# clicks, digital silence, the live detector. No board needed.
# Needs the MSVC Build Tools (cl.exe) and a Windows 10/11 SDK; the ESP-IDF build does not see this folder.
#   powershell -NoProfile -ExecutionPolicy Bypass -File components\audio\host_test\build.ps1
#   components\audio\host_test\out\vad_test.exe        -> "N checks, 0 failures", exit code 0
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
$out = Join-Path $here 'out'
New-Item -ItemType Directory -Force $out | Out-Null
& $cl /nologo /std:c11 /utf-8 /W3 /Od /Zi "/I$here\stubs" "/I$repo\components\audio" "$here\t_vad.c" `
    "$repo\components\audio\vad.c" /Fo"$out\\" /Fd"$out\\" /Fe"$out\vad_test.exe" /link /nologo
exit $LASTEXITCODE
