/*
 * platform.c - OS abstraction. Windows (Win32 wide APIs) first, POSIX fallback
 * kept so the code base still builds on macOS/Linux.
 */
#if !defined(_WIN32)
  #define _POSIX_C_SOURCE 200809L
  #define _DEFAULT_SOURCE
#endif

#include "platform.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* ======================================================================== */
/*                                 WINDOWS                                  */
/* ======================================================================== */
#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <direct.h>
#include <io.h>
#include <wchar.h>
#include <fcntl.h>

/* ---------- UTF-8 <-> UTF-16 ---------- */
static wchar_t *u8_to_w(const char *s) {
  if (!s) s = "";
  int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
  if (n <= 0) {
    /* Not valid UTF-8 (e.g. legacy ANSI text) -> fall back to the ANSI code page. */
    n = MultiByteToWideChar(CP_ACP, 0, s, -1, NULL, 0);
    if (n <= 0) return NULL;
    wchar_t *w = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
    if (w) MultiByteToWideChar(CP_ACP, 0, s, -1, w, n);
    return w;
  }
  wchar_t *w = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
  if (w) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
  return w;
}

static char *w_to_u8(const wchar_t *w) {
  if (!w) w = L"";
  int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
  if (n <= 0) return NULL;
  char *s = (char *)malloc((size_t)n);
  if (s) WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
  return s;
}

/* Wide path with '/' converted to '\\' (Explorer and some APIs prefer it). */
static wchar_t *path_w(const char *p) {
  wchar_t *w = u8_to_w(p);
  if (!w) return NULL;
  for (wchar_t *q = w; *q; q++) if (*q == L'/') *q = L'\\';
  return w;
}

void plat_console_init(void) {
  SetConsoleOutputCP(CP_UTF8);
  SetConsoleCP(CP_UTF8);
}

FILE *plat_fopen(const char *path, const char *mode) {
  wchar_t *wp = path_w(path);
  wchar_t *wm = u8_to_w(mode);
  FILE *f = NULL;
  if (wp && wm) f = _wfopen(wp, wm);
  free(wp);
  free(wm);
  return f;
}

PlatKind plat_stat(const char *path, long long *size_out) {
  if (size_out) *size_out = -1;
  wchar_t *wp = path_w(path);
  if (!wp) return PLAT_NONE;
  WIN32_FILE_ATTRIBUTE_DATA fad;
  BOOL ok = GetFileAttributesExW(wp, GetFileExInfoStandard, &fad);
  free(wp);
  if (!ok) return PLAT_NONE;
  if (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return PLAT_DIR;
  if (size_out) {
    ULARGE_INTEGER u;
    u.LowPart = fad.nFileSizeLow;
    u.HighPart = fad.nFileSizeHigh;
    *size_out = (long long)u.QuadPart;
  }
  return PLAT_FILE;
}

int plat_mkdir(const char *path) {
  wchar_t *wp = path_w(path);
  if (!wp) return -1;
  BOOL ok = CreateDirectoryW(wp, NULL);
  DWORD err = ok ? 0 : GetLastError();
  free(wp);
  if (ok || err == ERROR_ALREADY_EXISTS) return 0;
  errno = (err == ERROR_PATH_NOT_FOUND) ? ENOENT : EACCES;
  return -1;
}

int plat_unlink(const char *path) {
  wchar_t *wp = path_w(path);
  if (!wp) return -1;
  BOOL ok = DeleteFileW(wp);
  if (!ok && GetLastError() == ERROR_ACCESS_DENIED) {
    /* read-only file: clear the attribute and retry */
    SetFileAttributesW(wp, FILE_ATTRIBUTE_NORMAL);
    ok = DeleteFileW(wp);
  }
  free(wp);
  return ok ? 0 : -1;
}

int plat_rmdir(const char *path) {
  wchar_t *wp = path_w(path);
  if (!wp) return -1;
  BOOL ok = RemoveDirectoryW(wp);
  free(wp);
  return ok ? 0 : -1;
}

int plat_rename(const char *from, const char *to) {
  wchar_t *a = path_w(from);
  wchar_t *b = path_w(to);
  BOOL ok = FALSE;
  if (a && b) ok = MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED);
  free(a);
  free(b);
  return ok ? 0 : -1;
}

