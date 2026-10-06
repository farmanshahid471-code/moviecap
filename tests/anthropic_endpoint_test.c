/* The Claude / Anthropic-compatible planning path.
 *
 * The report these checks come from: "The Anthropic end is not working."
 * The path is driven here against a scripted HTTP transport, so the exact
 * request that goes on the wire (URL, headers, max_tokens, stream flag) and
 * the exact replies Anthropic sends back (a non-streaming JSON object, or a
 * server-sent event stream) are all covered without any network access.
 *
 * Build: compiled against src/generator.c (see CMakeLists.txt, BUILD_UNIT_TESTS).
 * It defines MOVIECAP_UNIT_TEST so the retry backoff does not sleep.
 */
#define main web_ui_main_unused_never_used
#include "../src/generator.c"
#undef main

static int g_pass = 0, g_fail = 0;

static void ck(bool ok, const char *what) {
  if (ok) { g_pass++; printf("[PASS] %s\n", what); }
  else    { g_fail++; printf("[FAIL] %s\n", what); }
}

static void ck_str(const char *got, const char *want, const char *what) {
  bool ok = got && want && strcmp(got, want) == 0;
  if (ok) { g_pass++; printf("[PASS] %s\n", what); }
  else {
    g_fail++;
    printf("[FAIL] %s (got \"%s\", want \"%s\")\n", what,
           got ? got : "(null)", want ? want : "(null)");
  }
}

/* ---------------------------------------------------------------- helpers */

static Config cfg_for(const char *base, const char *model, const char *key) {
  Config c;
  memset(&c, 0, sizeof(c));
  snprintf(c.openai_base_url, sizeof(c.openai_base_url), "%s", base);
  snprintf(c.openai_model, sizeof(c.openai_model), "%s", model);
  snprintf(c.openai_key, sizeof(c.openai_key), "%s", key);
  snprintf(c.recap_language, sizeof(c.recap_language), "English");
  return c;
}

/* A non-streaming Messages reply with one text block. */
static void queue_ok(const char *text) {
  char buf[8192];
  snprintf(buf, sizeof(buf),
           "{\"id\":\"msg_1\",\"type\":\"message\",\"role\":\"assistant\","
           "\"model\":\"claude-sonnet-4-5\",\"stop_reason\":\"end_turn\","
           "\"content\":[{\"type\":\"text\",\"text\":\"%s\"}],"
           "\"usage\":{\"input_tokens\":10,\"output_tokens\":20}}", text);
  stub_queue_reply(200, buf);
}

static const char *PLAN_JSON =
  "{\\\"clips\\\":[{\\\"start\\\":12,\\\"end\\\":20,"
  "\\\"narration\\\":\\\"The story begins in a small town.\\\"}]}";

/* A real Anthropic SSE stream: message_start, one text block, message_delta
 * with the stop reason, message_stop.  `stop` is the stop_reason. */
static void queue_sse(const char *text, const char *stop) {
  char buf[16384];
  snprintf(buf, sizeof(buf),
    "event: message_start\n"
    "data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_1\",\"type\":\"message\","
    "\"role\":\"assistant\",\"model\":\"claude-sonnet-4-5\",\"content\":[],"
    "\"stop_reason\":null,\"usage\":{\"input_tokens\":12,\"output_tokens\":1}}}\n"
    "\n"
    "event: content_block_start\n"
    "data: {\"type\":\"content_block_start\",\"index\":0,"
    "\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n"
    "\n"
    "event: content_block_delta\n"
    "data: {\"type\":\"content_block_delta\",\"index\":0,"
    "\"delta\":{\"type\":\"text_delta\",\"text\":\"%s\"}}\n"
    "\n"
    "event: content_block_stop\n"
    "data: {\"type\":\"content_block_stop\",\"index\":0}\n"
    "\n"
    "event: message_delta\n"
    "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"%s\","
    "\"stop_sequence\":null},\"usage\":{\"output_tokens\":30}}\n"
    "\n"
    "event: message_stop\n"
    "data: {\"type\":\"message_stop\"}\n"
    "\n", text, stop);
  stub_queue_reply(200, buf);
}

