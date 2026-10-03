/*
 * web_ui.c - browser control panel (movie_summary_web).
 *
 * A tiny dependency-free HTTP server that exposes the whole pipeline:
 *   - start / cancel a generation run
 *   - edit config.json (API keys, voice, model, clip count, speed cap,
 *     background-music volumes, vertical render, movie retiring, API base URLs)
 *   - list / upload / delete movies and subtitle files
 *   - watch the live log and the current stage / clip progress
 *   - preview and download output\tiktok_output videos (HTTP range requests)
 *
 * Everything OS specific goes through platform.h; the only extra system
 * dependency is the socket API (Winsock on Windows, BSD sockets elsewhere).
 *
 * Usage:  movie_summary_web [--host 127.0.0.1] [--port 8080]
 */

#include "generator.h"
#include "platform.h"
#include "cJSON.h"

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
  typedef SOCKET plat_sock_t;
  #define SOCK_INVALID INVALID_SOCKET
  #define SOCK_ERRNO   WSAGetLastError()
  static void sock_close(plat_sock_t s) { closesocket(s); }
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <errno.h>
  typedef int plat_sock_t;
  #define SOCK_INVALID (-1)
  #define SOCK_ERRNO   errno
  static void sock_close(plat_sock_t s) { close(s); }
#endif

#include <ctype.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ============================== shared state ============================== */

#define LOG_MAX_LINES 2000
#define LOG_LINE_MAX  1024
#define MAX_BODY      (32u * 1024u * 1024u)   /* JSON bodies */
#define MAX_UPLOAD    (64ull * 1024ull * 1024ull * 1024ull) /* streamed, 64 GB */

typedef struct {
  PlatMutex *lock;

  char       lines[LOG_MAX_LINES][LOG_LINE_MAX];
  long long  seqs[LOG_MAX_LINES];
  int        head;      /* next write slot */
  int        count;
  long long  next_seq;

  volatile int running;
  volatile int last_rc;
  volatile int have_rc;
  time_t     started_at;
  time_t     finished_at;

  GeneratorProgress progress;
} AppState;

static AppState g_app;

static void app_lock(void)   { plat_mutex_lock(g_app.lock); }
static void app_unlock(void) { plat_mutex_unlock(g_app.lock); }

static void hook_log(const char *line) {
  if (!line) return;
  app_lock();
  snprintf(g_app.lines[g_app.head], LOG_LINE_MAX, "%s", line);
  g_app.seqs[g_app.head] = ++g_app.next_seq;
  g_app.head = (g_app.head + 1) % LOG_MAX_LINES;
  if (g_app.count < LOG_MAX_LINES) g_app.count++;
  app_unlock();

  /* generator.c already mirrors every line to stderr, so do not print it again */
}

static void hook_progress(const GeneratorProgress *p) {
  if (!p) return;
  app_lock();
  g_app.progress = *p;
  app_unlock();
}

static void worker_thread(void *arg) {
  (void)arg;
  generator_set_log_hook(hook_log);
  generator_set_progress_hook(hook_progress);
  int rc = run_generation();
  generator_set_log_hook(NULL);
  generator_set_progress_hook(NULL);

  app_lock();
  g_app.last_rc = rc;
  g_app.have_rc = 1;
  g_app.finished_at = time(NULL);
  g_app.running = 0;
  app_unlock();
}

/* ============================== small helpers ============================== */

/* Portable case-insensitive compare (strcasecmp is not in MSVC). */
static int strcasecmp_local(const char *a, const char *b) {
  for (;; a++, b++) {
    int ca = tolower((unsigned char)*a), cb = tolower((unsigned char)*b);
    if (ca != cb || ca == 0) return ca - cb;
  }
}

static int strncasecmp_local(const char *a, const char *b, size_t n) {
  for (size_t i = 0; i < n; i++, a++, b++) {
    int ca = tolower((unsigned char)*a), cb = tolower((unsigned char)*b);
    if (ca != cb || ca == 0) return ca - cb;
  }
  return 0;
}

static char *xstrdup(const char *s) {
  size_t n = strlen(s) + 1;
  char *p = (char *)malloc(n);
  if (p) memcpy(p, s, n);
  return p;
}

typedef struct { char *p; size_t len, cap; } Buf;

static void buf_init(Buf *b) { b->p = NULL; b->len = 0; b->cap = 0; }

static bool buf_reserve(Buf *b, size_t extra) {
  if (b->len + extra + 1 <= b->cap) return true;
  size_t nc = b->cap ? b->cap : 8192;
  while (nc < b->len + extra + 1) nc *= 2;
  char *q = (char *)realloc(b->p, nc);
  if (!q) return false;
  b->p = q;
  b->cap = nc;
  b->p[b->len] = 0;
  return true;
}

static bool buf_add(Buf *b, const void *data, size_t n) {
  if (!buf_reserve(b, n)) return false;
  memcpy(b->p + b->len, data, n);
  b->len += n;
  b->p[b->len] = 0;
  return true;
}

static bool buf_addf(Buf *b, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int need = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  if (need < 0) return false;

  if (!buf_reserve(b, (size_t)need)) return false;
  va_start(ap, fmt);
  vsnprintf(b->p + b->len, (size_t)need + 1, fmt, ap);
  va_end(ap);
  b->len += (size_t)need;
  return true;
}

/* Replace bytes that are not valid UTF-8 with '?' so JSON stays valid. */
static void sanitize_utf8(char *s) {
  unsigned char *u = (unsigned char *)s;
  for (size_t i = 0; u[i]; ) {
    unsigned char c = u[i];
    size_t extra = 0;
    if (c < 0x80) { i++; continue; }
    else if (c >= 0xC2 && c <= 0xDF) extra = 1;
    else if (c >= 0xE0 && c <= 0xEF) extra = 2;
    else if (c >= 0xF0 && c <= 0xF4) extra = 3;
    else { u[i] = '?'; i++; continue; }

    bool ok = true;
    for (size_t k = 1; k <= extra; k++) {
      if ((u[i + k] & 0xC0) != 0x80) { ok = false; break; }
    }
    if (ok) i += extra + 1;
    else { u[i] = '?'; i++; }
  }
}

/* JSON string that is safe even for odd file names / log output. */
static void json_str(cJSON *obj, const char *key, const char *val) {
  char tmp[4096];
  snprintf(tmp, sizeof(tmp), "%s", val ? val : "");
  sanitize_utf8(tmp);
  cJSON_AddStringToObject(obj, key, tmp);
}

static long long file_size_at(const char *p) {
  long long sz = -1;
  if (plat_stat(p, &sz) != PLAT_FILE) return -1;
  return sz;
}

static char *read_whole_file(const char *path, size_t *out_len) {
  FILE *f = plat_fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (n < 0) { fclose(f); return NULL; }

  char *buf = (char *)malloc((size_t)n + 1);
  if (!buf) { fclose(f); return NULL; }
  size_t got = fread(buf, 1, (size_t)n, f);
  fclose(f);
  buf[got] = 0;
  if (out_len) *out_len = got;
  return buf;
}

static bool write_whole_file(const char *path, const char *data, size_t len) {
  FILE *f = plat_fopen(path, "wb");
  if (!f) return false;
  bool ok = (fwrite(data, 1, len, f) == len);
  fclose(f);
  return ok;
}

/* ============================ folder white list ============================ */

typedef struct { const char *key; const char *path; const char *label; } DirEntry;

static const DirEntry DIRS[] = {
  { "movies",         "movies",            "Movies (input)"        },
  { "movies_retired", "movies_retired",    "Retired movies"        },
  { "output",         "output",            "Output"                },
  { "tiktok_output",  "tiktok_output",     "Vertical output"       },
  { "srt",            "scripts/srt_files", "Subtitles / scripts"   },
  { "bgm",            "backgroundmusic",   "Background music"      },
  { "clips",          "clips",             "Temp clips"            },
  { NULL, NULL, NULL }
};

static const DirEntry *dir_find(const char *key) {
  if (!key) return NULL;
  for (int i = 0; DIRS[i].key; i++) {
    if (strcmp(DIRS[i].key, key) == 0) return &DIRS[i];
  }
  return NULL;
}

/* File names coming from the network must never escape the project folder. */
static bool name_is_safe(const char *name) {
  if (!name || !name[0]) return false;
  if (strlen(name) > 240) return false;
  if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return false;
  for (const char *p = name; *p; p++) {
    if (*p == '/' || *p == '\\' || *p == ':') return false;
    if ((unsigned char)*p < 0x20) return false;
  }
  if (strstr(name, "..")) return false;
  return true;
}

/* ============================== HTTP plumbing ============================== */

typedef struct {
  plat_sock_t s;
  char  method[16];
  char  path[2048];
  char  query[2048];
  char *buf;
  size_t len, cap;
  size_t hdr_end;
  long long content_length;
  bool have_content_length;
  bool expect_100;
} Req;

#ifdef MSG_NOSIGNAL
  #define SEND_FLAGS MSG_NOSIGNAL   /* Linux: a closed peer must not raise SIGPIPE */
#else
  #define SEND_FLAGS 0
#endif

static bool send_all(plat_sock_t s, const char *data, size_t n) {
  size_t off = 0;
  while (off < n) {
    int k = send(s, data + off, (int)(n - off > 65536 ? 65536 : n - off), SEND_FLAGS);
    if (k <= 0) {
      int e = SOCK_ERRNO;
#ifdef _WIN32
      if (e == WSAEINTR) continue;
#else
      if (e == EINTR) continue;
#endif
      return false;
    }
    off += (size_t)k;
  }
  return true;
}

