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

/* Wikimedia asks automated clients to identify themselves instead of sending a
   generic browser User-Agent, so the Wikipedia calls carry their own one. */
static const char *WIKI_UA =
  "AI-Movie-Shorts/2.0 (movie recap planner; "
  "https://github.com/farmanshahid471-code/moviecap)";

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

static MemBuf http_get_to_mem_ua(const char *url, long *http_code_out, const char *user_agent) {
  if (http_code_out) *http_code_out = -1;

  CURL *curl = curl_easy_init();
  if (!curl) {
    logw("curl_easy_init failed (out of memory?)");
    return (MemBuf){0};
  }

  MemBuf buf = (MemBuf){0};

  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

  curl_easy_setopt(curl, CURLOPT_USERAGENT, user_agent ? user_agent : BROWSER_UA);
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

static MemBuf http_get_to_mem_ex(const char *url, long *http_code_out) {
  return http_get_to_mem_ua(url, http_code_out, BROWSER_UA);
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
  TTS_OPENAI,          /* OpenAI-compatible POST /audio/speech (e.g. Kokoro)  */
  TTS_EDGE             /* Microsoft Edge neural TTS via bundled Python (free) */
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
  bool auto_transcribe;       /* faster-whisper when a movie has no subtitles */
  char whisper_model[32];     /* whisper model size; default "small"          */
  char recap_language[96];    /* language of the CURRENT run ("" = English)   */
  char recap_languages[4][96];/* languages to render, one recap each, in order */
  int  n_recap_languages;
  int  tts_voice_auto;        /* 1 = tts_voice was auto-picked, free to change */
  int  tts_rate;              /* narration speed in %, 100 = normal voice     */
  char caption_font[256];     /* font used for burnt-in captions              */
  char caption_font_zh[256];  /* per-language caption fonts ("" = use above)  */
  char caption_font_ar[256];
  char caption_font_es[256];

  /* plot summary context (Wikipedia) - keeps character names correct */
  bool   use_wikipedia_plot;  /* fetch the plot summary; default true */
  char   wikipedia_base_url[256]; /* "" = derive from the narration language */

  /* optional pipeline tuning */
  int    min_clips;          /* default 20   */
  int    max_clips;          /* default 30   */
  double max_video_speedup;  /* default 1.75 */
  double recap_minutes;     /* target recap length in minutes; 0 = auto */
  bool   captions;          /* burn small subtitles into clips; default true */
  double narration_volume;   /* default 2.5  */
  double bgm_volume;         /* default 0.1  */
  bool   bgm_enabled;        /* default true */
  bool   make_vertical;      /* default true */
  bool   retire_movies;      /* default true */
  bool   offline_planner;    /* default false: never ship raw subtitle text */
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

/* Language helpers for the multi-language recap pass.
 *
 * ONE table decides what every part of the pipeline means by a language: the
 * subtitle file tag (Title.fr.srt), the Wikipedia subdomain, the caption font,
 * the Edge voice and the right-to-left handling.  Before this table the only
 * languages the app really knew were Chinese, Arabic and Spanish - anything
 * else silently became "English" and got an English voice. */
static const struct { const char *name; const char *code; } k_langs[] = {
  { "english", "en" }, { "spanish", "es" }, { "espanol", "es" }, { "french", "fr" },
  { "german", "de" }, { "italian", "it" }, { "portuguese", "pt" }, { "russian", "ru" },
  { "hindi", "hi" }, { "urdu", "ur" }, { "arabic", "ar" }, { "chinese", "zh" },
  { "mandarin", "zh" }, { "japanese", "ja" }, { "korean", "ko" }, { "turkish", "tr" },
  { "indonesian", "id" }, { "dutch", "nl" }, { "polish", "pl" }, { "vietnamese", "vi" },
  { "thai", "th" }, { "bengali", "bn" }, { "tamil", "ta" }, { "filipino", "tl" },
  { "tagalog", "tl" }, { "greek", "el" }, { "hebrew", "he" }, { "swedish", "sv" },
  { "ukrainian", "uk" }, { "persian", "fa" }, { "farsi", "fa" }, { "malay", "ms" },
  { NULL, NULL }
};

/* A small ring of buffers so several of these can be alive at once (a code is
 * often kept in a local while other code is looked up); anything that lives
 * longer than a few lines copies the result. */
static const char *lang_code_of(const char *lang) {
  static char buf[8][8];
  static unsigned turn = 0;
  char *out = buf[turn & 7u];
  turn++;
  snprintf(out, 8, "en");
  if (!lang || !lang[0]) return out;

  char low[64];
  size_t i = 0;
  for (; lang[i] && i + 1 < sizeof(low); i++)
    low[i] = (char)tolower((unsigned char)lang[i]);
  low[i] = '\0';

  for (int k = 0; k_langs[k].name; k++)
    if (!strcmp(low, k_langs[k].name)) { snprintf(out, 8, "%s", k_langs[k].code); return out; }

  /* a plain two letter code was given ("es", "zh", "de", ...) */
  if (strlen(low) == 2 && isalpha((unsigned char)low[0]) && isalpha((unsigned char)low[1])) {
    snprintf(out, 8, "%s", low);
    return out;
  }

  /* "Chinese (Simplified)", "Spanish (Latin America)", ... */
  for (int k = 0; k_langs[k].name; k++)
    if (strstr(low, k_langs[k].name)) { snprintf(out, 8, "%s", k_langs[k].code); return out; }
  return out;
}

static const char *recap_lang_code(const char *lang) { return lang_code_of(lang); }

/* Label used in the output file name ("Title (Chinese).mp4") and the banner. */
static const char *recap_lang_label(const char *lang) {
  const char *c = recap_lang_code(lang);
  if (!strcmp(c, "en")) return "";
  if (!strcmp(c, "zh")) return "Chinese";
  if (!strcmp(c, "ar")) return "Arabic";
  if (!strcmp(c, "es")) return "Spanish";
  if (!strcmp(c, "fr")) return "French";
  if (!strcmp(c, "de")) return "German";
  if (!strcmp(c, "it")) return "Italian";
  if (!strcmp(c, "pt")) return "Portuguese";
  if (!strcmp(c, "ru")) return "Russian";
  if (!strcmp(c, "hi")) return "Hindi";
  if (!strcmp(c, "ur")) return "Urdu";
  if (!strcmp(c, "ja")) return "Japanese";
  if (!strcmp(c, "ko")) return "Korean";
  if (!strcmp(c, "tr")) return "Turkish";
  if (!strcmp(c, "id")) return "Indonesian";
  if (!strcmp(c, "nl")) return "Dutch";
  if (!strcmp(c, "pl")) return "Polish";
  if (!strcmp(c, "vi")) return "Vietnamese";
  if (!strcmp(c, "th")) return "Thai";
  if (!strcmp(c, "bn")) return "Bengali";
  if (!strcmp(c, "ta")) return "Tamil";
  if (!strcmp(c, "el")) return "Greek";
  if (!strcmp(c, "he")) return "Hebrew";
  if (!strcmp(c, "sv")) return "Swedish";
  if (!strcmp(c, "uk")) return "Ukrainian";
  if (!strcmp(c, "fa")) return "Persian";
  if (!strcmp(c, "ms")) return "Malay";
  if (!strcmp(c, "tl")) return "Filipino";
  return "Other";
}

/* A voice that actually speaks the language; Edge TTS voice names are
 * <locale>-<Name>Neural. */
static const char *edge_voice_for_language(const char *lang) {
  static const struct { const char *code; const char *voice; } v[] = {
    { "zh", "zh-CN-YunxiNeural" },   { "ar", "ar-EG-ShakirNeural" },
    { "es", "es-MX-JorgeNeural" },   { "fr", "fr-FR-HenriNeural" },
    { "de", "de-DE-ConradNeural" },  { "it", "it-IT-DiegoNeural" },
    { "pt", "pt-BR-AntonioNeural" }, { "ru", "ru-RU-DmitryNeural" },
    { "hi", "hi-IN-MadhurNeural" },  { "ur", "ur-PK-AsadNeural" },
    { "ja", "ja-JP-KeitaNeural" },   { "ko", "ko-KR-InJoonNeural" },
    { "tr", "tr-TR-AhmetNeural" },   { "id", "id-ID-ArdiNeural" },
    { "nl", "nl-NL-MaartenNeural" }, { "pl", "pl-PL-MarekNeural" },
    { "vi", "vi-VN-NamMinhNeural" }, { "th", "th-TH-NiwatNeural" },
    { NULL, NULL }
  };
  const char *c = recap_lang_code(lang);
  for (int k = 0; v[k].code; k++)
    if (!strcmp(c, v[k].code)) return v[k].voice;
  return "en-US-ChristopherNeural";
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
  else if (str_icmp(c.tts_provider_name, "edge") == 0 ||
           str_icmp(c.tts_provider_name, "edge_tts") == 0)   c.tts_provider = TTS_EDGE;
  else if (str_icmp(c.tts_provider_name, "openai_tts") == 0) c.tts_provider = TTS_OPENAI;
  else if (str_icmp(c.tts_provider_name, "openai") == 0)     c.tts_provider = TTS_OPENAI;
  else die("config.json: unknown tts_provider \"%s\" (use elevenlabs, xtts, piper, edge or openai_tts)",
           c.tts_provider_name);

  if (c.tts_base_url[0] == 0) {
    const char *dflt = (c.tts_provider == TTS_XTTS)  ? "http://127.0.0.1:8020" :
                       (c.tts_provider == TTS_PIPER) ? "http://127.0.0.1:5000" :
                       (c.tts_provider == TTS_OPENAI) ? "https://api.openai.com/v1" : "";
    snprintf(c.tts_base_url, sizeof(c.tts_base_url), "%s", dflt);
  }
  if (c.tts_language[0] == 0) snprintf(c.tts_language, sizeof(c.tts_language), "en");
  if (c.tts_model[0] == 0)    snprintf(c.tts_model, sizeof(c.tts_model), "tts-1");
  cfg_set_str(c.recap_language, sizeof(c.recap_language), cJSON_GetObjectItemCaseSensitive(root, "recap_language"));
  cfg_set_str(c.caption_font, sizeof(c.caption_font), cJSON_GetObjectItemCaseSensitive(root, "caption_font"));
  if (c.caption_font[0] == 0) snprintf(c.caption_font, sizeof(c.caption_font), "resources/Inter-Regular.ttf");
  cfg_set_str(c.caption_font_zh, sizeof(c.caption_font_zh), cJSON_GetObjectItemCaseSensitive(root, "caption_font_zh"));
  cfg_set_str(c.caption_font_ar, sizeof(c.caption_font_ar), cJSON_GetObjectItemCaseSensitive(root, "caption_font_ar"));
  cfg_set_str(c.caption_font_es, sizeof(c.caption_font_es), cJSON_GetObjectItemCaseSensitive(root, "caption_font_es"));
  c.use_wikipedia_plot = cfg_get_bool(cJSON_GetObjectItemCaseSensitive(root, "use_wikipedia_plot"), true);
  cfg_set_str(c.wikipedia_base_url, sizeof(c.wikipedia_base_url),
              cJSON_GetObjectItemCaseSensitive(root, "wikipedia_base_url"));
  c.tts_rate = (int)cfg_get_dbl(cJSON_GetObjectItemCaseSensitive(root, "tts_rate"), 110, 50, 200);
  if (c.tts_provider == TTS_XTTS && c.tts_voice[0] == 0)
    die("config.json: tts_voice must name an XTTS speaker (a .wav in the server's speakers folder)");
  c.tts_voice_auto = 0;
  if (c.tts_provider == TTS_EDGE && c.tts_voice[0] == 0) {
    snprintf(c.tts_voice, sizeof(c.tts_voice), "%s", edge_voice_for_language(c.recap_language));
    c.tts_voice_auto = 1;
  }

  c.n_recap_languages = 0;
  cJSON *langs = cJSON_GetObjectItemCaseSensitive(root, "recap_languages");
  if (cJSON_IsArray(langs)) {
    const cJSON *it = NULL;
    cJSON_ArrayForEach(it, langs) {
      if (cJSON_IsString(it) && c.n_recap_languages < 4) {
        snprintf(c.recap_languages[c.n_recap_languages], sizeof(c.recap_languages[0]),
                 "%s", it->valuestring);
        c.n_recap_languages++;
      }
    }
  }
  if (c.n_recap_languages == 0) {
    snprintf(c.recap_languages[0], sizeof(c.recap_languages[0]), "%s", c.recap_language);
    c.n_recap_languages = 1;
  }

  c.min_clips         = cfg_get_int(cJSON_GetObjectItemCaseSensitive(root, "min_clips"), MIN_NUM_CLIPS, 1, 200);
  c.max_clips         = cfg_get_int(cJSON_GetObjectItemCaseSensitive(root, "max_clips"), MAX_NUM_CLIPS, 1, 200);
  c.max_video_speedup = cfg_get_dbl(cJSON_GetObjectItemCaseSensitive(root, "max_video_speedup"), MAX_VIDEO_SPEEDUP, 1.0, 8.0);
  c.recap_minutes     = cfg_get_dbl(cJSON_GetObjectItemCaseSensitive(root, "recap_minutes"), 0, 0, 180);
  c.captions          = cfg_get_bool(cJSON_GetObjectItemCaseSensitive(root, "captions"), true);
  c.auto_transcribe   = cfg_get_bool(cJSON_GetObjectItemCaseSensitive(root, "auto_transcribe"), true);
  cfg_set_str(c.whisper_model, sizeof(c.whisper_model), cJSON_GetObjectItemCaseSensitive(root, "whisper_model"));
  if (c.whisper_model[0] == 0) snprintf(c.whisper_model, sizeof(c.whisper_model), "small");
  c.narration_volume  = cfg_get_dbl(cJSON_GetObjectItemCaseSensitive(root, "narration_volume"), 2.5, 0.0, 10.0);
  c.bgm_volume        = cfg_get_dbl(cJSON_GetObjectItemCaseSensitive(root, "bgm_volume"), 0.1, 0.0, 10.0);
  c.bgm_enabled       = cfg_get_bool(cJSON_GetObjectItemCaseSensitive(root, "bgm_enabled"), true);
  c.make_vertical     = cfg_get_bool(cJSON_GetObjectItemCaseSensitive(root, "make_vertical"), true);
  c.retire_movies     = cfg_get_bool(cJSON_GetObjectItemCaseSensitive(root, "retire_movies"), true);
  /* The offline planner narrates the raw subtitle lines.  That is not a recap,
     so it is off unless the user explicitly asks for it. */
  c.offline_planner   = cfg_get_bool(cJSON_GetObjectItemCaseSensitive(root, "offline_planner"), false);

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

/* "00:01:23,456" -> 83.  Also accepts a '.' decimal separator and the short
 * "MM:SS,mmm" form some subtitle files use. */
static int timestamp_to_seconds(const char *ts) {
  char buf[64];
  size_t i = 0;
  for (; ts[i] && i + 1 < sizeof(buf); i++) buf[i] = (ts[i] == '.') ? ',' : ts[i];
  buf[i] = '\0';

  int a = 0, b = 0, c = 0, d = 0;
  /* With milliseconds - the usual "00:10:06,500" and "00:10:06.500" forms. */
  if (sscanf(buf, "%d:%d:%d,%d", &a, &b, &c, &d) == 4) return a * 3600 + b * 60 + c;
  /* Without them: some tracks (and hand-made ones) write "00:10:06". */
  if (sscanf(buf, "%d:%d:%d", &a, &b, &c) == 3) return a * 3600 + b * 60 + c;
  /* mm:ss[,ms] */
  if (sscanf(buf, "%d:%d,%d", &a, &b, &c) == 3) return a * 60 + b;
  if (sscanf(buf, "%d:%d", &a, &b) == 2) return a * 60 + b;
  return -1;
}

/* SRT -> "seconds" subtitles.  Besides converting the timestamps this drops
 * <i> markup, and (important) it writes the cues back in chronological order:
 * some subtitle files - merged tracks, per-CD files glued together, editors'
 * leftovers - are not sorted by time, and an out-of-order subtitle file makes
 * the whole recap (clip ranges, narration and burnt-in captions) come out in
 * the wrong order. */
typedef struct {
  int   start;
  int   end;
  char *text;      /* may contain newlines */
} SrtCue;

/* Append a line to a cue's text (lines joined with newlines). */
static void srt_cue_append(char **text, const char *txt) {
  size_t a = *text ? strlen(*text) : 0;
  size_t b = strlen(txt);
  char *m = (char *)realloc(*text, a + b + 2);
  if (!m) die("OOM");
  if (a) m[a++] = '\n';
  memcpy(m + a, txt, b + 1);
  *text = m;
}

static void srt_cue_push(SrtCue **arr, size_t *n, size_t *cap, int st, int en, char *text) {
  if (*n + 1 > *cap) {
    *cap = *cap ? *cap * 2 : 64;
    *arr = (SrtCue *)realloc(*arr, *cap * sizeof(SrtCue));
    if (!*arr) die("OOM");
  }
  (*arr)[*n].start = st;
  (*arr)[*n].end = en;
  (*arr)[*n].text = text;
  (*n)++;
}

/* Parse a subtitle file into cues (in file order).
 *
 * A line that holds nothing but digits is either the next cue's index or real
 * dialogue ("1944", "42", "3").  The index is sequential and is followed by a
 * timestamp, so a digits-only line is held back for one line and only dropped
 * when it continues the numbering AND a timestamp comes next.  Everything else
 * is text - the old code dropped those lines, which is how a year could
 * disappear from the narration. */
static bool srt_parse_cues(const char *data, SrtCue **out, size_t *out_n) {
  *out = NULL;
  *out_n = 0;

  SrtCue *cues = NULL;
  size_t n = 0, cap = 0;
  bool open = false;
  int  st = 0, en = 0;
  char *text = NULL;

  int  expect_index = 1;
  char *pending_num = NULL;      /* digits-only line, not classified yet */

  char *work = str_dup(data ? data : "");
  if (!work) return false;

  char *cur = work;
  while (cur) {
    char *nl = strchr(cur, '\n');
    if (nl) *nl = '\0';
    char *line = cur;
    cur = nl ? nl + 1 : NULL;

    size_t ll = strlen(line);
    while (ll > 0 && (line[ll - 1] == '\r' || line[ll - 1] == ' ' || line[ll - 1] == '\t'))
      line[--ll] = '\0';
    while (*line == ' ' || *line == '\t') line++;

    /* drop <i>/<b>/<u> style markup, keep the words */
    for (;;) {
      char *lt = strchr(line, '<');
      if (!lt) break;
      char *gt = strchr(lt, '>');
      if (!gt) break;
      memmove(lt, gt + 1, strlen(gt + 1) + 1);
    }

    char a[64], b[64];
    if (sscanf(line, "%63s --> %63s", a, b) == 2 && strchr(a, ':') && strchr(b, ':')) {
      int s1 = timestamp_to_seconds(a);
      int s2 = timestamp_to_seconds(b);
      if (s1 >= 0 && s2 >= 0) {
        /* the held digits line sits directly before this timestamp: it was the
           cue index, not text */
        if (pending_num) { free(pending_num); pending_num = NULL; expect_index++; }
        if (open && text) srt_cue_push(&cues, &n, &cap, st, en, text);
        else if (text) free(text);
        open = true; st = s1; en = s2; text = NULL;
        continue;
      }
    }

    if (*line == '\0') {
      if (pending_num) { srt_cue_append(&text, pending_num); free(pending_num); pending_num = NULL; }
      if (open && text) srt_cue_push(&cues, &n, &cap, st, en, text);
      else if (text) free(text);
      open = false; text = NULL;
      continue;
    }

    if (open) {
      bool digits_only = true;
      int  value = 0;
      for (const char *d = line; *d; d++) {
        if (*d < '0' || *d > '9') { digits_only = false; break; }
        if (value < 100000) value = value * 10 + (*d - '0');
      }
      if (digits_only && value == expect_index) {
        free(pending_num);
        pending_num = str_dup(line);       /* maybe the next cue's index */
        continue;
      }
      if (pending_num) { srt_cue_append(&text, pending_num); free(pending_num); pending_num = NULL; }
      srt_cue_append(&text, line);
    }
  }
  if (pending_num) { srt_cue_append(&text, pending_num); free(pending_num); }
  if (open && text) srt_cue_push(&cues, &n, &cap, st, en, text);
  else if (text) free(text);
  free(work);

  *out = cues;
  *out_n = n;
  return true;
}

/* ---------------------------------------------------------------------------
 * Turning a fragment stream into sentences.
 *
 * A subtitle track is written for somebody who also sees the picture, so it is
 * cut into whatever fits the screen: "Star." / "Command.", "I" / "can't" /
 * "see the stars above us too much, Fog."  Handed to the model like that, that
 * is what comes back - fragments instead of a story - and the converted file
 * the user opens shows the same clump of pieces.  So the converted file is
 * assembled into sentence-sized cues: consecutive cues are joined while the
 * earlier one has no sentence ending yet, while the pause between them is
 * short, and while the result stays a sane length.  The window grows with the
 * merge, so a joined cue still covers the moment its words are spoken.
 * ------------------------------------------------------------------------ */
#define SRT_MERGE_GAP_SEC   1.50   /* a longer pause is a new beat            */
#define SRT_MERGE_MAX_CHARS 400    /* never a paragraph on one line           */
#define SRT_MERGE_MAX_SEC   30.0   /* never one cue over half a minute        */

/* Does this text finish a sentence?  Trailing quotes and brackets are ignored. */
static bool srt_text_ends_sentence(const char *t) {
  if (!t) return false;
  size_t n = strlen(t);
  for (;;) {
    while (n > 0 && (t[n - 1] == ' ' || t[n - 1] == '\t' ||
                     t[n - 1] == '\n' || t[n - 1] == '\r')) n--;
    if (n == 0) return true;
    unsigned char c = (unsigned char)t[n - 1];
    if (c == '"' || c == '\'' || c == ')' || c == ']' || c == '}') { n--; continue; }
    /* the closing half of a CJK quote (\u300d / \u300f) */
    if (n >= 3 && (unsigned char)t[n - 1] == 0x8D && (unsigned char)t[n - 2] == 0x80 &&
        (unsigned char)t[n - 3] == 0xE3) { n -= 3; continue; }
    if (n >= 3 && (unsigned char)t[n - 1] == 0x8F && (unsigned char)t[n - 2] == 0x80 &&
        (unsigned char)t[n - 3] == 0xE3) { n -= 3; continue; }
    break;
  }
  unsigned char c = (unsigned char)t[n - 1];
  if (c == '.' || c == '!' || c == '?') return true;
  if (n >= 3) {
    if (c == 0x82 && (unsigned char)t[n - 2] == 0x80 && (unsigned char)t[n - 3] == 0xE3) return true;  /* ideographic full stop */
    if (c == 0x81 && (unsigned char)t[n - 2] == 0xBC && (unsigned char)t[n - 3] == 0xEF) return true;  /* fullwidth exclamation mark */
    if (c == 0x9F && (unsigned char)t[n - 2] == 0xBC && (unsigned char)t[n - 3] == 0xEF) return true;  /* fullwidth question mark */
  }
  if (n >= 2 && c == 0x9F && (unsigned char)t[n - 2] == 0xD8) return true;                            /* Arabic question mark */
  return false;
}

/* "- " / en dash at the start of a cue marks a different speaker: never join it
 * onto the line before it. */
static bool srt_text_is_speaker_change(const char *t) {
  if (!t) return false;
  while (*t == ' ' || *t == '\t') t++;
  if (*t == '-') return true;
  if ((unsigned char)t[0] == 0xE2 && (unsigned char)t[1] == 0x80 &&
      ((unsigned char)t[2] == 0x93 || (unsigned char)t[2] == 0x94)) return true;
  return false;
}

/* Keep the written file well formed: no zero length cue and no cue running
 * into the next one (overlapping subtitles are common - two people talking at
 * once - and they would make the converted file look clumped again). */
static void srt_fix_windows(SrtCue *cues, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if (i + 1 < n && cues[i].end > cues[i + 1].start && cues[i + 1].start > cues[i].start)
      cues[i].end = cues[i + 1].start;
    if (cues[i].end <= cues[i].start) cues[i].end = cues[i].start + 1;
  }
}

static void srt_merge_fragments(SrtCue *cues, size_t *n) {
  size_t w = 0;
  for (size_t i = 0; i < *n; i++) {
    if (w > 0) {
      SrtCue *prev = &cues[w - 1];
      SrtCue *curc = &cues[i];
      size_t plen = prev->text ? strlen(prev->text) : 0;
      size_t clen = curc->text ? strlen(curc->text) : 0;
      bool join =
        prev->text && prev->text[0] && curc->text && curc->text[0] &&
        !srt_text_ends_sentence(prev->text) &&
        !srt_text_is_speaker_change(curc->text) &&
        (double)(curc->start - prev->end) <= SRT_MERGE_GAP_SEC &&
        (double)(curc->start - prev->end) >= -1.0 &&
        (double)(prev->end - prev->start) + (double)(curc->end - curc->start) <= SRT_MERGE_MAX_SEC &&
        plen + clen + 2 <= SRT_MERGE_MAX_CHARS;
      if (join) {
        char *m = (char *)realloc(prev->text, plen + clen + 2);
        if (!m) die("OOM");
        m[plen] = ' ';
        memcpy(m + plen + 1, curc->text, clen + 1);
        prev->text = m;
        if (curc->end > prev->end) prev->end = curc->end;
        free(curc->text);
        continue;
      }
    }
    cues[w++] = cues[i];
  }
  *n = w;
}

/* Count one line of cue text: how many words, and whether it is only a
 * fragment of a sentence (a couple of words, or a second of speech). */
static void count_cue_text(const char *line, int *cues, int *fragments) {
  int words = 0, nchars = 0;
  bool in_word = false;
  for (const char *q = line; *q; q++) {
    bool space = (*q == ' ' || *q == '\t');
    if (space) in_word = false;
    else { if (!in_word) { in_word = true; words++; } nchars++; }
  }
  if (nchars <= 0) return;
  (*cues)++;
  if (words <= 3) (*fragments)++;
}

/* True when a cached "seconds" subtitle file can be reused: it runs forwards
 * in time (older versions wrote them in the file's original, sometimes
 * scrambled order) and it has been structured into sentences rather than left
 * as a stream of two-word fragments (older versions did not merge at all, so
 * those files get re-converted instead of reused). */
static bool srt_seconds_file_is_usable(const char *path) {
  char *data = read_entire_file(path);
  if (!data) return false;

  bool ok = true;
  int prev = -1;
  int cues = 0, fragments = 0;
  const char *pending_digits = NULL;   /* may be a cue index, may be text */
  char *cur = data;
  while (cur) {
    char *nl = strchr(cur, '\n');
    if (nl) *nl = '\0';
    int s1 = 0, s2 = 0;
    if (sscanf(cur, "%d --> %d", &s1, &s2) == 2) {
      /* the digits line just before a window was this cue's index */
      pending_digits = NULL;
      if (s1 < prev) { ok = false; break; }
      prev = s1;
    } else {
      if (pending_digits) { count_cue_text(pending_digits, &cues, &fragments); pending_digits = NULL; }
      if (*cur) {
        bool digits_only = true;
        for (const char *q = cur; *q; q++)
          if (*q < '0' || *q > '9') { digits_only = false; break; }
        if (digits_only) pending_digits = cur;
        else count_cue_text(cur, &cues, &fragments);
      }
    }
    cur = nl ? nl + 1 : NULL;
  }
  free(data);
  if (!ok) return false;
  /* Most cues only a few words long means the file is still the old stream of
     subtitle fragments: re-convert it.  A sentence cue is well over three
     words, so a structured file stays far below this ratio. */
  if (cues >= 3 && fragments * 100 > cues * 60) return false;
  return true;
}

/* A subtitle track that only covers part of the movie (a per-CD file, a
 * trailer, a subtitle sheet for another release) is the number one reason a
 * recap talks about characters and events that are not on screen: the story
 * is retold from the wrong timestamps.  Say so instead of shipping it. */
static void warn_if_subtitles_cover_wrong_span(const char *subs_seconds_text,
                                               double movie_dur,
                                               const char *movie_title) {
  if (!subs_seconds_text || !subs_seconds_text[0] || movie_dur < 120.0) return;

  int first = -1, last = -1;
  char *work = str_dup(subs_seconds_text);
  if (!work) return;
  char *cur = work;
  while (cur) {
    char *nl = strchr(cur, '\n');
    if (nl) *nl = '\0';
    int s1 = 0, s2 = 0;
    if (sscanf(cur, "%d --> %d", &s1, &s2) == 2) {
      if (first < 0) first = s1;
      if (s2 > last) last = s2;
    }
    cur = nl ? nl + 1 : NULL;
  }
  free(work);
  if (first < 0 || last <= 0) return;

  char span[64];
  snprintf(span, sizeof(span), "%d:%02d to %d:%02d",
           first / 60, first % 60, last / 60, last % 60);

  if ((double)last < movie_dur * 0.5 || (double)first > movie_dur * 0.30) {
    logw("The subtitles only cover %s, but the movie is %d:%02d long. This looks "
         "like the wrong file (another cut/release, or only part of the movie), "
         "so the narration will describe moments that are not on screen. Put the "
         "full subtitle track at scripts/srt_files/%s.srt, delete "
         "scripts/srt_files/%s_modified.srt and run again.",
         span, (int)(movie_dur / 60.0), ((int)movie_dur) % 60, movie_title, movie_title);
  } else if ((double)last > movie_dur * 1.05) {
    logw("The subtitles run to %d:%02d but the movie ends at %d:%02d - they are "
         "probably for a longer/extended release, so the clip times will drift "
         "away from the picture after a while.",
         last / 60, last % 60, (int)(movie_dur / 60.0), ((int)movie_dur) % 60);
  }
}

static bool convert_srt_timestamps_to_seconds(const char *input_srt, const char *output_srt) {
  char *data = read_entire_file(input_srt);
  if (!data) return false;

  SrtCue *cues = NULL;
  size_t n = 0;
  srt_parse_cues(data, &cues, &n);
  free(data);

  if (n == 0) {
    /* Nothing that looks like a cue (a plain text file?): keep a copy so the
       pipeline still has something to plan from. */
    FILE *in = plat_fopen(input_srt, "rb");
    if (!in) { free(cues); return false; }
    FILE *out = plat_fopen(output_srt, "wb");
    if (!out) { fclose(in); free(cues); return false; }
    char buf[8192];
    size_t got;
    while ((got = fread(buf, 1, sizeof(buf), in)) > 0) fwrite(buf, 1, got, out);
    fclose(in);
    fclose(out);
    return true;
  }

  bool was_unordered = false;
  for (size_t i = 1; i < n; i++)
    if (cues[i].start < cues[i - 1].start) { was_unordered = true; break; }

  if (was_unordered) {                     /* stable insertion sort by start */
    for (size_t i = 1; i < n; i++) {
      SrtCue key = cues[i];
      size_t j = i;
      while (j > 0 && cues[j - 1].start > key.start) { cues[j] = cues[j - 1]; j--; }
      cues[j] = key;
    }
    logw("The subtitle file was not in chronological order - sorted %zu cues "
         "back into timeline order.", n);
  }

  /* Structure: word-by-word and half-sentence cues become sentences, so the
     planner gets a story and not a pile of pieces. */
  size_t before = n;
  srt_merge_fragments(cues, &n);
  srt_fix_windows(cues, n);
  if (before > n)
    logi("Structured the subtitles: %zu cues -> %zu sentence cues (%zu fragments joined).",
         before, n, before - n);

  FILE *out = plat_fopen(output_srt, "wb");
  if (!out) {
    for (size_t i = 0; i < n; i++) free(cues[i].text);
    free(cues);
    return false;
  }

  for (size_t i = 0; i < n; i++) {
    fprintf(out, "%zu\n%d --> %d\n%s\n\n", i + 1, cues[i].start, cues[i].end,
            cues[i].text ? cues[i].text : "");
    free(cues[i].text);
  }
  free(cues);
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

/* Declared here (defined further down): the Wikipedia fetch below scrubs the
 * text it gets from the article before handing it to the model. */
static char *sanitize_utf8_lossy(const char *in);
/* Defined with the offline fallback helpers below; the planner needs the
   same numbers the prompt uses for the per-clip length. */
static void clip_seconds_range(int per_clip_sec, int *min_sec, int *max_sec);
/* Speech-length helpers (defined with the offline planner); the prompt builder
 * needs them to talk about the right unit for the language. */
static bool   lang_counts_chars(const char *code);
static double lang_speech_units_per_sec(const char *code);

/* ---------------------------------------------------------------------------
 * Plot summary context (Wikipedia).
 *
 * Small and mid-size models mix up character names when they only have the
 * title and the subtitles to work from.  The plot section of the movie's
 * Wikipedia article is a reliable, citable text that spells the names
 * correctly, so it is handed to the model as the source of truth for names,
 * spelling and who does what.  Its absence is never fatal - the recap is still
 * built from the subtitles.
 * ------------------------------------------------------------------------ */

#define WIKI_PLOT_MAX_CHARS 14000
#define WIKI_PLOT_MIN_CHARS 400

/* Wikipedia subdomain for the narration language (the article is then written in
 * that language, so its names match what the narration says). */
static void wiki_lang_for(const char *recap_language, char *out, size_t outsz) {
  snprintf(out, outsz, "%s", lang_code_of(recap_language));
}

/* Cut the "Plot" (or "Synopsis") section out of a plain-text article extract.
 * Wikipedia's plain text marks headings as "== Name ==" on their own line, so
 * this walks the lines instead of matching one exact spelling. */
static char *wiki_extract_plot_section(const char *extract, size_t *out_len) {
  if (!extract || !extract[0]) return NULL;

  struct { const char *p; size_t n; } line;
  const char *start_body = NULL;
  const char *end_body = NULL;
  const char *p = extract;

  while (*p) {
    const char *nl = strchr(p, '\n');
    line.p = p;
    line.n = nl ? (size_t)(nl - p) : strlen(p);

    const char *q = line.p;
    const char *qe = line.p + line.n;
    while (q < qe && (*q == ' ' || *q == '\t')) q++;
    while (qe > q && (qe[-1] == ' ' || qe[-1] == '\t' || qe[-1] == '\r')) qe--;

    bool heading = (qe - q >= 4 && q[0] == '=' && q[1] == '=' && qe[-1] == '=' && qe[-2] == '=');
    if (heading) {
      /* normalize the heading text to compare it */
      const char *hs = q + 2, *he = qe - 2;
      while (hs < he && (*hs == ' ' || *hs == '_')) hs++;
      while (he > hs && (he[-1] == ' ' || he[-1] == '_')) he--;
      char name[64];
      size_t k = 0;
      for (const char *t = hs; t < he && k + 1 < sizeof(name); t++)
        name[k++] = (char)tolower((unsigned char)*t);
      name[k] = 0;

      if (start_body) { end_body = line.p; break; }   /* next heading ends it */
      if (strcmp(name, "plot") == 0 || strcmp(name, "plot summary") == 0 ||
          strcmp(name, "synopsis") == 0 || strcmp(name, "plot synopsis") == 0 ||
          strcmp(name, "story") == 0 || strcmp(name, "premise") == 0) {
        start_body = nl ? nl + 1 : NULL;
        if (!start_body) break;
      }
    }
    if (!nl) break;
    p = nl + 1;
  }

  char *copy;
  if (start_body) {
    size_t n = end_body ? (size_t)(end_body - start_body) : strlen(start_body);
    copy = (char *)malloc(n + 1);
    if (!copy) return NULL;
    memcpy(copy, start_body, n);
    copy[n] = 0;
  } else {
    copy = str_dup(extract);          /* no Plot heading: use the whole extract */
    if (!copy) return NULL;
  }

  /* trim leading/trailing whitespace */
  char *z = copy;
  while (*z == '\n' || *z == '\r' || *z == ' ' || *z == '\t') z++;
  size_t n = strlen(z);
  while (n > 0 && (z[n-1] == '\n' || z[n-1] == '\r' || z[n-1] == ' ' || z[n-1] == '\t')) z[--n] = 0;
  if (z != copy) memmove(copy, z, n + 1);

  if (out_len) *out_len = n;
  return copy;
}

/* Percent-encode for a query string (Wikipedia titles have spaces, quotes,
 * brackets and non-ASCII letters). */
static void wiki_encode(const char *in, char *out, size_t outsz) {
  size_t o = 0;
  for (size_t i = 0; in[i] && o + 4 < outsz; i++) {
    unsigned char c = (unsigned char)in[i];
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out[o++] = (char)c;
    } else if (c == ' ') {
      out[o++] = '+';
    } else {
      static const char hex[] = "0123456789ABCDEF";
      out[o++] = '%'; out[o++] = hex[c >> 4]; out[o++] = hex[c & 0x0F];
    }
  }
  out[o] = 0;
}

/* First page title returned by a Wikipedia search. */
static char *wiki_search_title(const char *base, const char *movie_title) {
  char enc[1024], query[512];
  snprintf(query, sizeof(query), "%s film", movie_title);
  wiki_encode(query, enc, sizeof(enc));

  char url[1600];
  snprintf(url, sizeof(url),
           "%s?action=query&list=search&srsearch=%s&srlimit=5&format=json", base, enc);

  long code = 0;
  MemBuf r = http_get_to_mem_ua(url, &code, WIKI_UA);
  if (code != 200 || !r.data) { if (r.data) free(r.data); return NULL; }

  char *title = NULL;
  cJSON *root = cJSON_Parse(r.data);
  if (root) {
    cJSON *q = cJSON_GetObjectItemCaseSensitive(root, "query");
    cJSON *list = q ? cJSON_GetObjectItemCaseSensitive(q, "search") : NULL;
    if (cJSON_IsArray(list)) {
      /* prefer an article that looks like the film (disambiguated or by year) */
      int best = -1;
      int n = cJSON_GetArraySize(list);
      for (int i = 0; i < n; i++) {
        cJSON *item = cJSON_GetArrayItem(list, (int)i);
        cJSON *t = item ? cJSON_GetObjectItemCaseSensitive(item, "title") : NULL;
        if (!cJSON_IsString(t) || !t->valuestring) continue;
        if (best < 0) best = i;
        if (strcasestr_local(t->valuestring, "(film") || strcasestr_local(t->valuestring, "film)")) {
          best = i;
          break;
        }
      }
      if (best >= 0) {
        cJSON *item = cJSON_GetArrayItem(list, best);
        cJSON *t = item ? cJSON_GetObjectItemCaseSensitive(item, "title") : NULL;
        if (cJSON_IsString(t) && t->valuestring) title = str_dup(t->valuestring);
      }
    }
    cJSON_Delete(root);
  }
  free(r.data);
  return title;
}

/* Plain-text article extract for a page title. */
static char *wiki_fetch_extract(const char *base, const char *page_title) {
  char enc[1024];
  wiki_encode(page_title, enc, sizeof(enc));

  char url[1600];
  snprintf(url, sizeof(url),
           "%s?action=query&prop=extracts&explaintext=1&redirects=1&format=json&titles=%s",
           base, enc);

  long code = 0;
  MemBuf r = http_get_to_mem_ua(url, &code, WIKI_UA);
  if (code != 200 || !r.data) { if (r.data) free(r.data); return NULL; }

  char *text = NULL;
  cJSON *root = cJSON_Parse(r.data);
  if (root) {
    cJSON *q = cJSON_GetObjectItemCaseSensitive(root, "query");
    cJSON *pages = q ? cJSON_GetObjectItemCaseSensitive(q, "pages") : NULL;
    if (cJSON_IsObject(pages)) {
      cJSON *page = NULL;
      cJSON_ArrayForEach(page, pages) {
        cJSON *ex = cJSON_GetObjectItemCaseSensitive(page, "extract");
        if (cJSON_IsString(ex) && ex->valuestring && ex->valuestring[0]) {
          text = str_dup(ex->valuestring);
          break;
        }
      }
    }
    cJSON_Delete(root);
  }
  free(r.data);
  return text;
}

/* The plot summary of the movie, cached in scripts/srt_files/<Title>_plot.txt.
 * Returns a malloc'd string or NULL (never fails the run). */
static char *wikipedia_plot_summary(const Config *cfg, const char *movie_title) {
  if (!cfg || !cfg->use_wikipedia_plot || !movie_title || !movie_title[0]) return NULL;

  ensure_dir("scripts");
  ensure_dir("scripts/srt_files");
  char cache[PATH_MAX];
  snprintf(cache, sizeof(cache), "scripts/srt_files/%s_plot.txt", movie_title);
  if (file_exists(cache)) {
    char *cached = read_entire_file(cache);
    if (cached && strlen(cached) >= WIKI_PLOT_MIN_CHARS) {
      logi("Plot summary: using the cached scripts/srt_files/%s_plot.txt (%zu chars)",
           movie_title, strlen(cached));
      return cached;
    }
    free(cached);
  }

  char base[320];
  if (cfg->wikipedia_base_url[0]) {
    snprintf(base, sizeof(base), "%s", cfg->wikipedia_base_url);
  } else {
    char wl[8];
    wiki_lang_for(cfg->recap_language, wl, sizeof(wl));
    snprintf(base, sizeof(base), "https://%s.wikipedia.org/w/api.php", wl);
  }

  char *title = wiki_search_title(base, movie_title);
  if (!title) {
    logw("Plot summary: Wikipedia has no article for \"%s\" - the recap will be built "
         "from the subtitles alone, so double-check character names in the output. "
         "(Add one by hand at scripts/srt_files/%s_plot.txt.)", movie_title, movie_title);
    return NULL;
  }

  char *extract = wiki_fetch_extract(base, title);
  if (!extract) {
    logw("Plot summary: could not read the article \"%s\" - continuing without it.", title);
    free(title);
    return NULL;
  }

  size_t plot_len = 0;
  char *plot = wiki_extract_plot_section(extract, &plot_len);
  free(extract);
  if (!plot || plot_len < WIKI_PLOT_MIN_CHARS) {
    logw("Plot summary: the article \"%s\" has no usable plot section - continuing without it.", title);
    free(plot);
    free(title);
    return NULL;
  }

  char *lossy = sanitize_utf8_lossy(plot);
  free(plot);
  plot = lossy ? lossy : NULL;
  if (!plot) { free(title); return NULL; }

  if (strlen(plot) > WIKI_PLOT_MAX_CHARS) plot[WIKI_PLOT_MAX_CHARS] = 0;

  if (!write_entire_file(cache, plot, strlen(plot)))
    logw("Plot summary: could not cache the summary at %s (continuing anyway).", cache);
  logi("Plot summary: %zu chars from Wikipedia (%s) will be handed to the model so "
       "character names come from a reliable text.", strlen(plot), title);
  free(title);
  return plot;
}

/* Is this two letter suffix a subtitle language tag rather than a marker such
 * as ".cc" or a piece of the title? */
static bool lang_tag_is_known(const char *tag) {
  for (int k = 0; k_langs[k].name; k++)
    if (k_langs[k].code[0] && k_langs[k].code[1] && !k_langs[k].code[2] &&
        !strcmp(k_langs[k].code, tag))
      return true;
  return false;
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

/* POST JSON with fully custom headers (Anthropic-compatible endpoints use
 * x-api-key + anthropic-version instead of a Bearer token). */
static MemBuf http_post_json_headers(const char *url, const char *const *headers_extra,
                                     const char *json_body, long *http_code_out,
                                     long timeout_s) {
  if (http_code_out) *http_code_out = -1;

  CURL *curl = curl_easy_init();
  if (!curl) {
    logw("curl init failed (out of memory?)");
    return (MemBuf){0};
  }

  MemBuf buf = (MemBuf){0};
  struct curl_slist *headers = NULL;
  headers = curl_slist_append(headers, "Content-Type: application/json");
  for (int i = 0; headers_extra && headers_extra[i]; i++)
    headers = curl_slist_append(headers, headers_extra[i]);

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
    /* Transport error (no answer at all): the caller decides what to do. */
    logw("HTTP POST failed for %s: %s", url, curl_easy_strerror(res));
    free(buf.data);
    return (MemBuf){0};
  }
  return buf;
}

/* ------------------------------------------- Anthropic Messages API (Claude) */

/* Anthropic-compatible base URLs come with or without the /v1 suffix:
 *   https://api.anthropic.com/v1  ->  https://api.anthropic.com/v1/messages
 *   https://api.deepseek.com/anthropic -> https://api.deepseek.com/anthropic/v1/messages */
static void anthropic_endpoint(const char *base, char *out, size_t outsz) {
  char tmp[512];
  snprintf(tmp, sizeof(tmp), "%s", base ? base : "");
  size_t bl = strlen(tmp);
  while (bl > 1 && tmp[bl - 1] == '/') tmp[--bl] = '\0';
  /* somebody pasted the full endpoint instead of the base URL */
  if (bl >= 9 && strcmp(tmp + bl - 9, "/messages") == 0) tmp[bl -= 9] = '\0';
  bl = strlen(tmp);
  bool has_v1 = bl >= 3 && strcmp(tmp + bl - 3, "/v1") == 0;
  snprintf(out, outsz, "%s%s/messages", tmp, has_v1 ? "" : "/v1");
}

/* The OpenAI-style base URL behind an Anthropic-style one.  A pasted endpoint
 * tail (/messages), a /v1 suffix and the gateway's /anthropic suffix are all
 * peeled off, so the /chat/completions retry lands on the right host:
 *   https://api.deepseek.com/anthropic           -> https://api.deepseek.com
 *   https://gw.example.com/anthropic/v1/messages -> https://gw.example.com
 */
static void anthropic_openai_base(const char *base, char *out, size_t outsz) {
  char tmp[512];
  snprintf(tmp, sizeof(tmp), "%s", base ? base : "");
  size_t bl = strlen(tmp);
  while (bl > 1 && tmp[bl - 1] == '/') tmp[--bl] = '\0';
  if (bl >= 9 && strcmp(tmp + bl - 9, "/messages") == 0) tmp[bl -= 9] = '\0';
  bl = strlen(tmp);
  if (bl >= 3 && strcmp(tmp + bl - 3, "/v1") == 0) tmp[bl -= 3] = '\0';
  bl = strlen(tmp);
  if (bl >= 10 && strcmp(tmp + bl - 10, "/anthropic") == 0) tmp[bl -= 10] = '\0';
  snprintf(out, outsz, "%s", tmp);
}

/* api.anthropic.com itself authenticates with x-api-key only and rejects an
 * unexpected Authorization header; third-party Anthropic-compatible gateways
 * (Azure AI Foundry, DeepSeek's /anthropic gateway, Ollama Cloud, ...) very
 * often want the key in a Bearer token instead.  OAuth tokens (sk-ant-oat...)
 * are always Bearer. */
static bool anthropic_host_is_native(const char *base) {
  return strcasestr_local(base, "api.anthropic.com") != NULL;
}

static MemBuf anthropic_post_messages(const Config *cfg, const char *sys_prompt,
                                      const char *prompt, int max_tokens,
                                      bool disable_thinking, bool stream,
                                      long *http_code, long timeout_s) {
  char endpoint[560];
  anthropic_endpoint(cfg->openai_base_url, endpoint, sizeof(endpoint));

  cJSON *req = cJSON_CreateObject();
  cJSON_AddStringToObject(req, "model", cfg->openai_model);
  /* A full clip plan is thousands of tokens; a small budget silently
     truncates the JSON and the run falls back to the offline planner. */
  cJSON_AddNumberToObject(req, "max_tokens", max_tokens);
  if (disable_thinking) {
    /* DeepSeek models default to thinking behind their gateway; the reasoning
       would eat the output budget and truncate the plan. */
    cJSON *th = cJSON_CreateObject();
    cJSON_AddStringToObject(th, "type", "disabled");
    cJSON_AddItemToObject(req, "thinking", th);
  }
  if (stream) {
    /* Anthropic answers a non-streaming request only while max_tokens stays
       under ~21,333 (a reply that may take longer than 10 minutes has to be
       streamed - see ANTHROPIC_STREAM_OVER); the reply then arrives as SSE and
       is folded back into one Messages object by anthropic_sse_fold. */
    cJSON_AddBoolToObject(req, "stream", 1);
  }
  cJSON_AddStringToObject(req, "system", sys_prompt);
  cJSON *msgs = cJSON_CreateArray();
  cJSON *u = cJSON_CreateObject();
  cJSON_AddStringToObject(u, "role", "user");
  cJSON_AddStringToObject(u, "content", prompt);
  cJSON_AddItemToArray(msgs, u);
  cJSON_AddItemToObject(req, "messages", msgs);
  char *body = cJSON_PrintUnformatted(req);
  cJSON_Delete(req);
  if (!body) { if (http_code) *http_code = -1; return (MemBuf){0}; }

  const char *key = cfg->openai_key;
  char keyhdr[1024], bearhdr[1024];
  snprintf(keyhdr, sizeof(keyhdr), "x-api-key: %s", key);
  snprintf(bearhdr, sizeof(bearhdr), "Authorization: Bearer %s", key);

  const char *hdrs[4];
  int nh = 0;
  hdrs[nh++] = "anthropic-version: 2023-06-01";
  if (strncmp(key, "sk-ant-oat", 10) == 0) {
    hdrs[nh++] = bearhdr;          /* OAuth token: Bearer only */
  } else {
    hdrs[nh++] = keyhdr;
    if (!anthropic_host_is_native(cfg->openai_base_url)) hdrs[nh++] = bearhdr;
  }
  hdrs[nh] = NULL;

  logi("Anthropic-compatible endpoint: %s (model=%s, max_tokens=%d%s%s)", endpoint,
       cfg->openai_model, max_tokens, disable_thinking ? ", thinking disabled" : "",
       stream ? ", streamed" : "");
  MemBuf r = http_post_json_headers(endpoint, hdrs, body, http_code, timeout_s);
  free(body);
  return r;
}

/* A streamed Messages reply is a server-sent event stream.  Fold it back into
 * one object shaped like a non-streaming reply:
 *   {"content":[{"type":"text","text":"..."}], "stop_reason":"end_turn"}
 * so every reader of the response keeps working unchanged.  Returns NULL when
 * the body is not an event stream (a plain JSON reply, an HTML error page). */
static char *anthropic_sse_fold(const char *body) {
  if (!body) return NULL;
  if (strncmp(body, "event:", 6) != 0 && strstr(body, "\nevent:") == NULL)
    return NULL;

  char *text = NULL;
  size_t tlen = 0, tcap = 0;
  char stop[64];
  stop[0] = '\0';
  char *err_json = NULL;

  const char *p = body;
  while (p && *p) {
    const char *nl = strchr(p, '\n');
    const char *line = p;
    size_t ll = nl ? (size_t)(nl - p) : strlen(p);

    if (ll > 5 && strncmp(line, "data:", 5) == 0) {
      const char *j = line + 5;
      while (*j == ' ' || *j == '\t') j++;
      size_t jlen = ll - (size_t)(j - line);
      if (strncmp(j, "[DONE]", 6) != 0) {
        cJSON *ev = cJSON_ParseWithLength(j, jlen);
        if (ev) {
          const cJSON *type = cJSON_GetObjectItemCaseSensitive(ev, "type");
          const char *t = cJSON_IsString(type) ? type->valuestring : "";
          if (strcmp(t, "content_block_delta") == 0) {
            const cJSON *d = cJSON_GetObjectItemCaseSensitive(ev, "delta");
            const cJSON *dt = d ? cJSON_GetObjectItemCaseSensitive(d, "text") : NULL;
            if (cJSON_IsString(dt) && dt->valuestring) {
              size_t add = strlen(dt->valuestring);
              if (tlen + add + 1 > tcap) {
                tcap = (tlen + add + 1) * 2;
                text = (char *)realloc(text, tcap);
                if (!text) die("OOM");
              }
              memcpy(text + tlen, dt->valuestring, add);
              tlen += add;
              text[tlen] = '\0';
            }
          } else if (strcmp(t, "message_delta") == 0) {
            const cJSON *d = cJSON_GetObjectItemCaseSensitive(ev, "delta");
            const cJSON *sr = d ? cJSON_GetObjectItemCaseSensitive(d, "stop_reason") : NULL;
            if (cJSON_IsString(sr) && sr->valuestring)
              snprintf(stop, sizeof(stop), "%s", sr->valuestring);
          } else if (strcmp(t, "error") == 0 || strcmp(t, "message_stop") == 0) {
            if (strcmp(t, "error") == 0) {
              free(err_json);
              err_json = cJSON_PrintUnformatted(ev);
            }
          }
          cJSON_Delete(ev);
        }
      }
    }
    p = nl ? nl + 1 : NULL;
  }

  if (err_json) {
    free(text);
    return err_json;
  }
  cJSON *out = cJSON_CreateObject();
  cJSON *content = cJSON_AddArrayToObject(out, "content");
  cJSON *item = cJSON_CreateObject();
  cJSON_AddStringToObject(item, "type", "text");
  cJSON_AddStringToObject(item, "text", text ? text : "");
  cJSON_AddItemToArray(content, item);
  if (stop[0]) cJSON_AddStringToObject(out, "stop_reason", stop);
  char *folded = cJSON_PrintUnformatted(out);
  cJSON_Delete(out);
  free(text);
  return folded;
}

static void anthropic_stop_reason(const char *body, char *out, size_t outsz) {
  out[0] = '\0';
  if (!body) return;
  cJSON *root = cJSON_Parse(body);
  if (!root) return;
  const cJSON *sr = cJSON_GetObjectItemCaseSensitive(root, "stop_reason");
  if (cJSON_IsString(sr) && sr->valuestring)
    snprintf(out, outsz, "%s", sr->valuestring);
  cJSON_Delete(root);
}

/* The provider tells us the real output limit in the error message, e.g.
 *   "max_tokens: 32000 > 8192, which is the maximum allowed number of output
 *    tokens for claude-3-5-sonnet-20241022"
 * Return the largest plausible limit that is smaller than `current`. */
static long anthropic_limit_from_error(const char *resp_json, long current) {
  const char *msg = resp_json ? resp_json : "";
  cJSON *root = cJSON_Parse(resp_json ? resp_json : "");
  if (root) {
    cJSON *err = cJSON_GetObjectItemCaseSensitive(root, "error");
    if (cJSON_IsObject(err)) {
      cJSON *m = cJSON_GetObjectItemCaseSensitive(err, "message");
      if (cJSON_IsString(m) && m->valuestring) msg = m->valuestring;
    }
  }

  long best = -1;
  for (const char *p = msg; *p; ) {
    if (isdigit((unsigned char)*p)) {
      long v = strtol(p, (char **)&p, 10);
      if (v >= 512 && v < current && v > best) best = v;
    } else {
      p++;
    }
  }
  if (root) cJSON_Delete(root);
  return best;
}

static bool anthropic_error_mentions(const char *resp_json, const char *needle) {
  return resp_json && strcasestr_local(resp_json, needle) != NULL;
}

/* An HTTP 200 can still be a cut-off answer: the provider stopped at its output
 * limit in the middle of the clips array.  Anthropic-compatible replies say so
 * in stop_reason, OpenAI-style replies in finish_reason / status. */
static bool openai_body_hit_output_limit(const char *body) {
  if (!body) return false;
  cJSON *root = cJSON_Parse(body);
  if (!root) return false;

  bool hit = false;
  const cJSON *stop = cJSON_GetObjectItemCaseSensitive(root, "stop_reason");
  if (cJSON_IsString(stop) && stop->valuestring &&
      strcmp(stop->valuestring, "max_tokens") == 0)
    hit = true;                                   /* Anthropic Messages API */
  const cJSON *status = cJSON_GetObjectItemCaseSensitive(root, "status");
  if (cJSON_IsString(status) && status->valuestring &&
      strcmp(status->valuestring, "incomplete") == 0)
    hit = true;                                   /* Responses API */
  const cJSON *choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
  if (cJSON_IsArray(choices)) {
    const cJSON *first = cJSON_GetArrayItem(choices, 0);
    const cJSON *fr = first ? cJSON_GetObjectItemCaseSensitive(first, "finish_reason") : NULL;
    if (cJSON_IsString(fr) && fr->valuestring && strcmp(fr->valuestring, "length") == 0)
      hit = true;                                 /* chat/completions */
  }
  cJSON_Delete(root);
  return hit;
}

/* Say out loud when a 200 reply stopped at the provider's output limit: the
 * plan that comes out is then only as long as the provider let it be. */
static void warn_if_plan_was_cut_short(const char *resp_body, size_t clips) {
  if (clips == 0 || !resp_body) return;
  if (!openai_body_hit_output_limit(resp_body)) return;
  logw("The reply was cut off by the provider's output limit and only %zu clip%s "
       "arrived - raise the model's output/token limit (or lower the clip count) "
       "for a full-length recap.", clips, clips == 1 ? "" : "s");
}

/* POST the plan to an Anthropic-compatible /v1/messages endpoint, adapting the
 * request when the provider rejects it:
 *   - "max_tokens too large"  -> retry with the limit named in the error
 *   - "thinking ... not supported" -> retry without the thinking field
 * The Messages API has no JSON mode, and questions of taste differ between
 * Claude, DeepSeek's gateway and Azure, so this keeps the run alive instead of
 * silently dropping to the raw-subtitle fallback planner. */
#define ANTHROPIC_STREAM_OVER   21333  /* non-streaming max_tokens ceiling  */
#define ANTHROPIC_NONSTREAM_MAX 16000  /* what we ask for when not streaming */

static MemBuf anthropic_plan_request(const Config *cfg, const char *sys_prompt,
                                     const char *prompt, long *http_code,
                                     long timeout_s) {
  char mdl_lc[160];
  {
    size_t i = 0;
    for (; cfg->openai_model[i] && i + 1 < sizeof(mdl_lc); i++)
      mdl_lc[i] = (char)tolower((unsigned char)cfg->openai_model[i]);
    mdl_lc[i] = '\0';
  }
  /* DeepSeek models (whatever name the gateway maps them from) think by
     default on the Anthropic endpoint, which truncates the JSON. */
  bool deepseek = strstr(mdl_lc, "deepseek") != NULL ||
                  strcasestr_local(cfg->openai_base_url, "deepseek") != NULL;
  bool disable_thinking = deepseek;
  int  budget = 32000;
  int  raised = 0;              /* times we asked for more room */
  bool lowered = false;         /* provider told us its ceiling */
  /* A full clip plan in one non-streaming reply is exactly what Anthropic
     refuses above ~21,333 max_tokens, so a big budget goes out streamed (the
     SSE reply is folded back into one JSON object). */
  bool stream = budget > ANTHROPIC_STREAM_OVER;
  bool tried_stream = stream, tried_nonstream = !stream;
  int  waited = 0;              /* transient-error retries used */

  /* Say the obvious things before spending a request: with a key that is not
     an Anthropic key, or a model that is not a Claude model, the native
     endpoint can only ever answer 401/404. */
  if (anthropic_host_is_native(cfg->openai_base_url)) {
    if (strncmp(mdl_lc, "claude", 6) != 0)
      logw("The model \"%s\" does not look like a Claude model, but the base URL "
           "points at api.anthropic.com - that host serves claude-... models only.",
           cfg->openai_model);
    if (strncmp(cfg->openai_key, "sk-ant-", 7) != 0)
      logw("The API key does not look like an Anthropic key (it should start with "
           "sk-ant-api... or sk-ant-oat... for an OAuth token), but the base URL "
           "points at api.anthropic.com.");
  }

  time_t t0 = time(NULL);
  MemBuf resp = {0};
  for (int attempt = 0; attempt < 6; attempt++) {
    resp = anthropic_post_messages(cfg, sys_prompt, prompt, budget, disable_thinking,
                                   stream, http_code, timeout_s);
    long code = http_code ? *http_code : 0;

    if (resp.data && code >= 200 && code < 300) {
      char *folded = anthropic_sse_fold(resp.data);
      if (folded) {
        free(resp.data);
        resp.data = folded;
        resp.size = strlen(folded);
        if (strstr(folded, "\"error\"") && !strstr(folded, "\"content\"")) {
          code = 400;                 /* an error event inside the stream */
          if (http_code) *http_code = 400;
          logw("The streamed reply carried an error: %.300s", folded);
        }
      }
    }

    if (code >= 200 && code < 300) {
      char stop[64];
      anthropic_stop_reason(resp.data, stop, sizeof(stop));
      if (strcmp(stop, "refusal") == 0)
        logw("The model refused to write this plan (stop_reason=refusal) - a different "
             "model may handle this movie.");
      /* Cut off mid-array?  Ask for more room, unless the provider already
         named its ceiling (then the plan is simply as long as it can be). */
      if (strcmp(stop, "max_tokens") == 0 && !lowered && raised < 2 &&
          budget < 160000) {
        int next = budget * 2;
        if (next > 160000) next = 160000;
        logw("The reply was cut off in the middle of the plan (stop_reason=max_tokens) - "
             "retrying with max_tokens=%d so all clips arrive.", next);
        budget = next;
        stream = true;                /* a budget this size has to stream */
        tried_stream = true;
        raised++;
        if (resp.data) free(resp.data);
        resp.data = NULL;
        resp.size = 0;
        continue;
      }
      logi("Anthropic endpoint answered HTTP %ld in %ld s (%zu bytes, stop_reason=%s)",
           code, (long)(time(NULL) - t0), resp.size,
           stop[0] ? stop : "none");
      return resp;
    }

    bool retried = false;
    if (anthropic_error_mentions(resp.data, "max_tokens") ||
        anthropic_error_mentions(resp.data, "output tokens")) {
      long limit = anthropic_limit_from_error(resp.data, budget);
      int next = (limit > 0 && limit < budget) ? (int)limit : budget / 4;
      if (next < 512) next = 512;
      if (!stream && next > ANTHROPIC_NONSTREAM_MAX) next = ANTHROPIC_NONSTREAM_MAX;
      if (next != budget) {
        logw("Anthropic endpoint rejected max_tokens=%d (HTTP %ld) - retrying with %d.",
             budget, code, next);
        budget = next;
        lowered = true;
        retried = true;
      }
    } else if (disable_thinking &&
               (anthropic_error_mentions(resp.data, "thinking") ||
                anthropic_error_mentions(resp.data, "output_config"))) {
      logw("Anthropic endpoint rejects the thinking field (HTTP %ld) - retrying without it.", code);
      disable_thinking = false;
      retried = true;
    }

    /* Transient: rate limit, overloaded (529), gateway hiccup.  Anthropic asks
       clients to retry these instead of failing the run. */
    if (!retried &&
        (code == 429 || code == 500 || code == 502 || code == 503 || code == 504 ||
         code == 529 || (code == 400 && anthropic_error_mentions(resp.data, "overloaded")))) {
      if (waited < 2) {
        int delay_ms = waited == 0 ? 2000 : 5000;
        waited++;
        logw("The Anthropic endpoint is overloaded or rate limited (HTTP %ld) - retrying "
             "in %d s.", code, delay_ms / 1000);
#ifndef MOVIECAP_UNIT_TEST
        plat_sleep_ms(delay_ms);
#endif
        retried = true;
      }
    }

    /* "Streaming is required for operations that may take longer than 10
       minutes": the reply has to be streamed at this budget. */
    if (!retried && !stream && !tried_stream &&
        (anthropic_error_mentions(resp.data, "streaming") ||
         anthropic_error_mentions(resp.data, "stream is required"))) {
      logw("The endpoint requires a streamed reply for this output budget - retrying "
           "with streaming.");
      stream = true;
      tried_stream = true;
      retried = true;
    }

    /* A gateway that cannot stream answers a streamed request with 400/422:
       go back to a plain request with a budget that fits one. */
    if (!retried && stream && !tried_nonstream && (code == 400 || code == 422)) {
      logw("The endpoint rejected the streamed request (HTTP %ld) - retrying without "
           "streaming (max_tokens=%d).", code,
           budget > ANTHROPIC_NONSTREAM_MAX ? ANTHROPIC_NONSTREAM_MAX : budget);
      stream = false;
      tried_nonstream = true;
      if (budget > ANTHROPIC_NONSTREAM_MAX) budget = ANTHROPIC_NONSTREAM_MAX;
      retried = true;
    }

    if (!retried) return resp;
    if (resp.data) free(resp.data);
    resp.data = NULL;
    resp.size = 0;
  }
  return resp;
}

/* Extract content[0].text from an Anthropic Messages API reply. */
static char *anthropic_extract_text(const cJSON *root) {
  const cJSON *content = cJSON_GetObjectItemCaseSensitive(root, "content");
  if (!cJSON_IsArray(content)) return NULL;
  const cJSON *item = NULL;
  cJSON_ArrayForEach(item, content) {
    if (!cJSON_IsObject(item)) continue;
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(item, "type");
    const cJSON *text = cJSON_GetObjectItemCaseSensitive(item, "text");
    if (cJSON_IsString(type) && type->valuestring &&
        strcmp(type->valuestring, "text") == 0 &&
        cJSON_IsString(text) && text->valuestring)
      return str_dup(text->valuestring);
  }
  return NULL;
}

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
    /* Classic /chat/completions shape (DeepSeek, Groq, Mistral, ...):
     * choices[0].message.content */
    cJSON *choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
    if (cJSON_IsArray(choices)) {
      cJSON *first = cJSON_GetArrayItem(choices, 0);
      cJSON *message = first ? cJSON_GetObjectItemCaseSensitive(first, "message") : NULL;
      cJSON *content = message ? cJSON_GetObjectItemCaseSensitive(message, "content") : NULL;
      if (cJSON_IsString(content) && content->valuestring) {
        char *out = str_dup(content->valuestring);
        cJSON_Delete(root);
        return out;
      }
    }
    /* Anthropic Messages shape: {"content":[{"type":"text","text":"..."}]} */
    char *anth = anthropic_extract_text(root);
    cJSON_Delete(root);
    return anth;
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

/* Models often wrap the JSON in ```json fences or a chatty preamble - pull
 * out the outermost {...} block before parsing so extra text never throws a
 * whole plan away (that silently dropped runs onto the offline planner). */
static char *json_extract_object(const char *in) {
  const char *a = strchr(in, '{');
  if (!a) return NULL;
  const char *b = strrchr(in, '}');
  if (!b || b <= a) return NULL;
  size_t n = (size_t)(b - a) + 1;
  char *out = (char *)malloc(n + 1);
  if (!out) return NULL;
  memcpy(out, a, n);
  out[n] = '\0';
  return out;
}

/* Models often run out of output tokens in the middle of the clips array
 * (thinking budgets, small max_tokens, a chatty preamble).  Instead of throwing
 * the whole answer away - which silently drops the run onto the offline
 * raw-subtitle planner - rebuild a plan from the clip objects that were
 * written completely.  Returns NULL when there is nothing to salvage. */
static char *json_repair_truncated_clips(const char *in) {
  if (!in || !in[0]) return NULL;

  const char *key = strstr(in, "\"clips\"");
  if (!key) return NULL;
  const char *arr = strchr(key, '[');
  if (!arr) return NULL;

  bool in_str = false, esc = false;
  int depth = 0;
  const char *last_obj = NULL;      /* end of the last complete clip object */
  for (const char *q = arr + 1; *q; q++) {
    char c = *q;
    if (in_str) {
      if (esc) esc = false;
      else if (c == '\\') esc = true;
      else if (c == '"') in_str = false;
      continue;
    }
    if (c == '"') { in_str = true; continue; }
    if (c == '{') depth++;
    else if (c == '}') {
      if (depth == 1) last_obj = q;
      if (depth > 0) depth--;
    }
  }
  if (!last_obj) return NULL;

  size_t head = strlen("{\"clips\":[");
  size_t body = (size_t)(last_obj - (arr + 1)) + 1;
  char *out = (char *)malloc(head + body + 3);
  if (!out) return NULL;
  memcpy(out, "{\"clips\":[", head);
  memcpy(out + head, arr + 1, body);
  memcpy(out + head + body, "]}", 3);
  return out;
}

/* The prompt demands clips in increasing order of start time, but models do
 * not always obey - and an out-of-order plan plays the movie out of order in
 * the finished video (the narration and the burnt-in captions with it).  Sort
 * the plan back into story order, drop unusable ranges and take out overlaps
 * so the recap always runs forwards. */
static void plan_normalize(ClipPlanList *lst) {
  if (!lst || lst->count == 0) return;

  bool was_unordered = false;
  for (size_t i = 1; i < lst->count; i++)
    if (lst->items[i].start < lst->items[i - 1].start) { was_unordered = true; break; }

  for (size_t i = 1; i < lst->count; i++) {   /* stable insertion sort */
    ClipPlan key = lst->items[i];
    size_t j = i;
    while (j > 0 && lst->items[j - 1].start > key.start) {
      lst->items[j] = lst->items[j - 1];
      j--;
    }
    lst->items[j] = key;
  }

  size_t keep = 0;
  int prev_end = 0;
  for (size_t i = 0; i < lst->count; i++) {
    ClipPlan it = lst->items[i];
    if (it.start <= 0 || it.end <= it.start || !it.narration || !it.narration[0]) {
      free(it.narration);
      continue;
    }
    if (keep > 0 && it.start < prev_end) {
      if (prev_end >= it.end - 3) { free(it.narration); continue; }  /* mostly a repeat */
      it.start = prev_end;
    }
    lst->items[keep++] = it;
    prev_end = it.end;
  }

  size_t dropped = lst->count - keep;
  lst->count = keep;

  if (was_unordered)
    logw("The AI returned the clips out of chronological order - sorted them "
         "back into story order.");
  if (dropped)
    logw("Dropped %zu unusable or overlapping clip(s) from the plan.", dropped);
}

/* A number, or a string that holds one ("120", "120.5", " 120 s"). */
static bool json_int_value(const cJSON *v, int *out) {
  if (cJSON_IsNumber(v)) { *out = (int)((double)v->valuedouble + (v->valuedouble < 0 ? -0.5 : 0.5)); return true; }
  if (cJSON_IsString(v) && v->valuestring) {
    const char *p = v->valuestring;
    while (*p == ' ' || *p == '\t') p++;
    if (!(*p == '-' || (*p >= '0' && *p <= '9'))) return false;
    char *endp = NULL;
    double d = strtod(p, &endp);
    if (endp == p) return false;
    *out = (int)(d + (d < 0 ? -0.5 : 0.5));
    return true;
  }
  return false;
}

/* The first of the given keys that holds a non-empty string. */
static cJSON *json_first_string(cJSON *obj, ...) {
  va_list ap;
  va_start(ap, obj);
  const char *key;
  cJSON *found = NULL;
  while ((key = va_arg(ap, const char *)) != NULL) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(v) && v->valuestring && v->valuestring[0]) { found = v; break; }
  }
  va_end(ap);
  return found;
}

