// See settings.h.
#include "settings.h"
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

const char *settings_root = "/data";
const char *settings_wifi_cmd = "/etc/init.d/S45wifi-auto";   // run with "start": joins from the new list
const char *settings_ssh_on_cmd = "/usr/sbin/dash-ssh-enable";
const char *settings_ssh_off_cmd = "/usr/sbin/dash-ssh-disable";
const char *settings_aa_cmd = "/usr/sbin/dash-aa";   // run with "start"/"stop": wireless Android Auto on demand
const char *settings_ssh_pid = "/var/run/dropbear.pid";

bool settings_car_saved;
static const int kCards[4] = {DASH_CARD_BUTTONS, DASH_CARD_CAR, DASH_CARD_ENERGY, DASH_CARD_G};   // full layout's
static dash_settings_t saved;                 // as last loaded or saved (passes "")
static char pass[DASH_NETS][64];              // saved.net[i]'s passphrase

static bool writable(void) { return settings_root && access(settings_root, W_OK) == 0; }

static void run(const char *cmd, const char *arg, const char *arg2) {
  if (!cmd) return;
  char *argv[] = {(char *)cmd, (char *)arg, (char *)(arg ? arg2 : NULL), NULL};
  pid_t pid;
  int e = posix_spawn(&pid, cmd, NULL, NULL, argv, environ);   // its error comes back, not in errno
  if (e) { log_msg("settings: %s: %s", cmd, strerror(e)); return; }
  log_msg("settings: ran %s%s%s%s", cmd, arg ? " " : "", arg ? arg : "", arg && arg2 ? " <network>" : "");   // no SSID in the log
  // reaped by the loop (dash.c)
}

void settings_aa(bool start) { run(settings_aa_cmd, start ? "start" : "stop", NULL); }
void settings_aa_restore(void) { run(settings_aa_cmd, "restore", NULL); }
void settings_aa_forget(void) { run(settings_aa_cmd, "forget", NULL); }
void settings_ota(const char *what) { run("/usr/sbin/dash-update", what, NULL); }
void settings_nor(const char *what) { run("/usr/sbin/dash-nor", what, NULL); }
// How the board restarts: the device profile's (device/<name>.mk; the F133 screen's power MCU cycles it, as a warm reboot
// loses its Wi-Fi chip)
#ifndef DASH_REBOOT
#define DASH_REBOOT "/sbin/reboot"
#endif
#ifndef DASH_REBOOT_ARG
#define DASH_REBOOT_ARG NULL
#endif
void settings_reboot(void) { run(DASH_REBOOT, DASH_REBOOT_ARG, NULL); }

// Whole file, atomically: <path>.tmp written and synced, renamed over, the folder synced.
static int put_sync(const char *dir, const char *name, const char *text, mode_t mode, bool sync);
static int put(const char *dir, const char *name, const char *text, mode_t mode) { return put_sync(dir, name, text, mode, true); }
bool settings_aa_kept(void) {
  char p[256];
  if (!settings_root) return false;
  snprintf(p, sizeof p, "%s/aa-on", settings_root);
  return access(p, F_OK) == 0;
}
static int put_sync(const char *dir, const char *name, const char *text, mode_t mode, bool sync) {
  char path[256], tmp[264];
  snprintf(path, sizeof path, "%s/%s", dir, name);
  snprintf(tmp, sizeof tmp, "%s.tmp", path);
  int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
  size_t n = strlen(text);
  bool ok = fd >= 0 && fchmod(fd, mode) == 0 && write(fd, text, n) == (ssize_t)n && (!sync || fsync(fd) == 0);
  if (fd >= 0) ok = close(fd) == 0 && ok;
  if (ok) ok = rename(tmp, path) == 0;
  if (!ok) { log_msg("settings: write %s: %s", path, strerror(errno)); unlink(tmp); return -1; }
  int d = sync ? open(dir, O_RDONLY | O_CLOEXEC) : -1;
  if (d >= 0) fsync(d), close(d);
  return 0;
}

