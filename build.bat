@echo off
REM ---------------------------------------------------------------------------
REM  AI-Movie-Shorts (Windows) - build
REM  Produces build\Release\movie_summary_bot.exe (UI) and movie_summary_cli.exe
REM  Usage:  build.bat          (normal build)
REM          build.bat clean    (delete build\ first)
REM ---------------------------------------------------------------------------
setlocal
cd /d "%~dp0"

where cmake >nul 2>nul
if errorlevel 1 (
  echo [ERROR] CMake not found. Run setup.ps1 first, then open a new terminal.
  exit /b 1
)

if /i "%~1"=="clean" (
  echo Removing build\ ...
  rmdir /s /q build 2>nul
)

REM Prefer Visual Studio (MSVC). Fall back to MinGW gcc + Ninja/Make if no VS is installed.
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "HAVE_VS="
if exist "%VSWHERE%" (
  for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "HAVE_VS=%%i"
)

if defined HAVE_VS (
  echo Using Visual Studio at: %HAVE_VS%
  cmake -S . -B build -A x64
  if errorlevel 1 goto :fail
  cmake --build build --config Release --parallel
  if errorlevel 1 goto :fail
  set "OUT=build\Release"
) else (
  where gcc >nul 2>nul
  if errorlevel 1 (
    echo [ERROR] No C compiler found. Install Visual Studio Build Tools ^(run setup.ps1^) or MinGW-w64.
    exit /b 1
  )
  echo Visual Studio not found - using MinGW gcc
  where ninja >nul 2>nul
  if errorlevel 1 (
    cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
  ) else (
    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
  )
  if errorlevel 1 goto :fail
  cmake --build build --parallel
  if errorlevel 1 goto :fail
  set "OUT=build"
)

echo.
echo ============================================================
echo  Build OK
echo    UI : %OUT%\movie_summary_bot.exe
echo    CLI: %OUT%\movie_summary_cli.exe
echo  Start the app with:  run.bat
echo ============================================================
exit /b 0

:fail
echo.
echo [ERROR] Build failed. See the messages above.
exit /b 1