struct PlatDir {
  HANDLE h;
  WIN32_FIND_DATAW fd;
  int first;
  char *name; /* current entry, UTF-8 */
};

PlatDir *plat_opendir(const char *path) {
  if (!path || !path[0]) return NULL;
  size_t n = strlen(path);
  char *pat = (char *)malloc(n + 3);
  if (!pat) return NULL;
  memcpy(pat, path, n);
  if (n > 0 && path[n - 1] != '/' && path[n - 1] != '\\') pat[n++] = '\\';
  pat[n++] = '*';
  pat[n] = 0;
  wchar_t *wp = path_w(pat);
  free(pat);
  if (!wp) return NULL;

  PlatDir *d = (PlatDir *)calloc(1, sizeof(PlatDir));
  if (!d) { free(wp); return NULL; }
  d->h = FindFirstFileW(wp, &d->fd);
  free(wp);
  if (d->h == INVALID_HANDLE_VALUE) { free(d); return NULL; }
  d->first = 1;
  return d;
}

const char *plat_readdir(PlatDir *d, bool *is_dir_out) {
  if (!d) return NULL;
  for (;;) {
    if (d->first) d->first = 0;
    else if (!FindNextFileW(d->h, &d->fd)) return NULL;

    if (wcscmp(d->fd.cFileName, L".") == 0 || wcscmp(d->fd.cFileName, L"..") == 0) continue;

    free(d->name);
    d->name = w_to_u8(d->fd.cFileName);
    if (!d->name) continue;
    if (is_dir_out) *is_dir_out = (d->fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    return d->name;
  }
}

void plat_closedir(PlatDir *d) {
  if (!d) return;
  if (d->h != INVALID_HANDLE_VALUE) FindClose(d->h);
  free(d->name);
  free(d);
}

bool plat_getcwd(char *out, size_t outsz) {
  if (!out || outsz == 0) return false;
  wchar_t buf[4096];
  DWORD n = GetCurrentDirectoryW(4096, buf);
  if (n == 0 || n >= 4096) { snprintf(out, outsz, "."); return false; }
  char *u = w_to_u8(buf);
  if (!u) { snprintf(out, outsz, "."); return false; }
  snprintf(out, outsz, "%s", u);
  free(u);
  return true;
}

static bool looks_like_project_root_w(const wchar_t *dir) {
  static const wchar_t *markers[] = { L"resources\\Inter-Regular.ttf", L"config.json", NULL };
  for (int i = 0; markers[i]; i++) {
    wchar_t p[4096];
    _snwprintf(p, 4096, L"%ls\\%ls", dir, markers[i]);
    p[4095] = 0;
    DWORD a = GetFileAttributesW(p);
    if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY)) return true;
  }
  return false;
}

/*
 * run.bat / install_tools.ps1 install a portable FFmpeg (by default under
 * F:\AI-Movie-Shorts\tools) instead of touching the system, so those binaries
 * are not on the system PATH. Find them and put the folder at the front of this
 * process' PATH - child processes inherit it, and C: stays untouched.
 */