static ClipPlanList parse_clip_plan_json(const char *json_text) {
  ClipPlanList out = {0};
  cJSON *root = cJSON_Parse(json_text);
  if (!root) {
    char *bare = json_extract_object(json_text);
    if (bare) {
      root = cJSON_Parse(bare);
      free(bare);
      if (root) logw("Model wrapped the JSON in extra text - recovered the clip plan anyway.");
    }
  }
  if (!root) {
    /* Still broken: most likely the output hit the token limit mid-array. */
    char *repaired = json_repair_truncated_clips(json_text);
    if (repaired) {
      root = cJSON_Parse(repaired);
      free(repaired);
      if (root)
        logw("The reply was cut off mid-JSON (output limit) - kept the clip "
             "ranges that were complete.");
    }
  }
  if (!root) return out;

  /* Accept the array under "clips" - or the bare array, which some models
     return when they drop the wrapper. */
  cJSON *clips = cJSON_GetObjectItemCaseSensitive(root, "clips");
  if (!cJSON_IsArray(clips) && cJSON_IsArray(root)) clips = root;
  if (!cJSON_IsArray(clips)) {
    cJSON_Delete(root);
    return out;
  }

  size_t n = (size_t)cJSON_GetArraySize(clips);
  if (n == 0) { cJSON_Delete(root); return out; }

  out.items = (ClipPlan *)calloc(n, sizeof(ClipPlan));
  if (!out.items) die("OOM");
  out.count = 0;
  size_t skipped = 0;

  for (size_t i = 0; i < n; i++) {
    cJSON *obj = cJSON_GetArrayItem(clips, (int)i);
    if (cJSON_IsArray(obj) && cJSON_GetArraySize(obj) >= 3) {
      /* ["start", "end", "narration"] - a shape some models fall back to. */
      cJSON *s3 = cJSON_GetArrayItem(obj, 0);
      cJSON *e3 = cJSON_GetArrayItem(obj, 1);
      cJSON *t3 = cJSON_GetArrayItem(obj, 2);
      int st3 = 0, en3 = 0;
      if (json_int_value(s3, &st3) && json_int_value(e3, &en3) &&
          cJSON_IsString(t3) && t3->valuestring && t3->valuestring[0]) {
        out.items[out.count].start = st3;
        out.items[out.count].end = en3;
        out.items[out.count].narration = str_dup(t3->valuestring);
        out.count++;
      } else {
        skipped++;
      }
      continue;
    }
    if (!cJSON_IsObject(obj)) { skipped++; continue; }

    cJSON *s = cJSON_GetObjectItemCaseSensitive(obj, "start");
    cJSON *e = cJSON_GetObjectItemCaseSensitive(obj, "end");
    cJSON *nar = json_first_string(obj, "narration", "text", "narration_text", "voiceover", NULL);

    /* Numbers are what the prompt asks for, but models do send "start": "120"
       (or 120.5, or with a stray space) - that must not throw the whole plan
       away, because an empty plan is what drops a run onto the offline
       subtitle planner. */
    int st = 0, en = 0;
    if (!json_int_value(s, &st) || !json_int_value(e, &en) ||
        !nar || !nar->valuestring || !nar->valuestring[0]) {
      skipped++;
      continue;
    }

    out.items[out.count].start = st;
    out.items[out.count].end = en;
    out.items[out.count].narration = str_dup(nar->valuestring);
    out.count++;
  }

  if (skipped)
    logw("%zu clip object(s) in the reply had an unusable shape and were dropped "
         "(expected {\"start\":<number>,\"end\":<number>,\"narration\":\"...\"}).", skipped);
  if (clips == root)
    logw("The model returned the bare clips array instead of {\"clips\":[...]} - accepted it anyway.");

  cJSON_Delete(root);
  plan_normalize(&out);
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

/* Languages with a writing system of their own can be recognised from the code
 * points alone; Latin-script languages are checked with function words.
 *
 * A wrong-language plan used to ship silently - which is how an English recap
 * could come out of a Chinese run.  A false positive costs ONE retry with an
 * explicit demand, never a failed run. */
static bool lang_script_hit(const char *code, unsigned v) {
  if (!strcmp(code, "zh"))
    return (v >= 0x4E00 && v <= 0x9FFF) || (v >= 0x3400 && v <= 0x4DBF) ||
           (v >= 0xF900 && v <= 0xFAFF);
  if (!strcmp(code, "ja"))
    return (v >= 0x3040 && v <= 0x30FF) || (v >= 0x31F0 && v <= 0x31FF) ||
           (v >= 0x4E00 && v <= 0x9FFF);
  if (!strcmp(code, "ko"))
    return (v >= 0xAC00 && v <= 0xD7AF) || (v >= 0x1100 && v <= 0x11FF) ||
           (v >= 0x3130 && v <= 0x318F);
  if (!strcmp(code, "ar") || !strcmp(code, "fa") || !strcmp(code, "ur"))
    return (v >= 0x0600 && v <= 0x06FF) || (v >= 0x0750 && v <= 0x077F) ||
           (v >= 0xFB50 && v <= 0xFDFF) || (v >= 0xFE70 && v <= 0xFEFF);
  if (!strcmp(code, "he")) return (v >= 0x0590 && v <= 0x05FF);
  if (!strcmp(code, "ru") || !strcmp(code, "uk") || !strcmp(code, "bg") ||
      !strcmp(code, "sr"))  return (v >= 0x0400 && v <= 0x04FF);
  if (!strcmp(code, "el"))  return (v >= 0x0370 && v <= 0x03FF);
  if (!strcmp(code, "hi") || !strcmp(code, "mr") || !strcmp(code, "ne"))
    return (v >= 0x0900 && v <= 0x097F);
  if (!strcmp(code, "bn"))  return (v >= 0x0980 && v <= 0x09FF);
  if (!strcmp(code, "ta"))  return (v >= 0x0B80 && v <= 0x0BFF);
  if (!strcmp(code, "th"))  return (v >= 0x0E00 && v <= 0x0E7F);
  return false;
}

static bool lang_has_own_script(const char *code) {
  static const char *codes[] = { "zh", "ja", "ko", "ar", "fa", "ur", "he",
                                 "ru", "uk", "bg", "sr", "el", "hi", "mr",
                                 "ne", "bn", "ta", "th", NULL };
  for (int k = 0; codes[k]; k++) if (!strcmp(codes[k], code)) return true;
  return false;
}

/* The function words of the target language versus English.  Any narration in
 * the target language cannot avoid them; text written in English collects
 * " the / and / of / to / is" instead. */
static bool looks_english_not_latin_lang(const ClipPlan *items, size_t n, const char *code) {
  static const char *es[] = { " el ", " la ", " de ", " que ", " y ", " los ",
                              " una ", " con ", " para ", " su ", NULL };
  static const char *fr[] = { " le ", " la ", " de ", " et ", " les ", " un ",
                              " une ", " des ", " que ", " qui ", " dans ", NULL };
  static const char *de[] = { " der ", " die ", " das ", " und ", " ein ",
                              " eine ", " mit ", " auf ", " ist ", " zu ",
                              " sich ", NULL };
  static const char *it[] = { " il ", " la ", " di ", " che ", " e ", " un ",
                              " una ", " per ", " con ", " non ", " sono ", NULL };
  static const char *pt[] = { " o ", " a ", " de ", " que ", " e ", " um ",
                              " uma ", " com ", " para ", " seu ", NULL };
  const char **target = NULL;
  if (!strcmp(code, "es")) target = es;
  else if (!strcmp(code, "fr")) target = fr;
  else if (!strcmp(code, "de")) target = de;
  else if (!strcmp(code, "it")) target = it;
  else if (!strcmp(code, "pt")) target = pt;
  if (!target) return false;

  static const char *en[] = { " the ", " and ", " of ", " to ", " is ", " that ",
                              " he ", " she ", " they ", " with ", NULL };
  size_t hit_en = 0, hit_other = 0, chars = 0;
  for (size_t i = 0; i < n; i++) {
    const char *p = items[i].narration;
    if (!p) continue;
    chars += strlen(p);
    for (int k = 0; en[k]; k++) {
      const char *q = p;
      while ((q = strcasestr_local(q, en[k])) != NULL) { hit_en++; q += strlen(en[k]); }
    }
    for (int k = 0; target[k]; k++) {
      const char *q = p;
      while ((q = strcasestr_local(q, target[k])) != NULL) { hit_other++; q += strlen(target[k]); }
    }
    /* accents the language cannot avoid either */
    if (!strcmp(code, "es") && (strcasestr_local(p, "\xC3\xA1") ||
                                strcasestr_local(p, "\xC3\xB1") ||
                                strcasestr_local(p, "\xC2\xBF") ||
                                strcasestr_local(p, "\xC2\xA1"))) hit_other += 2;
    if (!strcmp(code, "fr") && (strcasestr_local(p, "\xC3\xA9") ||
                                strcasestr_local(p, "\xC3\xA8") ||
                                strcasestr_local(p, "\xC3\xA7") ||
                                strcasestr_local(p, "\xC3\xA0"))) hit_other += 2;
    if (!strcmp(code, "de") && (strcasestr_local(p, "\xC3\xBC") ||
                                strcasestr_local(p, "\xC3\xB6") ||
                                strcasestr_local(p, "\xC3\xA4") ||
                                strcasestr_local(p, "\xC3\x9F"))) hit_other += 2;
    if (!strcmp(code, "it") && (strcasestr_local(p, "\xC3\xA0") ||
                                strcasestr_local(p, "\xC3\xB2") ||
                                strcasestr_local(p, "\xC3\xAC"))) hit_other += 2;
    if (!strcmp(code, "pt") && (strcasestr_local(p, "\xC3\xA3") ||
                                strcasestr_local(p, "\xC3\xB5") ||
                                strcasestr_local(p, "\xC3\xA7"))) hit_other += 2;
  }
  if (chars < 80) return false;
  return hit_en >= 8 && hit_en > hit_other * 3;
}

/* True when the recap had to be in `code` but the plan's narrations are mostly
 * written in something else - i.e. the model ignored the language rule. */
static bool plan_language_mismatch(const ClipPlan *items, size_t n, const char *code) {
  if (!code || !strcmp(code, "en")) return false;
  if (!lang_has_own_script(code)) return looks_english_not_latin_lang(items, n, code);

  size_t total = 0, hit = 0;
  for (size_t i = 0; i < n; i++) {
    const unsigned char *p = (const unsigned char *)items[i].narration;
    if (!p) continue;
    while (*p) {
      unsigned v; size_t l;
      if (*p < 0x80)                 { v = *p; l = 1; if (v != ' ') total++; }
      else if ((*p & 0xE0) == 0xC0)  { v = ((unsigned)(*p & 0x1F) << 6) | (p[1] & 0x3F); l = 2; total++; }
      else if ((*p & 0xF0) == 0xE0)  { v = ((unsigned)(*p & 0x0F) << 12) | ((unsigned)(p[1] & 0x3F) << 6) | (p[2] & 0x3F); l = 3; total++; }
      else if ((*p & 0xF8) == 0xF0)  { v = 0x10000u; l = 4; total++; }
      else                            { l = 1; continue; }
      if (l > 1 && lang_script_hit(code, v)) hit++;
      p += l;
    }
  }
  if (total < 40) return false;
  return (double)hit / (double)total < 0.30;
}

/* Character names are the hardest part of the recap prompt (STEP 1): small or
 * cheap models mix first names, surnames and ranks, or name people who are not
 * in the clip at all.  Warn once per movie instead of shipping a confusing
 * recap. */
static void warn_if_model_is_small(const Config *cfg) {
  char mdl[160];
  to_lower_copy(cfg->openai_model, mdl, sizeof(mdl));

  static const char *weak[] = { "mini", "nano", "tiny", "small", "haiku",
                                "3b", "7b", "8b", NULL };
  for (int i = 0; weak[i]; i++) {
    if (strstr(mdl, weak[i])) {
      logw("Model \"%s\" is a small/fast model - character names, the timeline "
           "and the strict JSON often come out wrong with the recap prompt. "
           "A strong model (for example gpt-5.2) gives much better names.",
           cfg->openai_model);
      return;
    }
  }
}

/* POST an OpenAI-style chat body that carries an output-token limit; retry the
 * plain body once when the provider rejects the parameter name. */
static MemBuf openai_post_chat_tokens(const char *endpoint, const char *key,
                                      const char *chat_body_limited,
                                      const char *chat_body_plain,
                                      long *http_code, long timeout_s) {
  MemBuf r = http_post_json_to_mem(endpoint, key,
                                   chat_body_limited ? chat_body_limited : "{}",
                                   http_code, timeout_s);
  long code = http_code ? *http_code : 0;
  if (code == 400 && r.data && chat_body_plain &&
      (strcasestr_local(r.data, "max_completion_tokens") ||
       strcasestr_local(r.data, "unsupported parameter") ||
       strcasestr_local(r.data, "unrecognized") ||
       strcasestr_local(r.data, "unknown parameter"))) {
    logw("The provider rejected max_completion_tokens - retrying the chat endpoint "
         "without an output-token limit.");
    free(r.data);
    r.data = NULL;
    r.size = 0;
    r = http_post_json_to_mem(endpoint, key, chat_body_plain, http_code, timeout_s);
  }
  return r;
}

static ClipPlanList openai_make_plan(const Config *cfg,
                                     const char *movie_title,
                                     const char *subs_seconds_text,
                                     const char *optional_script_text,
                                     const char *plot_summary,
                                     bool subs_placeholder,
                                     int num_clips,
                                     int per_clip_sec,
                                     bool *out_retry_without_script,
                                     bool *out_retry_json_only,
                                     const char *correction_note) {
  if (out_retry_without_script) *out_retry_without_script = false;
  if (out_retry_json_only) *out_retry_json_only = false;

  const size_t MAX_SUB_CHARS    = 320000;
  const size_t MAX_SCRIPT_CHARS = 80000;

  char *title_utf8 = sanitize_utf8_lossy(movie_title ? movie_title : "");
  char *subs_utf8  = sanitize_utf8_lossy(subs_seconds_text ? subs_seconds_text : "");
  char *scr_utf8   = sanitize_utf8_lossy(optional_script_text ? optional_script_text : "");

  char *subs_trim = trim_copy_utf8_safe(subs_utf8, MAX_SUB_CHARS);

  /* Only the placeholder track needs an extra note: with real subtitles the
     character rules are part of the prompt itself (STEP 1). */
  char placeholder_note[1024];
  if (subs_placeholder)
    snprintf(placeholder_note, sizeof(placeholder_note),
             "\nIMPORTANT: INPUT A is an auto-generated PLACEHOLDER track, NOT the "
             "real dialogue. You know the movie \"%s\". Retell its ACTUAL plot in "
             "the narrations, and spread the time ranges evenly across the whole "
             "runtime shown by the INPUT A timestamps.\n",
             movie_title);
  else
    placeholder_note[0] = '\0';

  bool non_en_lang = cfg->recap_language[0] && str_icmp(cfg->recap_language, "english") != 0;

  char language_rule[512];
  language_rule[0] = '\0';
  if (non_en_lang)
    snprintf(language_rule, sizeof(language_rule),
             "\nLANGUAGE - THE MOST IMPORTANT RULE: Write ALL narrations in %s - "
             "natural, fluent and native-sounding, like a native recap narrator. "
             "The subtitles may be in English; tell the story in %s anyway. Not "
             "one narration may be in English. Keep character names recognizable "
             "(common localized names or clean transliterations).\n",
             cfg->recap_language, cfg->recap_language);

  /* INPUT C: the published plot summary (Wikipedia) is the authority on names.
     When it could not be fetched, say so plainly instead of leaving a gap the
     model might fill with invented names. */
  char *plot_utf8 = NULL;
  char *plot_trim = NULL;
  const size_t MAX_PLOT_CHARS = 14000;
  if (plot_summary && plot_summary[0]) {
    plot_utf8 = sanitize_utf8_lossy(plot_summary);
    if (plot_utf8) plot_trim = trim_copy_utf8_safe(plot_utf8, MAX_PLOT_CHARS);
  }
  char no_plot_note[600];
  if (plot_trim && plot_trim[0]) {
    no_plot_note[0] = '\0';
  } else {
    snprintf(no_plot_note, sizeof(no_plot_note),
             "(no published plot summary was available - rely on the subtitles and on "
             "your own knowledge of \"%s\", and if you are not sure of a name, use the "
             "character's role instead of guessing)", movie_title);
    plot_trim = str_dup(no_plot_note);
    plot_utf8 = plot_utf8 ? plot_utf8 : str_dup("");
  }

  /* A note appended to the end of the prompt on a retry: the language demand,
     a "your narrations are far too short" complaint, or "send JSON only".
     Built by the caller so every retry goes through the same path. */
  char demand_note[1800];
  demand_note[0] = '\0';
  if (correction_note && correction_note[0])
    snprintf(demand_note, sizeof(demand_note), "\n%s\n", correction_note);

  char sys_lang_extra[160];
  sys_lang_extra[0] = '\0';
  if (non_en_lang)
    snprintf(sys_lang_extra, sizeof(sys_lang_extra),
             " Every narration you write must be entirely in %s, never English.",
             cfg->recap_language);

  /* The exact closing sentence, in the recap language - demanding an English
     closing line for a Chinese recap pushed models into writing the whole plan
     in English.  Only the sentence itself is stored here; STEP 3 of the prompt
     wraps it in the "finish the story, then end with ..." instruction. */
  char closing_line[600];
  char closing_extra[512];
  closing_extra[0] = '\0';
  {
    const char *ocode = recap_lang_code(cfg->recap_language);
    if (!strcmp(ocode, "zh"))
      snprintf(closing_line, sizeof(closing_line),
               "\u6545\u4e8b\u5c31\u8bb2\u5230\u8fd9\u91cc\u3002"
               "\u5728\u8bc4\u8bba\u533a\u544a\u8bc9\u6211\u4eec\u4f60\u7684"
               "\u770b\u6cd5\uff0c\u522b\u5fd8\u4e86\u70b9\u8d5e\u89c6\u9891"
               "\u5e76\u8ba2\u9605\u9891\u9053\u3002");
    else if (!strcmp(ocode, "ar"))
      snprintf(closing_line, sizeof(closing_line),
               "\u0648\u0628\u0647\u0630\u0627 \u062a\u0646\u062a\u0647\u064a "
               "\u0627\u0644\u0642\u0635\u0629 \u0647\u0646\u0627. "
               "\u0623\u062e\u0628\u0631\u0648\u0646\u0627 \u0641\u064a "
               "\u0627\u0644\u062a\u0639\u0644\u064a\u0642\u0627\u062a "
               "\u0628\u0631\u0623\u064a\u0643\u0645 \u0641\u064a \u0647\u0630\u0627 "
               "\u0627\u0644\u0634\u0631\u062d\u060c \u0648\u0644\u0627 "
               "\u062a\u0646\u0633\u0648\u0627 \u0627\u0644\u0625\u0639\u062c\u0627\u0628 "
               "\u0628\u0627\u0644\u0641\u064a\u062f\u064a\u0648 \u0648\u0627\u0644\u0627\u0634\u062a\u0631\u0627\u0643 "
               "\u0641\u064a \u0627\u0644\u0642\u0646\u0627\u0629.");
    else if (!strcmp(ocode, "es"))
      snprintf(closing_line, sizeof(closing_line),
               "Y con esto la historia termina justo aqu\u00ed. Cu\u00e9ntanos "
               "en los comentarios qu\u00e9 te pareci\u00f3 esta explicaci\u00f3n "
               "y no olvides darle like al video y suscribirte al canal.");
    else
      snprintf(closing_line, sizeof(closing_line),
               "With that the story ends right here. Let us know in the comments "
               "how you liked this explanation and don't forget to like the video "
               "and subscribe to the channel.");

    /* Any other language: no fixed translation to quote, so ask for a natural
       one instead of letting the model fall back to the English line. */
    if (non_en_lang && strcmp(ocode, "zh") && strcmp(ocode, "ar") && strcmp(ocode, "es"))
      snprintf(closing_extra, sizeof(closing_extra),
               "- That closing sentence must be written in %s - a natural %s "
               "translation of the English line above, never the English text "
               "itself.\n", cfg->recap_language, cfg->recap_language);
  }
  char *scr_trim  = trim_copy_utf8_safe(scr_utf8,  MAX_SCRIPT_CHARS);

  free(subs_utf8);
  free(scr_utf8);

  /* STEP 4/STEP 5 numbers.  The voice speaks ~2.6 words per second at
     tts_rate = 110 (see config.json); the tuning band is 2.4 (audio gets cut
     off / sped up too much) to 2.8 (voice ends before the clip does). */
  const double wps = 2.6;
  int min_sec = 0, max_sec = 0, sent_lo, sent_hi;
  clip_seconds_range(per_clip_sec, &min_sec, &max_sec);
  if (per_clip_sec >= 20) {
    sent_lo = per_clip_sec / 12;
    sent_hi = sent_lo + 2;
    if (sent_lo < 3) sent_lo = 3;
  } else {
    sent_lo = 3;
    sent_hi = 5;
  }

  /* Word counts are meaningless for languages written without spaces and off
     for Arabic (spoken slower), so those languages get an explicit override
     that matches the unit the length audit measures. */
  char words_extra[400];
  words_extra[0] = '\0';
  {
    char wbuf[8];
    snprintf(wbuf, sizeof(wbuf), "%s", recap_lang_code(cfg->recap_language));
    const char *wlabel = recap_lang_label(wbuf);
    if (lang_counts_chars(wbuf)) {
      double cps = lang_speech_units_per_sec(wbuf);
      int clo = (int)((double)min_sec * cps + 0.5);
      int chi = (int)((double)max_sec * cps + 0.5);
      snprintf(words_extra, sizeof(words_extra),
               "- %s is counted in characters, not words: each narration needs "
               "about %d-%d characters, in %d-%d short sentences.\n",
               wlabel[0] ? wlabel : "This language", clo, chi, sent_lo, sent_hi);
    } else if (!strcmp(wbuf, "ar")) {
      int wlo = min_sec * 21 / 10, whi = max_sec * 3;
      snprintf(words_extra, sizeof(words_extra),
               "- Arabic is spoken a little slower: aim for %d-%d words, in %d-%d "
               "short sentences, to fill the clip without gaps.\n",
               wlo, whi, sent_lo, sent_hi);
    }
  }

  /* The recap-script prompt.  Maintainers' tuning notes:
       - Languages: if {{LANGUAGE}} is not English, keep the closing line as a
         natural translation, and keep character names as recognizable
         transliterations (both are injected below).
       - Pace: if the voice ends before the clip does, raise the multiplier to
         2.8.  If the audio is cut off or sped up too much, lower it to 2.4.
         It depends on tts_rate (config.json, currently 110).
       - Model: a strong model matters most for character names.  Avoid small or
         "mini" models for this prompt. */
  char lang_label[96];
  snprintf(lang_label, sizeof(lang_label), "%s",
           cfg->recap_language[0] ? cfg->recap_language : "English");

  /* STEP 4.  These numbers MUST follow min_sec/max_sec.  With a fixed
     "12 s = 31 words, 16 s = 42 words" example in front of it, a model asked
     for 40-second clips writes 12-second narrations: the clip is then sped up
     (at most max_video_speedup) and cut down to the narration, which is how a
     20-minute recap came out at 12 minutes. */
  char pace_line[520];
  int wlo = (int)((double)min_sec * wps + 0.5);
  int whi = (int)((double)max_sec * wps + 0.5);
  int wmin = wlo < 25 ? 25 : wlo;
  snprintf(pace_line, sizeof(pace_line),
           "- The voice speaks about %.1f words per second. A %d-second narration "
           "needs about %d words and a %d-second one about %d words, so aim for "
           "%d-%d words per narration. Never fewer than %d words in one clip: a "
           "narration that is too short for its clip gets the clip shortened to "
           "match, and the finished recap then comes out shorter than asked. "
           "Use %d-%d short sentences per clip.\n",
           wps, min_sec, wlo, max_sec, whi, wlo, whi, wmin, sent_lo, sent_hi);

  /* With a recap-minutes target the model also needs to know the total, or it
     spreads a short script over many clips and the video comes out short. */
  char total_line[400];
  total_line[0] = '\0';
  if (per_clip_sec > 0) {
    int total_words = (int)((double)num_clips * (double)per_clip_sec * wps + 0.5);
    snprintf(total_line, sizeof(total_line),
             "- Together the %d narrations must add up to about %.1f minutes of "
             "speech (roughly %d words). Each clip carries its own full narration: "
             "no one-line summaries, no empty narrations.\n",
             num_clips, (double)num_clips * (double)per_clip_sec / 60.0, total_words);
  }

  const char *prompt_fmt =
    "Movie: %s\n"
    "Narration language: %s\n"
    "Number of clips: %d\n"
    "Target clip length: %d-%d seconds each\n"
    "%s"
    "\n"
    "INPUT A: subtitles with timestamps in SECONDS (the only source for start/end times):\n"
    "%s\n"
    "%s"
    "\n"
    "INPUT B: script text without timestamps (optional, may be empty; use it for story context and to confirm who is who):\n"
    "%s\n"
    "\n"
    "INPUT C: published plot summary of \"%s\" (the source of truth for names, spelling and who does what):\n"
    "%s\n"
    "\n"
    "STEP 1: BUILD A CHARACTER LIST FIRST (do this silently, never output it)\n"
    "\n"
    "Before writing any narration, read INPUT A, INPUT B and INPUT C and work out who the characters are.\n"
    "\n"
    "- Subtitles rarely label speakers. Names appear when someone is addressed, introduced, or mentioned (\"Sergeant Reyes!\", \"Tell Anna I'm coming\"). Collect every name you find this way.\n"
    "- If INPUT C is present, it is the AUTHORITY on names: every character must be named exactly as INPUT C names them, spelled exactly the same way. Only fall back to INPUT B or your own knowledge of \"%s\" when INPUT C does not mention that character.\n"
    "- Use the exact, officially correct name of each character. Do not invent a name, do not merge two characters into one, and never swap their roles or relationships.\n"
    "- For each character, fix ONE name and keep it for the entire video. Never switch between first name, surname, nickname and rank for the same person. Pick the form used most in the movie (e.g. \"Miller\", not \"Miller\" in one clip and \"John\" in the next).\n"
    "- Name a character in a narration only when that character takes part in the events of that clip's own time range. Never mention someone who is not part of the moment you are describing.\n"
    "- On a character's first appearance, introduce them once with a short role plus their name, for example: \"a young radio operator, Private Daniels\", or \"A man named John takes the key\". After that, use only the name.\n"
    "- Introduce only the core characters. Do not mention unnecessary minor characters, and never name a person the listener has no reason to remember.\n"
    "- Pronouns must always point at the right person: never write a sentence where \"he\", \"she\" or \"they\" could mean two different people. Repeat the name instead of risking confusion.\n"
    "- If you cannot tell who someone is, call them by their role and keep that same role label every time (\"the old farmer\", \"the colonel\"). Never guess a name. Never invent a name. Never use actor names.\n"
    "- Spell names exactly the same way every time. Check the whole list again before you finish.\n"
    "\n"
    "STEP 2: HOW THE NARRATION MUST SOUND (follow exactly)\n"
    "\n"
    "- Third person, present tense, strictly chronological. You are telling the STORY, not describing the video. Write the action as it happens: \"Hank hits the gas and gets the car out of there\", not \"Hank hit the gas\" and not \"Hank will escape\".\n"
    "- One continuous voice. Each clip must feel like the next sentence of the same story, not a separate summary. The reader should never notice where one clip ends and the next begins.\n"
    "- Never copy a subtitle line word for word and never list dialogue. Retell everything in your own words; the narration must read like one continuous story, never like subtitles being read out.\n"
    "- Fast pace. Every sentence moves the plot forward: someone does something, something goes wrong, someone decides, something changes. No scenery, no mood-setting, no reflection.\n"
    "- Short, simple, spoken sentences (about 8-16 words each). Plain words. No long clauses, no semicolons, no brackets, no emojis, no stage directions. Write for the ear, not for the eye.\n"
    "- Link events with cause and effect: \"because\", \"so\", \"after\", \"but\", \"until\", \"which means\". Jump in time or place with a short connector: \"Meanwhile,\", \"Later,\", \"That night,\", \"The next morning,\", \"Hours later,\", \"Back at the base,\".\n"
    "- Report dialogue instead of quoting it: \"He tells her the bridge is gone, but she refuses to turn back.\" Direct quotes only when a single short line is the turning point of the story.\n"
    "- Name the stakes early and keep them alive: what does the hero want, what is in the way, what happens if they fail.\n"
    "- Use concrete verbs: grabs, runs, hides, shoots, lies, betrays, discovers, escapes. Avoid vague words like \"things\", \"situation\", \"something happens\".\n"
    "- NEVER describe the screen: no \"in this scene\", \"we see\", \"the camera\", \"the movie shows\", \"the audience\". No opinions, no analysis, no themes, no cinematography, no spoilers-warnings, no jokes about the movie, no rhetorical questions to the viewer.\n"
    "- The \"narration\" text is ONLY the spoken words. No timestamps, no character headings like \"JOHN:\", no scene labels, no director notes, no notes to yourself, no markdown, no quotes around the whole text.\n"
    "- No filler or trailer cliches: \"the stakes get raised\", \"everything changes\", \"little does he know\", \"will he survive?\".\n"
    "- Do not invent anything. Every event must be supported by INPUT A (or INPUT B or INPUT C). If a stretch of subtitles is unclear, keep that narration short and factual instead of guessing.\n"
    "\n"
    "STEP 3: OPENING AND ENDING\n"
    "\n"
    "- The FIRST narration starts immediately with \"The story begins in...\" or \"The movie starts with...\" and in the first two sentences sets up who the main character is, where and when they are, and what they want or have lost. Then the story starts moving. No greeting, no channel intro, no \"welcome\", no \"today we\", no title, no year-only line.\n"
    "- The opening must describe what happens at the START of the movie (the first clip's own time range), and the character named first is the film's protagonist in those first scenes - the one INPUT C introduces first. Never open on a side character or on a later scene, and never name a character before the moment they actually appear.\n"
    "- The LAST narration finishes the story (the outcome for the main characters) and then ends EXACTLY with: \"%s\"\n"
    "%s"
    "\n"
    "STEP 4: PACE AND LENGTH (so the voice fills the clip with no gaps)\n"
    "\n"
    "%s"
    "%s"
    "%s"
    "- The narration must be spoken-length for the time range, so the audio fills the clip without silence and without needing to be cut off.\n"
    "- Do not leave plot holes between clips. Together the clips must tell the complete story from beginning to ending, with no important event skipped.\n"
    "\n"
    "STEP 5: CHOOSING THE CLIPS\n"
    "\n"
    "- Choose exactly %d non-overlapping time ranges that cover the whole plot arc from the opening to the ending, spaced so that early, middle and final parts of the movie all get fair coverage. Do not spend more than a few clips on the first quarter of the movie.\n"
    "- Each range must be %d-%d seconds long (end minus start). Never start at 0.\n"
    "- Use INPUT A only for the start and end times. Choose moments with clear action: arrivals, discoveries, confrontations, betrayals, escapes, deaths, big decisions, the climax and the ending.\n"
    "- Each narration must match what happens in its own time range, but may add one short line of setup from earlier so the story stays clear.\n"
    "- Clips must be in increasing order of start time.\n"
    "\n"
    "STEP 6: FINAL CHECK (do this silently before answering)\n"
    "\n"
    "1. Is every character named the same way in every clip, and spelled the same way?\n"
    "2. Does the first narration begin with \"The story begins...\" and the last end with the exact closing line?\n"
    "3. Does every narration have at least the minimum word count from STEP 4 for its own time range?\n"
    "4. Is any sentence describing the screen, the camera, or giving an opinion? Remove it.\n"
    "5. Is anything in the narration not supported by the subtitles or script? Remove it.\n"
    "6. Is the output valid JSON with nothing else around it?\n"
    "7. Are the clips in increasing order of start time, with no overlapping ranges and no repeated scene?\n"
    "\n"
    "OUTPUT FORMAT (strict JSON only)\n"
    "\n"
    "{\"clips\":[{\"start\":120,\"end\":135,\"narration\":\"...\"},{\"start\":142,\"end\":157,\"narration\":\"...\"}]}\n"
    "\n"
    "STYLE EXAMPLE\n"
    "\n"
    "GOOD: \"The story begins in the winter of 1944, deep behind enemy lines. Sergeant Cole and his four-man team are dropped into a frozen forest to cut a German supply route. But the landing goes wrong. Within minutes, enemy patrols surround them, and Cole is the only one who makes it out alive.\"\n"
    "\n"
    "BAD: \"In this scene we see soldiers in a forest. The camera pans across the snow and the lighting is dark. This is a very tense moment in the movie.\"\n";

  int plen = snprintf(NULL, 0, prompt_fmt,
                      title_utf8,          /* Movie:                    */
                      lang_label,          /* Narration language:       */
                      num_clips,           /* Number of clips:          */
                      min_sec, max_sec,    /* Target clip length:       */
                      language_rule,
                      subs_trim,           /* INPUT A                   */
                      placeholder_note,
                      scr_trim,            /* INPUT B                   */
                      title_utf8,          /* INPUT C: the movie name   */
                      plot_trim,           /* INPUT C: the plot summary */
                      title_utf8,          /* STEP 1: your knowledge of */
                      closing_line,        /* STEP 3: closing sentence  */
                      closing_extra,
                      pace_line,           /* STEP 4                    */
                      words_extra,
                      total_line,          /* STEP 4: the whole recap   */
                      num_clips,           /* STEP 5: how many clips    */
                      min_sec, max_sec);   /* STEP 5: range length      */
  if (plen < 0) die("snprintf failed building prompt");
  /* demand_note (language retry) is appended separately: it is empty on the
     first attempt and only set when the model answered in the wrong language. */
  size_t demand_len = strlen(demand_note);
  size_t plot_len = plot_trim ? strlen(plot_trim) : 0;
  char *prompt = (char *)malloc((size_t)plen + plot_len + demand_len + 1);
  if (!prompt) die("OOM");
  snprintf(prompt, (size_t)plen + plot_len + 1, prompt_fmt,
           title_utf8, lang_label, num_clips, min_sec, max_sec, language_rule,
           subs_trim, placeholder_note, scr_trim, title_utf8, plot_trim,
           title_utf8, closing_line, closing_extra, pace_line, words_extra,
           total_line, num_clips, min_sec, max_sec);
  plen = strlen(prompt);
  if (demand_len) memcpy(prompt + plen, demand_note, demand_len + 1);

  free(title_utf8);
  free(subs_trim);
  free(scr_trim);
  free(plot_utf8);
  free(plot_trim);

  cJSON *req = cJSON_CreateObject();
  cJSON_AddStringToObject(req, "model", cfg->openai_model);
  /* A 20-30 clip plan is several thousand tokens; without an explicit limit the
     provider's default can cut the JSON off mid-array. */
  cJSON_AddNumberToObject(req, "max_output_tokens", 32000);

  cJSON *reasoning = cJSON_CreateObject();
  cJSON_AddStringToObject(reasoning, "effort", "high");
  cJSON_AddItemToObject(req, "reasoning", reasoning);

  cJSON *input = cJSON_CreateArray();
  char sys_buf[900];
  snprintf(sys_buf, sizeof(sys_buf),
           "You are the narrator-scriptwriter for a top YouTube movie recap "
           "channel. You retell a whole movie as one continuous, gripping spoken "
           "story, the way a storyteller would tell it to a friend who has not "
           "seen it. You always answer with strict JSON only: no markdown, no "
           "commentary, no text before or after the JSON.%s",
           sys_lang_extra);
  const char *sys_prompt = sys_buf;
  cJSON *sys = cJSON_CreateObject();
  cJSON_AddStringToObject(sys, "role", "system");
  cJSON_AddStringToObject(sys, "content", sys_prompt);
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

  /* Classic /chat/completions shape, used as a fallback for OpenAI-compatible
   * providers that do not implement the Responses API (DeepSeek and most
   * others). */
  cJSON *creq = cJSON_CreateObject();
  cJSON_AddStringToObject(creq, "model", cfg->openai_model);
  cJSON *msgs = cJSON_CreateArray();
  cJSON *csys = cJSON_CreateObject();
  cJSON_AddStringToObject(csys, "role", "system");
  cJSON_AddStringToObject(csys, "content", sys_prompt);
  cJSON_AddItemToArray(msgs, csys);
  cJSON *cusr = cJSON_CreateObject();
  cJSON_AddStringToObject(cusr, "role", "user");
  cJSON_AddStringToObject(cusr, "content", prompt);
  cJSON_AddItemToArray(msgs, cusr);
  cJSON_AddItemToObject(creq, "messages", msgs);
  cJSON *rfmt = cJSON_CreateObject();
  cJSON_AddStringToObject(rfmt, "type", "json_object");
  cJSON_AddItemToObject(creq, "response_format", rfmt);

  /* A 20-30 clip plan needs a few thousand output tokens.  Newer OpenAI models
     only accept max_completion_tokens (max_tokens is an error there), while
     some compatible providers do not know the newer name at all - so the plain
     body is kept around and used if the provider complains. */
  char *chat_body = cJSON_PrintUnformatted(creq);
  cJSON_AddNumberToObject(creq, "max_completion_tokens", 32000);
  char *chat_body_limited = cJSON_PrintUnformatted(creq);
  cJSON_Delete(creq);

  if (!body) {
    free(chat_body);
    free(chat_body_limited);
    free(prompt);
    ClipPlanList empty = {0};
    return empty;
  }

  long http_code = 0;
  bool has_script = (optional_script_text && optional_script_text[0] != 0);
  long timeout_s = has_script ? 14400L : 3600L;

  bool anthropic_base = strcasestr_local(cfg->openai_base_url, "anthropic") != NULL;
  char endpoint[560];
  MemBuf resp;

  if (anthropic_base) {
    free(body);
    resp = anthropic_plan_request(cfg, sys_prompt, prompt, &http_code, timeout_s);
    if (http_code < 200 || http_code >= 300) {
      logw("Anthropic-compatible HTTP %ld from %s", http_code, cfg->openai_base_url);
      if (resp.data && resp.size) logw("Anthropic raw body: %.800s", resp.data);
      if (http_code == 401 || http_code == 403)
        logw("The key was rejected. api.anthropic.com wants an Anthropic API key "
             "(sk-ant-api...); Anthropic-compatible gateways (DeepSeek, Azure Foundry, "
             "...) want THEIR key while the base URL points at their /anthropic path.");
      else if (http_code == 404)
        logw("Endpoint not found. For native Claude use \"openai_base_url\": "
             "\"https://api.anthropic.com/v1\"; for DeepSeek use "
             "\"https://api.deepseek.com/anthropic\".");
      else if (http_code == 429)
        logw("Rate limited or out of quota on the Anthropic-compatible endpoint.");
      if (resp.data) free(resp.data);
      resp.data = NULL;
      resp.size = 0;

      /* api.anthropic.com only speaks the Messages API - the OpenAI-style
         /chat/completions retry below can only ever 404 there. */
      if (anthropic_host_is_native(cfg->openai_base_url)) {
        logw("api.anthropic.com has no OpenAI-style /chat/completions endpoint, "
             "so there is nothing to fall back to.");
        free(chat_body);
        free(chat_body_limited);
        free(prompt);
        ClipPlanList empty = {0};
        return empty;
      }

      /* Rescue: gateways like DeepSeek's also serve /chat/completions on the
         plain base URL - derive it and retry with the SAME prompt. */
      char base2[512];
      anthropic_openai_base(cfg->openai_base_url, base2, sizeof(base2));
      snprintf(endpoint, sizeof(endpoint), "%s/chat/completions", base2);
      logi("Anthropic endpoint failed - trying the OpenAI-style endpoint instead: %s", endpoint);
      resp = openai_post_chat_tokens(endpoint, cfg->openai_key,
                                     chat_body_limited ? chat_body_limited : chat_body,
                                     chat_body, &http_code, timeout_s);
      if (http_code < 200 || http_code >= 300) {
        logw("Chat-completions HTTP %ld", http_code);
        if (resp.data && resp.size) logw("Chat raw body: %.800s", resp.data);
        if (has_script && resp.data && openai_resp_should_retry_without_script(resp.data)) {
          if (out_retry_without_script) *out_retry_without_script = true;
        }
        if (resp.data) free(resp.data);
        free(chat_body);
        free(chat_body_limited);
        free(prompt);
        ClipPlanList empty = {0};
        return empty;
      }
    }
    goto have_response;
  }

  snprintf(endpoint, sizeof(endpoint), "%s/responses", cfg->openai_base_url);
  resp = http_post_json_to_mem(endpoint, cfg->openai_key, body, &http_code, timeout_s);
  free(body);

  if (http_code < 200 || http_code >= 300) {
    logw("OpenAI HTTP %ld", http_code);
    if (resp.data && resp.size) logw("OpenAI raw body: %.800s", resp.data);

    if (has_script && resp.data && openai_resp_should_retry_without_script(resp.data)) {
      if (out_retry_without_script) *out_retry_without_script = true;
    }

    if (resp.data) free(resp.data);
    resp.data = NULL;
    resp.size = 0;

    logi("Trying the provider's /chat/completions endpoint instead (DeepSeek and other OpenAI-compatible APIs)...");
    snprintf(endpoint, sizeof(endpoint), "%s/chat/completions", cfg->openai_base_url);
    resp = openai_post_chat_tokens(endpoint, cfg->openai_key,
                                   chat_body_limited ? chat_body_limited : chat_body,
                                   chat_body, &http_code, timeout_s);
    if (http_code < 200 || http_code >= 300) {
      logw("Chat-completions HTTP %ld", http_code);
      if (resp.data && resp.size) logw("Chat raw body: %.800s", resp.data);
      if (resp.data) free(resp.data);
      free(chat_body);
      free(chat_body_limited);
      free(prompt);
      ClipPlanList empty = {0};
      return empty;
    }
  }
have_response:;
  free(prompt);

  char *out_text = openai_extract_output_text(resp.data ? resp.data : "");
  if (!out_text) {
    logw("AI response parse failed (no text content in the reply).");
    if (resp.data && resp.size) logw("Raw reply: %.800s", resp.data);
    if (has_script && resp.data && openai_resp_should_retry_without_script(resp.data)) {
      if (out_retry_without_script) *out_retry_without_script = true;
    }
  }

  ClipPlanList plan = {0};
  if (out_text) plan = parse_clip_plan_json(out_text);
  if (plan.count == 0 && out_text) {
    logw("AI reply contained no usable clip plan - most likely the JSON was cut "
         "off (output limit) or the model refused.");
    logw("Reply started: %.300s", out_text);
    if (out_retry_json_only) *out_retry_json_only = true;
  }
  warn_if_plan_was_cut_short(resp.data, plan.count);

  /* Anthropic-style gateways truncate or refuse more often than the plain
     chat endpoint - give /chat/completions one chance with the SAME prompt
     before the run drops to the offline subtitle planner.  (Skipped for
     api.anthropic.com, which does not implement that endpoint at all.) */
  if (plan.count == 0 && anthropic_base && chat_body &&
      !anthropic_host_is_native(cfg->openai_base_url)) {
    char base2[512];
    anthropic_openai_base(cfg->openai_base_url, base2, sizeof(base2));
    snprintf(endpoint, sizeof(endpoint), "%s/chat/completions", base2);
    logi("Trying the OpenAI-style endpoint instead: %s", endpoint);
    long code2 = 0;
    MemBuf r2 = openai_post_chat_tokens(endpoint, cfg->openai_key,
                                        chat_body_limited ? chat_body_limited : chat_body,
                                        chat_body, &code2, timeout_s);
    if (code2 >= 200 && code2 < 300) {
      char *t2 = openai_extract_output_text(r2.data ? r2.data : "");
      if (t2) {
        plan = parse_clip_plan_json(t2);
        free(t2);
        warn_if_plan_was_cut_short(r2.data, plan.count);
        if (plan.count > 0) logok("Recovered the clip plan via %s", endpoint);
      }
    } else {
      logw("Chat-completions HTTP %ld", code2);
      if (r2.data && r2.size) logw("Chat raw body: %.800s", r2.data);
    }
    free(r2.data);
  }

  free(out_text);
  free(chat_body);
  free(chat_body_limited);
  free(resp.data);
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
  if (cfg->tts_rate > 0 && cfg->tts_rate != 100) {
    double sp = cfg->tts_rate / 100.0;
    if (sp < 0.7) sp = 0.7;
    if (sp > 1.2) sp = 1.2;
    cJSON_AddNumberToObject(root, "speed", sp);
  }
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
  if (cfg->tts_rate > 0 && cfg->tts_rate != 100) {
    double sp = cfg->tts_rate / 100.0;
    if (sp < 0.5) sp = 0.5;
    if (sp > 2.0) sp = 2.0;
    cJSON_AddNumberToObject(root, "speed", sp);
  }
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
  if (cfg->tts_rate > 0 && cfg->tts_rate != 100) {
    double ls = 100.0 / (double)cfg->tts_rate;   /* smaller length_scale = faster */
    if (ls < 0.5) ls = 0.5;
    if (ls > 2.0) ls = 2.0;
    cJSON_AddNumberToObject(root, "length_scale", ls);
  }
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
  if (cfg->tts_rate > 0 && cfg->tts_rate != 100) {
    double sp = cfg->tts_rate / 100.0;
    if (sp < 0.25) sp = 0.25;
    if (sp > 4.0) sp = 4.0;
    cJSON_AddNumberToObject(root, "speed", sp);
  }
  char *body = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);

  const char *key = cfg->tts_api_key[0] ? cfg->tts_api_key : cfg->openai_key;
  bool ok = tts_post_audio(url, body, key, out_mp3_path, "OpenAI-compatible TTS");
  free(body);
  return ok;
}

