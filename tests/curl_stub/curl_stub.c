/* Offline libcurl stand-in: records requests and answers from a scripted queue. */
#include "curl/curl.h"   /* the stub header next to this file */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  char  url[1024];
  char  body[65536];
  char  headers[16][256];
  int   nheaders;
  int   status;
  char *reply;          /* what perform() answered with */
} Req;

static Req  g_req[STUB_MAX_REQUESTS];
static int  g_nreq = 0;

static StubReply g_replies[STUB_MAX_REPLIES];
static int       g_nreplies = 0;
static int       g_next = 0;
static StubReply g_default = {200, NULL, 0};

/* per-handle state */
typedef struct {
  char  url[1024];
  char  body[65536];
  char  headers[16][256];
  int   nheaders;
  curl_write_callback wcb;
  void *wdata;
} Handle;

static int ci_prefix(const char *text, const char *prefix) {
  size_t i = 0;
  for (; prefix[i]; i++) {
    int a = (unsigned char)text[i], b = (unsigned char)prefix[i];
    if (a >= 'A' && a <= 'Z') a += 32;
    if (b >= 'A' && b <= 'Z') b += 32;
    if (a != b) return 0;
  }
  return 1;
}

static char *dup_(const char *s) {
  size_t n = strlen(s ? s : "") + 1;
  char *p = (char *)malloc(n);
  if (p) memcpy(p, s ? s : "", n);
  return p;
}

void stub_reset(void) {
  for (int i = 0; i < g_nreq; i++) free(g_req[i].reply);
  memset(g_req, 0, sizeof(g_req));
  g_nreq = 0;
  for (int i = 0; i < g_nreplies; i++) free(g_replies[i].body);
  memset(g_replies, 0, sizeof(g_replies));
  g_nreplies = 0;
  g_next = 0;
  free(g_default.body);
  g_default.status = 200;
  g_default.body = NULL;
  g_default.transport_error = 0;
}

void stub_queue_reply(int status, const char *body) {
  if (g_nreplies >= STUB_MAX_REPLIES) return;
  g_replies[g_nreplies].status = status;
  g_replies[g_nreplies].body = dup_(body);
  g_replies[g_nreplies].transport_error = 0;
  g_nreplies++;
}

void stub_queue_transport_error(CURLcode code) {
  if (g_nreplies >= STUB_MAX_REPLIES) return;
  g_replies[g_nreplies].status = 0;
  g_replies[g_nreplies].body = NULL;
  g_replies[g_nreplies].transport_error = code ? code : 7;
  g_nreplies++;
}

void stub_set_default_reply(int status, const char *body) {
  free(g_default.body);
  g_default.status = status;
  g_default.body = dup_(body);
  g_default.transport_error = 0;
}

int         stub_request_count(void) { return g_nreq; }
const char *stub_request_url(int i)  { return (i >= 0 && i < g_nreq) ? g_req[i].url : ""; }
const char *stub_request_body(int i) { return (i >= 0 && i < g_nreq) ? g_req[i].body : ""; }

const char *stub_request_header(int index, const char *name) {
  if (index < 0 || index >= g_nreq) return NULL;
  size_t nl = strlen(name);
  for (int h = 0; h < g_req[index].nheaders; h++) {
    if (ci_prefix(g_req[index].headers[h], name) &&
        g_req[index].headers[h][nl] == ':') {
      const char *v = g_req[index].headers[h] + nl + 1;
      while (*v == ' ') v++;
      return v;
    }
  }
  return NULL;
}

const char *stub_last_header(const char *name) {
  return stub_request_header(g_nreq - 1, name);
}

/* ---- the API the app uses ------------------------------------------------ */

CURL *curl_easy_init(void) { return (CURL *)calloc(1, sizeof(Handle)); }

void curl_easy_cleanup(CURL *h) { free(h); }

static Handle *h_of(CURL *h) { return (Handle *)h; }

CURLcode curl_easy_setopt(CURL *h, CURLoption opt, ...) {
  Handle *hd = h_of(h);
  if (!hd) return 43; /* CURLE_BAD_FUNCTION_ARGUMENT */
  va_list ap;
  va_start(ap, opt);
  switch (opt) {
    case CURLOPT_URL:
      snprintf(hd->url, sizeof(hd->url), "%s", va_arg(ap, const char *));
      break;
    case CURLOPT_POSTFIELDS:
      snprintf(hd->body, sizeof(hd->body), "%s", va_arg(ap, const char *));
      break;
    case CURLOPT_WRITEFUNCTION:
      hd->wcb = va_arg(ap, curl_write_callback);
      break;
    case CURLOPT_WRITEDATA:
      hd->wdata = va_arg(ap, void *);
      break;
    case CURLOPT_USERAGENT:
    case CURLOPT_ACCEPT_ENCODING:
    case CURLOPT_COOKIEFILE:
      (void)va_arg(ap, const char *);   /* string options we do not need */
      break;
    case CURLOPT_HTTPHEADER: {
      struct curl_slist *list = va_arg(ap, struct curl_slist *);
      hd->nheaders = 0;
      for (struct curl_slist *n = list; n && hd->nheaders < 16; n = n->next)
        snprintf(hd->headers[hd->nheaders++], sizeof(hd->headers[0]), "%s", n->data);
      break;
    }
    default: {
      /* numeric options (timeouts, flags): consume the arg, ignore it */
      (void)va_arg(ap, long);
      break;
    }
  }
  va_end(ap);
  return CURLE_OK;
}

struct curl_slist *curl_slist_append(struct curl_slist *list, const char *s) {
  struct curl_slist *node = (struct curl_slist *)calloc(1, sizeof(*node));
  node->data = dup_(s);
  if (!list) return node;
  struct curl_slist *t = list;
  while (t->next) t = t->next;
  t->next = node;
  return list;
}

void curl_slist_free_all(struct curl_slist *list) {
  while (list) {
    struct curl_slist *n = list->next;
    free(list->data);
    free(list);
    list = n;
  }
}

CURLcode curl_easy_perform(CURL *h) {
  Handle *hd = h_of(h);
  if (!hd) return 43;

  if (g_nreq < STUB_MAX_REQUESTS) {
    Req *r = &g_req[g_nreq++];
    snprintf(r->url, sizeof(r->url), "%s", hd->url);
    snprintf(r->body, sizeof(r->body), "%s", hd->body);
    r->nheaders = hd->nheaders;
    for (int i = 0; i < hd->nheaders; i++)
      snprintf(r->headers[i], sizeof(r->headers[i]), "%s", hd->headers[i]);
  }

  StubReply rep = (g_next < g_nreplies) ? g_replies[g_next++] : g_default;
  if (rep.transport_error) return rep.transport_error;

  if (hd->wcb && rep.body) hd->wcb((char *)rep.body, 1, strlen(rep.body), hd->wdata);
  g_req[g_nreq - 1].status = rep.status;
  return CURLE_OK;
}

CURLcode curl_easy_getinfo(CURL *h, CURLINFO info, ...) {
  (void)h;
  va_list ap;
  va_start(ap, info);
  if (info == CURLINFO_RESPONSE_CODE) {
    long *out = va_arg(ap, long *);
    if (out) *out = (g_nreq > 0) ? g_req[g_nreq - 1].status : 0;
  }
  va_end(ap);
  return CURLE_OK;
}

const char *curl_easy_strerror(CURLcode code) {
  (void)code;
  return "stub transport error";
}

CURLcode curl_global_init(long flags) { (void)flags; return CURLE_OK; }
void curl_global_cleanup(void) {}
