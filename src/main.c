/*
 * main.c - raylib desktop UI (movie_summary_bot.exe).
 *
 * Same UI as the original AI-Movie-Shorts:
 *   - buttons to open movies/, movies_retired/, output/, scripts/srt_files/
 *   - START GENERATION button (runs the pipeline on a background thread)
 *   - live log panel + status line, resizable window
 *
 * Windows note: this file must not include <windows.h> (it clashes with
 * raylib.h). Threads, mutexes and "open in Explorer" live in platform.c.
 */
#include "raylib.h"
#include "generator.h"
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

static PlatMutex *g_log_lock = NULL;
static void log_lock(void)   { plat_mutex_lock(g_log_lock); }
static void log_unlock(void) { plat_mutex_unlock(g_log_lock); }

static volatile int g_running = 0;
static volatile int g_last_rc = 0;

#define LOG_MAX_LINES 300
#define LOG_LINE_MAX  600

static char g_log[LOG_MAX_LINES][LOG_LINE_MAX];
static int  g_log_head = 0;
static int  g_log_count = 0;

/* snapshot buffer used while drawing (static: too big for the stack) */
static char g_log_snapshot[LOG_MAX_LINES][LOG_LINE_MAX];

// UI font
static Font g_uiFont = {0};
static const float g_uiSpacing = 1.0f;

static void ui_log_hook(const char *line) {
  if (!line) return;
  log_lock();
  snprintf(g_log[g_log_head], LOG_LINE_MAX, "%s", line);
  g_log_head = (g_log_head + 1) % LOG_MAX_LINES;
  if (g_log_count < LOG_MAX_LINES) g_log_count++;
  log_unlock();
}

static void open_folder_rel(const char *rel) {
  plat_open_folder(rel); /* creates the folder if needed, then opens Explorer */
}

static bool draw_button(Font font, Rectangle r, const char *label, bool enabled, float fontSize) {
  Vector2 m = GetMousePosition();
  bool hot = CheckCollisionPointRec(m, r);

  Color bg;
  Color fg = RAYWHITE;

  if (!enabled) bg = (Color){60, 60, 60, 255};
  else if (hot) bg = (Color){70, 120, 200, 255};
  else bg = (Color){40, 90, 170, 255};

  DrawRectangleRounded(r, 0.25f, 10, bg);
  DrawRectangleRoundedLinesEx(r, 0.25f, 10, 1.0f, (Color){20, 20, 20, 255});

  Vector2 ts = MeasureTextEx(font, label, fontSize, g_uiSpacing);

  Vector2 pos = {
    r.x + (r.width  - ts.x) * 0.5f,
    r.y + (r.height - ts.y) * 0.5f
  };

  DrawTextEx(font, label, pos, fontSize, g_uiSpacing, fg);

  if (!enabled) return false;
  return hot && IsMouseButtonReleased(MOUSE_BUTTON_LEFT);
}

static void worker_thread(void *p) {
  (void)p;
  generator_set_log_hook(ui_log_hook);
  g_last_rc = run_generation();
  generator_set_log_hook(NULL);
  g_running = 0;
}

static void start_generation_thread(void) {
  if (g_running) return;
  g_running = 1; /* set before the thread starts so the button can't double-fire */
  if (!plat_thread_start_detached(worker_thread, NULL)) {
    g_running = 0;
    g_last_rc = -1;
    ui_log_hook("[FATAL] could not start worker thread");
  }
}

static void draw_log_panel(Font font, Rectangle r) {
  DrawRectangleRec(r, (Color){18, 18, 18, 255});
  DrawRectangleLinesEx(r, 2.0f, (Color){40, 40, 40, 255});

  const float fontSize = 14.0f;
  const int pad = 8;
  const int lineH = 16;

  int maxLines = (int)((r.height - 2*pad) / (float)lineH);
  if (maxLines < 1) maxLines = 1;

  // snapshot log under lock
  int count = 0;

  log_lock();
  count = g_log_count;
  int head = g_log_head;
  for (int i = 0; i < count; i++) {
    int idx = (head - count + i);
    while (idx < 0) idx += LOG_MAX_LINES;
    idx %= LOG_MAX_LINES;
    snprintf(g_log_snapshot[i], LOG_LINE_MAX, "%s", g_log[idx]);
  }
  log_unlock();

  int start = 0;
  if (count > maxLines) start = count - maxLines;

  BeginScissorMode((int)r.x, (int)r.y, (int)r.width, (int)r.height);
  float y = r.y + (float)pad;
  for (int i = start; i < count; i++) {
    DrawTextEx(font, g_log_snapshot[i], (Vector2){r.x + (float)pad, y}, fontSize, g_uiSpacing,
               (Color){210, 210, 210, 255});
    y += (float)lineH;
  }
  EndScissorMode();
}