/* An SSE stream that ends with an error event instead of a message. */
static void queue_sse_error(const char *type, const char *message) {
  char buf[4096];
  snprintf(buf, sizeof(buf),
    "event: message_start\n"
    "data: {\"type\":\"message_start\",\"message\":{\"content\":[]}}\n"
    "\n"
    "event: error\n"
    "data: {\"type\":\"error\",\"error\":{\"type\":\"%s\",\"message\":\"%s\"}}\n"
    "\n", type, message);
  stub_queue_reply(200, buf);
}

static void queue_error(int status, const char *type, const char *message) {
  char buf[4096];
  snprintf(buf, sizeof(buf),
           "{\"type\":\"error\",\"error\":{\"type\":\"%s\",\"message\":\"%s\"}}",
           type, message);
  stub_queue_reply(status, buf);
}

/* Pull the plan out of a reply the way the pipeline does. */
static ClipPlanList plan_from(const MemBuf *resp) {
  ClipPlanList plan = {0};
  if (!resp || !resp->data) return plan;
  char *text = openai_extract_output_text(resp->data);
  if (!text) return plan;
  plan = parse_clip_plan_json(text);
  free(text);
  return plan;
}

/* Double-check a request body carries the field we expect.  body_is_json()
 * keeps the check meaningful: cJSON never emits spaces. */
static bool body_has(const char *body, const char *needle) {
  return body && strstr(body, needle) != NULL;
}

static MemBuf do_request(Config *cfg, long *code) {
  MemBuf r = anthropic_plan_request(cfg, "You are a test.", "Make a plan.",
                                    code, 30);
  return r;
}

/* ---------------------------------------------------------------- 1. native */

static void test_native_claude_request(void) {
  stub_reset();
  stub_set_default_reply(500, "{\"type\":\"error\",\"error\":{\"message\":\"unexpected\"}}");
  queue_ok(PLAN_JSON);

  Config c = cfg_for("https://api.anthropic.com/v1", "claude-sonnet-4-5",
                     "sk-ant-api03-testkey");
  long code = 0;
  MemBuf r = do_request(&c, &code);

  ck((code == 200), "native Claude answers 200");
  ck(stub_request_count() == 1, "exactly one request goes out (no blind retries)");
  ck_str(stub_request_url(0), "https://api.anthropic.com/v1/messages",
         "the URL is /v1/messages");

  const char *body = stub_request_body(0);
  ck(body_has(body, "\"model\":\"claude-sonnet-4-5\""), "the model name is sent");
  ck(body_has(body, "\"max_tokens\":32000"), "a full-size output budget is asked for");
  ck(body_has(body, "\"stream\":true"),
     "the reply is streamed (Anthropic refuses a non-streamed reply this long)");
  ck(body_has(body, "\"system\":") && body_has(body, "\"messages\":[{"),
     "system + messages are both present");
  ck(!body_has(body, "\"thinking\""), "no thinking field is sent to native Claude");

  ck_str(stub_request_header(0, "x-api-key"), "sk-ant-api03-testkey",
         "the key goes in x-api-key");
  ck_str(stub_request_header(0, "anthropic-version"), "2023-06-01",
         "anthropic-version is sent");
  ck(stub_request_header(0, "authorization") == NULL,
     "no Authorization header (api.anthropic.com rejects it)");
  ck_str(stub_request_header(0, "content-type"), "application/json",
         "content-type is JSON");

  ClipPlanList plan = plan_from(&r);
  ck(plan.count == 1, "the plan is parsed out of the reply");
  ck(plan.count == 1 && strcmp(plan.items[0].narration,
                               "The story begins in a small town.") == 0,
     "the narration survives the round trip");
  free_clip_plan_list(&plan);
  free(r.data);
}

/* ---------------------------------------------------------- 2. streamed reply */