static bool req_grow(Req *r, size_t extra) {
  if (r->len + extra + 1 <= r->cap) return true;
  size_t nc = r->cap ? r->cap : 8192;
  while (nc < r->len + extra + 1) nc *= 2;
  char *q = (char *)realloc(r->buf, nc);
  if (!q) return false;
  r->buf = q;
  r->cap = nc;
  r->buf[r->len] = 0;
  return true;
}

static const char *find_bytes(const char *hay, size_t n, const char *needle) {
  size_t nl = strlen(needle);
  if (nl == 0 || n < nl) return NULL;
  for (size_t i = 0; i + nl <= n; i++) {
    if (memcmp(hay + i, needle, nl) == 0) return hay + i;
  }
  return NULL;
}

static bool req_read_headers(Req *r) {
  for (;;) {
    const char *end = find_bytes(r->buf ? r->buf : "", r->len, "\r\n\r\n");
    if (end) {
      r->hdr_end = (size_t)(end - r->buf) + 4;
      return true;
    }
    if (r->len > 64 * 1024) return false;
    if (!req_grow(r, 8192)) return false;

    int k = recv(r->s, r->buf + r->len, (int)(r->cap - r->len - 1), 0);
    if (k <= 0) return false;
    r->len += (size_t)k;
    r->buf[r->len] = 0;
  }
}

/* Case-insensitive header lookup inside the header block. */
static bool req_header(const Req *r, const char *name, char *out, size_t outsz) {
  if (!r->buf || r->hdr_end < 4) return false;
  size_t n = r->hdr_end - 2; /* drop the final CRLF */
  size_t nlen = strlen(name);

  const char *first_eol = find_bytes(r->buf, n, "\r\n");
  if (!first_eol) return false;
  size_t i = (size_t)(first_eol - r->buf) + 2; /* skip the request line */

  while (i < n) {
    const char *eol = find_bytes(r->buf + i, n - i, "\r\n");
    size_t line_len = eol ? (size_t)(eol - (r->buf + i)) : (n - i);

    if (line_len > nlen && r->buf[i + nlen] == ':' &&
        strncasecmp_local(r->buf + i, name, nlen) == 0) {
      size_t v = i + nlen + 1;
      while (v < i + line_len && (r->buf[v] == ' ' || r->buf[v] == '\t')) v++;
      size_t vlen = (i + line_len) - v;
      if (vlen >= outsz) vlen = outsz - 1;
      memcpy(out, r->buf + v, vlen);
      out[vlen] = 0;
      return true;
    }
    if (!eol) break;
    i += line_len + 2;
  }
  return false;
}

static void req_parse_request_line(Req *r) {
  r->method[0] = r->path[0] = r->query[0] = 0;

  const char *nl = find_bytes(r->buf, r->hdr_end, "\r\n");
  if (!nl) return;
  size_t n = (size_t)(nl - r->buf);

  size_t sp1 = 0, sp2 = 0;
  bool f1 = false, f2 = false;
  for (size_t i = 0; i < n; i++) {
    if (r->buf[i] == ' ') {
      if (!f1) { sp1 = i; f1 = true; }
      else if (!f2) { sp2 = i; f2 = true; break; }
    }
  }
  if (!f1) return;
  if (!f2) sp2 = n;

  size_t ml = sp1; if (ml >= sizeof(r->method)) ml = sizeof(r->method) - 1;
  memcpy(r->method, r->buf, ml); r->method[ml] = 0;

  size_t tl = sp2 - sp1 - 1;
  char target[4096];
  if (tl >= sizeof(target)) tl = sizeof(target) - 1;
  memcpy(target, r->buf + sp1 + 1, tl);
  target[tl] = 0;

  char *q = strchr(target, '?');
  if (q) {
    *q = 0;
    snprintf(r->query, sizeof(r->query), "%s", q + 1);
  }
  snprintf(r->path, sizeof(r->path), "%s", target);
}

static void url_decode(char *s) {
  char *w = s;
  for (char *p = s; *p; ) {
    if (*p == '+') { *w++ = ' '; p++; }
    else if (*p == '%' && p[1] && p[2]) {
      char hex[3] = { p[1], p[2], 0 };
      *w++ = (char)strtol(hex, NULL, 16);
      p += 3;
    } else {
      *w++ = *p++;
    }
  }
  *w = 0;
}

/* value of ?key=... in the query string (decoded), or NULL */
static const char *query_get(Req *r, const char *key, char *out, size_t outsz) {
  size_t kl = strlen(key);
  const char *p = r->query;
  while (p && *p) {
    const char *amp = strchr(p, '&');
    size_t seglen = amp ? (size_t)(amp - p) : strlen(p);

    if (seglen > kl && p[kl] == '=' && strncmp(p, key, kl) == 0) {
      size_t vl = seglen - kl - 1;
      if (vl >= outsz) vl = outsz - 1;
      memcpy(out, p + kl + 1, vl);
      out[vl] = 0;
      url_decode(out);
      return out;
    }
    if (!amp) break;
    p = amp + 1;
  }
  return NULL;
}

/* Read the request body into memory (for the JSON APIs). */
static char *req_read_body(Req *r, size_t *out_len) {
  if (out_len) *out_len = 0;
  if (!r->have_content_length || r->content_length <= 0) {
    char *e = (char *)malloc(1);
    if (e) e[0] = 0;
    return e;
  }
  if (r->content_length > (long long)MAX_BODY) return NULL;

  Buf b; buf_init(&b);
  size_t already = r->len - r->hdr_end;
  if (already > 0) {
    size_t use = already > (size_t)r->content_length ? (size_t)r->content_length : already;
    if (!buf_add(&b, r->buf + r->hdr_end, use)) { free(b.p); return NULL; }
  }

  while ((long long)b.len < r->content_length) {
    char tmp[65536];
    size_t want = (size_t)(r->content_length - (long long)b.len);
    if (want > sizeof(tmp)) want = sizeof(tmp);
    int k = recv(r->s, tmp, (int)want, 0);
    if (k <= 0) { free(b.p); return NULL; }
    if (!buf_add(&b, tmp, (size_t)k)) { free(b.p); return NULL; }
  }
  if (out_len) *out_len = b.len;
  return b.p; /* caller frees */
}

static const char *status_text(int code) {
  switch (code) {
    case 200: return "OK";
    case 206: return "Partial Content";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 413: return "Payload Too Large";
    case 500: return "Internal Server Error";
    default:  return "OK";
  }
}

static void http_head(Req *r, int code, const char *ctype, long long body_len,
                      const char *extra) {
  Buf h; buf_init(&h);
  buf_addf(&h, "HTTP/1.1 %d %s\r\n", code, status_text(code));
  buf_addf(&h, "Content-Type: %s\r\n", ctype);
  buf_addf(&h, "Content-Length: %lld\r\n", body_len);
  buf_addf(&h, "Cache-Control: no-store\r\n");
  buf_addf(&h, "Accept-Ranges: bytes\r\n");
  buf_addf(&h, "Access-Control-Allow-Origin: *\r\n");
  if (extra) buf_addf(&h, "%s", extra);
  buf_addf(&h, "Connection: close\r\n\r\n");
  send_all(r->s, h.p, h.len);
  free(h.p);
}

static void http_respond(Req *r, int code, const char *ctype, const char *body, size_t len) {
  http_head(r, code, ctype, (long long)len, NULL);
  if (len) send_all(r->s, body, len);
}

static void http_json(Req *r, int code, cJSON *root) {
  char *txt = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (!txt) { http_respond(r, 500, "text/plain; charset=utf-8", "json error", 10); return; }
  http_respond(r, code, "application/json; charset=utf-8", txt, strlen(txt));
  free(txt);
}

static void http_error(Req *r, int code, const char *msg) {
  cJSON *o = cJSON_CreateObject();
  cJSON_AddBoolToObject(o, "ok", false);
  json_str(o, "error", msg ? msg : "error");
  http_json(r, code, o);
}

static void http_ok(Req *r) {
  cJSON *o = cJSON_CreateObject();
  cJSON_AddBoolToObject(o, "ok", true);
  http_json(r, 200, o);
}

/* =============================== folder APIs =============================== */

static cJSON *list_dir_json(const char *dir) {
  cJSON *arr = cJSON_CreateArray();
  PlatDir *d = plat_opendir(dir);
  if (!d) return arr;

  const char *name;
  bool is_dir = false;
  while ((name = plat_readdir(d, &is_dir))) {
    if (name[0] == '.') continue;
    char full[4096];
    snprintf(full, sizeof(full), "%s/%s", dir, name);
    if (plat_stat(full, NULL) == PLAT_DIR) continue;

    cJSON *e = cJSON_CreateObject();
    json_str(e, "name", name);
    cJSON_AddNumberToObject(e, "size", (double)file_size_at(full));
    cJSON_AddItemToArray(arr, e);
  }
  plat_closedir(d);
  return arr;
}

static int count_files(const char *dir, const char *ext1, const char *ext2) {
  int n = 0;
  PlatDir *d = plat_opendir(dir);
  if (!d) return 0;
  const char *name;
  while ((name = plat_readdir(d, NULL))) {
    if (name[0] == '.') continue;
    size_t ln = strlen(name);
    bool ok = false;
    if (ext1) { size_t e = strlen(ext1); if (ln >= e && strcasecmp_local(name + ln - e, ext1) == 0) ok = true; }
    if (!ok && ext2) { size_t e = strlen(ext2); if (ln >= e && strcasecmp_local(name + ln - e, ext2) == 0) ok = true; }
    if (!ok) continue;

    char full[4096];
    snprintf(full, sizeof(full), "%s/%s", dir, name);
    if (plat_stat(full, NULL) == PLAT_FILE) n++;
  }
  plat_closedir(d);
  return n;
}

/* ================================ config API =============================== */