/* ---------------------------------------------------------------------------
 * Piper auto-start: when the narration engine is Piper but nothing is
 * listening on its port, launch the already-installed portable server so a
 * second window is no longer needed.
 * ------------------------------------------------------------------------ */
static bool piper_server_reachable(const char *base_url) {
  char url[1200];
  snprintf(url, sizeof(url), "%s/info", base_url);
  long code = 0;
  MemBuf m = http_get_to_mem_ex(url, &code);
  bool ok = (m.data != NULL) && code >= 200 && code < 300;
  free(m.data);
  return ok;
}

static void piper_ensure_server(const Config *cfg) {
  if (cfg->tts_provider != TTS_PIPER) return;
  const char *base = cfg->tts_base_url;
  if (!base || !base[0]) base = "http://127.0.0.1:5000";
  if (piper_server_reachable(base)) return;

  logi("Piper server not reachable at %s - trying to start the installed one...", base);

  int port = 5000;
  const char *colon = strrchr(base, ':');
  if (colon) {
    int pv = atoi(colon + 1);
    if (pv > 0) port = pv;
  }

  const char *voice = getenv("PIPER_VOICE");
  if (!voice || !voice[0]) voice = "en_US-lessac-medium";

  char cwd[PATH_MAX] = "";
  plat_getcwd(cwd, sizeof(cwd));
  const char *envtools = getenv("MOVIECAP_TOOLS");

  char roots[3][PATH_MAX];
  int nroots = 0;
  if (envtools && envtools[0]) snprintf(roots[nroots++], sizeof(roots[0]), "%s", envtools);
  snprintf(roots[nroots++], sizeof(roots[0]), "F:/AI-Movie-Shorts/tools");
  if (cwd[0]) snprintf(roots[nroots++], sizeof(roots[0]), "%s/tools", cwd);

  char py[PATH_MAX], voices[PATH_MAX], onnx[PATH_MAX];
  bool found = false;
  for (int i = 0; i < nroots && !found; i++) {
    snprintf(py, sizeof(py), "%s/piper/python/python.exe", roots[i]);
    snprintf(voices, sizeof(voices), "%s/piper/voices", roots[i]);
    snprintf(onnx, sizeof(onnx), "%s/%s.onnx", voices, voice);
    if (file_exists(py) && file_exists(onnx)) found = true;
  }

  if (!found) {
    logw("Piper is not installed yet. Double-click run.bat once (it downloads Piper),");
    logw("or run \"run.bat tts\" in a second window, then press Generate again.");
    return;
  }

  char cmd[PATH_MAX * 2 + 256];
  snprintf(cmd, sizeof(cmd),
           "\"%s\" -m piper.http_server --host 127.0.0.1 --port %d -m %s --data-dir \"%s\"",
           py, port, voice, voices);
  if (!plat_spawn_detached(cmd)) {
    logw("Could not start the Piper server automatically. Run \"run.bat tts\" in a second window.");
    return;
  }

  for (int i = 0; i < 60; i++) {
    plat_sleep_ms(500);
    if (piper_server_reachable(base)) {
      logok("Piper server started automatically at %s", base);
      return;
    }
  }
  logw("Piper server was started but is not answering at %s yet.", base);
  logw("If TTS keeps failing, run \"run.bat tts\" in a second window to see its output.");
}

