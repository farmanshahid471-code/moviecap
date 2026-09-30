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
REM  Everything the app needs is downloaded into a portable tools folder, so
REM  C: is left alone and no admin rights are needed:
REM
REM      <tools>\ffmpeg\bin\     ffmpeg.exe + ffprobe.exe
REM      <tools>\piper\python\   a private Python for the free Piper voice
REM      <tools>\piper\voices\   the Piper voice model
REM
REM  <tools> is chosen like this:
REM      1. %MOVIECAP_TOOLS% if that folder already exists
REM      2. <this folder>\tools   when this folder is already on F:
REM      3. F:\AI-Movie-Shorts\tools   when an F: drive exists
REM      4. <this folder>\tools   otherwise
REM
REM  NOTE: this script deliberately uses no parenthesised IF blocks. A folder
REM  name containing "(" or ")" - like "AI-Movie-Shorts-Windows (1)" - breaks
REM  any unquoted expansion inside such a block.
REM ==========================================================================
setlocal EnableExtensions
cd /d "%~dp0"

if not defined MOVIECAP_TOOLS set "MOVIECAP_TOOLS=F:\AI-Movie-Shorts\tools"
call :resolve_tools
set "FFBIN=%TOOLSDIR%\ffmpeg\bin"

if /i "%~1"=="tts" goto :tts

call :ensure_tools
if not errorlevel 1 goto :tools_ready

echo.
echo ============================================================
echo  [ERROR] FFmpeg could not be set up automatically.
echo.
echo  To see the exact error, open a terminal in this folder and run:
echo      run.bat setup
echo.
echo  You can also install FFmpeg yourself with:
echo      winget install Gyan.FFmpeg
echo  then reopen this folder and run run.bat again.
echo ============================================================
if not defined CI pause
exit /b 1

:tools_ready
if /i not "%~1"=="setup" goto :launch
echo.
echo Tools are ready in:
echo   "%FFBIN%"
echo.
echo Run "run.bat" for the desktop UI or "run.bat web" for the browser panel.
exit /b 0

:launch
REM Make the portable FFmpeg visible to the app (this session only).
if exist "%FFBIN%\ffmpeg.exe" set "PATH=%FFBIN%;%PATH%"

set "EXE=movie_summary_bot.exe"
if /i "%~1"=="cli" set "EXE=movie_summary_cli.exe"
if /i "%~1"=="web" set "EXE=movie_summary_web.exe"

set "EXEPATH="
if exist "build\Release\%EXE%" set "EXEPATH=build\Release\%EXE%"
if not defined EXEPATH if exist "build\%EXE%" set "EXEPATH=build\%EXE%"
if not defined EXEPATH if exist "%EXE%" set "EXEPATH=%EXE%"
if not defined EXEPATH goto :no_exe

"%EXEPATH%" %2 %3 %4 %5
set "RC=%errorlevel%"
if not "%RC%"=="0" pause
exit /b %RC%

:no_exe
echo [ERROR] %EXE% was not found in this folder.
echo         If you are building from source, run build.bat first.
if not defined CI pause
exit /b 1

REM --------------------------------------------------------------------------
REM  Free narration: install (once) and then run the Piper HTTP server.
:tts
if not defined PIPER_VOICE set "PIPER_VOICE=en_US-lessac-medium"
if not defined PIPER_PORT  set "PIPER_PORT=5000"
set "PIPERDIR=%TOOLSDIR%\piper"
set "PIPERPY=%PIPERDIR%\python\python.exe"
set "PIPERVOICES=%PIPERDIR%\voices"

if exist "%PIPERVOICES%\%PIPER_VOICE%.onnx" goto :tts_ready

echo.
echo [..] Piper is not installed yet - downloading it now.
echo     Everything goes into "%PIPERDIR%"
echo     Nothing is installed on C: and no admin rights are needed.
echo     This is about 100 MB and can take a few minutes.
echo.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install_piper.ps1" -ToolsDir "%TOOLSDIR%" -Voice "%PIPER_VOICE%"
if not errorlevel 1 goto :tts_ready
echo.
echo [ERROR] Piper could not be installed. Check your internet connection
echo         and run "run.bat tts" again.
if not defined CI pause
exit /b 1

