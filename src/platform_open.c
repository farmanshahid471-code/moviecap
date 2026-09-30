/* platform_open.c - kept for compatibility; forwards to platform.c
 * (Windows: ShellExecuteW -> Explorer, macOS: open, Linux: xdg-open). */
#include "platform_open.h"
#include "platform.h"

void platform_open_folder(const char *path) {
  plat_open_folder(path);
}
