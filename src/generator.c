/*
 * generator.c - the AI-Movie-Shorts pipeline (Windows port).
 *
 * subtitles (subf2m) + optional script (IMSDb) -> OpenAI clip plan
 * -> ElevenLabs narration -> FFmpeg clips/concat/BGM/vertical render.
 *
 * All OS-specific work (UTF-8 paths, process spawning, directory listing)
 * goes through platform.h, so this file contains no Win32/POSIX calls.
 */
#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
  #define _CRT_SECURE_NO_WARNINGS
#endif

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <curl/curl.h>
#include "cJSON.h"
#include "miniz.h"

#include "generator.h"
#include "platform.h"


#ifdef PATH_MAX
  #undef PATH_MAX
#endif
#define PATH_MAX 4096

static const int MIN_NUM_CLIPS = 20;
static const int MAX_NUM_CLIPS = 30;

static const int MIN_TOTAL_DURATION = (int)(2.5 * 60);
static const int MAX_TOTAL_DURATION = (int)(4.5 * 60);

static const double MAX_VIDEO_SPEEDUP = 1.75;

static const char *BROWSER_UA =
  "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
  "(KHTML, like Gecko) Chrome/129.0.0.0 Safari/537.36";

typedef struct {
  char *data;
  size_t size;
} MemBuf;

/* Portable strdup (not part of strict C11). */
static char *str_dup(const char *s) {
  size_t n = strlen(s) + 1;
  char *p = (char *)malloc(n);
  if (p) memcpy(p, s, n);
  return p;
}

/* Portable case-insensitive compare (strcasecmp is not in MSVC). */
static int str_icmp(const char *a, const char *b) {
  for (;; a++, b++) {
    int ca = tolower((unsigned char)*a), cb = tolower((unsigned char)*b);
    if (ca != cb || ca == 0) return ca - cb;
  }
}

/* ------------------------ Log hook plumbing (for UI) ------------------------ */
static GeneratorLogHook g_log_hook = NULL;

void generator_set_log_hook(GeneratorLogHook hook) {
  g_log_hook = hook;
}

/* --------------------- Progress + cancel plumbing (for UI) --------------------- */
static GeneratorProgressHook g_progress_hook = NULL;
static volatile int g_cancel = 0;

void generator_set_progress_hook(GeneratorProgressHook hook) {
  g_progress_hook = hook;
}

void generator_request_cancel(void) { g_cancel = 1; }
void generator_clear_cancel(void)   { g_cancel = 0; }
bool generator_cancel_requested(void) { return g_cancel != 0; }

const char *generator_stage_name(int stage) {
  switch (stage) {
    case GEN_STAGE_IDLE:      return "Idle";
    case GEN_STAGE_SETUP:     return "Starting up";
    case GEN_STAGE_SUBTITLES: return "Subtitles";
    case GEN_STAGE_SCRIPT:    return "Script context";
    case GEN_STAGE_PLANNING:  return "AI clip plan";
    case GEN_STAGE_TTS:       return "Narration (TTS)";
    case GEN_STAGE_CLIP:      return "Building clip";
    case GEN_STAGE_CONCAT:    return "Concatenating";
    case GEN_STAGE_BGM:       return "Background music";
    case GEN_STAGE_VERTICAL:  return "Vertical render";
    case GEN_STAGE_DONE:      return "Done";
    case GEN_STAGE_FAILED:    return "Failed";
    case GEN_STAGE_CANCELLED: return "Cancelled";
    default:                  return "Working";
  }
}

/* Reports the current step to the UI. Called from the generation thread. */
static void report_progress(int stage, int movie_index, int movie_total,
                            int clip_index, int clip_total, const char *movie_title) {
  if (!g_progress_hook) return;

  GeneratorProgress p;
  p.stage       = stage;
  p.movie_index = movie_index;
  p.movie_total = movie_total;
  p.clip_index  = clip_index;
  p.clip_total  = clip_total;
  p.movie_title[0] = 0;
  if (movie_title && movie_title[0])
    snprintf(p.movie_title, sizeof(p.movie_title), "%s", movie_title);

  g_progress_hook(&p);
}

static void emit_line(const char *line) {
  fprintf(stderr, "%s\n", line);
  fflush(stderr);
  if (g_log_hook) g_log_hook(line);
}

static void logv(const char *tag, const char *fmt, va_list ap) {
  char msg[2048];

  va_list ap2;
  va_copy(ap2, ap);
  vsnprintf(msg, sizeof(msg), fmt, ap2);
  va_end(ap2);

  char line[2200];
  snprintf(line, sizeof(line), "[%s] %s", tag, msg);
  emit_line(line);
}

static void logi(const char *fmt, ...) {
  va_list ap; va_start(ap, fmt); logv("INFO", fmt, ap); va_end(ap);
}
static void logok(const char *fmt, ...) {
  va_list ap; va_start(ap, fmt); logv("OK", fmt, ap); va_end(ap);
}
static void logw(const char *fmt, ...) {
  va_list ap; va_start(ap, fmt); logv("WARN", fmt, ap); va_end(ap);
}

/*
 * die(): the original called exit(1), which on Windows silently closes the UI
 * window. Here a fatal error aborts only the current generation run and
 * run_generation() returns -1, so the UI stays open and shows the message.
 */
static jmp_buf g_die_jmp;
static bool    g_die_armed = false;

static void die(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  logv("FATAL", fmt, ap);
  va_end(ap);
  if (g_die_armed) longjmp(g_die_jmp, 1);
  exit(1);
}

static bool file_exists(const char *p) {
  return plat_stat(p, NULL) == PLAT_FILE;
}

static long file_size_bytes(const char *path) {
  long long sz = -1;
  if (plat_stat(path, &sz) != PLAT_FILE) return -1;
  return (long)sz;
}

static bool dir_exists(const char *p) {
  return plat_stat(p, NULL) == PLAT_DIR;
}

static void ensure_dir(const char *p) {
  if (dir_exists(p)) return;
  if (plat_mkdir(p) != 0) {
    die("mkdir failed for %s: %s", p, strerror(errno));
  }
}

static char *read_entire_file(const char *path) {
  FILE *f = plat_fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (n < 0) { fclose(f); return NULL; }

  char *buf = (char *)malloc((size_t)n + 1);
  if (!buf) die("OOM");
  if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
    fclose(f);
    free(buf);
    return NULL;
  }
  fclose(f);
  buf[n] = '\0';
  return buf;
}

static bool write_entire_file(const char *path, const void *data, size_t len) {
  FILE *f = plat_fopen(path, "wb");
  if (!f) return false;
  if (fwrite(data, 1, len, f) != len) {
    fclose(f);
    return false;
  }
  fclose(f);
  return true;
}

static size_t curl_write_cb(void *contents, size_t size, size_t nmemb, void *userp) {
  size_t realsz = size * nmemb;
  MemBuf *mem = (MemBuf *)userp;
  char *p = (char *)realloc(mem->data, mem->size + realsz + 1);
  if (!p) return 0;
  mem->data = p;
  memcpy(&(mem->data[mem->size]), contents, realsz);
  mem->size += realsz;
  mem->data[mem->size] = 0;
  return realsz;
}

/* Explicit fwrite callback: required on Windows when libcurl is a DLL
   (passing a FILE* to a DLL built against another C runtime crashes). */
static size_t curl_file_write_cb(void *contents, size_t size, size_t nmemb, void *userp) {
  return fwrite(contents, size, nmemb, (FILE *)userp);
}

static MemBuf http_get_to_mem_ex(const char *url, long *http_code_out) {
  if (http_code_out) *http_code_out = -1;

  CURL *curl = curl_easy_init();
  if (!curl) {
    logw("curl_easy_init failed (out of memory?)");
    return (MemBuf){0};
  }

  MemBuf buf = (MemBuf){0};

  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

  curl_easy_setopt(curl, CURLOPT_USERAGENT, BROWSER_UA);
  curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
  curl_easy_setopt(curl, CURLOPT_COOKIEFILE, "");
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&buf);

  CURLcode res = curl_easy_perform(curl);

  long code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
  if (http_code_out) *http_code_out = code;

  curl_easy_cleanup(curl);

  /*
   * A transport error (DNS failure, no internet, TLS problem, timeout) must not
   * abort the whole run: the callers treat "no usable response" as a normal
   * failure of this one step (subtitle download / script scrape / plan) and
   * continue with the rest. Only the message is reported.
   */
  if (res != CURLE_OK) {
    logw("GET failed: %s (%s)", url, curl_easy_strerror(res));
    if (buf.data) free(buf.data);
    if (http_code_out) *http_code_out = -1;
    return (MemBuf){0};
  }

  return buf;
}

static MemBuf http_post_json_to_mem(const char *url, const char *bearer_key, const char *json_body,
                                   long *http_code_out, long timeout_s) {
  if (http_code_out) *http_code_out = -1;

  CURL *curl = curl_easy_init();
  if (!curl) {
    logw("curl init failed (out of memory?)");
    return (MemBuf){0};
  }

  MemBuf buf = (MemBuf){0};

  struct curl_slist *headers = NULL;
  headers = curl_slist_append(headers, "Content-Type: application/json");

  if (bearer_key && bearer_key[0]) {
    char auth[1024];
    snprintf(auth, sizeof(auth), "Authorization: Bearer %s", bearer_key);
    headers = curl_slist_append(headers, auth);
  }

  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(json_body));
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&buf);

  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_s);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

  curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
  curl_easy_setopt(curl, CURLOPT_TCP_KEEPIDLE, 60L);
  curl_easy_setopt(curl, CURLOPT_TCP_KEEPINTVL, 30L);

  CURLcode res = curl_easy_perform(curl);

  long code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
  if (http_code_out) *http_code_out = code;

  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (res != CURLE_OK) {
    /* Transport error: report and let the caller retry / skip, never abort. */
    logw("POST failed: %s (%s)", url, curl_easy_strerror(res));
    if (buf.data) free(buf.data);
    if (http_code_out) *http_code_out = -1;
    return (MemBuf){0};
  }

  return buf;
}

/* Quote a path for a command line (Windows: CommandLineToArgvW rules, POSIX: '...'). */
static char *sh_escape(const char *s) {
  char *q = plat_quote_arg(s);
  if (!q) die("OOM");
  return q;
}

static void run_cmd_line_cb(const char *line, void *user) {
  (void)user;
  char buf[4200];
  snprintf(buf, sizeof(buf), "[ffmpeg] %s", line);
  emit_line(buf);
}

/*
 * Run an external tool (ffmpeg). On Windows this uses CreateProcessW directly
 * (no cmd.exe), so paths with spaces, &, %, ^, unicode etc. are safe, and the
 * tool's output is forwarded into the UI log.
 */
static int run_cmd(const char *fmt, ...) {
  char cmd[16384];
  va_list ap;
  va_start(ap, fmt);
  int need = vsnprintf(cmd, sizeof(cmd), fmt, ap);
  va_end(ap);

  if (need < 0) return -1;
  if ((size_t)need >= sizeof(cmd)) {
    logw("Command line too long (%d chars) - it was truncated and will fail.", need);
    return -1;
  }

  char line[sizeof(cmd) + 32];
  snprintf(line, sizeof(line), "[cmd] %s", cmd);
  emit_line(line);
  return plat_run(cmd, run_cmd_line_cb, NULL);
}

static char *popen_read_all(const char *cmd) {
  return plat_capture(cmd);
}

static bool ffprobe_video_dimensions(const char *path, int *out_w, int *out_h) {
  if (!out_w || !out_h) return false;
  *out_w = 0;
  *out_h = 0;

  char *esc = sh_escape(path);
  char cmd[8192];
  snprintf(cmd, sizeof(cmd),
           "ffprobe -v error -select_streams v:0 "
           "-show_entries stream=width,height "
           "-of csv=s=x:p=0 %s",
           esc);
  free(esc);

  char *out = popen_read_all(cmd);
  if (!out) return false;

  int w = 0, h = 0;
  if (sscanf(out, "%dx%d", &w, &h) != 2) {
    free(out);
    return false;
  }
  free(out);

  if (w <= 0 || h <= 0) return false;
  *out_w = w;
  *out_h = h;
  return true;
}

