/* Burnt-in captions and the language plumbing.
 *
 * Covers the "the Burn-in small subtitles shows the next sentence before the
 * present sentence" report (the two lines of one caption were stacked bottom
 * up, so the viewer read the end of a sentence first) and the language work:
 * every language goes through one table for the subtitle tag, the Wikipedia
 * subdomain, the caption font, the Edge voice and the wrong-language detector.
 *
 * Build: compiled against src/generator.c (see CMakeLists.txt, BUILD_UNIT_TESTS).
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

/* ---------------------------------------------------------------- chain parse */

typedef struct {
  char  text[1024];
  int   row;
  double t0, t1;
} CapLine;

static int parse_chain(const char *chain, CapLine *out, int max) {
  int n = 0;
  const char *p = chain;
  while (p && n < max) {
    const char *d = strstr(p, ",drawtext=");
    if (!d) break;
    const char *t = strstr(d, "text='");
    if (!t) break;
    t += 6;
    const char *te = strstr(t, "':fontcolor=");
    if (!te) break;
    size_t len = (size_t)(te - t);
    if (len >= sizeof(out[n].text)) len = sizeof(out[n].text) - 1;
    memcpy(out[n].text, t, len);
    out[n].text[len] = 0;

    const char *y = strstr(te, "-th-");
    if (!y) break;
    out[n].row = atoi(y + 4);

    const char *e = strstr(te, "enable='between(t,");
    if (!e) break;
    out[n].t0 = atof(e + strlen("enable='between(t,"));
    const char *c2 = strchr(e + strlen("enable='between(t,"), ',');
    out[n].t1 = atof(c2 + 1);

    n++;
    p = te + 1;
  }
  return n;
}

static CaptionAudio audio_of(double onset, double stop) {
  CaptionAudio au;
  au.pauses = NULL; au.plens = NULL; au.npauses = 0;
  au.onset = onset; au.stop = stop;
  return au;
}

/* Rebuild what a viewer reads: chunk by chunk in window order, and inside a
 * chunk the lines from the top one down. */
static void read_order(const CapLine *l, int n, char *out, size_t outsz) {
  int wins[64];
  int nw = 0;
  for (int i = 0; i < n; i++) {
    bool seen = false;
    for (int w = 0; w < nw; w++)
      if (l[wins[w]].t0 == l[i].t0 && l[wins[w]].t1 == l[i].t1) { seen = true; break; }
    if (!seen && nw < 64) wins[nw++] = i;
  }
  for (int a = 0; a < nw; a++)
    for (int b = a + 1; b < nw; b++)
      if (l[wins[b]].t0 < l[wins[a]].t0) { int t = wins[a]; wins[a] = wins[b]; wins[b] = t; }

  size_t o = 0;
  out[0] = 0;
  bool first_word = true;
  for (int w = 0; w < nw; w++) {
    int idx[64], k = 0;
    for (int i = 0; i < n; i++)
      if (l[i].t0 == l[wins[w]].t0 && l[i].t1 == l[wins[w]].t1 && k < 64) idx[k++] = i;
    for (int a = 0; a < k; a++)
      for (int b = a + 1; b < k; b++)
        if (l[idx[b]].row > l[idx[a]].row) { int t = idx[a]; idx[a] = idx[b]; idx[b] = t; }
    for (int a = 0; a < k; a++) {
      if (!first_word) { if (o + 1 < outsz) out[o++] = ' '; }
      first_word = false;
      for (const char *q = l[idx[a]].text; *q && o + 1 < outsz; q++) out[o++] = *q;
    }
  }
  out[o] = 0;
}

/* ---------------------------------------------------------------- the tests */