// Dropbear running: its pid file names a live process. (The card's development key starts it whatever settings.conf says.)
static bool ssh_running(void) {
  FILE *f = settings_ssh_pid ? fopen(settings_ssh_pid, "r") : NULL;
  int pid = 0;
  if (f) { if (fscanf(f, "%d", &pid) != 1) pid = 0; fclose(f); }
  return pid > 0 && kill(pid, 0) == 0;
}

static char *trim(char *s) {
  while (*s == ' ' || *s == '\t') s++;
  char *e = s + strlen(s);
  while (e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
  return s;
}

void settings_refresh(dash_settings_t *st) {   // SSH turned on elsewhere (the pairing): the switch shows it, off works
  if (ssh_running()) st->dev_ssh = saved.dev_ssh = true;
}

void settings_load(dash_settings_t *st, const conf_t *cf) {
  memset(st, 0, sizeof *st);
  memset(pass, 0, sizeof pass);
  st->minor_figures = cf->minor_figures, st->hour12 = cf->hour12;
  st->brightness_pct = 80, st->follow_car = cf->bright_follow_car;
  st->follow_offset = 0;   // a user bias on top of dash.conf's bright_follow_offset
  st->car = cf->car, st->sparse = cf->sparse, st->wifi_first = true;   // dash.conf's, the phone app's defaults
  st->unit_miles = st->unit_f = st->unit_psi = -1;
  st->basic = false;   // the phone layout's (the F133's): its full layout
  memcpy(st->cards, kCards, sizeof kCards);
  st->split_card = DASH_CARD_ENERGY, st->ring_page = DASH_RING_POWER;   // its split's card (the battery's), the power ring
  snprintf(st->tz, sizeof st->tz, "%s", cf->tz);
  snprintf(st->tz_conf, sizeof st->tz_conf, "%s", cf->tz);
  snprintf(st->panda_host, sizeof st->panda_host, "%s", cf->panda_host);
  st->panda_port = cf->panda_port;
  settings_car_saved = false;
  char path[256], line[160];
  FILE *f;
  if (settings_root) {
    snprintf(path, sizeof path, "%s/settings.conf", settings_root);
    if ((f = fopen(path, "r"))) {
      while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = trim(line);
        char *val = trim(eq + 1);
        int v = atoi(val);
        if (!strcmp(k, "minor_figures")) st->minor_figures = v;
        else if (!strcmp(k, "clock_24h")) st->hour12 = !v;
        else if (!strcmp(k, "brightness_pct")) st->brightness_pct = v < 5 ? 5 : v > 100 ? 100 : v;
        else if (!strcmp(k, "follow_car")) st->follow_car = v;
        else if (!strcmp(k, "follow_offset")) st->follow_offset = v < -50 ? -50 : v > 50 ? 50 : v;
        else if (!strcmp(k, "ssh")) st->dev_ssh = v;
        else if (!strcmp(k, "sparse")) st->sparse = v;
        else if (!strcmp(k, "wifi_first")) st->wifi_first = v;
        else if (!strcmp(k, "ota_auto")) st->ota_auto = v;   // S99ota reads it from the file too
        else if (!strcmp(k, "aa_low_res")) st->aa_low_res = v;   // aa reads it from the file at each connection
        else if (!strcmp(k, "unit_miles")) st->unit_miles = v < 0 ? -1 : !!v;
        else if (!strcmp(k, "unit_f")) st->unit_f = v < 0 ? -1 : !!v;
        else if (!strcmp(k, "unit_psi")) st->unit_psi = v < 0 ? -1 : !!v;
        else if (!strcmp(k, "basic")) st->basic = v;
        else if (!strcmp(k, "cards")) {   // four, left to right
          int c[4];
          if (sscanf(val, "%d , %d , %d , %d", &c[0], &c[1], &c[2], &c[3]) == 4)
            for (int i = 0; i < 4; i++) st->cards[i] = c[i] >= 0 && c[i] < DASH_CARDS ? c[i] : st->cards[i];
        }
        else if (!strcmp(k, "split_card")) st->split_card = v >= 0 && v < DASH_CARDS ? v : st->split_card;
        else if (!strcmp(k, "ring_page")) st->ring_page = v >= 0 && v < DASH_RING_PAGES ? v : st->ring_page;
        // fps60 (60 frames a second) is gone: the panel shows 30. An old file's line is ignored, dropped at the next save
        else if (!strcmp(k, "car")) {   // by its id (modelY7 ...), as dash.conf names it
          for (int i = 0; i < CAR_COUNT; i++)
            if (!strcmp(val, kCars[i].id)) st->car = i, settings_car_saved = true;
        }
        else if (!strcmp(k, "tz") && *val && strlen(val) < sizeof st->tz) strcpy(st->tz, val);
        else if (!strcmp(k, "panda_host") && *val && strlen(val) < sizeof st->panda_host) strcpy(st->panda_host, val);
        else if (!strcmp(k, "panda_port") && v > 0 && v < 65536) st->panda_port = v;
        else if (!strcmp(k, "record")) st->record = v;
      }
      fclose(f);
      log_msg("settings: %s", path);
    }
    snprintf(path, sizeof path, "%s/wifi/networks.conf", settings_root);
    if (!(f = fopen(path, "r"))) {   // nothing saved yet: the Commander's own WiFi with its default passphrase
      snprintf(st->net[0].ssid, sizeof st->net[0].ssid, "%s", SETTINGS_CAR_SSID);
      snprintf(pass[0], sizeof pass[0], "%s", SETTINGS_CAR_PASS);
      st->net_count = 1;
    } else {
      char ssid[160];
      while (st->net_count < DASH_NETS && fgets(ssid, sizeof ssid, f)) {
        if (!fgets(line, sizeof line, f)) line[0] = 0;
        char *s = ssid, *p = line + strcspn(line, "\r\n");
        s[strcspn(s, "\r\n")] = 0;   // an SSID keeps its spaces, as S45wifi-auto reads it
        *p = 0;   // a passphrase keeps its spaces
        if (!*s || strlen(s) >= sizeof st->net[0].ssid || strlen(line) >= sizeof pass[0]) continue;
        snprintf(st->net[st->net_count].ssid, sizeof st->net[0].ssid, "%s", s);
        snprintf(pass[st->net_count++], sizeof pass[0], "%s", line);
      }
      fclose(f);
      snprintf(path, sizeof path, "%s/wifi/car-removed", settings_root);
      bool has_car = false;
      for (int i = 0; i < st->net_count; i++) has_car |= !strcmp(st->net[i].ssid, SETTINGS_CAR_SSID);
      if (!has_car && access(path, F_OK) && st->net_count < DASH_NETS) {   // a list from before: it never held the car's
        memmove(&st->net[1], &st->net[0], sizeof st->net[0] * st->net_count);
        memmove(&pass[1], &pass[0], sizeof pass[0] * st->net_count);
        memset(&st->net[0], 0, sizeof st->net[0]);
        snprintf(st->net[0].ssid, sizeof st->net[0].ssid, "%s", SETTINGS_CAR_SSID);
        snprintf(pass[0], sizeof pass[0], "%s", SETTINGS_CAR_PASS);
        st->net_count++;
      }
    }
  }
  if (ssh_running()) st->dev_ssh = true;   // the switch shows SSH as it is; turning it off then runs dash-ssh-disable
  saved = *st;
}

