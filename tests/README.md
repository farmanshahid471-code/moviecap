# Pipeline tests (developer only, Linux/macOS)

Runs the **real** `src/generator.c` end to end against a local mock of subf2m, IMSDb,
OpenAI and ElevenLabs (`mock_server.py`). Production code is not modified:
`test_main.c` wraps `curl_easy_perform` to redirect the API hosts to `127.0.0.1:8765`.

What `run_scenarios.sh` checks:
- subtitles in UTF-8, UTF-8 BOM, UTF-16 LE/BE, Windows-1252 and CRLF are decoded and reach OpenAI
- `<i>` / `{\an8}` tags are stripped
- empty, incomplete or wrong-movie subtitles stop the run **before** OpenAI is called
- a looping / generic / ungrounded AI script is rejected, retried once with the reasons,
  and if still bad no video is made (no ElevenLabs credits spent) and the movie is not retired
- a script that is fixed on the retry produces the video

Setup (once): a static libcurl without TLS in `/tmp/curlinst`, cJSON + miniz sources in
`/tmp/deps/{cjson,miniz}` (miniz also needs a `miniz_export.h` defining `MINIZ_EXPORT`),
and `ffmpeg` + `ffprobe_shim.py` (as `ffprobe`) on PATH.

```sh
./build_test.sh
python3 mock_server.py &        # with ffmpeg on PATH
./run_scenarios.sh
```