static void test_sse_streamed_reply(void) {
  stub_reset();
  stub_set_default_reply(500, "{\"type\":\"error\",\"error\":{\"message\":\"unexpected\"}}");
  queue_sse(PLAN_JSON, "end_turn");

  Config c = cfg_for("https://api.anthropic.com/v1", "claude-sonnet-4-5",
                     "sk-ant-api03-testkey");
  long code = 0;
  MemBuf r = do_request(&c, &code);
  ck(code == 200, "a streamed reply is accepted");
  ck(stub_request_count() == 1, "a streamed reply needs no retry");

  ClipPlanList plan = plan_from(&r);
  ck(plan.count == 1, "the SSE deltas are folded back into one Messages object");
  ck(plan.count == 1 && strcmp(plan.items[0].narration,
                               "The story begins in a small town.") == 0,
     "the narration stitched out of text_delta events is intact");
  free_clip_plan_list(&plan);
  free(r.data);
}

static void test_sse_error_event(void) {
  stub_reset();
  stub_set_default_reply(500, "{\"type\":\"error\",\"error\":{\"message\":\"unexpected\"}}");
  queue_sse_error("overloaded_error", "Overloaded");

  Config c = cfg_for("https://api.anthropic.com/v1", "claude-sonnet-4-5",
                     "sk-ant-api03-testkey");
  long code = 0;
  MemBuf r = do_request(&c, &code);
  /* an overloaded_error inside the stream is retried, so the run may end on a
     later failure - what matters is that no half reply is taken for a plan */
  ck(code < 200 || code >= 300, "an error inside the stream is not taken for a plan");
  ClipPlanList plan = plan_from(&r);
  ck(plan.count == 0, "no plan comes out of an error stream");
  free_clip_plan_list(&plan);
  free(r.data);
}

static void test_refusal_is_reported(void) {
  stub_reset();
  stub_set_default_reply(500, "{\"type\":\"error\",\"error\":{\"message\":\"unexpected\"}}");
  queue_sse("I cannot write this.", "refusal");

  Config c = cfg_for("https://api.anthropic.com/v1", "claude-sonnet-4-5",
                     "sk-ant-api03-testkey");
  long code = 0;
  MemBuf r = do_request(&c, &code);
  ck(code == 200, "a refusal is a normal reply, not an HTTP error");
  ck(stub_request_count() == 1, "a refusal does not trigger a retry loop");
  ClipPlanList plan = plan_from(&r);
  ck(plan.count == 0, "a refusal yields no clips");
  free_clip_plan_list(&plan);
  free(r.data);
}

/* -------------------------------------------------- 3. cut off / budget rises */

static void test_streamed_reply_cut_off_raises_budget(void) {
  stub_reset();
  stub_set_default_reply(500, "{\"type\":\"error\",\"error\":{\"message\":\"unexpected\"}}");
  queue_sse("{\"clips\":[{\"start\":1,\"end\":9,\"narration\":\"A first clip that fits.\"}]",
            "max_tokens");
  queue_ok(PLAN_JSON);

  Config c = cfg_for("https://api.anthropic.com/v1", "claude-sonnet-4-5",
                     "sk-ant-api03-testkey");
  long code = 0;
  MemBuf r = do_request(&c, &code);
  ck(code == 200, "the retry after a cut-off stream answers 200");
  ck(stub_request_count() == 2, "a cut-off stream is retried once with more room");
  ck(body_has(stub_request_body(1), "\"max_tokens\":64000"),
     "the second request doubles the output budget");
  ck(body_has(stub_request_body(1), "\"stream\":true"),
     "the bigger request is streamed as well");
  ClipPlanList plan = plan_from(&r);
  ck(plan.count == 1, "the retried request yields the full plan");
  free_clip_plan_list(&plan);
  free(r.data);
}

