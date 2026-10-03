#!/usr/bin/env python
"""Transcribe an audio file into an SRT with faster-whisper (runs on CPU).

Called by the AI-Movie-Shorts app when a movie has no subtitle file:
    python whisper_transcribe.py --model small --audio audio.wav --out movie.srt --cache-dir <tools>\\hf-cache

The model (~150 MB - 1.5 GB depending on size) is downloaded once into
--cache-dir so nothing is written to C:.
"""
import argparse
import os
import sys


def fmt_ts(sec: float) -> str:
    ms = int(round(sec * 1000))
    h, ms = divmod(ms, 3600000)
    m, ms = divmod(ms, 60000)
    s, ms = divmod(ms, 1000)
    return f"{h:02d}:{m:02d}:{s:02d},{ms:03d}"


def main() -> int:
    ap = argparse.ArgumentParser(description="faster-whisper audio -> SRT")
    ap.add_argument("--model", default="small",
                    help="tiny | base | small | medium | large-v3 (default: small)")
    ap.add_argument("--audio", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--cache-dir", default="")
    args = ap.parse_args()

    if args.cache_dir:
        os.makedirs(args.cache_dir, exist_ok=True)
        # Set before importing faster_whisper so models never land on C:.
        os.environ["HF_HOME"] = args.cache_dir
        os.environ["HF_HUB_CACHE"] = args.cache_dir
        os.environ["XDG_CACHE_HOME"] = args.cache_dir

    try:
        import faster_whisper
        import ctranslate2
        from faster_whisper import WhisperModel
        print(f"[whisper] python={sys.version.split()[0]} "
              f"faster-whisper={faster_whisper.__version__} "
              f"ctranslate2={ctranslate2.__version__}", flush=True)
    except BaseException as exc:
        print(f"whisper_transcribe: faster-whisper is not usable: {exc}", flush=True)
        return 3

    if not os.path.exists(args.audio):
        print(f"whisper_transcribe: audio file not found: {args.audio}", file=sys.stderr)
        return 2

    print(f"[whisper] loading model '{args.model}' (first run downloads it)...", flush=True)
    model = None
    last_exc = None
    # If huggingface.co is unreachable (some regions/ISPs), retry via mirror.
    for attempt, endpoint in enumerate((None, "https://hf-mirror.com")):
        try:
            if endpoint:
                os.environ["HF_ENDPOINT"] = endpoint
                print(f"[whisper] retrying the download via {endpoint} ...", flush=True)
            model = WhisperModel(args.model, device="cpu", compute_type="int8")
            break
        except BaseException as exc:
            last_exc = exc
            print(f"[whisper] model load attempt {attempt + 1} failed: "
                  f"{type(exc).__name__}: {exc}", flush=True)
    if model is None:
        print(f"whisper_transcribe: model load failed: {last_exc}", flush=True)
        return 1

    print("[whisper] transcribing - for a full movie this can take a while on CPU...", flush=True)
    try:
        segments, info = model.transcribe(args.audio, vad_filter=True)
        n = 0
        with open(args.out, "w", encoding="utf-8") as f:
            for seg in segments:
                n += 1
                f.write(f"{n}\n{fmt_ts(seg.start)} --> {fmt_ts(seg.end)}\n"
                        f"{seg.text.strip()}\n\n")
                if n % 25 == 0:
                    print(f"[whisper] {n} segments, at {seg.end:.0f}s", flush=True)
    except Exception as exc:
        print(f"whisper_transcribe: transcription failed: {exc}", flush=True)
        return 1

    print(f"[whisper] done: {n} segments (language={info.language}, "
          f"probability={info.language_probability:.2f}) -> {args.out}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
