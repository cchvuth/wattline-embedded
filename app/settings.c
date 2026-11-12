// See settings.h.
#include "settings.h"
#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

const char *settings_root = "/data";
const char *settings_wifi_cmd = "/etc/init.d/S45wifi-auto";   // run with "start": joins from the new list
const char *settings_ssh_on_cmd = "/usr/sbin/dash-ssh-enable";
const char *settings_ssh_off_cmd = "/usr/sbin/dash-ssh-disable";

static dash_settings_t saved;                 // as last loaded or saved (passes "")
static char pass[DASH_NETS][64];              // saved.net[i]'s passphrase
static pid_t kids[4];

static bool writable(void) { return settings_root && access(settings_root, W_OK) == 0; }

static void run(const char *cmd, const char *arg) {
  for (int i = 0; i < 4; i++)   // reap what finished (ponytail: reaped at the next save, a zombie at most until then)
    if (kids[i] > 0 && waitpid(kids[i], NULL, WNOHANG) == kids[i]) kids[i] = 0;
  if (!cmd) return;
  char *argv[] = {(char *)cmd, (char *)arg, NULL};
  pid_t pid;
  if (posix_spawn(&pid, cmd, NULL, NULL, argv, environ)) { log_msg("settings: %s: %s", cmd, strerror(errno)); return; }
  log_msg("settings: ran %s%s%s", cmd, arg ? " " : "", arg ? arg : "");
  for (int i = 0; i < 4; i++) if (kids[i] <= 0) { kids[i] = pid; break; }
}

// Whole file, atomically: <path>.tmp written and synced, renamed over, the folder synced.
static int put(const char *dir, const char *name, const char *text, mode_t mode) {
  char path[256], tmp[264];
  snprintf(path, sizeof path, "%s/%s", dir, name);
  snprintf(tmp, sizeof tmp, "%s.tmp", path);
  int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
  size_t n = strlen(text);
  bool ok = fd >= 0 && fchmod(fd, mode) == 0 && write(fd, text, n) == (ssize_t)n && fsync(fd) == 0;
  if (fd >= 0) ok = close(fd) == 0 && ok;
  if (ok) ok = rename(tmp, path) == 0;
  if (!ok) { log_msg("settings: write %s: %s", path, strerror(errno)); unlink(tmp); return -1; }
  int d = open(dir, O_RDONLY | O_CLOEXEC);
  if (d >= 0) fsync(d), close(d);
  return 0;
}

static char *trim(char *s) {
  while (*s == ' ' || *s == '\t') s++;
  char *e = s + strlen(s);
  while (e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
  return s;
}

void settings_load(dash_settings_t *st, const conf_t *cf) {
  memset(st, 0, sizeof *st);
  memset(pass, 0, sizeof pass);
  st->minor_figures = cf->minor_figures, st->hour12 = cf->hour12;
  st->brightness_pct = 80, st->follow_car = cf->bright_follow_car;
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
        int v = atoi(trim(eq + 1));
        if (!strcmp(k, "minor_figures")) st->minor_figures = v;
        else if (!strcmp(k, "clock_24h")) st->hour12 = !v;
        else if (!strcmp(k, "brightness_pct")) st->brightness_pct = v < 5 ? 5 : v > 100 ? 100 : v;
        else if (!strcmp(k, "follow_car")) st->follow_car = v;
        else if (!strcmp(k, "ssh")) st->dev_ssh = v;
        else if (!strcmp(k, "record")) st->record = v;
      }
      fclose(f);
      log_msg("settings: %s", path);
    }
    snprintf(path, sizeof path, "%s/wifi/networks.conf", settings_root);
    if ((f = fopen(path, "r"))) {
      char ssid[160];
      while (st->net_count < DASH_NETS && fgets(ssid, sizeof ssid, f)) {
        if (!fgets(line, sizeof line, f)) line[0] = 0;
        char *s = trim(ssid), *p = line + strcspn(line, "\r\n");
        *p = 0;   // a passphrase keeps its spaces
        if (!*s || strlen(s) >= sizeof st->net[0].ssid || strlen(line) >= sizeof pass[0]) continue;
        snprintf(st->net[st->net_count].ssid, sizeof st->net[0].ssid, "%s", s);
        snprintf(pass[st->net_count++], sizeof pass[0], "%s", line);
      }
      fclose(f);
    }
  }
  saved = *st;
}

int settings_save(dash_settings_t *st) {
  // The list as it will be: a typed pass, else the one on file for that SSID ("" for a new one: open).
  dash_settings_t now = *st;
  char np[DASH_NETS][64] = {{0}};
  int n = 0;
  for (int i = 0; i < st->net_count && i < DASH_NETS; i++) {
    const dash_net_t *e = &st->net[i];
    if (!e->ssid[0] || !strcmp(e->ssid, "S3XY_OBD") || strchr(e->ssid, '\n') || strchr(e->pass, '\n')) continue;
    now.net[n] = *e;
    memset(now.net[n].pass, 0, sizeof now.net[n].pass);
    if (e->pass[0]) snprintf(np[n], sizeof np[n], "%s", e->pass);
    else
      for (int k = 0; k < saved.net_count; k++)
        if (!strcmp(saved.net[k].ssid, e->ssid)) snprintf(np[n], sizeof np[n], "%s", pass[k]);
    n++;
  }
  now.net_count = n;
  bool wifi = n != saved.net_count, ssh = now.dev_ssh != saved.dev_ssh;
  for (int i = 0; i < n && !wifi; i++) wifi = strcmp(now.net[i].ssid, saved.net[i].ssid) || strcmp(np[i], pass[i]);
  for (int i = 0; i < DASH_NETS; i++) memset(st->net[i].pass, 0, sizeof st->net[i].pass);   // typed once, gone
  saved = now;
  memcpy(pass, np, sizeof pass);
  if (!writable()) return 0;   // the Mac: memory only

  int rc = 0;
  char text[1024];
  snprintf(text, sizeof text, "minor_figures = %d\nclock_24h = %d\nbrightness_pct = %d\nfollow_car = %d\nssh = %d\n",
           now.minor_figures, !now.hour12, now.brightness_pct, now.follow_car, now.dev_ssh);
  rc |= put(settings_root, "settings.conf", text, 0644);
  if (wifi) {
    char dir[256];
    snprintf(dir, sizeof dir, "%s/wifi", settings_root);
    if (mkdir(dir, 0700) && errno != EEXIST) log_msg("settings: %s: %s", dir, strerror(errno));
    chmod(dir, 0700);
    size_t k = 0;
    text[0] = 0;
    for (int i = 0; i < n; i++) k += snprintf(text + k, sizeof text - k, "%s\n%s\n", now.net[i].ssid, np[i]);
    if (put(dir, "networks.conf", text, 0600)) rc = -1;
    else {
      log_msg("settings: %d networks saved", n);   // names and passphrases stay out of the log
      run(settings_wifi_cmd, "start");
    }
  }
  if (ssh) run(now.dev_ssh ? settings_ssh_on_cmd : settings_ssh_off_cmd, NULL);
  return rc;
}
