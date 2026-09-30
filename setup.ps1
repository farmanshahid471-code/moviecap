# ---------------------------------------------------------------------------
#  AI-Movie-Shorts (Windows) - one-time setup
#  Installs: FFmpeg, CMake, Visual Studio Build Tools (C compiler) via winget.
#  Usage (PowerShell):   powershell -ExecutionPolicy Bypass -File .\setup.ps1
# ---------------------------------------------------------------------------
$ErrorActionPreference = "Stop"
Set-Location -Path $PSScriptRoot

function Have($cmd) { return [bool](Get-Command $cmd -ErrorAction SilentlyContinue) }

Write-Host ""
Write-Host "=== AI-Movie-Shorts setup ===" -ForegroundColor Cyan

if (-not (Have "winget")) {
    Write-Host "winget was not found. Install 'App Installer' from the Microsoft Store, then re-run." -ForegroundColor Red
    exit 1
}

# 1) FFmpeg (ffmpeg.exe + ffprobe.exe on PATH)
if ((Have "ffmpeg") -and (Have "ffprobe")) {
    Write-Host "[OK] FFmpeg already installed" -ForegroundColor Green
} else {
    Write-Host "[..] Installing FFmpeg (Gyan.FFmpeg)..."
    winget install --id Gyan.FFmpeg -e --accept-source-agreements --accept-package-agreements
}

# 2) CMake
if (Have "cmake") {
    Write-Host "[OK] CMake already installed" -ForegroundColor Green
} else {
    Write-Host "[..] Installing CMake..."
    winget install --id Kitware.CMake -e --accept-source-agreements --accept-package-agreements
}

# 3) C compiler: Visual Studio (any edition) or Build Tools with the C++ workload
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$haveVS = $false
if (Test-Path $vswhere) {
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($vs) { $haveVS = $true }
}
if ($haveVS) {
    Write-Host "[OK] Visual Studio C/C++ tools found" -ForegroundColor Green
} elseif (Have "gcc") {
    Write-Host "[OK] gcc (MinGW) found - build.bat will use it" -ForegroundColor Green
} else {
    Write-Host "[..] Installing Visual Studio Build Tools (C++ workload, ~3-6 GB)..."
    winget install --id Microsoft.VisualStudio.2022.BuildTools -e --accept-source-agreements --accept-package-agreements `
        --override "--quiet --wait --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
}

# 4) Project folders
foreach ($d in @("movies","movies_retired","output","tiktok_output","clips","clips\audio","scripts","scripts\srt_files","backgroundmusic")) {
    New-Item -ItemType Directory -Force -Path $d | Out-Null
}

# 5) config.json reminder
$cfg = Get-Content "config.json" -Raw -ErrorAction SilentlyContinue
if (-not $cfg -or $cfg -match "OpenAIAPI" -or $cfg -match "ElevenLabsAPI") {
    Write-Host ""
    Write-Host "[!] Edit config.json and put in your OpenAI + ElevenLabs API keys." -ForegroundColor Yellow
}

Write-Host ""
Write-Host "Setup finished. Open a NEW terminal (so PATH updates apply), then run:  build.bat" -ForegroundColor Cyan
