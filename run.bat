@echo off
REM ==========================================================================
REM  AI-Movie-Shorts (Windows)
REM
REM    run.bat              -> desktop UI
REM    run.bat web          -> browser control panel (http://127.0.0.1:8080)
REM    run.bat cli          -> headless (console only) generation
REM    run.bat setup        -> install/check FFmpeg only, do not start the app
REM    run.bat tts          -> install + start the FREE Piper narration server
REM    run.bat tts install  -> install Piper only, do not start the server
REM
REM  Anything the app needs is downloaded and installed for you, into a portable
REM  folder on F: so that C: stays free:
REM
REM      F:\AI-Movie-Shorts\tools\ffmpeg\bin\     ffmpeg.exe + ffprobe.exe
REM      F:\AI-Movie-Shorts\tools\piper\python\   a private Python for Piper
REM      F:\AI-Movie-Shorts\tools\piper\voices\   the Piper voice model
REM
REM  No admin rights, no winget, no system PATH change, nothing on C:.
REM  Put the tools somewhere else with:   set MOVIECAP_TOOLS=F:\MovieTools
REM  If there is no F: drive they go into  <this folder>\tools  instead.
REM
REM  Optional Piper settings:
REM      set PIPER_VOICE=en_US-amy-medium     (default en_US-lessac-medium)
REM      set PIPER_PORT=5000
REM ==========================================================================
setlocal EnableExtensions
cd /d "%~dp0"

if not defined MOVIECAP_TOOLS set "MOVIECAP_TOOLS=F:\AI-Movie-Shorts\tools"
call :resolve_tools
set "FFBIN=%TOOLSDIR%\ffmpeg\bin"

if /i "%~1"=="tts" goto :tts

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
REM  Free narration: install (once) and then run the Piper HTTP server that the
REM  app talks to when "Narration engine" is set to Piper.
:tts
if not defined PIPER_VOICE set "PIPER_VOICE=en_US-lessac-medium"
if not defined PIPER_PORT  set "PIPER_PORT=5000"
set "PIPERDIR=%TOOLSDIR%\piper"
set "PIPERPY=%PIPERDIR%\python\python.exe"
set "PIPERVOICES=%PIPERDIR%\voices"

if not exist "%PIPERVOICES%\%PIPER_VOICE%.onnx" (
  echo.
  echo [..] Piper is not installed yet - downloading it now.
  echo     Everything goes into %PIPERDIR%
  echo     Nothing is installed on C: and no admin rights are needed.
  echo.
  powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install_piper.ps1" -ToolsDir "%TOOLSDIR%" -Voice "%PIPER_VOICE%"
  if errorlevel 1 (
    echo.
    echo [ERROR] Piper could not be installed. See the messages above.
    echo         Check your internet connection and run "run.bat tts" again.
    exit /b 1
  )
)

if /i "%~2"=="install" (
  echo.
  echo [OK] Piper is installed in %PIPERDIR%
  echo     Start it later with:  run.bat tts
  exit /b 0
)

if not exist "%PIPERPY%" (
  echo [ERROR] %PIPERPY% is missing - run "run.bat tts install" first.
  exit /b 1
)

echo.
echo ============================================================
echo   Piper narration server - free, runs entirely on your PC
echo.
echo   URL   : http://127.0.0.1:%PIPER_PORT%
echo   Voice : %PIPER_VOICE%
echo.
echo   In the web panel Settings, set:
echo       Narration engine = Piper
echo       TTS server URL   = http://127.0.0.1:%PIPER_PORT%
echo.
echo   Keep this window open while you generate videos.
echo   Press Ctrl+C to stop the server.
echo ============================================================
echo.

"%PIPERPY%" -m piper.http_server --host 127.0.0.1 --port %PIPER_PORT% -m %PIPER_VOICE% --data-dir "%PIPERVOICES%"
exit /b %errorlevel%

REM --------------------------------------------------------------------------
REM  Tools folder: honour MOVIECAP_TOOLS (F: by default), but fall back to a
REM  "tools" folder next to the app when that drive is not there.
:resolve_tools
set "TOOLSDIR=%MOVIECAP_TOOLS%"
if exist "%MOVIECAP_TOOLS%" exit /b 0
set "TOOLSDIR=%~dp0tools"
exit /b 0

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
if exist "%FFBIN%\ffmpeg.exe" (
  echo [OK] FFmpeg installed in %FFBIN%
  exit /b 0
)
if exist "%~dp0tools\ffmpeg\bin\ffmpeg.exe" (
  set "FFBIN=%~dp0tools\ffmpeg\bin"
  echo [OK] FFmpeg installed in %~dp0tools\ffmpeg\bin
  exit /b 0
)
echo [ERROR] The installer finished but no ffmpeg.exe was found afterwards.
exit /b 1
