@echo off
setlocal
rem DaedalCAD — Visual Studio 2022 + Qt 6.8 MSVC 64-bit
if "%QTDIR%"=="" set QTDIR=C:\Qt\6.8.3\msvc2022_64
if not exist "%QTDIR%\lib\cmake\Qt6\Qt6Config.cmake" (
  echo Set QTDIR to your Qt MSVC 64-bit kit.
  echo Example:  set QTDIR=C:\Qt\6.8.3\msvc2022_64
  exit /b 1
)
where cmake >nul 2>&1
if errorlevel 1 (
  echo CMake was not found. Open "Developer PowerShell for VS 2022" and run this again.
  exit /b 1
)
rem Configure only the first time (or with "build-windows.bat reconfigure"). After that the build
rem itself configures again when CMakeLists.txt changes, so the messages do not come every time.
if /i "%~1"=="reconfigure" if exist "%~dp0build\CMakeCache.txt" del "%~dp0build\CMakeCache.txt"
if not exist "%~dp0build\CMakeCache.txt" (
  cmake -S "%~dp0." -B "%~dp0build" -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH="%QTDIR%"
  if errorlevel 1 exit /b 1
)
cmake --build "%~dp0build" --config Release
if errorlevel 1 exit /b 1
"%QTDIR%\bin\windeployqt.exe" --release "%~dp0build\Release\DaedalCAD.exe"
echo.
echo Built %~dp0build\Release\DaedalCAD.exe
endlocal
