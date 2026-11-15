// A small rotating log in RAM (/run): path and path.1, each up to max_kb. No Diagnostics screen on the Dash.
#include "app.h"
#include <stdarg.h>
#include <fcntl.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

static FILE *f;
static char path[160];
static long max_bytes;

void log_open(const char *p, int max_kb) {
  snprintf(path, sizeof path, "%s", p);
  max_bytes = (long)max_kb * 1024;
  if (f) fclose(f);
  f = path[0] ? fopen(path, "a") : NULL;
  if (f) setvbuf(f, NULL, _IOLBF, 0), fcntl(fileno(f), F_SETFD, FD_CLOEXEC);   // not held open by the scripts it starts
}

void log_msg(const char *fmt, ...) {
  char line[512];
  int64_t ms = mono_ms();
  int n = snprintf(line, sizeof line, "[%lld.%03lld] ", (long long)(ms / 1000), (long long)(ms % 1000));
  struct timespec rt;   // the wall clock once 0x318 has set it (else 1970), for reading logs side by side
  clock_gettime(CLOCK_REALTIME, &rt);
  if (rt.tv_sec > 1700000000) {
    struct tm tm;
    gmtime_r(&rt.tv_sec, &tm);
    n += (int)strftime(line + n, sizeof line - n, "%Y-%m-%dT%H:%M:%SZ ", &tm);
  }
  va_list a;
  va_start(a, fmt);
  vsnprintf(line + n, sizeof line - n, fmt, a);
  va_end(a);
  if (!f || isatty(2)) fprintf(stderr, "%s\n", line);
  if (!f) return;
  fprintf(f, "%s\n", line);
  if (ftell(f) > max_bytes) {
    char old[170];
    snprintf(old, sizeof old, "%s.1", path);
    fclose(f);
    rename(path, old);
    f = fopen(path, "w");
    if (f) setvbuf(f, NULL, _IOLBF, 0), fcntl(fileno(f), F_SETFD, FD_CLOEXEC);
  }
}
