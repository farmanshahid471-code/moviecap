#!/usr/bin/env python3
"""
mock_api_server.py - offline stand-in for the OpenAI + ElevenLabs endpoints.

It lets you exercise the whole pipeline (plan -> narration -> ffmpeg cuts ->
concat -> BGM -> vertical render) without spending API credits, and it is what
the CI/smoke tests can use.

    python3 tools/mock_api_server.py --openai-port 9100 --eleven-port 9101 --tts-port 8020

Then point config.json at it:

    "openai_base_url":     "http://127.0.0.1:9100/v1",
    "elevenlabs_base_url": "http://127.0.0.1:9101/v1"

- POST /v1/responses                 -> a clip plan in OpenAI "responses" shape
- POST /v1/text-to-speech/<voice_id> -> a real MP3 (sine tone) of a varying length
- POST /tts_to_audio/                -> WAV, the free XTTS v2 server contract
- POST /synthesize                   -> WAV, the free Piper http_server contract
- POST /audio/speech                 -> MP3, any OpenAI-compatible TTS endpoint

The clip plan is derived from the subtitle text it is given, so the timestamps
stay inside the movie that is actually being processed.
"""

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

STATE = {"plans": 0, "tts": 0, "lock": threading.Lock()}


def make_tone_mp3(seconds, path, freq=440.0):
    """Render a short MP3 with ffmpeg (falls back to a tiny silent frame)."""
    try:
        subprocess.run(
            ["ffmpeg", "-y", "-hide_banner", "-loglevel", "error",
             "-f", "lavfi", "-i", f"sine=frequency={int(freq)}:duration={seconds}",
             "-c:a", "libmp3lame", "-b:a", "128k", path],
            check=True,
        )
        return True
    except Exception:
        # Minimal valid MP3 frame pattern (silence) as a last resort.
        frame = bytes.fromhex("fff360c4000000000000000000000000000000000000000000000000000000000")
        with open(path, "wb") as f:
            for _ in range(int(seconds * 38) + 1):
                f.write(frame)
        return False


def make_tone_wav(seconds, path, freq=330.0):
    """Render a short WAV with ffmpeg - what XTTS and Piper actually return."""
    subprocess.run(
        ["ffmpeg", "-y", "-hide_banner", "-loglevel", "error",
         "-f", "lavfi", "-i", f"sine=frequency={int(freq)}:duration={seconds}",
         "-c:a", "pcm_s16le", "-ar", "24000", "-ac", "1", path],
        check=True,
    )
    return True


def guess_clip_window(text):
    """Roughly where the subtitles end, in seconds."""
    stamps = [int(m) for m in re.findall(r"^\s*(\d+)\s*-->", text, re.M)]
    if stamps:
        return 1, max(stamps) + 10
    return 1, 600


def build_plan(body, clips_wanted):
    """Return {'clips': [...]} shaped like the real prompt asks for."""
    prompt = ""
    for item in body.get("input", []):
        if item.get("role") == "user":
            prompt = item.get("content", "")

    subs = ""
    if "INPUT A" in prompt:
        subs = prompt.split("INPUT A", 1)[1].split("INPUT B", 1)[0]

    lo, hi = guess_clip_window(subs)
    span = max(hi - lo, clips_wanted * 8)
    step = span / float(clips_wanted + 1)

    title_m = re.search(r"The first narration must start with: .Here we go, let.s go over the movie (.+?)\.",
                        prompt)
    title = title_m.group(1).strip() if title_m else "this movie"

    clips = []
    for i in range(clips_wanted):
        start = int(lo + step * (i + 0.5))
        end = start + 12
        if end > hi:
            end = hi
        if end - start < 4:
            start = max(lo, end - 6)
        clips.append({
            "start": start,
            "end": end,
            "narration": (
                f"Here we go, let's go over the movie {title}. "
                f"This is mock narration number {i + 1}. "
                f"Nothing exciting happens here, but the visuals keep moving. "
                f"Let's keep going." if i == 0 else
                f"Clip {i + 1} picks the story back up again. "
                f"The scene changes and the tension builds. "
                f"We are getting closer to the ending now."
            ),
        })
    return {"clips": clips}


class OpenAIHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        sys.stderr.write("[mock-openai] " + (fmt % args) + "\n")

    def _send(self, code, payload, ctype="application/json"):
        data = payload if isinstance(payload, bytes) else json.dumps(payload).encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        if self.path.startswith("/v1/models"):
            return self._send(200, {"data": [{"id": "mock-model"}]})
        self._send(200, {"ok": True})

    def do_POST(self):
        n = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(n) if n else b"{}"
        try:
            body = json.loads(raw or b"{}")
        except Exception:
            body = {}

        with STATE["lock"]:
            STATE["plans"] += 1

        if not self.path.endswith("/responses"):
            return self._send(404, {"error": {"message": "not found", "type": "invalid_request_error"}})

        wanted = 8
        plan = build_plan(body, wanted)
        text = json.dumps(plan)
        with STATE["lock"]:
            print(f"[mock-openai] returning {len(plan['clips'])} clips", flush=True)

        self._send(200, {
            "id": "resp_mock",
            "object": "response",
            "status": "completed",
            "model": body.get("model", "mock"),
            "output": [{
                "type": "message",
                "role": "assistant",
                "content": [{"type": "output_text", "text": text}],
            }],
        })


class ElevenHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        sys.stderr.write("[mock-elevenlabs] " + (fmt % args) + "\n")

    def do_GET(self):
        payload = json.dumps({"ok": True}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def do_POST(self):
        n = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(n) if n else b"{}"
        try:
            body = json.loads(raw or b"{}")
        except Exception:
            body = {}

        if not self.headers.get("xi-api-key"):
            err = json.dumps({"detail": {"message": "missing api key"}}).encode()
            self.send_response(401)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(err)))
            self.end_headers()
            self.wfile.write(err)
            return

        with STATE["lock"]:
            STATE["tts"] += 1
            idx = STATE["tts"]

        # Vary the narration length so the speed-cap logic gets exercised.
        seconds = 2.0 + (idx % 5) * 1.5
        fd, tmp = tempfile.mkstemp(suffix=".mp3")
        os.close(fd)
        make_tone_mp3(seconds, tmp, freq=220 + (idx % 8) * 60)

        with open(tmp, "rb") as f:
            data = f.read()
        os.unlink(tmp)

        self.send_response(200)
        self.send_header("Content-Type", "audio/mpeg")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)
        print(f"[mock-elevenlabs] clip {idx}: {seconds:.1f}s of audio ({len(data)} bytes)", flush=True)


class TtsHandler(BaseHTTPRequestHandler):
    """
    One server that speaks the three free/local narration contracts:

      POST /tts_to_audio/  XTTS v2  {text, speaker_wav, language}      -> audio/wav
      GET  /speakers       XTTS v2  -> ["speaker.wav"]
      POST /synthesize     Piper    {text, voice}                     -> audio/wav
      POST /audio/speech   OpenAI-compatible {model, input, voice}    -> audio/mpeg
    """

    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        sys.stderr.write("[mock-tts] " + (fmt % args) + "\n")

    def _json(self, code, obj):
        payload = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def do_GET(self):
        if self.path.startswith("/speakers"):
            self._json(200, ["demo_speaker.wav"])
        else:
            self._json(200, {"ok": True, "routes": ["/tts_to_audio/", "/synthesize", "/audio/speech"]})

    def do_POST(self):
        n = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(n) if n else b"{}"
        try:
            body = json.loads(raw or b"{}")
        except Exception:
            body = {}

        path = self.path.split("?")[0]
        if path == "/tts_to_audio/":
            kind, text = "xtts", body.get("text", "")
            if not body.get("speaker_wav"):
                return self._json(400, {"detail": "speaker_wav is required"})
            if not body.get("language"):
                return self._json(400, {"detail": "language is required"})
        elif path == "/synthesize":
            kind, text = "piper", body.get("text", "")
        elif path.endswith("/audio/speech"):
            kind, text = "openai", body.get("input", "")
            if not self.headers.get("Authorization"):
                return self._json(401, {"error": {"message": "missing api key"}})
        else:
            return self._json(404, {"detail": f"no route for {path}"})

        if not text:
            return self._json(400, {"detail": "text is required"})

        with STATE["lock"]:
            STATE["tts"] += 1
            idx = STATE["tts"]

        seconds = 2.0 + (idx % 5) * 1.5
        fd, tmp = tempfile.mkstemp(suffix=".wav" if kind != "openai" else ".mp3")
        os.close(fd)
        if kind == "openai":
            make_tone_mp3(seconds, tmp, freq=300 + (idx % 8) * 50)
        else:
            make_tone_wav(seconds, tmp, freq=300 + (idx % 8) * 50)
        with open(tmp, "rb") as f:
            data = f.read()
        os.unlink(tmp)

        self.send_response(200)
        self.send_header("Content-Type", "audio/mpeg" if kind == "openai" else "audio/wav")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)
        print(f"[mock-tts] {kind} clip {idx}: {seconds:.1f}s "
              f"({'mp3' if kind == 'openai' else 'wav'}, {len(data)} bytes)", flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--openai-port", type=int, default=9100)
    ap.add_argument("--eleven-port", type=int, default=9101)
    ap.add_argument("--tts-port", type=int, default=8020)
    args = ap.parse_args()

    a = ThreadingHTTPServer(("127.0.0.1", args.openai_port), OpenAIHandler)
    b = ThreadingHTTPServer(("127.0.0.1", args.eleven_port), ElevenHandler)
    c = ThreadingHTTPServer(("127.0.0.1", args.tts_port), TtsHandler)

    print(f"[mock] OpenAI     -> http://127.0.0.1:{args.openai_port}/v1", flush=True)
    print(f"[mock] ElevenLabs -> http://127.0.0.1:{args.eleven_port}/v1", flush=True)
    print(f"[mock] XTTS/Piper -> http://127.0.0.1:{args.tts_port}/tts_to_audio/ and /synthesize", flush=True)

    for srv in (b, c):
        threading.Thread(target=srv.serve_forever, daemon=True).start()
    try:
        a.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