static void test_caption_two_lines(void) {
  const char *text =
    "The story begins with a toy space ranger named Buzz Lightyear repeating his catchphrase";
  CaptionAudio au = audio_of(0.10, 6.00);
  char *chain = caption_filter_chain(text, "resources/Inter-Regular.ttf", 6.0, &au, 1920, 1080);
  ck(chain != NULL, "caption chain is built for a two line sentence");
  if (!chain) return;

  CapLine l[64];
  int n = parse_chain(chain, l, 64);
  ck(n >= 2, "the sentence became more than one caption line");

  int idx1 = 0, idx2 = -1;
  for (int i = 1; i < n; i++) if (l[i].t0 == l[0].t0 && l[i].t1 == l[0].t1) { idx2 = i; break; }
  ck(idx2 > 0, "both lines of the sentence share one window");
  if (idx2 > 0) {
    ck(l[idx1].row > l[idx2].row,
       "the first half of the sentence is drawn above the second half");
    char read[2048];
    read_order(l, n, read, sizeof(read));
    char want[1024];
    snprintf(want, sizeof(want), "%s", text);
    ck_str(read, want, "reading the caption top to bottom gives the sentence in order");
  }

  bool ordered = true, overlap = false;
  for (int i = 1; i < n; i++) {
    if (l[i].t0 < l[i - 1].t0) ordered = false;
    if (i > 0 && l[i].t0 < l[i - 1].t1 - 1e-6 && l[i].t1 != l[i - 1].t1) overlap = true;
  }
  ck(ordered, "caption windows are in ascending order");
  ck(!overlap, "caption windows do not overlap");
  ck(l[0].t0 >= 0.0 && l[n - 1].t1 <= 6.01, "caption windows stay inside the clip");

  free(chain);
}

static void test_caption_windows_and_order_multi(void) {
  const char *text =
    "The movie starts with a young engineer in Berlin. She loses her job that same "
    "week, and her brother offers her a place to stay. Then a stranger knocks on the "
    "door and everything changes.";
  CaptionAudio au = audio_of(0.20, 12.0);
  char *chain = caption_filter_chain(text, "resources/Inter-Regular.ttf", 12.0, &au, 1920, 1080);
  ck(chain != NULL, "multi sentence caption chain is built");
  if (!chain) return;

  CapLine l[128];
  int n = parse_chain(chain, l, 128);
  ck(n >= 3, "a three sentence narration makes several caption lines");

  bool ok_rows = true;
  for (int i = 1; i < n; i++) {
    if (l[i].t0 == l[i - 1].t0 && l[i].t1 == l[i - 1].t1) {
      if (l[i].row >= l[i - 1].row) ok_rows = false;
    }
  }
  ck(ok_rows, "each two line chunk is stacked top down");

  bool ok_win = true;
  double last_t0 = -1, last_t1 = -1;
  for (int i = 0; i < n; i++) {
    if (l[i].t0 > last_t0 + 1e-9) { last_t0 = l[i].t0; last_t1 = l[i].t1; }
    else if (l[i].t1 > last_t1 + 1e-9) { ok_win = false; }
  }
  ck(ok_win, "each chunk starts after the previous chunk ended");

  char read[4096];
  read_order(l, n, read, sizeof(read));
  ck(strstr(read, "The movie starts with a young engineer in Berlin.") != NULL,
     "the first sentence is the first thing the viewer reads");
  ck(strstr(read, "She loses her job") != NULL, "the story keeps its order in the captions");

  size_t longest = 0;
  for (int i = 0; i < n; i++) {
    size_t len = strlen(l[i].text);
    if (len > longest) longest = len;
  }
  ck(longest <= 42, "no caption line is longer than 42 characters at 1080p");

  free(chain);
}

static void test_caption_line_length_by_script(void) {
  const char *zh =
    "\u6545\u4e8b\u4ece\u4e00\u4e2a\u5e74\u8f7b\u7684\u5de5\u7a0b\u5e08\u5f00\u59cb\uff0c"
    "\u4ed6\u4f4f\u5728\u67cf\u6797\u3002\u4ed6\u5728\u540c\u4e00\u5468\u5931\u53bb\u4e86"
    "\u5de5\u4f5c\uff0c\u4ed6\u7684\u59d0\u59d0\u8ba9\u4ed6\u4f4f\u5728\u5979\u5bb6\u3002";
  CaptionAudio au = audio_of(0.10, 9.0);
  char *chain = caption_filter_chain(zh, "resources/Inter-Regular.ttf", 9.0, &au, 1920, 1080);
  ck(chain != NULL, "Chinese caption chain is built");
  if (!chain) return;

  CapLine l[128];
  int n = parse_chain(chain, l, 128);
  ck(n >= 2, "the Chinese narration became several lines");
  for (int i = 0; i < n; i++) {
    size_t runes = 0;
    for (const unsigned char *q = (const unsigned char *)l[i].text; *q; q++)
      if ((*q & 0xC0) != 0x80) runes++;
    if (runes > 24) {
      ck(false, "a Chinese caption line is short enough (<= 24 characters)");
      free(chain);
      return;
    }
  }
  ck(true, "Chinese caption lines are capped at 24 characters at 1080p");

  char *small = caption_filter_chain(zh, "resources/Inter-Regular.ttf", 9.0, &au, 640, 360);
  ck(small != NULL, "Chinese caption chain is built for a small frame");
  free(small);
  free(chain);
}