/* Single entry point used by the pipeline. */
/* The bundled private Python that run.bat installs under tools\piper\python. */
static bool find_tools_python(char *py_out, size_t py_sz, char *root_out, size_t root_sz) {
  char cwd[PATH_MAX] = "";
  plat_getcwd(cwd, sizeof(cwd));
  const char *envtools = getenv("MOVIECAP_TOOLS");
  char roots[3][PATH_MAX];
  int nroots = 0;
  if (envtools && envtools[0]) snprintf(roots[nroots++], sizeof(roots[0]), "%s", envtools);
  snprintf(roots[nroots++], sizeof(roots[0]), "F:/AI-Movie-Shorts/tools");
  if (cwd[0]) snprintf(roots[nroots++], sizeof(roots[0]), "%s/tools", cwd);
  for (int i = 0; i < nroots; i++) {
    snprintf(py_out, py_sz, "%s/piper/python/python.exe", roots[i]);
    if (file_exists(py_out)) {
      if (root_out && root_sz) snprintf(root_out, root_sz, "%s", roots[i]);
      return true;
    }
  }
  py_out[0] = '\0';
  return false;
}

/* Free Microsoft Edge neural TTS via the bundled Python + edge-tts package. */
static bool tts_edge(const Config *cfg, const char *text, const char *out_mp3_path) {
  char py[PATH_MAX];
  if (!find_tools_python(py, sizeof(py), NULL, 0)) {
    logw("Edge TTS needs the bundled Python - double-click run.bat once to install it.");
    return false;
  }
  char txt[PATH_MAX];
  snprintf(txt, sizeof(txt), "%s.txt", out_mp3_path);
  FILE *f = plat_fopen(txt, "wb");
  if (!f) { logw("Cannot write the Edge TTS text file."); return false; }
  fputs(text, f);
  fclose(f);
  const char *voice = cfg->tts_voice[0] ? cfg->tts_voice : "en-US-ChristopherNeural";
  char rate_arg[48] = "";
  if (cfg->tts_rate > 0 && cfg->tts_rate != 100) {
    int d = cfg->tts_rate - 100;
    snprintf(rate_arg, sizeof(rate_arg), " --rate \"%s%d%%\"", d > 0 ? "+" : "", d);
  }
  int rc = run_cmd("\"%s\" edge_tts_synth.py --voice %s --text-file \"%s\" --out \"%s\"%s",
                   py, voice, txt, out_mp3_path, rate_arg);
  remove(txt);
  if (rc != 0 || !file_exists(out_mp3_path)) {
    logw("Edge TTS failed (rc=%d) - check the internet connection or switch to Piper.", rc);
    return false;
  }
  return true;
}

