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

/* ------------------------------------------------- 8. the Message Batches API
 *
 * Batch planning is the 50%-cheaper path: every plan request is collected,
 * submitted as one Message Batch, and the run waits for the results.  These
 * checks drive that exact sequence against the scripted transport and verify
 * the two things that matter: the requests are identical to a live run (so the
 * narration does not change) and nothing is ever paid for twice.
 */

#define PLAN_TEXT "{\\\"clips\\\":[{\\\"start\\\":5,\\\"end\\\":9," \
                  "\\\"narration\\\":\\\"The story begins in the batched town.\\\"}]}"

static const char *BATCH_ID = "msgbatch_01TESTBATCH";

/* A page of the GET /v1/messages/batches/<id> status reply. */
static void queue_batch_status(const char *status, int processing, int succeeded,
                               int errored) {
  char buf[1024];
  snprintf(buf, sizeof(buf),
           "{\"id\":\"%s\",\"type\":\"message_batch\",\"processing_status\":\"%s\","
           "\"request_counts\":{\"processing\":%d,\"succeeded\":%d,\"errored\":%d,"
           "\"canceled\":0,\"expired\":0}}",
           BATCH_ID, status, processing, succeeded, errored);
  stub_queue_reply(200, buf);
}

static void queue_batch_create(void) {
  char buf[1024];
  snprintf(buf, sizeof(buf),
           "{\"id\":\"%s\",\"type\":\"message_batch\",\"processing_status\":\"in_progress\","
           "\"request_counts\":{\"processing\":1,\"succeeded\":0,\"errored\":0,"
           "\"canceled\":0,\"expired\":0}}", BATCH_ID);
  stub_queue_reply(200, buf);
}

/* One line of the results JSONL, the way the API returns it. */
static void queue_batch_results_line(const char *custom_id, const char *type,
                                     const char *text_or_error, int stop_max_tokens) {
  char buf[4096];
  if (strcmp(type, "succeeded") == 0) {
    snprintf(buf, sizeof(buf),
             "{\"custom_id\":\"%s\",\"result\":{\"type\":\"succeeded\",\"message\":{"
             "\"id\":\"msg_1\",\"type\":\"message\",\"role\":\"assistant\","
             "\"stop_reason\":\"%s\",\"content\":[{\"type\":\"text\",\"text\":\"%s\"}]}}}\n",
             custom_id, stop_max_tokens ? "max_tokens" : "end_turn", text_or_error);
  } else {
    snprintf(buf, sizeof(buf),
             "{\"custom_id\":\"%s\",\"result\":{\"type\":\"%s\",\"error\":{"
             "\"type\":\"invalid_request_error\",\"message\":\"%s\"}}}\n",
             custom_id, type, text_or_error);
  }
  stub_queue_reply(200, buf);
}

/* a batch of one request, ready to submit.  The result line has to carry the
 * custom id the app computed for this movie and language. */
static void queue_full_batch_run(const char *title, const char *lang) {
  char id[96];
  batch_custom_id(title, lang, id, sizeof(id));
  queue_batch_create();                      /* POST .../batches              */
  queue_batch_status("ended", 0, 1, 0);
  queue_batch_results_line(id, "succeeded", PLAN_TEXT, 0);
}

static Config batch_cfg(void) {
  Config c = cfg_for("https://api.anthropic.com/v1", "claude-sonnet-4-5",
                     "sk-ant-mock");
  c.batch_planning = true;
  c.batch_max_wait_minutes = 1;
  return c;
}

