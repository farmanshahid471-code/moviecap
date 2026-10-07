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
- GET  /w/api.php  (port 9102)        -> a fake Wikipedia search + article extract
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

_TRUNCATE_FIRST_BATCH = False      # set from --truncate-first-batch
_REASONING_FIRST_REPLY = False     # set from --reasoning-first-reply
_OVERSIZED_FIRST_PLAN = False      # set from --oversized-first-plan
_OVERLONG_PLANS = False            # set from --overlong-plans

STATE = {"plans": 0, "responses": 0, "tts": 0, "wiki_searches": 0, "wiki_extracts": 0,
         "batches": {}, "batch_seq": 0, "truncate_first_batch": False,
         "lock": threading.Lock()}


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


CLOSING_LINE = (
    "With that the story ends right here. Let us know in the comments how you liked this "
    "explanation and don't forget to like the video and subscribe to the channel."
)


def pad_narration(text, seconds, lang):
    """A real model writes to the requested length; the mock should too, so the
    length audit in the app has nothing to complain about.  ~2.7 words per
    second, and never after the closing line."""
    need = max(int(seconds * 2.7), 25)
    filler = ("Then the story turns again and the people in it have to decide "
              "what they are willing to lose.")
    while len(text.split()) < need:
        text = text + " " + filler
    return text


def m_pad(text, seconds):
    """The mock writing far more narration than the target allows."""
    need = max(int(seconds * 2.7), 25)
    filler = ("Then the story turns again and the people in it have to decide "
              "what they are willing to lose.")
    while len(text.split()) < need:
        text = text + " " + filler
    return text


def narration_for(i, clips_wanted, title, lang, names, target_sec=0):
    """A narration that follows the same script rules the real prompt asks for:
    no channel intro, present tense, and the exact closing sentence at the end.
    Only a stand-in for the real model - the words do not matter, the shape does.
    """
    if i == 0:
        text = (
            f"The story begins in {title}. "
            f"This is mock narration number 1, written in {lang}. "
            f"Someone makes a choice here and the story starts moving."
        )
    elif names:
        text = (
            f"The story keeps moving and {names[i % len(names)]} is right in the middle of it. "
            f"Something goes wrong and there is no time to think."
        )
    else:
        text = (
            f"The story keeps moving. Something goes wrong and there is no time to think."
        )

    if target_sec > 0:
        text = pad_narration(text, target_sec, lang)

    if i == clips_wanted - 1:
        text += " " + (CLOSING_LINE if lang.lower().startswith("english")
                       else "That is where this story ends.")
    return text


def build_plan(body, clips_wanted):
    """Return {'clips': [...]} shaped like the real prompt asks for."""
    prompt = ""
    for item in body.get("input", []):
        if item.get("role") == "user":
            prompt = item.get("content", "")
    return plan_from_prompt(prompt, clips_wanted)


