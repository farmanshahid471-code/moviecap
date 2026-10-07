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


/* ------------------------------------------- plan count / length enforcement */

static void test_oversized_plan_is_merged_to_the_target(void) {
  ClipPlanList lst;
  lst.count = 10;
  lst.items = (ClipPlan *)calloc(10, sizeof(ClipPlan));
  for (int i = 0; i < 10; i++) {
    lst.items[i].start = 100 + i * 12;
    lst.items[i].end   = 100 + i * 12 + 10;
    char buf[32];
    snprintf(buf, sizeof(buf), "Event %02d.", i + 1);
    lst.items[i].narration = str_dup(buf);
  }
  cap_clip_plan_to_max(&lst, 4, "en");
  ck(lst.count == 4, "a 10-clip plan asked for 4 clips comes back with 4");
  ck(lst.items[0].start == 100, "the merged plan still starts at the first clip");
  ck(lst.items[3].end == 100 + 9 * 12 + 10,
     "the merged plan still ends at the last clip");
  bool all_kept = true;
  char joined[512];
  joined[0] = '\0';
  for (size_t i = 0; i < lst.count; i++) {
    if (strlen(joined) + strlen(lst.items[i].narration) + 2 < sizeof(joined))
      strcat(joined, lst.items[i].narration);
    strcat(joined, " ");
  }
  for (int i = 1; i <= 10; i++) {
    char tag[16];
    snprintf(tag, sizeof(tag), "Event %02d", i);
    if (!strstr(joined, tag)) all_kept = false;
  }
  ck(all_kept, "merging keeps every narration - no event of the story is dropped");
  free_clip_plan_list(&lst);
}

static void test_plan_within_the_target_is_untouched(void) {
  ClipPlanList lst;
  lst.count = 3;
  lst.items = (ClipPlan *)calloc(3, sizeof(ClipPlan));
  for (int i = 0; i < 3; i++) {
    lst.items[i].start = 50 + i * 12;
    lst.items[i].end   = 50 + i * 12 + 10;
    lst.items[i].narration = str_dup("A scene.");
  }
  cap_clip_plan_to_max(&lst, 111, "en");
  ck(lst.count == 3, "a plan below the target is not padded or changed");
  ck_str(lst.items[1].narration, "A scene.", "the narrations are exactly as written");
  free_clip_plan_list(&lst);
}

static void queue_ok_big(const char *text) {
  size_t need = strlen(text) + 512;
  char *buf = (char *)malloc(need);
  if (!buf) exit(2);
  snprintf(buf, need,
           "{\"id\":\"msg_1\",\"type\":\"message\",\"role\":\"assistant\","
           "\"model\":\"claude-sonnet-4-5\",\"stop_reason\":\"end_turn\","
           "\"content\":[{\"type\":\"text\",\"text\":\"%s\"}],"
           "\"usage\":{\"input_tokens\":10,\"output_tokens\":20}}", text);
  stub_queue_reply(200, buf);
  free(buf);
}

static void test_openai_plan_249_clips_for_111_target(void) {
  /* Reproduces the user's run 3: target 111 clips (config allows 80-120),
     model replies with 249.  The accepted plan must honour the target. */
  size_t need = 249 * 140 + 512;
  char *json = (char *)malloc(need);
  if (!json) exit(2);
  size_t at = 0;
  at += (size_t)snprintf(json + at, need - at, "{\\\"clips\\\":[");
  for (int i = 0; i < 249; i++) {
    at += (size_t)snprintf(json + at, need - at,
                           "%s{\\\"start\\\":%d,\\\"end\\\":%d,\\\"narration\\\":"
                           "\\\"Part %03d of the tale continues here with what happens "
                           "next in the movie.\\\"}",
                           i ? "," : "", 12 + i * 12, 12 + i * 12 + 10, i + 1);
  }
  snprintf(json + at, need - at, "]}");

  stub_reset();
  stub_set_default_reply(500, "{\"type\":\"error\",\"error\":{\"message\":\"unexpected\"}}");
  queue_ok_big(json);
  free(json);

  g_batch_collect = false;
  g_batch_render = false;
  g_batch_bypass_lookup = false;

  Config c = cfg_for("https://api.anthropic.com/v1", "claude-sonnet-4-5",
                     "sk-ant-api03-testkey");
  snprintf(c.recap_language, sizeof(c.recap_language), "English");
  c.recap_minutes = 20.0;

  ClipPlanList plan = openai_make_plan(&c, "Toy Story 5 (2026)",
                                       "1\n12 --> 20\nThe story starts here.\n\n",
                                       "", "", false, 111, 10, NULL, NULL, NULL);
  ck(plan.count == 111, "249 clips for a 111-clip target are merged down to 111");
  ck(plan.count <= 120, "the accepted plan stays inside max_clips");
  ck(plan.items[0].start == 12, "the merged plan starts where the movie starts");
  ck(plan.items[plan.count - 1].end == 12 + 248 * 12 + 10,
     "the merged plan still reaches the end of the story");
  ck(plan.items[0].narration != NULL &&
     strstr(plan.items[0].narration, "Part 001") != NULL,
     "the first clip of the story is still narrated");
  ck(plan.items[plan.count - 1].narration != NULL &&
     strstr(plan.items[plan.count - 1].narration, "Part 249") != NULL,
     "the last clip of the story is still narrated");
  bool contiguous = true;
  for (size_t i = 1; i < plan.count; i++)
    if (plan.items[i].start < plan.items[i - 1].start) contiguous = false;
  ck(contiguous, "the merged plan is in playing order");
  free_clip_plan_list(&plan);
}


