#!/usr/bin/env python3
"""Mock subf2m / IMSDb / OpenAI / ElevenLabs for pipeline tests (Linux/macOS dev only).

Scenario is read from tests/.scenario on every request:
  good            OpenAI returns a grounded, varied plan
  loop            OpenAI always returns the generic looping filler (must be rejected)
  loop_then_good  first plan request loops, the retry (with feedback) is good
Subtitle served by the fake subf2m: tests/.srt_to_serve (raw bytes, any encoding).
Every OpenAI prompt is saved to tests/.last_prompt_N.txt
"""
import http.server, json, io, zipfile, subprocess, re, os, urllib.parse
HERE = os.path.dirname(os.path.abspath(__file__))
def scenario():
    try: return open(os.path.join(HERE, '.scenario')).read().strip()
    except OSError: return 'good'
plan_requests = 0
GENERIC = ["The story wastes no time and throws our characters straight into trouble.",
           "Tensions boil over as old allies turn into brand new enemies.",
           "Our hero digs deep.", "Secrets surface."]
def mp3(sec):
    return subprocess.run(['ffmpeg','-hide_banner','-loglevel','error','-f','lavfi','-i',f'sine=frequency=440:duration={sec}',
                           '-c:a','libmp3lame','-f','mp3','-'], capture_output=True).stdout
class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def send(self, code, body, ctype='text/html'):
        if isinstance(body, str): body = body.encode()
        self.send_response(code); self.send_header('Content-Type', ctype)
        self.send_header('Content-Length', str(len(body))); self.end_headers(); self.wfile.write(body)
    def do_GET(self):
        p = urllib.parse.unquote(self.path)
        m = re.match(r'^/subf2m/subtitles/([^/]+)/english$', p)
        if m: return self.send(200, f'<a href="/subtitles/{m[1]}/english/42">sub</a>')
        m = re.match(r'^/subf2m/subtitles/([^/]+)/english/42$', p)
        if m: return self.send(200, f'<a href="/subtitles/{m[1]}/english/42/download">dl</a>')
        if p.endswith('/english/42/download'):
            b = io.BytesIO()
            with zipfile.ZipFile(b, 'w') as z:
                z.writestr('Movie.srt', open(os.path.join(HERE, '.srt_to_serve'), 'rb').read())
            return self.send(200, b.getvalue(), 'application/zip')
        return self.send(404, 'nope')
    def do_POST(self):
        global plan_requests
        body = self.rfile.read(int(self.headers.get('Content-Length', 0)))
        if self.path == '/openai/v1/responses':
            plan_requests += 1
            req = json.loads(body)
            prompt = req['input'][-1]['content']
            k = 1
            while os.path.exists(os.path.join(HERE, f'.last_prompt_{k}.txt')): k += 1
            open(os.path.join(HERE, f'.last_prompt_{k}.txt'), 'w').write(
                json.dumps(req['input'], indent=1, ensure_ascii=False))
            sc = scenario()
            is_retry = len(req['input']) > 2
            if sc == 'loop' or (sc == 'loop_then_good' and not is_retry):
                clips = []
                for i in range(12):
                    s = 30 + i * 45
                    nar = ("Here we go, let's go over the movie. " if i == 0 else "") + " ".join(GENERIC)
                    clips.append({"start": s, "end": s + 10, "narration": nar})
                plan = {"main_characters": ["Hero"], "story_summary": "A hero has an adventure.", "clips": clips}
            else:
                names = ["Mara", "Theo", "Captain Vell"]
                clips = []
                for i in range(12):
                    s = 30 + i * 45
                    nar = (f"Here we go, let's go over the movie. " if i == 0 else "") + \
                          f"Scene {i+1}: {names[i%3]} finds clue number {i+1} at the lighthouse. " \
                          f"{names[(i+1)%3]} doesn't trust it, and part {i+1} of the plan changes."
                    clips.append({"start": s, "end": s + 10, "narration": nar})
                plan = {"main_characters": names, "story_summary": "Mara and Theo chase Captain Vell's map to the lighthouse.", "clips": clips}
            resp = {"output": [{"type": "message", "content": [{"type": "output_text", "text": json.dumps(plan)}]}]}
            return self.send(200, json.dumps(resp), 'application/json')
        if self.path.startswith('/eleven/v1/text-to-speech/'):
            return self.send(200, mp3(4), 'audio/mpeg')
        return self.send(404, 'nope')
http.server.ThreadingHTTPServer(('127.0.0.1', 8765), H).serve_forever()
