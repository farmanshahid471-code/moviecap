@echo off
REM AI-Movie-Shorts (Windows) - start the UI from the project folder
REM   run.bat        -> desktop UI
REM   run.bat web    -> browser control panel (http://127.0.0.1:8080)
REM   run.bat cli    -> headless (console only) generation
setlocal
cd /d "%~dp0"

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