static void test_max_tokens_rejection_lowers_budget(void) {
  stub_reset();
  stub_set_default_reply(500, "{\"type\":\"error\",\"error\":{\"message\":\"unexpected\"}}");
  queue_error(400, "invalid_request_error",
              "max_tokens: 32000 > 21333, which is the maximum allowed number of "
              "output tokens for claude-sonnet-4-5");
  queue_ok(PLAN_JSON);

  Config c = cfg_for("https://api.anthropic.com/v1", "claude-sonnet-4-5",
                     "sk-ant-api03-testkey");
  long code = 0;
  MemBuf r = do_request(&c, &code);
  ck(code == 200, "the request succeeds after the provider names its ceiling");
  ck(stub_request_count() == 2, "the rejected budget is retried once");
  ck(body_has(stub_request_body(1), "\"max_tokens\":21333"),
     "the retry uses the limit from the error message");
  ck(body_has(stub_request_body(1), "\"stream\":true"),
     "a budget above the non-streaming ceiling stays streamed");
  ClipPlanList plan = plan_from(&r);
  ck(plan.count == 1, "the retry yields a usable plan");
  free_clip_plan_list(&plan);
  free(r.data);
}

static void test_transient_errors_are_retried(void) {
  /* 429 then 529 then success: Anthropic asks clients to back off and retry. */
  stub_reset();
  stub_set_default_reply(500, "{\"type\":\"error\",\"error\":{\"message\":\"unexpected\"}}");
  queue_error(429, "rate_limit_error", "Number of request tokens has exceeded your rate limit");
  queue_error(529, "overloaded_error", "Overloaded");
  queue_ok(PLAN_JSON);

  Config c = cfg_for("https://api.anthropic.com/v1", "claude-sonnet-4-5",
                     "sk-ant-api03-testkey");
  long code = 0;
  MemBuf r = do_request(&c, &code);
  ck(code == 200, "a rate limit and an overload are retried until they clear");
  ck(stub_request_count() == 3, "two transient failures cost two extra requests");
  ck(body_has(stub_request_body(2), "\"stream\":true"),
     "the retry keeps streaming (the failure was not about the request shape)");
  ClipPlanList plan = plan_from(&r);
  ck(plan.count == 1, "the retry wins the plan");
  free_clip_plan_list(&plan);
  free(r.data);
}

/* ------------------------------------------------------- 4. gateways / keys */

static void test_gateway_gets_both_headers(void) {
  stub_reset();
  stub_set_default_reply(500, "{\"type\":\"error\",\"error\":{\"message\":\"unexpected\"}}");
  queue_ok(PLAN_JSON);

  Config c = cfg_for("https://api.deepseek.com/anthropic", "claude-sonnet-4-5",
                     "sk-gateway-key");
  long code = 0;
  MemBuf r = do_request(&c, &code);

  ck(code == 200, "the gateway answers 200");
  ck_str(stub_request_url(0), "https://api.deepseek.com/anthropic/v1/messages",
         "the gateway's /anthropic path gets /v1/messages appended");
  ck_str(stub_request_header(0, "x-api-key"), "sk-gateway-key",
         "gateways get x-api-key");
  ck_str(stub_request_header(0, "authorization"), "Bearer sk-gateway-key",
         "gateways also get the key as a Bearer token");
  ck(body_has(stub_request_body(0), "\"thinking\":{\"type\":\"disabled\"}"),
     "thinking is disabled for the gateway so the JSON is not truncated");
  ClipPlanList plan = plan_from(&r);
  ck(plan.count == 1, "the gateway plan is parsed");
  free_clip_plan_list(&plan);
  free(r.data);
}

static void test_gateway_without_streaming_recovers(void) {
  stub_reset();
  stub_set_default_reply(500, "{\"type\":\"error\",\"error\":{\"message\":\"unexpected\"}}");
  queue_error(400, "invalid_request_error", "stream is not supported by this endpoint");
  queue_ok(PLAN_JSON);

  Config c = cfg_for("https://gw.example.com/anthropic", "claude-sonnet-4-5",
                     "sk-gateway-key");
  long code = 0;
  MemBuf r = do_request(&c, &code);
  ck(code == 200, "a gateway that cannot stream still yields a plan");
  ck(stub_request_count() == 2, "the streaming attempt is retried without streaming");
  ck(!body_has(stub_request_body(1), "\"stream\":true"),
     "the retry is a plain non-streaming request");
  ck(body_has(stub_request_body(1), "\"max_tokens\":16000"),
     "the non-streaming retry stays under the 10 minute ceiling");
  ClipPlanList plan = plan_from(&r);
  ck(plan.count == 1, "the non-streaming reply is parsed as usual");
  free_clip_plan_list(&plan);
  free(r.data);
}