static void plat_use_portable_tools(void) {
  wchar_t cwd[4096];
  DWORD cn = GetCurrentDirectoryW(4096, cwd);
  if (cn == 0 || cn >= 4096) cwd[0] = 0;

  wchar_t envtools[4096] = L"";
  GetEnvironmentVariableW(L"MOVIECAP_TOOLS", envtools, 4096);

  wchar_t b_env[4200], b_cwd_tools[4200], b_cwd_ff[4200];
  _snwprintf(b_env, 4200, L"%ls\\ffmpeg\\bin", envtools);        b_env[4199] = 0;
  _snwprintf(b_cwd_tools, 4200, L"%ls\\tools\\ffmpeg\\bin", cwd); b_cwd_tools[4199] = 0;
  _snwprintf(b_cwd_ff, 4200, L"%ls\\ffmpeg\\bin", cwd);        b_cwd_ff[4199] = 0;

  const wchar_t *cands[4];
  cands[0] = envtools[0] ? b_env : NULL;
  cands[1] = L"F:\\AI-Movie-Shorts\\tools\\ffmpeg\\bin";
  cands[2] = cwd[0] ? b_cwd_tools : NULL;
  cands[3] = cwd[0] ? b_cwd_ff : NULL;

  for (int i = 0; i < 4; i++) {
    if (!cands[i] || !cands[i][0]) continue;

    wchar_t ff[4300], fp[4300];
    _snwprintf(ff, 4300, L"%ls\\ffmpeg.exe", cands[i]);  ff[4299] = 0;
    _snwprintf(fp, 4300, L"%ls\\ffprobe.exe", cands[i]); fp[4299] = 0;
    if (GetFileAttributesW(ff) == INVALID_FILE_ATTRIBUTES) continue;
    if (GetFileAttributesW(fp) == INVALID_FILE_ATTRIBUTES) continue;

    DWORD need = GetEnvironmentVariableW(L"PATH", NULL, 0); /* size incl. NUL */
    size_t total = wcslen(cands[i]) + (need ? need + 1 : 0) + 2;
    wchar_t *np = (wchar_t *)malloc(total * sizeof(wchar_t));
    if (!np) return;
    if (need > 1) {
      wchar_t *cur = (wchar_t *)malloc(need * sizeof(wchar_t));
      if (!cur) { free(np); return; }
      GetEnvironmentVariableW(L"PATH", cur, need);
      _snwprintf(np, total, L"%ls;%ls", cands[i], cur);
      free(cur);
    } else {
      _snwprintf(np, total, L"%ls", cands[i]);
    }
    np[total - 1] = 0;

    SetEnvironmentVariableW(L"PATH", np); /* what CreateProcessW children inherit */
    _wputenv_s(L"PATH", np);              /* keep the CRT copy in sync as well   */
    free(np);
    return;
  }
}

bool plat_enter_project_dir(void) {
  bool ok = false;

  wchar_t cwd[4096];
  if (GetCurrentDirectoryW(4096, cwd) && looks_like_project_root_w(cwd)) {
    ok = true;
  } else {
    wchar_t exe[4096];
    DWORD n = GetModuleFileNameW(NULL, exe, 4096);
    if (n != 0 && n < 4096) {
      /* strip file name, then walk up */
      for (int level = 0; level <= 5; level++) {
        wchar_t *slash = wcsrchr(exe, L'\\');
        if (!slash) break;
        *slash = 0;
        if (looks_like_project_root_w(exe)) {
          ok = SetCurrentDirectoryW(exe) != 0;
          break;
        }
      }
    }
  }

  plat_use_portable_tools();
  return ok;
}

/* ---------- processes ---------- */

/* Quote per CommandLineToArgvW / MSVCRT rules. */
char *plat_quote_arg(const char *s) {
  if (!s) s = "";
  size_t n = strlen(s);
  char *out = (char *)malloc(n * 2 + 3);
  if (!out) return NULL;
  size_t j = 0;
  out[j++] = '"';
  size_t bs = 0;
  for (size_t i = 0; i < n; i++) {
    char c = s[i];
    if (c == '\\') { bs++; continue; }
    if (c == '"') {
      for (size_t k = 0; k < bs * 2 + 1; k++) out[j++] = '\\';
      out[j++] = '"';
    } else {
      for (size_t k = 0; k < bs; k++) out[j++] = '\\';
      out[j++] = c;
    }
    bs = 0;
  }
  for (size_t k = 0; k < bs * 2; k++) out[j++] = '\\';
  out[j++] = '"';
  out[j] = 0;
  return out;
}

