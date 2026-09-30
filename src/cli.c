/* cli.c - headless version (movie_summary_cli.exe): runs one generation pass. */
#include "generator.h"
#include "platform.h"

int main(void) {
  plat_console_init();       /* UTF-8 output in cmd.exe / PowerShell */
  plat_enter_project_dir();  /* find config.json even if started from build\ */
  int rc = run_generation();
  return rc < 0 ? 1 : 0;
}
