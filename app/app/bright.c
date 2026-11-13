// The backlight's level: the car's own screen brightness while it is known (0x273, or the Commander's field 24),
// scaled into bright_min..bright_max; else day or night from the car's lamps; bright_idle without car data; off
// when away. Every change eases (smoothstep over bright_ease_ms) so the panel never steps visibly.
#include "app.h"
#include <math.h>
#include <string.h>

static const char *const kSrc[] = {"follows the car's screen", "day/night from the lamps", "idle: no car data", "away: off", "set by hand"};

void bright_init(bright_t *b, const conf_t *cf, int64_t now) {
  memset(b, 0, sizeof *b);
  b->follow = cf->bright_follow_car;
  b->offset = 0;   // the settings' "Follow offset" bias, on top of dash.conf's bright_follow_offset
  b->manual = -1;
  b->src = -1;
  b->live_at = now;
  b->shown = NAN;
}

// The settings screen's "Follow car screen brightness" switch and slider (dash.c applies them).
void bright_manual(bright_t *b, int pct) { b->follow = 0; b->manual = pct; }
void bright_follow(bright_t *b) { b->follow = 1; b->manual = -1; }

int bright_step(bright_t *b, const conf_t *cf, const car_t *c, int64_t now, int64_t idle) {
  bool live = car_live(c);
  if (live) {
    b->live_at = now;
    if (c->low_beam >= 0 || c->high_beam >= 0) b->night = c->low_beam == 1 || c->high_beam == 1;
  }
  double car = car_display_brightness(c, now), t;
  int src;
  if (live && b->follow && !isnan(car)) t = car * cf->bright_follow_scale + cf->bright_follow_offset + b->offset, src = BRIGHT_CAR;
  else if (b->manual >= 0) t = b->manual, src = BRIGHT_MANUAL;   // a level set by hand: held with or without car data
  else if (live) t = b->night ? cf->bright_night : cf->bright_day, src = BRIGHT_LAMPS;
  else t = cf->bright_idle, src = BRIGHT_IDLE;
  t = t < cf->bright_min ? cf->bright_min : t > cf->bright_max ? cf->bright_max : t;
  int64_t away = (int64_t)cf->away_min * 60000;
  if (!live && away > 0 && now - b->live_at >= away && idle >= away) t = 0, src = BRIGHT_AWAY;

  if (src != b->src || fabs(t - b->logged) >= 5) {   // sparse: a new source, or a move of 5 points
    if (src == BRIGHT_CAR) log_msg("backlight %.0f%%: %s (%.1f%%)", t, kSrc[src], car);
    else log_msg("backlight %.0f%%: %s", t, kSrc[src]);
    b->src = src;
    b->logged = t;
  }
  if (c->p.bt_bright_bad >= 0 && !b->bad_logged) {
    log_msg("Commander display_brightness %lld is not a %% (0..100): ignored", (long long)c->p.bt_bright_bad);
    b->bad_logged = true;
  }

  if (isnan(b->shown)) b->shown = b->from = b->target = t, b->ramp_at = now;   // the first level: no ramp
  else if (t != b->target) b->from = b->shown, b->target = t, b->ramp_at = now;
  double x = cf->bright_ease_ms > 0 ? (double)(now - b->ramp_at) / cf->bright_ease_ms : 1;
  x = x < 0 ? 0 : x > 1 ? 1 : x;
  b->shown = b->from + (b->target - b->from) * x * x * (3 - 2 * x);
  return (int)lround(b->shown);
}
