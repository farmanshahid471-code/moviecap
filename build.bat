@echo off
REM ---------------------------------------------------------------------------
REM  AI-Movie-Shorts (Windows) - build from source
REM
REM  Produces  build\Release\movie_summary_bot.exe   (desktop UI)
REM            build\Release\movie_summary_web.exe   (browser control panel)
REM            build\Release\movie_summary_cli.exe   (headless)
REM
REM  Usage:  build.bat          (normal build)
REM          build.bat clean    (delete build\ first)
REM
REM  You do NOT need this if you downloaded the ready-made zip - just run
REM  run.bat (or "run.bat web"). This is only for compiling the .exe yourself.
REM
REM  NOTE: no parenthesised IF blocks are used here. A folder name containing
REM  "(" or ")" closes such a block early and cmd aborts the script.
REM ---------------------------------------------------------------------------
setlocal EnableExtensions
cd /d "%~dp0"

where cmake >nul 2>nul
if not errorlevel 1 goto :have_cmake
echo [ERROR] CMake was not found on PATH.
echo         Run setup.ps1 first, then open a NEW terminal and try again.
goto :fail

:have_cmake
if /i not "%~1"=="clean" goto :find_vs
echo Removing build\ ...
rmdir /s /q build 2>nul

:find_vs
REM Prefer Visual Studio (MSVC); fall back to MinGW gcc when it is not installed.
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "HAVE_VS="
if not exist "%VSWHERE%" goto :try_gcc
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "HAVE_VS=%%i"
if not defined HAVE_VS goto :try_gcc

echo Using Visual Studio at:
echo   "%HAVE_VS%"
cmake -S . -B build -A x64
if errorlevel 1 goto :fail
cmake --build build --config Release --parallel
if errorlevel 1 goto :fail
set "OUT=build\Release"
goto :done

:try_gcc
where gcc >nul 2>nul
if not errorlevel 1 goto :have_gcc
echo [ERROR] No C compiler was found.
echo         Install Visual Studio Build Tools (run setup.ps1) or MinGW-w64,
echo         then open a NEW terminal and run build.bat again.
goto :fail

:have_gcc
echo Visual Studio not found - using MinGW gcc
where ninja >nul 2>nul
if errorlevel 1 goto :use_make
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
goto :generated
:use_make
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
:generated
if errorlevel 1 goto :fail
cmake --build build --parallel
if errorlevel 1 goto :fail
set "OUT=build"

:done
echo.
echo ============================================================
echo  Build OK
echo    Desktop UI : %OUT%\movie_summary_bot.exe
echo    Web panel  : %OUT%\movie_summary_web.exe
echo    CLI        : %OUT%\movie_summary_cli.exe
echo.
echo  Start the app with:   run.bat        (desktop window)
echo                    or: run.bat web    (browser control panel)
echo ============================================================
echo.
if not defined CI pause
exit /b 0

:fail
echo.
echo ============================================================
echo  [ERROR] The build failed - see the messages above.
echo ============================================================
echo.
if not defined CI pause
exit /b 1