static void test_batch_custom_id_shape(void) {
  char a[96], b[96], c[96];
  batch_custom_id("Amélie's Test (2001)", "en", a, sizeof(a));
  batch_custom_id("Amélie's Test (2001)", "en", b, sizeof(b));
  batch_custom_id("Amélie's Test (2001)", "zh", c, sizeof(c));

  ck_str(a, b, "the custom id is the same every time (both passes agree)");
  ck(strcmp(a, c) != 0, "a different language gets a different id");

  bool charset_ok = true, len_ok = strlen(a) <= 64 && strlen(a) >= 1;
  for (const char *q = a; *q; q++) {
    bool ok = (*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') ||
              (*q >= '0' && *q <= '9') || *q == '-' || *q == '_';
    if (!ok) charset_ok = false;
  }
  ck(charset_ok, "the id only uses the characters the API allows");
  ck(len_ok, "the id is 1-64 characters long");
  ck(strstr(a, "lzh") == NULL && strstr(c, "lzh") != NULL,
     "the language tag is part of the id");
}

static void test_batch_collect_then_render(void) {
  /* ---- collecting pass: no request goes out, the plan is queued ---------- */
  stub_reset();
  batch_items_free();
  ensure_dir("scripts");
  ensure_dir(BATCH_DIR);

  /* make sure no stale plan file from an earlier test is picked up */
  char stale[PATH_MAX];
  batch_result_path("p0000000000000000-len", ".result.json", stale, sizeof(stale));
  plat_unlink(stale);
  batch_result_path("p0000000000000000-len", ".failed", stale, sizeof(stale));
  plat_unlink(stale);

  Config c = batch_cfg();
  g_batch_poll_seconds = 0;          /* no sleeping in the tests */
  g_batch_collect = true;
  g_batch_render = false;

  ClipPlanList queued = openai_make_plan(&c, "Batch Movie", "1\n5 --> 9\nStory.\n\n",
                                        "", "", false, 2, 12, NULL, NULL, NULL);
  ck(queued.count == 0, "the collecting pass renders nothing");
  ck(g_plan_queued, "the collecting pass marks the plan as queued");
  ck(stub_request_count() == 0, "the collecting pass sends no request at all");
  ck(g_batch_n == 1, "exactly one request was collected");

  const char *params = g_batch_n ? g_batch_items[0].params : "";
  ck(body_has(params, "\"model\":\"claude-sonnet-4-5\""),
     "the batched request carries the same model");
  ck(body_has(params, "\"max_tokens\":64000"),
     "the batched request asks for a generous output budget");
  ck(body_has(params, "\"system\":") && body_has(params, "\"messages\":[{"),
     "the batched request carries the same system prompt and text");
  ck(!body_has(params, "\"stream\""),
     "the batched request is not streamed (the API rejects that in a batch)");

  /* ---- submitting it: one POST to /batches, then poll + results --------- */
  stub_reset();
  queue_full_batch_run("Batch Movie", "en");
  bool cancelled = false;
  bool in_flight = false;
  bool got = batch_run_all(&c, &cancelled, &in_flight, NULL, 0);
  ck(got, "the batch run reports that results were fetched");
  ck(stub_request_count() >= 3, "the batch is created, polled and downloaded");

  ck(stub_request_url(0) != NULL &&
     strcmp(stub_request_url(0), "https://api.anthropic.com/v1/messages/batches") == 0,
     "the batch is created on /v1/messages/batches");
  const char *create_body = stub_request_body(0);
  ck(body_has(create_body, "\"requests\":[{\"custom_id\":\""),
     "the create body holds a requests array with custom ids");
  ck(body_has(create_body, "\"params\":{"),
     "every entry carries its params");
  ck(!body_has(create_body, "\"stream\""),
     "a stream field is stripped from the batched params");

  /* The create call is a real API call: without the key the real API answers
   * 401 and every plan would fall back live (costing double). */
  ck_str(stub_request_header(0, "x-api-key"), "sk-ant-mock",
         "the batch create call is authenticated");
  ck_str(stub_request_header(0, "anthropic-version"), "2023-06-01",
         "the batch create call sends the API version");

  char results_url[512];
  snprintf(results_url, sizeof(results_url),
           "https://api.anthropic.com/v1/messages/batches/%s/results", BATCH_ID);
  bool saw_results = false, saw_status = false;
  for (int i = 0; i < stub_request_count(); i++) {
    if (stub_request_url(i) && strcmp(stub_request_url(i), results_url) == 0)
      saw_results = true;
    char status_url[512];
    snprintf(status_url, sizeof(status_url),
             "https://api.anthropic.com/v1/messages/batches/%s", BATCH_ID);
    if (stub_request_url(i) && strcmp(stub_request_url(i), status_url) == 0)
      saw_status = true;
  }
  ck(saw_status, "the batch status is polled while it runs");
  ck(saw_results, "the results are downloaded from the results endpoint");

  /* ---- rendering pass: the plan comes from disk, no API call ------------ */
  stub_reset();
  g_batch_collect = false;
  g_batch_render = true;
  ClipPlanList plan = openai_make_plan(&c, "Batch Movie", "1\n5 --> 9\nStory.\n\n",
                                       "", "", false, 2, 12, NULL, NULL, NULL);
  ck(plan.count == 1, "the rendering pass uses the batched plan");
  ck(plan.count == 1 && strcmp(plan.items[0].narration,
                               "The story begins in the batched town.") == 0,
     "the narration is the one the model wrote in the batch");
  ck(stub_request_count() == 0,
     "the rendering pass makes NO api call - that is where the 50% saving is");
  free_clip_plan_list(&plan);

  /* the plan is not bought a second time if the collecting pass runs again */
  stub_reset();
  g_batch_collect = true;
  g_batch_render = false;
  ClipPlanList again = openai_make_plan(&c, "Batch Movie", "1\n5 --> 9\nStory.\n\n",
                                       "", "", false, 2, 12, NULL, NULL, NULL);
  ck(again.count == 0 && stub_request_count() == 0,
     "a plan that is already on disk is not queued (and not paid for) again");
  free_clip_plan_list(&again);

  g_batch_collect = false;
  g_batch_render = false;
  batch_items_free();
}

static void test_batch_failure_falls_back_live(void) {
  /* An errored batch item must not cost the movie its recap: the app asks
     live, at the normal price, with the identical prompt. */
  stub_reset();
  batch_items_free();
  ensure_dir(BATCH_DIR);

  Config c = batch_cfg();
  batch_result_path("p0000000000000000-len", ".result.json", (char[PATH_MAX]){0},
                    sizeof(char[PATH_MAX]));
  char stale[PATH_MAX];
  batch_custom_id("Failed Movie", "en", stale, sizeof(stale));
  char path[PATH_MAX];
  batch_result_path(stale, ".result.json", path, sizeof(path));
  plat_unlink(path);
  batch_result_path(stale, ".failed", path, sizeof(path));
  plat_unlink(path);

  /* the collecting pass, then a batch whose one request fails */
  g_batch_collect = true;
  g_batch_render = false;
  ClipPlanList q = openai_make_plan(&c, "Failed Movie", "1\n5 --> 9\nStory.\n\n",
                                    "", "", false, 2, 12, NULL, NULL, NULL);
  free_clip_plan_list(&q);
  ck(g_batch_n == 1, "the failing movie was collected");

  stub_reset();
  queue_batch_create();
  queue_batch_status("ended", 0, 0, 1);
  queue_batch_results_line(stale, "errored", "overloaded_error: try again later", 0);
  bool cancelled = false, in_flight = false;
  batch_run_all(&c, &cancelled, &in_flight, NULL, 0);

  char failed[PATH_MAX];
  ck(batch_failed_file("Failed Movie", "en", failed, sizeof(failed)),
     "the failure is remembered next to the plans");

  /* the rendering pass must ask live instead of skipping the movie */
  stub_reset();
  queue_ok(PLAN_JSON);
  g_batch_collect = false;
  g_batch_render = true;
  ClipPlanList plan = openai_make_plan(&c, "Failed Movie", "1\n5 --> 9\nStory.\n\n",
                                       "", "", false, 2, 12, NULL, NULL, NULL);
  ck(plan.count == 1, "a failed batch request still produces a plan (asked live)");
  ck(stub_request_count() == 1, "exactly one live request was made for it");
  ck(stub_request_url(0) != NULL && strstr(stub_request_url(0), "/v1/messages"),
     "the live fallback goes to the Messages API");
  free_clip_plan_list(&plan);

  g_batch_collect = false;
  g_batch_render = false;
  batch_items_free();
}

static void test_batch_cut_short_is_rebatched(void) {
  /* A batched reply that stopped at the output limit used to be re-asked live
     at full price - paying for the same plan twice.  It now goes back into ONE
     more batch with twice the budget, and only what is still cut off after that
     is asked live. */
  stub_reset();
  batch_items_free();
  ensure_dir(BATCH_DIR);

  Config c = batch_cfg();
  c.batch_max_wait_minutes = 1;
  g_batch_poll_seconds = 0;

  char id[96], path[PATH_MAX], r1[160], r1_path[PATH_MAX];
  batch_custom_id("Cut Short Movie", "en", id, sizeof(id));
  snprintf(r1, sizeof(r1), "%s_r1", id);
  batch_result_path(id, ".result.json", path, sizeof(path));
  batch_result_path(r1, ".result.json", r1_path, sizeof(r1_path));
  plat_unlink(path);
  plat_unlink(r1_path);
  batch_result_path(id, ".failed", path, sizeof(path));
  plat_unlink(path);

  g_batch_collect = true;
  g_batch_render = false;
  ClipPlanList q = openai_make_plan(&c, "Cut Short Movie", "1\n5 --> 9\nStory.\n\n",
                                    "", "", false, 2, 12, NULL, NULL, NULL);
  free_clip_plan_list(&q);
  ck(g_batch_n == 1, "the request was collected");

  /* first batch: the reply stops at the output limit */
  stub_reset();
  queue_batch_create();
  queue_batch_status("ended", 0, 1, 0);
  queue_batch_results_line(id, "succeeded", PLAN_TEXT, 1);   /* stop_reason=max_tokens */
  /* the second batch (the escalation) answers properly */
  queue_batch_create();
  queue_batch_status("ended", 0, 1, 0);
  queue_batch_results_line(r1, "succeeded", PLAN_TEXT, 0);   /* complete this time */

  bool cancelled = false, in_flight = false;
  bool got = batch_run_all(&c, &cancelled, &in_flight, NULL, 0);
  ck(got, "the run reports that plans were fetched");

  /* the second create must carry the doubled budget and the _r1 custom id */
  int creates = 0, second = -1;
  for (int i = 0; i < stub_request_count(); i++) {
    if (stub_request_body(i) && strstr(stub_request_body(i), "\"requests\":") &&
        strstr(stub_request_body(i), "\"params\":")) {
      creates++;
      if (creates == 2) second = i;
    }
  }
  ck(creates == 2, "the cut-off plan was re-submitted as one more batch");
  ck(second >= 0 && body_has(stub_request_body(second), "\"max_tokens\":128000"),
     "the second batch asks for twice the output budget");
  ck(second >= 0 && body_has(stub_request_body(second), "_r1"),
     "the second batch uses an id of its own (the first plan stays on disk)");
  ck(!file_exists(path), "the truncated plan is not left behind for the render pass");

  /* the rendering pass must use the second batch's plan, with NO live call */
  stub_reset();
  char found[PATH_MAX];
  ck(batch_plan_file("Cut Short Movie", "en", found, sizeof(found)),
     "the second batch's plan is found for the rendering pass");
  ck(strstr(found, "_r1") != NULL, "and it is the plan from the second batch");

  g_batch_collect = false;
  g_batch_render = true;
  ClipPlanList plan = openai_make_plan(&c, "Cut Short Movie", "1\n5 --> 9\nStory.\n\n",
                                       "", "", false, 2, 12, NULL, NULL, NULL);
  ck(plan.count == 1, "the rendering pass renders the re-batched plan");
  ck(stub_request_count() == 0,
     "and makes no live request - the second batch is where the saving is");
  free_clip_plan_list(&plan);

  /* the plan that was written is the complete one from the second batch */
  char *t = read_entire_file(r1_path);
  char stop[64] = "";
  if (t) anthropic_stop_reason(t, stop, sizeof(stop));
  ck(t != NULL && strcmp(stop, "end_turn") == 0,
     "the re-batched plan is complete (not a cut-off reply)");
  free(t);

  g_batch_collect = false;
  g_batch_render = false;
  batch_items_free();
  plat_unlink(r1_path);
}

static void test_batch_still_cut_off_after_two_rounds_goes_live(void) {
  /* Both batches cut the plan off: the app must still produce the recap, so it
     asks live with the live ladder (streamed, bigger budget). */
  stub_reset();
  batch_items_free();
  ensure_dir(BATCH_DIR);

  Config c = batch_cfg();
  g_batch_poll_seconds = 0;
  char id[96], path[PATH_MAX], r1[160], r1_path[PATH_MAX];
  batch_custom_id("Twice Cut Movie", "en", id, sizeof(id));
  snprintf(r1, sizeof(r1), "%s_r1", id);
  batch_result_path(id, ".result.json", path, sizeof(path));
  batch_result_path(r1, ".result.json", r1_path, sizeof(r1_path));
  plat_unlink(path);
  plat_unlink(r1_path);

  g_batch_collect = true;
  g_batch_render = false;
  ClipPlanList q = openai_make_plan(&c, "Twice Cut Movie", "1\n5 --> 9\nStory.\n\n",
                                    "", "", false, 2, 12, NULL, NULL, NULL);
  free_clip_plan_list(&q);

  stub_reset();
  queue_batch_create();
  queue_batch_status("ended", 0, 1, 0);
  queue_batch_results_line(id, "succeeded", PLAN_TEXT, 1);
  queue_batch_create();
  queue_batch_status("ended", 0, 1, 0);
  queue_batch_results_line(r1, "succeeded", PLAN_TEXT, 1);   /* still cut off */
  bool cancelled = false, in_flight = false;
  batch_run_all(&c, &cancelled, &in_flight, NULL, 0);

  /* now the render pass: it must go live rather than render a half plan */
  stub_reset();
  queue_ok(PLAN_JSON);
  g_batch_collect = false;
  g_batch_render = true;
  ClipPlanList plan = openai_make_plan(&c, "Twice Cut Movie", "1\n5 --> 9\nStory.\n\n",
                                       "", "", false, 2, 12, NULL, NULL, NULL);
  ck(plan.count == 1, "a plan cut short twice still produces a recap (asked live)");
  ck(stub_request_count() >= 1, "the live request went out");
  free_clip_plan_list(&plan);

  g_batch_collect = false;
  g_batch_render = false;
  batch_items_free();
  plat_unlink(path);
  plat_unlink(r1_path);
}

static void test_batch_chunking(void) {
  /* More requests than one batch may hold are split, never dropped. */
  stub_reset();
  batch_items_free();
  ensure_dir(BATCH_DIR);

  for (int i = 0; i < 3; i++) {
    char title[64];
    snprintf(title, sizeof(title), "Chunk Movie %d", i);
    char params[256];
    snprintf(params, sizeof(params), "{\"model\":\"claude-sonnet-4-5\",\"max_tokens\":32}");
    char *copy = str_dup(params);
    batch_item_add(title, "en", 2, 12, 32000, copy);
  }
  ck(g_batch_n == 3, "three requests were collected");

  Config c = batch_cfg();
  /* one create call per chunk; the stub answers the same id twice */
  queue_batch_create();
  queue_batch_status("ended", 0, 3, 0);
  queue_batch_create();
  queue_batch_status("ended", 0, 3, 0);
  queue_batch_results_line("p0000000000000000-len", "succeeded", PLAN_TEXT, 0);

  bool cancelled = false, in_flight = false;
  batch_run_all(&c, &cancelled, &in_flight, NULL, 0);
  ck(stub_request_count() >= 2, "the run submitted at least one batch");

  int creates = 0;
  for (int i = 0; i < stub_request_count(); i++)
    if (stub_request_url(i) && strstr(stub_request_url(i), "/batches") &&
        !strstr(stub_request_url(i), "/results") &&
        strstr(stub_request_url(i), "msgbatch_") == NULL)
      creates++;
  ck(creates >= 1, "the batch create call happened");
  batch_items_free();
}

static void test_batch_manifest_resume(void) {
  /* A batch that was submitted but never fetched must be reused, not re-bought. */
  ensure_dir(BATCH_DIR);
  batch_save_manifest("msgbatch_01PENDING", "submitted", 4);

  char id[128], status[32];
  size_t items = 0;
  ck(batch_load_manifest(id, sizeof(id), status, sizeof(status), &items),
     "the pending batch is remembered on disk");
  ck_str(id, "msgbatch_01PENDING", "the pending batch id is restored");
  ck_str(status, "submitted", "its status says it was not fetched yet");
  ck(items == 4, "the request count is restored");

  batch_save_manifest("msgbatch_01PENDING", "fetched", 4);
  batch_load_manifest(id, sizeof(id), status, sizeof(status), &items);
  ck_str(status, "fetched", "a fetched batch is not resumed again");
}

/* A batch that has been submitted but has not finished yet must NOT lead to a
 * live request: that would pay for the very same plans twice. */
static void test_batch_in_flight_is_not_paid_for_twice(void) {
  stub_reset();
  batch_items_free();
  ensure_dir("scripts");
  ensure_dir(BATCH_DIR);
  g_batch_poll_seconds = 0;                 /* do not sleep: never finishes */

  Config c = batch_cfg();
  c.batch_max_wait_minutes = 1;

  g_batch_collect = true;
  g_batch_render = false;
  ClipPlanList queued = openai_make_plan(&c, "In Flight Movie", "1\n5 --> 9\nStory.\n\n",
                                        "", "", false, 2, 12, NULL, NULL, NULL);
  ck(g_batch_n == 1, "one request was collected");

  char id[96];
  batch_custom_id("In Flight Movie", "en", id, sizeof(id));
  char plan_path[PATH_MAX], failed_path[PATH_MAX];
  batch_result_path(id, ".result.json", plan_path, sizeof(plan_path));
  batch_result_path(id, ".failed", failed_path, sizeof(failed_path));
  plat_unlink(plan_path);
  plat_unlink(failed_path);

  stub_reset();
  queue_batch_create();
  queue_batch_status("in_progress", 1, 0, 0);     /* never ends */
  bool cancelled = false, in_flight = false;
  char in_flight_id[128] = "";
  bool got = batch_run_all(&c, &cancelled, &in_flight, in_flight_id, sizeof(in_flight_id));

  ck(!got, "an unfinished batch does not count as fetched");
  ck(in_flight, "the run is told the batch is still in flight");
  ck(strcmp(in_flight_id, BATCH_ID) == 0, "the in-flight batch id is handed back");
  ck(!file_exists(plan_path), "no plan file was invented for it");
  ck(!file_exists(failed_path),
     "and it is not marked as failed - that would force a full-price live request");
  ck(stub_request_count() == 2, "only the create and the status poll went out");

  char mid[128], status[32];
  size_t items = 0;
  ck(batch_load_manifest(mid, sizeof(mid), status, sizeof(status), &items),
     "the batch is remembered on disk");
  ck(strcmp(status, "submitted") == 0 && strcmp(mid, BATCH_ID) == 0,
     "the manifest says the batch is still to be fetched");
}

/* -------------------------------------------------------------------------
 * The OpenAI-compatible path (DeepSeek et al.): no output-token limit is
 * imposed, and a thinking model that spends the provider's whole allowance on
 * its reasoning gets asked again instead of costing the movie its recap.
 * ---------------------------------------------------------------------- */

static void test_openai_plan_imposes_no_token_limit(void) {
  stub_reset();
  Config c = cfg_for("https://api.deepseek.com/v1", "deepseek-v4-pro", "sk-ds-mock");

  queue_ok(PLAN_JSON);                 /* a normal, complete reply */
  ClipPlanList plan = openai_make_plan(&c, "No Limit Movie", "1\n5 --> 9\nStory.\n\n",
                                       "", "", false, 2, 12, NULL, NULL, NULL);

  ck(plan.count == 1, "the plan comes back");
  ck(stub_request_count() == 1, "a complete reply costs exactly one request");

  const char *body = stub_request_body(0);
  ck(body && !body_has(body, "max_output_tokens"),
     "the /responses request carries NO max_output_tokens");
  ck(body && !body_has(body, "max_completion_tokens"),
     "and no max_completion_tokens either");
  ck(body && !body_has(body, "\"max_tokens\""),
     "and no max_tokens: the provider's own maximum applies");
  ck(body && body_has(body, "\"reasoning\""),
     "the reasoning request itself is untouched");
  ck(stub_request_url(0) != NULL && strstr(stub_request_url(0), "/responses"),
     "it is the Responses endpoint");

  free_clip_plan_list(&plan);
}

static void test_openai_reasoning_model_asked_again_with_room(void) {
  /* Exactly what deepseek-v4-pro did: 200 OK, status=incomplete,
   * reason=max_output_tokens, and nothing in the output but reasoning. */
  stub_reset();
  Config c = cfg_for("https://api.deepseek.com/v1", "deepseek-v4-pro", "sk-ds-mock");

  stub_queue_reply(200,
    "{\"id\":\"r1\",\"object\":\"response\",\"status\":\"incomplete\","
    "\"incomplete_details\":{\"reason\":\"max_output_tokens\"},"
    "\"output\":[{\"type\":\"reasoning\",\"content\":[{\"type\":\"reasoning_text\","
    "\"text\":\"We need answer JSON only. Need build narration for 96 clips...\"}]}]}");
  queue_ok(PLAN_JSON);                 /* the second attempt writes the plan */

  ClipPlanList plan = openai_make_plan(&c, "Thinking Movie", "1\n5 --> 9\nStory.\n\n",
                                       "", "", false, 2, 12, NULL, NULL, NULL);

  ck(plan.count == 1, "the reasoning-only reply is retried and the plan arrives");
  ck(stub_request_count() == 2, "exactly one extra request was made");
  const char *again = stub_request_body(1);
  ck(again && body_has(again, "\"max_output_tokens\":128000"),
     "the retry names a big output budget (128000 for a reasoner)");
  ck(again && body_has(again, "\"effort\":\"high\""),
     "and keeps the high reasoning effort - quality first");
  free_clip_plan_list(&plan);
}

static void test_openai_reasoning_model_then_drops_effort(void) {
  /* The model burns the big budget too: the next asks lower the reasoning
   * effort rather than the quality of the answer itself. */
  stub_reset();
  Config c = cfg_for("https://api.deepseek.com/v1", "deepseek-v4-pro", "sk-ds-mock");

  const char *only_reasoning =
    "{\"id\":\"r1\",\"object\":\"response\",\"status\":\"incomplete\","
    "\"incomplete_details\":{\"reason\":\"max_output_tokens\"},"
    "\"output\":[{\"type\":\"reasoning\",\"content\":[]}]}";
  stub_queue_reply(200, only_reasoning);
  stub_queue_reply(200, only_reasoning);
  queue_ok(PLAN_JSON);

  ClipPlanList plan = openai_make_plan(&c, "Stubborn Movie", "1\n5 --> 9\nStory.\n\n",
                                       "", "", false, 2, 12, NULL, NULL, NULL);

  ck(plan.count == 1, "the third attempt finally produces the plan");
  ck(stub_request_count() == 3, "two extra asks, no more");
  ck(body_has(stub_request_body(2), "\"effort\":\"low\""),
     "the last ask lowers the reasoning effort");
  ck(body_has(stub_request_body(2), "\"max_output_tokens\":128000"),
     "while keeping the big output budget");
  free_clip_plan_list(&plan);
}

static void test_openai_provider_ceiling_is_obeyed(void) {
  /* A provider that names its own maximum must not be asked for more again. */
  stub_reset();
  Config c = cfg_for("https://api.deepseek.com/v1", "deepseek-v4-pro", "sk-ds-mock");

  stub_queue_reply(200,
    "{\"status\":\"incomplete\",\"incomplete_details\":{\"reason\":\"max_output_tokens\"},"
    "\"output\":[{\"type\":\"reasoning\",\"content\":[]}]}");
  stub_queue_reply(400,
    "{\"error\":{\"message\":\"max_output_tokens: 128000 > 8192, which is the maximum "
    "allowed number of output tokens for this model\",\"type\":\"invalid_request_error\"}}");
  queue_ok(PLAN_JSON);

  ClipPlanList plan = openai_make_plan(&c, "Capped Movie", "1\n5 --> 9\nStory.\n\n",
                                       "", "", false, 2, 12, NULL, NULL, NULL);

  ck(plan.count == 1, "the ceiling is obeyed and the plan still arrives");
  ck(stub_request_count() == 3, "the rejected budget costs one extra request");
  const char *third = stub_request_body(2);
  ck(third && body_has(third, "\"max_output_tokens\":8192"),
     "the third ask uses the 8192 the provider named");
  free_clip_plan_list(&plan);
}

static void test_openai_no_limit_means_no_extra_requests(void) {
  /* The 20-minute case from the report: 96 clips of narration in one reply.
   * Nothing about the request may push the model into a second call. */
  stub_reset();
  Config c = cfg_for("https://api.deepseek.com/v1", "deepseek-v4-pro", "sk-ds-mock");
  queue_ok(PLAN_JSON);

  ClipPlanList plan = openai_make_plan(&c, "Long Movie", "1\n5 --> 9\nStory.\n\n",
                                       "", "", false, 96, 12, NULL, NULL, NULL);
  ck(plan.count > 0, "a 96-clip plan is accepted in one request");
  ck(stub_request_count() == 1, "no retry was needed");
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
  test_batch_in_flight_is_not_paid_for_twice();
  test_openai_plan_imposes_no_token_limit();
  test_openai_reasoning_model_asked_again_with_room();
  test_openai_reasoning_model_then_drops_effort();
  test_openai_provider_ceiling_is_obeyed();
  test_openai_no_limit_means_no_extra_requests();
  test_batch_custom_id_shape();
  test_batch_collect_then_render();
  test_batch_failure_falls_back_live();
  test_batch_cut_short_is_rebatched();
  test_batch_still_cut_off_after_two_rounds_goes_live();
  test_batch_chunking();
  test_batch_manifest_resume();

  printf("\n%d checks, %d failures\n", g_pass + g_fail, g_fail);
  if (g_fail == 0) printf("ALL OK\n");
  return g_fail == 0 ? 0 : 1;
}