def plan_from_prompt(prompt, clips_wanted, honour_length=False):
    """The same plan, built from the prompt text alone - which is all a batched
    Messages request carries (its params hold system + messages)."""
    target_sec = 0
    m = re.search(r"Target clip length:\s*(\d+)-(\d+) seconds each", prompt or "")
    if m:
        # the middle of the asked range: a real model plans windows of about the
        # length the prompt asks for, so the mock does too
        target_sec = (int(m.group(1)) + int(m.group(2))) // 2
        if target_sec < 4:
            target_sec = 4
    if honour_length:
        m = re.search(r"Number of clips:\s*(\d+)", prompt or "")
        if m:
            clips_wanted = max(1, min(50, int(m.group(1))))        # the asked count
    subs = ""
    if "INPUT A" in prompt:
        subs = prompt.split("INPUT A", 1)[1].split("INPUT B", 1)[0]

    lo, hi = guess_clip_window(subs)
    span = max(hi - lo, clips_wanted * 8)
    step = span / float(clips_wanted + 1)

    title_m = re.search(r"^Movie:\s*(.+?)\s*$", prompt, re.M)
    title = title_m.group(1).strip() if title_m else "this movie"
    lang_m = re.search(r"^Narration language:\s*(.+?)\s*$", prompt, re.M)
    lang = lang_m.group(1).strip() if lang_m else "English"

    names = names_from_prompt(prompt)
    if names:
        print(f"[mock-openai] using character names from INPUT C: {', '.join(names)}", flush=True)

    clips = []
    clip_len = target_sec if target_sec > 0 else 12
    for i in range(clips_wanted):
        start = int(lo + step * (i + 0.5))
        end = start + clip_len
        if end > hi:
            end = hi
        if end - start < 4:
            start = max(lo, end - 6)
        clips.append({
            "start": start,
            "end": end,
            "narration": narration_for(i, clips_wanted, title, lang, names, target_sec),
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
        # --- Anthropic Message Batches: status + results ------------------
        m = re.match(r"^/anthropic/v1/messages/batches/([^/]+)/results$", self.path)
        if m:
            return self._send_batch_results(m.group(1))
        m = re.match(r"^/anthropic/v1/messages/batches/([^/]+)$", self.path)
        if m:
            with STATE["lock"]:
                batch = STATE["batches"].get(m.group(1))
            if not batch:
                return self._send(404, {"type": "error", "error": {
                    "type": "not_found_error", "message": "no such batch"}})
            done = len(batch["requests"])
            return self._send(200, {
                "id": m.group(1),
                "type": "message_batch",
                "processing_status": "ended",
                "request_counts": {"processing": 0, "succeeded": done,
                                   "errored": 0, "canceled": 0, "expired": 0},
            })
        self._send(200, {"ok": True})

    def _send_batch_results(self, batch_id):
        with STATE["lock"]:
            batch = STATE["batches"].get(batch_id)
        if not batch:
            return self._send(404, {"type": "error", "error": {
                "type": "not_found_error", "message": "no such batch"}})
        lines = []
        for req in batch["requests"]:
            params = req.get("params", {})
            prompt = ""
            for msg in params.get("messages", []):
                if msg.get("role") == "user":
                    prompt = msg.get("content", "")
            plan = plan_from_prompt(prompt, batch["clips"], honour_length=True)
            text = json.dumps(plan)
            stop_reason = "end_turn"
            if STATE["truncate_first_batch"] and batch["seq"] == 1:
                # a reply that ran out of output budget: the JSON just stops
                text = text[: len(text) // 2]
                stop_reason = "max_tokens"
                print(f"[mock-openai] batch {batch_id}: replying CUT OFF "
                      f"(stop_reason=max_tokens)", flush=True)
            lines.append(json.dumps({
                "custom_id": req.get("custom_id"),
                "result": {"type": "succeeded", "message": {
                    "id": "msg_mock",
                    "type": "message",
                    "role": "assistant",
                    "model": params.get("model", "mock"),
                    "stop_reason": stop_reason,
                    "content": [{"type": "text", "text": text}],
                    "usage": {"input_tokens": 100, "output_tokens": 500},
                }},
            }))
        print(f"[mock-openai] batch {batch_id}: {len(lines)} result(s)", flush=True)
        self._send(200, ("\n".join(lines) + "\n").encode(), "application/x-jsonlines")

    def do_POST(self):
        n = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(n) if n else b"{}"
        try:
            body = json.loads(raw or b"{}")
        except Exception:
            body = {}

        if self.path == "/anthropic/v1/messages/batches":
            STATE["truncate_first_batch"] = _TRUNCATE_FIRST_BATCH

        with STATE["lock"]:
            STATE["plans"] += 1
            if self.path.endswith("/responses"):
                STATE["responses"] += 1

        # The app must not impose an output-token limit on the FIRST plan request
        # (the provider's own maximum applies).  A later retry may name one - the
        # app only does that once the provider said the answer never got written.
        if self.path.endswith("/responses") or self.path.endswith("/chat/completions"):
            limited = ("max_output_tokens" in body or "max_completion_tokens" in body
                       or "max_tokens" in body)
            first = STATE["responses"] <= 1
            if not limited:
                print("[mock-openai] plan request carries no output-token limit",
                      flush=True)
            elif first:
                print("[mock-openai] WARNING: FIRST plan request carried an "
                      "output-token limit", flush=True)
            else:
                limit = (body.get("max_output_tokens") or body.get("max_completion_tokens")
                         or body.get("max_tokens"))
                print(f"[mock-openai] retry request names an explicit output budget "
                      f"({limit} tokens)", flush=True)

        # --- Anthropic Message Batches: create ---------------------------
        if self.path == "/anthropic/v1/messages/batches":
            reqs = body.get("requests", [])
            with STATE["lock"]:
                STATE["batch_seq"] += 1
                batch_id = f"msgbatch_mock{STATE['batch_seq']:03d}"
                STATE["batches"][batch_id] = {"requests": reqs, "clips": 8,
                                              "seq": STATE["batch_seq"]}
            for req in reqs:
                if "stream" in req.get("params", {}):
                    return self._send(400, {"type": "error", "error": {
                        "type": "invalid_request_error",
                        "message": "streaming is not supported in a batch"}})
            print(f"[mock-openai] batch {batch_id} created with {len(reqs)} request(s)",
                  flush=True)
            return self._send(200, {
                "id": batch_id,
                "type": "message_batch",
                "processing_status": "in_progress",
                "request_counts": {"processing": len(reqs), "succeeded": 0,
                                   "errored": 0, "canceled": 0, "expired": 0},
            })

        if not self.path.endswith("/responses"):
            return self._send(404, {"error": {"message": "not found", "type": "invalid_request_error"}})

        # A thinking model that spent the whole allowance on its reasoning: 200 OK
        # with no answer text at all (this is what deepseek-v4-pro did).
        if _REASONING_FIRST_REPLY and STATE["responses"] <= 1:
            print("[mock-openai] replying like a thinking model out of output room",
                  flush=True)
            return self._send(200, {
                "id": "resp_reasoning_only",
                "object": "response",
                "status": "incomplete",
                "incomplete_details": {"reason": "max_output_tokens"},
                "model": body.get("model", "mock"),
                "output": [{
                    "type": "reasoning",
                    "id": "rs_1",
                    "status": "incomplete",
                    "content": [{"type": "reasoning_text",
                                 "text": "We need answer JSON only. Need build narration "
                                         "for every clip. Need choose ranges from INPUT A..."}],
                }],
            })

        wanted = 8
        prompt = ""
        for item in body.get("input", []):
            if item.get("role") == "user":
                prompt = item.get("content", "")
        m = re.search(r"Number of clips:\s*(\d+)", prompt or "")
        if m:
            wanted = max(1, int(m.group(1)))
        if _OVERSIZED_FIRST_PLAN and STATE["responses"] <= 1:
            over = wanted * 3
            print(f"[mock-openai] replying with {over} clips for the {wanted}-clip "
                  f"target (oversized plan, like the reported 249-for-111 run)",
                  flush=True)
            wanted = over
        plan = build_plan(body, wanted)

        if _OVERLONG_PLANS:
            # A model that writes far more narration than the requested length
            # allows (the "20 minute recap comes out at 26" report).  Padding
            # happens before the closing line, which stays at the very end.
            tgt = 0
            tm = re.search(r"Target clip length:\s*(\d+)-(\d+) seconds each",
                           prompt or "")
            if tm:
                tgt = int(((int(tm.group(1)) + int(tm.group(2))) // 2) * 2.5)
            for _c in plan["clips"]:
                narr = _c["narration"]
                outro = ""
                if CLOSING_LINE in narr:
                    narr, _, _ = narr.rpartition(CLOSING_LINE)
                    outro = " " + CLOSING_LINE
                _c["narration"] = m_pad(narr, tgt or 40) + outro
            print(f"[mock-openai] writing an over-long plan ({len(plan['clips'])} clips, "
                  f"about {tgt or 40}s of narration each for a target of about "
                  f"{(tgt or 40) / 2.5:.0f}s)", flush=True)

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



# ---------------------------------------------------------------- Wikipedia ---

WIKI_ARTICLE = {
    "lead": "Test Movie is a 2026 thriller film directed by someone.",
    "plot": (
        "A detective named Mara Quinn tracks a thief named Axel Vance. "
        "Her partner is a sergeant named Dale Ryker, a man who lost his brother to "
        "Vance years earlier. The trail leads them from the flooded warehouse "
        "district to the old docks, where Vance keeps the ledger he uses to "
        "blackmail half the city. Quinn's chief, Commissioner Odell, orders her "
        "to drop the case, because Vance pays him too. She refuses, and Ryker "
        "backs her. They set a trap with a copy of the ledger. Vance sees through "
        "it and takes Quinn hostage on the ferry. Ryker boards the ferry and "
        "fights Vance on the deck. Quinn breaks free, grabs the ledger, and Vance "
        "goes over the rail into the river. Odell is arrested that night. Quinn "
        "hands in her badge, keeps the ledger, and walks away with Ryker."
    ),
    "cast": "Mara Quinn ... an unnamed actress\nAxel Vance ... an unnamed actor",
}


def wiki_article_text():
    return (WIKI_ARTICLE["lead"] + "\n\n== Plot ==\n" + WIKI_ARTICLE["plot"] +
            "\n\n== Cast ==\n" + WIKI_ARTICLE["cast"] + "\n")


class WikiHandler(BaseHTTPRequestHandler):
    """Offline stand-in for the Wikipedia action API the pipeline calls."""

    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        sys.stderr.write("[mock-wiki] " + (fmt % args) + "\n")

    def _send(self, code, payload):
        data = json.dumps(payload).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        from urllib.parse import urlparse, parse_qs, unquote
        q = parse_qs(urlparse(self.path).query)
        action = (q.get("action") or [""])[0]
        if action == "query" and (q.get("list") or [""])[0] == "search":
            wanted = unquote((q.get("srsearch") or ["movie"])[0])
            wanted = re.sub(r"\s*film\s*$", "", wanted, flags=re.I).strip() or "Test Movie"
            title = wanted[:1].upper() + wanted[1:] + " (2026 film)"
            with STATE["lock"]:
                STATE["wiki_searches"] += 1
            print(f"[mock-wiki] search -> {title}", flush=True)
            return self._send(200, {"query": {"search": [
                {"title": title, "pageid": 123, "snippet": "a film"},
                {"title": wanted + " (soundtrack)", "pageid": 124, "snippet": "an album"},
            ]}})
        if action == "query" and "extracts" in q.get("prop", [""])[0]:
            with STATE["lock"]:
                STATE["wiki_extracts"] += 1
            return self._send(200, {"query": {"pages": {"123": {
                "pageid": 123, "title": "Test Movie (2026 film)",
                "extract": wiki_article_text()}}}})
        self._send(200, {"query": {}})


def names_from_prompt(prompt):
    """The character names the model was told to use, taken from INPUT C."""
    if "INPUT C" not in prompt:
        return []
    block = prompt.split("INPUT C", 1)[1].split("STEP 1", 1)[0]
    return re.findall(r"named ([A-Z][a-z]+ [A-Z][a-z]+)", block)



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
    ap.add_argument("--wiki-port", type=int, default=9102)
    ap.add_argument("--reasoning-first-reply", action="store_true",
                    help="answer the FIRST /responses call the way a thinking "
                         "model with a small output allowance does: status="
                         "incomplete, incomplete_details.reason=max_output_tokens, "
                         "and an output array holding nothing but a reasoning "
                         "item. Later calls answer normally.")
    ap.add_argument("--overlong-plans", action="store_true",
                    help="answer EVERY /responses plan request with about 2.5x the "
                         "narration the asked clip length allows - the reported run "
                         "where a 20 minute recap came out longer than 20 minutes "
                         "because the model ignored the length budget.")
    ap.add_argument("--oversized-first-plan", action="store_true",
                    help="answer the FIRST /responses call with THREE times the "
                         "clips the prompt asks for - the reported run where the "
                         "model planned 249 clips for a 111-clip (20 minute) "
                         "target. Later calls answer normally.")
    ap.add_argument("--truncate-first-batch", action="store_true",
                    help="answer the FIRST batch with a reply that stops at the "
                         "output limit (stop_reason=max_tokens), the way an "
                         "under-budgeted plan comes back. Later batches answer "
                         "completely. Used to test the escalation path.")
    args = ap.parse_args()

    global _TRUNCATE_FIRST_BATCH, _REASONING_FIRST_REPLY, _OVERSIZED_FIRST_PLAN
    global _OVERLONG_PLANS
    _TRUNCATE_FIRST_BATCH = bool(args.truncate_first_batch)
    _REASONING_FIRST_REPLY = bool(args.reasoning_first_reply)
    _OVERSIZED_FIRST_PLAN = bool(args.oversized_first_plan)
    _OVERLONG_PLANS = bool(args.overlong_plans)

    a = ThreadingHTTPServer(("127.0.0.1", args.openai_port), OpenAIHandler)
    b = ThreadingHTTPServer(("127.0.0.1", args.eleven_port), ElevenHandler)
    c = ThreadingHTTPServer(("127.0.0.1", args.tts_port), TtsHandler)
    d = ThreadingHTTPServer(("127.0.0.1", args.wiki_port), WikiHandler)

    print(f"[mock] OpenAI     -> http://127.0.0.1:{args.openai_port}/v1", flush=True)
    print(f"[mock] ElevenLabs -> http://127.0.0.1:{args.eleven_port}/v1", flush=True)
    print(f"[mock] XTTS/Piper -> http://127.0.0.1:{args.tts_port}/tts_to_audio/ and /synthesize", flush=True)
    print(f"[mock] Wikipedia  -> http://127.0.0.1:{args.wiki_port}/w/api.php", flush=True)

    for srv in (b, c, d):
        threading.Thread(target=srv.serve_forever, daemon=True).start()
    try:
        a.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
