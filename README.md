# AI-Movie-Shorts for Windows

Turn full-length `.mp4` movies into **AI-narrated movie recap videos** (horizontal + vertical), built automatically from subtitles + optional script context.

This is a **Windows port** of [keithhb33/AI-Movie-Shorts](https://github.com/keithhb33/AI-Movie-Shorts). It has the same features, UI, folder layout, `config.json` and pipeline, rebuilt so it works on Windows 10 and 11 without extra setup.

Example Video: [Citizen Kane (1941)](https://www.youtube.com/watch?v=ej8c0NwKW00&t=5s)

![UI Screenshot](resources/ui.png)

---

## Quick start (Windows 10 / 11)

```powershell
# 1. one-time setup: installs FFmpeg, CMake and the Visual Studio C compiler via winget
powershell -ExecutionPolicy Bypass -File .\setup.ps1

# 2. open a NEW terminal, then build
build.bat

# 3. add your API keys to config.json, then start the app
run.bat
```

Then put a movie in `movies\` (e.g. `movies\Citizen Kane.mp4`) and click **START GENERATION**.

> **No compiler?** Every push is built by GitHub Actions on Windows (see `.github/workflows/windows-build.yml`).
> Download the **AI-Movie-Shorts-Windows** zip from the latest run's *Artifacts* section. It contains the
> ready-to-run `.exe` files, `resources\`, `backgroundmusic\`, `config.json` and `run.bat`. You still need FFmpeg (`winget install Gyan.FFmpeg`).

---

## What it does

Given a movie file, AI-Movie-Shorts will:

1. **Fetch subtitles (SRT)** automatically (and convert timestamps to seconds)
2. **Optionally fetch a script** (used only as extra story context)
3. Ask **OpenAI** to generate a **clip plan** (timestamps + narration per clip)
4. Use **ElevenLabs** to generate voiceover audio for each clip
5. Use **FFmpeg** to:
   - cut each clip
   - time-stretch video to match narration length (speed-up capped at 1.75×)
   - concatenate all clips into one recap video
6. Optionally add **background music** from `backgroundmusic\`
7. Export:
   - `output\<MovieTitle>.mp4` (standard)
   - `tiktok_output\<MovieTitle>_vertical.mp4` (9:16 vertical)
8. Move the source movie to `movies_retired\`

### Quality guards: the recap must come from the movie
Before spending anything on ElevenLabs or rendering, the app checks two things:

1. **The subtitles are real and complete** (checked *before* OpenAI is called):
   - encoding is detected automatically: UTF-8, UTF-8 BOM, UTF-16 LE/BE (with or without BOM), Windows-1252
   - formatting tags (`<i>`, `<font>`, `{\an8}`) are removed
   - it stops if there is almost no dialogue (fewer than 40 lines), if the subtitles end before the
     middle of the movie, or if they run much longer than the movie (wrong movie or cut)
   - the log shows the line count, the encoding and a few sample lines, so you can see the dialogue was read
2. **The AI's script is about this movie**: it is rejected if it repeats sentences or whole narrations,
   uses generic trailer filler ("tensions boil over", "secrets surface", ...), names characters who never
   appear in the subtitles, or rarely mentions the characters at all. A rejected script is sent back to
   OpenAI once with the reasons. If it is still bad, the movie is **stopped**: no video, no ElevenLabs
   credits spent, and the script is saved as `output\<Title>_REJECTED_script.txt`.

The prompt asks for a storyteller recap based only on the subtitles: hook, setup, inciting incident,
rising action, climax, resolution, using character names and explaining *why* things happen. Every
accepted script is saved as **`output\<Title>_script.txt`**, with the main characters, a plot summary
and each clip's timestamps and narration.

It also **clears generated files in `clips\` each run** (while preserving the `clips\audio\` folder and only removing files inside it).

---

## UI (raylib)

The app (`movie_summary_bot.exe`) includes a simple **desktop UI window** (raylib) that lets you:

- Open key folders in **File Explorer** with one click:
  - `movies\`
  - `movies_retired\`
  - `output\`
  - `scripts\srt_files\` (subtitles/scripts cache)
- Start generation with a **START GENERATION** button
- View generation output in an in-app **log panel**, including FFmpeg errors (it is also printed to the console window)
- The window is **resizable** (the log panel grows with it)

There is also a headless **`movie_summary_cli.exe`** that runs one generation pass in the terminal (`run.bat cli`).

### UI font
The UI uses **Inter Regular** from `resources\Inter-Regular.ttf`.

You can start the `.exe` from anywhere, including double-clicking `build\Release\movie_summary_bot.exe` in Explorer.
On startup it looks for the project folder (the folder that contains `resources\` / `config.json`) next to the
executable and in up to 5 parent folders, then switches to it.

---

## Folder structure

- `movies\` — input `.mp4` files (filename should be the movie title)
- `movies_retired\` — processed movies are moved here
- `output\` — final horizontal recap videos
- `tiktok_output\` — final vertical recap videos
- `backgroundmusic\` — optional `.mp3` / `.m4a` music used as BGM (10 tracks included)
- `clips\` — temporary working files (auto-cleared each run)
  - `clips\audio\` — generated narration MP3s (files cleared each run; folder preserved)
- `scripts\srt_files\` — downloaded/cached subtitles and optional scripts
- `resources\`
  - `Inter-Regular.ttf` — UI font
  - `ui.png` — UI screenshot (for README)
  - `app.ico`, `app.rc` — Windows icon + version info embedded into the `.exe`
- `src\` — C source (see *Source layout* below)
- `setup.ps1`, `build.bat`, `run.bat` — Windows helper scripts

The runtime folders are created automatically.

---

## Requirements

| What | Why | Install |
|---|---|---|
| **FFmpeg + ffprobe** (on PATH) | cutting / encoding / mixing | `winget install Gyan.FFmpeg` |
| **CMake ≥ 3.20** | build system | `winget install Kitware.CMake` |
| **Visual Studio 2022/2026** or **Build Tools** with *Desktop development with C++*, **or** MinGW-w64 gcc | C compiler | `setup.ps1` installs Build Tools |
| Internet access during the first build | downloads the libraries | — |

`setup.ps1` installs all of these for you.

**You do NOT need** vcpkg, OpenSSL, Git, `curl.exe`, `unzip` or `pkg-config`. The build downloads and compiles
everything into one static `.exe`:

| Library | Used for | Notes |
|---|---|---|
| raylib 5.5 | UI | |
| libcurl 8.11 | HTTP | built with **Schannel**, the Windows TLS stack, so it uses the Windows certificate store |
| cJSON 1.7.18 | JSON | |
| miniz 3.0.2 | extracting subtitle `.zip` files | replaces the `unzip` tool |

---

## Config

Edit `config.json` in the project root:

```json
{
  "open_api_key": "YOUR_OPENAI_KEY",
  "elevenlabs_api_key": "YOUR_ELEVENLABS_KEY",
  "eleven_voice_id": "OPTIONAL_VOICE_ID",
  "eleven_model_id": "OPTIONAL_MODEL_ID"
}
```

Notes:
- `eleven_voice_id` defaults to `JBFqnCBsd6RMkjVDRZzb` if omitted.
- `eleven_model_id` defaults to `eleven_multilingual_v2` if omitted.
- Optional (new): `"openai_model"`. Defaults to `gpt-5.2`, the same model the original uses.
- Optional (new): `"target_minutes"`, the approximate recap length (about 6 clips per minute; for example
  `20` gives about 120 clips). If you leave it out, the original 20–30 clips (about 3–4 minutes) are used.
  Longer recaps cost more ElevenLabs credits.
- If the placeholder values (`OpenAIAPI` / `ElevenLabsAPI`) are still there, the app stops with a clear message.

> `config.json` is committed with placeholders only. After you add real keys, run
> `git update-index --skip-worktree config.json` so you never commit them by accident.

---

## Build

### Easiest
```bat
build.bat          :: or: build.bat clean
```
This uses Visual Studio (MSVC) if it's installed, and otherwise MinGW gcc (with Ninja or MinGW Makefiles).

### Manual (Developer PowerShell / cmd)
```bat
cmake -S . -B build -A x64
cmake --build build --config Release
```
Outputs:
- `build\Release\movie_summary_bot.exe` — UI
- `build\Release\movie_summary_cli.exe` — CLI

With MinGW: `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build` puts the executables in `build\`.

### CMake options
| Option | Default | Meaning |
|---|---|---|
| `BUILD_RAYLIB_UI` | ON | build `movie_summary_bot.exe` |
| `BUILD_CLI_APP` | ON | build `movie_summary_cli.exe` |
| `BUILD_SIMPLE_UI` | OFF | build the original's older light-theme UI (`src\ui_main.c`) |
| `BUILD_WINDOWS_GUI` | OFF | build the UI with no console window (logs are still shown in the in-app panel) |
| `USE_SYSTEM_CURL` | OFF on Windows | use an installed libcurl (e.g. vcpkg) instead of building it |

### Visual Studio IDE
Use *File → Open → Folder…* on this project. VS detects `CMakeLists.txt` automatically. Pick `movie_summary_bot.exe` as the startup item.

---

## Usage

1. Put movie files in `movies\`:
   - Example: `movies\Sinners.mp4`
2. Start the app: `run.bat` (or double-click the `.exe`)
3. In the UI:
   - Use the folder buttons to open `movies\`, `output\`, `scripts\srt_files\`, etc.
   - Click **START GENERATION** to run.
   - Watch progress in the in-app **Log** panel.

Outputs:
- `output\Sinners.mp4`
- `tiktok_output\Sinners_vertical.mp4`

Movies that already have an `output\<Title>.mp4` are skipped.

---

## Background music behavior

If `backgroundmusic\` contains `.mp3` or `.m4a` files, the program will:
- randomly choose tracks (only tracks longer than 60 s),
- trim from ~40s in (to skip intros),
- stitch enough pieces to cover the recap duration,
- mix narration louder (×2.5) + BGM quieter (×0.1).

If no music files exist, output will be narration-only.

---

## Subtitles / script fetching

- Subtitles (SRT) are auto-downloaded (subf2m) when missing and extracted from the downloaded zip.
- Script fetching (IMSDb) is **best effort** and **optional**. If it fails, the program continues with subtitles only.
  If OpenAI rejects the request because the script makes it too large, it automatically retries without the script.

If auto-fetch fails, you can manually add:
- `scripts\srt_files\<MovieTitle>.srt`

Then rerun.

---

## Vertical output

The vertical render:
- center-crops the movie to a narrower width,
- scales/pads to a 9:16 canvas,
- keeps audio if present.

Saved to `tiktok_output\`.

---

## What changed vs. the original (Windows port details)

| Area | Original (macOS/Linux focus) | This port |
|---|---|---|
| Running FFmpeg | `system()` / `popen()` through the shell | `CreateProcessW` directly, with no `cmd.exe`, so `&`, `%`, `^`, spaces and non-English characters in file names are safe. FFmpeg output goes to the UI log, and no console windows flash up |
| File names | narrow ANSI APIs (`fopen`, `FindFirstFileA`) | UTF-8 everywhere via wide Win32 APIs (`_wfopen`, `FindFirstFileW`, `MoveFileExW` …), so titles like `Amélie`, `千と千尋` and `Léon` work |
| UI compile on Windows | `main.c` included `<windows.h>` next to `raylib.h`, which fails to compile (`CloseWindow`, `DrawText`, `Rectangle` clash) | all Win32 code moved into `src\platform.c` |
| Unzipping subtitles | external `unzip` tool | built in (miniz) |
| libcurl | system package / pkg-config | built from source with Schannel |
| Downloads to file | `CURLOPT_WRITEFUNCTION = NULL` (crashes with a DLL libcurl on Windows) | explicit write callbacks |
| Replacing files | `rename()` (fails on Windows if the target exists) | `MoveFileExW(MOVEFILE_REPLACE_EXISTING)` |
| Fatal errors | `exit(1)`, which closes the whole UI window | the run is stopped, the error appears in the log, and the UI stays open (exit code −1) |
| ElevenLabs errors | JSON error saved as `.mp3` | HTTP status checked and the error message logged |
| BGM builder | could loop forever if no track was > 60 s | bounded retries |
| Filenames with `'` | broke FFmpeg concat lists | escaped properly |
| Working directory | had to be started from the project root | finds the project folder automatically |
| Console | — | UTF-8 console output, app icon + version info |

The pipeline logic itself is unchanged: prompt, clip counts (20–30), durations, speed cap, FFmpeg filters,
BGM mixing and vertical crop all match the original.

### Source layout
- `src\generator.c/.h` — the pipeline (subtitles → OpenAI → ElevenLabs → FFmpeg)
- `src\platform.c/.h` — Windows layer (UTF-8 file system, processes, threads, Explorer), plus a POSIX fallback
- `src\main.c` — raylib UI (`movie_summary_bot`)
- `src\cli.c` — CLI (`movie_summary_cli`)
- `src\ui_main.c` — the original's older simple UI (optional)
- `src\platform_open.c/.h` — kept for compatibility (opens folders)

It still builds on macOS/Linux (`cmake -S . -B build && cmake --build build`, using the system libcurl).

---

## Notes & troubleshooting

- **Filename matters**: `movies\My Movie.mp4` → treated as title `My Movie`
- **"ffmpeg/ffprobe not found in PATH"** → `winget install Gyan.FFmpeg`, then **close and reopen** the app/terminal so the new PATH applies. Check with `ffmpeg -version`.
- **Windows SmartScreen** may warn about an unsigned `.exe` you built or downloaded. Click *More info → Run anyway*.
- **"STOPPED before calling OpenAI: …subtitles…"** → the subtitle file is empty, broken, incomplete or for
  another movie/cut. A downloaded one is renamed to `<Title>.srt.bad`. Put a correct English `.srt` for your
  exact movie file at `scripts\srt_files\<Title>.srt` and press START again.
- **"The AI's script failed the quality check"** → it was sent back once automatically. If it still fails,
  read `output\<Title>_REJECTED_script.txt`. Usually the subtitles don't match the movie (check the sample
  lines in the log) or a weaker `openai_model` was set.
- If you see "plan count = 0" / "No plan returned" or missing clips, check:
  - your OpenAI key (the exact error from OpenAI is shown in the log)
  - network connectivity
  - that the SRT conversion output is not empty
- If the UI font looks wrong: confirm `resources\Inter-Regular.ttf` exists (the log shows a warning if it's missing).
- If vertical render fails, confirm:
  - FFmpeg is installed and in PATH
  - the input `output\<MovieTitle>.mp4` exists and is valid
- "Could not move … to movies_retired" → the movie is open in another program (e.g. a video player).
- Antivirus software can slow FFmpeg down a lot. Consider excluding the project folder.
- Build errors about downloads → the first build needs internet access to download raylib, curl, cJSON and miniz.

---

## Legal

Only process videos you own or have permission to use. Subtitle/script sources may be unavailable for some titles, and availability can change over time.
