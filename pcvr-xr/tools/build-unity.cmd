@echo off
rem Builds the Unity bridge (pcvrxr_unity.dll) for both architectures, static CRT.
rem   tools\build-unity.cmd
rem Output: build-unity-x64\Release and build-unity-x86\Release
setlocal
set "ROOT=%~dp0.."
set "PATH=C:\Program Files (x86)\Microsoft Visual Studio\Installer;%PATH%"
set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
set "CMAKE=%ROOT%\..\tools\cmake-3.28.6\bin\cmake.exe"
if not exist "%CMAKE%" set "CMAKE=cmake"

call "%VCVARS%" x64 >nul || exit /b 1
"%CMAKE%" -S "%ROOT%" -B "%ROOT%\build-unity-x64" -A x64 -DPCVRXR_UNITY=ON -DPCVRXR_STATIC_CRT=ON >nul || exit /b 1
"%CMAKE%" --build "%ROOT%\build-unity-x64" --config Release --target pcvrxr_unity || exit /b 1
"%CMAKE%" -S "%ROOT%" -B "%ROOT%\build-unity-x86" -A Win32 -DPCVRXR_UNITY=ON -DPCVRXR_STATIC_CRT=ON >nul || exit /b 1
"%CMAKE%" --build "%ROOT%\build-unity-x86" --config Release --target pcvrxr_unity || exit /b 1
echo built both
