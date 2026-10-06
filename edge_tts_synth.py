#!/usr/bin/env python
"""Synthesize narration audio with Microsoft Edge neural TTS (free).

Called by the AI-Movie-Shorts app:
    python edge_tts_synth.py --voice en-US-GuyNeural --text-file in.txt --out out.mp3

The text is read from a file (never from the command line) so quotes and
apostrophes in the script can never break the call. Requires internet.
"""
import argparse
import asyncio
import sys


def main() -> int:
    ap = argparse.ArgumentParser(description="Edge neural TTS synthesis")
    ap.add_argument("--voice", default="en-US-GuyNeural")
    ap.add_argument("--text-file", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--rate", default="+0%",
                    help="speaking rate offset, e.g. +10%% or -10%%")
    args = ap.parse_args()

    try:
        with open(args.text_file, encoding="utf-8") as f:
            text = f.read().strip()
    except OSError as exc:
        print(f"edge_tts_synth: cannot read text file: {exc}", file=sys.stderr)
        return 2
    if not text:
        print("edge_tts_synth: empty text", file=sys.stderr)
        return 2

    try:
        import edge_tts
    except ImportError:
        print("edge_tts_synth: the edge-tts package is not installed. "
              "Double-click run.bat once to install it.", file=sys.stderr)
        return 3

    async def run() -> None:
        tts = edge_tts.Communicate(text, args.voice, rate=args.rate)
        await tts.save(args.out)

    try:
        asyncio.run(run())
    except Exception as exc:  # network errors, voice not found, ...
        print(f"edge_tts_synth: synthesis failed: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