/* faster-whisper: movie audio -> real SRT, so a movie without subtitles can
 * still get a story-accurate recap. Model cache stays on the tools drive. */
static bool whisper_transcribe_to_srt(const char *movie_path, const char *out_srt,
                                      const char *model, const char *py,
                                      const char *tools_root) {
  ensure_dir("scripts/srt_files");
  ensure_dir("clips");
  const char *wav = "clips/_whisper_audio.wav";
  if (run_cmd("ffmpeg -y -hide_banner -loglevel error -i \"%s\" -vn -ac 1 -ar 16000 \"%s\"",
              movie_path, wav) != 0) {
    logw("Could not extract the movie audio for transcription.");
    return false;
  }
  char cache[PATH_MAX];
  snprintf(cache, sizeof(cache), "%s/hf-cache", tools_root);
  int rc = run_cmd("\"%s\" whisper_transcribe.py --model %s --audio \"%s\" --out \"%s\" --cache-dir \"%s\"",
                   py, model, wav, out_srt, cache);
  remove(wav);
  if (rc != 0) return false;
  FILE *f = plat_fopen(out_srt, "rb");
  if (!f) return false;
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fclose(f);
  return sz > 200;
}

/* Every TTS engine leaves silence at the start/end of a clip; those dead
 * gaps at every cut are most of the "slow pacing" feel - and they also shift
 * every caption away from the words, because the caption timeline starts at the
 * beginning of the audio file. Trim them. */