static void test_thinking_rejection_recovers(void) {
  stub_reset();
  stub_set_default_reply(500, "{\"type\":\"error\",\"error\":{\"message\":\"unexpected\"}}");
  queue_error(400, "invalid_request_error",
              "thinking: this model does not support the thinking parameter");
  queue_ok(PLAN_JSON);

  Config c = cfg_for("https://api.deepseek.com/anthropic", "deepseek-chat", "sk-gateway-key");
  long code = 0;
  MemBuf r = do_request(&c, &code);
  ck(code == 200, "a rejected thinking field is retried without it");
  ck(stub_request_count() == 2, "the thinking retry costs one extra request");
  ck(!body_has(stub_request_body(1), "\"thinking\""),
     "the retry drops the thinking field");
  ClipPlanList plan = plan_from(&r);
  ck(plan.count == 1, "the plan arrives after dropping thinking");
  free_clip_plan_list(&plan);
  free(r.data);
}

static void test_oauth_token_is_bearer_only(void) {
  stub_reset();
  stub_set_default_reply(500, "{\"type\":\"error\",\"error\":{\"message\":\"unexpected\"}}");
  queue_ok(PLAN_JSON);

  Config c = cfg_for("https://api.anthropic.com/v1", "claude-sonnet-4-5",
                     "sk-ant-oat01-someoauthToken");
  long code = 0;
  MemBuf r = do_request(&c, &code);
  ck(code == 200, "an OAuth token is accepted on the native host");
  ck(stub_request_header(0, "x-api-key") == NULL,
     "an OAuth token is never sent as x-api-key");
  ck_str(stub_request_header(0, "authorization"), "Bearer sk-ant-oat01-someoauthToken",
         "an OAuth token goes in the Authorization header");
  free(r.data);
}

/* ------------------------------------------------------------- 5. URL shapes */

static void test_endpoint_shapes(void) {
  char out[560];
  anthropic_endpoint("https://api.anthropic.com/v1", out, sizeof(out));
  ck_str(out, "https://api.anthropic.com/v1/messages", "the documented base URL");
  anthropic_endpoint("https://api.anthropic.com/v1/", out, sizeof(out));
  ck_str(out, "https://api.anthropic.com/v1/messages", "a trailing slash is tolerated");
  anthropic_endpoint("https://api.anthropic.com", out, sizeof(out));
  ck_str(out, "https://api.anthropic.com/v1/messages", "a bare host gets /v1/messages");
  anthropic_endpoint("https://api.anthropic.com/v1/messages", out, sizeof(out));
  ck_str(out, "https://api.anthropic.com/v1/messages",
         "a pasted /messages endpoint is not doubled");
  anthropic_endpoint("https://api.deepseek.com/anthropic", out, sizeof(out));
  ck_str(out, "https://api.deepseek.com/anthropic/v1/messages",
         "the gateway path keeps /anthropic");
  anthropic_endpoint("https://api.deepseek.com/anthropic/", out, sizeof(out));
  ck_str(out, "https://api.deepseek.com/anthropic/v1/messages",
         "the gateway path tolerates a trailing slash");

  anthropic_openai_base("https://api.deepseek.com/anthropic", out, sizeof(out));
  ck_str(out, "https://api.deepseek.com", "the OpenAI fallback drops /anthropic");
  anthropic_openai_base("https://gw.example.com/anthropic/v1/messages", out, sizeof(out));
  ck_str(out, "https://gw.example.com",
         "the OpenAI fallback drops the pasted tail, /v1 and /anthropic");
  anthropic_openai_base("https://api.openai.com/v1", out, sizeof(out));
  ck_str(out, "https://api.openai.com", "a plain /v1 base is peeled back");

  ck(anthropic_host_is_native("https://api.anthropic.com/v1"),
     "api.anthropic.com is recognised");
  ck(anthropic_host_is_native("https://API.Anthropic.COM/v1"),
     "the native host is recognised whatever the case");
  ck(!anthropic_host_is_native("https://api.deepseek.com/anthropic"),
     "a gateway is not the native host");

  /* a gateway that only serves /v1/messages (no /v1 prefix in its base) */
  anthropic_endpoint("https://gw.example.com/api/anthropic", out, sizeof(out));
  ck_str(out, "https://gw.example.com/api/anthropic/v1/messages",
         "an arbitrary prefix is preserved");
}

