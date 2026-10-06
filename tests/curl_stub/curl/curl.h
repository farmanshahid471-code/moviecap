/* Minimal libcurl stand-in for the offline unit tests.  Only what the app
 * actually calls is provided; every perform() is recorded and answered from a
 * scripted queue, so the tests can drive the API paths without a network.
 *
 * anthropic_endpoint_test builds against THIS header instead of the real
 * libcurl (and links tests/curl_stub/curl_stub.c instead of CURL::libcurl),
 * which is what makes the Claude/Anthropic request shape and every retry path
 * testable.  Nothing else in the project sees it. */
#ifndef STUB_CURL_H
#define STUB_CURL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int CURLcode;
typedef int CURLoption;
typedef int CURLINFO;

#define CURLE_OK 0
#define CURL_GLOBAL_DEFAULT 3
#define CURL_GLOBAL_ALL 3
#define CURLE_COULDNT_CONNECT 7

typedef void CURL;

struct curl_slist {
  char *data;
  struct curl_slist *next;
};

typedef size_t (*curl_write_callback)(char *ptr, size_t size, size_t nmemb, void *userdata);

/* options */
#define CURLOPT_URL 10002
#define CURLOPT_WRITEFUNCTION 20011
#define CURLOPT_WRITEDATA 10001
#define CURLOPT_POSTFIELDS 10015
#define CURLOPT_POSTFIELDSIZE 60
#define CURLOPT_HTTPHEADER 10023
#define CURLOPT_FOLLOWLOCATION 52
#define CURLOPT_MAXREDIRS 68
#define CURLOPT_CONNECTTIMEOUT 78
#define CURLOPT_TIMEOUT 13
#define CURLOPT_NOSIGNAL 99
#define CURLOPT_ACCEPT_ENCODING 10102
#define CURLOPT_USERAGENT 10018
#define CURLOPT_COOKIEFILE 10031
#define CURLOPT_TCP_KEEPALIVE 213
#define CURLOPT_TCP_KEEPIDLE 214
#define CURLOPT_TCP_KEEPINTVL 215

#define CURLINFO_RESPONSE_CODE 0x200002

/* ---- the libcurl API the app uses --------------------------------------- */
CURL *curl_easy_init(void);
void  curl_easy_cleanup(CURL *handle);
CURLcode curl_easy_setopt(CURL *handle, CURLoption option, ...);
CURLcode curl_easy_perform(CURL *handle);
CURLcode curl_easy_getinfo(CURL *handle, CURLINFO info, ...);
const char *curl_easy_strerror(CURLcode code);
struct curl_slist *curl_slist_append(struct curl_slist *list, const char *s);
void curl_slist_free_all(struct curl_slist *list);
CURLcode curl_global_init(long flags);
void curl_global_cleanup(void);

/* ---- test control ------------------------------------------------------- */
#define STUB_MAX_REQUESTS 64
#define STUB_MAX_REPLIES  64

typedef struct {
  int         status;      /* HTTP status to report */
  char       *body;        /* response body (copied) */
  CURLcode    transport_error; /* non-zero: fail instead of answering */
} StubReply;

void stub_reset(void);
void stub_queue_reply(int status, const char *body);
void stub_queue_transport_error(CURLcode code);
void stub_set_default_reply(int status, const char *body);

int         stub_request_count(void);
const char *stub_request_url(int index);
const char *stub_request_body(int index);
const char *stub_request_header(int index, const char *name);
const char *stub_last_header(const char *name);

#ifdef __cplusplus
}
#endif
#endif
