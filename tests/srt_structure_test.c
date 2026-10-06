/* Structure of the converted ("modified") subtitle file.
 *
 * The user's report: "when subtitles file is updated in modified file it
 * should be structured properly, not clumped together".  A subtitle track is
 * cut into whatever fits the screen, so the converted file used to be a stream
 * of pieces ("Star." / "Command.", "I" / "can't" / "see the stars ..."); it is
 * now assembled into sentence-sized cues.  These checks read the converted
 * file back and verify both the structure and that nothing is lost on the way.
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

static void write_file_(const char *path, const char *data) {
  FILE *f = fopen(path, "wb");
  if (!f) { printf("[FAIL] cannot write %s\n", path); return; }
  fputs(data, f);
  fclose(f);
}

static char *slurp(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *b = (char *)malloc((size_t)n + 1);
  size_t got = fread(b, 1, (size_t)n, f);
  b[got] = 0;
  fclose(f);
  return b;
}

/* Count cue headers: a line of digits whose next line carries the "-->". */
static int count_cues(const char *srt) {
  int n = 0;
  const char *s = srt;
  while (s && *s) {
    const char *nl = strchr(s, '\n');
    if (!nl) break;
    bool digits = nl > s;
    for (const char *p = s; p < nl; p++) if (*p < '0' || *p > '9') { digits = false; break; }
    if (digits) {
      const char *next = nl + 1;
      const char *next_nl = strchr(next, '\n');
      const char *arrow = strstr(next, "-->");
      if (arrow && (!next_nl || arrow < next_nl)) n++;
    }
    s = nl + 1;
  }
  return n;
}