/* -------------------------------------------------- 6. the whole plan call */

static void test_openai_make_plan_uses_messages_api(void) {
  stub_reset();
  stub_set_default_reply(500, "{\"type\":\"error\",\"error\":{\"message\":\"unexpected\"}}");
  queue_ok(PLAN_JSON);

  /* Deliberately upper case: the base URL check must not care. */
  Config c = cfg_for("https://API.ANTHROPIC.COM/v1", "claude-sonnet-4-5",
                     "sk-ant-api03-testkey");
  snprintf(c.recap_language, sizeof(c.recap_language), "English");
  c.recap_minutes = 0;

  ClipPlanList plan = openai_make_plan(&c, "Free Run (2026)",
                                       "1\n12 --> 20\nThe story starts here.\n\n",
                                       "", "", false, 2, 12, NULL, NULL, NULL);
  ck(stub_request_count() >= 1, "the pipeline reaches the API");
  ck(stub_request_url(0) != NULL &&
     strstr(stub_request_url(0), "/v1/messages") != NULL,
     "an upper case Anthropic base still uses the Messages API");
  ck(plan.count == 1, "the plan comes back through openai_make_plan");
  free_clip_plan_list(&plan);
}

static void test_plain_json_is_not_touched(void) {
  /* A non-streaming Messages reply is plain JSON: the SSE folding must leave
     it completely alone (it is only a pass-through for event streams). */
  ck(anthropic_sse_fold("{\"content\":[{\"type\":\"text\",\"text\":\"hi\"}]}") == NULL,
     "a plain JSON reply is not mistaken for an event stream");
  ck(anthropic_sse_fold("") == NULL, "an empty body folds to nothing");
  ck(anthropic_sse_fold("<html>502 Bad Gateway</html>") == NULL,
     "an HTML error page from a proxy folds to nothing");

  /* and a real stream still folds, even without a stop_reason */
  char *folded = anthropic_sse_fold(
    "event: content_block_delta\n"
    "data: {\"type\":\"content_block_delta\",\"index\":0,"
    "\"delta\":{\"type\":\"text_delta\",\"text\":\"Hello \"}}\n\n"
    "event: content_block_delta\n"
    "data: {\"type\":\"content_block_delta\",\"index\":0,"
    "\"delta\":{\"type\":\"text_delta\",\"text\":\"world\"}}\n\n");
  ck(folded && strstr(folded, "Hello world") != NULL,
     "text deltas are stitched together in order");
  ck(folded && strstr(folded, "\"stop_reason\"") == NULL,
     "a stream without a stop_reason folds without inventing one");
  free(folded);

  /* thinking deltas must not leak into the plan text */
  char *with_thinking = anthropic_sse_fold(
    "event: content_block_delta\n"
    "data: {\"type\":\"content_block_delta\",\"index\":0,"
    "\"delta\":{\"type\":\"thinking_delta\",\"thinking\":\"let me think\"}}\n\n"
    "event: content_block_delta\n"
    "data: {\"type\":\"content_block_delta\",\"index\":1,"
    "\"delta\":{\"type\":\"text_delta\",\"text\":\"{\\\"clips\\\":[]}\"}}\n\n");
  ck(with_thinking && strstr(with_thinking, "let me think") == NULL,
     "thinking deltas are not mixed into the answer");
  ck(with_thinking && strstr(with_thinking, "clips") != NULL,
     "the answer after a thinking block is kept");
  free(with_thinking);
}