static void test_caption_escaping(void) {
  const char *text = "Anna's plan works: the bank closes at 5.";
  CaptionAudio au = audio_of(0.0, 3.0);
  char *chain = caption_filter_chain(text, "resources/Inter-Regular.ttf", 3.0, &au, 1280, 720);
  ck(chain != NULL, "apostrophe + colon caption is built");
  if (!chain) return;
  ck(strstr(chain, "\\:") != NULL, "colons are escaped for the ffmpeg filter parser");
  ck(strstr(chain, "Anna") != NULL, "the words survive the escaping");
  ck(strstr(chain, "Anna's") == NULL, "a raw apostrophe never reaches ffmpeg");
  CapLine l[8];
  int n = parse_chain(chain, l, 8);
  ck(n >= 1 && strstr(l[0].text, "\xE2\x80\x99") != NULL,
     "the apostrophe became the typographic quote ffmpeg can render");
  free(chain);

  char *empty = caption_filter_chain("   ", "f.ttf", 3.0, &au, 1280, 720);
  ck(empty == NULL, "a blank caption produces no filters");
}

/* ---------------------------------------------------------------- languages */

static void test_language_codes(void) {
  ck_str(recap_lang_code("Chinese"), "zh", "Chinese -> zh");
  ck_str(recap_lang_code("chinese"), "zh", "case does not matter for languages");
  ck_str(recap_lang_code("Chinese (Simplified)"), "zh", "a decorated name still maps");
  ck_str(recap_lang_code("Spanish"), "es", "Spanish -> es");
  ck_str(recap_lang_code("French"), "fr", "French -> fr");
  ck_str(recap_lang_code("German"), "de", "German -> de");
  ck_str(recap_lang_code("Arabic"), "ar", "Arabic -> ar");
  ck_str(recap_lang_code("Japanese"), "ja", "Japanese -> ja");
  ck_str(recap_lang_code("hi"), "hi", "a plain two letter code is kept");
  ck_str(recap_lang_code("English"), "en", "English -> en");
  ck_str(recap_lang_code(""), "en", "empty -> en");
  ck_str(recap_lang_code("Klingon"), "en", "an unknown language falls back to en");

  ck_str(recap_lang_label("Chinese"), "Chinese", "label for Chinese");
  ck_str(recap_lang_label("French"), "French", "label for French");
  ck_str(recap_lang_label("English"), "", "English has no file suffix");

  ck_str(edge_voice_for_language("Chinese"), "zh-CN-YunxiNeural", "Chinese edge voice");
  ck_str(edge_voice_for_language("French"), "fr-FR-HenriNeural", "French edge voice");
  ck_str(edge_voice_for_language("German"), "de-DE-ConradNeural", "German edge voice");
  ck_str(edge_voice_for_language("Klingon"), "en-US-ChristopherNeural",
         "an unknown language gets the English voice");

  char wl[8];
  wiki_lang_for("Spanish", wl, sizeof(wl));
  ck_str(wl, "es", "Wikipedia subdomain for Spanish");
  wiki_lang_for("Japanese", wl, sizeof(wl));
  ck_str(wl, "ja", "Wikipedia subdomain for Japanese");
  wiki_lang_for("", wl, sizeof(wl));
  ck_str(wl, "en", "Wikipedia subdomain defaults to en");
}

static ClipPlanList make_plan(const char **texts, size_t n) {
  ClipPlanList l;
  l.items = (ClipPlan *)calloc(n, sizeof(ClipPlan));
  l.count = n;
  for (size_t i = 0; i < n; i++) {
    l.items[i].start = (int)(i * 10 + 1);
    l.items[i].end   = (int)(i * 10 + 9);
    l.items[i].narration = str_dup(texts[i]);
  }
  return l;
}