static double ffprobe_duration_seconds(const char *path) {
  char *esc = sh_escape(path);
  char cmd[8192];
  snprintf(cmd, sizeof(cmd),
           "ffprobe -v error -show_entries format=duration "
           "-of default=noprint_wrappers=1:nokey=1 %s",
           esc);
  free(esc);
  char *out = popen_read_all(cmd);
  if (!out) return -1.0;
  double d = atof(out);
  free(out);
  return d;
}

typedef enum {
  TTS_ELEVENLABS = 0,  /* api.elevenlabs.io (needs a key)                     */
  TTS_XTTS,            /* Coqui XTTS v2 server, POST /tts_to_audio/ (free)    */
  TTS_PIPER,           /* piper http_server, POST /synthesize (free, local)   */
  TTS_OPENAI           /* OpenAI-compatible POST /audio/speech (e.g. Kokoro)  */
} TtsProvider;

typedef struct {
  /* required */
  char openai_key[512];
  char eleven_key[512];

  /* optional, with defaults */
  char eleven_voice_id[128];
  char eleven_model_id[128];
  char openai_model[128];   /* "openai_model",   default gpt-5.2 */

  /* optional API base URLs (proxies, Azure-style gateways, local mocks) */
  char openai_base_url[256]; /* default https://api.openai.com/v1     */
  char eleven_base_url[256]; /* default https://api.elevenlabs.io/v1  */

  /* narration engine: "elevenlabs" (default), "xtts", "piper" or "openai_tts" */
  int  tts_provider;          /* TtsProvider */
  char tts_provider_name[32];
  char tts_base_url[256];     /* local server for xtts / piper / openai_tts */
  char tts_voice[256];        /* xtts: speaker_wav, piper: voice, openai_tts: voice */
  char tts_language[16];      /* xtts only, default "en" */
  char tts_model[64];         /* openai_tts only, default "tts-1" */
  char tts_api_key[512];      /* optional bearer for openai_tts */

  /* optional pipeline tuning */
  int    min_clips;          /* default 20   */
  int    max_clips;          /* default 30   */
  double max_video_speedup;  /* default 1.75 */
  double narration_volume;   /* default 2.5  */
  double bgm_volume;         /* default 0.1  */
  bool   bgm_enabled;        /* default true */
  bool   make_vertical;      /* default true */
  bool   retire_movies;      /* default true */
} Config;

static void cfg_set_str(char *dst, size_t dstsz, const cJSON *node) {
  if (cJSON_IsString(node) && node->valuestring) {
    strncpy(dst, node->valuestring, dstsz - 1);
    dst[dstsz - 1] = 0;
  }
}

static bool cfg_get_bool(const cJSON *node, bool def) {
  if (cJSON_IsBool(node)) return cJSON_IsTrue(node) ? true : false;
  if (cJSON_IsNumber(node)) return node->valuedouble != 0.0;
  return def;
}

static int cfg_get_int(const cJSON *node, int def, int lo, int hi) {
  if (!cJSON_IsNumber(node)) return def;
  int v = (int)(node->valuedouble + (node->valuedouble >= 0 ? 0.5 : -0.5));
  if (v < lo) v = lo;
  if (v > hi) v = hi;
  return v;
}

static double cfg_get_dbl(const cJSON *node, double def, double lo, double hi) {
  if (!cJSON_IsNumber(node)) return def;
  double v = node->valuedouble;
  if (v < lo) v = lo;
  if (v > hi) v = hi;
  return v;
}

/* Strip a trailing '/' so "<base>/responses" never becomes "//responses". */
static void cfg_trim_trailing_slash(char *s) {
  size_t n = strlen(s);
  while (n > 0 && s[n - 1] == '/') s[--n] = 0;
}

static Config load_config_json(const char *path) {
  Config c = {0};
  char *txt = read_entire_file(path);
  if (!txt) die("Missing config.json (expected at %s)", path);

  cJSON *root = cJSON_Parse(txt);
  free(txt);
  if (!root) die("config.json parse failed");

  const cJSON *ok  = cJSON_GetObjectItemCaseSensitive(root, "open_api_key");
  const cJSON *ek  = cJSON_GetObjectItemCaseSensitive(root, "elevenlabs_api_key");
  const cJSON *vid = cJSON_GetObjectItemCaseSensitive(root, "eleven_voice_id");
  const cJSON *mid = cJSON_GetObjectItemCaseSensitive(root, "eleven_model_id");
  const cJSON *oam = cJSON_GetObjectItemCaseSensitive(root, "openai_model");

  cfg_set_str(c.openai_key,     sizeof(c.openai_key),     ok);
  cfg_set_str(c.eleven_key,     sizeof(c.eleven_key),     ek);
  cfg_set_str(c.eleven_voice_id, sizeof(c.eleven_voice_id), vid);
  cfg_set_str(c.eleven_model_id, sizeof(c.eleven_model_id), mid);
  cfg_set_str(c.openai_model,   sizeof(c.openai_model),   oam);

  cfg_set_str(c.openai_base_url, sizeof(c.openai_base_url),
              cJSON_GetObjectItemCaseSensitive(root, "openai_base_url"));
  cfg_set_str(c.eleven_base_url, sizeof(c.eleven_base_url),
              cJSON_GetObjectItemCaseSensitive(root, "elevenlabs_base_url"));

  /* ---- narration engine ---- */
  cfg_set_str(c.tts_provider_name, sizeof(c.tts_provider_name),
              cJSON_GetObjectItemCaseSensitive(root, "tts_provider"));
  cfg_set_str(c.tts_base_url, sizeof(c.tts_base_url),
              cJSON_GetObjectItemCaseSensitive(root, "tts_base_url"));
  cfg_set_str(c.tts_voice, sizeof(c.tts_voice),
              cJSON_GetObjectItemCaseSensitive(root, "tts_voice"));
  cfg_set_str(c.tts_language, sizeof(c.tts_language),
              cJSON_GetObjectItemCaseSensitive(root, "tts_language"));
  cfg_set_str(c.tts_model, sizeof(c.tts_model),
              cJSON_GetObjectItemCaseSensitive(root, "tts_model"));
  cfg_set_str(c.tts_api_key, sizeof(c.tts_api_key),
              cJSON_GetObjectItemCaseSensitive(root, "tts_api_key"));

  if (c.tts_provider_name[0] == 0) snprintf(c.tts_provider_name, sizeof(c.tts_provider_name), "elevenlabs");
  if (str_icmp(c.tts_provider_name, "elevenlabs") == 0)      c.tts_provider = TTS_ELEVENLABS;
  else if (str_icmp(c.tts_provider_name, "xtts") == 0)       c.tts_provider = TTS_XTTS;
  else if (str_icmp(c.tts_provider_name, "coqui") == 0)      c.tts_provider = TTS_XTTS;
  else if (str_icmp(c.tts_provider_name, "piper") == 0)      c.tts_provider = TTS_PIPER;
  else if (str_icmp(c.tts_provider_name, "openai_tts") == 0) c.tts_provider = TTS_OPENAI;
  else if (str_icmp(c.tts_provider_name, "openai") == 0)     c.tts_provider = TTS_OPENAI;
  else die("config.json: unknown tts_provider \"%s\" (use elevenlabs, xtts, piper or openai_tts)",
           c.tts_provider_name);

  if (c.tts_base_url[0] == 0) {
    const char *dflt = (c.tts_provider == TTS_XTTS)  ? "http://127.0.0.1:8020" :
                       (c.tts_provider == TTS_PIPER) ? "http://127.0.0.1:5000" :
                       (c.tts_provider == TTS_OPENAI) ? "https://api.openai.com/v1" : "";
    snprintf(c.tts_base_url, sizeof(c.tts_base_url), "%s", dflt);
  }
  if (c.tts_language[0] == 0) snprintf(c.tts_language, sizeof(c.tts_language), "en");
  if (c.tts_model[0] == 0)    snprintf(c.tts_model, sizeof(c.tts_model), "tts-1");
  if (c.tts_provider == TTS_XTTS && c.tts_voice[0] == 0)
    die("config.json: tts_voice must name an XTTS speaker (a .wav in the server's speakers folder)");

  c.min_clips         = cfg_get_int(cJSON_GetObjectItemCaseSensitive(root, "min_clips"), MIN_NUM_CLIPS, 1, 200);
  c.max_clips         = cfg_get_int(cJSON_GetObjectItemCaseSensitive(root, "max_clips"), MAX_NUM_CLIPS, 1, 200);
  c.max_video_speedup = cfg_get_dbl(cJSON_GetObjectItemCaseSensitive(root, "max_video_speedup"), MAX_VIDEO_SPEEDUP, 1.0, 8.0);
  c.narration_volume  = cfg_get_dbl(cJSON_GetObjectItemCaseSensitive(root, "narration_volume"), 2.5, 0.0, 10.0);
  c.bgm_volume        = cfg_get_dbl(cJSON_GetObjectItemCaseSensitive(root, "bgm_volume"), 0.1, 0.0, 10.0);
  c.bgm_enabled       = cfg_get_bool(cJSON_GetObjectItemCaseSensitive(root, "bgm_enabled"), true);
  c.make_vertical     = cfg_get_bool(cJSON_GetObjectItemCaseSensitive(root, "make_vertical"), true);
  c.retire_movies     = cfg_get_bool(cJSON_GetObjectItemCaseSensitive(root, "retire_movies"), true);

  if (c.min_clips > c.max_clips) c.min_clips = c.max_clips;

  if (c.openai_key[0] == 0) die("config.json: open_api_key missing");
  if (strcmp(c.openai_key, "OpenAIAPI") == 0)
    die("config.json: replace the placeholder \"OpenAIAPI\" with your real OpenAI API key");
  if (c.openai_model[0] == 0) strncpy(c.openai_model, "gpt-5.2", sizeof(c.openai_model) - 1);

  /* The ElevenLabs key is only needed when ElevenLabs is the narration engine. */
  if (c.tts_provider == TTS_ELEVENLABS) {
    if (strcmp(c.eleven_key, "ElevenLabsAPI") == 0)
      die("config.json: replace the placeholder \"ElevenLabsAPI\" with your real ElevenLabs API key "
          "(or set \"tts_provider\": \"xtts\" / \"piper\" to narrate for free)");
    if (c.eleven_key[0] == 0) die("config.json: elevenlabs_api_key missing");
  }
  if (c.eleven_voice_id[0] == 0) strncpy(c.eleven_voice_id, "JBFqnCBsd6RMkjVDRZzb", sizeof(c.eleven_voice_id) - 1);
  if (c.eleven_model_id[0] == 0) strncpy(c.eleven_model_id, "eleven_multilingual_v2", sizeof(c.eleven_model_id) - 1);
  if (c.openai_base_url[0] == 0) strncpy(c.openai_base_url, "https://api.openai.com/v1", sizeof(c.openai_base_url) - 1);
  if (c.eleven_base_url[0] == 0) strncpy(c.eleven_base_url, "https://api.elevenlabs.io/v1", sizeof(c.eleven_base_url) - 1);
  cfg_trim_trailing_slash(c.openai_base_url);
  cfg_trim_trailing_slash(c.eleven_base_url);

  cJSON_Delete(root);
  return c;
}

static int timestamp_to_seconds(const char *ts) {
  int hh = 0, mm = 0, ss = 0, ms = 0;
  if (sscanf(ts, "%d:%d:%d,%d", &hh, &mm, &ss, &ms) != 4) return -1;
  (void)ms;
  return hh * 3600 + mm * 60 + ss;
}