static const char *CONFIG_PATH = "config.json";

static void mask_key(const char *in, char *out, size_t outsz) {
  if (!in || !in[0]) { snprintf(out, outsz, "%s", ""); return; }
  size_t n = strlen(in);
  if (n <= 8) { snprintf(out, outsz, "********"); return; }
  char head[8], tail[8];
  snprintf(head, sizeof(head), "%s", in);
  snprintf(tail, sizeof(tail), "%s", in + n - 4);
  snprintf(out, outsz, "%s...%s", head, tail);
}

static cJSON *config_read_json(void) {
  cJSON *o = cJSON_CreateObject();
  size_t len = 0;
  char *txt = read_whole_file(CONFIG_PATH, &len);
  cJSON *root = txt ? cJSON_Parse(txt) : NULL;
  free(txt);

  cJSON_AddBoolToObject(o, "exists", root != NULL);
  if (!root) {
    cJSON_AddBoolToObject(o, "parse_ok", false);
    return o;
  }
  cJSON_AddBoolToObject(o, "parse_ok", true);

  const cJSON *k1 = cJSON_GetObjectItemCaseSensitive(root, "open_api_key");
  const cJSON *k2 = cJSON_GetObjectItemCaseSensitive(root, "elevenlabs_api_key");

  const cJSON *k3 = cJSON_GetObjectItemCaseSensitive(root, "tts_api_key");

  char m1[128], m2[128], m3[128];
  mask_key(cJSON_IsString(k1) ? k1->valuestring : "", m1, sizeof(m1));
  mask_key(cJSON_IsString(k2) ? k2->valuestring : "", m2, sizeof(m2));
  mask_key(cJSON_IsString(k3) ? k3->valuestring : "", m3, sizeof(m3));

  const cJSON *s;
  json_str(o, "open_api_key_masked", m1);
  json_str(o, "elevenlabs_api_key_masked", m2);
  json_str(o, "tts_api_key_masked", m3);
  cJSON_AddBoolToObject(o, "tts_key_set", cJSON_IsString(k3) && k3->valuestring[0]);
  cJSON_AddBoolToObject(o, "openai_key_set",
                        cJSON_IsString(k1) && k1->valuestring[0] && strcmp(k1->valuestring, "OpenAIAPI") != 0);
  cJSON_AddBoolToObject(o, "elevenlabs_key_set",
                        cJSON_IsString(k2) && k2->valuestring[0] && strcmp(k2->valuestring, "ElevenLabsAPI") != 0);

#define ADD_STR(field, key, def) do {                                    \
    s = cJSON_GetObjectItemCaseSensitive(root, key);                     \
    json_str(o, field, cJSON_IsString(s) ? s->valuestring : def);        \
  } while (0)
#define ADD_NUM(field, key, def) do {                                    \
    s = cJSON_GetObjectItemCaseSensitive(root, key);                     \
    cJSON_AddNumberToObject(o, field, cJSON_IsNumber(s) ? s->valuedouble : def); \
  } while (0)
#define ADD_BOOL(field, key, def) do {                                   \
    s = cJSON_GetObjectItemCaseSensitive(root, key);                     \
    cJSON_AddBoolToObject(o, field, cJSON_IsBool(s) ? cJSON_IsTrue(s) : def);    \
  } while (0)

  ADD_STR("eleven_voice_id", "eleven_voice_id", "JBFqnCBsd6RMkjVDRZzb");
  ADD_STR("eleven_model_id", "eleven_model_id", "eleven_multilingual_v2");
  ADD_STR("openai_model",    "openai_model",    "gpt-5.2");
  ADD_STR("openai_base_url", "openai_base_url", "https://api.openai.com/v1");
  ADD_STR("elevenlabs_base_url", "elevenlabs_base_url", "https://api.elevenlabs.io/v1");

  ADD_STR("tts_provider", "tts_provider", "elevenlabs");
  ADD_STR("tts_base_url", "tts_base_url", "");
  ADD_STR("tts_voice",    "tts_voice",    "");
  ADD_STR("tts_language", "tts_language", "en");
  ADD_STR("tts_model",    "tts_model",    "tts-1");
  ADD_STR("whisper_model",  "whisper_model",  "small");

  ADD_NUM("min_clips",         "min_clips",         20);
  ADD_NUM("max_clips",         "max_clips",         30);
  ADD_NUM("max_video_speedup", "max_video_speedup", 1.75);
  ADD_NUM("narration_volume",  "narration_volume",  2.5);
  ADD_NUM("bgm_volume",        "bgm_volume",        0.1);
  ADD_NUM("recap_minutes",     "recap_minutes",     0);

  ADD_BOOL("bgm_enabled",   "bgm_enabled",   true);
  ADD_BOOL("make_vertical", "make_vertical", true);
  ADD_BOOL("captions",        "captions",        true);
  ADD_BOOL("retire_movies", "retire_movies", true);
  ADD_BOOL("auto_transcribe", "auto_transcribe", true);

#undef ADD_STR
#undef ADD_NUM
#undef ADD_BOOL

  cJSON_Delete(root);
  return o;
}

/* Write the known fields back, preserving any other keys already present. */
static bool config_write_json(cJSON *patch, char *err, size_t errsz) {
  cJSON *root = NULL;
  size_t len = 0;
  char *txt = read_whole_file(CONFIG_PATH, &len);
  if (txt) {
    root = cJSON_Parse(txt);
    free(txt);
  }
  if (!root) root = cJSON_CreateObject();

  const char *str_keys[] = {
    "eleven_voice_id", "eleven_model_id", "openai_model",
    "openai_base_url", "elevenlabs_base_url",
    "tts_provider", "tts_base_url", "tts_voice", "tts_language", "tts_model",
    "whisper_model", NULL
  };
  for (int i = 0; str_keys[i]; i++) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(patch, str_keys[i]);
    if (!cJSON_IsString(v)) continue;
    cJSON *cur = cJSON_GetObjectItemCaseSensitive(root, str_keys[i]);
    if (cur) cJSON_SetValuestring(cur, v->valuestring);
    else cJSON_AddStringToObject(root, str_keys[i], v->valuestring);
  }

  /* API keys: an empty value means "keep what is already there". */
  const char *key_pairs[][2] = {
    { "open_api_key",       "open_api_key"       },
    { "elevenlabs_api_key", "elevenlabs_api_key" },
    { "tts_api_key",        "tts_api_key"        },
    { NULL, NULL }
  };
  for (int i = 0; key_pairs[i][0]; i++) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(patch, key_pairs[i][0]);
    if (!cJSON_IsString(v) || !v->valuestring[0]) continue;
    cJSON *cur = cJSON_GetObjectItemCaseSensitive(root, key_pairs[i][1]);
    if (cur) cJSON_SetValuestring(cur, v->valuestring);
    else cJSON_AddStringToObject(root, key_pairs[i][1], v->valuestring);
  }

  const char *num_keys[] = {
    "min_clips", "max_clips", "max_video_speedup", "narration_volume", "bgm_volume",
    "recap_minutes", NULL
  };
  for (int i = 0; num_keys[i]; i++) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(patch, num_keys[i]);
    if (!cJSON_IsNumber(v)) continue;
    cJSON *cur = cJSON_GetObjectItemCaseSensitive(root, num_keys[i]);
    if (cur) cJSON_SetNumberValue(cur, v->valuedouble);
    else cJSON_AddNumberToObject(root, num_keys[i], v->valuedouble);
  }

  const char *bool_keys[] = { "bgm_enabled", "make_vertical", "retire_movies", "captions", "auto_transcribe", NULL };
  for (int i = 0; bool_keys[i]; i++) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(patch, bool_keys[i]);
    if (!cJSON_IsBool(v)) continue;
    cJSON_DeleteItemFromObjectCaseSensitive(root, bool_keys[i]);
    cJSON_AddBoolToObject(root, bool_keys[i], cJSON_IsTrue(v));
  }

  char *out = cJSON_Print(root);
  cJSON_Delete(root);
  if (!out) { snprintf(err, errsz, "could not serialise config.json"); return false; }

  bool ok = write_whole_file(CONFIG_PATH, out, strlen(out));
  free(out);
  if (!ok) snprintf(err, errsz, "could not write %s", CONFIG_PATH);
  return ok;
}

/* ================================ streaming ================================ */

static void http_send_file(Req *r, const char *path, const char *ctype) {
  long long size = file_size_at(path);
  if (size < 0) { http_error(r, 404, "file not found"); return; }

  long long start = 0, end = size - 1;
  bool partial = false;

  char range[256];
  if (req_header(r, "Range", range, sizeof(range)) && strncmp(range, "bytes=", 6) == 0) {
    const char *p = range + 6;
    char *dash = strchr(p, '-');
    if (dash) {
      *dash = 0;
      const char *srest = dash + 1;
      if (*p) start = strtoll(p, NULL, 10);
      if (*srest) end = strtoll(srest, NULL, 10);
      else if (*p) end = size - 1;             /* "bytes=100-"      */
      if (start < 0) start = 0;
      if (end >= size) end = size - 1;
      if (start > end) { start = 0; end = size - 1; }
      else partial = true;
    }
  }

  FILE *f = plat_fopen(path, "rb");
  if (!f) { http_error(r, 500, "cannot open file"); return; }

  long long len = end - start + 1;
  Buf h; buf_init(&h);
  if (partial) {
    buf_addf(&h, "HTTP/1.1 206 Partial Content\r\n");
    buf_addf(&h, "Content-Range: bytes %lld-%lld/%lld\r\n", start, end, size);
  } else {
    buf_addf(&h, "HTTP/1.1 200 OK\r\n");
  }
  buf_addf(&h, "Content-Type: %s\r\n", ctype);
  buf_addf(&h, "Content-Length: %lld\r\n", len);
  buf_addf(&h, "Accept-Ranges: bytes\r\n");
  buf_addf(&h, "Cache-Control: no-store\r\n");
  buf_addf(&h, "Access-Control-Allow-Origin: *\r\n");
  buf_addf(&h, "Connection: close\r\n\r\n");
  bool sent = send_all(r->s, h.p, h.len);
  free(h.p);

  if (sent && fseek(f, (long)start, SEEK_SET) == 0) {
    char tmp[65536];
    long long left = len;
    while (left > 0) {
      size_t want = left > (long long)sizeof(tmp) ? sizeof(tmp) : (size_t)left;
      size_t got = fread(tmp, 1, want, f);
      if (got == 0) break;
      if (!send_all(r->s, tmp, got)) break;
      left -= (long long)got;
    }
  }
  fclose(f);
}