/* ------------------------------ recap length guarantee and script quality */

static void test_over_long_narration_is_trimmed_to_the_target(void) {
  /* 4 clips, 15 seconds of speech each (60 s) for a 30 s target: the trim must
     cut whole sentences until the speech fits, keeping every clip usable. */
  ClipPlanList lst;
  lst.count = 4;
  lst.items = (ClipPlan *)calloc(4, sizeof(ClipPlan));
  const char *sent = "He opens the door and steps inside the dark room. "
                     "Then he hears a noise behind him and turns around.";
  for (int i = 0; i < 4; i++) {
    lst.items[i].start = 100 + i * 40;
    lst.items[i].end   = 100 + i * 40 + 30;
    char buf[512];
    int at = 0;
    for (int k = 0; k < 6; k++) at += snprintf(buf + at, sizeof(buf) - at, "%s", sent);
    lst.items[i].narration = str_dup(buf);
  }
  cap_plan_narration_length(&lst, 60.0, 0.0, "en", "English");
  double speech = plan_speech_seconds(lst.items, lst.count, "en");
  ck(speech <= 60.0 * 1.15, "a 277 s narration set is trimmed to a 60 s target");
  ck(speech >= 60.0 * 0.50, "the trim keeps each clip narrated, not emptied");
  ck(speech >= 60.0 * 0.85,
     "the refill pass hands the sentence slack back, so the target is nearly met");
  bool whole = true;
  for (size_t i = 0; i < lst.count; i++) {
    size_t n = strlen(lst.items[i].narration);
    if (n == 0 || lst.items[i].narration[n - 1] != '.') whole = false;
    if (count_speech_units(lst.items[i].narration, "en") < 2.0) whole = false;
  }
  ck(whole, "every trimmed narration ends on a whole sentence and keeps words");
  free_clip_plan_list(&lst);
}

static void test_narration_at_the_target_is_not_touched(void) {
  ClipPlanList lst;
  lst.count = 2;
  lst.items = (ClipPlan *)calloc(2, sizeof(ClipPlan));
  for (int i = 0; i < 2; i++) {
    lst.items[i].start = 10 + i * 20;
    lst.items[i].end   = 10 + i * 20 + 15;
    lst.items[i].narration = str_dup("He runs for the door and makes it out.");
  }
  char *before = str_dup(lst.items[0].narration);
  cap_plan_narration_length(&lst, 300.0, 0.0, "en", "English");
  ck_str(lst.items[0].narration, before,
         "a narration already under the target is never trimmed");
  free(before);
  free_clip_plan_list(&lst);
}

static void test_trim_never_cuts_the_closing_line(void) {
  ClipPlanList lst;
  lst.count = 2;
  lst.items = (ClipPlan *)calloc(2, sizeof(ClipPlan));
  char tail[600];
  recap_closing_line_for("English", tail, sizeof(tail));
  ck(tail[0] != 0, "the English closing line is known");
  lst.items[0].start = 5;  lst.items[0].end = 35;
  lst.items[1].start = 40; lst.items[1].end = 70;
  const char *sent = "He opens the door and steps inside the dark room. "
                     "Then he hears a noise behind him and turns around.";
  char long_body[2048];
  int at = 0;
  for (int k = 0; k < 4; k++) at += snprintf(long_body + at, sizeof(long_body) - at, "%s", sent);
  lst.items[0].narration = str_dup(long_body);
  char last[2600];
  snprintf(last, sizeof(last), "%s %s", long_body, tail);
  lst.items[1].narration = str_dup(last);
  cap_plan_narration_length(&lst, 60.0, 0.0, "en", "English");
  double speech = plan_speech_seconds(lst.items, lst.count, "en");
  ck(speech <= 60.0 * 1.15, "the trimmed plan fits the target");
  ck(speech >= 60.0 * 0.85,
     "the trimmed plan fills the target as far as whole sentences allow");
  size_t n = strlen(lst.items[1].narration), tl = strlen(tail);
  ck(n >= tl && memcmp(lst.items[1].narration + n - tl, tail, tl) == 0,
     "the closing line of the last clip survives the trim");
  free_clip_plan_list(&lst);
}

