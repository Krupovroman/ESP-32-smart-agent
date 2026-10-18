# Host tests for the app state machine (main/app): app.c and app_io.c on recording mocks of the UI, AI,
# TTS, audio, settings, board and net; the speech gate is the real components/audio/vad.c. No board needed.
# Needs the MSVC Build Tools (cl.exe) and a Windows 10/11 SDK; the ESP-IDF build does not see this folder.
#   powershell -NoProfile -ExecutionPolicy Bypass -File main\app\host_test\build.ps1
#   main\app\host_test\out\app_test.exe        -> "N checks, 0 failures", exit code 0
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
$inc = @("/I$here\stubs", "/I$here", "/I$repo\main\app", "/I$repo\main\ui", "/I$repo\components\ai",
         "/I$repo\components\orhttp", "/I$repo\components\audio", "/I$repo\components\board",
         "/I$repo\components\devconsole", "/I$repo\components\settings", "/I$repo\components\net")
$src = @("$here\t_app.c", "$here\mocks.c", "$repo\components\audio\vad.c")
& $cl /nologo /std:c11 /utf-8 /W3 /wd4996 /wd4244 /wd4267 /wd4018 /Od /Zi /FI"$here\stubs\host_shim.h" `
    @inc @src /Fo"$out\\" /Fd"$out\\" /Fe"$out\app_test.exe" /link /nologo
exit $LASTEXITCODE