/* ================================= routing ================================= */

/* ======================= embedded single-page front end ======================= */

static const char *PAGE_HTML[] = {
  "<!doctype html>",
  "<html lang='en'>",
  "<head>",
  "<meta charset='utf-8'>",
  "<meta name='viewport' content='width=device-width,initial-scale=1'>",
  "<title>AI Movie Shorts - Control Panel</title>",
  "<style>",
  ":root{--bg:#0d1014;--panel:#151a21;--panel2:#1b222b;--line:#28313d;--txt:#e7ebf2;--mut:#8c98ab;--acc:#4c8dff;--ok:#31c46b;--warn:#f0b429;--err:#f0554c}",
  "*{box-sizing:border-box}",
  "html,body{margin:0;height:100%}",
  "body{background:var(--bg);color:var(--txt);font:14px/1.5 -apple-system,BlinkMacSystemFont,Segoe UI,Roboto,Helvetica,Arial,sans-serif}",
  "header{display:flex;align-items:center;gap:14px;padding:12px 18px;border-bottom:1px solid var(--line);background:var(--panel);flex-wrap:wrap}",
  ".brand{font-weight:650;font-size:15px;letter-spacing:.2px}",
  ".brand span{color:var(--mut);font-weight:400;font-size:12px;margin-left:6px}",
  ".pill{padding:3px 10px;border-radius:999px;font-size:11px;font-weight:700;letter-spacing:.6px}",
  ".pill.idle{background:#252d38;color:var(--mut)}",
  ".pill.run{background:rgba(76,141,255,.16);color:#8ab4ff}",
  ".pill.err{background:rgba(240,85,76,.16);color:#ff8d86}",
  ".pill.ok{background:rgba(49,196,107,.16);color:#6ee7a0}",
  ".tools{font-size:12px;color:var(--mut)}",
  ".grow{flex:1}",
  ".cwd{font-size:11px;color:var(--mut);font-family:ui-monospace,Menlo,Consolas,monospace;max-width:44vw;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}",
  "main{display:flex;gap:14px;padding:14px 18px;align-items:flex-start}",
  ".col{display:flex;flex-direction:column;gap:14px;width:360px;flex:0 0 360px}",
  ".col.wide{flex:1;width:auto;min-width:0}",
  ".card{background:var(--panel);border:1px solid var(--line);border-radius:12px;padding:14px}",
  "h2{margin:0 0 12px;font-size:12px;text-transform:uppercase;letter-spacing:.9px;color:var(--mut);display:flex;align-items:center;gap:10px}",
  ".stage{font-size:17px;font-weight:600}",
  ".sub{color:var(--mut);font-size:12px;margin-top:2px;min-height:18px}",
  ".bar{height:6px;background:#212a35;border-radius:99px;overflow:hidden;margin:10px 0 6px}",
  ".bar>div{height:100%;width:0;background:linear-gradient(90deg,#4c8dff,#7db4ff);transition:width .35s ease}",
  ".row{display:flex;gap:8px;align-items:center;flex-wrap:wrap;margin-top:10px}",
  ".btn{background:#232c38;color:var(--txt);border:1px solid #33404f;border-radius:9px;padding:9px 13px;font-size:13px;font-weight:600;cursor:pointer}",
  ".btn:hover{background:#2b3542}",
  ".btn:disabled{opacity:.45;cursor:not-allowed}",
  ".btn.primary{background:#2a5fd0;border-color:#2a5fd0}",
  ".btn.primary:hover{background:#356ee4}",
  ".btn.danger{background:#5d2b28;border-color:#7a3a35}",
  ".btn.danger:hover{background:#6f3430}",
  ".btn.tiny{padding:3px 8px;font-size:11px;font-weight:600}",
  ".hint{color:var(--mut);font-size:11px}",
  "label.f{display:block;margin:9px 0 3px;font-size:11px;color:var(--mut)}",
  "input[type=text],input[type=password],input[type=number]{width:100%;background:var(--panel2);border:1px solid var(--line);color:var(--txt);border-radius:8px;padding:7px 9px;font-size:13px;font-family:inherit}",
  "input[type=number]{width:96px}",
  "input:focus{outline:none;border-color:#3d6fc4}",
  ".grid2{display:grid;grid-template-columns:1fr 1fr;gap:10px}",
  ".chk{display:flex;align-items:center;gap:7px;margin-top:10px;font-size:13px;color:var(--txt)}",
  ".counts{display:grid;grid-template-columns:1fr 1fr;gap:7px}",
  ".count{background:var(--panel2);border:1px solid var(--line);border-radius:9px;padding:8px 10px;font-size:12px;color:var(--mut);cursor:pointer}",
  ".count b{display:block;color:var(--txt);font-size:16px;font-weight:650}",
  "pre#log{background:#0a0d11;border:1px solid var(--line);border-radius:10px;padding:10px;height:44vh;min-height:200px;overflow:auto;margin:0;font:12px/1.5 ui-monospace,Menlo,Consolas,monospace;white-space:pre-wrap;word-break:break-word}",
  "pre#log .warn{color:#f0c96b}",
  "pre#log .err{color:#ff8d86}",
  "pre#log .ok{color:#7ee2a8}",
  "pre#log .cmd{color:#8ab4ff}",
  ".tabs{display:flex;gap:6px;flex-wrap:wrap;margin-bottom:10px}",
  ".tab{padding:5px 10px;border-radius:8px;background:var(--panel2);border:1px solid var(--line);font-size:12px;cursor:pointer;color:var(--mut)}",
  ".tab.on{background:#26313f;color:var(--txt);border-color:#3a4a5e}",
  ".files{display:flex;flex-direction:column;gap:6px;max-height:34vh;overflow:auto}",
  ".frow{display:flex;align-items:center;gap:10px;background:var(--panel2);border:1px solid var(--line);border-radius:9px;padding:7px 10px;font-size:12.5px}",
  ".frow .nm{flex:1;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}",
  ".frow .sz{color:var(--mut);font-size:11px;white-space:nowrap}",
  ".mini{background:#232c38;border:1px solid #33404f;color:var(--txt);border-radius:7px;padding:3px 8px;font-size:11px;cursor:pointer}",
  ".mini:hover{background:#2b3542}",
  ".mini.del:hover{background:#6f3430;border-color:#7a3a35}",
  "#modal{position:fixed;inset:0;background:rgba(4,6,9,.82);display:none;align-items:center;justify-content:center;padding:22px;z-index:9}",
  "#modal.on{display:flex}",
  "#modal .box{background:#0f1319;border:1px solid var(--line);border-radius:12px;padding:12px;max-width:92vw;max-height:92vh;display:flex;flex-direction:column;gap:8px}",
  "#modal video{max-width:86vw;max-height:76vh;border-radius:8px;background:#000}",
  "#modal .t{font-size:12px;color:var(--mut)}",
  "@media (max-width:900px){main{flex-direction:column}.col{width:auto;flex:auto}}",
  "</style>",
  "</head>",
  "<body>",
  "<header>",
  "  <div class='brand'>AI Movie Shorts<span>web control panel</span></div>",
  "  <div id='pill' class='pill idle'>IDLE</div>",
  "  <div id='tools' class='tools'></div>",
  "  <div class='grow'></div>",
  "  <div id='cwd' class='cwd'></div>",
  "</header>",
  "<main>",
  "  <section class='col'>",
  "    <div class='card'>",
  "      <h2>Generation</h2>",
  "      <div id='stage' class='stage'>Idle</div>",
  "      <div class='bar'><div id='barfill'></div></div>",
  "      <div id='sub' class='sub'>Put .mp4 files in movies/ and press start.</div>",
  "      <div class='row'>",
  "        <button id='btnStart' class='btn primary'>START GENERATION</button>",
  "        <button id='btnCancel' class='btn danger' disabled>CANCEL</button>",
  "      </div>",
  "      <div id='lastrc' class='hint'></div>",
  "    </div>",
  "",
  "    <div class='card'>",
  "      <h2>Settings <span class='hint'>written to config.json</span></h2>",
  "      <label class='f'>OpenAI API key <span id='k1state' class='hint'></span></label>",
  "      <input type='password' id='open_api_key' placeholder='sk-...  (leave empty to keep current)'>",
  "      <label class='f'>Narration engine</label>",
  "      <select id='tts_provider'>",
  "        <option value='elevenlabs'>ElevenLabs - cloud, needs an API key</option>",
  "        <option value='xtts'>XTTS v2 - free, local, clones a voice sample</option>",
  "        <option value='piper'>Piper - free, offline, very fast</option>",
  "        <option value='edge'>Edge TTS - free, natural neural voices (internet)</option>",
  "        <option value='openai_tts'>OpenAI-compatible /audio/speech (OpenAI, Kokoro, ...)</option>",
  "      </select>",
  "      <div id='ttsHint' class='hint'></div>",
  "      <label class='f'>ElevenLabs API key <span id='k2state' class='hint'></span></label>",
  "      <input type='password' id='elevenlabs_api_key' placeholder='only needed when the engine is ElevenLabs'>",
  "      <div class='grid2'>",
  "        <div><label class='f'>OpenAI model</label><input type='text' id='openai_model'></div>",
  "        <div><label class='f'>Voice id</label><input type='text' id='eleven_voice_id'></div>",
  "        <div><label class='f'>TTS model</label><input type='text' id='eleven_model_id'></div>",
  "        <div><label class='f'>Clips (min)</label><input type='number' id='min_clips' min='1' max='200'></div>",
  "        <div><label class='f'>Clips (max)</label><input type='number' id='max_clips' min='1' max='200'></div>",
  "        <div><label class='f'>Recap minutes (0=auto)</label><input type='number' id='recap_minutes' min='0' max='180'></div>",
  "        <div><label class='f'>Max speed-up</label><input type='number' id='max_video_speedup' step='0.05' min='1' max='8'></div>",
  "        <div><label class='f'>Narration vol</label><input type='number' id='narration_volume' step='0.1' min='0' max='10'></div>",
  "        <div><label class='f'>Music vol</label><input type='number' id='bgm_volume' step='0.05' min='0' max='10'></div>",
  "      </div>",
  "      <label class='f'>OpenAI base URL</label><input type='text' id='openai_base_url'>",
  "      <label class='f'>ElevenLabs base URL</label><input type='text' id='elevenlabs_base_url'>",
  "      <label class='f'>TTS server URL <span class='hint'>xtts / piper / openai-compatible</span></label>",
  "      <input type='text' id='tts_base_url' placeholder='http://127.0.0.1:8020'>",
  "      <div class='grid2'>",
  "        <div><label class='f'>TTS voice / speaker</label><input type='text' id='tts_voice'></div>",
  "        <div><label class='f'>Whisper model <span class='hint'>auto-transcribe when no SRT</span></label><input type='text' id='whisper_model'></div>",
  "        <div><label class='f'>TTS language</label><input type='text' id='tts_language'></div>",
  "      </div>",
  "      <label class='f'>TTS model</label><input type='text' id='tts_model'>",
  "      <label class='f'>TTS API key <span id='k3state' class='hint'></span></label>",
  "      <input type='password' id='tts_api_key' placeholder='optional - leave empty to keep current'>",
  "      <label class='chk'><input type='checkbox' id='bgm_enabled'> Background music</label>",
  "      <label class='chk'><input type='checkbox' id='make_vertical'> Also render vertical 9:16</label>",
  "      <label class='chk'><input type='checkbox' id='captions'> Burn-in small subtitles</label>",
  "      <label class='chk'><input type='checkbox' id='retire_movies'> Move processed movies to movies_retired</label>",
  "      <div class='row'>",
  "        <button id='btnSave' class='btn'>SAVE SETTINGS</button>",
  "        <span id='saveMsg' class='hint'></span>",
  "      </div>",
  "    </div>",
  "",
  "    <div class='card'>",
  "      <h2>Folders <span class='hint'>click to open in the file list</span></h2>",
  "      <div class='counts' id='counts'></div>",
  "    </div>",
  "  </section>",
  "",
  "  <section class='col wide'>",
  "    <div class='card'>",
  "      <h2>Live log",
  "        <button id='btnClear' class='btn tiny'>CLEAR VIEW</button>",
  "        <label class='hint' style='margin-left:auto'><input type='checkbox' id='autoscroll' checked> auto-scroll</label>",
  "      </h2>",
  "      <pre id='log'></pre>",
  "    </div>",
  "",
  "    <div class='card'>",
  "      <h2>Files</h2>",
  "      <div class='tabs' id='tabs'></div>",
  "      <div class='row' style='margin-top:0'>",
  "        <input type='file' id='file' multiple style='display:none'>",
  "        <button id='btnUpload' class='btn'>UPLOAD TO <span id='upDir'>movies</span></button>",
  "        <button id='btnOpen' class='btn'>OPEN IN FILE MANAGER</button>",
  "        <span id='upMsg' class='hint'></span>",
  "      </div>",
  "      <div id='files' class='files'></div>",
  "    </div>",
  "  </section>",
  "</main>",
  "",
  "<div id='modal'><div class='box'><div class='t' id='modalTitle'></div><video id='modalVideo' controls></video>",
  "<div class='row'><a id='modalDl' class='btn' download>DOWNLOAD</a><button class='btn' id='modalClose'>CLOSE</button></div></div></div>",
  "",
  "<script>",
  "var cursor = 0, tab = 'movies', cfgLoaded = false, lastRunning = false;",
  "var DIRS = [['movies','Movies'],['srt','Subtitles'],['output','Output'],['tiktok_output','Vertical'],",
  "            ['bgm','Music'],['movies_retired','Retired'],['clips','Temp']];",
  "var UPLOADABLE = {movies:1, srt:1, bgm:1};",
  "",
  "function el(id){ return document.getElementById(id); }",
  "function esc(s){ return String(s == null ? '' : s).replace(/[&<>]/g, function(c){",
  "  return c === '&' ? '&amp;' : (c === '<' ? '&lt;' : '&gt;'); }); }",
  "function mb(n){ return n > 1048576 ? (n/1048576).toFixed(1) + ' MB' : (n/1024).toFixed(0) + ' KB'; }",
  "function api(path, opts){ return fetch(path, opts).then(function(r){ return r.json(); }).catch(function(){ return null; }); }",
  "",
  "/* ---------------- log ---------------- */",
  "function cls(t){",
  "  if (t.indexOf('[FATAL]') === 0 || t.indexOf('[ERROR]') === 0) return 'err';",
  "  if (t.indexOf('[WARN]') === 0) return 'warn';",
  "  if (t.indexOf('[OK]') === 0) return 'ok';",
  "  if (t.indexOf('[cmd]') === 0 || t.indexOf('[ffmpeg]') === 0) return 'cmd';",
  "  return '';",
  "}",
  "function appendLog(lines){",
  "  var box = el('log'), html = '';",
  "  for (var i = 0; i < lines.length; i++){",
  "    var c = cls(lines[i].text);",
  "    html += c ? '<span class=' + c + '>' + esc(lines[i].text) + '</span>' : esc(lines[i].text);",
  "    html += String.fromCharCode(10);",
  "  }",
  "  box.insertAdjacentHTML('beforeend', html);",
  "  while (box.childNodes.length > 4000) box.removeChild(box.firstChild);",
  "  if (el('autoscroll').checked) box.scrollTop = box.scrollHeight;",
  "}",
  "",
  "/* ---------------- status ---------------- */",
  "function renderStatus(s){",
  "  if (!s) return;",
  "  var p = s.progress || {}, running = !!s.running;",
  "  var pill = el('pill');",
  "  pill.className = 'pill ' + (running ? 'run' : (s.last_rc < 0 ? 'err' : (s.have_rc ? 'ok' : 'idle')));",
  "  pill.textContent = running ? (s.cancel_requested ? 'CANCELLING' : 'RUNNING')",
  "                             : (s.have_rc ? (s.last_rc < 0 ? 'STOPPED' : 'IDLE') : 'IDLE');",
  "",
  "  el('stage').textContent = p.stage_name || 'Idle';",
  "  var sub = '';",
  "  if (p.movie_total > 0) sub = 'Movie ' + p.movie_index + ' of ' + p.movie_total + (p.movie_title ? ' - ' + p.movie_title : '');",
  "  if (p.clip_total > 0 && p.clip_index > 0) sub += (sub ? '  |  ' : '') + 'clip ' + p.clip_index + ' of ' + p.clip_total;",
  "  el('sub').textContent = sub || (running ? 'Working...' : 'Put .mp4 files in movies/ and press start.');",
  "",
  "  var pct = 0;",
  "  if (p.clip_total > 0 && p.clip_index > 0) pct = 100 * p.clip_index / p.clip_total;",
  "  else if (p.movie_total > 0 && p.movie_index > 0) pct = 100 * (p.movie_index - 0.5) / p.movie_total;",
  "  else if (running) pct = 4;",
  "  el('barfill').style.width = pct.toFixed(1) + '%';",
  "",
  "  el('btnStart').disabled = running;",
  "  el('btnCancel').disabled = !running;",
  "  el('lastrc').textContent = s.have_rc ? ('last run exit code: ' + s.last_rc) : '';",
  "  el('cwd').textContent = s.cwd || '';",
  "  el('tools').textContent = 'ffmpeg ' + (s.ffmpeg ? 'ok' : 'MISSING') + ' | ffprobe ' + (s.ffprobe ? 'ok' : 'MISSING');",
  "  el('tools').style.color = (s.ffmpeg && s.ffprobe) ? '' : '#ff8d86';",
  "",
  "  var c = s.counts || {};",
  "  var order = [['movies','Movies'],['output','Output'],['vertical','Vertical'],['retired','Retired'],['srt','Subtitles'],['bgm','Music']];",
  "  var map = {movies:'movies', output:'output', vertical:'tiktok_output', retired:'movies_retired', srt:'srt', bgm:'bgm'};",
  "  var html = '';",
  "  for (var i = 0; i < order.length; i++){",
  "    html += '<div class=count data-dir=' + map[order[i][0]] + '><b>' + (c[order[i][0]] || 0) + '</b>' + order[i][1] + '</div>';",
  "  }",
  "  el('counts').innerHTML = html;",
  "  var cards = el('counts').children;",
  "  for (var j = 0; j < cards.length; j++){",
  "    cards[j].onclick = (function(d){ return function(){ setTab(d); }; })(cards[j].getAttribute('data-dir'));",
  "  }",
  "",
  "  if (s.config) fillConfig(s.config);",
  "  if (running !== lastRunning){",
  "    lastRunning = running;",
  "    if (!running) refreshFiles();",
  "  }",
  "}",
  "",
  "function fillConfig(c){",
  "  if (cfgLoaded) return;",
  "  cfgLoaded = true;",
  "  var texts = ['openai_model','eleven_voice_id','eleven_model_id','openai_base_url','elevenlabs_base_url',",
  "               'tts_base_url','tts_voice','tts_language','tts_model','whisper_model'];",
  "  var nums  = ['min_clips','max_clips','max_video_speedup','narration_volume','bgm_volume','recap_minutes'];",
  "  var bools = ['bgm_enabled','make_vertical','retire_movies','captions'];",
  "  texts.forEach(function(k){ el(k).value = c[k] || ''; });",
  "  nums.forEach(function(k){ el(k).value = c[k]; });",
  "  bools.forEach(function(k){ el(k).checked = !!c[k]; });",
  "  el('k1state').textContent = c.openai_key_set ? '(' + c.open_api_key_masked + ')' : '(not set)';",
  "  el('k2state').textContent = c.elevenlabs_key_set ? '(' + c.elevenlabs_api_key_masked + ')' : '(not set)';",
  "  el('k1state').style.color = c.openai_key_set ? '' : '#ff8d86';",
  "  el('k2state').style.color = c.elevenlabs_key_set ? '' : '#ff8d86';",
  "  el('k3state').textContent = c.tts_key_set ? '(' + c.tts_api_key_masked + ')' : '(not set)';",
  "  var prov = c.tts_provider || 'elevenlabs';",
  "  var sel = el('tts_provider');",
  "  for (var i = 0; i < sel.options.length; i++){ if (sel.options[i].value === prov) sel.selectedIndex = i; }",
  "  syncTtsUi();",
  "}",
  "",
  "var TTS_HELP = {",
  "  elevenlabs: 'Needs an ElevenLabs API key. Voice id and TTS model above are used.',",
  "  xtts: 'Free. Run: pip install xtts-api-server && python -m xtts_api_server  (port 8020). Voice = a .wav in the servers speakers folder. Language: en, hi, ur, es, ...',",
  "  piper: 'Free and offline. Just run: run.bat tts  - it installs a private Python and a voice under F: and starts the server on port 5000. Voice is optional.',",
  "  openai_tts: 'Any OpenAI-compatible endpoint: api.openai.com/v1, Kokoro-FastAPI, LM Studio, ... Uses TTS model + voice + TTS API key.'",
  "};",
  "var TTS_PH = {",
  "  elevenlabs: '', xtts: 'http://127.0.0.1:8020', piper: 'http://127.0.0.1:5000', openai_tts: 'https://api.openai.com/v1'",
  "};",
  "function syncTtsUi(){",
  "  var p = el('tts_provider').value;",
  "  el('ttsHint').textContent = TTS_HELP[p] || '';",
  "  el('tts_base_url').placeholder = TTS_PH[p] || '';",
  "}",
  "el('tts_provider').onchange = syncTtsUi;",
  "",
  "/* ---------------- files ---------------- */",
  "function setTab(d){",
  "  tab = d;",
  "  var t = el('tabs');",
  "  for (var i = 0; i < t.children.length; i++){",
  "    t.children[i].className = 'tab' + (t.children[i].getAttribute('data-dir') === d ? ' on' : '');",
  "  }",
  "  el('upDir').textContent = d;",
  "  el('btnUpload').style.display = UPLOADABLE[d] ? '' : 'none';",
  "  refreshFiles();",
  "}",
  "function refreshFiles(){",
  "  api('/api/list?dir=' + encodeURIComponent(tab)).then(function(r){",
  "    if (!r || !r.files) { el('files').innerHTML = '<div class=hint>folder not readable</div>'; return; }",
  "    if (!r.files.length){ el('files').innerHTML = '<div class=hint>empty folder</div>'; return; }",
  "    var html = '';",
  "    r.files.forEach(function(f){",
  "      var isVid = /[.]mp4$/i.test(f.name);",
  "      var url = '/media/' + tab + '/' + encodeURIComponent(f.name);",
  "      html += '<div class=frow><span class=nm title=' + esc(f.name) + '>' + esc(f.name) + '</span>' +",
  "              '<span class=sz>' + mb(f.size) + '</span>' +",
  "              (isVid ? '<button class=mini data-play=' + esc(url) + ' data-title=' + esc(f.name) + '>PLAY</button>' : '') +",
  "              '<a class=mini href=' + url + ' download>GET</a>' +",
  "              '<button class=mini del data-del=' + esc(f.name) + '>DEL</button></div>';",
  "    });",
  "    el('files').innerHTML = html;",
  "    Array.prototype.forEach.call(el('files').querySelectorAll('[data-del]'), function(b){",
  "      b.onclick = function(){ doDelete(b.getAttribute('data-del')); };",
  "    });",
  "    Array.prototype.forEach.call(el('files').querySelectorAll('[data-play]'), function(b){",
  "      b.onclick = function(){ openModal(b.getAttribute('data-play'), b.getAttribute('data-title')); };",
  "    });",
  "  });",
  "}",
  "function doDelete(name){",
  "  if (!confirm('Delete ' + name + ' from ' + tab + '?')) return;",
  "  api('/api/delete', {method:'POST', headers:{'Content-Type':'application/json'},",
  "                      body: JSON.stringify({dir: tab, name: name})}).then(function(){ refreshFiles(); tick(); });",
  "}",
  "function openModal(url, title){",
  "  el('modalTitle').textContent = title;",
  "  el('modalVideo').src = url;",
  "  el('modalDl').href = url;",
  "  el('modal').className = 'on';",
  "  el('modalVideo').play();",
  "}",
  "el('modalClose').onclick = function(){ el('modal').className = ''; el('modalVideo').pause(); el('modalVideo').src = ''; };",
  "",
  "/* ---------------- actions ---------------- */",
  "el('btnStart').onclick = function(){",
  "  el('log').innerHTML = '';",
  "  cursor = 0;",
  "  api('/api/generate', {method:'POST'}).then(function(r){",
  "    if (r && r.error) alert(r.error);",
  "    tick();",
  "  });",
  "};",
  "el('btnCancel').onclick = function(){ api('/api/cancel', {method:'POST'}); tick(); };",
  "el('btnClear').onclick = function(){ el('log').innerHTML = ''; };",
  "el('btnOpen').onclick = function(){ api('/api/open?dir=' + encodeURIComponent(tab)); };",
  "el('btnUpload').onclick = function(){ el('file').click(); };",
  "el('file').onchange = function(){",
  "  var files = el('file').files;",
  "  if (!files || !files.length) return;",
  "  var i = 0;",
  "  el('upMsg').textContent = 'uploading ' + files.length + ' file(s)...';",
  "  function next(){",
  "    if (i >= files.length){ el('upMsg').textContent = 'done'; refreshFiles(); tick(); el('file').value = ''; return; }",
  "    var f = files[i++];",
  "    var url = '/api/upload?dir=' + encodeURIComponent(tab) + '&name=' + encodeURIComponent(f.name);",
  "    fetch(url, {method:'POST', body:f}).then(function(r){ return r.json(); }).then(function(j){",
  "      el('upMsg').textContent = (j && j.ok) ? ('saved ' + f.name) : ('failed: ' + f.name);",
  "      next();",
  "    }).catch(function(){ el('upMsg').textContent = 'failed: ' + f.name; next(); });",
  "  }",
  "  next();",
  "};",
  "el('btnSave').onclick = function(){",
  "  var body = {};",
  "  ['openai_model','eleven_voice_id','eleven_model_id','openai_base_url','elevenlabs_base_url',",
  "   'tts_base_url','tts_voice','tts_language','tts_model'].forEach(function(k){ body[k] = el(k).value; });",
  "  body.tts_provider = el('tts_provider').value;",
  "  ['min_clips','max_clips','max_video_speedup','narration_volume','bgm_volume','recap_minutes'].forEach(function(k){ body[k] = Number(el(k).value); });",
  "  ['bgm_enabled','make_vertical','retire_movies','captions'].forEach(function(k){ body[k] = el(k).checked; });",
  "  if (el('open_api_key').value) body.open_api_key = el('open_api_key').value;",
  "  if (el('elevenlabs_api_key').value) body.elevenlabs_api_key = el('elevenlabs_api_key').value;",
  "  if (el('tts_api_key').value) body.tts_api_key = el('tts_api_key').value;",
  "  el('saveMsg').textContent = 'saving...';",
  "  api('/api/config', {method:'POST', headers:{'Content-Type':'application/json'}, body: JSON.stringify(body)}).then(function(c){",
  "    if (!c) { el('saveMsg').textContent = 'failed'; return; }",
  "    if (c.error){ el('saveMsg').textContent = c.error; return; }",
  "    cfgLoaded = false;",
  "    fillConfig(c);",
  "    el('open_api_key').value = '';",
  "    el('elevenlabs_api_key').value = '';",
  "    el('tts_api_key').value = '';",
  "    el('saveMsg').textContent = 'saved';",
  "    setTimeout(function(){ el('saveMsg').textContent = ''; }, 2500);",
  "  });",
  "};",
  "",
  "/* ---------------- tabs + polling ---------------- */",
  "(function(){",
  "  var html = '';",
  "  DIRS.forEach(function(d){ html += '<div class=tab data-dir=' + d[0] + '>' + d[1] + '</div>'; });",
  "  el('tabs').innerHTML = html;",
  "  Array.prototype.forEach.call(el('tabs').children, function(t){",
  "    t.onclick = function(){ setTab(t.getAttribute('data-dir')); };",
  "  });",
  "  setTab('movies');",
  "})();",
  "",
  "function tick(){",
  "  api('/api/status?cfg=1').then(renderStatus);",
  "  api('/api/logs?since=' + cursor).then(function(l){",
  "    if (!l) return;",
  "    if (l.lines && l.lines.length){ appendLog(l.lines); cursor = l.next; }",
  "    else if (l.next) cursor = l.next;",
  "  });",
  "}",
  "tick();",
  "setInterval(tick, 900);",
  "setInterval(refreshFiles, 5000);",
  "</script>",
  "</body>",
  "</html>",
  "",
  NULL
};