static void tts_trim_silence(const char *mp3) {
  char tmp[PATH_MAX];
  snprintf(tmp, sizeof(tmp), "%s.trim.mp3", mp3);
  char *in_esc  = sh_escape(mp3);
  char *tmp_esc = sh_escape(tmp);
  /* -f mp3: the temporary name ends in .mp3, but be explicit - without a known
     container ffmpeg refuses to write the file and the trim silently never
     happens (which is how captions drifted for every clip). */
  int rc = run_cmd(
    "ffmpeg -y -hide_banner -loglevel error -i %s -af "
    "\"silenceremove=start_periods=1:start_threshold=-45dB,areverse,"
    "silenceremove=start_periods=1:start_threshold=-45dB,areverse\" "
    "-c:a libmp3lame -b:a 192k -f mp3 %s", in_esc, tmp_esc);
  free(in_esc);
  free(tmp_esc);
  if (rc == 0 && file_size_bytes(tmp) > 2048) {
    plat_unlink(mp3);
    if (plat_rename(tmp, mp3) != 0) logw("Could not swap in the silence-trimmed narration.");
  } else {
    /* Never silent: the trim decides how well captions line up with the voice. */
    logw("Could not trim the silence at the edges of %s (ffmpeg exit %d) - "
         "captions and pacing will be slightly less tight.", mp3, rc);
    plat_unlink(tmp);
  }
}

static bool tts_synthesize(const Config *cfg, const char *text, const char *out_mp3_path) {
  bool ok = false;
  switch (cfg->tts_provider) {
    case TTS_XTTS:   ok = tts_xtts(cfg, text, out_mp3_path); break;
    case TTS_PIPER:  ok = tts_piper(cfg, text, out_mp3_path); break;
    case TTS_OPENAI: ok = tts_openai_compat(cfg, text, out_mp3_path); break;
    case TTS_EDGE:   ok = tts_edge(cfg, text, out_mp3_path); break;
    default:         ok = elevenlabs_tts_to_mp3(cfg, text, out_mp3_path); break;
  }
  if (ok) tts_trim_silence(out_mp3_path);
  return ok;
}

/* ---------------------------------------------------------------------------
 * Burnt-in subtitles: small, centred near the bottom of the frame.
 * ------------------------------------------------------------------------ */
static bool caption_font_available(const char *font) {
  return file_exists(font);
}

/* A font that actually has the glyphs of this language.  The per-language
 * setting in the panel wins; without it a Windows system font for that script
 * is used, because the bundled Inter font has no CJK and no Arabic letters at
 * all - the captions would come out as a row of empty boxes.  Returns "" when
 * nothing suitable was found (the caller then falls back and warns). */
static const char *caption_font_for_language(const Config *cfg, const char *lang) {
  const char *code = recap_lang_code(lang);

  if (!strcmp(code, "zh") && cfg->caption_font_zh[0]) return cfg->caption_font_zh;
  if (!strcmp(code, "ar") && cfg->caption_font_ar[0]) return cfg->caption_font_ar;
  if (!strcmp(code, "es") && cfg->caption_font_es[0]) return cfg->caption_font_es;

  static const char *zh_fonts[] = {
    "C:/Windows/Fonts/msyh.ttc",      /* Microsoft YaHei  */
    "C:/Windows/Fonts/msyhbd.ttc",
    "C:/Windows/Fonts/msjh.ttc",      /* Microsoft JhengHei */
    "C:/Windows/Fonts/simhei.ttf",    /* SimHei           */
    "C:/Windows/Fonts/simsun.ttc",    /* SimSun           */
    "C:/Windows/Fonts/meiryo.ttc",    /* Meiryo (kana)    */
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    NULL
  };
  static const char *ko_fonts[] = {
    "C:/Windows/Fonts/malgun.ttf",    /* Malgun Gothic (Hangul) */
    "C:/Windows/Fonts/malgunbd.ttf",
    NULL
  };
  static const char *ar_fonts[] = {
    "C:/Windows/Fonts/arial.ttf",
    "C:/Windows/Fonts/segoeui.ttf",
    "C:/Windows/Fonts/tahoma.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    NULL
  };
  const char **cand = NULL;
  if (!strcmp(code, "ko")) cand = ko_fonts;
  else if (!strcmp(code, "zh") || !strcmp(code, "ja")) cand = zh_fonts;
  else if (!strcmp(code, "ar") || !strcmp(code, "fa") || !strcmp(code, "ur")) cand = ar_fonts;
  if (cand) {
    for (int k = 0; cand[k]; k++)
      if (file_exists(cand[k])) return cand[k];
    if (!strcmp(code, "ko")) {          /* fall back to a CJK font for Hangul */
      for (int k = 0; zh_fonts[k]; k++)
        if (file_exists(zh_fonts[k])) return zh_fonts[k];
    }
    return "";
  }
  return cfg->caption_font;
}

/* Build a chain of up to three stacked drawtext filters for the caption.
 * Two ffmpeg tokenizers see this string.  Level 1 (the filtergraph parser)
 * copies single-quoted sections verbatim but ends them at a raw apostrophe;
 * level 2 (the filter-args parser) splits options on ':' and honours
 * backslash escapes.  So inside text='...' we keep no ' " \ (the argv
 * splitter chokes on " and \ too), and every ':' is written \: so it
 * survives level 2 as a literal colon.  Wrapped lines become separate
 * drawtext filters - no newline characters anywhere. */
/* Word-wrapped captions: one segment = one sentence-ish chunk (at most two
 * lines), one line = one drawtext filter.  The limits are generous because the
 * character budget per line shrinks for wide scripts (see caption_line_cap). */
#define CAP_MAX_SEG  512
#define CAP_MAX_LINE 1024

/* ---------------------------------------------------------------------------
 * Caption timing.
 *
 * A caption must change exactly when the voice moves on to the next sentence.
 * Splitting the clip's narration duration by character count does not do that:
 * "1944" is 4 characters but over a second of speech, long words take longer
 * than their letter count suggests, and sentences are separated by pauses.  So
 * the duration is estimated per sentence (syllables + digits that are read out
 * + punctuation pauses), and the resulting boundaries are then snapped onto the
 * real pauses found in the narration audio - the points where the voice
 * genuinely stops.  That is what keeps the text on screen in step with the
 * words being spoken instead of running ahead of them in one sentence and
 * behind in the next.
 * ------------------------------------------------------------------------ */

static bool cp_is_cjk(unsigned v) {
  return (v >= 0x3040 && v <= 0x30FF) ||   /* kana            */
         (v >= 0x3400 && v <= 0x4DBF) ||   /* CJK ext A       */
         (v >= 0x4E00 && v <= 0x9FFF) ||   /* CJK unified     */
         (v >= 0xAC00 && v <= 0xD7AF) ||   /* Hangul          */
         (v >= 0xF900 && v <= 0xFAFF);     /* CJK compat      */
}

static bool cp_is_arabic(unsigned v) {
  return (v >= 0x0600 && v <= 0x06FF) ||
         (v >= 0x0750 && v <= 0x077F) ||
         (v >= 0xFB50 && v <= 0xFDFF) ||
         (v >= 0xFE70 && v <= 0xFEFF);
}

static bool cp_is_letter(unsigned v) {
  if ((v >= 'A' && v <= 'Z') || (v >= 'a' && v <= 'z')) return true;
  if (v >= 0xC0 && v <= 0x24F) return true;      /* Latin-1 + extended */
  if (v >= 0x370 && v <= 0x3FF) return true;     /* Greek              */
  if (v >= 0x400 && v <= 0x4FF) return true;     /* Cyrillic           */
  return cp_is_cjk(v) || cp_is_arabic(v);
}

static unsigned cp_lower(unsigned v) {
  if (v >= 'A' && v <= 'Z') return v + 32;
  return v;
}

/* How wide is a character of this script on screen, in units of the font size?
 * CJK characters are square, Latin letters average about half the height and
 * Arabic a little more than half.  This is what decides how many characters fit
 * on one caption line, so a Chinese caption does not run off both sides of the
 * frame and a Latin one is not cut in half by the vertical (9:16) crop. */
static double cp_width_factor(unsigned v) {
  if (v == ' ' || v == '\t') return 0.30;
  if (cp_is_cjk(v)) return 1.00;
  if (cp_is_arabic(v)) return 0.58;
  if (v >= '0' && v <= '9') return 0.60;
  if (v >= 'A' && v <= 'Z') return 0.62;
  if (!cp_is_letter(v)) return 0.40;      /* punctuation */
  return 0.52;
}

static double caption_avg_width_factor(const unsigned *cp, size_t n) {
  if (n == 0) return 0.52;
  double sum = 0;
  size_t counted = 0;
  for (size_t i = 0; i < n; i++) {
    if (cp[i] == ' ' || cp[i] == '\t') continue;   /* spaces do not decide width */
    sum += cp_width_factor(cp[i]);
    counted++;
  }
  if (counted == 0) return 0.52;
  return sum / (double)counted;
}

/* Characters per caption line.  Sized for the NARROW CENTRE STRIP that the
 * vertical render keeps: ffmpeg_make_vertical crops to the middle 60% of the
 * width, so anything wider would be cut off at the sides in the Short. */
static int caption_line_cap(const unsigned *cp, size_t nr, int frame_w, int frame_h) {
  double w = frame_w > 0 ? (double)frame_w : 1280.0;
  double h = frame_h > 0 ? (double)frame_h : 720.0;
  double font_px = h * 0.035;
  double usable = w * 0.56;
  double avg = caption_avg_width_factor(cp, nr);
  int cap = (int)(usable / (avg * font_px + 1e-9));
  if (cap > 42) cap = 42;      /* never a wall of text, whatever the frame is */
  /* Square characters read best with fewer of them per line, and 24 of them
     still fit the middle 60% of a 1080p frame with room to spare. */
  if (avg >= 0.85 && cap > 24) cap = 24;
  if (cap < 12) cap = 12;      /* never one word per line either */
  return cap;
}

/* Vowel groups are a good enough stand-in for syllables: "extraordinary" (5)
 * vs "cat" (1) is what decides how long a word takes to say. */
static int latin_syllables(const unsigned *cp, size_t a, size_t b) {
  static const unsigned extra[] = {
    0xE1, 0xE0, 0xE2, 0xE4, 0xE3, 0xE5,   /* a variants */
    0xE9, 0xE8, 0xEA, 0xEB,               /* e variants */
    0xED, 0xEC, 0xEE, 0xEF,               /* i variants */
    0xF3, 0xF2, 0xF4, 0xF6, 0xF5,         /* o variants */
    0xFA, 0xF9, 0xFB, 0xFC,               /* u variants */
    0xE7, 0xF1, 0xFF, 0xFD                /* c-cedilla, n-tilde, y variants */
  };

  int syl = 0;
  bool prev_vowel = false;
  for (size_t i = a; i < b; i++) {
    unsigned v = cp_lower(cp[i]);
    bool is_v = (v == 'a' || v == 'e' || v == 'i' || v == 'o' || v == 'u' || v == 'y');
    for (size_t k = 0; !is_v && k < sizeof(extra) / sizeof(extra[0]); k++)
      if (v == extra[k]) is_v = true;

    if (is_v) {
      if (!prev_vowel) syl++;
      prev_vowel = true;
    } else {
      prev_vowel = false;
    }
  }
  if (syl == 0) syl = 1;

  /* silent final "e"/"es": "make" is one syllable, not two */
  if (syl > 1 && b > a) {
    unsigned last = cp_lower(cp[b - 1]);
    bool e_end    = (last == 'e'  || last == 0xE9);
    bool es_end   = (last == 's'  && b - a >= 2 && cp_lower(cp[b - 2]) == 'e');
    if (e_end || es_end) {
      unsigned before = (es_end && b - a >= 3) ? cp_lower(cp[b - 3])
                       : (e_end && b - a >= 2) ? cp_lower(cp[b - 2]) : 0;
      if (before != 'l' && before != 'r' && before != 'c' && before != 's' &&
          before != 'g' && before != 'z')
        syl--;
    }
  }
  return syl;
}

/* Relative speech time of one caption chunk, in "syllable units". */
static double speech_units(const unsigned *cp, size_t from, size_t to) {
  double units = 0;
  size_t i = from;

  while (i < to) {
    unsigned v = cp[i];

    if (v == ' ' || v == '\t' || v == 0x00A0) { i++; continue; }

    /* Digits are read out one by one (or as a number): both are slow. */
    if ((v >= '0' && v <= '9') || (v >= 0xFF10 && v <= 0xFF19)) {
      size_t d = 0;
      while (i + d < to && ((cp[i + d] >= '0' && cp[i + d] <= '9') ||
                            (cp[i + d] >= 0xFF10 && cp[i + d] <= 0xFF19))) d++;
      units += 0.15 + 0.9 * (double)d;
      i += d;
      continue;
    }

    /* Sentence punctuation is a pause, list punctuation a shorter one.  A run
       of terminators ("?!", "...") counts as one pause. */
    if (v == '.' || v == '!' || v == '?' || v == 0x2026 || v == 0x3002 ||
        v == 0xFF01 || v == 0xFF1F || v == 0x061F || v == 0x061B) {
      do { i++; } while (i < to &&
                         (cp[i] == '.' || cp[i] == '!' || cp[i] == '?' || cp[i] == 0x2026 ||
                          cp[i] == 0x3002 || cp[i] == 0xFF01 || cp[i] == 0xFF1F ||
                          cp[i] == 0x061F || cp[i] == 0x061B));
      units += 0.8;
      continue;
    }
    if (v == ',' || v == ';' || v == ':' || v == 0xFF0C || v == 0x3001 || v == 0x060C) {
      units += 0.4;
      i++;
      continue;
    }

    if (cp_is_cjk(v)) {           /* one character, one syllable */
      units += 1.0;
      i++;
      continue;
    }

    if (cp_is_letter(v)) {
      size_t a = i;
      while (i < to && cp_is_letter(cp[i])) i++;
      size_t len = i - a;
      if (cp_is_arabic(cp[a])) {
        units += (double)len / 2.2;          /* Arabic words are long */
      } else if (cp_is_cjk(cp[a])) {
        units += (double)len;
      } else {
        units += (double)latin_syllables(cp, a, i);
      }
      continue;
    }

    i++;                                     /* anything else: no time */
  }

  return units;
}

/* --- pauses in the narration audio (where a caption may change) ---------- */

typedef struct {
  double *v;        /* midpoint of each silence, in order */
  double *len;      /* how long that silence lasted */
  size_t  n, cap;
  double  pending_start;
  bool    pending;
} PauseScan;

static void pause_scan_line(const char *line, void *user) {
  PauseScan *ps = (PauseScan *)user;
  if (!line || !ps) return;

  const char *s = strstr(line, "silence_start:");
  const char *e = strstr(line, "silence_end:");
  if (s) {
    ps->pending_start = atof(s + strlen("silence_start:"));
    ps->pending = true;
  } else if (e && ps->pending) {
    double end = atof(e + strlen("silence_end:"));
    double len = end - ps->pending_start;
    double mid = (ps->pending_start + end) / 2.0;
    if (len >= 0.08 && len <= 4.0 && mid > 0.05) {
      if (ps->n + 1 > ps->cap) {
        ps->cap = ps->cap ? ps->cap * 2 : 32;
        double *nv = (double *)realloc(ps->v, ps->cap * sizeof(double));
        if (!nv) die("OOM");
        ps->v = nv;
        double *nl = (double *)realloc(ps->len, ps->cap * sizeof(double));
        if (!nl) die("OOM");
        ps->len = nl;
      }
      ps->v[ps->n]   = mid;
      ps->len[ps->n] = len;
      ps->n++;
    }
    ps->pending = false;
  }
}

/* Midpoints (seconds) of the silences inside the narration audio, in order, and
 * how long each of them lasted.  Returns the count (0 when ffmpeg is missing or
 * the audio has no pauses). */
static size_t narration_pauses(const char *mp3, double **mids, double **lens) {
  *mids = NULL;
  *lens = NULL;
  if (!mp3 || !mp3[0] || !file_exists(mp3)) return 0;

  PauseScan ps = {0};
  char *esc = sh_escape(mp3);
  char cmd[PATH_MAX + 256];
  /* -35 dB / 80 ms: any real pause between sentences, nothing inside a word. */
  snprintf(cmd, sizeof(cmd),
           "ffmpeg -hide_banner -nostdin -i %s -af "
           "\"silencedetect=noise=-35dB:d=0.08\" -f null -", esc);
  free(esc);
  plat_run(cmd, pause_scan_line, &ps);

  *mids = ps.v;
  *lens = ps.len;
  return ps.n;
}

/* The audio the captions have to follow: where the voice starts and stops
 * (the lead-in / tail of a TTS file is not speech) plus the silences inside it. */
typedef struct {
  const double *pauses;   /* midpoint of each silence, in order */
  const double *plens;    /* how long that silence lasted */
  size_t        npauses;
  double        onset;    /* first moment the voice is heard */
  double        stop;     /* last moment the voice is heard */
} CaptionAudio;

/* Work out the spoken part of the narration from its silences: a silence that
 * starts at 0 is the engine's lead-in, one that reaches the end is the tail.
 * Captions have to follow the voice, not the file - otherwise every caption of
 * the clip sits early by the length of that lead-in and the last one hangs
 * there through the tail. */
static CaptionAudio caption_audio_from(const double *mids, const double *lens,
                                       size_t np, double dur) {
  CaptionAudio au;
  au.pauses = mids;
  au.plens  = lens;
  au.npauses = np;
  au.onset = 0.0;
  au.stop  = dur;
  if (np == 0) return au;
  double head = lens[0] * 0.5, tail = lens[np - 1] * 0.5;
  if (mids[0] - head <= 0.05) au.onset = mids[0] + head;
  if (mids[np - 1] + tail >= dur - 0.05) au.stop = mids[np - 1] - tail;
  if (au.stop <= au.onset + 0.30) { au.onset = 0.0; au.stop = dur; }
  return au;
}

/* Put the estimated sentence boundaries onto the real pauses of the narration.
 *
 * A greedy "snap it when a pause is close enough" misses whenever the estimate
 * has drifted: a boundary that is two seconds out never sees its pause inside
 * the tolerance window, and stays wrong.  So the boundaries and the pauses are
 * aligned as two ordered sequences with a small dynamic program - skip a pause,
 * skip a boundary, or match the two - scoring a match by how far the pause is
 * from the estimate minus a bonus for how long the silence lasted (a sentence
 * end is a longer stop than a comma).  Every boundary that was not matched is
 * then re-spread inside the interval between its two matched neighbours in
 * proportion to its speech weight, so a drift can never survive past the next
 * matched pause. */
static void align_boundaries_to_pauses(double *bound, const double *w_cum, int nseg,
                                       const CaptionAudio *au) {
  const int nb = nseg - 1;                  /* boundaries between chunks */
  bound[nseg] = au->stop;
  if (nb <= 0 || au->npauses == 0) return;

  /* Candidate change points: the moment the voice stops before a silence, so a
     caption is never taken away while its line is still being spoken.  The one
     exception is a very long pause: there the change point is pulled to at most
     CHASE seconds before the next line starts, so the next caption does not sit
     on screen long before its words are said.  Only silences strictly inside the
     spoken part can carry a sentence end. */
  const double CHASE = 0.50;
  int *pidx = (int *)malloc(au->npauses * sizeof(int));
  double *ppos = (double *)malloc(au->npauses * sizeof(double));
  if (!pidx || !ppos) die("OOM");
  size_t ne = 0;
  for (size_t k = 0; k < au->npauses; k++) {
    double len   = au->plens ? au->plens[k] : 0.0;
    double start = au->pauses[k] - len * 0.5;
    double end   = au->pauses[k] + len * 0.5;
    double p     = start;
    if (end - CHASE > p) p = end - CHASE;
    if (p > au->onset + 0.05 && p < au->stop - 0.05) { pidx[ne] = (int)k; ppos[ne] = p; ne++; }
  }
  if (ne == 0) { free(pidx); free(ppos); return; }

  /* Weights of the two "give up" moves.  Skipping a boundary has to cost more
     than matching a pause that is up to ~1.8 s away, otherwise a boundary that
     merely drifted is left as an estimate instead of being put right - which is
     how the drift used to accumulate over a whole clip. */
  const double SKIP_B   = 1.50;   /* not every sentence end is a detected stop */
  const double BONUS    = 0.50;   /* ... and a long stop is likely a sentence end */
  const double MAXJUMP  = 3.50;   /* beyond this it is not the same moment */
  const double INF      = 1e30;

  size_t cols = ne + 1;
  double *dp = (double *)malloc((size_t)(nb + 1) * cols * sizeof(double));
  unsigned char *ch = (unsigned char *)malloc((size_t)(nb + 1) * cols);
  if (!dp || !ch) die("OOM");
  for (size_t q = 0; q < (size_t)(nb + 1) * cols; q++) { dp[q] = INF; ch[q] = 0; }
  dp[0] = 0.0;

  for (int i = 0; i <= nb; i++) {
    for (size_t k = 0; k <= ne; k++) {
      double cur = dp[(size_t)i * cols + k];
      if (cur >= INF) continue;

      if (i < nb) {                                   /* leave boundary i estimated */
        double *t = &dp[(size_t)(i + 1) * cols + k];
        if (cur + SKIP_B < *t) { *t = cur + SKIP_B; ch[(size_t)(i + 1) * cols + k] = 1; }
      }
      if (k < ne) {                                   /* leave pause k unused */
        double *t = &dp[(size_t)i * cols + (k + 1)];
        if (cur < *t) { *t = cur; ch[(size_t)i * cols + (k + 1)] = 2; }
      }
      if (i < nb && k < ne) {                         /* boundary i+1 <-> pause k */
        size_t pk = (size_t)pidx[k];
        double p   = ppos[k];
        double d   = fabs(p - bound[i + 1]);
        if (d <= MAXJUMP) {
          double len = au->plens ? au->plens[pk] : 0.0;
          if (len > 0.6) len = 0.6;
          double *t = &dp[(size_t)(i + 1) * cols + (k + 1)];
          if (cur + d - BONUS * len < *t) {
            *t = cur + d - BONUS * len;
            ch[(size_t)(i + 1) * cols + (k + 1)] = 3;
          }
        }
      }
    }
  }

  /* walk back: every cell saying "match" is a boundary sitting on a real stop */
  bool is_anchor[CAP_MAX_SEG + 1];
  for (int g = 0; g <= nseg; g++) is_anchor[g] = false;
  int matched = 0, i = nb;
  size_t k = ne;
  while (i > 0 || k > 0) {
    unsigned char c = ch[(size_t)i * cols + k];
    if (c == 3) { bound[i] = ppos[k - 1]; is_anchor[i] = true; matched++; i--; k--; }
    else if (c == 1) { i--; }
    else if (c == 2) { k--; }
    else break;
  }
  free(dp); free(ch); free(pidx); free(ppos);

  /* re-spread the unmatched boundaries between their neighbouring anchors */
  int    prev_g = 0;
  double prev_t = au->onset;
  for (int g = 1; g <= nseg; g++) {
    bool anchor = (g == nseg) || is_anchor[g];
    if (!anchor) continue;
    double t = (g == nseg) ? au->stop : bound[g];
    if (g > prev_g + 1 && w_cum) {
      double w0 = w_cum[prev_g], w1 = w_cum[g];
      if (w1 > w0 + 1e-6) {
        for (int q = prev_g + 1; q < g; q++)
          bound[q] = prev_t + (t - prev_t) * (w_cum[q] - w0) / (w1 - w0);
      }
    }
    prev_g = g;
    prev_t = t;
  }

  /* captions stay in order, on screen long enough, and inside the voice */
  for (int g = 1; g <= nseg; g++) {
    double limit = au->stop - 0.20 * (double)(nseg - g);
    if (limit < bound[g - 1] + 0.05) limit = bound[g - 1] + 0.05;
    if (bound[g] < bound[g - 1] + 0.25) bound[g] = bound[g - 1] + 0.25;
    if (bound[g] > limit) bound[g] = limit;
  }
  bound[nseg] = au->stop;

  if (matched)
    logi("Caption timing: %d of %d sentence boundaries aligned to the pauses in the "
         "narration (voice %.2f-%.2f s).", matched, nb, au->onset, au->stop);
  else if (au->npauses)
    logw("Found %zu pause(s) in the narration but none lined up with a sentence end - "
         "caption timing stays estimated for this clip.", au->npauses);
}