typedef struct {
  char *buf;
  size_t len, cap;
} Acc;

static void acc_add(Acc *a, const char *p, size_t n) {
  if (a->len + n + 1 > a->cap) {
    size_t nc = a->cap ? a->cap * 2 : 8192;
    while (nc < a->len + n + 1) nc *= 2;
    char *q = (char *)realloc(a->buf, nc);
    if (!q) return;
    a->buf = q;
    a->cap = nc;
  }
  memcpy(a->buf + a->len, p, n);
  a->len += n;
  a->buf[a->len] = 0;
}

/*
 * Spawn `cmdline` with stdout (and optionally stderr) connected to a pipe.
 * mode 0: stdout+stderr -> pipe (run), mode 1: stdout -> pipe, stderr -> NUL (capture)
 */
static int spawn_and_read(const char *cmdline, int mode, PlatLineFn on_line, void *user, Acc *acc) {
  SECURITY_ATTRIBUTES sa;
  sa.nLength = sizeof(sa);
  sa.lpSecurityDescriptor = NULL;
  sa.bInheritHandle = TRUE;

  HANDLE rd = NULL, wr = NULL;
  if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
  SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

  HANDLE nul_in = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                              OPEN_EXISTING, 0, NULL);
  HANDLE nul_err = NULL;
  if (mode == 1) {
    nul_err = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                          OPEN_EXISTING, 0, NULL);
  }

  STARTUPINFOW si;
  ZeroMemory(&si, sizeof(si));
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = nul_in;
  si.hStdOutput = wr;
  si.hStdError = (mode == 1) ? nul_err : wr;

  PROCESS_INFORMATION pi;
  ZeroMemory(&pi, sizeof(pi));

  wchar_t *wcmd = u8_to_w(cmdline);
  BOOL ok = FALSE;
  if (wcmd) {
    /* CREATE_NO_WINDOW: no console flashes for ffmpeg even in a GUI build */
    ok = CreateProcessW(NULL, wcmd, NULL, NULL, TRUE, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                        NULL, NULL, &si, &pi);
  }
  free(wcmd);

  CloseHandle(wr);
  if (nul_in && nul_in != INVALID_HANDLE_VALUE) CloseHandle(nul_in);
  if (nul_err && nul_err != INVALID_HANDLE_VALUE) CloseHandle(nul_err);

  if (!ok) {
    CloseHandle(rd);
    return -1;
  }

  char chunk[4096];
  char line[4096];
  size_t ll = 0;
  DWORD got = 0;
  while (ReadFile(rd, chunk, sizeof(chunk), &got, NULL) && got > 0) {
    if (acc) acc_add(acc, chunk, got);
    if (on_line) {
      for (DWORD i = 0; i < got; i++) {
        char c = chunk[i];
        if (c == '\r' || c == '\n') {
          if (ll > 0) { line[ll] = 0; on_line(line, user); ll = 0; }
        } else if (ll + 1 < sizeof(line)) {
          line[ll++] = c;
        }
      }
    }
  }
  if (on_line && ll > 0) { line[ll] = 0; on_line(line, user); }
  CloseHandle(rd);

  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  return (int)code;
}

int plat_run(const char *cmdline, PlatLineFn on_line, void *user) {
  return spawn_and_read(cmdline, 0, on_line, user, NULL);
}

char *plat_capture(const char *cmdline) {
  Acc a = {0};
  int rc = spawn_and_read(cmdline, 1, NULL, NULL, &a);
  if (rc == -1 && !a.buf) return NULL;
  if (!a.buf) {
    a.buf = (char *)malloc(1);
    if (a.buf) a.buf[0] = 0;
  }
  return a.buf;
}

bool plat_have_tool(const char *exe) {
  char cmd[512];
  snprintf(cmd, sizeof(cmd), "%s -version", exe);
  return spawn_and_read(cmd, 1, NULL, NULL, NULL) == 0;
}