int main(void) {
  ensure_dir("scripts");
  ensure_dir("scripts/srt_files");

  /* ---- 1. ordinary file: structure must be preserved cue for cue ---------- */
  const char *plain =
    "1\n"
    "00:03:00,120 --> 00:03:01,400\n"
    "Star.\n\n"
    "2\n"
    "00:03:04,000 --> 00:03:05,900\n"
    "Command.\n\n"
    "3\n"
    "00:03:15,000 --> 00:03:23,000\n"
    "Rendez-vous with Star Command !\n\n";
  write_file_("srt_a.srt", plain);
  ck(convert_srt_timestamps_to_seconds("srt_a.srt", "a_mod.srt"),
     "an ordinary SRT converts");
  char *a = slurp("a_mod.srt");
  ck(a && count_cues(a) == 3, "three cues in, three cues out");
  ck(a && strstr(a, "Star.") && strstr(a, "Command.")
        && strstr(a, "Rendez-vous with Star Command !"), "every text line is kept");
  ck(a && strstr(a, "180 --> 181"), "the first cue keeps its own window");
  ck(a && strstr(a, "195 --> 203"), "the last cue keeps its own window");
  ck(a && !strstr(a, "194 --> 203"), "two cues were not clumped into one window");
  free(a);

  /* ---- 2. a subtitle line that is only digits --------------------------- */
  const char *digits =
    "1\n"
    "00:10:00,000 --> 00:10:02,000\n"
    "1944\n\n"
    "2\n"
    "00:10:03,000 --> 00:10:05,000\n"
    "The story starts here.\n\n"
    "3\n"
    "00:10:06,000 --> 00:10:08,000\n"
    "42\n\n";
  write_file_("srt_b.srt", digits);
  convert_srt_timestamps_to_seconds("srt_b.srt", "b_mod.srt");
  char *b = slurp("b_mod.srt");
  printf("---- digits-only file ----\n%s----------------\n", b ? b : "(null)");
  ck(b && count_cues(b) == 2, "the digits-only cues are cues, not cue indexes");
  ck(b && strstr(b, "1944") != NULL, "the year 1944 survives the conversion");
  ck(b && strstr(b, "606 --> 608\n42") != NULL,
     "a number line is kept as its own cue when the sentence before it ended");
  ck(b && strstr(b, "The story starts here.") != NULL, "normal text survives");
  free(b);

  /* ---- 3. dot as the millisecond separator, and no milliseconds ---------- */
  const char *dots =
    "1\n"
    "00:10:00.000 --> 00:10:02.500\n"
    "Dot separator cue.\n\n"
    "2\n"
    "00:10:03,000 --> 00:10:05,000\n"
    "Comma separator cue.\n\n"
    "3\n"
    "00:10:06 --> 00:10:08\n"
    "No milliseconds cue.\n\n";
  write_file_("srt_c.srt", dots);
  convert_srt_timestamps_to_seconds("srt_c.srt", "c_mod.srt");
  char *c = slurp("c_mod.srt");
  printf("---- separator file ----\n%s----------------\n", c ? c : "(null)");
  ck(c && strstr(c, "Dot separator cue.") != NULL, "a dot separated cue is kept");
  ck(c && strstr(c, "00:10:00.000") == NULL,
     "no timestamp text is left behind inside the narration");
  ck(c && strstr(c, "No milliseconds cue.") != NULL, "a cue without milliseconds is kept");
  ck(c && count_cues(c) == 3, "three cues after a mixed-separator file");
  free(c);

  /* ---- 4. multi line text stays multi line ------------------------------ */
  const char *multi =
    "1\n"
    "00:20:00,000 --> 00:20:02,000\n"
    "- What are you guys doing ?\n"
    "- Playing !!!\n\n"
    "2\n"
    "00:20:03,000 --> 00:20:05,000\n"
    "Next cue.\n\n";
  write_file_("srt_d.srt", multi);
  convert_srt_timestamps_to_seconds("srt_d.srt", "d_mod.srt");
  char *d = slurp("d_mod.srt");
  printf("---- multi line file ----\n%s----------------\n", d ? d : "(null)");
  ck(d && strstr(d, "- What are you guys doing ?\n- Playing !!!") != NULL,
     "the two dialogue lines of one cue stay on their own lines");
  ck(d && count_cues(d) == 2, "still two cues");
  free(d);

  /* ---- 5. out of order input is put back in timeline order -------------- */
  const char *messy =
    "1\n"
    "00:30:10,000 --> 00:30:12,000\n"
    "Second.\n\n"
    "2\n"
    "00:30:00,000 --> 00:30:02,000\n"
    "First.\n\n";
  write_file_("srt_e.srt", messy);
  convert_srt_timestamps_to_seconds("srt_e.srt", "e_mod.srt");
  char *e = slurp("e_mod.srt");
  ck(e && strstr(e, "First.") < strstr(e, "Second."), "cues are sorted into timeline order");
  ck(e && count_cues(e) == 2, "both cues survive the sort");
  free(e);

  /* ---- 6. WebVTT style settings are not swallowed as text ---------------- */
  const char *vtt =
    "WEBVTT\n\n"
    "1\n"
    "00:40:00,000 --> 00:40:02,000 align:start position:0%\n"
    "Cue with settings.\n\n";
  write_file_("srt_f.srt", vtt);
  convert_srt_timestamps_to_seconds("srt_f.srt", "f_mod.srt");
  char *f = slurp("f_mod.srt");
  printf("---- settings file ----\n%s----------------\n", f ? f : "(null)");
  ck(f && strstr(f, "Cue with settings.") != NULL, "a cue with trailing settings is kept");
  ck(f && strstr(f, "align:start") == NULL, "the settings are not copied into the narration");
  free(f);

  /* ---- 7. fragments are joined into sentences --------------------------- */
  const char *frag =
    "1\n"
    "00:50:00,000 --> 00:50:00,800\n"
    "I\n\n"
    "2\n"
    "00:50:00,800 --> 00:50:01,900\n"
    "can't\n\n"
    "3\n"
    "00:50:01,900 --> 00:50:04,000\n"
    "see the stars above us too much, Fog.\n\n"
    "4\n"
    "00:50:04,100 --> 00:50:05,000\n"
    "We need to find higher ground.\n\n";
  write_file_("srt_g.srt", frag);
  convert_srt_timestamps_to_seconds("srt_g.srt", "g_mod.srt");
  char *g = slurp("g_mod.srt");
  printf("---- fragment file ----\n%s----------------\n", g ? g : "(null)");
  ck(g && strstr(g, "I can't see the stars above us too much, Fog.") != NULL,
     "word-by-word fragments are joined into one sentence");
  ck(g && count_cues(g) == 2, "the two sentences stayed two cues");
  ck(g && strstr(g, "3000 --> 3004") != NULL,
     "the joined cue spans all of its fragments");
  ck(g && strstr(g, "3004 --> 3005") != NULL, "the next sentence keeps its own window");
  free(g);

  /* ---- 8. a sentence end and a long pause stop the merge ----------------- */
  const char *stop =
    "1\n"
    "01:00:00,000 --> 01:00:01,500\n"
    "Star.\n\n"
    "2\n"
    "01:00:01,500 --> 01:00:03,000\n"
    "Command.\n\n"
    "3\n"
    "01:00:03,000 --> 01:00:04,500\n"
    "and then\n\n"
    "4\n"
    "01:00:09,000 --> 01:00:10,500\n"
    "a long pause comes before this.\n\n";
  write_file_("srt_h.srt", stop);
  convert_srt_timestamps_to_seconds("srt_h.srt", "h_mod.srt");
  char *h = slurp("h_mod.srt");
  printf("---- stops file ----\n%s----------------\n", h ? h : "(null)");
  ck(h && count_cues(h) == 4, "finished sentences and long pauses are not merged");
  ck(h && strstr(h, "Star. Command.") == NULL, "two finished sentences stay apart");
  ck(h && strstr(h, "and then a long pause") == NULL, "a pause of 4.5 s is a boundary");
  free(h);

  /* ---- 9. two speakers in one cue are left alone ------------------------- */
  const char *duo =
    "1\n"
    "01:10:00,000 --> 01:10:02,000\n"
    "What are you guys doing?\n"
    "- Playing !!!\n\n";
  write_file_("srt_i.srt", duo);
  convert_srt_timestamps_to_seconds("srt_i.srt", "i_mod.srt");
  char *i = slurp("i_mod.srt");
  ck(i && strstr(i, "doing?\n- Playing !!!") != NULL,
     "the two lines of one cue stay two lines");
  ck(i && count_cues(i) == 1, "still one cue");
  free(i);

  /* ---- 10. overlapping and zero length cues are tidied up --------------- */
  const char *overlap =
    "1\n"
    "01:20:00,000 --> 01:20:05,000\n"
    "Two people talk at once here,\n\n"
    "2\n"
    "01:20:02,000 --> 01:20:04,000\n"
    "and the second one cuts in.\n\n"
    "3\n"
    "01:20:10,000 --> 01:20:10,000\n"
    "A cue with no length.\n\n";
  write_file_("srt_j.srt", overlap);
  convert_srt_timestamps_to_seconds("srt_j.srt", "j_mod.srt");
  char *j = slurp("j_mod.srt");
  printf("---- overlap file ----\n%s----------------\n", j ? j : "(null)");
  ck(j && strstr(j, "4800 --> 4802") != NULL, "an overlapping cue ends where the next one starts");
  ck(j && strstr(j, "4810 --> 4811") != NULL, "a zero length cue is given a second");
  ck(j && count_cues(j) == 3, "both cues of an overlap survive");
  free(j);

  /* ---- 11. a cached file from the old code is recognised as out of date -- */
  write_file_("old_mod.srt",
              "1\n100 --> 100\nI\n\n"
              "2\n100 --> 101\ncan't\n\n"
              "3\n101 --> 102\nsee\n\n"
              "4\n102 --> 103\nthe stars\n\n");
  ck(srt_seconds_file_is_usable("old_mod.srt") == false,
     "a fragment stream from an older version is rebuilt");
  ck(srt_seconds_file_is_usable("i_mod.srt") == true, "a structured file is reused");
  ck(srt_seconds_file_is_usable("g_mod.srt") == true, "a merged file is reused");

  printf("\n%d checks, %d failures\n", g_pass + g_fail, g_fail);
  if (g_fail == 0) printf("ALL OK\n");
  return g_fail == 0 ? 0 : 1;
}
