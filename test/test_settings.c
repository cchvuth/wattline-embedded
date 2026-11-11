// app/settings.c against a scratch folder standing in for /data: defaults, the two files (format, modes, atomic),
// a "" pass keeping the one on file, S3XY_OBD never written, the commands run on a change only, memory-only without it.
#include "../app/settings.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int pass, fail;
#define CHECK(c) do { if (c) pass++; else { fail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
static char logbuf[4096];
void log_msg(const char *fmt, ...) {   // kept, to prove no passphrase reaches the log
  va_list a;
  va_start(a, fmt);
  size_t n = strlen(logbuf);
  vsnprintf(logbuf + n, sizeof logbuf - n, fmt, a);
  va_end(a);
  strncat(logbuf, "\n", sizeof logbuf - strlen(logbuf) - 1);
}

static char root[128], ran[160];
static const char *slurp(const char *name) {
  static char b[1024];
  char p[256];
  snprintf(p, sizeof p, "%s/%s", root, name);
  FILE *f = fopen(p, "r");
  size_t n = f ? fread(b, 1, sizeof b - 1, f) : 0;
  b[n] = 0;
  if (f) fclose(f);
  return f ? b : NULL;
}
static int mode_of(const char *name) {
  char p[256];
  struct stat st;
  snprintf(p, sizeof p, "%s/%s", root, name);
  return stat(p, &st) ? -1 : (int)(st.st_mode & 0777);
}
static int runs(int want) {   // lines the fake commands appended (they run detached: wait up to 1 s for [want])
  int n = 0;
  for (int i = 0; i < 100; i++) {
    FILE *f = fopen(ran, "r");
    int c;
    n = 0;
    if (f) { while ((c = fgetc(f)) != EOF) n += c == '\n'; fclose(f); }
    if (n >= want) break;
    usleep(10000);
  }
  return n;
}

int main(void) {
  snprintf(root, sizeof root, "build/settings_test");
  system("rm -rf build/settings_test && mkdir -p build/settings_test");
  char cmd[160];
  snprintf(ran, sizeof ran, "%s/ran", root);
  snprintf(cmd, sizeof cmd, "%s/cmd.sh", root);
  FILE *f = fopen(cmd, "w");
  fprintf(f, "#!/bin/sh\necho \"$0 $1\" >> %s\n", ran);
  fclose(f);
  chmod(cmd, 0755);
  settings_root = root;
  settings_wifi_cmd = settings_ssh_on_cmd = settings_ssh_off_cmd = cmd;

  conf_t cf;
  conf_defaults(&cf);
  cf.hour12 = 0, cf.bright_follow_car = 1;
  dash_settings_t st;
  settings_load(&st, &cf);   // nothing saved: dash.conf's
  CHECK(st.minor_figures == 1 && st.hour12 == 0 && st.follow_car && st.brightness_pct == 80 && st.net_count == 0);

  st.hour12 = true, st.follow_car = false, st.brightness_pct = 40;
  st.net_count = 3;
  snprintf(st.net[0].ssid, 33, "Home"), snprintf(st.net[0].pass, 64, "s3cret pass");
  snprintf(st.net[1].ssid, 33, "S3XY_OBD"), snprintf(st.net[1].pass, 64, "12345678");
  snprintf(st.net[2].ssid, 33, "Cafe");   // open
  CHECK(settings_save(&st) == 0);
  CHECK(!strcmp(slurp("settings.conf"), "minor_figures = 1\nclock_24h = 0\nbrightness_pct = 40\nfollow_car = 0\nssh = 0\n"));
  CHECK(!strcmp(slurp("wifi/networks.conf"), "Home\ns3cret pass\nCafe\n\n"));   // S3XY_OBD left out
  CHECK(mode_of("wifi") == 0700 && mode_of("wifi/networks.conf") == 0600);
  CHECK(access("build/settings_test/settings.conf.tmp", F_OK) != 0);
  CHECK(!st.net[0].pass[0]);   // the typed pass is gone from memory the UI sees
  CHECK(runs(1) == 1);          // the WiFi list changed: S45wifi-auto start
  CHECK(!strstr(logbuf, "s3cret") && !strstr(logbuf, "Home"));

  dash_settings_t back;
  settings_load(&back, &cf);
  CHECK(back.hour12 && !back.follow_car && back.brightness_pct == 40 && back.net_count == 2);
  CHECK(!strcmp(back.net[0].ssid, "Home") && !back.net[0].pass[0] && !strcmp(back.net[1].ssid, "Cafe"));

  back.dev_ssh = true;          // only SSH changes: the list is kept as it was (its "" passes keep the file's)
  CHECK(settings_save(&back) == 0);
  CHECK(!strcmp(slurp("wifi/networks.conf"), "Home\ns3cret pass\nCafe\n\n"));
  CHECK(runs(2) == 2 && strstr(slurp("settings.conf"), "ssh = 1"));
  CHECK(settings_save(&back) == 0);   // nothing changed: nothing run
  usleep(200000);
  CHECK(runs(2) == 2);

  back.net[0] = back.net[1];    // reordered and one dropped: the file follows, in order
  back.net_count = 1;
  settings_save(&back);
  CHECK(!strcmp(slurp("wifi/networks.conf"), "Cafe\n\n") && runs(3) == 3);

  settings_root = NULL;         // no /data (the Mac): kept in memory, nothing written or run
  system("rm -rf build/settings_test/*");
  settings_load(&st, &cf);
  st.brightness_pct = 55;
  CHECK(settings_save(&st) == 0 && slurp("settings.conf") == NULL);
  system("rm -rf build/settings_test");
  printf("test_settings: %d/%d checks pass\n", pass, pass + fail);
  return fail != 0;
}