static char *caption_filter_chain(const char *text, const char *font, double dur,
                                  const CaptionAudio *au, int frame_w, int frame_h) {
  if (!text || !text[0] || dur <= 0.1) return NULL;

  /* Clean: curly apostrophe for ', space for " \\ and newlines. */
  size_t cap = strlen(text) * 3 + 8;
  char *clean = (char *)malloc(cap);
  if (!clean) die("OOM");
  size_t o = 0;
  for (const char *t = text; *t && o + 5 < cap; t++) {
    char c = *t;
    if (c == '\'') { clean[o++] = (char)0xE2; clean[o++] = (char)0x80; clean[o++] = (char)0x99; continue; }
    if (c == '"' || c == '\\' || c == '\n' || c == '\r') { clean[o++] = ' '; continue; }
    clean[o++] = c;
  }
  clean[o] = '\0';

  /* Decode to code points with byte offsets. */
  size_t maxr = strlen(clean) + 1;
  unsigned *cp = (unsigned *)malloc(maxr * sizeof(unsigned));
  unsigned char *cl = (unsigned char *)malloc(maxr);
  size_t *boff = (size_t *)malloc((maxr + 1) * sizeof(size_t));
  if (!cp || !cl || !boff) die("OOM");
  size_t nr = 0;
  boff[0] = 0;
  for (size_t k = 0; clean[k]; ) {
    unsigned char c = (unsigned char)clean[k];
    unsigned v = c; size_t l = 1;
    if (c >= 0xF0 && clean[k+1] && clean[k+2] && clean[k+3]) {
      v = (((unsigned)(c & 0x07) << 18) | (((unsigned char)clean[k+1] & 0x3F) << 12) |
           (((unsigned char)clean[k+2] & 0x3F) << 6) | ((unsigned char)clean[k+3] & 0x3F));
      l = 4;
    } else if (c >= 0xE0 && clean[k+1] && clean[k+2]) {
      v = (((unsigned)(c & 0x0F) << 12) | (((unsigned char)clean[k+1] & 0x3F) << 6) |
           ((unsigned char)clean[k+2] & 0x3F));
      l = 3;
    } else if (c >= 0xC0 && clean[k+1]) {
      v = (((unsigned)(c & 0x1F) << 6) | ((unsigned char)clean[k+1] & 0x3F));
      l = 2;
    }
    cp[nr] = v; cl[nr] = (unsigned char)l; nr++;
    k += l; boff[nr] = k;
  }
  if (nr == 0) { free(clean); free(cp); free(cl); free(boff); return NULL; }

  /* Split into sentence chunks (Latin, CJK and Arabic terminators).  One chunk
     holds at most two lines, and how many characters fit on a line depends on
     the script and the frame - see caption_line_cap. */
  const int line_cap = caption_line_cap(cp, nr, frame_w, frame_h);
  const size_t seg_cap = (size_t)line_cap * 2;

  size_t seg_s[CAP_MAX_SEG], seg_e[CAP_MAX_SEG];
  int nseg = 0;
  size_t cur = 0;
  while (cur < nr && nseg < CAP_MAX_SEG) {
    while (cur < nr && cp[cur] == ' ') cur++;
    if (cur >= nr) break;
    size_t j = cur, soft = 0;
    while (j < nr && j - cur < seg_cap) {
      unsigned v = cp[j];
      bool term = (v == '.' || v == '!' || v == '?' || v == 0x3002u ||
                   v == 0xFF01u || v == 0xFF1Fu || v == 0x061Fu || v == 0x061Bu);
      bool softp = (v == ',' || v == ';' || v == ':' || v == ' ' ||
                    v == 0xFF0Cu || v == 0x3001u || v == 0x060Cu);
      /* Break before a space (the space belongs to neither line) but after a
         comma or colon, which stay with the words they follow. */
      if (v == ' ') soft = j;
      else if (softp) soft = j + 1;
      j++;
      if (term && j - cur >= 16) break;
    }
    if (j >= nr) j = nr;
    else if (j - cur >= seg_cap && soft > cur + 8) j = soft;
    seg_s[nseg] = cur; seg_e[nseg] = j; nseg++;
    cur = j;
  }
  if (nseg == 0) { free(clean); free(cp); free(cl); free(boff); return NULL; }

  /* Wrap each chunk into <= 2 lines of <= line_cap runes (break on a space when
     there is one, never in the middle of a word unless the word is too long). */
  size_t ls[CAP_MAX_LINE], le[CAP_MAX_LINE], lseg[CAP_MAX_LINE];
  int lrow[CAP_MAX_LINE];       /* which stacked slot the line was drawn in */
  int nline = 0;
  for (int g = 0; g < nseg && nline + 2 <= CAP_MAX_LINE; g++) {
    size_t w0 = seg_s[g], we = seg_e[g];
    int nl = 0;
    while (we > w0 && nl < 2) {
      size_t take = we - w0;
      if (take > (size_t)line_cap) {
        take = (size_t)line_cap;
        size_t t2 = take;
        while (t2 > (size_t)line_cap / 2 && cp[w0 + t2] != ' ') t2--;
        if (t2 > (size_t)line_cap / 2) take = t2;
      }
      ls[nline] = w0; le[nline] = w0 + take; lseg[nline] = (size_t)g; nline++;
      w0 += take;
      while (w0 < we && cp[w0] == ' ') w0++;
      nl++;
    }
  }

  /* Show each chunk while it is being spoken: window proportional to the
     estimated speech time, then snapped onto the real pauses in the audio. */
  double seg_w[CAP_MAX_SEG];
  double total_w = 0;
  for (int g = 0; g < nseg; g++) {
    double w = speech_units(cp, seg_s[g], seg_e[g]);
    if (w < 1.0) w = 1.0;
    seg_w[g] = w;
    total_w += w;
  }
  if (total_w <= 0) total_w = 1;

  /* Estimated share of the spoken time per chunk: speech units, spread over the
     part of the file that actually contains speech. */
  double w_cum[CAP_MAX_SEG + 1];
  double bound[CAP_MAX_SEG + 1];
  {
    double acc = 0;
    w_cum[0] = 0;
    bound[0] = au->onset;
    for (int g = 0; g < nseg; g++) {
      acc += seg_w[g];
      w_cum[g + 1] = acc;
      bound[g + 1] = au->onset + (au->stop - au->onset) * acc / total_w;
    }
    bound[0] = au->onset;
    align_boundaries_to_pauses(bound, w_cum, nseg, au);
    bound[0] = au->onset;
  }

  char font_esc[300];
  size_t fo = 0;
  for (const char *q = font; *q && fo + 2 < sizeof(font_esc); q++) {
    if (*q == '"' || *q == '\'') { font_esc[fo++] = ' '; continue; }
    if (*q == '\\') { font_esc[fo++] = '/'; continue; }
    if (*q == ':') font_esc[fo++] = '\\';
    font_esc[fo++] = *q;
  }
  font_esc[fo] = '\0';

  size_t chain_cap = (size_t)nline * 400 + 1024;
  char *chain = (char *)malloc(chain_cap);
  if (!chain) die("OOM");
  chain[0] = '\0';
  size_t off = 0;
  for (int g = 0; g < nseg; g++) {
    double t0 = bound[g];
    double t1 = (g == nseg - 1) ? au->stop : bound[g + 1];

    /* How many lines does this chunk use?  The FIRST line goes on top: ffmpeg
       stacks drawtext filters upward, and numbering them the other way round
       put the second line of a sentence above the first - so the viewer read
       the end of a sentence before its beginning ("it shows the next sentence
       before the present one"). */
    int lines_here = 0;
    for (int pj = 0; pj < nline; pj++) if (lseg[pj] == (size_t)g) lines_here++;

    for (int li = 0; li < nline; li++) {
      if (lseg[li] != (size_t)g) continue;
      int stack = 0;
      for (int pj = 0; pj < li; pj++) if (lseg[pj] == lseg[li]) stack++;
      int row = lines_here - 1 - stack;      /* 0 = bottom line of the chunk */
      char line_txt[600];
      size_t b0 = boff[ls[li]], b1 = boff[le[li]];
      size_t eo = 0;
      for (size_t q = b0; q < b1 && eo + 2 < sizeof(line_txt); q++) {
        if (clean[q] == ':') line_txt[eo++] = '\\';
        line_txt[eo++] = clean[q];
      }
      line_txt[eo] = '\0';
      lrow[li] = row;
      int w = snprintf(chain + off, chain_cap - off,
                       ",drawtext=fontfile='%s':expansion=none:"
                       "text='%s':fontcolor=white:borderw=2:bordercolor=black:"
                       "fontsize=h*0.035:x=(w-text_w)/2:y=h-h*0.07-th-%d*h*0.045:"
                       "enable='between(t,%.2f,%.2f)'",
                       font_esc, line_txt, row, t0, t1);
      if (w < 0 || (size_t)w >= chain_cap - off) goto cap_done;
      off += (size_t)w;
    }
  }
cap_done:

  /* One log line per clip so caption problems can be spotted (and tested) from
     the log instead of from the finished video: the chunks in the order they
     appear, each with the seconds it is on screen and its first words. */
  {
    char line[900];
    size_t o = 0;
    o += (size_t)snprintf(line + o, sizeof(line) - o, "Captions: %d chunk(s)", nseg);
    for (int g = 0; g < nseg && g < 5 && o + 140 < sizeof(line); g++) {
      size_t ra = seg_s[g], rb = seg_e[g];
      if (rb - ra > 26) rb = ra + 26;
      char seg_txt[128];
      size_t so = 0;
      for (size_t q = boff[ra]; q < boff[rb] && so + 1 < sizeof(seg_txt); q++) {
        char ch = clean[q];
        if (ch == '"' || ch == '\\' || ch == '\n' || ch == '\r') ch = ' ';
        seg_txt[so++] = ch;
      }
      seg_txt[so] = '\0';
      o += (size_t)snprintf(line + o, sizeof(line) - o, " | %.2f-%.2fs rows[",
                            bound[g], (g == nseg - 1) ? au->stop : bound[g + 1]);
      bool first_row = true;
      for (int li = 0; li < nline && o + 8 < sizeof(line); li++) {
        if (lseg[li] != (size_t)g) continue;
        o += (size_t)snprintf(line + o, sizeof(line) - o, "%s%d", first_row ? "" : ",", lrow[li]);
        first_row = false;
      }
      o += (size_t)snprintf(line + o, sizeof(line) - o, "] \"%s%s\"",
                            seg_txt, (seg_e[g] - seg_s[g] > 26) ? "..." : "");
    }
    if (nseg > 5) o += (size_t)snprintf(line + o, sizeof(line) - o, " | +%d more", nseg - 5);
    logi("%s", line);
  }
  free(clean); free(cp); free(cl); free(boff);
  return chain;
}