/* ---------- shell ---------- */
void plat_open_folder(const char *path) {
  if (!path || !path[0]) return;
  plat_mkdir(path); /* make sure it exists so Explorer doesn't show an error */

  /* Build absolute path: <cwd>\<path> */
  wchar_t cwd[4096];
  wchar_t full[8192];
  wchar_t *wp = path_w(path);
  if (!wp) return;
  if ((wp[0] && wp[1] == L':') || (wp[0] == L'\\' && wp[1] == L'\\')) {
    _snwprintf(full, 8192, L"%ls", wp);
  } else if (GetCurrentDirectoryW(4096, cwd)) {
    _snwprintf(full, 8192, L"%ls\\%ls", cwd, wp);
  } else {
    _snwprintf(full, 8192, L"%ls", wp);
  }
  full[8191] = 0;
  free(wp);

  ShellExecuteW(NULL, L"open", full, NULL, NULL, SW_SHOWNORMAL);
}

/* ---------- threads & locks ---------- */
typedef struct {
  PlatThreadFn fn;
  void *arg;
} ThreadBox;

static DWORD WINAPI thread_trampoline(LPVOID p) {
  ThreadBox b = *(ThreadBox *)p;
  free(p);
  b.fn(b.arg);
  return 0;
}

bool plat_thread_start_detached(PlatThreadFn fn, void *arg) {
  ThreadBox *b = (ThreadBox *)malloc(sizeof(ThreadBox));
  if (!b) return false;
  b->fn = fn;
  b->arg = arg;
  /* 8 MB stack: the generator uses some large local buffers */
  HANDLE h = CreateThread(NULL, 8u * 1024u * 1024u, thread_trampoline, b,
                          STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
  if (!h) { free(b); return false; }
  CloseHandle(h);
  return true;
}

struct PlatMutex { CRITICAL_SECTION cs; };

PlatMutex *plat_mutex_create(void) {
  PlatMutex *m = (PlatMutex *)calloc(1, sizeof(PlatMutex));
  if (m) InitializeCriticalSection(&m->cs);
  return m;
}
void plat_mutex_lock(PlatMutex *m)   { if (m) EnterCriticalSection(&m->cs); }
void plat_mutex_unlock(PlatMutex *m) { if (m) LeaveCriticalSection(&m->cs); }

/* ======================================================================== */
/*                            POSIX (macOS / Linux)                         */
/* ======================================================================== */
#else

#include <dirent.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <limits.h>

void plat_console_init(void) {}

FILE *plat_fopen(const char *path, const char *mode) { return fopen(path, mode); }

PlatKind plat_stat(const char *path, long long *size_out) {
  if (size_out) *size_out = -1;
  struct stat st;
  if (stat(path, &st) != 0) return PLAT_NONE;
  if (S_ISDIR(st.st_mode)) return PLAT_DIR;
  if (size_out) *size_out = (long long)st.st_size;
  return S_ISREG(st.st_mode) ? PLAT_FILE : PLAT_NONE;
}

int plat_mkdir(const char *path) {
  if (mkdir(path, 0755) == 0 || errno == EEXIST) return 0;
  return -1;
}
int plat_unlink(const char *path) { return unlink(path); }
int plat_rmdir(const char *path) { return rmdir(path); }
int plat_rename(const char *from, const char *to) { return rename(from, to); }

struct PlatDir { DIR *d; };

PlatDir *plat_opendir(const char *path) {
  DIR *d = opendir(path);
  if (!d) return NULL;
  PlatDir *p = (PlatDir *)calloc(1, sizeof(PlatDir));
  if (!p) { closedir(d); return NULL; }
  p->d = d;
  return p;
}

const char *plat_readdir(PlatDir *p, bool *is_dir_out) {
  if (!p) return NULL;
  struct dirent *e;
  while ((e = readdir(p->d))) {
    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
    if (is_dir_out) {
      struct stat st;
      /* d_type is not portable; lstat relative names isn't possible without the dir path,
         callers use plat_stat() when they need certainty. */
      *is_dir_out = false;
#ifdef DT_DIR
      if (e->d_type == DT_DIR) *is_dir_out = true;
#endif
      (void)st;
    }
    return e->d_name;
  }
  return NULL;
}

void plat_closedir(PlatDir *p) {
  if (!p) return;
  closedir(p->d);
  free(p);
}

bool plat_getcwd(char *out, size_t outsz) {
  if (!out || outsz == 0) return false;
  if (!getcwd(out, outsz)) { snprintf(out, outsz, "."); return false; }
  return true;
}

/* Same idea as the Windows version: prefer a bundled/portable FFmpeg that was
 * installed next to the project over whatever happens to be on PATH. */
static void plat_use_portable_tools(void) {
  const char *envtools = getenv("MOVIECAP_TOOLS");

  char b_env[PATH_MAX + 64], b_tools[PATH_MAX + 64], b_ff[PATH_MAX + 64];
  if (envtools && envtools[0]) snprintf(b_env, sizeof(b_env), "%s/ffmpeg/bin", envtools);
  else b_env[0] = 0;
  snprintf(b_tools, sizeof(b_tools), "tools/ffmpeg/bin");
  snprintf(b_ff, sizeof(b_ff), "ffmpeg/bin");

  const char *cands[3] = { b_env, b_tools, b_ff };
  for (int i = 0; i < 3; i++) {
    if (!cands[i][0]) continue;
    char ff[PATH_MAX + 80], fp[PATH_MAX + 80];
    snprintf(ff, sizeof(ff), "%s/ffmpeg", cands[i]);
    snprintf(fp, sizeof(fp), "%s/ffprobe", cands[i]);
    if (plat_stat(ff, NULL) != PLAT_FILE || plat_stat(fp, NULL) != PLAT_FILE) continue;

    const char *cur = getenv("PATH");
    size_t need = strlen(cands[i]) + (cur ? strlen(cur) + 2 : 1);
    char *np = (char *)malloc(need);
    if (!np) return;
    if (cur && cur[0]) snprintf(np, need, "%s:%s", cands[i], cur);
    else snprintf(np, need, "%s", cands[i]);
    setenv("PATH", np, 1);
    free(np);
    return;
  }
}

bool plat_enter_project_dir(void) {
  bool ok = false;

  if (plat_stat("resources/Inter-Regular.ttf", NULL) == PLAT_FILE ||
      plat_stat("config.json", NULL) == PLAT_FILE) {
    ok = true;
  }
#if defined(__linux__)
  else {
    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0) {
      exe[n] = 0;
      for (int level = 0; level <= 5; level++) {
        char *slash = strrchr(exe, '/');
        if (!slash || slash == exe) break;
        *slash = 0;
        char p1[PATH_MAX + 64], p2[PATH_MAX + 64];
        snprintf(p1, sizeof(p1), "%s/resources/Inter-Regular.ttf", exe);
        snprintf(p2, sizeof(p2), "%s/config.json", exe);
        if (plat_stat(p1, NULL) == PLAT_FILE || plat_stat(p2, NULL) == PLAT_FILE) {
          ok = chdir(exe) == 0;
          break;
        }
      }
    }
  }
#endif

  plat_use_portable_tools();
  return ok;
}

