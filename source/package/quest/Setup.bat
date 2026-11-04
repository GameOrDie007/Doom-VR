@echo off
rem Doom VR for Quest - Setup. Takes the games from Doom VR's PC version on this PC (or
rem puts the game folder together here with the PC Setup's own search), installs the app on
rem your Quest over USB and copies the games to it. Safe to run again: it copies only what
rem changed and never touches your settings or saved games.
rem
rem Tools and hubs: Setup.bat -Quiet [-GamePath "<Doom VR game folder>"] [-Json]
rem (README.txt, "For tools and hubs"). Exit code 0 done, 1 no games, 2 error, 3 no headset.
rem No parenthesised blocks: a path with "(x86)" in it would end them early.
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0files\setup-quest.ps1" %*
set "RC=%ERRORLEVEL%"
echo %* | findstr /i /c:"-Quiet" /c:"-Json" >nul && exit /b %RC%
echo.
pause
exit /b %RC%
