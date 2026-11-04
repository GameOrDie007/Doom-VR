@echo off
rem Doom VR on a Steam Frame - Collect logs. Over the home network, puts the game's logs and
rem settings from the Frame, Lepton's own launch logs, Setup's logs and the library entry's
rem log in one zip on your desktop, ready to send us. Nothing is sent anywhere by itself.
rem No parenthesised blocks: a path with "(x86)" in it would end them early.
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0files\setup-quest.ps1" -SteamFrame -Collect %*
set "RC=%ERRORLEVEL%"
echo %* | findstr /i /c:"-Quiet" /c:"-Json" >nul && exit /b %RC%
echo.
pause
exit /b %RC%
