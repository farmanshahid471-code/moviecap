#!/bin/bash
# End-to-end scenarios against tests/mock_server.py (start it first, with ffmpeg + ffprobe_shim on PATH).
# Linux/macOS dev only.
HERE=$(cd "$(dirname "$0")" && pwd)
MOVIE_CACHE=/tmp/ms_test_movie.mp4
[ -f $MOVIE_CACHE ] || ffmpeg -hide_banner -loglevel error -f lavfi -i testsrc=size=320x180:rate=10:duration=620 \
  -f lavfi -i sine=duration=620 -c:v libx264 -preset ultrafast -c:a aac -shortest $MOVIE_CACHE
PASS=0; FAIL=0
check() { if eval "$2"; then echo "   ok   - $1"; PASS=$((PASS+1)); else echo "   FAIL - $1"; FAIL=$((FAIL+1)); fi; }

run_case() { # name scenario encoding [extra srt transform]
  local name=$1 sc=$2 enc=$3 xf=$4
  W=/tmp/ms_ws_$name; rm -rf $W; mkdir -p $W/movies; cd $W
  printf '{"open_api_key":"sk-test","elevenlabs_api_key":"el-test"}' > config.json
  cp $MOVIE_CACHE "movies/Lighthouse Test.mp4"
  echo $sc > $HERE/.scenario; rm -f $HERE/.last_prompt_*
  python3 $HERE/make_test_srt.py $HERE/.srt_to_serve $enc $( [ "$xf" = crlf ] && echo crlf )
  [ "$xf" = short ] && python3 -c "
d=open('$HERE/.srt_to_serve','rb').read().decode('utf-8'); open('$HERE/.srt_to_serve','wb').write('\n'.join(d.split('\n')[:240]).encode())"
  [ "$xf" = tiny ] && printf '1\n00:00:01,000 --> 00:00:02,000\nHi.\n' > $HERE/.srt_to_serve
  $HERE/test_gen > log.txt 2>&1
  echo "== $name ($sc, $enc${xf:+, $xf})"
}

run_case utf16le good utf16le
check "dialogue reached OpenAI" "grep -q 'Mara, wake up' $HERE/.last_prompt_1.txt"
check "encoding detected" "grep -q 'encoding UTF-16 LE' log.txt"
check "video produced" "[ -f 'output/Lighthouse Test.mp4' ] && [ -f 'tiktok_output/Lighthouse Test_vertical.mp4' ]"
check "script saved" "grep -q 'Mara' 'output/Lighthouse Test_script.txt'"

for e in utf8 utf8bom utf16be cp1252; do
  run_case enc_$e good $e
  check "$e: accented text decoded (Café)" "grep -q 'Café' $HERE/.last_prompt_1.txt"
  check "$e: tags stripped" "! grep -q '<i>\|an8' $HERE/.last_prompt_1.txt"
  check "$e: video produced" "[ -f 'output/Lighthouse Test.mp4' ]"
done
run_case crlf good utf8 crlf
check "CRLF parsed (120 lines)" "grep -q 'Subtitles: 120 lines' log.txt"

run_case loop loop utf8
check "loop rejected + retried" "grep -q 'Asking OpenAI to rewrite' log.txt && [ -f $HERE/.last_prompt_2.txt ]"
check "retry carries the reasons" "grep -q 'repeated sentences' $HERE/.last_prompt_2.txt"
check "no video made" "[ ! -f 'output/Lighthouse Test.mp4' ]"
check "rejected script saved" "[ -f 'output/Lighthouse Test_REJECTED_script.txt' ]"
check "movie NOT retired" "[ -f 'movies/Lighthouse Test.mp4' ]"

run_case loop_then_good loop_then_good utf8
check "fixed on retry, video made" "[ -f 'output/Lighthouse Test.mp4' ] && grep -q 'Recap script accepted' log.txt"

run_case tiny good utf8 tiny
check "empty subtitles stop BEFORE OpenAI" "grep -q 'STOPPED before calling OpenAI' log.txt && [ ! -f $HERE/.last_prompt_1.txt ]"
check "bad download moved aside" "[ -f 'scripts/srt_files/Lighthouse Test.srt.bad' ]"

run_case short good utf8 short
check "incomplete subtitles stop" "grep -q 'stop at' log.txt && [ ! -f $HERE/.last_prompt_1.txt ]"

echo; echo "PASSED $PASS, FAILED $FAIL"
[ $FAIL -eq 0 ]
