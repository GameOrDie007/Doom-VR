@echo off
rem Doom VR on a Steam Frame (standalone, no PC while you play): takes the games from Doom
rem VR's PC version on this PC, then copies the app and the games to the Frame over the home
rem network and puts it in the Frame's own library, under its own name. The Frame needs
rem Developer Mode on (Settings, System) and Lepton installed (Store); this PC pairs with it
rem once (on the Frame: Settings, Developer, Pair new host, then Accept). Safe to run again:
rem it copies only what changed and never touches your settings or saves.
rem Tools and hubs: -Quiet, -Json, -Frame <name or address>. Exit code 0 done, 1 no games,
rem 2 error, 3 no Frame.
rem No parenthesised blocks: a path with "(x86)" in it would end them early.
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0files\setup-quest.ps1" -SteamFrame %*
set "RC=%ERRORLEVEL%"
echo %* | findstr /i /c:"-Quiet" /c:"-Json" >nul && exit /b %RC%
echo.
pause
exit /b %RC%