static void test_trim_handles_cjk_punctuation(void) {
  ClipPlanList lst;
  lst.count = 2;
  lst.items = (ClipPlan *)calloc(2, sizeof(ClipPlan));
  for (int i = 0; i < 2; i++) {
    lst.items[i].start = 10 + i * 20;
    lst.items[i].end   = 10 + i * 20 + 15;
    lst.items[i].narration = str_dup(
        "\u4ed6\u4eec\u8d70\u8fdb\u623f\u95f4\u3002\u5c4b\u5b50\u91cc\u5f88\u9ed1\u3002"
        "\u4ed6\u542c\u5230\u4e00\u4e2a\u58f0\u97f3\u3002\u4ed6\u8f6c\u8eab\u770b\u89c1"
        "\u4e00\u4e2a\u4eba\u3002\u90a3\u4e2a\u4eba\u6ca1\u6709\u52a8\u3002");
  }
  cap_plan_narration_length(&lst, 3.0, 0.0, "zh", "Chinese");
  double speech = plan_speech_seconds(lst.items, lst.count, "zh");
  ck(speech <= 3.0 * 1.25, "CJK narrations are trimmed on their own punctuation");
  for (size_t i = 0; i < lst.count; i++) {
    size_t n = strlen(lst.items[i].narration);
    bool ends = false;
    if (n >= 3 && (unsigned char)lst.items[i].narration[n - 3] == 0xE3 &&
        (unsigned char)lst.items[i].narration[n - 2] == 0x80 &&
        (unsigned char)lst.items[i].narration[n - 1] == 0x82)
      ends = true;      /* the 3-byte 。 terminator */
    ck(ends, i == 0 ? "the first CJK narration ends on a full stop"
                    : "the last CJK narration ends on a full stop");
  }
  free_clip_plan_list(&lst);
}

static void test_invented_character_names_are_flagged(void) {
  ClipPlanList lst;
  lst.count = 2;
  lst.items = (ClipPlan *)calloc(2, sizeof(ClipPlan));
  lst.items[0].start = 1;  lst.items[0].end = 20;
  lst.items[1].start = 22; lst.items[1].end = 40;
  lst.items[0].narration = str_dup(
      "The story begins in a small town. Woody runs across the yard and shouts.");
  lst.items[1].narration = str_dup(
      "Buzz turns to him and nods. They climb the fence together.");
  const char *plot =
      "Woody is a cowboy doll. When a new toy named Buzz arrives, Woody gets "
      "jealous and the two of them get lost together.";
  char suspects[200];
  int n = plan_name_suspects(&lst, plot, suspects, sizeof(suspects));
  ck(n == 0, "names that the plot summary uses are not flagged");
  free(lst.items[1].narration);
  lst.items[1].narration = str_dup(
      "Then Rexxy turns to him and nods. They climb the fence together.");
  n = plan_name_suspects(&lst, plot, suspects, sizeof(suspects));
  ck(n == 1, "a name the plot summary never mentions is flagged");
  ck(strstr(suspects, "Rexxy") != NULL, "the flagged name is reported by name");
  ck(plan_name_suspects(&lst, "", suspects, sizeof(suspects)) == 0,
     "without a plot summary nothing is guessed about names");
  free_clip_plan_list(&lst);
}

static void test_subtitle_copying_is_flagged(void) {
  ClipPlanList lst;
  lst.count = 2;
  lst.items = (ClipPlan *)calloc(2, sizeof(ClipPlan));
  lst.items[0].start = 1;  lst.items[0].end = 20;
  lst.items[1].start = 22; lst.items[1].end = 40;
  const char *line = "you do not understand what i am doing here tonight";
  lst.items[0].narration = str_dup(
      "You do not understand what I am doing here tonight, so listen to me now.");
  lst.items[1].narration = str_dup(
      "He tells her the bridge is gone and she refuses to turn back.");
  const char *subs = "1 --> 12\nYou do not understand what I am doing here tonight\n\n";
  int copied = plan_copied_subtitle_clips(&lst, subs);
  ck(copied == 1, "one narration repeating a subtitle line word for word is counted");
  ck(plan_copied_subtitle_clips(&lst, "1 --> 12\nSomething completely different here\n\n") == 0,
     "an original narration is not counted as copied");
  free_clip_plan_list(&lst);
  (void)line;
}