static bool convert_srt_timestamps_to_seconds(const char *input_srt, const char *output_srt) {
  FILE *in = plat_fopen(input_srt, "rb");
  if (!in) return false;
  FILE *out = plat_fopen(output_srt, "wb");
  if (!out) {
    fclose(in);
    return false;
  }

  char line[4096];
  while (fgets(line, sizeof(line), in)) {
    while (strstr(line, "<i>")) {
      char *p = strstr(line, "<i>");
      memmove(p, p + 3, strlen(p + 3) + 1);
    }
    while (strstr(line, "</i>")) {
      char *p = strstr(line, "</i>");
      memmove(p, p + 4, strlen(p + 4) + 1);
    }

    char a[64], b[64];
    if (sscanf(line, "%63s --> %63s", a, b) == 2 && strchr(a, ':') && strchr(b, ':')) {
      int s1 = timestamp_to_seconds(a);
      int s2 = timestamp_to_seconds(b);
      if (s1 >= 0 && s2 >= 0) {
        fprintf(out, "%d --> %d\n", s1, s2);
      } else {
        fputs(line, out);
      }
    } else {
      fputs(line, out);
    }
  }

  fclose(in);
  fclose(out);
  return true;
}

static char *strcasestr_local(const char *haystack, const char *needle) {
  if (!haystack || !needle) return NULL;
  if (*needle == '\0') return (char *)haystack;

  for (const char *h = haystack; *h; h++) {
    const char *h2 = h;
    const char *n2 = needle;
    while (*h2 && *n2 &&
           tolower((unsigned char)*h2) == tolower((unsigned char)*n2)) {
      h2++;
      n2++;
    }
    if (*n2 == '\0') return (char *)h;
  }
  return NULL;
}

static bool href_next(const char **p, char *out, size_t outsz) {
  const char *s = strstr(*p, "href=");
  if (!s) return false;
  s += 5;
  while (*s && isspace((unsigned char)*s)) s++;

  char q = 0;
  if (*s == '"' || *s == '\'') { q = *s; s++; }
  else { *p = s; return false; }

  const char *e = strchr(s, q);
  if (!e) return false;

  size_t n = (size_t)(e - s);
  if (n + 1 > outsz) n = outsz - 1;
  memcpy(out, s, n);
  out[n] = 0;

  *p = e + 1;
  return true;
}

static bool str_ends_with(const char *s, const char *suffix) {
  size_t ls = strlen(s), lf = strlen(suffix);
  if (lf > ls) return false;
  return strcmp(s + (ls - lf), suffix) == 0;
}

/* ----------------------- NEW HELPERS (IMSDb robustness) ----------------------- */

static void to_lower_copy(const char *in, char *out, size_t outsz) {
  size_t j = 0;
  for (size_t i = 0; in[i] && j + 1 < outsz; i++) {
    out[j++] = (char)tolower((unsigned char)in[i]);
  }
  out[j] = 0;
}

/* Percent-encode a path component (spaces -> %20, etc.) */
static void url_encode_component(const char *in, char *out, size_t outsz) {
  size_t j = 0;
  for (size_t i = 0; in[i] && j + 1 < outsz; i++) {
    unsigned char c = (unsigned char)in[i];
    if (isalnum(c) || c == '-' || c == '_' || c == '.') {
      out[j++] = (char)c;
    } else if (c == ' ') {
      if (j + 3 >= outsz) break;
      out[j++] = '%'; out[j++] = '2'; out[j++] = '0';
    } else {
      if (j + 3 >= outsz) break;
      static const char *hex = "0123456789ABCDEF";
      out[j++] = '%';
      out[j++] = hex[(c >> 4) & 0xF];
      out[j++] = hex[c & 0xF];
    }
  }
  out[j] = 0;
}

/* Tiny string builder */
static void sb_append(char **buf, size_t *len, size_t *cap, const char *s, size_t n) {
  if (*len + n + 1 > *cap) {
    *cap = (*cap == 0) ? 8192 : (*cap * 2);
    while (*len + n + 1 > *cap) *cap *= 2;
    *buf = (char *)realloc(*buf, *cap);
    if (!*buf) die("OOM");
  }
  memcpy(*buf + *len, s, n);
  *len += n;
  (*buf)[*len] = 0;
}

/* Very simple HTML->text: strips tags, preserves <br> as newline, decodes a few entities */
static char *html_to_text_basic(const char *html, size_t n, size_t *out_n) {
  char *out = NULL;
  size_t len = 0, cap = 0;

  for (size_t i = 0; i < n;) {
    if (html[i] == '<') {
      size_t j = i + 1;
      while (j < n && isspace((unsigned char)html[j])) j++;
      if (j + 1 < n &&
          tolower((unsigned char)html[j]) == 'b' &&
          tolower((unsigned char)html[j + 1]) == 'r') {
        sb_append(&out, &len, &cap, "\n", 1);
      }
      while (i < n && html[i] != '>') i++;
      if (i < n) i++;
      continue;
    }

    if (html[i] == '&') {
      const char *p = html + i;
      if (i + 6 <= n && !strncmp(p, "&nbsp;", 6)) { sb_append(&out, &len, &cap, " ", 1); i += 6; continue; }
      if (i + 5 <= n && !strncmp(p, "&amp;", 5))  { sb_append(&out, &len, &cap, "&", 1); i += 5; continue; }
      if (i + 4 <= n && !strncmp(p, "&lt;", 4))   { sb_append(&out, &len, &cap, "<", 1); i += 4; continue; }
      if (i + 4 <= n && !strncmp(p, "&gt;", 4))   { sb_append(&out, &len, &cap, ">", 1); i += 4; continue; }
    }

    sb_append(&out, &len, &cap, &html[i], 1);
    i++;
  }

  if (!out) out = str_dup("");
  if (out_n) *out_n = len;
  return out;
}

/* ----------------------- Subtitle downloader ---------------------- */

static void parse_movie_title_slug(const char *movie_title, char *out, size_t outsz) {
  size_t j = 0;
  for (size_t i = 0; movie_title[i] && j + 1 < outsz; i++) {
    char ch = movie_title[i];
    if (ch == '\'') continue;
    if (ch == '(' || ch == ')') continue;
    if (ch == ' ') ch = '-';
    out[j++] = (char)tolower((unsigned char)ch);
  }
  out[j] = 0;

  size_t n = strlen(out);
  if (n >= 2 && strcmp(out + n - 2, "ii") == 0) strncat(out, "-2", outsz - strlen(out) - 1);
  if (n >= 3 && strcmp(out + n - 3, "iii") == 0) strncat(out, "-3", outsz - strlen(out) - 1);
  if (n >= 2 && strcmp(out + n - 2, "iv") == 0) strncat(out, "-4", outsz - strlen(out) - 1);
}

/*
 * Replacement for `unzip -p file.zip "*.srt" > out.srt`.
 * Reads the archive into memory (so unicode paths work on Windows) and writes
 * the largest .srt entry (skipping macOS "__MACOSX/" junk).
 */
static bool extract_srt_from_zip(const char *zip_path, const char *dest_srt_path) {
  FILE *f = plat_fopen(zip_path, "rb");
  if (!f) return false;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (n <= 0) { fclose(f); return false; }
  void *zbuf = malloc((size_t)n);
  if (!zbuf) { fclose(f); return false; }
  size_t got = fread(zbuf, 1, (size_t)n, f);
  fclose(f);
  if (got != (size_t)n) { free(zbuf); return false; }

  mz_zip_archive zip;
  memset(&zip, 0, sizeof(zip));
  if (!mz_zip_reader_init_mem(&zip, zbuf, (size_t)n, 0)) {
    logw("Subtitle archive is not a valid zip: %s", zip_path);
    free(zbuf);
    return false;
  }

  int best = -1;
  mz_uint64 best_size = 0;
  mz_uint count = mz_zip_reader_get_num_files(&zip);
  for (mz_uint i = 0; i < count; i++) {
    mz_zip_archive_file_stat st;
    if (!mz_zip_reader_file_stat(&zip, i, &st)) continue;
    if (st.m_is_directory) continue;
    if (strstr(st.m_filename, "__MACOSX")) continue;
    size_t ln = strlen(st.m_filename);
    if (ln < 4 || str_icmp(st.m_filename + ln - 4, ".srt") != 0) continue;
    if (best < 0 || st.m_uncomp_size > best_size) { best = (int)i; best_size = st.m_uncomp_size; }
  }

  bool ok = false;
  if (best >= 0) {
    size_t out_n = 0;
    void *data = mz_zip_reader_extract_to_heap(&zip, (mz_uint)best, &out_n, 0);
    if (data && out_n > 0) ok = write_entire_file(dest_srt_path, data, out_n);
    if (data) mz_free(data);
  }

  mz_zip_reader_end(&zip);
  free(zbuf);
  return ok;
}

static bool download_subtitle_srt(const char *movie_title, const char *dest_srt_path) {
  ensure_dir("scripts");
  ensure_dir("scripts/srt_files");

  char slug[512];
  parse_movie_title_slug(movie_title, slug, sizeof(slug));

  /* The slug can contain non-ASCII characters (e.g. "amelie" with an accent),
     which must be percent-encoded before they go into a URL. */
  char slug_enc[1024];
  url_encode_component(slug, slug_enc, sizeof(slug_enc));

  char list_url[1536];
  snprintf(list_url, sizeof(list_url), "https://subf2m.co/subtitles/%s/english", slug_enc);

  long code = 0;
  MemBuf page = http_get_to_mem_ex(list_url, &code);
  if (code < 200 || code >= 300 || !page.data || page.size == 0) {
    if (page.data) {
      logw("subf2m list HTTP %ld for %s (body starts: %.200s)", code, list_url, page.data);
      free(page.data);
    }
    return false;
  }

  char want_subpage_prefix[1280];
  snprintf(want_subpage_prefix, sizeof(want_subpage_prefix), "/subtitles/%s/english/", slug_enc);

  char subpage_url[2112] = {0};

  {
    const char *p = page.data;
    char href[2048];
    while (href_next(&p, href, sizeof(href))) {
      if (strncmp(href, want_subpage_prefix, strlen(want_subpage_prefix)) == 0) {
        if (strstr(href, "english-german")) continue;
        snprintf(subpage_url, sizeof(subpage_url), "https://subf2m.co%s", href);
        break;
      }
    }
  }

  if (subpage_url[0] == 0) {
    const char *p = page.data;
    char href[2048];
    int tried_profiles = 0;

    while (href_next(&p, href, sizeof(href))) {
      if (strncmp(href, "/u/", 3) != 0) continue;

      char profile_url[2112];
      snprintf(profile_url, sizeof(profile_url), "https://subf2m.co%s", href);

      long pcode = 0;
      MemBuf prof = http_get_to_mem_ex(profile_url, &pcode);
      if (pcode < 200 || pcode >= 300 || !prof.data) {
        if (prof.data) free(prof.data);
        continue;
      }

      const char *pp = prof.data;
      char phref[2048];
      while (href_next(&pp, phref, sizeof(phref))) {
        if (strncmp(phref, want_subpage_prefix, strlen(want_subpage_prefix)) == 0) {
          snprintf(subpage_url, sizeof(subpage_url), "https://subf2m.co%s", phref);
          break;
        }
      }
      free(prof.data);

      if (subpage_url[0] != 0) break;
      if (++tried_profiles >= 12) break;
    }
  }

  free(page.data);

  if (subpage_url[0] == 0) {
    logw("subf2m: couldn't locate subtitle detail page for %s (slug=%s)", movie_title, slug);
    return false;
  }

  long scode = 0;
  MemBuf subpage = http_get_to_mem_ex(subpage_url, &scode);
  if (scode < 200 || scode >= 300 || !subpage.data) {
    if (subpage.data) free(subpage.data);
    logw("subf2m: subtitle detail HTTP %ld for %s", scode, subpage_url);
    return false;
  }

  char download_url[2112] = {0};
  {
    const char *p = subpage.data;
    char href[2048];
    while (href_next(&p, href, sizeof(href))) {
      if (str_ends_with(href, "download")) {
        snprintf(download_url, sizeof(download_url), "https://subf2m.co%s", href);
        break;
      }
    }
  }

  free(subpage.data);

  if (download_url[0] == 0) {
    logw("subf2m: couldn't find download link on %s", subpage_url);
    return false;
  }

  char tmpzip[PATH_MAX];
  snprintf(tmpzip, sizeof(tmpzip), "scripts/srt_files/%s_tmp.zip", movie_title);

  CURL *curl = curl_easy_init();
  if (!curl) die("curl init failed");

  FILE *zf = plat_fopen(tmpzip, "wb");
  if (!zf) { curl_easy_cleanup(curl); return false; }

  curl_easy_setopt(curl, CURLOPT_URL, download_url);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, BROWSER_UA);
  curl_easy_setopt(curl, CURLOPT_COOKIEFILE, "");
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, zf);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_file_write_cb);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

  CURLcode res = curl_easy_perform(curl);
  long zcode = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &zcode);
  fclose(zf);
  curl_easy_cleanup(curl);

  if (res != CURLE_OK || zcode < 200 || zcode >= 300) {
    plat_unlink(tmpzip);
    if (res != CURLE_OK) logw("subf2m: zip download failed: %s", curl_easy_strerror(res));
    else logw("subf2m: zip download HTTP %ld", zcode);
    return false;
  }

  /* Extract the subtitle from the zip in-process (Windows has no `unzip`). */
  bool ok = extract_srt_from_zip(tmpzip, dest_srt_path);
  plat_unlink(tmpzip);
  if (!ok) logw("subf2m: no .srt found in downloaded archive for %s", movie_title);
  return ok && file_exists(dest_srt_path);
}

