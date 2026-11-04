@echo off
rem Doom VR for Quest - Collect logs. With the headset plugged in, puts the game's logs and
rem settings from the headset, Setup's logs and a line on the headset (model, version) in
rem one zip on your desktop, ready to send us. Nothing is sent anywhere by itself.
rem No parenthesised blocks: a path with "(x86)" in it would end them early.
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0files\setup-quest.ps1" -Collect %*
set "RC=%ERRORLEVEL%"
echo %* | findstr /i /c:"-Quiet" /c:"-Json" >nul && exit /b %RC%
echo.
pause
exit /b %RC%
