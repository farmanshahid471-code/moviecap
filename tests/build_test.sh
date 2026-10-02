#!/bin/sh
# Linux/macOS dev only. Needs: a TLS-less static libcurl in $CURL (default /tmp/curlinst),
# cJSON + miniz sources in $DEPS (default /tmp/deps). See tests/README.md.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
S="$HERE/../src"; DEPS=${DEPS:-/tmp/deps}; CURL=${CURL:-/tmp/curlinst}
gcc -std=c11 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-const-variable -Wno-unused-function -Wno-format-truncation \
  -I"$S" -I"$DEPS/cjson" -I"$DEPS/miniz" -I"$CURL/include" \
  "$HERE/test_main.c" "$S/platform.c" "$DEPS/cjson/cJSON.c" \
  "$DEPS/miniz/miniz.c" "$DEPS/miniz/miniz_zip.c" "$DEPS/miniz/miniz_tdef.c" "$DEPS/miniz/miniz_tinfl.c" \
  "$CURL/lib/libcurl.a" -lpthread -lm -o "$HERE/test_gen"
echo "built $HERE/test_gen"