/* ----------------------- IMSDb scraper (UPDATED) ----------------------- */

static void strip_parens(const char *in, char *out, size_t outsz) {
  size_t j = 0;
  for (size_t i = 0; in[i] && j + 1 < outsz; i++) {
    if (in[i] == '(' || in[i] == ')') continue;
    out[j++] = in[i];
  }
  out[j] = 0;
}

static void imsdb_format_title_loose(const char *movie_title, char *out, size_t outsz) {
  size_t j = 0;
  for (size_t i = 0; movie_title[i] && j + 1 < outsz; i++) {
    unsigned char ch = (unsigned char)movie_title[i];
    if (ch == '(' || ch == ')') continue;
    if (ch == '\'') continue;
    if (ch == ' ') ch = '-';
    if (isalnum(ch) || ch == '-' || ch == '_' ) {
      out[j++] = (char)ch;
    }
  }
  out[j] = 0;
}

/* Extract script from either <pre>...</pre> OR class="scrtext" region */
static bool imsdb_fetch_script_to_file(const char *url, const char *dest_txt_path,
                                       char *why, size_t whysz) {
  if (why && whysz) { why[0] = 0; }

  long code = 0;
  MemBuf page = http_get_to_mem_ex(url, &code);

  if (code != 200 || !page.data || page.size == 0) {
    if (why && whysz) snprintf(why, whysz, "HTTP %ld", code);
    if (page.data) free(page.data);
    return false;
  }

  const char *start = NULL;
  const char *end   = NULL;

  /* 1) Prefer <pre> */
  char *pre = strcasestr_local(page.data, "<pre");
  if (pre) {
    pre = strchr(pre, '>');
    if (pre) {
      pre++;
      char *pend = strcasestr_local(pre, "</pre>");
      if (pend) { start = pre; end = pend; }
    }
  }

  /* 2) Fallback: class="scrtext" */
  if (!start || !end) {
    char *scr = strcasestr_local(page.data, "class=\"scrtext\"");
    if (!scr) scr = strcasestr_local(page.data, "class='scrtext'");
    if (scr) {
      char *gt = strchr(scr, '>');
      if (gt) {
        gt++;
        char *tdend  = strcasestr_local(gt, "</td>");
        char *divend = strcasestr_local(gt, "</div>");
        char *best = NULL;
        if (tdend && divend) best = (tdend < divend) ? tdend : divend;
        else best = tdend ? tdend : divend;
        if (best) { start = gt; end = best; }
      }
    }
  }

  if (!start || !end || end <= start) {
    if (why && whysz) snprintf(why, whysz, "script block not found");
    free(page.data);
    return false;
  }

  size_t raw_n = (size_t)(end - start);
  size_t txt_n = 0;
  char *txt = html_to_text_basic(start, raw_n, &txt_n);

  if (txt_n < 1000) {
    if (why && whysz) snprintf(why, whysz, "extracted text too small (%zu)", txt_n);
    free(txt);
    free(page.data);
    return false;
  }

  bool ok = write_entire_file(dest_txt_path, txt, txt_n);
  free(txt);
  free(page.data);

  if (!ok) {
    if (why && whysz) snprintf(why, whysz, "write failed");
    return false;
  }

  return true;
}

/* Try multiple URL families, including Movie%20Scripts/<Title>%20Script.html */
static bool download_imsdb_script_ex(const char *movie_title,
                                     const char *dest_txt_path,
                                     char *used_url, size_t used_url_sz) {
  ensure_dir("scripts");
  ensure_dir("scripts/srt_files");

  if (used_url && used_url_sz) used_url[0] = 0;

  char a[512] = {0};
  char b[512] = {0};
  char c[512] = {0};

  {
    size_t j = 0;
    for (size_t i = 0; movie_title[i] && j + 1 < sizeof(a); i++) {
      char ch = movie_title[i];
      if (ch == ' ') ch = '-';
      a[j++] = ch;
    }
    a[j] = 0;
  }

  strip_parens(a, b, sizeof(b));
  imsdb_format_title_loose(movie_title, c, sizeof(c));

  char a_lo[512]; to_lower_copy(a, a_lo, sizeof(a_lo));
  char b_lo[512]; to_lower_copy(b, b_lo, sizeof(b_lo));
  char c_lo[512]; to_lower_copy(c, c_lo, sizeof(c_lo));

  char enc_title[1024];
  url_encode_component(movie_title, enc_title, sizeof(enc_title));

  /* Every title variant is percent-encoded: raw spaces, apostrophes and
     accented characters are not valid inside a URL path. */
  char a_enc[1024], b_enc[1024], c_enc[1024];
  char a_lo_enc[1024], b_lo_enc[1024], c_lo_enc[1024];
  url_encode_component(a,    a_enc,    sizeof(a_enc));
  url_encode_component(b,    b_enc,    sizeof(b_enc));
  url_encode_component(c,    c_enc,    sizeof(c_enc));
  url_encode_component(a_lo, a_lo_enc, sizeof(a_lo_enc));
  url_encode_component(b_lo, b_lo_enc, sizeof(b_lo_enc));
  url_encode_component(c_lo, c_lo_enc, sizeof(c_lo_enc));

  char url0[1536], url1[1536], url2[1536], url3[1536], url4[1536], url5[1536], url6[1536];

  snprintf(url0, sizeof(url0), "https://imsdb.com/scripts/%s.html", a_enc);
  snprintf(url1, sizeof(url1), "https://imsdb.com/scripts/%s.html", b_enc);
  snprintf(url2, sizeof(url2), "https://imsdb.com/scripts/%s.html", c_enc);

  snprintf(url3, sizeof(url3), "https://imsdb.com/scripts/%s.html", a_lo_enc);
  snprintf(url4, sizeof(url4), "https://imsdb.com/scripts/%s.html", b_lo_enc);
  snprintf(url5, sizeof(url5), "https://imsdb.com/scripts/%s.html", c_lo_enc);

  snprintf(url6, sizeof(url6), "https://imsdb.com/Movie%%20Scripts/%s%%20Script.html", enc_title);

  const char *attempts[] = { url0, url1, url2, url3, url4, url5, url6, NULL };

  for (int i = 0; attempts[i]; i++) {
    if (!attempts[i][0]) continue;

    char why[256];
    if (imsdb_fetch_script_to_file(attempts[i], dest_txt_path, why, sizeof(why))) {
      if (used_url && used_url_sz) {
        snprintf(used_url, used_url_sz, "%s", attempts[i]);
      }
      return true;
    }
    logw("IMSDb attempt failed (%s): %s", why, attempts[i]);
  }

  return false;
}

/* ----------------------- OpenAI response parsing ----------------------- */

static char *openai_extract_output_text(const char *resp_json) {
  cJSON *root = cJSON_Parse(resp_json);
  if (!root) return NULL;

  cJSON *err = cJSON_GetObjectItemCaseSensitive(root, "error");
  if (cJSON_IsObject(err)) {
    cJSON *msg = cJSON_GetObjectItemCaseSensitive(err, "message");
    cJSON *typ = cJSON_GetObjectItemCaseSensitive(err, "type");
    cJSON *cod = cJSON_GetObjectItemCaseSensitive(err, "code");
    if (cJSON_IsString(msg) && msg->valuestring) logw("OpenAI error message: %s", msg->valuestring);
    if (cJSON_IsString(typ) && typ->valuestring) logw("OpenAI error type: %s", typ->valuestring);
    if (cJSON_IsString(cod) && cod->valuestring) logw("OpenAI error code: %s", cod->valuestring);
    cJSON_Delete(root);
    return NULL;
  }

  cJSON *output = cJSON_GetObjectItemCaseSensitive(root, "output");
  if (!cJSON_IsArray(output)) {
    cJSON_Delete(root);
    return NULL;
  }

  cJSON *item = NULL;
  cJSON_ArrayForEach(item, output) {
    if (!cJSON_IsObject(item)) continue;

    cJSON *content = cJSON_GetObjectItemCaseSensitive(item, "content");
    if (!cJSON_IsArray(content)) continue;

    cJSON *it = NULL;
    cJSON_ArrayForEach(it, content) {
      if (!cJSON_IsObject(it)) continue;

      cJSON *type = cJSON_GetObjectItemCaseSensitive(it, "type");
      cJSON *text = cJSON_GetObjectItemCaseSensitive(it, "text");
      if (cJSON_IsString(type) && type->valuestring &&
          strcmp(type->valuestring, "output_text") == 0 &&
          cJSON_IsString(text) && text->valuestring) {
        char *out = str_dup(text->valuestring);
        cJSON_Delete(root);
        return out;
      }
    }
  }

  cJSON_Delete(root);
  return NULL;
}

static bool openai_resp_should_retry_without_script(const char *resp_json) {
  if (!resp_json || !resp_json[0]) return false;

  cJSON *root = cJSON_Parse(resp_json);
  if (!root) return false;

  bool yes = false;
  cJSON *err = cJSON_GetObjectItemCaseSensitive(root, "error");
  if (cJSON_IsObject(err)) {
    const char *m = NULL;
    const char *c = NULL;

    cJSON *msg = cJSON_GetObjectItemCaseSensitive(err, "message");
    cJSON *cod = cJSON_GetObjectItemCaseSensitive(err, "code");

    if (cJSON_IsString(msg) && msg->valuestring) m = msg->valuestring;
    if (cJSON_IsString(cod) && cod->valuestring) c = cod->valuestring;

    if (c && (str_icmp(c, "context_length_exceeded") == 0 ||
              str_icmp(c, "invalid_json") == 0 ||
              strcasestr_local(c, "context") != NULL)) {
      yes = true;
    }

    if (m) {
      if (strcasestr_local(m, "too large") ||
          strcasestr_local(m, "message is too long") ||
          strcasestr_local(m, "maximum context length") ||
          strcasestr_local(m, "context length") ||
          strcasestr_local(m, "reduce") ||
          strcasestr_local(m, "token") ||
          strcasestr_local(m, "request is too large") ||
          strcasestr_local(m, "unicode decode error") ||
          strcasestr_local(m, "invalid unicode") ||
          strcasestr_local(m, "invalid body")) {
        yes = true;
      }
    }
  }

  cJSON_Delete(root);
  return yes;
}

typedef struct {
  int start;
  int end;
  char *narration;
} ClipPlan;

typedef struct {
  ClipPlan *items;
  size_t count;
} ClipPlanList;

static void free_clip_plan_list(ClipPlanList *lst) {
  if (!lst) return;
  for (size_t i = 0; i < lst->count; i++) {
    free(lst->items[i].narration);
  }
  free(lst->items);
  lst->items = NULL;
  lst->count = 0;
}