static bool ffmpeg_make_adjusted_clip(const Config *cfg, const char *input_mp4,
                                      int start_s, int end_s,
                                      const char *narration_mp3, double narration_dur,
                                      const char *out_mp4, const char *caption,
                                      int frame_w, int frame_h) {
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

  char *cap_esc = NULL;
  if (cfg->captions && caption && caption[0] && caption_font_available(cfg->caption_font)) {
    double *pauses = NULL, *plens = NULL;
    size_t npauses = narration_pauses(narration_mp3, &pauses, &plens);
    CaptionAudio au = caption_audio_from(pauses, plens, npauses, narration_dur);
    if (npauses == 0)
      logw("No pauses could be found in the narration of this clip - caption timing "
           "stays estimated (is the portable ffmpeg complete?).");
    cap_esc = caption_filter_chain(caption, cfg->caption_font, narration_dur, &au,
                                   frame_w, frame_h);
    free(pauses);
    free(plens);
  }

  int rc;
  if (cap_esc) {
    rc = run_cmd(
      "ffmpeg -y -hide_banner -loglevel error "
      "-ss %d -to %d -i %s "
      "-i %s "
      "-filter_complex \"[0:v]setpts=PTS/%.10f%s[v]\" "
      "-map \"[v]\" -map 1:a "
      "-c:v libx264 -pix_fmt yuv420p -preset veryfast -crf 22 "
      "-c:a aac -b:a 192k "
      "-shortest %s",
      use_start, use_end, in_esc, nar_esc, speed, cap_esc, out_esc
    );
  } else {
    rc = run_cmd(
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
  }

  free(cap_esc);
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

/* ---------------------------------------------------------------------------
 * Free / offline fallbacks.
 *
 * Some movies have no downloadable subtitles anywhere, and some machines have
 * no working OpenAI key.  Instead of dead-ending, synthesize an evenly spaced
 * subtitle track and/or build a clip plan locally from whatever cues exist,
 * so the pipeline can finish with nothing but FFmpeg + Piper.
 * ------------------------------------------------------------------------ */

static void srt_write_cue(FILE *f, int idx, int start_s, int end_s, const char *text) {
  fprintf(f, "%d\n%02d:%02d:%02d,000 --> %02d:%02d:%02d,000\n%s\n\n",
          idx,
          start_s / 3600, (start_s / 60) % 60, start_s % 60,
          end_s / 3600, (end_s / 60) % 60, end_s % 60,
          text);
}

/* The per-clip seconds range the prompt asks for.  Kept in one place so the
 * pipeline can talk about the same numbers the model was given. */
static void clip_seconds_range(int per_clip_sec, int *min_sec, int *max_sec) {
  if (per_clip_sec >= 20) {
    *min_sec = per_clip_sec * 8 / 10;
    *max_sec = per_clip_sec * 12 / 10;
  } else {
    *min_sec = 8;
    *max_sec = 16;
  }
  if (*min_sec < 6) *min_sec = 6;
  if (*max_sec < *min_sec + 3) *max_sec = *min_sec + 3;
}

/* How much speech the narration of a plan adds up to.  The prompt tells the
 * model to write 2.6 words per second of clip (see STEP 4); Chinese is counted
 * in characters (about 4 per second), Arabic is spoken a little slower. */
static double lang_speech_units_per_sec(const char *code) {
  if (!strcmp(code, "zh")) return 4.0;      /* characters per second */
  if (!strcmp(code, "ja")) return 4.5;
  if (!strcmp(code, "ko")) return 4.0;
  if (!strcmp(code, "th")) return 4.0;
  if (!strcmp(code, "ar")) return 2.1;      /* words per second */
  return 2.6;
}

/* Languages that are not written with spaces between words are measured in
 * characters, not words. */
static bool lang_counts_chars(const char *code) {
  return !strcmp(code, "zh") || !strcmp(code, "ja") ||
         !strcmp(code, "ko") || !strcmp(code, "th");
}

/* Words for a space-separated language, characters for Chinese/Japanese/Korean/
 * Thai. */
static double count_speech_units(const char *text, const char *code) {
  if (!text) return 0.0;
  if (!lang_counts_chars(code)) {
    double words = 0.0;
    bool in_word = false;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
      bool space = (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r');
      if (space) in_word = false;
      else if (!in_word) { in_word = true; words += 1.0; }
    }
    return words;
  }
  double chars = 0.0;
  for (const unsigned char *p = (const unsigned char *)text; *p; ) {
    unsigned v = 0; size_t l = 1;
    if (*p < 0x80) v = *p;
    else if ((*p & 0xE0) == 0xC0 && p[1]) { v = ((unsigned)(*p & 0x1F) << 6) | (p[1] & 0x3F); l = 2; }
    else if ((*p & 0xF0) == 0xE0 && p[1] && p[2]) {
      v = ((unsigned)(*p & 0x0F) << 12) | ((unsigned)(p[1] & 0x3F) << 6) | (p[2] & 0x3F); l = 3;
    } else if ((*p & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) { v = 0x10000u; l = 4; }
    /* CJK ideographs, kana and Hangul count as one character each; CJK
       punctuation and Latin letters do not count (Latin words are not what the
       Chinese narration is measured in anyway). */
    if ((v >= 0x2E80 && v <= 0x9FFF && !(v >= 0x3000 && v <= 0x303F)) ||
        (v >= 0xAC00 && v <= 0xD7AF) || (v >= 0x0E00 && v <= 0x0E7F)) chars += 1.0;
    p += l;
  }
  return chars;
}

/* Seconds of spoken narration in the whole plan. */
static double plan_speech_seconds(const ClipPlan *items, size_t n, const char *code) {
  double units = 0.0;
  for (size_t i = 0; i < n; i++) units += count_speech_units(items[i].narration, code);
  double per_sec = lang_speech_units_per_sec(code);
  return per_sec > 0.0 ? units / per_sec : 0.0;
}

/* Find a user-provided SRT whose name approximately matches the movie title.
 * Names are normalized to lowercase alphanumerics, so "Toy Story 5 (2026)
 * [1080p].mp4" matches "Toy Story 5.srt". Files ending in _modified.srt or
 * _placeholder.srt are ignored. Returns false when nothing plausible exists. */
static bool find_subtitle_srt(const char *movie_title, char *out, size_t outsz, const char *code) {
  size_t n = 0;
  char **files = list_files_with_ext("scripts/srt_files", ".srt", NULL, &n);
  if (!files) return false;

  char want[256];
  size_t wo = 0;
  for (const char *p = movie_title; *p && wo + 1 < sizeof(want); p++) {
    char c = *p;
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) want[wo++] = c;
  }
  want[wo] = '\0';

  bool found = false;
  int best_score = 0;
  for (size_t i = 0; i < n; i++) {
    const char *full = files[i];
    const char *base = strrchr(full, '/');
    base = base ? base + 1 : full;
    size_t bl = strlen(base);
    if (bl > 12 && str_icmp(base + bl - 12, "_modified.srt") == 0) continue;
    if (bl > 15 && str_icmp(base + bl - 15, "_placeholder.srt") == 0) continue;

    /* Language-tagged files: two letter tags are languages, so "Toy Story.fr.srt"
       belongs to a French run, "Toy Story.en.cc.srt" is an English file and
       "Toy Story.zh.srt" is not English.  Strip the tags and keep the file only
       when the language that was asked for is among them. */
    size_t name_end = bl - 4; /* without ".srt" */
    bool wanted = false, other_lang = false;
    while (name_end >= 3 && base[name_end - 3] == '.') {
      char c0 = base[name_end - 2], c1 = base[name_end - 1];
      if (!isalpha((unsigned char)c0) || !isalpha((unsigned char)c1)) break;
      char tag[3];
      tag[0] = (char)tolower((unsigned char)c0);
      tag[1] = (char)tolower((unsigned char)c1);
      tag[2] = 0;
      bool is_lang = lang_tag_is_known(tag);
      if (!is_lang && strcmp(tag, "cc") != 0) break;   /* ".sdh", ".part1", ... */
      if (is_lang) {
        if (!strcmp(tag, code)) wanted = true;
        else other_lang = true;
      }
      name_end -= 3;
    }
    if (other_lang && !wanted) continue;

    char cand[256];
    size_t co = 0;
    for (size_t k = 0; base[k] && k < name_end && co + 1 < sizeof(cand); k++) {
      char c = base[k];
      if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
      if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) cand[co++] = c;
    }
    cand[co] = '\0';
    if (co < 3 || wo < 3) continue;

    int score = 0;
    int ldiff = (int)co - (int)wo;
    if (ldiff < 0) ldiff = -ldiff;
    if (strcmp(cand, want) == 0) score = 10000;
    else if (strncmp(cand, want, wo < co ? wo : co) == 0) score = 5000 - ldiff;
    else if (strstr(cand, want) || strstr(want, cand)) score = 1000 - ldiff;
    if (score > best_score) {
      best_score = score;
      snprintf(out, outsz, "scripts/srt_files/%s", base);
      found = true;
    }
  }
  free_str_list(files, n);
  return found;
}

static bool make_fallback_srt(const char *movie_path, const char *movie_title,
                              const char *dest_srt_path, int num_clips) {
  double dur = ffprobe_duration_seconds(movie_path);
  if (dur < 40.0) {
    logw("Movie is too short (%.0fs) to build a fallback subtitle track.", dur);
    return false;
  }
  if (num_clips < 2) num_clips = 2;
  if (num_clips > 60) num_clips = 60;

  static const char *generic_lines[] = {
    "The story wastes no time and throws our characters straight into trouble.",
    "Right here the stakes get raised, and nobody walks away unchanged.",
    "This moment quietly changes everything our heroes thought they knew.",
    "Tensions boil over as old allies turn into brand new enemies.",
    "A clever twist flips the whole plan completely on its head.",
    "Our hero digs deep and finds one last burst of courage.",
    "Everything collides at once in this unforgettable showdown.",
    "The dust settles for a moment and the story catches its breath.",
    "Secrets surface, and the real villain finally shows a face.",
    "What looked like a dead end turns into the way forward.",
    "Friendship is tested, and loyalty earns its keep.",
    "The chase is on, and there is no turning back now.",
  };
  const size_t n_generic = sizeof(generic_lines) / sizeof(generic_lines[0]);

  FILE *f = plat_fopen(dest_srt_path, "wb");
  if (!f) return false;

  double usable = dur - 30.0;
  double seg = usable / (double)num_clips;

  char line[1024];
  for (int i = 0; i < num_clips; i++) {
    int start_s = (int)(15.0 + (double)i * seg);
    int end_s = start_s + 12;
    if (end_s > (int)dur - 5) end_s = (int)dur - 5;
    if (end_s <= start_s) end_s = start_s + 4;

    if (i == 0) {
      snprintf(line, sizeof(line),
               "The story begins in %s. "
               "Today we cover the whole story from start to finish.", movie_title);
    } else if (i == num_clips - 1) {
      snprintf(line, sizeof(line),
               "And that is how %s wraps up. Thanks for watching, "
               "and see you in the next one.", movie_title);
    } else {
      snprintf(line, sizeof(line), "%s", generic_lines[(size_t)(i - 1) % n_generic]);
    }
    srt_write_cue(f, i + 1, start_s, end_s, line);
  }

  fclose(f);
  return file_exists(dest_srt_path);
}

typedef struct {
  int   start;
  int   end;
  char *text;
} LocalCue;

static void local_cue_push(LocalCue **arr, size_t *n, size_t *cap, int st, int en, char *text) {
  if (*n + 1 > *cap) {
    *cap = *cap ? *cap * 2 : 32;
    *arr = (LocalCue *)realloc(*arr, *cap * sizeof(LocalCue));
    if (!*arr) die("OOM");
  }
  (*arr)[*n].start = st;
  (*arr)[*n].end   = en;
  (*arr)[*n].text  = text;
  (*n)++;
}

static char *trim_ws(char *t) {
  while (*t == ' ' || *t == '\t') t++;
  char *end = t + strlen(t);
  while (end > t && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) *--end = '\0';
  return t;
}

/* Build a clip plan without any API: sample the subtitle cues evenly across
 * the file and use the cue text itself as the narration. */
static ClipPlanList local_make_plan(const char *subs_seconds_text, int num_clips, int per_clip_sec) {
  ClipPlanList out = {0};
  if (!subs_seconds_text || !*subs_seconds_text || num_clips <= 0) return out;

  char *work = str_dup(subs_seconds_text);
  if (!work) return out;

  LocalCue *cues = NULL;
  size_t n = 0, cap = 0;

  int  pend_s = 0, pend_e = 0;
  bool pend = false;
  char *pend_text = NULL;

  char *cur = work;
  while (cur) {
    char *nl = strchr(cur, '\n');
    if (nl) *nl = '\0';
    char *ln = trim_ws(cur);
    cur = nl ? nl + 1 : NULL;

    int s1 = 0, s2 = 0;
    if (sscanf(ln, "%d --> %d", &s1, &s2) == 2) {
      if (pend && pend_text) local_cue_push(&cues, &n, &cap, pend_s, pend_e, pend_text);
      else if (pend_text) free(pend_text);
      pend = true; pend_s = s1; pend_e = s2; pend_text = NULL;
      continue;
    }

    if (*ln == '\0') {
      if (pend && pend_text) local_cue_push(&cues, &n, &cap, pend_s, pend_e, pend_text);
      else if (pend_text) free(pend_text);
      pend = false; pend_text = NULL;
      continue;
    }

    /* Every text line belongs to the open cue.  A cue number is a digits line
       that arrives while no cue is open (the previous one was closed by the
       blank line), so it is skipped by the "pend" test - and a line that is
       only digits ("1944", "42") stays dialogue instead of being dropped. */
    if (pend) {
      if (pend_text) {
        size_t a = strlen(pend_text), b = strlen(ln);
        char *m = (char *)realloc(pend_text, a + b + 2);
        if (!m) die("OOM");
        m[a] = ' ';
        memcpy(m + a + 1, ln, b + 1);
        pend_text = m;
      } else {
        pend_text = str_dup(ln);
      }
    }
  }
  if (pend && pend_text) local_cue_push(&cues, &n, &cap, pend_s, pend_e, pend_text);
  else if (pend_text) free(pend_text);
  free(work);

  if (n == 0) { free(cues); return out; }

  int want = num_clips;
  if ((size_t)want > n) want = (int)n;

  out.items = (ClipPlan *)calloc((size_t)want, sizeof(ClipPlan));
  if (!out.items) die("OOM");
  out.count = 0;

  if (per_clip_sec > 0) {
    /* Long-recap mode: each clip covers ~per_clip_sec of the timeline and its
       narration is ALL the dialogue spoken in that stretch, so the spoken
       audio actually fills the clip (a single cue would be 1-3 s of speech
       and the speed cap would collapse the clip to match). */
    int last_time = cues[n - 1].end;
    if (last_time <= 0) last_time = (int)n * 10;
    size_t max_chars = (size_t)per_clip_sec * 15 + 60;
    if (max_chars > 900) max_chars = 900;
    size_t next_min = 0;
    for (int i = 0; i < want && next_min < n; i++) {
      double target = ((double)i + 0.5) * (double)last_time / (double)want;
      size_t idx0 = next_min;
      while (idx0 + 1 < n && (double)cues[idx0].start < target) idx0++;
      if ((double)cues[idx0].start > target && idx0 > next_min) idx0--;

      size_t k = idx0;
      size_t len = 0;
      while (k < n && (cues[k].start - cues[idx0].start) < per_clip_sec && len < max_chars) {
        len += strlen(cues[k].text);
        k++;
      }
      if (k == idx0) k = idx0 + 1;

      size_t total = (k - idx0);
      for (size_t j = idx0; j < k; j++) total += strlen(cues[j].text);
      char *txt = (char *)malloc(total + 2);
      if (!txt) die("OOM");
      size_t o = 0;
      for (size_t j = idx0; j < k; j++) {
        size_t l = strlen(cues[j].text);
        if (o) txt[o++] = ' ';
        memcpy(txt + o, cues[j].text, l);
        o += l;
      }
      txt[o] = '\0';

      int st = cues[idx0].start;
      int en = cues[k - 1].end;
      if (st <= 0) st = 1;
      if (en <= st) en = st + 8;
      if (en - st > per_clip_sec + 15) en = st + per_clip_sec + 15;

      out.items[out.count].start = st;
      out.items[out.count].end   = en;
      out.items[out.count].narration = txt;
      out.count++;
      next_min = k;
    }
  } else {
    int last_idx = -1;
    for (int i = 0; i < want; i++) {
      int idx = (want == 1) ? 0
                : (int)(((long long)i * (long long)(n - 1)) / (long long)(want - 1));
      if (idx == last_idx) continue;
      last_idx = idx;

      int st = cues[idx].start;
      int en = cues[idx].end;
      if (st <= 0) st = 1;
      if (en <= st) en = st + 10;
      if (en - st > 20) en = st + 16;

      const char *src = cues[idx].text;
      if (!src || !*src) src = "The story keeps moving, and the best is yet to come.";
      char narbuf[512];
      size_t tl = strlen(src);
      if (tl >= sizeof(narbuf)) {
        tl = sizeof(narbuf) - 1;
        while (tl > 200 && src[tl] != ' ') tl--;
      }
      memcpy(narbuf, src, tl);
      narbuf[tl] = '\0';

      out.items[out.count].start = st;
      out.items[out.count].end   = en;
      out.items[out.count].narration = str_dup(narbuf);
      out.count++;
    }
  }

  for (size_t i = 0; i < n; i++) free(cues[i].text);
  free(cues);
  return out;
}

static bool process_movie(const Config *cfg, const char *movie_path, const char *movie_title,
                          int num_clips, int movie_index, int movie_total,
                          const char *out_suffix, bool retire_after) {
  ensure_dir("clips");
  ensure_dir("clips/audio");
  ensure_dir("output");
  ensure_dir("tiktok_output");
  ensure_dir("scripts");
  ensure_dir("scripts/srt_files");
  ensure_dir("movies_retired");

  char out_base[PATH_MAX];
  if (out_suffix && out_suffix[0])
    snprintf(out_base, sizeof(out_base), "%s (%s)", movie_title, out_suffix);
  else
    snprintf(out_base, sizeof(out_base), "%s", movie_title);

  char lang_code_buf[8];
  snprintf(lang_code_buf, sizeof(lang_code_buf), "%s",
           recap_lang_code(cfg->recap_language));
  const char *lang_code = lang_code_buf;
  char srt_in[PATH_MAX], srt_mod[PATH_MAX], script_txt[PATH_MAX];
  if (strcmp(lang_code, "en") != 0)
    snprintf(srt_in, sizeof(srt_in), "scripts/srt_files/%s.%s.srt", movie_title, lang_code);
  else
    snprintf(srt_in, sizeof(srt_in), "scripts/srt_files/%s.srt", movie_title);
  if (strcmp(lang_code, "en") != 0)
    snprintf(srt_mod, sizeof(srt_mod), "scripts/srt_files/%s_%s_modified.srt", movie_title, lang_code);
  else
    snprintf(srt_mod, sizeof(srt_mod), "scripts/srt_files/%s_modified.srt", movie_title);
  snprintf(script_txt, sizeof(script_txt), "scripts/srt_files/%s_summary.txt", movie_title);

  report_progress(GEN_STAGE_SUBTITLES, movie_index, movie_total, 0, 0, movie_title);

  bool subs_placeholder = false;
  char srt_ph[PATH_MAX];
  snprintf(srt_ph, sizeof(srt_ph), "scripts/srt_files/%s_placeholder.srt", movie_title);

  if (!file_exists(srt_in) && find_subtitle_srt(movie_title, srt_in, sizeof(srt_in), lang_code))
    logok("Matched a subtitle file for %s: %s", movie_title, srt_in);

  if (!file_exists(srt_in) && strcmp(lang_code, "en") != 0) {
    char plain[PATH_MAX];
    snprintf(plain, sizeof(plain), "scripts/srt_files/%s.srt", movie_title);
    if (file_exists(plain)) {
      snprintf(srt_in, sizeof(srt_in), "%s", plain);
      logi("No %s subtitles for %s - using the English ones; the story will be told in %s.",
           cfg->recap_language[0] ? cfg->recap_language : "the requested language",
           movie_title, cfg->recap_language);
    }
  }

  if (!file_exists(srt_in)) {
    logi("No exact SRT found for %s; attempting download...", movie_title);
    if (!download_subtitle_srt(movie_title, srt_in)) {
      logw("Subtitle download failed for %s.", movie_title);
      if (cfg->auto_transcribe) {
        char wpy[PATH_MAX], wroot[PATH_MAX];
        if (find_tools_python(wpy, sizeof(wpy), wroot, sizeof(wroot))) {
          logi("No subtitles anywhere - transcribing the movie audio with faster-whisper instead.");
          logi("(First run downloads the model; a full movie can take 10-60 minutes on CPU.)");
          if (whisper_transcribe_to_srt(movie_path, srt_in, cfg->whisper_model, wpy, wroot))
            logok("AI transcription ready: %s", srt_in);
          else
            logw("AI transcription failed.");
        } else {
          logi("AI transcription is not installed (run run.bat once) - skipping it.");
        }
      }
      if (!file_exists(srt_in)) {
        if (file_exists(srt_ph)) {
          snprintf(srt_in, sizeof(srt_in), "%s", srt_ph);
          subs_placeholder = true;
          logw("Reusing the placeholder subtitle track for %s - the recap will NOT know the real story!", movie_title);
        } else {
          if (!make_fallback_srt(movie_path, movie_title, srt_ph, num_clips)) {
            logw("Placeholder SRT could not be built. You can still place your own SRT at: scripts/srt_files/%s.srt", movie_title);
            return false;
          }
          snprintf(srt_in, sizeof(srt_in), "%s", srt_ph);
          subs_placeholder = true;
          logw("NO REAL SUBTITLES for %s! The recap cannot know the story and will be generic.", movie_title);
        }
        logw("For an accurate recap, put your subtitle file at: scripts/srt_files/%s.srt", movie_title);
      }
    } else {
      logok("Downloaded SRT: %s", srt_in);
    }
  } else {
    logok("Found SRT: %s", srt_in);
  }

  if (generator_cancel_requested()) return false;

  bool need_convert = !file_exists(srt_mod);
  if (!need_convert && !srt_seconds_file_is_usable(srt_mod)) {
    logw("The cached converted subtitles are out of date (not in timeline order, or "
         "still a stream of subtitle fragments) - rebuilding: %s", srt_mod);
    plat_unlink(srt_mod);
    need_convert = true;
  }
  if (need_convert) {
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
  warn_if_subtitles_cover_wrong_span(subs_seconds, ffprobe_duration_seconds(movie_path), movie_title);

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

  int per_clip_sec = 0;
  if (cfg->recap_minutes >= 1.0) {
    per_clip_sec = (int)((cfg->recap_minutes * 60.0) / (double)num_clips);
    if (per_clip_sec > 0 && per_clip_sec < 8) per_clip_sec = 8;
    if (per_clip_sec > 0)
      logi("Target recap length ~%.0f min => about %d s per clip.", cfg->recap_minutes, per_clip_sec);
  }

  report_progress(GEN_STAGE_PLANNING, movie_index, movie_total, 0, 0, movie_title);
  logi("Requesting OpenAI clip plan (%d clips target)...", num_clips);
  warn_if_model_is_small(cfg);

  /* The published plot summary gives the model a reliable text for character
     names, so it does not have to rely on its memory of the film.  Optional:
     when it cannot be fetched the run continues with the subtitles. */
  char *plot_summary = wikipedia_plot_summary(cfg, movie_title);
  if (!plot_summary && cfg->use_wikipedia_plot)
    logw("Continuing without a plot summary - character names rely on the subtitles "
         "and the model's memory. Set \"use_wikipedia_plot\": false to silence this, "
         "or drop a summary at scripts/srt_files/%s_plot.txt.", movie_title);

  bool retry_no_script = false, retry_json_only = false;
  ClipPlanList plan = openai_make_plan(cfg, movie_title, subs_seconds,
                                       imsdb_script ? imsdb_script : "",
                                       plot_summary ? plot_summary : "",
                                       subs_placeholder,
                                       num_clips, per_clip_sec,
                                       &retry_no_script, &retry_json_only, NULL);

  if (plan.count == 0 && retry_no_script && imsdb_script && imsdb_script[0]) {
    logw("OpenAI request failed with IMSDb context; retrying without IMSDb script for %s", movie_title);
    plan = openai_make_plan(cfg, movie_title, subs_seconds, "", plot_summary ? plot_summary : "",
                            subs_placeholder, num_clips, per_clip_sec, NULL, NULL, NULL);
  }

  /* The request succeeded but the answer was not a usable clip plan (markdown
     fences, prose around the JSON, a different shape).  Going straight to the
     offline planner would emit the raw subtitle lines as the "recap", so ask
     once more for bare JSON first. */
  if (plan.count == 0 && retry_json_only) {
    logw("Asking the model once more for the clip plan as bare JSON.");
    plan = openai_make_plan(cfg, movie_title, subs_seconds,
                            imsdb_script ? imsdb_script : "",
                            plot_summary ? plot_summary : "", subs_placeholder,
                            num_clips, per_clip_sec, NULL, NULL,
                            "CRITICAL: the previous answer could not be used. Reply with ONE "
                            "JSON object and nothing else - no markdown fences, no comments, "
                            "no text before or after it, no trailing commas, and start/end as "
                            "plain whole numbers of seconds: "
                            "{\"clips\":[{\"start\":120,\"end\":150,\"narration\":\"...\"}]}");
  }

  if (plan.count > 0 && plan_language_mismatch(plan.items, plan.count, lang_code)) {
    logw("The AI plan came back in the wrong language - demanding %s and retrying once.",
         cfg->recap_language);
    char lang_note[600];
    snprintf(lang_note, sizeof(lang_note),
             "CRITICAL: the previous answer was written in the wrong language and was "
             "rejected. EVERY narration string MUST be written entirely in %s, in that "
             "language's own characters. Do not output English.", cfg->recap_language);
    free_clip_plan_list(&plan);
    plan = openai_make_plan(cfg, movie_title, subs_seconds,
                            imsdb_script ? imsdb_script : "",
                            plot_summary ? plot_summary : "", subs_placeholder,
                            num_clips, per_clip_sec, NULL, NULL, lang_note);
    if (plan.count > 0 && plan_language_mismatch(plan.items, plan.count, lang_code))
      logw("The AI is still answering in the wrong language - the %s recap may come "
           "out in that language instead. Try a stronger model for this language.",
           cfg->recap_language);
  }

  /* Length audit.  With "recap_minutes" set, the narrations have to add up to
     the requested speaking time: a model that writes one-liners per clip makes
     the video come out far shorter than asked (the clip is sped up at most
     max_video_speedup and then cut down to the narration). */
  if (per_clip_sec > 0 && cfg->recap_minutes >= 1.0 && plan.count > 0) {
    double target_sec = cfg->recap_minutes * 60.0;
    double speech = plan_speech_seconds(plan.items, plan.count, lang_code);
    logi("Plan speech: about %.1f min of narration for the %.0f min target (%zu clips).",
         speech / 60.0, cfg->recap_minutes, plan.count);

    if (speech < target_sec * 0.8) {
      int mn = 0, mx = 0;
      clip_seconds_range(per_clip_sec, &mn, &mx);
      double per_sec = lang_speech_units_per_sec(lang_code);
      bool in_chars = lang_counts_chars(lang_code);
      const char *unit = in_chars ? "characters" : "words";
      char len_note[900];
      snprintf(len_note, sizeof(len_note),
               "CRITICAL LENGTH RULE: the narrations of the previous answer added up to "
               "only about %d seconds of speech, but the target for this video is about "
               "%d seconds (%.0f minutes). Every clip must carry a FULL narration of "
               "roughly %d-%d %s (a %d to %d second clip needs that much speech) - not "
               "one line, not a short summary. Rewrite the whole plan with full-length "
               "narrations and keep the same JSON shape.",
               (int)speech, (int)target_sec, cfg->recap_minutes,
               (int)((double)mn * per_sec + 0.5), (int)((double)mx * per_sec + 0.5),
               unit, mn, mx);
      logw("The narrations are much shorter than the %.0f minute target (%.1f min of "
           "speech) - asking the model once more for full-length narrations.",
           cfg->recap_minutes, speech / 60.0);
      ClipPlanList longer = openai_make_plan(cfg, movie_title, subs_seconds,
                                             imsdb_script ? imsdb_script : "",
                                             plot_summary ? plot_summary : "",
                                             subs_placeholder, num_clips, per_clip_sec,
                                             NULL, NULL, len_note);
      double speech2 = longer.count ? plan_speech_seconds(longer.items, longer.count, lang_code) : 0.0;
      if (longer.count > 0 && speech2 > speech) {
        logok("Second attempt: about %.1f min of narration (was %.1f min).",
              speech2 / 60.0, speech / 60.0);
        free_clip_plan_list(&plan);
        plan = longer;
        speech = speech2;
      } else {
        free_clip_plan_list(&longer);
        logw("The model still wrote short narrations - expect a recap near %.1f min "
             "instead of %.0f min. A stronger model, fewer clips (min_clips/max_clips) "
             "or a lower max_video_speedup change this.",
             speech / 60.0, cfg->recap_minutes);
      }
    }
  }

  if (plan.count == 0 && !cfg->offline_planner) {
    logw("No AI clip plan for %s - skipping this movie instead of turning the raw "
         "subtitle lines into the narration.", movie_title);
    logw("The messages above name the exact failure (API key, model id, base URL, "
         "quota, output limit). Fix that and run again; set \"offline_planner\": true "
         "in config.json only if you really want the raw-subtitle fallback video.");
    free(subs_seconds);
    if (imsdb_script) free(imsdb_script);
    free(plot_summary);
    free_clip_plan_list(&plan);
    return false;
  }

  if (plan.count == 0) {
    logi("No AI plan available - falling back to the offline planner.");
    logw("Offline planner = the narration will be RAW SUBTITLE LINES, not a retold "
         "story (offline_planner=true in config.json). Check the warnings above for "
         "why the AI request failed (API key, model id, base URL, quota).");
    if (strcmp(lang_code, "en") != 0)
      logw("The offline fallback planner cannot translate - this pass will stay in "
           "the subtitle language, NOT %s!", cfg->recap_language);
    plan = local_make_plan(subs_seconds, num_clips, per_clip_sec);
  }

  free(subs_seconds);
  if (imsdb_script) free(imsdb_script);
  free(plot_summary);

  if (plan.count == 0) {
    logw("No plan returned for %s", movie_title);
    free_clip_plan_list(&plan);
    return false;
  }
  logok("Clip plan ready: %zu clips", plan.count);

  char concat_list_path[PATH_MAX];
  snprintf(concat_list_path, sizeof(concat_list_path), "clips/%s_concat_list.txt", movie_title);

  FILE *listf = plat_fopen(concat_list_path, "wb");
  if (!listf) {
    logw("Failed to create concat list: %s", concat_list_path);
    free_clip_plan_list(&plan);
    return false;
  }

  bool non_en = cfg->recap_language[0] && str_icmp(cfg->recap_language, "english") != 0;
  if (non_en && cfg->tts_provider == TTS_PIPER)
    logw("Recap language is %s but the installed Piper voice speaks English - switch to Edge TTS or install a matching Piper voice.", cfg->recap_language);
  if (cfg->captions && non_en && lang_has_own_script(recap_lang_code(cfg->recap_language))) {
    const char *lf = caption_font_for_language(cfg, cfg->recap_language);
    if (lf && lf[0] && strcmp(lf, cfg->caption_font) != 0)
      logi("Caption font for %s: %s", recap_lang_code(cfg->recap_language), lf);
    else
      logw("Captions for %s use %s, which has no %s glyphs - they will render as "
           "boxes. Set the per-language caption font in the panel (caption_font_zh "
           "for Chinese/Japanese/Korean, caption_font_ar for Arabic/Persian/Urdu) "
           "to e.g. C:/Windows/Fonts/msyh.ttc or C:/Windows/Fonts/arial.ttf.",
           cfg->recap_language, cfg->caption_font, cfg->recap_language);
  }
  if (cfg->captions && !caption_font_available(cfg->caption_font))
    logw("Captions are on but resources/Inter-Regular.ttf is missing - skipping burnt-in subtitles.");

  /* Caption wrapping is sized from the frame: the vertical (9:16) render keeps
     only the middle 60% of the width, so a caption that is wider than that
     would be cut off at the sides of the Short. */
  int frame_w = 0, frame_h = 0;
  if (!ffprobe_video_dimensions(movie_path, &frame_w, &frame_h)) { frame_w = 1280; frame_h = 720; }
  logi("Movie frame: %dx%d", frame_w, frame_h);

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
    if (!ffmpeg_make_adjusted_clip(cfg, movie_path, start_s, end_s, nar_mp3, nar_dur, out_clip,
                                     plan.items[i].narration, frame_w, frame_h)) {
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

  /* Say out loud how close the recap got to the requested length: a model that
     under-writes the narrations cannot be fixed downstream, the video simply
     has to be sped up (capped by max_video_speedup) and comes out short. */
  if (cfg->recap_minutes >= 1.0) {
    double want = cfg->recap_minutes * 60.0;
    logi("Recap length: %.1f min of the %.0f min target (%.0f%%).",
         final_dur / 60.0, cfg->recap_minutes, 100.0 * final_dur / want);
    if (final_dur < want * 0.85)
      logw("This recap is much shorter than the %.0f minutes asked for. Each clip "
           "was sped up at most %.2fx and then cut down to its narration, so the "
           "spoken lines were too short for their clip ranges. Try a stronger model, "
           "fewer clips (min_clips/max_clips), or set \"recap_minutes\": 0.",
           cfg->recap_minutes, cfg->max_video_speedup);
  }

  /* Cancelled after the clips were joined: keep the recap we already have and
     skip the optional BGM / vertical steps instead of burning more time. */
  if (generator_cancel_requested()) {
    char out_partial[PATH_MAX];
    snprintf(out_partial, sizeof(out_partial), "output/%s.mp4", out_base);
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
    snprintf(out_final_only, sizeof(out_final_only), "output/%s.mp4", out_base);
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
      snprintf(out_final_only, sizeof(out_final_only), "output/%s.mp4", out_base);
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
      snprintf(out_final_only, sizeof(out_final_only), "output/%s.mp4", out_base);
      plat_rename(tmp_concat, out_final_only);
      logok("Wrote output (no BGM): %s", out_final_only);
    } else {
      logok("BGM concat OK: %s", bgm_out);

      char out_final_only[PATH_MAX];
      snprintf(out_final_only, sizeof(out_final_only), "output/%s.mp4", out_base);

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
  snprintf(out_final, sizeof(out_final), "output/%s.mp4", out_base);
  snprintf(out_vert,  sizeof(out_vert),  "tiktok_output/%s_vertical.mp4", out_base);

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

  if (!retire_after) {
    logi("More recap languages to render - leaving %s in movies/ for now.", movie_path);
  } else if (cfg->retire_movies) {
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

/* One recap per language, so each language has its own file ("Title.mp4" for
 * English, "Title (Chinese).mp4" for the Chinese pass).  Checking only the
 * English file used to skip the whole movie, and with it every other language
 * the user had just asked for. */
static bool output_file_exists(const char *file_name) {
  char out[PATH_MAX];
  snprintf(out, sizeof(out), "output/%s", file_name);
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

  /* FFmpeg is installed on demand by run.bat into a portable tools folder. */
  if (!plat_have_tool("ffmpeg") || !plat_have_tool("ffprobe")) {
#if defined(_WIN32)
    die("ffmpeg/ffprobe not found.\n"
        "  Start the app with run.bat (or run.bat web) - it downloads a portable\n"
        "  FFmpeg into the tools folder for you, with no admin rights and nothing\n"
        "  written to C:.\n"
        "  If you launched the .exe directly, close it and use run.bat instead.\n"
        "  Manual alternative: winget install Gyan.FFmpeg, then reopen this app.");
#else
    die("ffmpeg/ffprobe not found in PATH. Install FFmpeg and try again.\n"
        "  A bundled copy in ./tools/ffmpeg/bin or ./ffmpeg/bin is picked up\n"
        "  automatically, or set MOVIECAP_TOOLS to the folder that contains it.");
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

  piper_ensure_server(&cfg);

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

    char path[PATH_MAX];
    snprintf(path, sizeof(path), "movies/%s", names[i]);

    char banner[PATH_MAX + 64];
    snprintf(banner, sizeof(banner), "=== Processing: %s ===", title);
    emit_line("");
    emit_line(banner);
    if (generator_cancel_requested()) { free_str_list(names, n_names); break; }

    report_progress(GEN_STAGE_SETUP, (int)(i + 1), (int)n_names, 0, 0, title);

    int nl = cfg.n_recap_languages > 0 ? cfg.n_recap_languages : 1;
    bool any_ok = false;
    int ran = 0;
    for (int li = 0; li < nl; li++) {
      if (generator_cancel_requested()) break;
      Config lcfg = cfg;
      snprintf(lcfg.recap_language, sizeof(lcfg.recap_language), "%s", cfg.recap_languages[li]);
      if (lcfg.tts_provider == TTS_EDGE && cfg.tts_voice_auto)
        snprintf(lcfg.tts_voice, sizeof(lcfg.tts_voice), "%s",
                 edge_voice_for_language(lcfg.recap_language));
      char lcode_buf[8];
      snprintf(lcode_buf, sizeof(lcode_buf), "%s", recap_lang_code(lcfg.recap_language));
      const char *lcode = lcode_buf;
      const char *lfont = caption_font_for_language(&cfg, lcfg.recap_language);
      if (lfont && lfont[0])
        snprintf(lcfg.caption_font, sizeof(lcfg.caption_font), "%s", lfont);
      else if (lang_has_own_script(lcode))
        logw("No caption font with %s glyphs was found on this machine (checked the "
             "panel setting and the usual system fonts) - captions will fall back to "
             "%s and %s characters may show as boxes.", lcfg.recap_language,
             cfg.caption_font, lcfg.recap_language);
      const char *label = recap_lang_label(lcfg.recap_language);
      if (nl > 1) {
        snprintf(banner, sizeof(banner), "--- Recap %d/%d: %s ---", li + 1, nl,
                 label[0] ? label : "English");
        emit_line("");
        emit_line(banner);
      }

      char out_name[PATH_MAX];
      if (label[0]) snprintf(out_name, sizeof(out_name), "%s (%s).mp4", title, label);
      else          snprintf(out_name, sizeof(out_name), "%s.mp4", title);
      if (output_file_exists(out_name)) {
        logi("Skipping %s: output/%s already exists.", title, out_name);
        logi("Delete that file (or move the movie back from movies_retired/) to render it again.");
        continue;
      }

      ran++;
      if (process_movie(&lcfg, path, title, num_clips, (int)(i + 1), (int)n_names,
                        label, li == nl - 1)) {
        any_ok = true;
      } else {
        break;
      }
    }
    if (ran == 0) {
      logi("Skipping %s: every requested recap already exists in output/. Delete the "
           "file(s) there to render one again.", title);
      continue;                       /* not a failure, just nothing to do */
    }
    if (any_ok) {
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
