// dash.conf: key = value lines, # comments. Every key has a built-in default (conf_defaults).
#include "app.h"
#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void conf_defaults(conf_t *c) {
  memset(c, 0, sizeof *c);
  snprintf(c->panda_host, sizeof c->panda_host, "192.168.4.1");
  c->panda_port = 1338;
  c->link = kLinkDefaults;
  c->gear_fresh_ms = 2000, c->grip_hold_ms = 3000, c->steer_hold_ms = 2000, c->yaw_steer_ms = 300;
  c->bt_motor_bad_ms = 30000;
  c->sparse = 1;
  c->car = CAR_FALLBACK;
  c->kph = 1;
  snprintf(c->tz, sizeof c->tz, "UTC0");
  c->hour12 = 1;
  c->minor_figures = 1;
  c->set_clock = 1;
  snprintf(c->backlight, sizeof c->backlight, "/sys/class/backlight/backlight");
  c->bright_day = 100, c->bright_night = 30, c->bright_idle = 10, c->bright_min = 5, c->bright_max = 100;
  c->bright_follow_car = 1, c->bright_follow_scale = 1.0, c->bright_follow_offset = 0;
  c->bright_ease_ms = 300, c->bright_stale_ms = 5000;
  c->away_min = 0;
  snprintf(c->log_path, sizeof c->log_path, "/run/dash.log");
  c->log_kb = 256;
  snprintf(c->fb, sizeof c->fb, "/dev/fb0");
  snprintf(c->touch, sizeof c->touch, "/dev/input/event0");
  c->rotate = 270, c->touch_swap = 1;
  c->touch_cal[0] = 479, c->touch_cal[1] = 0, c->touch_cal[2] = 0, c->touch_cal[3] = 1919;
  c->frame_ms = 33;
  snprintf(c->cmdr_id, sizeof c->cmdr_id, "/run/dash-cmdr-id");
  snprintf(c->cmdr_pin, sizeof c->cmdr_pin, "/run/dash-cmdr-pin");
  c->record_max_mb = 200;
  c->watchdog = 1, c->watchdog_s = 15;
}

enum { KINT, KSTR, KLIST, KCAR, KDBL };
typedef struct { const char *key; int kind; size_t off; int n; } key_t_;
#define K_I(k, f) {k, KINT, offsetof(conf_t, f), 1}
#define K_D(k, f) {k, KDBL, offsetof(conf_t, f), 1}
#define K_S(k, f) {k, KSTR, offsetof(conf_t, f), sizeof(((conf_t *)0)->f)}
#define K_L(k, f, n) {k, KLIST, offsetof(conf_t, f), n}
static const key_t_ kKeys[] = {
  K_S("panda_host", panda_host), K_I("panda_port", panda_port),
  K_I("tick_ms", link.tick_ms), K_I("quiet_ms", link.quiet_ms), K_I("lost_ms", link.lost_ms),
  K_I("frozen_ms", link.frozen_ms), K_L("backoff_ms", link.backoff_ms, 4), K_I("ack_resub_ms", link.ack_resub_ms),
  K_I("warmup_ms", link.warmup_ms), K_I("slot_ms", link.slot_ms), K_I("stray_ms", link.stray_ms),
  K_I("strays", link.strays), K_I("flood_frames", link.flood_frames), K_I("flood_ms", link.flood_ms),
  K_I("hot_ms", link.hot_ms), K_I("back_ms", link.back_ms), K_I("steer_readd_ms", link.steer_readd_ms),
  K_I("gear_fresh_ms", gear_fresh_ms), K_I("grip_hold_ms", grip_hold_ms), K_I("steer_hold_ms", steer_hold_ms),
  K_I("yaw_steer_ms", yaw_steer_ms), K_I("bt_motor_bad_ms", bt_motor_bad_ms),
  K_I("sparse", sparse), {"car", KCAR, offsetof(conf_t, car), 1}, K_S("tz", tz), K_I("hour12", hour12),
  K_I("minor_figures", minor_figures), K_I("set_clock", set_clock),
  K_S("backlight", backlight), K_I("bright_day", bright_day), K_I("bright_night", bright_night),
  K_I("bright_idle", bright_idle), K_I("bright_min", bright_min), K_I("bright_max", bright_max),
  K_I("bright_follow_car", bright_follow_car), K_D("bright_follow_scale", bright_follow_scale),
  K_D("bright_follow_offset", bright_follow_offset), K_I("bright_ease_ms", bright_ease_ms),
  K_I("bright_stale_ms", bright_stale_ms),
  K_I("away_min", away_min), K_S("log", log_path), K_I("log_kb", log_kb),
  K_S("fb", fb), K_S("touch", touch), K_I("rotate", rotate), K_I("touch_swap", touch_swap),
  K_L("touch_cal", touch_cal, 4), K_I("frame_ms", frame_ms), K_S("cmdr_id", cmdr_id), K_S("cmdr_pin", cmdr_pin),
  K_I("watchdog", watchdog), K_I("watchdog_s", watchdog_s), K_I("record_max_mb", record_max_mb),
};

static char *trim(char *s) {
  while (isspace((unsigned char)*s)) s++;
  char *e = s + strlen(s);
  while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
  return s;
}

static int to_int(const char *v, int *out) {
  char *end;
  long x = strtol(v, &end, 10);
  if (end == v || *trim(end)) return -1;
  *out = (int)x;
  return 0;
}

static int set(conf_t *c, const char *k, const char *v) {
  if (!strcmp(k, "units")) {   // a key of its own: km or mi, the start value only
    if (strcmp(v, "km") && strcmp(v, "mi")) return -1;
    c->kph = !strcmp(v, "km");
    return 0;
  }
  for (size_t i = 0; i < sizeof kKeys / sizeof *kKeys; i++) {
    const key_t_ *e = &kKeys[i];
    if (strcmp(e->key, k)) continue;
    char *p = (char *)c + e->off;
    switch (e->kind) {
    case KINT: return to_int(v, (int *)p);
    case KDBL: {
      char *end;
      double x = strtod(v, &end);
      if (end == v || *trim(end)) return -1;
      *(double *)p = x;
      return 0;
    }
    case KSTR:
      if (strlen(v) >= (size_t)e->n) return -1;
      strcpy(p, v);
      return 0;
    case KCAR:
      for (int t = 0; t < CAR_COUNT; t++)
        if (!strcmp(kCars[t].id, v)) { *(int *)p = t; return 0; }
      return -1;
    case KLIST: {   // n ints, comma separated: all of them or none
      int tmp[8];
      char buf[128];
      snprintf(buf, sizeof buf, "%s", v);
      int n = 0;
      for (char *t = strtok(buf, ","); t; t = strtok(NULL, ","))
        if (n >= e->n || to_int(trim(t), &tmp[n++])) return -1;
      if (n != e->n) return -1;
      memcpy(p, tmp, sizeof(int) * n);
      return 0;
    }
    }
  }
  return -1;
}

int conf_load(conf_t *c, const char *path) {
  FILE *f = fopen(path, "r");
  if (!f) return -1;
  char line[256];
  for (int no = 1; fgets(line, sizeof line, f); no++) {
    char *h = strchr(line, '#');
    if (h) *h = 0;
    char *s = trim(line);
    if (!*s) continue;
    char *eq = strchr(s, '=');
    if (!eq) { log_msg("%s:%d: no '='", path, no); continue; }
    *eq = 0;
    char *k = trim(s), *v = trim(eq + 1);
    if (set(c, k, v)) log_msg("%s:%d: bad or unknown: %s = %s", path, no, k, v);
  }
  fclose(f);
  return 0;
}
