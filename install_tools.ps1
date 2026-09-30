# ---------------------------------------------------------------------------
#  AI-Movie-Shorts (Windows) - portable FFmpeg installer
#
#  Downloads ffmpeg.exe + ffprobe.exe into a tools folder and leaves Windows
#  alone: no winget, no admin rights, no system PATH edit, nothing on C:.
#  Only the two .exe files are kept, so the ~90 MB download shrinks to the
#  binaries and the zip is deleted afterwards.
#
#  Where it installs (first that works):
#     1. -ToolsDir argument
#     2. $env:MOVIECAP_TOOLS
#     3. F:\AI-Movie-Shorts\tools        <- default, keeps C: free
#     4. <this folder>\tools             <- fallback when there is no F: drive
#
#  Usage:
#     powershell -ExecutionPolicy Bypass -File .\install_tools.ps1
#     powershell -ExecutionPolicy Bypass -File .\install_tools.ps1 -ToolsDir D:\tools
#     powershell -ExecutionPolicy Bypass -File .\install_tools.ps1 -Force   # reinstall
#
#  run.bat calls this automatically the first time it needs FFmpeg.
# ---------------------------------------------------------------------------
[CmdletBinding()]
param(
    [string]$ToolsDir,
    [switch]$Force
)

$ErrorActionPreference = "Stop"

# Older Windows builds default to TLS 1.0, which the download hosts refuse.
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$ProgressPreference = "SilentlyContinue"   # Invoke-WebRequest is ~10x faster like this

$sources = @(
    "https://www.gyan.dev/ffmpeg/builds/ffmpeg-release-essentials.zip",
    "https://github.com/BtbN/FFmpeg-Builds/releases/latest/download/ffmpeg-master-latest-win64-gpl.zip"
)

function Say([string]$msg, [string]$color = "Gray") {
    Write-Host $msg -ForegroundColor $color
}

function Resolve-ToolsDir {
    param([string]$requested)

    $candidates = @()
    if ($requested)                     { $candidates += $requested }
    if ($env:MOVIECAP_TOOLS)            { $candidates += $env:MOVIECAP_TOOLS }
    $candidates += "F:\AI-Movie-Shorts\tools"
    $candidates += (Join-Path $PSScriptRoot "tools")

    foreach ($c in $candidates) {
        if (-not $c) { continue }
        try {
            New-Item -ItemType Directory -Force -Path $c -ErrorAction Stop | Out-Null
            # prove it is really writable (a read-only or absent drive throws)
            $probe = Join-Path $c ".write_test"
            Set-Content -Path $probe -Value "ok" -ErrorAction Stop
            Remove-Item -Path $probe -Force -ErrorAction SilentlyContinue
            return (Resolve-Path $c).Path
        } catch {
            continue
        }
    }
    throw "No writable folder for the tools. Create F:\ or pass -ToolsDir <folder>."
}

function Test-FFmpeg {
    param([string]$binDir)

    $ff  = Join-Path $binDir "ffmpeg.exe"
    $fp  = Join-Path $binDir "ffprobe.exe"
    if (-not (Test-Path $ff) -or -not (Test-Path $fp)) { return $false }
    try {
        $out = & $ff -version 2>&1
        if ($LASTEXITCODE -ne 0) { return $false }
        $out2 = & $fp -version 2>&1
        if ($LASTEXITCODE -ne 0) { return $false }
        return ("$out" -match "ffmpeg version") -and ("$out2" -match "ffprobe version")
    } catch {
        return $false
    }
}

# --------------------------------------------------------------------------
$root = Resolve-ToolsDir -requested $ToolsDir
$binDir = Join-Path $root "ffmpeg\bin"

Say ""
Say "=== AI-Movie-Shorts: FFmpeg installer ===" "Cyan"
Say "Tools folder : $root"

if ((Test-FFmpeg $binDir) -and -not $Force) {
    Say "[OK] FFmpeg is already installed here - nothing to download." "Green"
    Say "Add to PATH (or let run.bat do it):  $binDir"
    exit 0
}

New-Item -ItemType Directory -Force -Path $binDir | Out-Null
$dlDir = Join-Path $root "downloads"
New-Item -ItemType Directory -Force -Path $dlDir | Out-Null
$zipPath = Join-Path $dlDir "ffmpeg.zip"

Add-Type -AssemblyName System.IO.Compression.FileSystem

$installed = $false
foreach ($url in $sources) {
    try {
        Say "[..] Downloading $url"
        if (Test-Path $zipPath) { Remove-Item $zipPath -Force }
        Invoke-WebRequest -Uri $url -OutFile $zipPath -UseBasicParsing -TimeoutSec 900
        $size = (Get-Item $zipPath).Length
        Say ("[..] Downloaded {0:N1} MB - extracting ffmpeg.exe + ffprobe.exe" -f ($size / 1MB))

        $zip = [System.IO.Compression.ZipFile]::OpenRead($zipPath)
        try {
            foreach ($entry in $zip.Entries) {
                $name = [System.IO.Path]::GetFileName($entry.FullName)
                if ($name -ieq "ffmpeg.exe" -or $name -ieq "ffprobe.exe") {
                    $dest = Join-Path $binDir $name
                    [System.IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $dest, $true)
                }
            }
        } finally {
            $zip.Dispose()
        }

        Remove-Item $zipPath -Force -ErrorAction SilentlyContinue

        if (Test-FFmpeg $binDir) { $installed = $true; break }
        Say "[WARN] That archive did not contain working binaries, trying the next source..." "Yellow"
    } catch {
        Say "[WARN] $url failed: $($_.Exception.Message)" "Yellow"
    }
}

Remove-Item $zipPath -Force -ErrorAction SilentlyContinue

if (-not $installed) {
    Say ""
    Say "[ERROR] Could not download FFmpeg from any source." "Red"
    Say "        Check your internet connection, or install it manually:" "Red"
    Say "            winget install Gyan.FFmpeg" "Red"
    exit 1
}

$ver = (& (Join-Path $binDir "ffmpeg.exe") -version 2>&1 | Select-Object -First 1)
Say "[OK] Installed: $ver" "Green"
Say "[OK] Location : $binDir" "Green"
Say ""
Say "run.bat puts this on PATH for the app automatically. To use ffmpeg"
Say "yourself in any terminal, add that folder to your PATH."
exit 0