static char *page_html_cache = NULL;
static size_t page_html_len = 0;

static const char *page_html(size_t *out_len) {
  if (!page_html_cache) {
    Buf b; buf_init(&b);
    for (int i = 0; PAGE_HTML[i]; i++) {
      buf_add(&b, PAGE_HTML[i], strlen(PAGE_HTML[i]));
      buf_add(&b, "\n", 1);
    }
    page_html_cache = b.p ? b.p : xstrdup("");
    page_html_len = b.len;
  }
  *out_len = page_html_len;
  return page_html_cache;
}

static void handle_status(Req *r) {
  cJSON *o = cJSON_CreateObject();

  app_lock();
  bool running = g_app.running != 0;
  int last_rc = g_app.last_rc;
  int have_rc = g_app.have_rc;
  GeneratorProgress p = g_app.progress;
  long long next_seq = g_app.next_seq;
  time_t started = g_app.started_at, finished = g_app.finished_at;
  app_unlock();

  cJSON_AddBoolToObject(o, "running", running);
  cJSON_AddNumberToObject(o, "last_rc", last_rc);
  cJSON_AddBoolToObject(o, "have_rc", have_rc != 0);
  cJSON_AddBoolToObject(o, "cancel_requested", generator_cancel_requested());
  cJSON_AddNumberToObject(o, "log_next", (double)next_seq);
  cJSON_AddNumberToObject(o, "started_at", (double)started);
  cJSON_AddNumberToObject(o, "finished_at", (double)finished);

  char cwd[4096];
  plat_getcwd(cwd, sizeof(cwd));
  json_str(o, "cwd", cwd);

  cJSON *prog = cJSON_CreateObject();
  cJSON_AddNumberToObject(prog, "stage", p.stage);
  json_str(prog, "stage_name", generator_stage_name(p.stage));
  cJSON_AddNumberToObject(prog, "movie_index", p.movie_index);
  cJSON_AddNumberToObject(prog, "movie_total", p.movie_total);
  cJSON_AddNumberToObject(prog, "clip_index", p.clip_index);
  cJSON_AddNumberToObject(prog, "clip_total", p.clip_total);
  json_str(prog, "movie_title", p.movie_title);
  cJSON_AddItemToObject(o, "progress", prog);

  cJSON *counts = cJSON_CreateObject();
  cJSON_AddNumberToObject(counts, "movies",    count_files("movies", ".mp4", NULL));
  cJSON_AddNumberToObject(counts, "retired",   count_files("movies_retired", ".mp4", NULL));
  cJSON_AddNumberToObject(counts, "output",    count_files("output", ".mp4", NULL));
  cJSON_AddNumberToObject(counts, "vertical",  count_files("tiktok_output", ".mp4", NULL));
  cJSON_AddNumberToObject(counts, "srt",       count_files("scripts/srt_files", ".srt", NULL));
  cJSON_AddNumberToObject(counts, "bgm",       count_files("backgroundmusic", ".mp3", ".m4a"));
  cJSON_AddItemToObject(o, "counts", counts);

  /* ffmpeg is probed once per process unless ?tools=1 asks for a re-check */
  static int have_ffmpeg = -1, have_ffprobe = -1;
  char tq[32];
  if (have_ffmpeg < 0 || have_ffprobe < 0 || query_get(r, "tools", tq, sizeof(tq))) {
    have_ffmpeg  = plat_have_tool("ffmpeg")  ? 1 : 0;
    have_ffprobe = plat_have_tool("ffprobe") ? 1 : 0;
  }
  cJSON_AddBoolToObject(o, "ffmpeg",  have_ffmpeg == 1);
  cJSON_AddBoolToObject(o, "ffprobe", have_ffprobe == 1);

  char q[64];
  if (query_get(r, "cfg", q, sizeof(q))) {
    cJSON_AddItemToObject(o, "config", config_read_json());
  }
  http_json(r, 200, o);
}