static void test_transition_command_shape(void) {
  ck(XFADE_CHUNK >= 2 && XFADE_CHUNK <= 16,
     "the crossfade chunks stay well inside the command-line limit");
  char out[64];
  recap_closing_line_for("Chinese", out, sizeof(out));
  ck(out[0] != 0, "the Chinese closing line is known");
  recap_closing_line_for("Russian", out, sizeof(out));
  ck(out[0] == 0, "a language with no fixed closing line asks for a natural one");
  char t2[600];
  recap_closing_line_for("English", t2, sizeof(t2));
  ck(strstr(t2, "With that the story ends right here") != NULL,
     "the English closing line is the exact one the prompt names");
}


static void test_audit_trims_a_stubborn_over_long_plan(void) {
  /* The model keeps writing 3x the narration the 1 minute target allows, even
     after the correction note.  The audit must try once and then hold the
     length itself, so the render cannot come out longer than requested. */
  const char *sent = "He opens the door and steps inside the dark room. "
                     "Then he hears a noise behind him and turns around.";
  char big[4096];
  int at = 0;
  for (int k = 0; k < 8; k++) at += snprintf(big + at, sizeof(big) - at, "%s", sent);
  /* the plan as real JSON (parsed directly) and escaped for the reply body */
  char plan_json[8192];
  at = 0;
  at += snprintf(plan_json + at, sizeof(plan_json) - at, "{\"clips\":[");
  for (int i = 0; i < 3; i++)
    at += snprintf(plan_json + at, sizeof(plan_json) - at,
                   "%s{\"start\":%d,\"end\":%d,\"narration\":\"%s\"}",
                   i ? "," : "", 5 + i * 30, 5 + i * 30 + 25, big);
  snprintf(plan_json + at, sizeof(plan_json) - at, "]}");

  char esc[16384];
  {
    size_t w = 0;
    for (size_t r = 0; plan_json[r] && w + 3 < sizeof(esc); r++) {
      if (plan_json[r] == '"') esc[w++] = '\\';
      esc[w++] = plan_json[r];
    }
    esc[w] = 0;
  }

  stub_reset();
  stub_set_default_reply(500, "{\"type\":\"error\",\"error\":{\"message\":\"unexpected\"}}");
  queue_ok_big(esc);
  queue_ok_big(esc);
  g_batch_collect = false;
  g_batch_render = false;
  g_batch_bypass_lookup = false;

  Config c = cfg_for("https://api.anthropic.com/v1", "claude-sonnet-4-5",
                     "sk-ant-api03-testkey");
  snprintf(c.recap_language, sizeof(c.recap_language), "English");
  c.recap_minutes = 1.0;
  c.transitions = false;
  c.transition_seconds = 0;

  ClipPlanList plan = parse_clip_plan_json(plan_json);
  ck(plan.count == 3, "the over-long plan parses");
  double before = plan_speech_seconds(plan.items, plan.count, "en");
  ck(before > 90.0, "the plan really is over three times the 60 s target");

  audit_plan_length_and_script(&c, "Long Movie (2026)",
                               "1\n5 --> 9\nHe opens the door and steps inside.\n\n",
                               "", "", false, 3, 20, "en", &plan);
  ck(stub_request_count() == 1,
     "the model was asked exactly once more, with the length rule");
  ck(plan.count == 3, "the corrected plan keeps the configured clip count");
  double after = plan_speech_seconds(plan.items, plan.count, "en");
  ck(after <= 60.0 * 1.15, "the accepted plan now fits the 1 minute target");
  ck(after < before, "the narration really got shorter");
  free_clip_plan_list(&plan);
  g_batch_bypass_lookup = false;   /* the next tests rely on the batch lookup */
}


static void test_voice_pace_trim_keeps_whole_sentences(void) {
  /* The voice speaks faster than the estimate, so after the first clip the app
     re-trims the REST of the plan with the measured pace.  That trim must obey
     the same rules: whole sentences, closing line kept. */
  char tail[600];
  recap_closing_line_for("English", tail, sizeof(tail));
  const char *sent = "He opens the door and steps inside the dark room. "
                     "Then he hears a noise behind him and turns around.";
  char body[2048];
  int at = 0;
  for (int k = 0; k < 6; k++) at += snprintf(body + at, sizeof(body) - at, "%s", sent);
  char with_tail[2600];
  snprintf(with_tail, sizeof(with_tail), "%s %s", body, tail);

  bool trimmed = false;
  char *out = trim_narration_to_units(body, 30.0, "en", NULL, &trimmed);
  ck(trimmed, "an over-budget narration is trimmed");
  ck(count_speech_units(out, "en") <= 30.0 * 1.05, "the trim obeys the measured-pace budget");
  size_t n = strlen(out);
  ck(n > 0 && out[n - 1] == '.', "the trimmed narration ends on a whole sentence");
  free(out);

  out = trim_narration_to_units(with_tail, 40.0, "en", tail, &trimmed);
  ck(trimmed, "the narration with the closing line is trimmed too");
  n = strlen(out);
  size_t tl = strlen(tail);
  ck(n >= tl && memcmp(out + n - tl, tail, tl) == 0,
     "the closing line survives the measured-pace trim");
  ck(count_speech_units(out, "en") >= 30.0,
     "the closing line alone keeps the narration above the tiny budget");
  free(out);

  out = trim_narration_to_units("Short line here.", 40.0, "en", NULL, &trimmed);
  ck(!trimmed && strcmp(out, "Short line here.") == 0,
     "a narration inside the budget is returned untouched");
  free(out);
}