/* ------------------------------------------- 7. what happens when it fails */

/* A wrong model name / wrong host on api.anthropic.com is a 404.  The app must
 * say so and must NOT waste a request on /chat/completions, which does not
 * exist on that host. */
static void test_native_failure_says_why(void) {
  stub_reset();
  stub_set_default_reply(500, "{\"type\":\"error\",\"error\":{\"message\":\"unexpected\"}}");
  queue_error(404, "not_found_error", "model: claude-typo-9 does not exist");

  Config c = cfg_for("https://api.anthropic.com/v1", "claude-typo-9", "sk-ant-api03-testkey");
  c.recap_minutes = 0;
  ClipPlanList plan = openai_make_plan(&c, "Free Run (2026)",
                                       "1\n12 --> 20\nThe story starts here.\n\n",
                                       "", "", false, 2, 12, NULL, NULL, NULL);
  ck(plan.count == 0, "a 404 yields no plan instead of a bogus one");
  ck(stub_request_count() == 1,
     "api.anthropic.com is not retried on /chat/completions (it does not exist there)");
  ck(stub_request_url(0) != NULL && strstr(stub_request_url(0), "api.anthropic.com") != NULL,
     "the request that was made went to the Anthropic host");
  free_clip_plan_list(&plan);
}

/* A gateway whose /anthropic path fails gets one rescue attempt on its OpenAI
 * style /chat/completions endpoint, derived from the same base URL. */
static void test_gateway_rescue_url(void) {
  stub_reset();
  stub_set_default_reply(500, "{\"type\":\"error\",\"error\":{\"message\":\"unexpected\"}}");
  queue_error(503, "api_error", "upstream unavailable");

  Config c = cfg_for("https://gw.example.com/anthropic/v1", "claude-sonnet-4-5", "sk-gateway-key");
  ClipPlanList plan = openai_make_plan(&c, "Free Run (2026)",
                                       "1\n12 --> 20\nThe story starts here.\n\n",
                                       "", "", false, 2, 12, NULL, NULL, NULL);
  ck(plan.count == 0, "a failing gateway yields no plan");
  bool saw_chat = false, saw_messages = false;
  for (int i = 0; i < stub_request_count(); i++) {
    if (strstr(stub_request_url(i), "/chat/completions")) saw_chat = true;
    if (strstr(stub_request_url(i), "/messages")) saw_messages = true;
  }
  ck(saw_messages, "the gateway was first asked on its /v1/messages endpoint");
  ck(saw_chat, "the gateway rescue went to /chat/completions");
  ck(stub_request_url(stub_request_count() - 1) != NULL &&
     strcmp(stub_request_url(stub_request_count() - 1),
            "https://gw.example.com/chat/completions") == 0,
     "the rescued URL has /anthropic, /v1 and the pasted tail peeled off");
  free_clip_plan_list(&plan);
}

int main(void) {
  test_endpoint_shapes();
  test_native_claude_request();
  test_sse_streamed_reply();
  test_sse_error_event();
  test_refusal_is_reported();
  test_streamed_reply_cut_off_raises_budget();
  test_max_tokens_rejection_lowers_budget();
  test_transient_errors_are_retried();
  test_gateway_gets_both_headers();
  test_gateway_without_streaming_recovers();
  test_thinking_rejection_recovers();
  test_oauth_token_is_bearer_only();
  test_openai_make_plan_uses_messages_api();
  test_plain_json_is_not_touched();
  test_native_failure_says_why();
  test_gateway_rescue_url();

  printf("\n%d checks, %d failures\n", g_pass + g_fail, g_fail);
  if (g_fail == 0) printf("ALL OK\n");
  return g_fail == 0 ? 0 : 1;
}
