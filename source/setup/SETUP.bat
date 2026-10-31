@echo off
rem DoomVR setup. Finds your Doom-family games on Steam and GOG - and in any
rem folder you drop onto this file - copies their game files in, downloads
rem the free games and mods, makes the cover art, writes a launcher for each
rem game, and then starts DoomVR. Safe to run again: it only ever adds.
pushd "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0setup\setup.ps1" %*
popd
if exist "%~dp0DoomVR.bat" goto askstart
pause
exit /b

:askstart
echo.
choice /c YN /t 15 /d Y /m "Start DoomVR now (starts by itself in 15 seconds)"
if errorlevel 2 exit /b
call "%~dp0DoomVR.bat"
