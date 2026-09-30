# ---------------------------------------------------------------------------
#  AI-Movie-Shorts (Windows) - free narration engine installer (Piper TTS)
#
#  Installs a completely self-contained Piper into the tools folder:
#
#      <tools>\piper\python\     a private Python (embeddable build, ~11 MB)
#      <tools>\piper\voices\     the voice model (.onnx)
#
#  Nothing is written to C:, no admin rights, no system Python needed, and your
#  own Python (if you have one) is left completely alone.
#
#  Where it installs (first that works):
#     1. -ToolsDir argument
#     2. $env:MOVIECAP_TOOLS
#     3. F:\AI-Movie-Shorts\tools        <- default, keeps C: free
#     4. <this folder>\tools             <- fallback when there is no F: drive
#
#  Usage:
#     powershell -ExecutionPolicy Bypass -File .\install_piper.ps1
#     powershell -ExecutionPolicy Bypass -File .\install_piper.ps1 -Voice en_US-amy-medium
#
#  run.bat calls this automatically from:   run.bat tts
# ---------------------------------------------------------------------------
[CmdletBinding()]
param(
    [string]$ToolsDir,
    [string]$Voice = "en_US-lessac-medium",
    [string]$PyVersion = "3.12.10",
    [switch]$Force
)

$ErrorActionPreference = "Stop"

[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$ProgressPreference = "SilentlyContinue"

function Say([string]$msg, [string]$color = "Gray") { Write-Host $msg -ForegroundColor $color }

function Resolve-ToolsDir {
    param([string]$requested)

    $candidates = @()
    if ($requested)          { $candidates += $requested }
    if ($env:MOVIECAP_TOOLS) { $candidates += $env:MOVIECAP_TOOLS }
    $candidates += "F:\AI-Movie-Shorts\tools"
    $candidates += (Join-Path $PSScriptRoot "tools")

    foreach ($c in $candidates) {
        if (-not $c) { continue }
        try {
            New-Item -ItemType Directory -Force -Path $c -ErrorAction Stop | Out-Null
            $probe = Join-Path $c ".write_test"
            Set-Content -Path $probe -Value "ok" -ErrorAction Stop
            Remove-Item -Path $probe -Force -ErrorAction SilentlyContinue
            return (Resolve-Path $c).Path
        } catch { continue }
    }
    throw "No writable folder for the tools. Create F:\ or pass -ToolsDir <folder>."
}

function Get-File([string]$url, [string]$dest) {
    if (Test-Path $dest) { Remove-Item $dest -Force }
    Invoke-WebRequest -Uri $url -OutFile $dest -UseBasicParsing -TimeoutSec 1800
}

# --------------------------------------------------------------------------
$tools  = Resolve-ToolsDir -requested $ToolsDir
$root   = Join-Path $tools  "piper"
$pyDir  = Join-Path $root   "python"
$py     = Join-Path $pyDir  "python.exe"
$voices = Join-Path $root   "voices"

Say ""
Say "=== AI-Movie-Shorts: Piper TTS installer (free narration) ===" "Cyan"
Say "Install folder : $root"
Say "Voice          : $Voice"

New-Item -ItemType Directory -Force -Path $root   | Out-Null
New-Item -ItemType Directory -Force -Path $voices | Out-Null

# ---------------------------------------------------------------- 1. Python
if ((Test-Path $py) -and -not $Force) {
    Say "[OK] Private Python already present." "Green"
} else {
    $zipUrl = "https://www.python.org/ftp/python/$PyVersion/python-$PyVersion-embed-amd64.zip"
    $zip = Join-Path $root "python.zip"
    Say "[..] Downloading a private Python $PyVersion (embeddable, nothing touches C:)..."
    Get-File $zipUrl $zip
    Say ("[..] {0:N1} MB downloaded - unpacking" -f ((Get-Item $zip).Length / 1MB))

    if (Test-Path $pyDir) { Remove-Item -Recurse -Force $pyDir }
    Expand-Archive -Path $zip -DestinationPath $pyDir -Force
    Remove-Item $zip -Force -ErrorAction SilentlyContinue
    if (-not (Test-Path $py)) { throw "python.exe is missing after unpacking" }

    # The embeddable build ignores site-packages until "import site" is enabled.
    $pth = Get-ChildItem -Path $pyDir -Filter "python*._pth" | Select-Object -First 1
    if (-not $pth) { throw "python*._pth not found - cannot enable site-packages" }
    $lines = Get-Content $pth.FullName | Where-Object { $_ -notmatch "^\s*#?\s*import site\s*$" }
    $lines += "Lib\site-packages"
    $lines += "import site"
    Set-Content -Path $pth.FullName -Value $lines -Encoding ASCII
    Say "[OK] Private Python ready." "Green"
}

# ---------------------------------------------------------------- 2. pip
$hasPip = $false
try {
    & $py -m pip --version *> $null
    $hasPip = ($LASTEXITCODE -eq 0)
} catch { $hasPip = $false }

if (-not $hasPip) {
    Say "[..] Installing pip..."
    $getPip = Join-Path $root "get-pip.py"
    Get-File "https://bootstrap.pypa.io/get-pip.py" $getPip
    & $py $getPip --no-warn-script_LOCATION 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "pip bootstrap failed" }
    Remove-Item $getPip -Force -ErrorAction SilentlyContinue
}

# ------------------------------------------------------------ 3. piper-tts
$hasPiper = $false
try {
    & $py -c "import piper, flask" *> $null
    $hasPiper = ($LASTEXITCODE -eq 0)
} catch { $hasPiper = $false }

if ((-not $hasPiper) -or $Force) {
    # flask is an *extra* of piper-tts, so the [http] part is required for the
    # HTTP server that this app talks to.
    Say "[..] Installing piper-tts[http] + onnxruntime (this can take a minute)..."
    & $py -m pip install --no-warn-script-location --upgrade "piper-tts[http]" 2>&1 |
        ForEach-Object { Write-Host "    $_" }
    if ($LASTEXITCODE -ne 0) { throw "pip install piper-tts[http] failed" }
}
Say "[OK] piper-tts ready." "Green"

# ------------------------------------------------------------- 4. the voice
$onnx = Join-Path $voices "$Voice.onnx"
if ((Test-Path $onnx) -and -not $Force) {
    Say "[OK] Voice already downloaded." "Green"
} else {
    Say "[..] Downloading the voice model $Voice (about 60 MB)..."
    & $py -m piper.download_voices $Voice --download-dir $voices 2>&1 |
        ForEach-Object { Write-Host "    $_" }
    if ($LASTEXITCODE -ne 0) { throw "downloading the voice failed" }
    if (-not (Test-Path $onnx)) { throw "the voice model did not arrive at $onnx" }
}

# ------------------------------------------------------------- 5. smoke test
Say "[..] Loading the voice to make sure it really works..."
& $py -c "from piper import PiperVoice; PiperVoice.load(r'$onnx'); print('voice loads OK')"
if ($LASTEXITCODE -ne 0) { throw "the voice model could not be loaded" }

Say ""
Say "[OK] Piper is installed and working." "Green"
Say "    Python : $py"
Say "    Voice  : $onnx"
Say ""
Say "Start the narration server with:   run.bat tts"
Say "Then in the web panel set Narration engine = Piper and"
Say "TTS server URL = http://127.0.0.1:5000"
exit 0
