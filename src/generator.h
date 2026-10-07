#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Which project folder a file belongs in, judged from its name: the web panel
   files uploads by this, so a movie picked while another tab is open still
   lands in movies/ (and subtitles in scripts/srt_files, music in
   backgroundmusic).  MEDIA_OTHER means "leave it in the tab the user chose". */
typedef enum {
  MEDIA_MOVIE = 0,
  MEDIA_SUBTITLE,
  MEDIA_MUSIC,
  MEDIA_OTHER
} MediaKind;

MediaKind media_kind_for_name(const char *name);

// Log hook type (generator.c currently expects this name)
typedef void (*GeneratorLogHook)(const char *line);

// Optional alias if other files use a different typedef name
typedef GeneratorLogHook generator_log_hook_fn;

// Install/uninstall log hook (pass NULL to disable)
void generator_set_log_hook(GeneratorLogHook hook);

/* --------------------------- progress reporting --------------------------- */

typedef enum {
  GEN_STAGE_IDLE = 0,
  GEN_STAGE_SETUP,      /* checking tools / config, clearing clips */
  GEN_STAGE_SUBTITLES,  /* finding or downloading the SRT */
  GEN_STAGE_SCRIPT,     /* optional IMSDb script context */
  GEN_STAGE_PLANNING,   /* waiting for the OpenAI clip plan */
  GEN_STAGE_TTS,        /* ElevenLabs narration for one clip */
  GEN_STAGE_CLIP,       /* ffmpeg: cut + time-stretch one clip */
  GEN_STAGE_CONCAT,     /* ffmpeg: join all clips */
  GEN_STAGE_BGM,        /* background music build + mix */
  GEN_STAGE_VERTICAL,   /* 9:16 render */
  GEN_STAGE_DONE,
  GEN_STAGE_FAILED,
  GEN_STAGE_CANCELLED
} GeneratorStage;

typedef struct {
  int  stage;             /* GeneratorStage */
  int  movie_index;       /* 1-based, 0 when unknown */
  int  movie_total;
  int  clip_index;        /* 1-based, 0 when not working on clips */
  int  clip_total;
  char movie_title[256];
} GeneratorProgress;

typedef void (*GeneratorProgressHook)(const GeneratorProgress *p);

/* Install/uninstall progress hook (pass NULL to disable).
   Called from the generation thread; copy the struct if you keep it. */
void generator_set_progress_hook(GeneratorProgressHook hook);

/* Human readable name of a GeneratorStage value (never NULL). */
const char *generator_stage_name(int stage);

/* ------------------------------- cancelling ------------------------------- */

/* Ask the running generation to stop at the next step boundary
   (between clips / movies; a running ffmpeg call is not killed). */
void generator_request_cancel(void);
void generator_clear_cancel(void);
bool generator_cancel_requested(void);

// Core generation entrypoint
int run_generation(void);

// Back-compat: older UI code calls generator_run()
#ifndef generator_run
  #define generator_run() run_generation()
#endif

#ifdef __cplusplus
}
#endif
