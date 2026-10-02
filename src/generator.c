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

static void *xrealloc(void *p, size_t n) {
  void *q = realloc(p, n);
  if (!q) die("OOM");
  return q;
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
  CURL *curl = curl_easy_init();
  if (!curl) die("curl_easy_init failed");

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

  if (res != CURLE_OK) {
    if (buf.data) free(buf.data);
    die("GET failed: %s (%s)", url, curl_easy_strerror(res));
  }

  return buf;
}

static MemBuf http_post_json_to_mem(const char *url, const char *bearer_key, const char *json_body,
                                   long *http_code_out, long timeout_s) {
  CURL *curl = curl_easy_init();
  if (!curl) die("curl init failed");

  MemBuf buf = (MemBuf){0};

  struct curl_slist *headers = NULL;
  headers = curl_slist_append(headers, "Content-Type: application/json");

  char auth[1024];
  snprintf(auth, sizeof(auth), "Authorization: Bearer %s", bearer_key);
  headers = curl_slist_append(headers, auth);

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
    if (buf.data) free(buf.data);
    die("POST failed: %s", curl_easy_strerror(res));
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
  char cmd[8192];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(cmd, sizeof(cmd), fmt, ap);
  va_end(ap);

  char line[8300];
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

typedef struct {
  char openai_key[512];
  char eleven_key[512];
  char eleven_voice_id[128];
  char eleven_model_id[128];
  char openai_model[128]; /* optional "openai_model" in config.json, default gpt-5.2 */
  double target_minutes;  /* optional "target_minutes"; 0 = original behaviour (20-30 clips, ~3-4 min) */
} Config;

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
  const cJSON *tmn = cJSON_GetObjectItemCaseSensitive(root, "target_minutes");
  if (cJSON_IsNumber(tmn) && tmn->valuedouble > 0) c.target_minutes = tmn->valuedouble;

  if (cJSON_IsString(ok)  && ok->valuestring)  strncpy(c.openai_key, ok->valuestring, sizeof(c.openai_key)-1);
  if (cJSON_IsString(ek)  && ek->valuestring)  strncpy(c.eleven_key, ek->valuestring, sizeof(c.eleven_key)-1);
  if (cJSON_IsString(vid) && vid->valuestring) strncpy(c.eleven_voice_id, vid->valuestring, sizeof(c.eleven_voice_id)-1);
  if (cJSON_IsString(mid) && mid->valuestring) strncpy(c.eleven_model_id, mid->valuestring, sizeof(c.eleven_model_id)-1);

  if (cJSON_IsString(oam) && oam->valuestring) strncpy(c.openai_model, oam->valuestring, sizeof(c.openai_model)-1);

  if (c.openai_key[0] == 0) die("config.json: open_api_key missing");
  if (strcmp(c.openai_key, "OpenAIAPI") == 0)
    die("config.json: replace the placeholder \"OpenAIAPI\" with your real OpenAI API key");
  if (strcmp(c.eleven_key, "ElevenLabsAPI") == 0)
    die("config.json: replace the placeholder \"ElevenLabsAPI\" with your real ElevenLabs API key");
  if (c.openai_model[0] == 0) strncpy(c.openai_model, "gpt-5.2", sizeof(c.openai_model)-1);
  if (c.eleven_key[0] == 0) die("config.json: elevenlabs_api_key missing");
  if (c.eleven_voice_id[0] == 0) strncpy(c.eleven_voice_id, "JBFqnCBsd6RMkjVDRZzb", sizeof(c.eleven_voice_id)-1);
  if (c.eleven_model_id[0] == 0) strncpy(c.eleven_model_id, "eleven_multilingual_v2", sizeof(c.eleven_model_id)-1);

  cJSON_Delete(root);
  return c;
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

/* ----------------------- Subtitle decoding + parsing ----------------------- */
/*
 * Why this exists: the original read subtitles with fgets()/strlen(). A UTF-16
 * .srt (very common on Windows) has a zero byte after almost every letter, so
 * only "\xFF\xFE" + "1" reached OpenAI, and the model invented a generic,
 * looping "recap" from the title alone. Subtitles are now decoded properly,
 * parsed into cues, and checked before anything is sent to OpenAI.
 */

static void sb_put_cp(char **b, size_t *l, size_t *c, unsigned cp) {
  char t[4];
  size_t n;
  if (cp < 0x80)         { t[0] = (char)cp; n = 1; }
  else if (cp < 0x800)   { t[0] = (char)(0xC0 | (cp >> 6)); t[1] = (char)(0x80 | (cp & 0x3F)); n = 2; }
  else if (cp < 0x10000) { t[0] = (char)(0xE0 | (cp >> 12)); t[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
                           t[2] = (char)(0x80 | (cp & 0x3F)); n = 3; }
  else                   { t[0] = (char)(0xF0 | (cp >> 18)); t[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
                           t[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); t[3] = (char)(0x80 | (cp & 0x3F)); n = 4; }
  sb_append(b, l, c, t, n);
}

static bool is_valid_utf8(const unsigned char *p, size_t n) {
  size_t i = 0;
  while (i < n) {
    unsigned char c = p[i];
    size_t need;
    if (c < 0x80) { i++; continue; }
    else if (c >= 0xC2 && c <= 0xDF) need = 1;
    else if (c >= 0xE0 && c <= 0xEF) need = 2;
    else if (c >= 0xF0 && c <= 0xF4) need = 3;
    else return false;
    for (size_t k = 1; k <= need; k++) {
      if (i + k >= n || (p[i + k] & 0xC0) != 0x80) return false;
    }
    i += need + 1;
  }
  return true;
}

static const unsigned short CP1252_80_9F[32] = {
  0x20AC,0xFFFD,0x201A,0x0192,0x201E,0x2026,0x2020,0x2021,0x02C6,0x2030,0x0160,0x2039,0x0152,0xFFFD,0x017D,0xFFFD,
  0xFFFD,0x2018,0x2019,0x201C,0x201D,0x2022,0x2013,0x2014,0x02DC,0x2122,0x0161,0x203A,0x0153,0xFFFD,0x017E,0x0178
};

/* Decode raw subtitle bytes (UTF-8 / UTF-8 BOM / UTF-16 LE/BE with or without BOM / Windows-1252) to UTF-8. */
static char *decode_text_to_utf8(const unsigned char *p, size_t n, const char **enc_out) {
  char *out = NULL;
  size_t len = 0, cap = 0;
  const char *enc = "UTF-8";
  int utf16 = 0; /* 1 = LE, 2 = BE */

  if (n >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) { p += 3; n -= 3; enc = "UTF-8 (BOM)"; }
  else if (n >= 2 && p[0] == 0xFF && p[1] == 0xFE) { p += 2; n -= 2; utf16 = 1; enc = "UTF-16 LE"; }
  else if (n >= 2 && p[0] == 0xFE && p[1] == 0xFF) { p += 2; n -= 2; utf16 = 2; enc = "UTF-16 BE"; }
  else {
    size_t m = n < 4000 ? n : 4000, z_even = 0, z_odd = 0;
    for (size_t i = 0; i < m; i++) if (p[i] == 0) { if (i & 1) z_odd++; else z_even++; }
    if (z_odd > m / 6 && z_even < z_odd / 4) { utf16 = 1; enc = "UTF-16 LE (no BOM)"; }
    else if (z_even > m / 6 && z_odd < z_even / 4) { utf16 = 2; enc = "UTF-16 BE (no BOM)"; }
  }

  if (utf16) {
    for (size_t i = 0; i + 1 < n; i += 2) {
      unsigned u = (utf16 == 1) ? (unsigned)(p[i] | (p[i + 1] << 8)) : (unsigned)((p[i] << 8) | p[i + 1]);
      if (u >= 0xD800 && u <= 0xDBFF && i + 3 < n) {
        unsigned u2 = (utf16 == 1) ? (unsigned)(p[i + 2] | (p[i + 3] << 8)) : (unsigned)((p[i + 2] << 8) | p[i + 3]);
        if (u2 >= 0xDC00 && u2 <= 0xDFFF) {
          sb_put_cp(&out, &len, &cap, 0x10000 + ((u - 0xD800) << 10) + (u2 - 0xDC00));
          i += 2;
          continue;
        }
        u = 0xFFFD;
      } else if (u >= 0xD800 && u <= 0xDFFF) {
        u = 0xFFFD;
      }
      if (u == 0) continue;
      sb_put_cp(&out, &len, &cap, u);
    }
  } else if (is_valid_utf8(p, n)) {
    for (size_t i = 0; i < n; i++) if (p[i]) sb_append(&out, &len, &cap, (const char *)&p[i], 1);
  } else {
    enc = "Windows-1252";
    for (size_t i = 0; i < n; i++) {
      unsigned char c = p[i];
      if (!c) continue;
      unsigned cp = (c >= 0x80 && c <= 0x9F) ? CP1252_80_9F[c - 0x80] : c;
      sb_put_cp(&out, &len, &cap, cp);
    }
  }

  if (!out) out = str_dup("");
  if (enc_out) *enc_out = enc;
  return out;
}

static char *read_text_file_utf8(const char *path, const char **enc_out) {
  FILE *f = plat_fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (n < 0) { fclose(f); return NULL; }
  unsigned char *raw = (unsigned char *)malloc((size_t)n + 1);
  if (!raw) die("OOM");
  size_t got = fread(raw, 1, (size_t)n, f);
  fclose(f);
  char *txt = decode_text_to_utf8(raw, got, enc_out);
  free(raw);
  return txt;
}

typedef struct {
  double start, end;
  char *text;
} SubCue;

typedef struct {
  SubCue *items;
  size_t count, cap;
  size_t dialogue_chars;
  double first_start, last_end;
} SubList;

static void free_sub_list(SubList *l) {
  if (!l) return;
  for (size_t i = 0; i < l->count; i++) free(l->items[i].text);
  free(l->items);
  memset(l, 0, sizeof(*l));
}

/* "01:02:03,456" / "01:02:03.456" / "02:03,456" / "1:02:03" -> seconds */
static bool parse_srt_time(const char *s, double *out) {
  while (*s == ' ' || *s == '\t') s++;
  int f[3] = {0, 0, 0}, nf = 0;
  while (nf < 3 && isdigit((unsigned char)*s)) {
    int v = 0;
    while (isdigit((unsigned char)*s)) { v = v * 10 + (*s - '0'); s++; }
    f[nf++] = v;
    if (*s == ':') s++; else break;
  }
  if (nf < 2) return false;
  double ms = 0.0;
  if (*s == ',' || *s == '.') {
    s++;
    double scale = 0.1;
    while (isdigit((unsigned char)*s)) { ms += (*s - '0') * scale; scale /= 10.0; s++; }
  }
  double t = (nf == 3) ? f[0] * 3600.0 + f[1] * 60.0 + f[2] : f[0] * 60.0 + f[1];
  *out = t + ms;
  return true;
}

/* strip <i>, </font ...>, {\an8} etc., collapse whitespace */
static void clean_sub_line(const char *in, char *out, size_t outsz) {
  size_t j = 0;
  bool space = false;
  for (size_t i = 0; in[i] && j + 1 < outsz; i++) {
    char c = in[i];
    if (c == '<') { const char *e = strchr(in + i, '>'); if (e) { i = (size_t)(e - in); continue; } }
    if (c == '{') { const char *e = strchr(in + i, '}'); if (e) { i = (size_t)(e - in); continue; } }
    if (c == '\t' || c == ' ') { space = (j > 0); continue; }
    if (space) { out[j++] = ' '; space = false; if (j + 1 >= outsz) break; }
    out[j++] = c;
  }
  out[j] = 0;
}

static void sub_push(SubList *l, double a, double b, const char *text) {
  if (!text || !text[0]) return;
  if (l->count + 1 > l->cap) {
    l->cap = l->cap ? l->cap * 2 : 1024;
    l->items = (SubCue *)xrealloc(l->items, l->cap * sizeof(SubCue));
  }
  l->items[l->count].start = a;
  l->items[l->count].end = b;
  l->items[l->count].text = str_dup(text);
  l->count++;
  l->dialogue_chars += strlen(text);
  if (l->count == 1 || a < l->first_start) l->first_start = a;
  if (b > l->last_end) l->last_end = b;
}

static bool is_all_digits(const char *s) {
  if (!*s) return false;
  for (; *s; s++) if (!isdigit((unsigned char)*s)) return false;
  return true;
}

static SubList parse_srt_text(const char *txt) {
  SubList l;
  memset(&l, 0, sizeof(l));
  char cur[8192] = {0};
  char pending_num[64] = {0};
  bool in_cue = false;
  double a = 0, b = 0;

  const char *p = txt;
  while (*p) {
    const char *e = p;
    while (*e && *e != '\n') e++;
    size_t n = (size_t)(e - p);
    char line[4096];
    if (n >= sizeof(line)) n = sizeof(line) - 1;
    memcpy(line, p, n);
    line[n] = 0;
    while (n > 0 && (line[n - 1] == '\r' || line[n - 1] == ' ')) line[--n] = 0;
    p = *e ? e + 1 : e;

    char *arrow = strstr(line, "-->");
    double ta, tb;
    if (arrow && parse_srt_time(line, &ta) && parse_srt_time(arrow + 3, &tb)) {
      if (in_cue) sub_push(&l, a, b, cur);
      cur[0] = 0;
      pending_num[0] = 0;
      a = ta; b = tb;
      in_cue = true;
      continue;
    }
    if (!in_cue) continue;
    if (line[0] == 0) { sub_push(&l, a, b, cur); cur[0] = 0; in_cue = false; continue; }

    char clean[4096];
    clean_sub_line(line, clean, sizeof(clean));
    if (!clean[0]) continue;
    if (is_all_digits(clean) && strlen(clean) < sizeof(pending_num)) {
      /* could be the next cue's index (missing blank line) - decide on the next line */
      if (pending_num[0]) { strncat(cur, " ", sizeof(cur) - strlen(cur) - 1); strncat(cur, pending_num, sizeof(cur) - strlen(cur) - 1); }
      snprintf(pending_num, sizeof(pending_num), "%s", clean);
      continue;
    }
    if (pending_num[0]) {
      if (cur[0]) strncat(cur, " ", sizeof(cur) - strlen(cur) - 1);
      strncat(cur, pending_num, sizeof(cur) - strlen(cur) - 1);
      pending_num[0] = 0;
    }
    if (cur[0]) strncat(cur, " ", sizeof(cur) - strlen(cur) - 1);
    strncat(cur, clean, sizeof(cur) - strlen(cur) - 1);
  }
  if (in_cue) sub_push(&l, a, b, cur);
  return l;
}

/* Compact planning text: one line per subtitle, "START-END: dialogue" (whole seconds). */
static char *sub_list_to_planning_text(const SubList *l) {
  char *out = NULL;
  size_t len = 0, cap = 0;
  for (size_t i = 0; i < l->count; i++) {
    char head[64];
    int n = snprintf(head, sizeof(head), "%d-%d: ", (int)floor(l->items[i].start), (int)ceil(l->items[i].end));
    sb_append(&out, &len, &cap, head, (size_t)n);
    sb_append(&out, &len, &cap, l->items[i].text, strlen(l->items[i].text));
    sb_append(&out, &len, &cap, "\n", 1);
  }
  if (!out) out = str_dup("");
  return out;
}

static void fmt_mmss(double t, char *out, size_t outsz) {
  int s = (int)(t + 0.5);
  if (s >= 3600) snprintf(out, outsz, "%d:%02d:%02d", s / 3600, (s % 3600) / 60, s % 60);
  else snprintf(out, outsz, "%d:%02d", s / 60, s % 60);
}

/* Refuse to plan from subtitles that are empty, truncated or belong to another movie. */
static bool check_subtitles(const SubList *l, double movie_dur, char *why, size_t whysz) {
  why[0] = 0;
  if (l->count < 40 || l->dialogue_chars < 1500) {
    snprintf(why, whysz,
             "the subtitle file has almost no readable dialogue (%zu subtitle lines, %zu characters). "
             "It is probably empty, damaged or in an unsupported format",
             l->count, l->dialogue_chars);
    return false;
  }
  if (movie_dur > 60.0) {
    char a[32], b[32];
    fmt_mmss(l->last_end, a, sizeof(a));
    fmt_mmss(movie_dur, b, sizeof(b));
    if (l->last_end < movie_dur * 0.5) {
      snprintf(why, whysz,
               "the subtitles stop at %s but the movie is %s long - they are incomplete or for a different movie/cut",
               a, b);
      return false;
    }
    if (l->last_end > movie_dur * 1.25 + 60.0) {
      snprintf(why, whysz,
               "the subtitles run until %s but the movie is only %s long - they are for a different movie or cut",
               a, b);
      return false;
    }
  }
  return true;
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

  char list_url[1024];
  snprintf(list_url, sizeof(list_url), "https://subf2m.co/subtitles/%s/english", slug);

  long code = 0;
  MemBuf page = http_get_to_mem_ex(list_url, &code);
  if (code < 200 || code >= 300 || !page.data || page.size == 0) {
    if (page.data) {
      logw("subf2m list HTTP %ld for %s (body starts: %.200s)", code, list_url, page.data);
      free(page.data);
    }
    return false;
  }

  char want_subpage_prefix[768];
  snprintf(want_subpage_prefix, sizeof(want_subpage_prefix), "/subtitles/%s/english/", slug);

  char subpage_url[1024] = {0};

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

      char profile_url[1024];
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

  char download_url[1024] = {0};
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

  char url0[1024], url1[1024], url2[1024], url3[1024], url4[1024], url5[1024], url6[1024];

  snprintf(url0, sizeof(url0), "https://imsdb.com/scripts/%s.html", a);
  snprintf(url1, sizeof(url1), "https://imsdb.com/scripts/%s.html", b);
  snprintf(url2, sizeof(url2), "https://imsdb.com/scripts/%s.html", c);

  snprintf(url3, sizeof(url3), "https://imsdb.com/scripts/%s.html", a_lo);
  snprintf(url4, sizeof(url4), "https://imsdb.com/scripts/%s.html", b_lo);
  snprintf(url5, sizeof(url5), "https://imsdb.com/scripts/%s.html", c_lo);

  snprintf(url6, sizeof(url6), "https://imsdb.com/Movie%%20Scripts/%s%%20Script.html", enc_title);

  const char *attempts[] = { url0, url1, url2, url3, url4, url5, url6, NULL };

  for (int i = 0; attempts[i]; i++) {
    if (!attempts[i][0]) continue;

    char why[256];
    if (imsdb_fetch_script_to_file(attempts[i], dest_txt_path, why, sizeof(why))) {
      if (used_url && used_url_sz) {
        strncpy(used_url, attempts[i], used_url_sz - 1);
        used_url[used_url_sz - 1] = 0;
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

/* ----------------------- Story planning (OpenAI) ----------------------- */

static const char *PLAN_SYSTEM_PROMPT =
  "You are an engaging YouTube movie-recap storyteller. You retell a movie's actual plot as one "
  "continuous story, like explaining it to a friend who hasn't seen it. You ONLY use what is in the "
  "material you are given; you never fall back on generic trailer phrases or filler, and you never "
  "repeat yourself. You always answer with a single JSON object.";

static void sb_add(char **b, size_t *l, size_t *c, const char *s) { sb_append(b, l, c, s, strlen(s)); }

static char *build_plan_prompt(const char *title, const char *subs, const char *script,
                               int num_clips, double movie_dur) {
  char *p = NULL;
  size_t l = 0, c = 0;
  char tmp[1024];
  int dur_s = (int)movie_dur;

  sb_add(&p, &l, &c, "Movie: ");
  sb_add(&p, &l, &c, title);
  sb_add(&p, &l, &c, "\n");
  if (dur_s > 0) {
    snprintf(tmp, sizeof(tmp), "Movie length: %d seconds (%d min).\n", dur_s, dur_s / 60);
    sb_add(&p, &l, &c, tmp);
  }
  sb_add(&p, &l, &c,
    "\nINPUT A - the movie's subtitles. One line per subtitle: \"START-END: dialogue\" (seconds from the "
    "start of the movie). This is the ONLY source of truth for what happens and when.\n");
  sb_add(&p, &l, &c, subs);
  sb_add(&p, &l, &c,
    "\n\nINPUT B - optional screenplay text without timestamps (may be a different draft, may be empty). "
    "Use it only to understand context; if it disagrees with INPUT A, INPUT A wins.\n");
  sb_add(&p, &l, &c, script && script[0] ? script : "(empty)");

  sb_add(&p, &l, &c,
    "\n\nSTEP 1 - Understand the story from INPUT A: who the main characters are (use the names that "
    "appear in the dialogue), what the protagonist wants, the main conflict, the turning points, the "
    "climax and how it actually ends.\n\n");
  snprintf(tmp, sizeof(tmp),
    "STEP 2 - Tell that story as a recap split into %d clips, in chronological order:\n", num_clips);
  sb_add(&p, &l, &c, tmp);
  sb_add(&p, &l, &c,
    "- Structure: Hook -> Setup (the characters and their normal world) -> Inciting incident -> Rising "
    "action -> Climax -> Resolution. Cover the whole movie, including the real ending.\n"
    "- Each narration continues the story from the previous one. Use character names. Explain WHY "
    "characters act, not just what happens. Summarize; don't transcribe dialogue.\n"
    "- Narration length: 20-35 words per clip, 2-4 short, natural, conversational sentences.\n"
    "- STRICT: every narration must mention specific people, places, objects or events from THIS "
    "movie's subtitles. No generic trailer phrases (for example: \"tensions boil over\", \"secrets "
    "surface\", \"our hero digs deep\", \"wastes no time\", \"nothing will ever be the same\"). Never "
    "repeat a sentence. Never pad or loop.\n");
  sb_add(&p, &l, &c,
    "- Clip time ranges: chosen ONLY from INPUT A timestamps, at the moment the narrated event happens "
    "on screen; 8-16 seconds each (never more than 20); non-overlapping; increasing; never starting at 0.\n");
  if (dur_s > 0) {
    snprintf(tmp, sizeof(tmp), "- Every clip must end before second %d.\n", dur_s);
    sb_add(&p, &l, &c, tmp);
  }
  sb_add(&p, &l, &c,
    "- If INPUT A does not contain enough dialogue to understand the story, do NOT invent one: return "
    "{\"error\":\"<why>\"} instead.\n");
  sb_add(&p, &l, &c, "- The first narration must start with: \"Here we go, let's go over the movie ");
  sb_add(&p, &l, &c, title);
  sb_add(&p, &l, &c, ".\"\n\n");
  sb_add(&p, &l, &c,
    "Return STRICT JSON only, in exactly this shape:\n"
    "{\"main_characters\":[\"names exactly as they appear in the subtitles\"],"
    "\"story_summary\":\"3-5 sentences summarizing the actual plot, including the ending\","
    "\"clips\":[{\"start\":120,\"end\":132,\"narration\":\"...\"}]}\n");
  return p;
}

/*
 * One planning request. Returns the model's JSON text (caller frees) or NULL.
 * prev_output/feedback: when set, the previous (rejected) answer and the reasons
 * are sent back so the model can fix them.
 */
static char *openai_request_plan(const Config *cfg, const char *movie_title, const char *subs_text,
                                 const char *optional_script_text, int num_clips, double movie_dur,
                                 const char *prev_output, const char *feedback,
                                 bool *out_retry_without_script) {
  if (out_retry_without_script) *out_retry_without_script = false;

  const size_t MAX_SUB_CHARS    = 320000;
  const size_t MAX_SCRIPT_CHARS = 80000;

  char *title_utf8 = sanitize_utf8_lossy(movie_title ? movie_title : "");
  char *subs_utf8  = sanitize_utf8_lossy(subs_text ? subs_text : "");
  char *scr_utf8   = sanitize_utf8_lossy(optional_script_text ? optional_script_text : "");

  char *subs_trim = trim_copy_utf8_safe(subs_utf8, MAX_SUB_CHARS);
  char *scr_trim  = trim_copy_utf8_safe(scr_utf8,  MAX_SCRIPT_CHARS);
  if (strlen(subs_utf8) > MAX_SUB_CHARS)
    logw("Subtitles are very long (%zu bytes); only the first %zu bytes are sent.", strlen(subs_utf8), MAX_SUB_CHARS);
  free(subs_utf8);
  free(scr_utf8);

  char *prompt = build_plan_prompt(title_utf8, subs_trim, scr_trim, num_clips, movie_dur);
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
  cJSON_AddStringToObject(sys, "content", PLAN_SYSTEM_PROMPT);
  cJSON_AddItemToArray(input, sys);

  cJSON *usr = cJSON_CreateObject();
  cJSON_AddStringToObject(usr, "role", "user");
  cJSON_AddStringToObject(usr, "content", prompt);
  cJSON_AddItemToArray(input, usr);

  if (prev_output && feedback) {
    char *prev_utf8 = sanitize_utf8_lossy(prev_output);
    cJSON *asst = cJSON_CreateObject();
    cJSON_AddStringToObject(asst, "role", "assistant");
    cJSON_AddStringToObject(asst, "content", prev_utf8);
    cJSON_AddItemToArray(input, asst);
    free(prev_utf8);

    char *fb = NULL;
    size_t fl = 0, fc = 0;
    sb_add(&fb, &fl, &fc, "Your answer was rejected for these reasons:\n");
    sb_add(&fb, &fl, &fc, feedback);
    sb_add(&fb, &fl, &fc,
      "\nRewrite the whole recap from scratch. Every narration must be about specific events and the "
      "named characters from INPUT A (the subtitles). No repeated sentences, no generic filler. Same JSON format.");
    cJSON *fbm = cJSON_CreateObject();
    cJSON_AddStringToObject(fbm, "role", "user");
    cJSON_AddStringToObject(fbm, "content", fb);
    cJSON_AddItemToArray(input, fbm);
    free(fb);
  }
  cJSON_AddItemToObject(req, "input", input);

  cJSON *text = cJSON_CreateObject();
  cJSON *format = cJSON_CreateObject();
  cJSON_AddStringToObject(format, "type", "json_object");
  cJSON_AddItemToObject(text, "format", format);
  cJSON_AddItemToObject(req, "text", text);

  char *body = cJSON_PrintUnformatted(req);
  cJSON_Delete(req);
  free(prompt);
  if (!body) return NULL;

  long http_code = 0;
  bool has_script = (optional_script_text && optional_script_text[0] != 0);
  long timeout_s = has_script ? 14400L : 3600L;

  MemBuf resp = http_post_json_to_mem("https://api.openai.com/v1/responses",
                                      cfg->openai_key, body, &http_code, timeout_s);
  free(body);

  if (http_code < 200 || http_code >= 300) {
    logw("OpenAI HTTP %ld", http_code);
    if (resp.data && resp.size) logw("OpenAI raw body: %.800s", resp.data);
    if (has_script && resp.data && openai_resp_should_retry_without_script(resp.data)) {
      if (out_retry_without_script) *out_retry_without_script = true;
    }
    if (resp.data) free(resp.data);
    return NULL;
  }

  char *out_text = openai_extract_output_text(resp.data ? resp.data : "");
  if (!out_text) {
    logw("OpenAI response parse failed.");
    if (resp.data && resp.size) logw("OpenAI raw body: %.800s", resp.data);
    if (has_script && resp.data && openai_resp_should_retry_without_script(resp.data)) {
      if (out_retry_without_script) *out_retry_without_script = true;
    }
  }
  if (resp.data) free(resp.data);
  return out_text;
}

typedef struct {
  ClipPlanList clips;
  char *summary;
  char **chars;
  size_t n_chars;
  char *error; /* the model said it could not do it */
} StoryPlan;

static void free_story_plan(StoryPlan *sp) {
  if (!sp) return;
  free_clip_plan_list(&sp->clips);
  free(sp->summary);
  for (size_t i = 0; i < sp->n_chars; i++) free(sp->chars[i]);
  free(sp->chars);
  free(sp->error);
  memset(sp, 0, sizeof(*sp));
}

static StoryPlan parse_story_plan(const char *json_text) {
  StoryPlan sp;
  memset(&sp, 0, sizeof(sp));
  if (!json_text) return sp;
  sp.clips = parse_clip_plan_json(json_text);
  cJSON *root = cJSON_Parse(json_text);
  if (!root) return sp;
  cJSON *sum = cJSON_GetObjectItemCaseSensitive(root, "story_summary");
  if (cJSON_IsString(sum) && sum->valuestring) sp.summary = str_dup(sum->valuestring);
  cJSON *err = cJSON_GetObjectItemCaseSensitive(root, "error");
  if (cJSON_IsString(err) && err->valuestring && err->valuestring[0]) sp.error = str_dup(err->valuestring);
  cJSON *chars = cJSON_GetObjectItemCaseSensitive(root, "main_characters");
  if (cJSON_IsArray(chars)) {
    int n = cJSON_GetArraySize(chars);
    sp.chars = (char **)calloc((size_t)(n > 0 ? n : 1), sizeof(char *));
    if (!sp.chars) die("OOM");
    cJSON *it = NULL;
    cJSON_ArrayForEach(it, chars) {
      if (cJSON_IsString(it) && it->valuestring && it->valuestring[0]) sp.chars[sp.n_chars++] = str_dup(it->valuestring);
    }
  }
  cJSON_Delete(root);
  return sp;
}

/* lowercase letters/digits only, single spaces */
static void normalize_sentence(const char *in, size_t n, char *out, size_t outsz, int *words) {
  size_t j = 0;
  bool sp = false;
  *words = 0;
  for (size_t i = 0; i < n && j + 1 < outsz; i++) {
    unsigned char c = (unsigned char)in[i];
    if (isalnum(c) || c >= 0x80) {
      if (sp && j > 0) { out[j++] = ' '; if (j + 1 >= outsz) break; }
      if (j == 0 || sp) (*words)++;
      sp = false;
      out[j++] = (char)tolower(c);
    } else if (c == ' ' || c == '\t' || c == '\n' || c == '-' || c == ',') {
      sp = true;
    }
  }
  out[j] = 0;
}

static int cmp_strp(const void *a, const void *b) {
  return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* Does `name` (any word of it, >= 3 letters) appear in `hay`? */
static bool name_appears(const char *name, const char *hay) {
  char w[128];
  const char *p = name;
  while (*p) {
    while (*p && !isalnum((unsigned char)*p) && (unsigned char)*p < 0x80) p++;
    size_t j = 0;
    while (*p && (isalnum((unsigned char)*p) || (unsigned char)*p >= 0x80 || *p == '\'') && j + 1 < sizeof(w)) w[j++] = *p++;
    w[j] = 0;
    if (j >= 3 && strcasestr_local(hay, w)) return true;
    if (j == 0 && *p) p++;
  }
  return false;
}

static const char *GENERIC_PHRASES[] = {
  "wastes no time", "tensions boil", "tensions rise", "tensions run high", "secrets surface",
  "digs deep", "old allies", "brand new enemies", "nothing will ever be the same",
  "stakes have never been higher", "things take a dark turn", "against all odds",
  "little do they know", "throws our characters", "everything changes", "a race against time",
  NULL
};

/*
 * Reject scripts that are not about the movie: looping/repeated sentences,
 * generic trailer filler, characters that never appear in the subtitles,
 * too few usable clips. Returns true if OK; otherwise `problems` lists why.
 */
static bool validate_story_plan(const StoryPlan *sp, const char *dialogue, double movie_dur,
                                int num_clips, char *problems, size_t psz) {
  problems[0] = 0;
  size_t pl = 0;
#define ADD_PROBLEM(...) do { if (pl < psz) { int _n = snprintf(problems + pl, psz - pl, __VA_ARGS__); if (_n > 0) pl += (size_t)_n; } } while (0)

  if (sp->error) {
    ADD_PROBLEM("- the model said it could not write the recap: %s\n", sp->error);
    return false;
  }
  const ClipPlanList *cl = &sp->clips;
  if (cl->count == 0) {
    ADD_PROBLEM("- no clips were returned\n");
    return false;
  }

  /* usable clips */
  size_t usable = 0, out_of_range = 0;
  int prev_end = 0;
  for (size_t i = 0; i < cl->count; i++) {
    int s = cl->items[i].start, e = cl->items[i].end;
    bool ok = s > 0 && e > s && s >= prev_end - 1;
    if (movie_dur > 1.0 && e > movie_dur + 1.0) { ok = false; out_of_range++; }
    if (ok) { usable++; prev_end = e; }
  }
  /* fewer clips than asked only makes the video shorter; reject only if far too few */
  size_t need = (size_t)(num_clips * 0.3);
  if (need < 3) need = 3;
  if (usable < need)
    ADD_PROBLEM("- only %zu usable clips (asked for %d); %zu clips had timestamps past the end of the movie\n",
                usable, num_clips, out_of_range);

  /* repeated sentences (the "endless loop" failure) */
  size_t cap = 64, ns = 0;
  char **sents = (char **)malloc(cap * sizeof(char *));
  if (!sents) die("OOM");
  size_t identical_narrations = 0;
  for (size_t i = 0; i < cl->count; i++) {
    const char *t = cl->items[i].narration;
    for (size_t k = 0; k < i; k++) if (strcmp(cl->items[k].narration, t) == 0) { identical_narrations++; break; }
    const char *st = t;
    bool first_sentence = true;
    for (const char *q = t;; q++) {
      if (*q == '.' || *q == '!' || *q == '?' || *q == 0) {
        char norm[512];
        int words = 0;
        normalize_sentence(st, (size_t)(q - st), norm, sizeof(norm), &words);
        bool intro = (i == 0 && first_sentence);
        if (words >= 2 && !intro) {
          if (ns + 1 > cap) { cap *= 2; sents = (char **)xrealloc(sents, cap * sizeof(char *)); }
          sents[ns++] = str_dup(norm);
        }
        if (words > 0) first_sentence = false;
        if (*q == 0) break;
        st = q + 1;
      }
    }
  }
  qsort(sents, ns, sizeof(char *), cmp_strp);
  size_t repeats = 0;
  const char *example = NULL;
  for (size_t i = 1; i < ns; i++) {
    if (strcmp(sents[i], sents[i - 1]) == 0) { repeats++; if (!example) example = sents[i]; }
  }
  if (identical_narrations > 0 || (repeats >= 2 && repeats * 20 > ns)) {
    ADD_PROBLEM("- %zu repeated sentences and %zu identical narrations (it is looping), e.g. \"%.120s\"\n",
                repeats, identical_narrations, example ? example : cl->items[0].narration);
  }
  for (size_t i = 0; i < ns; i++) free(sents[i]);
  free(sents);

  /* generic filler */
  size_t generic = 0;
  const char *gex = NULL;
  for (size_t i = 0; i < cl->count; i++) {
    for (int g = 0; GENERIC_PHRASES[g]; g++) {
      if (strcasestr_local(cl->items[i].narration, GENERIC_PHRASES[g])) { generic++; if (!gex) gex = GENERIC_PHRASES[g]; break; }
    }
  }
  if (generic > 2 && generic * 5 > cl->count)
    ADD_PROBLEM("- %zu of %zu narrations use generic trailer phrases (e.g. \"%s\") instead of describing the movie\n",
                generic, cl->count, gex);

  /* grounding: characters must come from the subtitles, and be used */
  if (sp->n_chars == 0) {
    ADD_PROBLEM("- no main characters were named\n");
  } else {
    size_t found = 0;
    char missing[512] = {0};
    for (size_t i = 0; i < sp->n_chars; i++) {
      if (name_appears(sp->chars[i], dialogue)) found++;
      else if (strlen(missing) + strlen(sp->chars[i]) + 3 < sizeof(missing)) {
        if (missing[0]) strcat(missing, ", ");
        strcat(missing, sp->chars[i]);
      }
    }
    if (found * 2 < sp->n_chars)
      ADD_PROBLEM("- most of the named characters never appear in the subtitles (%s) - the story was not taken from them\n",
                  missing);
    size_t mentions = 0;
    for (size_t i = 0; i < cl->count; i++) {
      for (size_t k = 0; k < sp->n_chars; k++) {
        if (name_appears(sp->chars[k], dialogue) && name_appears(sp->chars[k], cl->items[i].narration)) { mentions++; break; }
      }
    }
    if (found > 0 && mentions * 4 < cl->count)
      ADD_PROBLEM("- only %zu of %zu narrations mention any of the main characters by name\n", mentions, cl->count);
  }
#undef ADD_PROBLEM
  return problems[0] == 0;
}

/* output/<Title>_script.txt so the recap can be read before/after rendering */
static void write_script_file(const char *path, const char *movie_title, const StoryPlan *sp, const char *problems) {
  FILE *f = plat_fopen(path, "wb");
  if (!f) { logw("Could not write %s", path); return; }
  fprintf(f, "%s - recap script\n\n", movie_title);
  if (problems && problems[0]) fprintf(f, "REJECTED because:\n%s\n", problems);
  if (sp->n_chars) {
    fprintf(f, "Main characters: ");
    for (size_t i = 0; i < sp->n_chars; i++) fprintf(f, "%s%s", i ? ", " : "", sp->chars[i]);
    fprintf(f, "\n");
  }
  if (sp->summary) fprintf(f, "Summary: %s\n", sp->summary);
  fprintf(f, "\n");
  for (size_t i = 0; i < sp->clips.count; i++) {
    char a[32], b[32];
    fmt_mmss(sp->clips.items[i].start, a, sizeof(a));
    fmt_mmss(sp->clips.items[i].end, b, sizeof(b));
    fprintf(f, "[%s - %s]  %s\n\n", a, b, sp->clips.items[i].narration);
  }
  fclose(f);
}

static bool elevenlabs_tts_to_mp3(const Config *cfg, const char *text, const char *out_mp3_path) {
  char url[1024];
  snprintf(url, sizeof(url),
           "https://api.elevenlabs.io/v1/text-to-speech/%s?output_format=mp3_44100_128",
           cfg->eleven_voice_id);

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

static bool ffmpeg_make_adjusted_clip(const char *input_mp4, int start_s, int end_s,
                                      const char *narration_mp3, double narration_dur,
                                      const char *out_mp4) {
  double orig_seg_dur = (double)(end_s - start_s);
  if (orig_seg_dur <= 0.1 || narration_dur <= 0.1) return false;

  int use_start = start_s;
  int use_end   = end_s;

  double speed = orig_seg_dur / narration_dur;

  if (speed > MAX_VIDEO_SPEEDUP) {
    double desired_src_dur = narration_dur * MAX_VIDEO_SPEEDUP;

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
    if (speed > MAX_VIDEO_SPEEDUP) speed = MAX_VIDEO_SPEEDUP;

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

static bool ffmpeg_mix_bgm(const char *video_in, const char *bgm_in, const char *video_out) {
  char *v_esc = sh_escape(video_in);
  char *b_esc = sh_escape(bgm_in);
  char *o_esc = sh_escape(video_out);

  int rc = run_cmd(
    "ffmpeg -y -hide_banner -loglevel error "
    "-i %s -i %s "
    "-filter_complex \"[0:a]volume=2.5[a0];[1:a]volume=0.1[a1];"
    "[a0][a1]amix=inputs=2:duration=first:dropout_transition=2[a]\" "
    "-map 0:v -map \"[a]\" "
    "-c:v copy -c:a aac -b:a 192k -movflags +faststart %s",
    v_esc, b_esc, o_esc
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
                          int num_clips) {
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

  bool downloaded_now = false;
  if (!file_exists(srt_in)) {
    downloaded_now = true;
    logi("No SRT found for %s; attempting download...", movie_title);
    if (!download_subtitle_srt(movie_title, srt_in)) {
      logw("Subtitle download failed for %s. Place your SRT at: %s", movie_title, srt_in);
      return false;
    }
    logok("Downloaded SRT: %s", srt_in);
  } else {
    logok("Found SRT: %s", srt_in);
  }

  /* Decode + parse the subtitles every run (never trust a cached conversion). */
  const char *enc = "?";
  char *srt_raw = read_text_file_utf8(srt_in, &enc);
  if (!srt_raw) {
    logw("Could not read %s", srt_in);
    return false;
  }
  SubList subs = parse_srt_text(srt_raw);
  free(srt_raw);

  double movie_dur = ffprobe_duration_seconds(movie_path);
  {
    char a[32], b[32];
    fmt_mmss(subs.last_end, a, sizeof(a));
    fmt_mmss(movie_dur > 0 ? movie_dur : 0, b, sizeof(b));
    logi("Subtitles: %zu lines of dialogue, %zu characters, encoding %s, last line at %s (movie length %s)",
         subs.count, subs.dialogue_chars, enc, a, b);
  }

  char why[512];
  if (!check_subtitles(&subs, movie_dur, why, sizeof(why))) {
    logw("STOPPED before calling OpenAI: %s.", why);
    if (downloaded_now) {
      /* keep it for inspection, but out of the way so the next run downloads again */
      char bad[PATH_MAX];
      snprintf(bad, sizeof(bad), "scripts/srt_files/%s.srt.bad", movie_title);
      plat_rename(srt_in, bad);
      logw("The downloaded subtitle was moved to %s.", bad);
    }
    logw("Fix: put a correct English .srt for this exact movie at %s and press START again.", srt_in);
    free_sub_list(&subs);
    return false;
  }
  for (size_t i = 0; i < subs.count && i < 3; i++) {
    size_t k = subs.count / 2 + i; /* a few lines from the middle, as proof the dialogue was read */
    if (k >= subs.count) break;
    logi("  sample %d-%ds: %.120s", (int)subs.items[k].start, (int)subs.items[k].end, subs.items[k].text);
  }

  char *subs_seconds = sub_list_to_planning_text(&subs);
  if (!write_entire_file(srt_mod, subs_seconds, strlen(subs_seconds)))
    logw("Could not write %s (continuing)", srt_mod);
  else
    logok("Subtitles prepared for planning: %s", srt_mod);

  /* all dialogue in one string, used to check the AI's script against the movie */
  char *dialogue = NULL;
  {
    size_t dl = 0, dc = 0;
    for (size_t i = 0; i < subs.count; i++) {
      sb_append(&dialogue, &dl, &dc, subs.items[i].text, strlen(subs.items[i].text));
      sb_append(&dialogue, &dl, &dc, "\n", 1);
    }
    if (!dialogue) dialogue = str_dup("");
  }
  free_sub_list(&subs);

  long sz = file_size_bytes(script_txt);
  if (sz >= 0 && sz < 200) {
    logw("IMSDb script file looks too small (%ld bytes). Deleting to retry: %s", sz, script_txt);
    plat_unlink(script_txt);
  }

  char imsdb_url[1024] = {0};
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

  const char *script_ctx = (imsdb_script && imsdb_script[0]) ? imsdb_script : "";

  logi("Requesting the recap script from OpenAI (%s, %d clips)...", cfg->openai_model, num_clips);
  bool retry_no_script = false;
  char *plan_json = openai_request_plan(cfg, movie_title, subs_seconds, script_ctx, num_clips, movie_dur,
                                        NULL, NULL, &retry_no_script);
  if (!plan_json && retry_no_script && script_ctx[0]) {
    logw("OpenAI request failed with IMSDb context; retrying without IMSDb script for %s", movie_title);
    script_ctx = "";
    plan_json = openai_request_plan(cfg, movie_title, subs_seconds, "", num_clips, movie_dur, NULL, NULL, NULL);
  }

  StoryPlan sp = parse_story_plan(plan_json);
  char problems[2048];
  bool plan_ok = plan_json && validate_story_plan(&sp, dialogue, movie_dur, num_clips, problems, sizeof(problems));

  if (plan_json && !plan_ok) {
    logw("The AI's script failed the quality check:");
    for (char *ln = strtok(problems, "\n"); ln; ln = strtok(NULL, "\n")) logw("  %s", ln);
    /* strtok destroyed `problems`; rebuild it for the feedback message */
    validate_story_plan(&sp, dialogue, movie_dur, num_clips, problems, sizeof(problems));
    logi("Asking OpenAI to rewrite it (1 retry)...");
    char *plan_json2 = openai_request_plan(cfg, movie_title, subs_seconds, script_ctx, num_clips, movie_dur,
                                           plan_json, problems, NULL);
    if (plan_json2) {
      free_story_plan(&sp);
      free(plan_json);
      plan_json = plan_json2;
      sp = parse_story_plan(plan_json);
      plan_ok = validate_story_plan(&sp, dialogue, movie_dur, num_clips, problems, sizeof(problems));
    }
  }

  free(subs_seconds);
  free(dialogue);
  if (imsdb_script) free(imsdb_script);

  char script_path[PATH_MAX];
  if (!plan_json) {
    logw("No plan returned for %s", movie_title);
    free_story_plan(&sp);
    return false;
  }
  free(plan_json);

  if (!plan_ok) {
    snprintf(script_path, sizeof(script_path), "output/%s_REJECTED_script.txt", movie_title);
    write_script_file(script_path, movie_title, &sp, problems);
    logw("STOPPED: the AI's script still isn't based on the movie after a retry, so no video was made");
    logw("(no ElevenLabs credits were spent). Rejected script: %s", script_path);
    for (char *ln = strtok(problems, "\n"); ln; ln = strtok(NULL, "\n")) logw("  %s", ln);
    free_story_plan(&sp);
    return false;
  }

  snprintf(script_path, sizeof(script_path), "output/%s_script.txt", movie_title);
  write_script_file(script_path, movie_title, &sp, NULL);
  logok("Recap script accepted: %zu clips. Saved to %s", sp.clips.count, script_path);
  if (sp.n_chars) {
    char names[512] = {0};
    for (size_t i = 0; i < sp.n_chars; i++) {
      if (strlen(names) + strlen(sp.chars[i]) + 3 >= sizeof(names)) break;
      if (i) strcat(names, ", ");
      strcat(names, sp.chars[i]);
    }
    logi("Characters: %s", names);
  }
  if (sp.summary) logi("Story: %.400s", sp.summary);

  ClipPlanList plan = sp.clips;
  sp.clips.items = NULL;
  sp.clips.count = 0;
  free_story_plan(&sp);

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
    int start_s = plan.items[i].start;
    int end_s   = plan.items[i].end;
    if (start_s <= 0) { logw("Skipping clip %zu (start<=0)", i + 1); continue; }
    if (end_s <= start_s) { logw("Skipping clip %zu (end<=start)", i + 1); continue; }

    char nar_mp3[PATH_MAX];
    snprintf(nar_mp3, sizeof(nar_mp3), "clips/audio/%s_audio_%zu.mp3", movie_title, i + 1);

    logi("TTS clip %zu/%zu -> %s", i + 1, plan.count, nar_mp3);
    if (!elevenlabs_tts_to_mp3(cfg, plan.items[i].narration, nar_mp3)) {
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

    logi("Building clip %zu: %d -> %d sec (narr=%.2fs) => %s", i + 1, start_s, end_s, nar_dur, out_clip);
    if (!ffmpeg_make_adjusted_clip(movie_path, start_s, end_s, nar_mp3, nar_dur, out_clip)) {
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

  size_t song_n = 0;
  char **songs = list_files_with_ext("backgroundmusic", ".mp3", ".m4a", &song_n);
  if (!songs || song_n == 0) {
    logw("No backgroundmusic files found; output will be narration-only.");
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
      if (!ffmpeg_mix_bgm(tmp_concat, bgm_out, out_final_only)) {
        logw("Mix failed; output narration-only.");
        plat_rename(tmp_concat, out_final_only);
      } else {
        plat_unlink(tmp_concat);
      }
      logok("Wrote output: %s", out_final_only);
    }

    free_str_list(songs, song_n);
  }

  char out_final[PATH_MAX], out_vert[PATH_MAX];
  snprintf(out_final, sizeof(out_final), "output/%s.mp4", movie_title);
  snprintf(out_vert,  sizeof(out_vert),  "tiktok_output/%s_vertical.mp4", movie_title);

  logi("Rendering vertical -> %s", out_vert);
  if (!ffmpeg_make_vertical(out_final, out_vert)) {
    logw("Vertical render failed for %s", movie_title);
  } else {
    logok("Vertical render OK: %s", out_vert);
  }

  char retired[PATH_MAX];
  snprintf(retired, sizeof(retired), "movies_retired/%s.mp4", movie_title);
  if (plat_rename(movie_path, retired) == 0) {
    logok("Retired source movie -> %s", retired);
  } else {
    logw("Could not move %s -> %s (is the file open in another program?)", movie_path, retired);
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
    emit_line("Generation aborted (see FATAL message above).");
    return -1;
  }
  g_die_armed = true;

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
  int num_clips = MIN_NUM_CLIPS + (rand() % (MAX_NUM_CLIPS - MIN_NUM_CLIPS + 1));
  if (cfg.target_minutes > 0) {
    /* each clip is ~10 s of narration (20-35 words) */
    num_clips = (int)lround(cfg.target_minutes * 6.0);
    if (num_clips < 5) num_clips = 5;
    if (num_clips > 200) num_clips = 200;
    logi("target_minutes = %.1f -> %d clips", cfg.target_minutes, num_clips);
  }

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
    if (process_movie(&cfg, path, title, num_clips)) {
      processed++;
      snprintf(banner, sizeof(banner), "DONE: %s", title);
    } else {
      snprintf(banner, sizeof(banner), "FAILED: %s", title);
    }
    emit_line(banner);
  }
  free_str_list(names, n_names);

  char done[128];
  snprintf(done, sizeof(done), "All done. Processed: %d", processed);
  emit_line("");
  emit_line(done);

  g_die_armed = false;
  curl_global_cleanup();
  g_curl_inited = false;
  return processed; /* 0 is also a valid "nothing to do" result */
}