static ClipPlanList parse_clip_plan_json(const char *json_text) {
  ClipPlanList out = {0};
  cJSON *root = cJSON_Parse(json_text);
  if (!root) return out;

  cJSON *clips = cJSON_GetObjectItemCaseSensitive(root, "clips");
  if (!cJSON_IsArray(clips)) {
    cJSON_Delete(root);
    return out;
  }

  size_t n = (size_t)cJSON_GetArraySize(clips);
  if (n == 0) { cJSON_Delete(root); return out; }

  out.items = (ClipPlan *)calloc(n, sizeof(ClipPlan));
  if (!out.items) die("OOM");
  out.count = 0;

  for (size_t i = 0; i < n; i++) {
    cJSON *obj = cJSON_GetArrayItem(clips, (int)i);
    if (!cJSON_IsObject(obj)) continue;

    cJSON *s = cJSON_GetObjectItemCaseSensitive(obj, "start");
    cJSON *e = cJSON_GetObjectItemCaseSensitive(obj, "end");
    cJSON *nar = cJSON_GetObjectItemCaseSensitive(obj, "narration");

    if (!cJSON_IsNumber(s) || !cJSON_IsNumber(e) || !cJSON_IsString(nar) || !nar->valuestring) continue;

    out.items[out.count].start = s->valueint;
    out.items[out.count].end = e->valueint;
    out.items[out.count].narration = str_dup(nar->valuestring);
    out.count++;
  }

  cJSON_Delete(root);
  return out;
}

static void *xrealloc(void *p, size_t n) {
  void *q = realloc(p, n);
  if (!q) die("OOM");
  return q;
}

static char *sanitize_utf8_lossy(const char *in) {
  if (!in) return str_dup("");
  size_t n = strlen(in);
  size_t cap = n * 4 + 1;
  char *out = (char *)malloc(cap);
  if (!out) die("OOM");

  size_t i = 0, j = 0;
  while (i < n) {
    unsigned char c = (unsigned char)in[i];

    if (c < 0x80) {
      if (j + 2 >= cap) { cap *= 2; out = (char *)xrealloc(out, cap); }
      out[j++] = (char)c;
      i++;
      continue;
    }

    if (c >= 0xC2 && c <= 0xDF) {
      if (i + 1 < n) {
        unsigned char c1 = (unsigned char)in[i + 1];
        if ((c1 & 0xC0) == 0x80) {
          if (j + 3 >= cap) { cap *= 2; out = (char *)xrealloc(out, cap); }
          out[j++] = (char)c;
          out[j++] = (char)c1;
          i += 2;
          continue;
        }
      }
    } else if (c >= 0xE0 && c <= 0xEF) {
      if (i + 2 < n) {
        unsigned char c1 = (unsigned char)in[i + 1];
        unsigned char c2 = (unsigned char)in[i + 2];
        if (((c1 & 0xC0) == 0x80) && ((c2 & 0xC0) == 0x80)) {
          if (c == 0xE0 && c1 < 0xA0) goto invalid;
          if (c == 0xED && c1 >= 0xA0) goto invalid;
          if (j + 4 >= cap) { cap *= 2; out = (char *)xrealloc(out, cap); }
          out[j++] = (char)c;
          out[j++] = (char)c1;
          out[j++] = (char)c2;
          i += 3;
          continue;
        }
      }
    } else if (c >= 0xF0 && c <= 0xF4) {
      if (i + 3 < n) {
        unsigned char c1 = (unsigned char)in[i + 1];
        unsigned char c2 = (unsigned char)in[i + 2];
        unsigned char c3 = (unsigned char)in[i + 3];
        if (((c1 & 0xC0) == 0x80) && ((c2 & 0xC0) == 0x80) && ((c3 & 0xC0) == 0x80)) {
          if (c == 0xF0 && c1 < 0x90) goto invalid;
          if (c == 0xF4 && c1 > 0x8F) goto invalid;
          if (j + 5 >= cap) { cap *= 2; out = (char *)xrealloc(out, cap); }
          out[j++] = (char)c;
          out[j++] = (char)c1;
          out[j++] = (char)c2;
          out[j++] = (char)c3;
          i += 4;
          continue;
        }
      }
    }

  invalid:
    if (j + 3 >= cap) { cap *= 2; out = (char *)xrealloc(out, cap); }
    if (c < 0xC0) {
      out[j++] = (char)0xC2;
      out[j++] = (char)c;
    } else {
      out[j++] = (char)0xC3;
      out[j++] = (char)(c - 0x40);
    }
    i++;
  }

  out[j] = 0;
  return out;
}

static char *trim_copy_utf8_safe(const char *s, size_t max_bytes) {
  if (!s) return str_dup("");
  size_t n = strlen(s);
  if (n <= max_bytes) return str_dup(s);

  size_t cut = max_bytes;
  if (cut >= n) cut = n;

  while (cut > 0 && cut < n && (((unsigned char)s[cut] & 0xC0) == 0x80)) cut--;

  char *out = (char *)malloc(cut + 1);
  if (!out) die("OOM");
  memcpy(out, s, cut);
  out[cut] = 0;
  return out;
}

static ClipPlanList openai_make_plan(const Config *cfg,
                                     const char *movie_title,
                                     const char *subs_seconds_text,
                                     const char *optional_script_text,
                                     int num_clips,
                                     bool *out_retry_without_script) {
  if (out_retry_without_script) *out_retry_without_script = false;

  const size_t MAX_SUB_CHARS    = 320000;
  const size_t MAX_SCRIPT_CHARS = 80000;

  char *title_utf8 = sanitize_utf8_lossy(movie_title ? movie_title : "");
  char *subs_utf8  = sanitize_utf8_lossy(subs_seconds_text ? subs_seconds_text : "");
  char *scr_utf8   = sanitize_utf8_lossy(optional_script_text ? optional_script_text : "");

  char *subs_trim = trim_copy_utf8_safe(subs_utf8, MAX_SUB_CHARS);
  char *scr_trim  = trim_copy_utf8_safe(scr_utf8,  MAX_SCRIPT_CHARS);

  free(subs_utf8);
  free(scr_utf8);

  const char *prompt_fmt =
    "You are given TWO inputs.\n"
    "Movie: %s\n"
    "\n"
    "INPUT A (Subtitles with timestamps in SECONDS):\n"
    "%s\n"
    "\n"
    "INPUT B (Optional script text WITHOUT timestamps; may be empty):\n"
    "%s\n"
    "\n"
    "TASK:\n"
    "- Choose %d non-overlapping time ranges that best cover the full plot arc.\n"
    "- ONLY use INPUT A for selecting start/end times (seconds). INPUT B is for story context.\n"
    "- Each time range should usually be 8-16 seconds long (end-start). Avoid >20 seconds.\n"
    "- Keep narrations punchy but not tiny: about 20-35 words total, in 3-5 short sentences.\n"
    "- Prefer ranges with clear visual action (reveals, confrontations, entrances, big moments).\n"
    "- Skip any range that starts at 0.\n"
    "- Return STRICT JSON with this shape ONLY:\n"
    "  {\"clips\":[{\"start\":120,\"end\":145,\"narration\":\"...\"}, ...]}\n"
    "- Clips must be increasing by start time.\n"
    "- Each narration must be at least 3 full sentences, casual commentator vibe.\n"
    "- The first narration must start with: \"Here we go, let's go over the movie %s.\".\n";

  int plen = snprintf(NULL, 0, prompt_fmt, title_utf8, subs_trim, scr_trim, num_clips, title_utf8);
  if (plen < 0) die("snprintf failed building prompt");
  char *prompt = (char *)malloc((size_t)plen + 1);
  if (!prompt) die("OOM");
  snprintf(prompt, (size_t)plen + 1, prompt_fmt, title_utf8, subs_trim, scr_trim, num_clips, title_utf8);

  free(title_utf8);
  free(subs_trim);
  free(scr_trim);

  cJSON *req = cJSON_CreateObject();
  cJSON_AddStringToObject(req, "model", cfg->openai_model);

  cJSON *reasoning = cJSON_CreateObject();
  cJSON_AddStringToObject(reasoning, "effort", "high");
  cJSON_AddItemToObject(req, "reasoning", reasoning);

  cJSON *input = cJSON_CreateArray();
  cJSON *sys = cJSON_CreateObject();
  cJSON_AddStringToObject(sys, "role", "system");
  cJSON_AddStringToObject(sys, "content", "You are a helpful assistant designed to output JSON.");
  cJSON_AddItemToArray(input, sys);

  cJSON *usr = cJSON_CreateObject();
  cJSON_AddStringToObject(usr, "role", "user");
  cJSON_AddStringToObject(usr, "content", prompt);
  cJSON_AddItemToArray(input, usr);
  cJSON_AddItemToObject(req, "input", input);

  cJSON *text = cJSON_CreateObject();
  cJSON *format = cJSON_CreateObject();
  cJSON_AddStringToObject(format, "type", "json_object");
  cJSON_AddItemToObject(text, "format", format);
  cJSON_AddItemToObject(req, "text", text);

  char *body = cJSON_PrintUnformatted(req);
  cJSON_Delete(req);
  free(prompt);

  if (!body) {
    ClipPlanList empty = {0};
    return empty;
  }

  long http_code = 0;
  bool has_script = (optional_script_text && optional_script_text[0] != 0);
  long timeout_s = has_script ? 14400L : 3600L;

  char endpoint[512];
  snprintf(endpoint, sizeof(endpoint), "%s/responses", cfg->openai_base_url);

  MemBuf resp = http_post_json_to_mem(endpoint, cfg->openai_key, body, &http_code, timeout_s);
  free(body);

  if (http_code < 200 || http_code >= 300) {
    logw("OpenAI HTTP %ld", http_code);
    if (resp.data && resp.size) logw("OpenAI raw body: %.800s", resp.data);

    if (has_script && resp.data && openai_resp_should_retry_without_script(resp.data)) {
      if (out_retry_without_script) *out_retry_without_script = true;
    }

    if (resp.data) free(resp.data);
    ClipPlanList empty = {0};
    return empty;
  }

  char *out_text = openai_extract_output_text(resp.data ? resp.data : "");
  if (!out_text) {
    logw("OpenAI response parse failed.");
    if (resp.data && resp.size) logw("OpenAI raw body: %.800s", resp.data);

    if (has_script && resp.data && openai_resp_should_retry_without_script(resp.data)) {
      if (out_retry_without_script) *out_retry_without_script = true;
    }

    if (resp.data) free(resp.data);
    ClipPlanList empty = {0};
    return empty;
  }

  ClipPlanList plan = parse_clip_plan_json(out_text);
  free(out_text);
  if (resp.data) free(resp.data);
  return plan;
}

static bool elevenlabs_tts_to_mp3(const Config *cfg, const char *text, const char *out_mp3_path) {
  char url[1024];
  snprintf(url, sizeof(url),
           "%s/text-to-speech/%s?output_format=mp3_44100_128",
           cfg->eleven_base_url, cfg->eleven_voice_id);

  cJSON *root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "text", text);
  cJSON_AddStringToObject(root, "model_id", cfg->eleven_model_id);
  char *body = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);

  CURL *curl = curl_easy_init();
  if (!curl) die("curl init failed");

  FILE *f = plat_fopen(out_mp3_path, "wb");
  if (!f) {
    curl_easy_cleanup(curl);
    free(body);
    return false;
  }

  struct curl_slist *headers = NULL;
  headers = curl_slist_append(headers, "Content-Type: application/json");

  char keyhdr[1024];
  snprintf(keyhdr, sizeof(keyhdr), "xi-api-key: %s", cfg->eleven_key);
  headers = curl_slist_append(headers, keyhdr);

  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, f);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_file_write_cb);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

  CURLcode res = curl_easy_perform(curl);
  long http_code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

  fclose(f);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  free(body);

  if (res != CURLE_OK) {
    plat_unlink(out_mp3_path);
    logw("ElevenLabs TTS failed: %s", curl_easy_strerror(res));
    return false;
  }
  if (http_code < 200 || http_code >= 300) {
    /* the body is a JSON error, not audio - show it and discard the file */
    char *err = read_entire_file(out_mp3_path);
    logw("ElevenLabs HTTP %ld: %.600s", http_code, err ? err : "");
    free(err);
    plat_unlink(out_mp3_path);
    return false;
  }
  return file_exists(out_mp3_path);
}