:tts_ready
if /i not "%~2"=="install" goto :tts_serve
echo.
echo [OK] Piper is installed in "%PIPERDIR%"
echo      Start it later with:  run.bat tts
exit /b 0

:tts_serve
if exist "%PIPERPY%" goto :tts_go
echo [ERROR] "%PIPERPY%" is missing - run "run.bat tts install" first.
if not defined CI pause
exit /b 1

:tts_go
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
echo.
echo The Piper server has stopped.
if not defined CI pause
exit /b 0

REM --------------------------------------------------------------------------
REM  Pick the tools folder. Everything stays on F: whenever that is possible.
:resolve_tools
if not exist "%MOVIECAP_TOOLS%" goto :rt_appdrive
set "TOOLSDIR=%MOVIECAP_TOOLS%"
exit /b 0

:rt_appdrive
set "APPDRIVE="
for %%D in ("%~dp0.") do set "APPDRIVE=%%~dD"
if /i not "%APPDRIVE%"=="F:" goto :rt_fdrive
set "TOOLSDIR=%~dp0tools"
exit /b 0

:rt_fdrive
vol F: >nul 2>nul
if errorlevel 1 goto :rt_local
set "TOOLSDIR=%MOVIECAP_TOOLS%"
exit /b 0

:rt_local
set "TOOLSDIR=%~dp0tools"
exit /b 0

REM --------------------------------------------------------------------------
REM  NOTE: every path out of this subroutine uses an explicit "exit /b N".
REM  ECHO and GOTO do not reset ERRORLEVEL, so a leftover failure from the
REM  "where" lookups would otherwise be reported as a failed install.
:ensure_tools
where ffmpeg >nul 2>nul
if errorlevel 1 goto :et_need
where ffprobe >nul 2>nul
if errorlevel 1 goto :et_need
echo [OK] FFmpeg found on PATH.
exit /b 0

:et_need
if not exist "%FFBIN%\ffmpeg.exe" goto :et_appcopy
if not exist "%FFBIN%\ffprobe.exe" goto :et_appcopy
echo [OK] Using the portable FFmpeg in "%FFBIN%"
exit /b 0

:et_appcopy
if not exist "%~dp0tools\ffmpeg\bin\ffmpeg.exe" goto :et_install
if not exist "%~dp0tools\ffmpeg\bin\ffprobe.exe" goto :et_install
set "TOOLSDIR=%~dp0tools"
set "FFBIN=%TOOLSDIR%\ffmpeg\bin"
echo [OK] Using the portable FFmpeg in "%FFBIN%"
exit /b 0

:et_install
echo.
echo [..] FFmpeg was not found - downloading a portable copy.
echo     Target folder: "%FFBIN%"
echo     Nothing is installed on C: and no admin rights are needed.
echo     This is about 110 MB and can take a few minutes.
echo.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install_tools.ps1" -ToolsDir "%TOOLSDIR%"
if errorlevel 1 exit /b 1
if not exist "%FFBIN%\ffmpeg.exe" goto :et_missing
if not exist "%FFBIN%\ffprobe.exe" goto :et_missing
echo [OK] FFmpeg installed in "%FFBIN%"
exit /b 0

:et_missing
REM The installer falls back to <app>\tools if the target folder is not usable.
if not exist "%~dp0tools\ffmpeg\bin\ffmpeg.exe" goto :et_fail
if not exist "%~dp0tools\ffmpeg\bin\ffprobe.exe" goto :et_fail
set "TOOLSDIR=%~dp0tools"
set "FFBIN=%TOOLSDIR%\ffmpeg\bin"
echo [OK] FFmpeg installed in "%FFBIN%"
exit /b 0

:et_fail
echo [ERROR] The installer finished but no ffmpeg.exe was found in "%FFBIN%"
echo         or in "%~dp0tools\ffmpeg\bin".
exit /b 1
