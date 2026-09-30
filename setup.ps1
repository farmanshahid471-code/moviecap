# ---------------------------------------------------------------------------
#  AI-Movie-Shorts (Windows) - one-time setup for BUILDING FROM SOURCE
#
#  >>> If you downloaded the ready-made zip you do NOT need this script. <<<
#  Just double-click run.bat (or run "run.bat web"). FFmpeg is downloaded for
#  you into a portable folder on F: - nothing is written to C:.
#
#  This script prepares a machine that wants to compile the .exe files itself:
#     1. FFmpeg          -> portable copy via install_tools.ps1 (stays on F:)
#     2. CMake           -> winget
#     3. C/C++ compiler  -> winget (Visual Studio Build Tools, ~3-6 GB)
#
#  Usage:  right-click -> "Run with PowerShell", or
#          powershell -ExecutionPolicy Bypass -File .\setup.ps1
#
#  The window stays open at the end so you can read the result.
# ---------------------------------------------------------------------------
$ErrorActionPreference = "Stop"
Set-Location -Path $PSScriptRoot

function Have([string]$cmd) { return [bool](Get-Command $cmd -ErrorAction SilentlyContinue) }

function Finish([int]$code) {
    Write-Host ""
    Read-Host "Press Enter to close this window" | Out-Null
    exit $code
}

function Say([string]$msg, [string]$color = "Gray") { Write-Host $msg -ForegroundColor $color }

try {
    Say ""
    Say "=== AI-Movie-Shorts setup (only needed to build from source) ===" "Cyan"
    Say "Working folder: $PSScriptRoot"

    # ---------------------------------------------------------- 1. FFmpeg
    # Deliberately NOT "winget install Gyan.FFmpeg": that lands on C:. The
    # portable installer puts it under F:\AI-Movie-Shorts\tools instead, which
    # is also what run.bat and the app look for.
    if ((Have "ffmpeg") -and (Have "ffprobe")) {
        Say "[OK] FFmpeg is already on PATH" "Green"
    } else {
        Say "[..] Installing a portable FFmpeg (nothing is written to C:)..."
        & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "install_tools.ps1")
        if ($LASTEXITCODE -ne 0) { throw "the FFmpeg installer failed (exit $LASTEXITCODE)" }
    }

    # ------------------------------------------------------------- 2. winget
    if (-not (Have "winget")) {
        Say ""
        Say "[!] winget was not found, so CMake and the C compiler could not be" "Yellow"
        Say "    installed automatically. Get 'App Installer' from the Microsoft" "Yellow"
        Say "    Store and run this script again." "Yellow"
        Finish 1
    }

    # ------------------------------------------------------------- 3. CMake
    if (Have "cmake") {
        Say "[OK] CMake is already installed" "Green"
    } else {
        Say "[..] Installing CMake..."
        winget install --id Kitware.CMake -e --accept-source-agreements --accept-package-agreements
        if ($LASTEXITCODE -ne 0) { throw "installing CMake failed (exit $LASTEXITCODE)" }
    }

    # ---------------------------------------------- 4. C/C++ build tools
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $haveVS = $false
    if (Test-Path $vswhere) {
        $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($vs) { $haveVS = $true }
    }
    if ($haveVS) {
        Say "[OK] Visual Studio C/C++ tools found" "Green"
    } elseif (Have "gcc") {
        Say "[OK] gcc (MinGW) found - build.bat will use it" "Green"
    } else {
        Say "[..] Installing Visual Studio Build Tools (C++ workload, ~3-6 GB, this takes a while)..."
        winget install --id Microsoft.VisualStudio.2022.BuildTools -e --accept-source-agreements --accept-package-agreements `
            --override "--quiet --wait --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
        if ($LASTEXITCODE -ne 0) { throw "installing the Visual Studio Build Tools failed (exit $LASTEXITCODE)" }
    }

    # ---------------------------------------------------- 5. project folders
    foreach ($d in @("movies","movies_retired","output","tiktok_output","clips","clips\audio","scripts","scripts\srt_files","backgroundmusic")) {
        New-Item -ItemType Directory -Force -Path $d | Out-Null
    }

    # ------------------------------------------------------------ 6. config
    $cfg = Get-Content "config.json" -Raw -ErrorAction SilentlyContinue
    if (-not $cfg -or $cfg -match "OpenAIAPI") {
        Say ""
        Say "[!] Put your OpenAI key in config.json (or in the web panel's Settings)." "Yellow"
        Say "    An ElevenLabs key is optional: set `"tts_provider`": `"piper`" and run" "Yellow"
        Say "    `"run.bat tts`" to narrate for free instead." "Yellow"
    }

    Say ""
    Say "=== Setup finished ===" "Green"
    Say "Open a NEW terminal (so the PATH changes apply), then run:"
    Say "    build.bat        to compile"
    Say "    run.bat web      to start the browser control panel"
    Finish 0
}
catch {
    Say ""
    Say "[ERROR] Setup stopped: $($_.Exception.Message)" "Red"
    Say "        Nothing else was changed. Fix the problem above and run this again." "Red"
    Finish 1
}
