# Runs the ESP-IDF venv Python (pyserial 3.5, esptool) without activating the whole environment.
#   .\tools\py.ps1 tools\dev.py --port COM7 log 20
$py = 'C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe'
if (-not (Test-Path $py)) { throw "IDF venv python not found ($py)" }
& $py @args
exit $LASTEXITCODE
