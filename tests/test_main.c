/* Test harness: runs the real generator, but redirects the real API hosts
   (OpenAI, ElevenLabs, subf2m, IMSDb) to a local mock server (tests/mock_server.py).
   Production code is not modified: we wrap curl_easy_perform before including generator.c. */
#include <curl/curl.h>
#include <stdio.h>
#include <string.h>
static CURLcode test_perform(CURL *c) {
  char *u = NULL;
  curl_easy_getinfo(c, CURLINFO_EFFECTIVE_URL, &u);
  static const char *map[][2] = {
    {"https://api.openai.com", "http://127.0.0.1:8765/openai"},
    {"https://api.elevenlabs.io", "http://127.0.0.1:8765/eleven"},
    {"https://subf2m.co", "http://127.0.0.1:8765/subf2m"},
    {"https://imsdb.com", "http://127.0.0.1:8765/imsdb"},
  };
  char nu[4096];
  if (u) for (int i = 0; i < 4; i++) {
    size_t n = strlen(map[i][0]);
    if (strncmp(u, map[i][0], n) == 0) {
      snprintf(nu, sizeof nu, "%s%s", map[i][1], u + n);
      curl_easy_setopt(c, CURLOPT_URL, nu);
      break;
    }
  }
  return curl_easy_perform(c);
}
#define curl_easy_perform test_perform
#include "generator.c"
#undef curl_easy_perform
int main(void) {
  plat_console_init();
  int rc = run_generation();
  printf("RC=%d\n", rc);
  return 0;
}