static void test_pace_scale_speaks_the_requested_minutes(void) {
  /* The plan budgets words for 2.6 words per second.  A voice that really
     speaks 3.9 words per second turns a 20 minute recap into 13 minutes, so the
     remaining clips must be narrated more slowly - and a slow voice the other
     way round.  The scale stays inside a band that still sounds human. */
  double fast = pace_scale_for(2.6, 3.9, 110);
  ck(fast < 1.0, "a fast voice is slowed down");
  ck(fast >= 0.75, "but never slowed past the natural band");
  ck(fabs(fast * 3.9 - 2.6) < 0.9, "the slowed voice lands near the planned pace");

  double slow = pace_scale_for(2.6, 1.6, 110);
  ck(slow > 1.0, "a slow voice is sped up");
  ck(slow <= 1.33, "but never sped past the natural band");

  ck(pace_scale_for(2.6, 2.7, 110) == 1.0, "a voice at the assumed pace is left alone");
  ck(pace_scale_for(2.6, 2.75, 110) == 1.0, "a few percent off is left alone");
  ck(pace_scale_for(2.6, 2.9, 110) < 1.0, "ten percent off is corrected");
  ck(pace_scale_for(0.0, 3.0, 110) == 1.0, "no estimate means no change");
  ck(pace_scale_for(2.6, 0.0, 110) == 1.0, "no measurement means no change");
  ck(pace_scale_for(2.6, 3.9, 0) == 1.0, "no configured rate means no change");
  ck(pace_scale_for(4.0, 1.0, 100) <= 1.33, "the fastest correction is still bounded");
  ck(pace_scale_for(1.0, 4.0, 100) >= 0.75, "the slowest correction is still bounded");
}

static void test_recap_band_is_a_range(void) {
  /* "Give me a 20 minute recap" now means 10-20 minutes: the top is the hard cap,
     the bottom is what still counts as the video that was asked for, and 0 for
     the bottom means half of the top. */
  Config cfg;
  memset(&cfg, 0, sizeof(cfg));
  double lo = 0, hi = 0;
  cfg.recap_minutes = 20.0;
  cfg.min_recap_minutes = 0.0;
  recap_band_minutes(&cfg, &lo, &hi);
  ck(hi == 20.0 && lo == 10.0, "20 minutes asked -> 10-20 min band");
  cfg.min_recap_minutes = 12.0;
  recap_band_minutes(&cfg, &lo, &hi);
  ck(lo == 12.0 && hi == 20.0, "an explicit bottom is used as it is");
  cfg.min_recap_minutes = 45.0;                 /* above the top */
  recap_band_minutes(&cfg, &lo, &hi);
  ck(lo == 20.0 && hi == 20.0, "a bottom above the top is clamped to the top");
  cfg.recap_minutes = 0.0;                      /* auto */
  cfg.min_recap_minutes = 0.0;
  recap_band_minutes(&cfg, &lo, &hi);
  ck(lo == 0.0 && hi == 0.0, "no length requested -> no band");
  cfg.recap_minutes = 1.0;
  recap_band_minutes(&cfg, &lo, &hi);
  ck(lo == 0.5 && hi == 1.0, "the CI's 1 minute recap bands at 0.5-1");
}