int settings_save(dash_settings_t *st) {
  // The list as it will be: a typed pass, else the one on file for that SSID ("" for a new one: open).
  dash_settings_t now = *st;
  char np[DASH_NETS][64] = {{0}};
  int n = 0;
  for (int i = 0; i < st->net_count && i < DASH_NETS; i++) {
    const dash_net_t *e = &st->net[i];
    if (!e->ssid[0] || strchr(e->ssid, '\n') || strchr(e->pass, '\n')) continue;
    now.net[n] = *e;
    memset(now.net[n].pass, 0, sizeof now.net[n].pass);
    if (e->pass[0]) snprintf(np[n], sizeof np[n], "%s", e->pass);
    else
      for (int k = 0; k < saved.net_count; k++)
        if (!strcmp(saved.net[k].ssid, e->ssid)) snprintf(np[n], sizeof np[n], "%s", pass[k]);
    n++;
  }
  now.net_count = n;
  for (int i = 1; i < n; i++)   // the car's adapter first: wifi-connect prefers the first, and a re-added one lands last
    if (!strcmp(now.net[i].ssid, SETTINGS_CAR_SSID)) {
      dash_net_t e = now.net[i];
      char ep[64];
      memcpy(ep, np[i], sizeof ep);
      memmove(&now.net[1], &now.net[0], sizeof now.net[0] * i);
      memmove(&np[1], &np[0], sizeof np[0] * i);
      now.net[0] = e;
      memcpy(np[0], ep, sizeof ep);
      break;
    }
  static bool net_unsaved;   // the last networks.conf write failed: write it again whatever changed
  bool wifi = net_unsaved || n != saved.net_count, ssh = now.dev_ssh != saved.dev_ssh;
  for (int i = 0; i < n && !wifi; i++) wifi = strcmp(now.net[i].ssid, saved.net[i].ssid) || strcmp(np[i], pass[i]);
  for (int i = 0; i < DASH_NETS; i++) memset(st->net[i].pass, 0, sizeof st->net[i].pass);   // typed once, gone
  const char *join = now.join[0] && !strchr(now.join, '\n') ? now.join : NULL;   // S45wifi-auto puts it first
  st->join[0] = 0;
  saved = now;
  memcpy(pass, np, sizeof pass);
  if (!writable()) {   // no such folder (the Mac): memory only; there but read-only (a card gone bad): a failed save
    struct stat sb;
    return settings_root && stat(settings_root, &sb) == 0 ? (log_msg("settings: %s is read-only", settings_root), -1) : 0;
  }

  int rc = 0;
  char text[1024];
  snprintf(text, sizeof text,
           "minor_figures = %d\nclock_24h = %d\nbrightness_pct = %d\nfollow_car = %d\nfollow_offset = %d\nssh = %d\nrecord = %d\n"
           "car = %s\nsparse = %d\nwifi_first = %d\ntz = %s\npanda_host = %s\npanda_port = %d\nota_auto = %d\naa_low_res = %d\n"
           "unit_miles = %d\nunit_f = %d\nunit_psi = %d\n",
           now.minor_figures, !now.hour12, now.brightness_pct, now.follow_car, now.follow_offset, now.dev_ssh, now.record,
           kCars[now.car >= 0 && now.car < CAR_COUNT ? now.car : CAR_FALLBACK].id, now.sparse, now.wifi_first,
           strchr(now.tz, '\n') ? "UTC0" : now.tz, strchr(now.panda_host, '\n') ? "192.168.4.1" : now.panda_host,
           now.panda_port, now.ota_auto, now.aa_low_res, now.unit_miles, now.unit_f, now.unit_psi);
  if (now.basic || memcmp(now.cards, kCards, sizeof kCards)) {   // the phone layout's, once off its full layout (the Dash
    size_t k = strlen(text);                                      // never moves them: its file as before)
    snprintf(text + k, sizeof text - k, "basic = %d\ncards = %d, %d, %d, %d\n", now.basic, now.cards[0], now.cards[1],
             now.cards[2], now.cards[3]);
  }
  if (now.split_card != DASH_CARD_ENERGY || now.ring_page != DASH_RING_POWER) {   // the split's card and the ring's face,
    size_t k = strlen(text);                                                        // once swiped (the phone layout's too)
    snprintf(text + k, sizeof text - k, "split_card = %d\nring_page = %d\n", now.split_card, now.ring_page);
  }
  if (!(rc |= put(settings_root, "settings.conf", text, 0644))) settings_car_saved = true;
  if (wifi) {
    char dir[256];
    snprintf(dir, sizeof dir, "%s/wifi", settings_root);
    if (mkdir(dir, 0700) && errno != EEXIST) log_msg("settings: %s: %s", dir, strerror(errno));
    chmod(dir, 0700);
    size_t k = 0;
    text[0] = 0;
    for (int i = 0; i < n; i++) k += snprintf(text + k, sizeof text - k, "%s\n%s\n", now.net[i].ssid, np[i]);
    char mark[300];
    snprintf(mark, sizeof mark, "%s/car-removed", dir);
    bool has_car = false;
    for (int i = 0; i < n; i++) has_car |= !strcmp(now.net[i].ssid, SETTINGS_CAR_SSID);
    if (has_car) unlink(mark);
    else put(dir, "car-removed", "", 0600);   // deleted on purpose: not added back at boot (S45wifi-auto reads it too)
    if ((net_unsaved = put(dir, "networks.conf", text, 0600) != 0)) rc = -1;
    else {
      log_msg("settings: %d networks saved", n);   // names and passphrases stay out of the log
      run(settings_wifi_cmd, "start", join);
    }
  } else if (join) run(settings_wifi_cmd, "start", join);
  if (ssh) run(now.dev_ssh ? settings_ssh_on_cmd : settings_ssh_off_cmd, NULL, NULL);
  return rc;
}