static void handle_logs(Req *r) {
  long long since = 0;
  char q[64];
  if (query_get(r, "since", q, sizeof(q))) since = strtoll(q, NULL, 10);

  cJSON *o = cJSON_CreateObject();
  cJSON *arr = cJSON_CreateArray();

  app_lock();
  int start;
  if (since > 0) {
    /* first slot with seq > since; lines older than the ring are gone */
    start = g_app.count;
    for (int i = 0; i < g_app.count; i++) {
      int idx = (g_app.head - g_app.count + i) % LOG_MAX_LINES;
      if (idx < 0) idx += LOG_MAX_LINES;
      if (g_app.seqs[idx] > since) { start = i; break; }
    }
  } else {
    /* first poll: send the most recent window only */
    start = g_app.count > 250 ? g_app.count - 250 : 0;
  }

  long long next = since;
  int sent = 0;
  for (int i = start; i < g_app.count && sent < 250; i++, sent++) {
    int idx = (g_app.head - g_app.count + i) % LOG_MAX_LINES;
    if (idx < 0) idx += LOG_MAX_LINES;
    cJSON *e = cJSON_CreateObject();
    cJSON_AddNumberToObject(e, "i", (double)g_app.seqs[idx]);
    json_str(e, "text", g_app.lines[idx]);
    cJSON_AddItemToArray(arr, e);
    next = g_app.seqs[idx];
  }
  if (sent == 0) next = g_app.next_seq;
  bool running = g_app.running != 0;
  app_unlock();

  cJSON_AddItemToObject(o, "lines", arr);
  cJSON_AddNumberToObject(o, "next", (double)next);
  cJSON_AddBoolToObject(o, "running", running);
  http_json(r, 200, o);
}