static void test_transition_styles_and_join_length(void) {
  /* The fade names the app accepts, and what a fade costs in length: one fade
     per junction, because every clip is grown by a held frame instead of the
     narrations being overlapped. */
  ck(transition_style_known("fade"), "fade is a known style");
  ck(transition_style_known("fadeblack"), "fadeblack is a known style");
  ck(transition_style_known("slideleft"), "slideleft is a known style");
  ck(transition_style_known("dissolve"), "dissolve is a known style");
  ck(transition_style_known("pixelize"), "pixelize is a known style");
  ck(transition_style_known("none") && transition_style_known("cut"),
     "the original repo's hard cut is a style of its own");
  ck(!transition_style_known(NULL), "no style is not a style");
  ck(!transition_style_known(""), "an empty style is not a style");
  ck(!transition_style_known("fade;rm -rf /"), "a filter name from outside is refused");
  ck(!transition_style_known("Fade"), "the filter names are case sensitive");

  ck(transition_join_seconds(true, 20, 0.35) > 6.6 &&
     transition_join_seconds(true, 20, 0.35) < 6.7,
     "20 clips fade into 19 junctions of 0.35 s");
  ck(transition_join_seconds(false, 20, 0.35) == 0.0, "hard cuts add nothing");
  ck(transition_join_seconds(true, 1, 0.35) == 0.0, "one clip has no junction");
  ck(transition_join_seconds(true, 5, 0.0) == 0.0, "no fade length, no cost");
}

static void test_plan_request_asks_for_the_band_and_audits_it(void) {
  /* The band has to reach the model in the request itself, and the audit has to
     hold the plan to it: over the top it trims, under the floor it says so. */
  stub_reset();
  stub_set_default_reply(500, "{\"type\":\"error\",\"error\":{\"message\":\"unexpected\"}}");
  queue_ok_big("{\\\"clips\\\":[{\\\"start\\\":12,\\\"end\\\":42,\\\"narration\\\":\\\"The story "
               "begins in a small town where nothing ever happens to anyone at all.\\\"}]}");

  g_batch_collect = false;
  g_batch_render = false;
  g_batch_bypass_lookup = false;

  Config c = cfg_for("https://api.anthropic.com/v1", "claude-sonnet-4-5",
                     "sk-ant-api03-testkey");
  snprintf(c.recap_language, sizeof(c.recap_language), "English");
  c.recap_minutes = 20.0;
  c.min_recap_minutes = 10.0;

  ClipPlanList plan = openai_make_plan(&c, "Toy Story 5 (2026)",
                                       "1\n12 --> 20\nThe story starts here.\n\n",
                                       "", "", false, 25, 48, NULL, NULL, NULL);
  ck(stub_request_count() == 1, "one plan request");
  const char *body = stub_request_body(0);
  ck(strstr(body, "add up to 10.0 to 20.0 minutes of speech") != NULL,
     "the request carries the top AND the bottom of the band");
  ck(strstr(body, "never go over it") != NULL,
     "the request says the top is a hard cap");
  ck(strstr(body, "Target clip length: 38-57 seconds each") != NULL,
     "the per-clip window still comes from the clip length");
  ck(strstr(body, "array must hold EXACTLY 25 clip objects") != NULL,
     "the clip count is still a hard limit");
  free_clip_plan_list(&plan);

  /* Over the top: the deterministic trim must land the speech inside the band. */
  Config cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.recap_minutes = 20.0;
  cfg.min_recap_minutes = 10.0;
  cfg.transition_seconds = 0.35;
  cfg.transitions = true;
  snprintf(cfg.recap_language, sizeof(cfg.recap_language), "English");
  ClipPlanList over;
  over.count = 20;
  over.items = (ClipPlan *)calloc(20, sizeof(ClipPlan));
  const char *sent = "He opens the door and steps inside the dark room. "
                     "Then he hears a noise behind him and turns around quickly.";
  for (int i = 0; i < 20; i++) {
    over.items[i].start = 60 + i * 60;
    over.items[i].end   = 60 + i * 60 + 60;
    char buf[2048];
    int at = 0;
    for (int k = 0; k < 4; k++) at += snprintf(buf + at, sizeof(buf) - at, "%s", sent);
    over.items[i].narration = str_dup(buf);
  }
  double join = transition_join_seconds(cfg.transitions, over.count, cfg.transition_seconds);
  ck(join > 6.6 && join < 6.7, "20 clips carry 19 fades of 0.35 s");
  g_batch_collect = true;          /* no re-ask: only the deterministic trim */
  audit_plan_length_and_script(&cfg, "Toy Story 5 (2026)", NULL, NULL, NULL, false, 20, 60,
                               "en", &over);
  g_batch_collect = false;
  double speech = plan_speech_seconds(over.items, over.count, "en");
  double allowed = (cfg.recap_minutes * 60.0 - join) * 1.02;
  ck(speech <= allowed, "a 24 minute plan is trimmed to fit the 20 minute top");
  ck(speech > (cfg.min_recap_minutes * 60.0 - join) * 0.8,
     "the trimmed plan is still above the 10 minute floor");
  ck(over.count == 20, "the trim never drops a clip");
  bool all_narrated = true;
  for (size_t i = 0; i < over.count; i++)
    if (!over.items[i].narration || over.items[i].narration[0] == '\0') all_narrated = false;
  ck(all_narrated, "every clip still has a narration after the trim");
  free_clip_plan_list(&over);

  /* Under the floor: the audit keeps the plan but must not stay silent about it. */
  ClipPlanList under;
  under.count = 3;
  under.items = (ClipPlan *)calloc(3, sizeof(ClipPlan));
  for (int i = 0; i < 3; i++) {
    under.items[i].start = 60 + i * 30;
    under.items[i].end   = 60 + i * 30 + 30;
    under.items[i].narration = str_dup("A short line happens here and that is all.");
  }
  g_batch_collect = true;
  audit_plan_length_and_script(&cfg, "Toy Story 5 (2026)", NULL, NULL, NULL, false, 3, 30,
                               "en", &under);
  g_batch_collect = false;
  ck(under.count == 3, "a plan under the floor is left alone");
  ck(plan_speech_seconds(under.items, under.count, "en") < 20.0 * 60.0,
     "the short plan really is short");
  free_clip_plan_list(&under);
}