static void test_wrong_language_detection(void) {
  const char *en_txt[] = {
    "The story begins in a small town where a young nurse named Anna works the night "
    "shift at the only hospital. She finds a locked room that is not on any map, and "
    "the doctor tells her to forget what she saw.",
    "Later that week she comes back with a key she copied, and what she sees inside "
    "changes everything she thought she knew about the people she works with."
  };
  const char *zh_txt[] = {
    "\u6545\u4e8b\u4ece\u4e00\u4e2a\u5c0f\u9547\u5f00\u59cb\uff0c\u5e74\u8f7b\u7684"
    "\u62a4\u58eb\u5b89\u5a1c\u5728\u552f\u4e00\u7684\u533b\u9662\u4e0a\u591c\u73ed\u3002"
    "\u5979\u53d1\u73b0\u4e86\u4e00\u4e2a\u4e0d\u5728\u5730\u56fe\u4e0a\u7684\u623f\u95f4\uff0c"
    "\u533b\u751f\u8ba9\u5979\u5fd8\u6389\u5979\u770b\u5230\u7684\u4e00\u5207\u3002",
    "\u4e00\u5468\u4e4b\u540e\uff0c\u5979\u5e26\u7740\u914d\u597d\u7684\u94a5\u5319\u56de\u6765\uff0c"
    "\u5979\u5728\u91cc\u9762\u770b\u5230\u7684\u4e1c\u897f\u6539\u53d8\u4e86\u5979\u5bf9\u540c\u4e8b\u7684"
    "\u770b\u6cd5\u3002"
  };
  const char *ja_txt[] = {
    "\u7269\u8a9e\u306f\u5c0f\u3055\u306a\u753a\u304b\u3089\u59cb\u307e\u308a\u307e\u3059\u3002"
    "\u82e5\u3044\u770b\u8b77\u5e2b\u306e\u30a2\u30f3\u30ca\u306f\u552f\u4e00\u306e\u75c5\u9662\u3067"
    "\u591c\u52e4\u3092\u3057\u3066\u3044\u307e\u3059\u3002\u5f7c\u5973\u306f\u5730\u56f3\u306b\u306a\u3044"
    "\u90e8\u5c4b\u3092\u898b\u3064\u3051\u307e\u3059\u3002",
    "\u4e00\u9031\u9593\u5f8c\u3001\u5f7c\u5973\u306f\u5408\u3044\u9375\u3092\u6301\u3063\u3066"
    "\u623b\u3063\u3066\u304d\u307e\u3059\u3002\u5f7c\u5973\u304c\u898b\u305f\u3082\u306e\u306f\u3001"
    "\u540c\u50da\u306b\u5bfe\u3059\u308b\u5f7c\u5973\u306e\u8003\u3048\u3092\u5909\u3048\u307e\u3057\u305f\u3002"
  };
  const char *ru_txt[] = {
    "\u0418\u0441\u0442\u043e\u0440\u0438\u044f \u043d\u0430\u0447\u0438\u043d\u0430\u0435\u0442\u0441\u044f "
    "\u0432 \u043c\u0430\u043b\u0435\u043d\u044c\u043a\u043e\u043c \u0433\u043e\u0440\u043e\u0434\u0435, "
    "\u0433\u0434\u0435 \u043c\u043e\u043b\u043e\u0434\u0430\u044f \u043c\u0435\u0434\u0441\u0435\u0441\u0442\u0440\u0430 "
    "\u0410\u043d\u043d\u0430 \u0440\u0430\u0431\u043e\u0442\u0430\u0435\u0442 \u0432 \u043d\u043e\u0447\u043d\u0443\u044e "
    "\u0441\u043c\u0435\u043d\u0443 \u0432 \u0435\u0434\u0438\u043d\u0441\u0442\u0432\u0435\u043d\u043d\u043e\u0439 "
    "\u0431\u043e\u043b\u044c\u043d\u0438\u0446\u0435 \u0433\u043e\u0440\u043e\u0434\u0430.",
    "\u041e\u043d\u0430 \u043d\u0430\u0445\u043e\u0434\u0438\u0442 \u0437\u0430\u043f\u0435\u0440\u0442\u0443\u044e "
    "\u043a\u043e\u043c\u043d\u0430\u0442\u0443, \u043a\u043e\u0442\u043e\u0440\u043e\u0439 \u043d\u0435\u0442 "
    "\u043d\u0430 \u043a\u0430\u0440\u0442\u0435, \u0438 \u0432\u0440\u0430\u0447 \u0433\u043e\u0432\u043e\u0440\u0438\u0442 "
    "\u0435\u0439 \u0437\u0430\u0431\u044b\u0442\u044c \u043e\u0431 \u044d\u0442\u043e\u043c."
  };
  const char *es_txt[] = {
    "La historia comienza en un pueblo pequeno donde una joven enfermera trabaja de "
    "noche en el unico hospital. Ella encuentra una habitacion que no esta en el mapa "
    "y el doctor le dice que olvide lo que vio con sus propios ojos esa noche.",
    "Una semana despues vuelve con una llave copiada y lo que ve dentro cambia todo lo "
    "que pensaba sobre las personas con las que trabaja en el hospital."
  };
  const char *fr_txt[] = {
    "L'histoire commence dans une petite ville ou une jeune infirmiere travaille la "
    "nuit dans le seul hopital. Elle trouve une chambre qui n'est pas sur le plan et "
    "le medecin lui dit d'oublier ce qu'elle a vu cette nuit la.",
    "Une semaine plus tard elle revient avec une cle copiee et ce qu'elle voit dans "
    "la piece change tout ce qu'elle pensait des gens avec qui elle travaille."
  };

  ClipPlanList en = make_plan(en_txt, 2);
  ClipPlanList zh = make_plan(zh_txt, 2);
  ClipPlanList ja = make_plan(ja_txt, 2);
  ClipPlanList ru = make_plan(ru_txt, 2);
  ClipPlanList es = make_plan(es_txt, 2);
  ClipPlanList fr = make_plan(fr_txt, 2);

  ck(!plan_language_mismatch(en.items, en.count, "en"), "an English plan is never flagged");
  ck(plan_language_mismatch(en.items, en.count, "zh"), "English text is flagged for a Chinese run");
  ck(!plan_language_mismatch(zh.items, zh.count, "zh"), "a Chinese plan passes the Chinese check");
  ck(plan_language_mismatch(en.items, en.count, "ja"), "English text is flagged for a Japanese run");
  ck(!plan_language_mismatch(ja.items, ja.count, "ja"), "a Japanese plan passes the Japanese check");
  ck(!plan_language_mismatch(ru.items, ru.count, "ru"), "a Russian plan passes the Russian check");
  ck(plan_language_mismatch(en.items, en.count, "ru"), "English text is flagged for a Russian run");
  ck(plan_language_mismatch(en.items, en.count, "ar"), "English text is flagged for an Arabic run");
  ck(plan_language_mismatch(en.items, en.count, "es"), "English text is flagged for a Spanish run");
  ck(!plan_language_mismatch(es.items, es.count, "es"), "a Spanish plan passes the Spanish check");
  ck(plan_language_mismatch(en.items, en.count, "fr"), "English text is flagged for a French run");
  ck(!plan_language_mismatch(fr.items, fr.count, "fr"), "a French plan passes the French check");

  const char *tiny[] = { "It starts.", "It ends." };
  ClipPlanList t = make_plan(tiny, 2);
  ck(!plan_language_mismatch(t.items, t.count, "zh"), "a very short plan is not flagged");

  free_clip_plan_list(&en); free_clip_plan_list(&zh); free_clip_plan_list(&ja);
  free_clip_plan_list(&ru); free_clip_plan_list(&es); free_clip_plan_list(&fr);
  free_clip_plan_list(&t);
}

