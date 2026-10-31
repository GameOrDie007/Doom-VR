@echo off
rem Collect logs: puts DoomVR's logs, settings and a short report about this PC
rem into one zip in this folder, to send with a bug report. Nothing is uploaded,
rem and no game files are copied.
pushd "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0setup\collect-logs.ps1"
popd
echo.
pause
