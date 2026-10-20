# Host regression tests for components/orhttp and components/ai (no board needed).
# esp_http_client is replaced by a scripted mock (mocks.c); FreeRTOS, settings, net and audio are stubs.
# Needs the MSVC Build Tools (cl.exe) and a Windows 10/11 SDK; the ESP-IDF build does not see this folder.
#   powershell -NoProfile -ExecutionPolicy Bypass -File components\ai\host_test\build.ps1
#   components\ai\host_test\out\host_test.exe        -> "N checks, 0 failures", exit code 0
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
$cjson = "$repo\managed_components\espressif__cjson\cJSON"
if (-not (Test-Path "$cjson\cJSON.c")) { throw 'run the ESP-IDF build once first (fetches espressif/cjson)' }
$inc = @("/I$here\stubs", "/I$here", "/I$repo\components\orhttp", "/I$repo\components\ai", "/I$repo\components\settings",
         "/I$repo\components\net", "/I$repo\components\audio", "/I$cjson")
# t_ai.c, t_tts.c and t_models.c include ai_client.c, tts_client.c and ai_models.c to reach their statics
$src = @("$here\main.c", "$here\mocks.c", "$here\t_misc.c", "$here\t_models.c", "$here\t_ai.c", "$here\t_tts.c",
         "$repo\components\orhttp\orconn.c", "$repo\components\orhttp\orerr.c",
         "$repo\components\ai\ai_util.c", "$repo\components\ai\ai_prompts.c", "$repo\components\ai\ai_history.c", "$repo\components\ai\ai_trcheck.c",
         "$repo\components\ai\tts_split.c", "$repo\components\ai\tts_voices.c", "$repo\components\ai\stt_client.c",
         "$cjson\cJSON.c")
& $cl /nologo /std:c11 /utf-8 /W3 /wd4996 /wd4244 /wd4267 /wd4018 /Od /Zi /FI"$here\stubs\host_shim.h" `
    /D_CRT_SECURE_NO_WARNINGS /DCJSON_NESTING_LIMIT=1000 /DCJSON_CIRCULAR_LIMIT=10000 @inc @src `
    /Fo"$out\\" /Fd"$out\\" /Fe"$out\host_test.exe" /link /nologo user32.lib
exit $LASTEXITCODE
