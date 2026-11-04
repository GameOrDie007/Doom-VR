@echo off
rem Doom VR on a Steam Frame - Uninstall. Copies your settings and saves from the Frame onto
rem this PC (Documents\Doom VR\Steam Frame saves), then removes the library entry and our files.
rem No parenthesised blocks: a path with "(x86)" in it would end them early.
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0files\setup-quest.ps1" -SteamFrame -Uninstall %*
set "RC=%ERRORLEVEL%"
echo %* | findstr /i /c:"-Quiet" /c:"-Json" >nul && exit /b %RC%
echo.
pause
exit /b %RC%