bool settings_apply_tz(const char *tz) {
  const char *was = getenv("TZ");
  if (!*tz || (was && !strcmp(was, tz))) return false;
  setenv("TZ", tz, 1);   // ponytail: setenv beside the Bluetooth thread; only on a change, which is a tap
  tzset();
  log_msg("tz %s", tz);
  return true;
}

// <root>/car-unit: "kph" or "mph", the car's own display unit as it last said it (over WiFi: main.dart keeps carKph
// alike). On Bluetooth alone it never comes, and its speeds are in it.
static int unit_kept = -1;
int settings_car_unit_load(void) {
  char path[256], w[8] = "";
  snprintf(path, sizeof path, "%s/car-unit", settings_root ? settings_root : "");
  FILE *f = settings_root ? fopen(path, "r") : NULL;
  if (f) { if (fscanf(f, "%7s", w) != 1) w[0] = 0; fclose(f); }
  return unit_kept = !strcmp(w, "kph") ? 1 : !strcmp(w, "mph") ? 0 : -1;
}
void settings_car_unit_keep(bool kph) {
  if (unit_kept == (int)kph) return;
  unit_kept = kph;
  if (writable()) put_sync(settings_root, "car-unit", kph ? "kph\n" : "mph\n", 0644, false);
}

// <root>/tyres: "FL FR RL RR" in bar, "nan" for none (main.dart _saveTyres keeps '' for a null).
static double kept[4] = {NAN, NAN, NAN, NAN};
static int64_t keep_at;   // the next write may happen from here (once a minute at most)

