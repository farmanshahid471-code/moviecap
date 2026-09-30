@echo off
REM ==========================================================================
REM  AI-Movie-Shorts (Windows)
REM
REM    run.bat         -> desktop UI
REM    run.bat web     -> browser control panel (http://127.0.0.1:8080)
REM    run.bat cli     -> headless (console only) generation
REM    run.bat setup   -> only install/check the tools, do not start the app
REM
REM  Anything the app needs (FFmpeg) is downloaded and installed for you the
REM  first time, into a portable folder on F: so that C: stays free:
REM
REM      F:\AI-Movie-Shorts\tools\ffmpeg\bin\ffmpeg.exe
REM      F:\AI-Movie-Shorts\tools\ffmpeg\bin\ffprobe.exe
REM
REM  No admin rights, no winget, no system PATH change, nothing on C:.
REM  Put the tools somewhere else with:   set MOVIECAP_TOOLS=F:\MovieTools
REM  If there is no F: drive they go into  <this folder>\tools  instead.
REM ==========================================================================
setlocal EnableExtensions
cd /d "%~dp0"

if not defined MOVIECAP_TOOLS set "MOVIECAP_TOOLS=F:\AI-Movie-Shorts\tools"
set "FFBIN=%MOVIECAP_TOOLS%\ffmpeg\bin"

call :ensure_tools
if errorlevel 1 exit /b 1

if /i "%~1"=="setup" (
  echo.
  echo Tools are ready in: %FFBIN%
  echo Run "run.bat" for the desktop UI or "run.bat web" for the browser panel.
  exit /b 0
)

REM Make the portable FFmpeg visible to the app (this session only).
if exist "%FFBIN%\ffmpeg.exe" set "PATH=%FFBIN%;%PATH%"

set "EXE=movie_summary_bot.exe"
if /i "%~1"=="cli" set "EXE=movie_summary_cli.exe"
if /i "%~1"=="web" set "EXE=movie_summary_web.exe"

if exist "build\Release\%EXE%" (
  "build\Release\%EXE%" %2 %3 %4 %5
) else if exist "build\%EXE%" (
  "build\%EXE%" %2 %3 %4 %5
) else if exist "%EXE%" (
  "%EXE%" %2 %3 %4 %5
) else (
  echo [ERROR] %EXE% not found. Run build.bat first.
  exit /b 1
)
exit /b %errorlevel%

REM --------------------------------------------------------------------------
REM  NOTE: every path out of this subroutine uses an explicit "exit /b N".
REM  ECHO and GOTO do not reset ERRORLEVEL, so a leftover failure from the
REM  "where" lookups would otherwise be reported as a failed install.
:ensure_tools
REM Already on PATH? Then there is nothing to download.
where ffmpeg >nul 2>nul
if errorlevel 1 goto :need_tools
where ffprobe >nul 2>nul
if errorlevel 1 goto :need_tools
echo [OK] FFmpeg found on PATH.
exit /b 0

:need_tools
if exist "%FFBIN%\ffmpeg.exe" if exist "%FFBIN%\ffprobe.exe" (
  echo [OK] Using the portable FFmpeg in %FFBIN%
  exit /b 0
)
REM A previous run may have fallen back to the app folder (no F: drive).
if exist "%~dp0tools\ffmpeg\bin\ffmpeg.exe" if exist "%~dp0tools\ffmpeg\bin\ffprobe.exe" (
  set "FFBIN=%~dp0tools\ffmpeg\bin"
  echo [OK] Using the portable FFmpeg in %~dp0tools\ffmpeg\bin
  exit /b 0
)

echo.
echo [..] FFmpeg was not found - downloading a portable copy.
echo     Target folder: %FFBIN%
echo     Nothing is installed on C: and no admin rights are needed.
echo.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install_tools.ps1" -ToolsDir "%MOVIECAP_TOOLS%"
if errorlevel 1 (
  echo.
  echo [ERROR] FFmpeg could not be installed automatically.
  echo         Check your internet connection and run this again, or install
  echo         it yourself with:   winget install Gyan.FFmpeg
  echo         then close and reopen this folder and run run.bat again.
  exit /b 1
)

REM The installer falls back to <app folder>\tools when F: is not available.
if exist "%FFBIN%\ffmpeg.exe" exit /b 0
if exist "%~dp0tools\ffmpeg\bin\ffmpeg.exe" set "FFBIN=%~dp0tools\ffmpeg\bin"
exit /b 0