/* ============================================ free / local narration engines */

/* MP3 frame sync (0xFFEx) or an ID3 tag; WAV/OGG/FLAC payloads need ffmpeg. */
static bool looks_like_mp3(const unsigned char *d, size_t n) {
  if (n >= 3 && d[0] == 'I' && d[1] == 'D' && d[2] == '3') return true;
  if (n >= 2 && d[0] == 0xFF && (d[1] & 0xE0) == 0xE0) return true;
  return false;
}

/*
 * Everything downstream (the concat demuxer, the duration probe, the mixer)
 * expects MP3. ElevenLabs and OpenAI already return MP3; XTTS and Piper return
 * WAV, so convert those with ffmpeg. This keeps the rest of the pipeline
 * completely unaware of which engine spoke.
 */
static bool save_audio_as_mp3(const void *data, size_t len, const char *out_mp3_path) {
  if (!data || len < 64) return false;
  if (looks_like_mp3((const unsigned char *)data, len))
    return write_entire_file(out_mp3_path, data, len);

  char tmp[PATH_MAX];
  snprintf(tmp, sizeof(tmp), "%s.in", out_mp3_path);
  if (!write_entire_file(tmp, data, len)) return false;

  char *in_esc  = sh_escape(tmp);
  char *out_esc = sh_escape(out_mp3_path);
  int rc = run_cmd("ffmpeg -y -hide_banner -loglevel error -i %s -vn -c:a libmp3lame -b:a 192k %s",
                   in_esc, out_esc);
  free(in_esc);
  free(out_esc);
  plat_unlink(tmp);

  if (rc != 0) { logw("ffmpeg could not convert the narration audio to MP3"); return false; }
  return file_exists(out_mp3_path);
}

/* POST a JSON synthesis request, then store whatever audio comes back as MP3. */
static bool tts_post_audio(const char *url, const char *json_body, const char *bearer,
                           const char *out_mp3_path, const char *what) {
  long code = 0;
  MemBuf r = http_post_json_to_mem(url, bearer, json_body, &code, 300);

  if (code < 200 || code >= 300) {
    logw("%s HTTP %ld: %.600s", what, code, (r.data && r.data[0]) ? r.data : "no response");
    free(r.data);
    return false;
  }
  if (!r.data || r.size < 64) {
    logw("%s returned no audio", what);
    free(r.data);
    return false;
  }
  bool ok = save_audio_as_mp3(r.data, r.size, out_mp3_path);
  free(r.data);
  return ok;
}

/* Coqui XTTS v2 (daswer123/xtts-api-server): POST /tts_to_audio/ -> audio/wav */
static bool tts_xtts(const Config *cfg, const char *text, const char *out_mp3_path) {
  char url[1024];
  snprintf(url, sizeof(url), "%s/tts_to_audio/", cfg->tts_base_url);

  cJSON *root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "text", text);
  cJSON_AddStringToObject(root, "speaker_wav", cfg->tts_voice);
  cJSON_AddStringToObject(root, "language", cfg->tts_language);
  char *body = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);

  bool ok = tts_post_audio(url, body, NULL, out_mp3_path, "XTTS TTS");
  free(body);
  return ok;
}

/* Piper (python -m piper.http_server): POST /synthesize -> audio/wav */
static bool tts_piper(const Config *cfg, const char *text, const char *out_mp3_path) {
  char url[1024];
  snprintf(url, sizeof(url), "%s/synthesize", cfg->tts_base_url);

  cJSON *root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "text", text);
  if (cfg->tts_voice[0]) cJSON_AddStringToObject(root, "voice", cfg->tts_voice);
  char *body = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);

  bool ok = tts_post_audio(url, body, NULL, out_mp3_path, "Piper TTS");
  free(body);
  return ok;
}

/* Any OpenAI-compatible /v1/audio/speech endpoint: OpenAI, Kokoro-FastAPI, ... */
static bool tts_openai_compat(const Config *cfg, const char *text, const char *out_mp3_path) {
  char url[1024];
  snprintf(url, sizeof(url), "%s/audio/speech", cfg->tts_base_url);

  cJSON *root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "model", cfg->tts_model);
  cJSON_AddStringToObject(root, "input", text);
  cJSON_AddStringToObject(root, "voice", cfg->tts_voice[0] ? cfg->tts_voice : "alloy");
  cJSON_AddStringToObject(root, "response_format", "mp3");
  char *body = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);

  const char *key = cfg->tts_api_key[0] ? cfg->tts_api_key : cfg->openai_key;
  bool ok = tts_post_audio(url, body, key, out_mp3_path, "OpenAI-compatible TTS");
  free(body);
  return ok;
}

/* Single entry point used by the pipeline. */
static bool tts_synthesize(const Config *cfg, const char *text, const char *out_mp3_path) {
  switch (cfg->tts_provider) {
    case TTS_XTTS:  return tts_xtts(cfg, text, out_mp3_path);
    case TTS_PIPER: return tts_piper(cfg, text, out_mp3_path);
    case TTS_OPENAI: return tts_openai_compat(cfg, text, out_mp3_path);
    default:        return elevenlabs_tts_to_mp3(cfg, text, out_mp3_path);
  }
}

static bool ffmpeg_make_adjusted_clip(const Config *cfg, const char *input_mp4,
                                      int start_s, int end_s,
                                      const char *narration_mp3, double narration_dur,
                                      const char *out_mp4) {
  const double max_speedup = cfg->max_video_speedup;

  double orig_seg_dur = (double)(end_s - start_s);
  if (orig_seg_dur <= 0.1 || narration_dur <= 0.1) return false;

  int use_start = start_s;
  int use_end   = end_s;

  double speed = orig_seg_dur / narration_dur;

  if (speed > max_speedup) {
    double desired_src_dur = narration_dur * max_speedup;

    if (desired_src_dur > orig_seg_dur) desired_src_dur = orig_seg_dur;
    if (desired_src_dur < 1.0) desired_src_dur = 1.0;

    double center = ((double)start_s + (double)end_s) / 2.0;
    double half = desired_src_dur / 2.0;

    double ns = center - half;
    double ne = center + half;

    if (ns < (double)start_s) { ns = (double)start_s; ne = ns + desired_src_dur; }
    if (ne > (double)end_s)   { ne = (double)end_s;   ns = ne - desired_src_dur; }

    if (ns < (double)start_s) ns = (double)start_s;
    if (ne > (double)end_s)   ne = (double)end_s;

    use_start = (int)llround(ns);
    use_end   = (int)llround(ne);
    if (use_end <= use_start) use_end = use_start + 1;

    double new_seg_dur = (double)(use_end - use_start);
    speed = new_seg_dur / narration_dur;
    if (speed > max_speedup) speed = max_speedup;

    logi("Speed-cap applied: planned %d-%d (%.2fs) vs narr %.2fs => %.2fx. Using %d-%d (%.2fs) => %.2fx.",
         start_s, end_s, orig_seg_dur,
         narration_dur, orig_seg_dur / narration_dur,
         use_start, use_end, (double)(use_end - use_start),
         speed);
  }

  if (speed < 0.05) speed = 0.05;
  if (speed > 20.0) speed = 20.0;

  char *in_esc  = sh_escape(input_mp4);
  char *nar_esc = sh_escape(narration_mp3);
  char *out_esc = sh_escape(out_mp4);

  int rc = run_cmd(
    "ffmpeg -y -hide_banner -loglevel error "
    "-ss %d -to %d -i %s "
    "-i %s "
    "-filter_complex \"[0:v]setpts=PTS/%.10f[v]\" "
    "-map \"[v]\" -map 1:a "
    "-c:v libx264 -pix_fmt yuv420p -preset veryfast -crf 22 "
    "-c:a aac -b:a 192k "
    "-shortest %s",
    use_start, use_end, in_esc, nar_esc, speed, out_esc
  );

  free(in_esc);
  free(nar_esc);
  free(out_esc);

  return rc == 0 && file_exists(out_mp4);
}

static bool ffmpeg_concat_videos(const char *list_txt, const char *out_mp4) {
  char *list_esc = sh_escape(list_txt);
  char *out_esc  = sh_escape(out_mp4);

  int rc = run_cmd(
    "ffmpeg -y -hide_banner -loglevel error "
    "-f concat -safe 0 -i %s "
    "-c:v libx264 -pix_fmt yuv420p -preset veryfast -crf 22 "
    "-c:a aac -b:a 192k "
    "-movflags +faststart %s",
    list_esc, out_esc
  );

  free(list_esc);
  free(out_esc);
  return rc == 0 && file_exists(out_mp4);
}

static bool ffmpeg_trim_audio(const char *in_audio, double start_s, double dur_s, const char *out_m4a) {
  char *in_esc  = sh_escape(in_audio);
  char *out_esc = sh_escape(out_m4a);
  int rc = run_cmd(
    "ffmpeg -y -hide_banner -loglevel error "
    "-ss %.3f -i %s -t %.3f "
    "-c:a aac -b:a 192k %s",
    start_s, in_esc, dur_s, out_esc
  );
  free(in_esc);
  free(out_esc);
  return rc == 0 && file_exists(out_m4a);
}

static bool ffmpeg_concat_audio(const char *list_txt, const char *out_m4a) {
  char *list_esc = sh_escape(list_txt);
  char *out_esc  = sh_escape(out_m4a);
  int rc = run_cmd(
    "ffmpeg -y -hide_banner -loglevel error "
    "-f concat -safe 0 -i %s -c copy %s",
    list_esc, out_esc
  );
  free(list_esc);
  free(out_esc);
  return rc == 0 && file_exists(out_m4a);
}

static bool ffmpeg_mix_bgm(const Config *cfg, const char *video_in, const char *bgm_in,
                           const char *video_out) {
  char *v_esc = sh_escape(video_in);
  char *b_esc = sh_escape(bgm_in);
  char *o_esc = sh_escape(video_out);

  int rc = run_cmd(
    "ffmpeg -y -hide_banner -loglevel error "
    "-i %s -i %s "
    "-filter_complex \"[0:a]volume=%.3f[a0];[1:a]volume=%.3f[a1];"
    "[a0][a1]amix=inputs=2:duration=first:dropout_transition=2[a]\" "
    "-map 0:v -map \"[a]\" "
    "-c:v copy -c:a aac -b:a 192k -movflags +faststart %s",
    v_esc, b_esc, cfg->narration_volume, cfg->bgm_volume, o_esc
  );

  free(v_esc);
  free(b_esc);
  free(o_esc);
  return rc == 0 && file_exists(video_out);
}

static bool ffmpeg_make_vertical(const char *in_mp4, const char *out_mp4) {
  int w = 0, h = 0;
  if (!ffprobe_video_dimensions(in_mp4, &w, &h)) return false;

  double dur = ffprobe_duration_seconds(in_mp4);
  if (dur <= 0.1) return false;

  int out_w = (int)((double)h * 9.0 / 16.0 + 0.5);
  int out_h = h;

  out_w &= ~1;
  out_h &= ~1;

  char *in_esc  = sh_escape(in_mp4);
  char *out_esc = sh_escape(out_mp4);

  int rc = run_cmd(
    "ffmpeg -y -hide_banner -loglevel error "
    "-i %s -t %.3f "
    "-filter_complex "
    "\"[0:v]"
      "crop=iw*0.6:ih:iw*0.2:0,"
      "scale=%d:%d:force_original_aspect_ratio=decrease,"
      "pad=%d:%d:(ow-iw)/2:(oh-ih)/2:black"
    "[v]\" "
    "-map \"[v]\" -map 0:a? "
    "-c:v libx264 -pix_fmt yuv420p -preset veryfast -crf 22 "
    "-c:a aac -b:a 192k "
    "-movflags +faststart "
    "%s",
    in_esc, dur,
    out_w, out_h,
    out_w, out_h,
    out_esc
  );

  free(in_esc);
  free(out_esc);

  if (rc != 0) { plat_unlink(out_mp4); return false; }
  return file_exists(out_mp4);
}