static void test_speech_measurement(void) {
  ck(lang_counts_chars("zh"), "Chinese is measured in characters");
  ck(lang_counts_chars("th"), "Thai is measured in characters");
  ck(!lang_counts_chars("en"), "English is measured in words");
  ck(!lang_counts_chars("ar"), "Arabic is measured in words");
  ck((count_speech_units("one two three", "en") == 3.0), "three English words counted");
  ck((count_speech_units("\u4e00\u4e8c\u4e09\u56db\u4e94", "zh") == 5.0),
     "five Chinese characters counted");
  ck((lang_speech_units_per_sec("zh") == 4.0), "Chinese speech rate");
  ck((lang_speech_units_per_sec("ar") == 2.1), "Arabic speech rate");
  ck((lang_speech_units_per_sec("en") == 2.6), "English speech rate");
  ck((lang_speech_units_per_sec("fr") == 2.6), "French falls back to the normal rate");
}

/* A 20 minute recap at 30 clips asks for 40 second clips; the words target has
 * to follow the clip length or the video comes out at 12 minutes. */
static void test_prompt_length_numbers(void) {
  int mn = 0, mx = 0;
  clip_seconds_range(40, &mn, &mx);
  ck((mn == 32 && mx == 48), "40 s clips give a 32-48 s band");
  clip_seconds_range(12, &mn, &mx);
  ck((mn == 8 && mx == 16), "the short band is unchanged");

  const char *texts[] = {
    "Word word word word word word word word word word word word word word word word "
    "word word word word word word word word word word word word word word word word.",
    "Short."
  };
  ClipPlanList l = make_plan(texts, 2);
  double sec = plan_speech_seconds(l.items, l.count, "en");
  ck((sec > 12.0 && sec < 16.0), "32 words at 2.6 words/s measures about 13 s");
  free_clip_plan_list(&l);
}

