// app/settings.c against a scratch folder standing in for /data: defaults, the two files (format, modes, atomic),
// a "" pass keeping the one on file, the car's adapter as the first boot's (editable) list, the commands run on a change only, memory-only without it.
#include "../app/settings.h"
#include <math.h>
#include <stdarg.h>
#include <time.h>
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
  CHECK(st.minor_figures == 1 && st.hour12 == 0 && st.follow_car && st.brightness_pct == 80 && st.net_count == 1);
  CHECK(!strcmp(st.net[0].ssid, "S3XY_OBD") && !st.net[0].pass[0]);   // the car's, its passphrase not shown
  CHECK(!settings_car_saved && !strcmp(st.tz, "UTC0") && !strcmp(st.panda_host, "192.168.4.1") && st.panda_port == 1338);

  CHECK(!st.record);         // recording off unless switched on
  CHECK(!st.ota_auto);       // automatic updates off unless switched on
  st.hour12 = true, st.follow_car = false, st.brightness_pct = 40, st.record = true;
  st.net_count = 3;
  snprintf(st.net[1].ssid, 33, "Home"), snprintf(st.net[1].pass, 64, "s3cret pass");   // [0]: the car's, "" keeps its pass
  snprintf(st.net[2].ssid, 33, "Cafe");   // open
  CHECK(settings_save(&st) == 0);
  CHECK(!strcmp(slurp("settings.conf"), "minor_figures = 1\nclock_24h = 0\nbrightness_pct = 40\nfollow_car = 0\nfollow_offset = 0\nssh = 0\nrecord = 1\n"
                                          "car = modelY7\nsparse = 1\nwifi_first = 1\ntz = UTC0\npanda_host = 192.168.4.1\n"
                                          "panda_port = 1338\nota_auto = 0\naa_low_res = 0\n"));
  CHECK(settings_car_saved);   // written: no first-boot question next time
  CHECK(!strcmp(slurp("wifi/networks.conf"), "S3XY_OBD\n12345678\nHome\ns3cret pass\nCafe\n\n"));
  CHECK(mode_of("wifi") == 0700 && mode_of("wifi/networks.conf") == 0600);
  CHECK(access("build/settings_test/settings.conf.tmp", F_OK) != 0);
  CHECK(!st.net[1].pass[0]);   // the typed pass is gone from memory the UI sees
  CHECK(runs(1) == 1);          // the WiFi list changed: S45wifi-auto start
  CHECK(!strstr(logbuf, "s3cret") && !strstr(logbuf, "Home"));

  dash_settings_t back;
  settings_load(&back, &cf);
  CHECK(back.hour12 && !back.follow_car && back.brightness_pct == 40 && back.net_count == 3 && back.record);
  CHECK(back.car == CAR_Y7 && back.sparse && back.wifi_first);   // dash.conf's defaults, kept
  back.car = CAR_MODEL3_RWD, back.sparse = false, back.wifi_first = false;   // the picker and the two switches
  back.ota_auto = true;                                                      // and automatic updates
  settings_save(&back);
  CHECK(strstr(slurp("settings.conf"), "\nota_auto = 1\n") != NULL);   // S99ota greps this exact form
  { dash_settings_t again; settings_load(&again, &cf); CHECK(again.ota_auto); }
  back.aa_low_res = true;                                                    // Android Auto's lower resolution
  CHECK(settings_save(&back) == 0);
  CHECK(strstr(slurp("settings.conf"), "\naa_low_res = 1\n") != NULL);   // aa reads this exact form
  { dash_settings_t again; settings_load(&again, &cf); CHECK(again.aa_low_res); }
  CHECK(!settings_aa_kept());                                                // Android Auto: off until dash-aa says up
  fclose(fopen("build/settings_test/aa-on", "w"));                           // dash-aa start, once up
  CHECK(settings_aa_kept());                                                 // left on: up again at the next start
  CHECK(settings_save(&back) == 0 && settings_aa_kept());                    // the screen's save does not undo it
  remove("build/settings_test/aa-on");                                       // dash-aa stop
  CHECK(!settings_aa_kept());
  CHECK(strstr(slurp("settings.conf"), "car = model3rwd\nsparse = 0\nwifi_first = 0\n") != NULL);
  settings_load(&back, &cf);
  CHECK(back.car == CAR_MODEL3_RWD && !back.sparse && !back.wifi_first && settings_car_saved);
  snprintf(back.tz, sizeof back.tz, "CET-1CEST,M3.5.0,M10.5.0/3");   // the time zone and the Panda's address
  snprintf(back.panda_host, sizeof back.panda_host, "10.0.0.7"), back.panda_port = 2000;
  settings_save(&back);
  settings_load(&back, &cf);
  CHECK(!strcmp(back.tz, "CET-1CEST,M3.5.0,M10.5.0/3") && !strcmp(back.panda_host, "10.0.0.7") && back.panda_port == 2000);
  CHECK(!strcmp(back.net[1].ssid, "Home") && !back.net[1].pass[0] && !strcmp(back.net[2].ssid, "Cafe"));

  back.dev_ssh = true;          // only SSH changes: the list is kept as it was (its "" passes keep the file's)
  CHECK(settings_save(&back) == 0);
  CHECK(!strcmp(slurp("wifi/networks.conf"), "S3XY_OBD\n12345678\nHome\ns3cret pass\nCafe\n\n"));
  CHECK(runs(2) == 2 && strstr(slurp("settings.conf"), "ssh = 1"));
  CHECK(settings_save(&back) == 0);   // nothing changed: nothing run
  usleep(200000);
  CHECK(runs(2) == 2);

  snprintf(back.net[0].pass, 64, "carpass99");   // the car adapter's own passphrase, typed
  settings_save(&back);
  CHECK(!strcmp(slurp("wifi/networks.conf"), "S3XY_OBD\ncarpass99\nHome\ns3cret pass\nCafe\n\n") && runs(3) == 3);

  back.net[0] = back.net[2];    // reordered and two dropped (the car's too): the file follows, in order
  back.net_count = 1;
  settings_save(&back);
  CHECK(!strcmp(slurp("wifi/networks.conf"), "Cafe\n\n") && runs(4) == 4);
  settings_load(&back, &cf);    // a saved list without the car's stays without it
  CHECK(back.net_count == 1 && !strcmp(back.net[0].ssid, "Cafe"));
  back.net_count = 2;           // the car's re-added: it lands last on screen, but is saved first (joined first)
  snprintf(back.net[1].ssid, 33, "S3XY_OBD"), snprintf(back.net[1].pass, 64, "carpass77");
  settings_save(&back);
  CHECK(!strcmp(slurp("wifi/networks.conf"), "S3XY_OBD\ncarpass77\nCafe\n\n") && runs(5) == 5);
  settings_load(&back, &cf);
  CHECK(back.net_count == 2 && !strcmp(back.net[0].ssid, "S3XY_OBD") && !strcmp(back.net[1].ssid, "Cafe"));

  // SSH running from the card's key while settings.conf says off: the switch loads on, and off runs the off command
  back.dev_ssh = false;
  settings_save(&back);
  CHECK(runs(6) == 6 && strstr(slurp("settings.conf"), "ssh = 0"));
  char pidf[160];
  snprintf(pidf, sizeof pidf, "%s/dropbear.pid", root);
  f = fopen(pidf, "w"); fprintf(f, "%d\n", (int)getpid()); fclose(f);
  settings_ssh_pid = pidf;
  settings_load(&back, &cf);
  CHECK(back.dev_ssh);
  back.dev_ssh = false;
  settings_save(&back);
  CHECK(runs(7) == 7);
  f = fopen(pidf, "w"); fprintf(f, "999999\n"); fclose(f);   // a stale pid file: off
  settings_load(&back, &cf);
  CHECK(!back.dev_ssh);
  settings_ssh_pid = NULL;

  // a list saved by the image before (never held the car's, no car-removed mark): the car's comes back, first
  FILE *o = fopen("build/settings_test/wifi/networks.conf", "w");
  fputs("Home\nhomepass1\n", o), fclose(o);
  unlink("build/settings_test/wifi/car-removed");
  settings_load(&back, &cf);
  CHECK(back.net_count == 2 && !strcmp(back.net[0].ssid, "S3XY_OBD") && !strcmp(back.net[1].ssid, "Home"));
  back.net[0] = back.net[1], back.net_count = 1;   // the car's deleted on purpose: the mark, and it stays away
  settings_save(&back);
  CHECK(access("build/settings_test/wifi/car-removed", F_OK) == 0);
  settings_load(&back, &cf);
  CHECK(back.net_count == 1 && !strcmp(back.net[0].ssid, "Home"));

  // tz: applied at once, a change only
  struct tm tm;
  time_t t0 = 0;
  CHECK(settings_apply_tz("JST-9") && localtime_r(&t0, &tm) && tm.tm_hour == 9 && tm.tm_min == 0);
  CHECK(!settings_apply_tz("JST-9"));
  CHECK(settings_apply_tz("IST-5:30") && localtime_r(&t0, &tm) && tm.tm_hour == 5 && tm.tm_min == 30);
  time_t july = 15552000;   // 1970-06-29: summer time in Central Europe, UTC+2
  CHECK(settings_apply_tz("CET-1CEST,M3.5.0,M10.5.0/3") && localtime_r(&july, &tm) && tm.tm_hour == 2);

  // tyres: none at first; kept when changed, once a minute at most, and at stop; back at the next start
  double tp[4], v[4] = {2.9, 2.875, NAN, 3.0};
  settings_tyres_load(tp);
  CHECK(isnan(tp[0]) && isnan(tp[3]) && slurp("tyres") == NULL);
  settings_tyres_keep(tp, 1000, false);   // the same: nothing written
  CHECK(slurp("tyres") == NULL);
  settings_tyres_keep(v, 1000, false);
  CHECK(slurp("tyres") && !strcmp(slurp("tyres"), "2.9000 2.8750 nan 3.0000\n"));
  v[0] = 2.8;
  settings_tyres_keep(v, 30000, false);   // within the minute: not yet
  CHECK(!strncmp(slurp("tyres"), "2.9000", 6));
  settings_tyres_keep(v, 61001, false);
  CHECK(!strncmp(slurp("tyres"), "2.8000", 6));
  v[1] = 2.7;
  settings_tyres_keep(v, 62000, true);    // stopping: at once
  CHECK(!strncmp(slurp("tyres"), "2.8000 2.7000", 13));
  settings_tyres_load(tp);
  CHECK(tp[0] == 2.8 && tp[1] == 2.7 && isnan(tp[2]) && tp[3] == 3.0);

  settings_root = NULL;         // no /data (the Mac): kept in memory, nothing written or run
  system("rm -rf build/settings_test/*");
  settings_load(&st, &cf);
  st.brightness_pct = 55;
  CHECK(settings_save(&st) == 0 && slurp("settings.conf") == NULL);
  system("rm -rf build/settings_test");
  printf("test_settings: %d/%d checks pass\n", pass, pass + fail);
  return fail != 0;
}
