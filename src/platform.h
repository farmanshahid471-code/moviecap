#pragma once
/*
 * platform.h - thin OS abstraction layer.
 *
 * Windows is the primary target. Every function takes / returns UTF-8 strings
 * and on Windows is implemented with the wide-char (W) Win32 APIs, so movie
 * titles with accents, CJK characters, etc. work correctly.
 *
 * IMPORTANT: this header must NOT include <windows.h>. raylib.h and windows.h
 * clash (CloseWindow, DrawText, Rectangle, LoadImage, PlaySound, ...), so all
 * Win32 code is kept inside platform.c.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- console ---------- */
void plat_console_init(void); /* UTF-8 console output on Windows */

/* ---------- filesystem (UTF-8 paths, '/' or '\\' separators) ---------- */
typedef enum { PLAT_NONE = 0, PLAT_FILE = 1, PLAT_DIR = 2 } PlatKind;

FILE    *plat_fopen(const char *path, const char *mode);
PlatKind plat_stat(const char *path, long long *size_out);
int      plat_mkdir(const char *path);           /* 0 on success or already exists */
int      plat_unlink(const char *path);          /* 0 on success */
int      plat_rmdir(const char *path);           /* 0 on success */
int      plat_rename(const char *from, const char *to); /* replaces existing target; 0 on success */

typedef struct PlatDir PlatDir;
PlatDir    *plat_opendir(const char *path);
/* Returns next entry name (UTF-8, valid until next call), skipping "." and "..". */
const char *plat_readdir(PlatDir *d, bool *is_dir_out);
void        plat_closedir(PlatDir *d);

/* Absolute path of the current working directory (UTF-8). */
bool plat_getcwd(char *out, size_t outsz);

/* Change the working directory (UTF-8); returns true on success. */
bool plat_chdir(const char *path);

/*
 * If the working directory does not look like the project root
 * (contains resources/Inter-Regular.ttf or config.json), look next to the
 * executable and up to 4 parent directories, and chdir there.
 * Lets you double-click build\Release\movie_summary_bot.exe in Explorer.
 */
bool plat_enter_project_dir(void);

/* ---------- processes (no shell involved on Windows) ---------- */

/* Quote a single argument for a command line (caller frees). */
char *plat_quote_arg(const char *s);

/* Line callback for child output. */
typedef void (*PlatLineFn)(const char *line, void *user);

/*
 * Run a command line, wait for it, return its exit code (-1 if it could not be
 * started). stdout+stderr of the child are streamed line by line to `on_line`
 * (may be NULL). stdin is the null device.
 */
int plat_run(const char *cmdline, PlatLineFn on_line, void *user);

/* Run a command line and return everything it wrote to stdout (caller frees). */
char *plat_capture(const char *cmdline);

/* True if `exe` (e.g. "ffmpeg") can be started from PATH. */
bool plat_have_tool(const char *exe);

void plat_sleep_ms(int ms);
bool plat_spawn_detached(const char *cmdline); /* fire-and-forget child, no console */

/* ---------- shell ---------- */
void plat_open_folder(const char *path); /* Explorer / Finder / xdg-open */

/* ---------- threads & locks ---------- */
typedef void (*PlatThreadFn)(void *arg);
bool plat_thread_start_detached(PlatThreadFn fn, void *arg);

typedef struct PlatMutex PlatMutex;
PlatMutex *plat_mutex_create(void);
void       plat_mutex_lock(PlatMutex *m);
void       plat_mutex_unlock(PlatMutex *m);

#ifdef __cplusplus
}
#endif
