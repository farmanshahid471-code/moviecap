@echo off
REM AI-Movie-Shorts (Windows) - start the UI from the project folder
REM   run.bat        -> desktop UI
REM   run.bat cli    -> headless (console only) generation
setlocal
cd /d "%~dp0"

set "EXE=movie_summary_bot.exe"
if /i "%~1"=="cli" set "EXE=movie_summary_cli.exe"

if exist "build\Release\%EXE%" (
  "build\Release\%EXE%"
) else if exist "build\%EXE%" (
  "build\%EXE%"
) else if exist "%EXE%" (
  "%EXE%"
) else (
  echo [ERROR] %EXE% not found. Run build.bat first.
  exit /b 1
)
