@echo off
rem DaedalCAD: install what is missing, then build. Double-click it, or run it from a command prompt.
rem It runs setup-windows.ps1 (see the top of that file, or docs/install.md). Words you add are
rem passed on:
rem   build-windows.bat               build (installing Git, Visual Studio Build Tools, CMake and Qt if missing)
rem   build-windows.bat step          also OpenCASCADE, for exact STEP (the first time: 30-60 minutes)
rem   build-windows.bat package       also make the release zip and installer
rem   build-windows.bat run           start DaedalCAD when it is built
rem   build-windows.bat reconfigure   start the CMake configuration over
rem   build-windows.bat check         only report what is installed and what is missing
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0setup-windows.ps1" %*
set RESULT=%ERRORLEVEL%
rem Started by double-clicking: keep the window open so the result can be read.
echo %CMDCMDLINE% | find /i "%~nx0" >nul && pause
exit /b %RESULT%