static void handle_list(Req *r) {
  char key[64];
  const DirEntry *de = NULL;
  if (query_get(r, "dir", key, sizeof(key))) de = dir_find(key);
  if (!de) { http_error(r, 400, "unknown dir (use movies, output, ...)"); return; }

  cJSON *o = cJSON_CreateObject();
  json_str(o, "dir", de->key);
  json_str(o, "label", de->label);
  cJSON_AddItemToObject(o, "files", list_dir_json(de->path));
  http_json(r, 200, o);
}

static void handle_generate(Req *r) {
  app_lock();
  bool running = g_app.running != 0;
  if (!running) {
    g_app.running = 1;
    g_app.have_rc = 0;
    g_app.started_at = time(NULL);
    g_app.finished_at = 0;
  }
  app_unlock();

  if (running) { http_error(r, 409, "a generation run is already active"); return; }

  generator_clear_cancel();
  hook_log("[INFO] Generation started from the web UI.");

  if (!plat_thread_start_detached(worker_thread, NULL)) {
    app_lock();
    g_app.running = 0;
    g_app.last_rc = -1;
    g_app.have_rc = 1;
    app_unlock();
    http_error(r, 500, "could not start worker thread");
    return;
  }
  http_ok(r);
}

static void handle_cancel(Req *r) {
  generator_request_cancel();
  hook_log("[WARN] Cancel requested from the web UI - stopping at the next step.");
  http_ok(r);
}

static void handle_upload(Req *r) {
  char dirkey[64], name[512];
  const DirEntry *de = NULL;
  if (query_get(r, "dir", dirkey, sizeof(dirkey))) de = dir_find(dirkey);
  if (!de) { http_error(r, 400, "unknown dir"); return; }
  if (!query_get(r, "name", name, sizeof(name)) || !name_is_safe(name)) {
    http_error(r, 400, "missing or invalid file name");
    return;
  }
  if (!r->have_content_length || r->content_length <= 0) {
    http_error(r, 413, "upload needs a Content-Length (no chunked uploads)");
    return;
  }
  if ((unsigned long long)r->content_length > MAX_UPLOAD) {
    http_error(r, 413, "file too large");
    return;
  }

  plat_mkdir(de->path);

  char dest[4096];
  snprintf(dest, sizeof(dest), "%s/%s", de->path, name);

  FILE *f = plat_fopen(dest, "wb");
  if (!f) { http_error(r, 500, "cannot create file"); return; }

  long long total = r->content_length;
  long long done = 0;
  bool ok = true;

  size_t already = r->len - r->hdr_end;
  if (already > 0) {
    size_t use = already > (size_t)total ? (size_t)total : already;
    if (fwrite(r->buf + r->hdr_end, 1, use, f) != use) ok = false;
    done += (long long)use;
  }

  const size_t CHUNK = 256 * 1024;
  char *chunk = (char *)malloc(CHUNK);
  if (!chunk) { fclose(f); http_error(r, 500, "out of memory"); return; }

  while (ok && done < total) {
    size_t want = (size_t)(total - done);
    if (want > CHUNK) want = CHUNK;
    int k = recv(r->s, chunk, (int)want, 0);
    if (k <= 0) { ok = false; break; }
    if (fwrite(chunk, 1, (size_t)k, f) != (size_t)k) { ok = false; break; }
    done += k;
  }
  free(chunk);
  fclose(f);

  if (!ok) {
    plat_unlink(dest);
    http_error(r, 500, "upload interrupted");
    return;
  }

  hook_log("[INFO] Uploaded file saved.");
  cJSON *o = cJSON_CreateObject();
  cJSON_AddBoolToObject(o, "ok", true);
  json_str(o, "path", dest);
  cJSON_AddNumberToObject(o, "size", (double)done);
  http_json(r, 200, o);
}