static char **list_files_with_ext(const char *dir, const char *ext1, const char *ext2, size_t *out_n) {
  *out_n = 0;
  PlatDir *d = plat_opendir(dir);
  if (!d) return NULL;

  char **arr = NULL;
  size_t cap = 0;

  const char *name;
  bool is_dir = false;
  while ((name = plat_readdir(d, &is_dir))) {
    if (name[0] == '.' || is_dir) continue;
    size_t ln = strlen(name);
    bool ok = false;
    if (ext1) {
      size_t e1 = strlen(ext1);
      if (ln >= e1 && str_icmp(name + ln - e1, ext1) == 0) ok = true;
    }
    if (!ok && ext2) {
      size_t e2 = strlen(ext2);
      if (ln >= e2 && str_icmp(name + ln - e2, ext2) == 0) ok = true;
    }
    if (!ok) continue;

    if (*out_n + 1 > cap) {
      cap = cap ? cap * 2 : 16;
      arr = (char **)realloc(arr, cap * sizeof(char *));
      if (!arr) die("OOM");
    }
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    arr[*out_n] = str_dup(path);
    (*out_n)++;
  }
  plat_closedir(d);
  return arr;
}

/* Write one line of an ffmpeg concat list; escapes ' as '\'' (e.g. "Schindler's List"). */
static void concat_list_add(FILE *f, const char *name) {
  fputs("file '", f);
  for (const char *p = name; *p; p++) {
    if (*p == '\'') fputs("'\\''", f);
    else fputc(*p, f);
  }
  fputs("'\n", f);
}

static void free_str_list(char **lst, size_t n) {
  if (!lst) return;
  for (size_t i = 0; i < n; i++) free(lst[i]);
  free(lst);
}

static bool rm_rf_path(const char *path) {
  PlatKind k = plat_stat(path, NULL);
  if (k == PLAT_NONE) return false;
  if (k == PLAT_FILE) return plat_unlink(path) == 0;

  PlatDir *d = plat_opendir(path);
  if (!d) return false;

  bool ok = true;
  const char *name;
  while ((name = plat_readdir(d, NULL))) {
    char child[PATH_MAX];
    snprintf(child, sizeof(child), "%s/%s", path, name);
    if (!rm_rf_path(child)) ok = false;
  }
  plat_closedir(d);

  if (plat_rmdir(path) != 0) ok = false;
  return ok;
}

/* Clears clips/ recursively, but keeps the clips/audio/ folder itself
   (only the files inside it are removed) - same behaviour as the original. */
static bool clear_directory_contents(const char *dir_path) {
  if (!dir_exists(dir_path)) return true;

  PlatDir *d = plat_opendir(dir_path);
  if (!d) return false;

  bool ok = true;
  const char *name;
  while ((name = plat_readdir(d, NULL))) {
    char child[PATH_MAX];
    snprintf(child, sizeof(child), "%s/%s", dir_path, name);

    if (strcmp(name, "audio") == 0 && dir_exists(child)) {
      if (!clear_directory_contents(child)) ok = false;
      continue;
    }
    if (!rm_rf_path(child)) ok = false;
  }
  plat_closedir(d);
  return ok;
}

/* ----------------------- Movie pipeline ----------------------- */

static bool process_movie(const Config *cfg, const char *movie_path, const char *movie_title,
                          int num_clips, int movie_index, int movie_total) {
  ensure_dir("clips");
  ensure_dir("clips/audio");
  ensure_dir("output");
  ensure_dir("tiktok_output");
  ensure_dir("scripts");
  ensure_dir("scripts/srt_files");
  ensure_dir("movies_retired");

  char srt_in[PATH_MAX], srt_mod[PATH_MAX], script_txt[PATH_MAX];
  snprintf(srt_in, sizeof(srt_in), "scripts/srt_files/%s.srt", movie_title);
  snprintf(srt_mod, sizeof(srt_mod), "scripts/srt_files/%s_modified.srt", movie_title);
  snprintf(script_txt, sizeof(script_txt), "scripts/srt_files/%s_summary.txt", movie_title);

  report_progress(GEN_STAGE_SUBTITLES, movie_index, movie_total, 0, 0, movie_title);

  if (!file_exists(srt_in)) {
    logi("No SRT found for %s; attempting download...", movie_title);
    if (!download_subtitle_srt(movie_title, srt_in)) {
      logw("Subtitle download failed for %s. Place your SRT at: %s", movie_title, srt_in);
      return false;
    }
    logok("Downloaded SRT: %s", srt_in);
  } else {
    logok("Found SRT: %s", srt_in);
  }

  if (generator_cancel_requested()) return false;

  if (!file_exists(srt_mod)) {
    logi("Converting SRT timestamps -> seconds: %s -> %s", srt_in, srt_mod);
    if (!convert_srt_timestamps_to_seconds(srt_in, srt_mod)) {
      logw("Failed to convert SRT for %s", movie_title);
      return false;
    }
    logok("Converted subtitles (seconds): %s", srt_mod);
  } else {
    logok("Using cached converted subtitles: %s", srt_mod);
  }

  long sz = file_size_bytes(script_txt);
  if (sz >= 0 && sz < 200) {
    logw("IMSDb script file looks too small (%ld bytes). Deleting to retry: %s", sz, script_txt);
    plat_unlink(script_txt);
  }

  report_progress(GEN_STAGE_SCRIPT, movie_index, movie_total, 0, 0, movie_title);

  char imsdb_url[1600] = {0};
  if (file_exists(script_txt)) {
    logok("Found cached IMSDb script: %s (%ld bytes)", script_txt, file_size_bytes(script_txt));
  } else {
    logi("Attempting IMSDb script scrape for %s (optional context)...", movie_title);
    if (download_imsdb_script_ex(movie_title, script_txt, imsdb_url, sizeof(imsdb_url))) {
      logok("IMSDb script saved: %s (source: %s)", script_txt, imsdb_url[0] ? imsdb_url : "unknown");
    } else {
      logw("IMSDb scrape failed for %s (this is OK; continuing with subtitles-only).", movie_title);
    }
  }

  char *subs_seconds = read_entire_file(srt_mod);
  if (!subs_seconds) {
    logw("Failed to read converted subtitles for %s: %s", movie_title, srt_mod);
    return false;
  }
  logok("Loaded subtitles for planning: %s (%zu bytes)", srt_mod, strlen(subs_seconds));

  char *imsdb_script = NULL;
  if (file_exists(script_txt)) {
    imsdb_script = read_entire_file(script_txt);
    if (imsdb_script && strlen(imsdb_script) > 0) {
      logok("Loaded IMSDb script for extra context: %s (%zu bytes)", script_txt, strlen(imsdb_script));
    } else {
      if (imsdb_script) { free(imsdb_script); imsdb_script = NULL; }
      logw("IMSDb script file existed but was empty/unreadable: %s", script_txt);
    }
  } else {
    logi("No IMSDb script available; using subtitles only.");
  }

  if (generator_cancel_requested()) return false;

  report_progress(GEN_STAGE_PLANNING, movie_index, movie_total, 0, 0, movie_title);
  logi("Requesting OpenAI clip plan (%d clips target)...", num_clips);
  bool retry_no_script = false;
  ClipPlanList plan = openai_make_plan(cfg, movie_title, subs_seconds,
                                       imsdb_script ? imsdb_script : "",
                                       num_clips, &retry_no_script);

  if (plan.count == 0 && retry_no_script && imsdb_script && imsdb_script[0]) {
    logw("OpenAI request failed with IMSDb context; retrying without IMSDb script for %s", movie_title);
    plan = openai_make_plan(cfg, movie_title, subs_seconds, "", num_clips, NULL);
  }

  free(subs_seconds);
  if (imsdb_script) free(imsdb_script);

  if (plan.count == 0) {
    logw("No plan returned for %s", movie_title);
    free_clip_plan_list(&plan);
    return false;
  }
  logok("OpenAI plan received: %zu clips", plan.count);

  char concat_list_path[PATH_MAX];
  snprintf(concat_list_path, sizeof(concat_list_path), "clips/%s_concat_list.txt", movie_title);

  FILE *listf = plat_fopen(concat_list_path, "wb");
  if (!listf) {
    logw("Failed to create concat list: %s", concat_list_path);
    free_clip_plan_list(&plan);
    return false;
  }

  size_t made = 0;
  for (size_t i = 0; i < plan.count; i++) {
    if (generator_cancel_requested()) {
      logw("Cancel requested - stopping after clip %zu of %zu.", i, plan.count);
      break;
    }

    int start_s = plan.items[i].start;
    int end_s   = plan.items[i].end;
    if (start_s <= 0) { logw("Skipping clip %zu (start<=0)", i + 1); continue; }
    if (end_s <= start_s) { logw("Skipping clip %zu (end<=start)", i + 1); continue; }

    char nar_mp3[PATH_MAX];
    snprintf(nar_mp3, sizeof(nar_mp3), "clips/audio/%s_audio_%zu.mp3", movie_title, i + 1);

    report_progress(GEN_STAGE_TTS, movie_index, movie_total, (int)(i + 1), (int)plan.count, movie_title);
    logi("TTS clip %zu/%zu -> %s", i + 1, plan.count, nar_mp3);
    if (!tts_synthesize(cfg, plan.items[i].narration, nar_mp3)) {
      logw("TTS failed clip %zu for %s", i + 1, movie_title);
      continue;
    }

    double nar_dur = ffprobe_duration_seconds(nar_mp3);
    if (nar_dur <= 0.1) {
      logw("Bad narration duration for clip %zu", i + 1);
      continue;
    }

    char out_clip_name[PATH_MAX];
    snprintf(out_clip_name, sizeof(out_clip_name), "%s_clip_%zu.mp4", movie_title, i + 1);

    char out_clip[PATH_MAX];
    snprintf(out_clip, sizeof(out_clip), "clips/%s", out_clip_name);

    report_progress(GEN_STAGE_CLIP, movie_index, movie_total, (int)(i + 1), (int)plan.count, movie_title);
    logi("Building clip %zu: %d -> %d sec (narr=%.2fs) => %s", i + 1, start_s, end_s, nar_dur, out_clip);
    if (!ffmpeg_make_adjusted_clip(cfg, movie_path, start_s, end_s, nar_mp3, nar_dur, out_clip)) {
      logw("Failed to build adjusted clip %zu", i + 1);
      continue;
    }

    concat_list_add(listf, out_clip_name);
    made++;
    logok("Built clip %zu OK: %s", i + 1, out_clip);
  }

  fclose(listf);
  free_clip_plan_list(&plan);

  if (made == 0) {
    logw("No clips produced for %s", movie_title);
    return false;
  }
  logok("Clips produced: %zu (concat list: %s)", made, concat_list_path);

  char tmp_concat[PATH_MAX];
  snprintf(tmp_concat, sizeof(tmp_concat), "clips/%s_concat_tmp.mp4", movie_title);

  report_progress(GEN_STAGE_CONCAT, movie_index, movie_total, 0, (int)made, movie_title);
  logi("Concatenating clips -> %s", tmp_concat);
  if (!ffmpeg_concat_videos(concat_list_path, tmp_concat)) {
    logw("Concat failed for %s", movie_title);
    return false;
  }
  logok("Concat OK: %s", tmp_concat);

  double final_dur = ffprobe_duration_seconds(tmp_concat);
  if (final_dur <= 0.1) {
    logw("Bad final duration for %s", movie_title);
    return false;
  }
  logok("Final duration: %.2f seconds", final_dur);

  /* Cancelled after the clips were joined: keep the recap we already have and
     skip the optional BGM / vertical steps instead of burning more time. */
  if (generator_cancel_requested()) {
    char out_partial[PATH_MAX];
    snprintf(out_partial, sizeof(out_partial), "output/%s.mp4", movie_title);
    logw("Cancel requested - keeping the narration-only recap and skipping BGM/vertical.");
    plat_rename(tmp_concat, out_partial);
    return true;
  }

  size_t song_n = 0;
  char **songs = NULL;
  if (cfg->bgm_enabled) {
    report_progress(GEN_STAGE_BGM, movie_index, movie_total, 0, 0, movie_title);
    songs = list_files_with_ext("backgroundmusic", ".mp3", ".m4a", &song_n);
  } else {
    logi("Background music disabled in config (bgm_enabled=false).");
  }

  if (!songs || song_n == 0) {
    if (cfg->bgm_enabled) logw("No backgroundmusic files found; output will be narration-only.");
    else logi("Output will be narration-only (bgm_enabled=false).");
    char out_final_only[PATH_MAX];
    snprintf(out_final_only, sizeof(out_final_only), "output/%s.mp4", movie_title);
    plat_rename(tmp_concat, out_final_only);
    logok("Wrote output (no BGM): %s", out_final_only);
  } else {
    srand((unsigned)time(NULL));

    char bgm_list[PATH_MAX];
    snprintf(bgm_list, sizeof(bgm_list), "clips/%s_bgm_list.txt", movie_title);
    FILE *bgml = plat_fopen(bgm_list, "wb");
    if (!bgml) die("Failed bgm list create");

    logi("Building BGM track list (%zu songs available)...", song_n);

    double covered = 0.0;
    int part = 0;
    int misses = 0;
    while (covered + 0.01 < final_dur) {
      if (generator_cancel_requested()) { logw("Cancel requested - stopping BGM build."); break; }
      if (misses > 50) { logw("Too many unusable BGM tracks (need > 60s long); stopping BGM build."); break; }
      const char *song = songs[rand() % song_n];
      double sd = ffprobe_duration_seconds(song);
      if (sd <= 60.0) { misses++; continue; }

      double start = 40.0;
      double avail = sd - start;
      if (avail <= 1.0) { misses++; continue; }

      double need = final_dur - covered;
      double take = (avail < need) ? avail : need;

      char part_name[PATH_MAX];
      snprintf(part_name, sizeof(part_name), "%s_bgm_part_%d.m4a", movie_title, ++part);

      char part_path[PATH_MAX];
      snprintf(part_path, sizeof(part_path), "clips/%s", part_name);

      if (!ffmpeg_trim_audio(song, start, take, part_path)) { misses++; continue; }

      concat_list_add(bgml, part_name);
      covered += take;

      if (part > 200) break;
    }
    fclose(bgml);

    logok("BGM parts created: %d (covered %.2fs / %.2fs)", part, covered, final_dur);

    if (part == 0) {
      /* Nothing usable (or the run was cancelled): do not feed ffmpeg an empty list.
         (bgml was already closed above.) */
      logw("No usable BGM parts; output stays narration-only.");
      char out_final_only[PATH_MAX];
      snprintf(out_final_only, sizeof(out_final_only), "output/%s.mp4", movie_title);
      plat_rename(tmp_concat, out_final_only);
      logok("Wrote output (no BGM): %s", out_final_only);
      free_str_list(songs, song_n);
      goto after_bgm;
    }

    char bgm_out[PATH_MAX];
    snprintf(bgm_out, sizeof(bgm_out), "clips/%s_bgm.m4a", movie_title);

    logi("Concatenating BGM -> %s", bgm_out);
    if (!ffmpeg_concat_audio(bgm_list, bgm_out)) {
      logw("BGM concat failed; output narration-only.");
      char out_final_only[PATH_MAX];
      snprintf(out_final_only, sizeof(out_final_only), "output/%s.mp4", movie_title);
      plat_rename(tmp_concat, out_final_only);
      logok("Wrote output (no BGM): %s", out_final_only);
    } else {
      logok("BGM concat OK: %s", bgm_out);

      char out_final_only[PATH_MAX];
      snprintf(out_final_only, sizeof(out_final_only), "output/%s.mp4", movie_title);

      logi("Mixing narration + BGM -> %s", out_final_only);
      if (!ffmpeg_mix_bgm(cfg, tmp_concat, bgm_out, out_final_only)) {
        logw("Mix failed; output narration-only.");
        plat_rename(tmp_concat, out_final_only);
      } else {
        plat_unlink(tmp_concat);
      }
      logok("Wrote output: %s", out_final_only);
    }

    free_str_list(songs, song_n);
  }

after_bgm:
  if (generator_cancel_requested()) {
    logw("Cancel requested - skipping the remaining steps for %s.", movie_title);
  }

  char out_final[PATH_MAX], out_vert[PATH_MAX];
  snprintf(out_final, sizeof(out_final), "output/%s.mp4", movie_title);
  snprintf(out_vert,  sizeof(out_vert),  "tiktok_output/%s_vertical.mp4", movie_title);

  if (generator_cancel_requested()) {
    logw("Cancel requested - skipping the vertical render for %s.", movie_title);
  } else if (cfg->make_vertical) {
    report_progress(GEN_STAGE_VERTICAL, movie_index, movie_total, 0, 0, movie_title);
    logi("Rendering vertical -> %s", out_vert);
    if (!ffmpeg_make_vertical(out_final, out_vert)) {
      logw("Vertical render failed for %s", movie_title);
    } else {
      logok("Vertical render OK: %s", out_vert);
    }
  } else {
    logi("Vertical render disabled in config (make_vertical=false).");
  }

  if (cfg->retire_movies) {
    char retired[PATH_MAX];
    snprintf(retired, sizeof(retired), "movies_retired/%s.mp4", movie_title);
    if (plat_rename(movie_path, retired) == 0) {
      logok("Retired source movie -> %s", retired);
    } else {
      logw("Could not move %s -> %s (is the file open in another program?)", movie_path, retired);
    }
  } else {
    logi("Leaving %s in movies/ (retire_movies=false).", movie_path);
  }

  return true;
}

