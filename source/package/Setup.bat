@echo off
rem Doom VR: Setup.  Makes a VR folder for Doom VR (in your DOOM + DOOM II folder,
rem or in Documents), finds every Doom-family game you own (Steam, GOG or any
rem folder you drop on this file), copies their game files in, fetches the free
rem games and mods, and offers a desktop shortcut and your Steam library.
rem Safe to run again: it never touches your settings or saved games.
rem
rem Tools and hubs: Setup.bat -Quiet [-GamePath "<folder>"] [-Json]
rem Exit code 0 done, 1 nothing installed, 2 error.
rem No parenthesised blocks: a path with "(x86)" in it would end them early.
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0files\setup.ps1" %*
set "RC=%ERRORLEVEL%"
echo %* | findstr /i /c:"-Quiet" /c:"-Json" >nul && exit /b %RC%
echo.
pause
exit /b %RC%