static void test_minutes_print_without_lying(void) {
  char b[24];
  fmt_minutes(20.0, b, sizeof(b));  ck_str(b, "20", "whole minutes print whole");
  fmt_minutes(10.0, b, sizeof(b));  ck_str(b, "10", "ten prints as ten");
  fmt_minutes(0.5, b, sizeof(b));   ck_str(b, "0.5", "a half minute is not zero");
  fmt_minutes(0.0, b, sizeof(b));   ck_str(b, "0", "zero prints as zero");
  fmt_minutes(12.5, b, sizeof(b));  ck_str(b, "12.5", "half minutes keep their half");
  fmt_minutes(7.04, b, sizeof(b));  ck_str(b, "7", "a rounding hair is not a decimal");
  fmt_minutes(7.6, b, sizeof(b));   ck_str(b, "7.6", "a real fraction is shown");
}

static void test_upload_landing_folder_by_file_type(void) {
  /* The panel files every upload by what the file IS: a movie picked while the
     Subtitles tab is open must still land in movies/ (a real run reported "No
     .mp4 files found in movies/" after the movie went to scripts/srt_files). */
  ck(media_kind_for_name("Citizen Kane.mp4") == MEDIA_MOVIE, "mp4 is a movie");
  ck(media_kind_for_name("Dune.Part.Two.MKV") == MEDIA_MOVIE, "extensions ignore case");
  ck(media_kind_for_name("movie.mov") == MEDIA_MOVIE, "mov is a movie");
  ck(media_kind_for_name("a.webm") == MEDIA_MOVIE, "webm is a movie");
  ck(media_kind_for_name("Citizen Kane.srt") == MEDIA_SUBTITLE, "srt is a subtitle file");
  ck(media_kind_for_name("subs.vtt") == MEDIA_SUBTITLE, "vtt is a subtitle file");
  ck(media_kind_for_name("notes.txt") == MEDIA_SUBTITLE, "txt goes with the scripts");
  ck(media_kind_for_name("theme.mp3") == MEDIA_MUSIC, "mp3 is music");
  ck(media_kind_for_name("theme.m4a") == MEDIA_MUSIC, "m4a is music");
  ck(media_kind_for_name("config.json") == MEDIA_OTHER, "unknown types stay in the tab");
  ck(media_kind_for_name("noextension") == MEDIA_OTHER, "a bare name stays in the tab");
  ck(media_kind_for_name(".mp4") == MEDIA_OTHER, "a dot file is not a movie");
  ck(media_kind_for_name(NULL) == MEDIA_OTHER, "no name is not a movie");
  ck(media_kind_for_name("archive.mp4.zip") == MEDIA_OTHER, "the LAST extension wins");
}