static bool output_already_exists(const char *movie_title) {
  char out[PATH_MAX];
  snprintf(out, sizeof(out), "output/%s.mp4", movie_title);
  return file_exists(out);
}

static void strip_ext(const char *filename, char *out, size_t outsz) {
  strncpy(out, filename, outsz - 1);
  out[outsz - 1] = 0;
  char *dot = strrchr(out, '.');
  if (dot) *dot = 0;
}

/* -------------------------- PUBLIC ENTRYPOINT -------------------------- */

/* State kept static so it survives a longjmp out of die(). */
static PlatDir *g_movies_dir = NULL;
static bool     g_curl_inited = false;

int run_generation(void) {
  g_movies_dir = NULL;
  g_curl_inited = false;

  if (setjmp(g_die_jmp) != 0) {
    /* a fatal error happened somewhere below */
    g_die_armed = false;
    if (g_movies_dir) { plat_closedir(g_movies_dir); g_movies_dir = NULL; }
    if (g_curl_inited) { curl_global_cleanup(); g_curl_inited = false; }
    report_progress(GEN_STAGE_FAILED, 0, 0, 0, 0, NULL);
    emit_line("Generation aborted (see FATAL message above).");
    return -1;
  }
  g_die_armed = true;

  report_progress(GEN_STAGE_SETUP, 0, 0, 0, 0, NULL);

  char cwd[PATH_MAX];
  plat_getcwd(cwd, sizeof(cwd));
  logi("Working directory: %s", cwd);

  /* FFmpeg must be installed and on PATH (winget install Gyan.FFmpeg). */
  if (!plat_have_tool("ffmpeg") || !plat_have_tool("ffprobe")) {
#if defined(_WIN32)
    die("ffmpeg/ffprobe not found in PATH. Install with:  winget install Gyan.FFmpeg  "
        "(then restart this app / open a new terminal)");
#else
    die("ffmpeg/ffprobe not found in PATH. Install FFmpeg and try again.");
#endif
  }
  logok("ffmpeg + ffprobe found.");

  curl_global_init(CURL_GLOBAL_DEFAULT);
  g_curl_inited = true;

  Config cfg = load_config_json("config.json");

  if (cfg.tts_provider == TTS_ELEVENLABS)
    logi("Narration engine: ElevenLabs (model %s)", cfg.eleven_model_id);
  else if (cfg.tts_provider == TTS_XTTS)
    logi("Narration engine: XTTS at %s (speaker %s, language %s)",
         cfg.tts_base_url, cfg.tts_voice, cfg.tts_language);
  else if (cfg.tts_provider == TTS_PIPER)
    logi("Narration engine: Piper at %s (voice %s)", cfg.tts_base_url,
         cfg.tts_voice[0] ? cfg.tts_voice : "server default");
  else
    logi("Narration engine: OpenAI-compatible %s/audio/speech (model %s)",
         cfg.tts_base_url, cfg.tts_model);

  ensure_dir("movies");
  ensure_dir("output");
  ensure_dir("backgroundmusic");
  ensure_dir("clips");
  ensure_dir("scripts");
  ensure_dir("scripts/srt_files");
  ensure_dir("tiktok_output");
  ensure_dir("movies_retired");

  logi("Clearing clips/ folder...");
  if (!clear_directory_contents("clips")) {
    logw("Failed to fully clear clips/ (continuing anyway).");
  } else {
    logok("Cleared clips/ folder.");
  }

  ensure_dir("clips");
  ensure_dir("clips/audio");

  srand((unsigned)time(NULL));
  int span = cfg.max_clips - cfg.min_clips;
  if (span < 0) span = 0;
  int num_clips = cfg.min_clips + (span > 0 ? (rand() % (span + 1)) : 0);
  logi("Clip plan target: %d clips (config allows %d-%d).", num_clips, cfg.min_clips, cfg.max_clips);

  /* Collect the movie list first: process_movie() moves files out of movies/,
     and modifying a directory while enumerating it is unreliable on Windows. */
  g_movies_dir = plat_opendir("movies");
  if (!g_movies_dir) die("Failed to open movies/");

  char **names = NULL;
  size_t n_names = 0, cap_names = 0;
  const char *name;
  bool is_dir = false;
  while ((name = plat_readdir(g_movies_dir, &is_dir))) {
    if (name[0] == '.' || is_dir) continue;
    size_t ln = strlen(name);
    if (ln < 4) continue;
    if (str_icmp(name + ln - 4, ".mp4") != 0) continue;
    if (n_names + 1 > cap_names) {
      cap_names = cap_names ? cap_names * 2 : 16;
      names = (char **)realloc(names, cap_names * sizeof(char *));
      if (!names) die("OOM");
    }
    names[n_names++] = str_dup(name);
  }
  plat_closedir(g_movies_dir);
  g_movies_dir = NULL;

  if (n_names == 0) {
    logw("No .mp4 files found in movies/. Put e.g. movies\\Citizen Kane.mp4 there and press START again.");
  }

  int processed = 0;
  for (size_t i = 0; i < n_names; i++) {
    if (generator_cancel_requested()) {
      logw("Cancel requested - %zu of %zu movies left unprocessed.", n_names - i, n_names);
      break;
    }

    char title[PATH_MAX];
    strip_ext(names[i], title, sizeof(title));

    if (output_already_exists(title)) {
      logi("Skipping %s (already in output/)", title);
      continue;
    }

    char path[PATH_MAX];
    snprintf(path, sizeof(path), "movies/%s", names[i]);

    char banner[PATH_MAX + 64];
    snprintf(banner, sizeof(banner), "=== Processing: %s ===", title);
    emit_line("");
    emit_line(banner);
    if (generator_cancel_requested()) { free_str_list(names, n_names); break; }

    report_progress(GEN_STAGE_SETUP, (int)(i + 1), (int)n_names, 0, 0, title);

    if (process_movie(&cfg, path, title, num_clips, (int)(i + 1), (int)n_names)) {
      processed++;
      snprintf(banner, sizeof(banner), "DONE: %s", title);
    } else {
      snprintf(banner, sizeof(banner), "FAILED: %s", title);
    }
    emit_line(banner);
  }
  free_str_list(names, n_names);

  if (generator_cancel_requested()) report_progress(GEN_STAGE_CANCELLED, 0, 0, 0, 0, NULL);
  else report_progress(GEN_STAGE_DONE, 0, 0, 0, 0, NULL);

  char done[128];
  snprintf(done, sizeof(done), "All done. Processed: %d", processed);
  emit_line("");
  emit_line(done);

  g_die_armed = false;
  curl_global_cleanup();
  g_curl_inited = false;
  return processed; /* 0 is also a valid "nothing to do" result */
}