static void handle_delete(Req *r, cJSON *body) {
  const cJSON *d = cJSON_GetObjectItemCaseSensitive(body, "dir");
  const cJSON *n = cJSON_GetObjectItemCaseSensitive(body, "name");
  const DirEntry *de = dir_find(cJSON_IsString(d) ? d->valuestring : NULL);
  if (!de) { http_error(r, 400, "unknown dir"); return; }
  if (!cJSON_IsString(n) || !name_is_safe(n->valuestring)) { http_error(r, 400, "invalid name"); return; }

  char path[4096];
  snprintf(path, sizeof(path), "%s/%s", de->path, n->valuestring);
  if (plat_unlink(path) != 0) { http_error(r, 404, "could not delete file"); return; }
  http_ok(r);
}

static void handle_media(Req *r) {
  /* /media/<dirkey>/<percent-encoded name> */
  const char *p = r->path + strlen("/media/");
  const char *slash = strchr(p, '/');
  if (!slash) { http_error(r, 400, "bad media path"); return; }

  char key[64];
  size_t kl = (size_t)(slash - p);
  if (kl >= sizeof(key)) kl = sizeof(key) - 1;
  memcpy(key, p, kl);
  key[kl] = 0;

  const DirEntry *de = dir_find(key);
  if (!de) { http_error(r, 400, "unknown dir"); return; }

  char name[1024];
  snprintf(name, sizeof(name), "%s", slash + 1);
  url_decode(name);
  if (!name_is_safe(name)) { http_error(r, 400, "invalid name"); return; }

  char path[4096];
  snprintf(path, sizeof(path), "%s/%s", de->path, name);

  const char *ctype = "application/octet-stream";
  size_t nl = strlen(name);
  if (nl >= 4 && strcasecmp_local(name + nl - 4, ".mp4") == 0) ctype = "video/mp4";
  else if (nl >= 4 && strcasecmp_local(name + nl - 4, ".mp3") == 0) ctype = "audio/mpeg";
  else if (nl >= 4 && strcasecmp_local(name + nl - 4, ".m4a") == 0) ctype = "audio/mp4";
  else if (nl >= 4 && strcasecmp_local(name + nl - 4, ".srt") == 0) ctype = "text/plain; charset=utf-8";
  else if (nl >= 4 && strcasecmp_local(name + nl - 4, ".txt") == 0) ctype = "text/plain; charset=utf-8";

  http_send_file(r, path, ctype);
}

static void route(Req *r) {
  if (strcmp(r->path, "/") == 0 || strcmp(r->path, "/index.html") == 0) {
    size_t n = 0;
    const char *html = page_html(&n);
    http_respond(r, 200, "text/html; charset=utf-8", html, n);
    return;
  }
  if (strcmp(r->path, "/favicon.ico") == 0) { http_error(r, 404, "no icon"); return; }

  if (strncmp(r->path, "/media/", 7) == 0) {
    if (strcmp(r->method, "GET") != 0 && strcmp(r->method, "HEAD") != 0) {
      http_error(r, 405, "GET only");
      return;
    }
    handle_media(r);
    return;
  }

  if (strncmp(r->path, "/api/", 5) != 0) { http_error(r, 404, "not found"); return; }

  /* ---- GET APIs ---- */
  if (strcmp(r->method, "GET") == 0) {
    if (strcmp(r->path, "/api/status") == 0) { handle_status(r); return; }
    if (strcmp(r->path, "/api/logs") == 0)   { handle_logs(r); return; }
    if (strcmp(r->path, "/api/list") == 0)   { handle_list(r); return; }
    if (strcmp(r->path, "/api/config") == 0) { http_json(r, 200, config_read_json()); return; }
    if (strcmp(r->path, "/api/open") == 0) {
      char key[64];
      const DirEntry *de = NULL;
      if (query_get(r, "dir", key, sizeof(key))) de = dir_find(key);
      if (!de) { http_error(r, 400, "unknown dir"); return; }
      plat_open_folder(de->path);
      http_ok(r);
      return;
    }
    http_error(r, 404, "unknown api");
    return;
  }

  /* ---- POST APIs ---- */
  if (strcmp(r->method, "POST") == 0) {
    if (strcmp(r->path, "/api/generate") == 0) { handle_generate(r); return; }
    if (strcmp(r->path, "/api/cancel") == 0)   { handle_cancel(r); return; }
    if (strcmp(r->path, "/api/upload") == 0)   { handle_upload(r); return; }

    size_t blen = 0;
    char *body = req_read_body(r, &blen);
    cJSON *json = body ? cJSON_ParseWithLength(body, blen) : NULL;
    free(body);
    if (!json) { http_error(r, 400, "expected a JSON body"); return; }

    if (strcmp(r->path, "/api/config") == 0) {
      char err[512] = "config write failed";
      if (config_write_json(json, err, sizeof(err))) {
        cJSON_Delete(json);
        hook_log("[INFO] config.json updated from the web UI.");
        http_json(r, 200, config_read_json());
      } else {
        cJSON_Delete(json);
        http_error(r, 500, err);
      }
      return;
    }
    if (strcmp(r->path, "/api/delete") == 0) {
      handle_delete(r, json);
      cJSON_Delete(json);
      return;
    }
    cJSON_Delete(json);
    http_error(r, 404, "unknown api");
    return;
  }

  http_error(r, 405, "method not allowed");
}

/* ============================== server loop =============================== */

typedef struct { plat_sock_t s; } ConnCtx;

static void conn_thread(void *arg) {
  ConnCtx *c = (ConnCtx *)arg;
  plat_sock_t s = c->s;
  free(c);

  Req r;
  memset(&r, 0, sizeof(r));
  r.s = s;
  r.content_length = 0;

  if (req_read_headers(&r)) {
    req_parse_request_line(&r);

    char cl[64];
    if (req_header(&r, "Content-Length", cl, sizeof(cl))) {
      r.content_length = strtoll(cl, NULL, 10);
      r.have_content_length = true;
    }
    char exp[64];
    r.expect_100 = req_header(&r, "Expect", exp, sizeof(exp)) &&
                   strstr(exp, "100-continue") != NULL;
    if (r.expect_100) send_all(s, "HTTP/1.1 100 Continue\r\n\r\n", 25);

    route(&r);
  }
  free(r.buf);
  sock_close(s);
}

static bool net_init(void) {
#ifdef _WIN32
  WSADATA wsa;
  return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
#else
  /*
   * A browser that closes the tab mid-download makes send() raise SIGPIPE, which
   * would otherwise kill the whole server (and any generation in progress).
   * send() also uses MSG_NOSIGNAL; this covers every other write path too.
   */
  signal(SIGPIPE, SIG_IGN);
  return true;
#endif
}


int main(int argc, char **argv) {
  const char *host = "127.0.0.1";
  int port = 8080;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) host = argv[++i];
    else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) port = atoi(argv[++i]);
    else if (strncmp(argv[i], "--port=", 7) == 0) port = atoi(argv[i] + 7);
    else if (strncmp(argv[i], "--host=", 7) == 0) host = argv[i] + 7;
  }
  if (port <= 0 || port > 65535) port = 8080;

  plat_console_init();
  plat_enter_project_dir();
  g_app.lock = plat_mutex_create();

  plat_mkdir("movies");
  plat_mkdir("movies_retired");
  plat_mkdir("output");
  plat_mkdir("tiktok_output");
  plat_mkdir("scripts");
  plat_mkdir("scripts/srt_files");
  plat_mkdir("clips");
  plat_mkdir("clips/audio");

  if (!net_init()) {
    fprintf(stderr, "[FATAL] could not initialise the network stack\n");
    return 1;
  }

  plat_sock_t ls = socket(AF_INET, SOCK_STREAM, 0);
  if (ls == SOCK_INVALID) {
    fprintf(stderr, "[FATAL] socket() failed\n");
    return 1;
  }

  int yes = 1;
  setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons((unsigned short)port);
  if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (strcmp(host, "0.0.0.0") != 0) {
      fprintf(stderr, "[WARN] '%s' is not a valid IPv4 address - listening on all interfaces\n", host);
    }
  }

  if (bind(ls, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    fprintf(stderr, "[FATAL] could not bind %s:%d (port already in use?)\n", host, port);
    sock_close(ls);
    return 1;
  }
  if (listen(ls, 32) != 0) {
    fprintf(stderr, "[FATAL] listen() failed\n");
    sock_close(ls);
    return 1;
  }

  char cwd[4096];
  plat_getcwd(cwd, sizeof(cwd));
  printf("=============================================================\n");
  printf("  AI Movie Shorts - web control panel\n");
  printf("  Project folder : %s\n", cwd);
  printf("  Open           : http://%s:%d/\n", host, port);
  printf("  Stop with Ctrl+C\n");
  printf("=============================================================\n");
  fflush(stdout);

  for (;;) {
    struct sockaddr_in cli;
    socklen_t clen = sizeof(cli);
    plat_sock_t cs = accept(ls, (struct sockaddr *)&cli, &clen);
    if (cs == SOCK_INVALID) continue;

    ConnCtx *c = (ConnCtx *)malloc(sizeof(ConnCtx));
    if (!c) { sock_close(cs); continue; }
    c->s = cs;
    if (!plat_thread_start_detached(conn_thread, c)) {
      free(c);
      sock_close(cs);
    }
  }
  /* not reached */
}