int main(void) {
  plat_console_init();
  plat_enter_project_dir(); /* so double-clicking the .exe in build\ still finds resources/ */
  g_log_lock = plat_mutex_create();

  /* make sure the folders behind the buttons exist */
  plat_mkdir("movies");
  plat_mkdir("movies_retired");
  plat_mkdir("output");
  plat_mkdir("tiktok_output");
  plat_mkdir("scripts");
  plat_mkdir("scripts/srt_files");

  SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT);
  InitWindow(920, 560, "C-AI Movie Shorts");
  SetWindowMinSize(700, 420);
  SetTargetFPS(60);

  // Load Inter Regular from resources/
  if (FileExists("resources/Inter-Regular.ttf")) {
    g_uiFont = LoadFontEx("resources/Inter-Regular.ttf", 64, NULL, 0);
    SetTextureFilter(g_uiFont.texture, TEXTURE_FILTER_BILINEAR);
  } else {
    g_uiFont = GetFontDefault();
    ui_log_hook("[WARN] resources/Inter-Regular.ttf not found - using default font. "
                "Run the app from the project folder.");
  }

  {
    char cwd[1024];
    char line[1200];
    plat_getcwd(cwd, sizeof(cwd));
    snprintf(line, sizeof(line), "[INFO] Project folder: %s", cwd);
    ui_log_hook(line);
    ui_log_hook("[INFO] Put .mp4 movies in movies/ and press START GENERATION.");
  }

  while (!WindowShouldClose()) {
    const float W = (float)GetScreenWidth();
    const float H = (float)GetScreenHeight();

    BeginDrawing();
    ClearBackground((Color){25, 25, 25, 255});

    DrawTextEx(g_uiFont, "Folders", (Vector2){30, 20}, 24, g_uiSpacing, RAYWHITE);

    if (draw_button(g_uiFont, (Rectangle){30, 60, 260, 44}, "Open Movies Folder", true, 18)) {
      open_folder_rel("movies");
    }
    if (draw_button(g_uiFont, (Rectangle){30, 115, 260, 44}, "Open Retired Movies Folder", true, 18)) {
      open_folder_rel("movies_retired");
    }
    if (draw_button(g_uiFont, (Rectangle){30, 170, 260, 44}, "Open Output Folder", true, 18)) {
      open_folder_rel("output");
    }
    if (draw_button(g_uiFont, (Rectangle){30, 225, 260, 44}, "Open SRT Folder", true, 18)) {
      open_folder_rel("scripts/srt_files");
    }

    bool canStart = (g_running == 0);
    const char *startLabel = canStart ? "START GENERATION" : "RUNNING...";
    if (draw_button(g_uiFont, (Rectangle){30, 280, 260, 70}, startLabel, canStart, 22)) {
      // clear UI log before run
      log_lock();
      g_log_head = 0;
      g_log_count = 0;
      log_unlock();

      start_generation_thread();
    }

    char status[256];
    snprintf(status, sizeof(status),
             "Status: %s   (last exit code: %d)",
             (g_running ? "RUNNING" : "IDLE"),
             (int)g_last_rc);
    DrawTextEx(g_uiFont, status, (Vector2){30, 370}, 18, g_uiSpacing, (Color){220, 220, 220, 255});

    DrawTextEx(g_uiFont, "Log", (Vector2){320, 20}, 24, g_uiSpacing, RAYWHITE);
    /* log panel stretches with the window */
    float lw = W - 320 - 30;
    float lh = H - 60 - 30;
    if (lw < 100) lw = 100;
    if (lh < 100) lh = 100;
    draw_log_panel(g_uiFont, (Rectangle){320, 60, lw, lh});

    EndDrawing();
  }

  if (g_uiFont.texture.id != GetFontDefault().texture.id) UnloadFont(g_uiFont);
  CloseWindow();
  return 0;
}