static void test_stray_movie_files_are_adopted(void) {
  /* A video that sits in the subtitle folder (an older upload, or a drag and
     drop) must not end the run with "no movies found": it is moved into
     movies/ and the run continues with it.  Runs on a scratch tree. */
  const char *root = "testadopt";
  char cwd[PATH_MAX];
  if (!plat_getcwd(cwd, sizeof(cwd))) { ck(false, "cwd"); return; }
  ensure_dir(root);
  if (!plat_chdir(root)) { ck(false, "chdir into the scratch tree"); return; }

  ensure_dir("scripts");              /* ensure_dir makes one level at a time */
  ensure_dir("scripts/srt_files");
  ensure_dir("backgroundmusic");
  ensure_dir("movies");

  /* a movie in the wrong place, a subtitle that must NOT be touched, and music */
  FILE *f = plat_fopen("scripts/srt_files/Stray Movie.mp4", "wb");
  if (f) { fputs("not a real movie", f); fclose(f); }
  f = plat_fopen("scripts/srt_files/Stray Movie.srt", "wb");
  if (f) { fputs("1\n00:00:01,000 --> 00:00:02,000\nhi\n", f); fclose(f); }
  f = plat_fopen("backgroundmusic/piano.mp3", "wb");
  if (f) { fputs("tune", f); fclose(f); }
  f = plat_fopen("movies/Movie With Srt (2026).mp4", "wb");
  if (f) { fputs("movie", f); fclose(f); }
  f = plat_fopen("movies/Movie With Srt (2026).srt", "wb");
  if (f) { fputs("1\n00:00:01,000 --> 00:00:02,000\nhi\n", f); fclose(f); }

  size_t moved = adopt_stray_movies();
  ck(moved == 1, "the stray movie was adopted");
  ck(file_exists("movies/Stray Movie.mp4"), "the movie is now in movies/");
  ck(!file_exists("scripts/srt_files/Stray Movie.mp4"), "and no longer in the subtitle folder");
  ck(file_exists("scripts/srt_files/Stray Movie.srt"), "the subtitle next to it stayed put");
  ck(file_exists("backgroundmusic/piano.mp3"), "music was not mistaken for a movie");

  /* the other direction: a subtitle dropped next to the movie */
  size_t subs = adopt_stray_subtitle("Movie With Srt (2026)");
  ck(subs == 1, "the subtitle dropped next to the movie was adopted");
  ck(file_exists("scripts/srt_files/Movie With Srt (2026).srt"),
     "the subtitle is now where the app looks for it");
  ck(!file_exists("movies/Movie With Srt (2026).srt"), "and gone from movies/");
  size_t again = adopt_stray_subtitle("Movie With Srt (2026)");
  ck(again == 0, "adopting twice does nothing the second time");

  /* an unrelated subtitle in movies/ is left alone - only the title match counts */
  f = plat_fopen("movies/Other Film.srt", "wb");
  if (f) { fputs("1\n00:00:01,000 --> 00:00:02,000\nhi\n", f); fclose(f); }
  size_t other = adopt_stray_subtitle("Movie With Srt (2026)");
  ck(other == 0, "an unrelated subtitle is not taken");
  ck(file_exists("movies/Other Film.srt"), "and is still where the user put it");

  /* never overwrite: a stray with the same name as an existing movie is left */
  f = plat_fopen("scripts/srt_files/Twin.mp4", "wb");
  if (f) { fputs("stray", f); fclose(f); }
  f = plat_fopen("movies/Twin.mp4", "wb");
  if (f) { fputs("the real one", f); fclose(f); }
  adopt_stray_movies();
  char keep[64] = "";
  f = plat_fopen("movies/Twin.mp4", "rb");
  if (f) { size_t n = fread(keep, 1, sizeof(keep) - 1, f); keep[n] = 0; fclose(f); }
  ck_str(keep, "the real one", "an existing movie of the same name is never overwritten");

  if (!plat_chdir(cwd)) ck(false, "chdir back");
  /* tidy up the scratch tree */
  plat_unlink("testadopt/movies/Stray Movie.mp4");
  plat_unlink("testadopt/movies/Movie With Srt (2026).mp4");
  plat_unlink("testadopt/movies/Other Film.srt");
  plat_unlink("testadopt/movies/Twin.mp4");
  plat_unlink("testadopt/scripts/srt_files/Stray Movie.srt");
  plat_unlink("testadopt/scripts/srt_files/Movie With Srt (2026).srt");
  plat_unlink("testadopt/scripts/srt_files/Twin.mp4");
  plat_unlink("testadopt/backgroundmusic/piano.mp3");
  plat_rmdir("testadopt/scripts/srt_files");
  plat_rmdir("testadopt/scripts");
  plat_rmdir("testadopt/backgroundmusic");
  plat_rmdir("testadopt/movies");
  plat_rmdir("testadopt");
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
  test_oversized_plan_is_merged_to_the_target();
  test_plan_within_the_target_is_untouched();
  test_openai_plan_249_clips_for_111_target();
  test_over_long_narration_is_trimmed_to_the_target();
  test_narration_at_the_target_is_not_touched();
  test_trim_never_cuts_the_closing_line();
  test_trim_handles_cjk_punctuation();
  test_invented_character_names_are_flagged();
  test_subtitle_copying_is_flagged();
  test_transition_command_shape();
  test_audit_trims_a_stubborn_over_long_plan();
  test_voice_pace_trim_keeps_whole_sentences();
  test_pace_scale_speaks_the_requested_minutes();
  test_recap_band_is_a_range();
  test_transition_styles_and_join_length();
  test_plan_request_asks_for_the_band_and_audits_it();
  test_minutes_print_without_lying();
  test_upload_landing_folder_by_file_type();
  test_stray_movie_files_are_adopted();
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