void settings_tyres_load(double tp[4]) {
  char path[256];
  snprintf(path, sizeof path, "%s/tyres", settings_root ? settings_root : "");
  FILE *f = settings_root ? fopen(path, "r") : NULL;
  for (int i = 0; i < 4; i++) tp[i] = NAN;
  if (f) {
    double v[4];
    if (fscanf(f, "%lf %lf %lf %lf", &v[0], &v[1], &v[2], &v[3]) == 4)
      for (int i = 0; i < 4; i++) tp[i] = v[i] > 0 && v[i] <= 6.3 ? v[i] : NAN;   // the decoder's own bounds
    fclose(f);
  }
  memcpy(kept, tp, sizeof kept);
}

static bool same(double a, double b) { return isnan(a) ? isnan(b) : a == b; }

void settings_tyres_keep(const double tp[4], int64_t now, bool stop) {
  bool changed = false;
  for (int i = 0; i < 4; i++) changed |= !same(tp[i], kept[i]);
  if (!changed || (!stop && now < keep_at)) return;
  keep_at = now + 60000;
  memcpy(kept, tp, sizeof kept);
  if (!writable()) return;
  char text[96];
  snprintf(text, sizeof text, "%.4f %.4f %.4f %.4f\n", tp[0], tp[1], tp[2], tp[3]);
  put_sync(settings_root, "tyres", text, 0644, false);   // no fsync on the UI thread while driving: the rename keeps it whole
}