static void test_caption_font_choice(void) {
  Config c;
  memset(&c, 0, sizeof(c));
  snprintf(c.caption_font, sizeof(c.caption_font), "resources/Inter-Regular.ttf");
  snprintf(c.caption_font_zh, sizeof(c.caption_font_zh), "C:/Windows/Fonts/msyh.ttc");

  ck_str(caption_font_for_language(&c, "Chinese"), "C:/Windows/Fonts/msyh.ttc",
         "the configured Chinese caption font wins");
  ck_str(caption_font_for_language(&c, "English"), "resources/Inter-Regular.ttf",
         "Latin languages use the normal caption font");

  Config c2;
  memset(&c2, 0, sizeof(c2));
  snprintf(c2.caption_font, sizeof(c2.caption_font), "resources/Inter-Regular.ttf");
  const char *auto_zh = caption_font_for_language(&c2, "Chinese");
  ck(auto_zh != NULL, "the auto detected Chinese font is never NULL");
  printf("       (auto Chinese font on this machine: \"%s\")\n", auto_zh);
  ck_str(caption_font_for_language(&c2, "French"), "resources/Inter-Regular.ttf",
         "French keeps the normal font when nothing is configured");
}

/* Subtitle files: ".zh.srt" belongs to the Chinese run, ".en.cc.srt" is English. */
static void test_subtitle_language_tags(void) {
  ensure_dir("scripts");
  ensure_dir("scripts/srt_files");
  const char *files[] = {
    "scripts/srt_files/Tag Test.en.cc.srt",
    "scripts/srt_files/Tag Test.zh.srt",
    NULL
  };
  for (int i = 0; files[i]; i++) {
    FILE *f = fopen(files[i], "wb");
    if (f) { fputs("1\n00:00:01,000 --> 00:00:02,000\nhello\n\n", f); fclose(f); }
  }

  char out[PATH_MAX];
  bool ok = find_subtitle_srt("Tag Test", out, sizeof(out), "zh");
  ck(ok && strstr(out, "Tag Test.zh.srt") != NULL,
     "the Chinese run picks up Tag Test.zh.srt");
  ok = find_subtitle_srt("Tag Test", out, sizeof(out), "en");
  ck(ok && strstr(out, "Tag Test.en.cc.srt") != NULL,
     "the English run picks up Tag Test.en.cc.srt (and not the Chinese file)");
  ok = find_subtitle_srt("Tag Test", out, sizeof(out), "fr");
  ck(!ok, "a French run does not grab an English or Chinese file");
}

int main(void) {
  test_caption_two_lines();
  test_caption_windows_and_order_multi();
  test_caption_line_length_by_script();
  test_caption_escaping();
  test_language_codes();
  test_wrong_language_detection();
  test_speech_measurement();
  test_prompt_length_numbers();
  test_caption_font_choice();
  test_subtitle_language_tags();

  printf("\n%d checks, %d failures\n", g_pass + g_fail, g_fail);
  if (g_fail == 0) printf("ALL OK\n");
  return g_fail == 0 ? 0 : 1;
}
