@echo off
rem Doom VR for Quest - Uninstall. Copies your settings and saved games from the headset
rem onto this PC (Documents\Doom VR\Quest saves), then removes the app and the game files
rem it was given.
rem No parenthesised blocks: a path with "(x86)" in it would end them early.
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0files\setup-quest.ps1" -Uninstall %*
set "RC=%ERRORLEVEL%"
echo %* | findstr /i /c:"-Quiet" /c:"-Json" >nul && exit /b %RC%
echo.
pause
exit /b %RC%