char *plat_quote_arg(const char *s) {
  if (!s) s = "";
  size_t n = strlen(s);
  char *out = (char *)malloc(n * 4 + 3);
  if (!out) return NULL;
  size_t j = 0;
  out[j++] = '\'';
  for (size_t i = 0; i < n; i++) {
    if (s[i] == '\'') { memcpy(out + j, "'\\''", 4); j += 4; }
    else out[j++] = s[i];
  }
  out[j++] = '\'';
  out[j] = 0;
  return out;
}

int plat_run(const char *cmdline, PlatLineFn on_line, void *user) {
  size_t n = strlen(cmdline);
  char *full = (char *)malloc(n + 32);
  if (!full) return -1;
  snprintf(full, n + 32, "%s </dev/null 2>&1", cmdline);
  FILE *p = popen(full, "r");
  free(full);
  if (!p) return -1;
  char line[4096];
  while (fgets(line, sizeof(line), p)) {
    size_t l = strlen(line);
    while (l > 0 && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = 0;
    if (l > 0 && on_line) on_line(line, user);
  }
  int st = pclose(p);
  if (st == -1) return -1;
  if (WIFEXITED(st)) return WEXITSTATUS(st);
  return 1;
}

char *plat_capture(const char *cmdline) {
  size_t n = strlen(cmdline);
  char *full = (char *)malloc(n + 32);
  if (!full) return NULL;
  snprintf(full, n + 32, "%s 2>/dev/null", cmdline);
  FILE *p = popen(full, "r");
  free(full);
  if (!p) return NULL;
  char *buf = NULL;
  size_t len = 0, cap = 0;
  char tmp[4096];
  size_t got;
  while ((got = fread(tmp, 1, sizeof(tmp), p)) > 0) {
    if (len + got + 1 > cap) {
      cap = cap ? cap * 2 : 8192;
      while (cap < len + got + 1) cap *= 2;
      char *q = (char *)realloc(buf, cap);
      if (!q) break;
      buf = q;
    }
    memcpy(buf + len, tmp, got);
    len += got;
    buf[len] = 0;
  }
  pclose(p);
  if (!buf) { buf = (char *)malloc(1); if (buf) buf[0] = 0; }
  return buf;
}

bool plat_have_tool(const char *exe) {
  char cmd[512];
  snprintf(cmd, sizeof(cmd), "%s -version >/dev/null 2>&1", exe);
  return system(cmd) == 0;
}

void plat_open_folder(const char *path) {
  if (!path || !path[0]) return;
  plat_mkdir(path);
  char cwd[4096];
  plat_getcwd(cwd, sizeof(cwd));
  char full[8192];
  if (path[0] == '/') snprintf(full, sizeof(full), "%s", path);
  else snprintf(full, sizeof(full), "%s/%s", cwd, path);
  char *q = plat_quote_arg(full);
  if (!q) return;
  char cmd[9000];
#if defined(__APPLE__)
  snprintf(cmd, sizeof(cmd), "open %s", q);
#else
  snprintf(cmd, sizeof(cmd), "xdg-open %s >/dev/null 2>&1 &", q);
#endif
  free(q);
  (void)system(cmd);
}

typedef struct { PlatThreadFn fn; void *arg; } ThreadBox;

static void *thread_trampoline(void *p) {
  ThreadBox b = *(ThreadBox *)p;
  free(p);
  b.fn(b.arg);
  return NULL;
}

bool plat_thread_start_detached(PlatThreadFn fn, void *arg) {
  ThreadBox *b = (ThreadBox *)malloc(sizeof(ThreadBox));
  if (!b) return false;
  b->fn = fn;
  b->arg = arg;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 8u * 1024u * 1024u);
  pthread_t t;
  int rc = pthread_create(&t, &attr, thread_trampoline, b);
  pthread_attr_destroy(&attr);
  if (rc != 0) { free(b); return false; }
  pthread_detach(t);
  return true;
}

struct PlatMutex { pthread_mutex_t m; };

PlatMutex *plat_mutex_create(void) {
  PlatMutex *m = (PlatMutex *)calloc(1, sizeof(PlatMutex));
  if (m) pthread_mutex_init(&m->m, NULL);
  return m;
}
void plat_mutex_lock(PlatMutex *m)   { if (m) pthread_mutex_lock(&m->m); }
void plat_mutex_unlock(PlatMutex *m) { if (m) pthread_mutex_unlock(&m->m); }

#endif
