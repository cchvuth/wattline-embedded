// The backlight's level: the car's own screen brightness while it is known (0x273, or the Commander's field 24),
// scaled into bright_min..bright_max; else day or night from the car's lamps; without car data bright_no_car (20 %) while
// following the car (a slow WiFi join: readable in daylight, not glaring at night), else bright_idle; off when away. 0 % is not off: it is the
// lowest level at which the panel still shows its picture (bright_floor, bright_raw): one step of the backlights' 2000
// (both screens'). The F133's is lit, and its picture read, down to that last step while it is on (its driver keeps a
// minimum of its own), and dark only off. Every change eases (smoothstep over bright_ease_ms) so the panel never steps visibly.
#include "app.h"
#include <math.h>
#include <string.h>

static const char *const kSrc[] = {"follows the car's screen", "day/night from the lamps", "idle: no car data", "away: off", "set by hand",
                                   "following the car: no car data yet (bright_no_car)"};

enum { JUST_LIVE_MS = 1500 };   // car data this new: its own brightness is waited for before the lamps' rule

// What to write to the backlight for a level in %, of [max] steps: 0 % the lowest that still shows a picture
// (bright_floor, % of the full duty), 100 % all of it. From 10 % up the duty is the level itself; below, the steps
// grow with the square from the floor: fine at the dark end, where the eye tells small steps apart (linear, 1 % was
// ten times the floor). Never 0: that is off, the away rule's.
int bright_raw(const conf_t *cf, int pct, int max) {
  double f = cf->bright_floor < 0 ? 0 : cf->bright_floor > 10 ? 10 : cf->bright_floor;
  double d = pct >= 10 ? pct : f + (10 - f) * pct * pct / 100.0;
  long v = lround(d * max / 100);
  return v < 1 ? 1 : v > max ? max : (int)v;
}

void bright_init(bright_t *b, const conf_t *cf, int64_t now) {
  memset(b, 0, sizeof *b);
  b->follow = cf->bright_follow_car;
  b->offset = 0;   // the settings' "Follow offset" bias, on top of dash.conf's bright_follow_offset
  b->manual = -1;
  b->src = -1;
  b->live_at = now;
  b->shown = NAN;
  b->car_at = -1;
}

// The settings screen's "Follow car screen brightness" switch and slider (dash.c applies them).
void bright_manual(bright_t *b, int pct) { b->follow = 0; b->manual = pct; }
void bright_follow(bright_t *b) { b->follow = 1; b->manual = -1; }

int bright_step(bright_t *b, const conf_t *cf, const car_t *c, int64_t now, int64_t idle) {
  bool live = car_live(c);
  if (live && !b->was_live) b->live_since = now;
  b->was_live = live;
  if (live) {
    b->live_at = now, b->seen_live = true;
    if (c->low_beam >= 0 || c->high_beam >= 0) b->night = c->low_beam == 1 || c->high_beam == 1, b->lamps = true;
  }
  double car = car_display_brightness(c, now), t;
  int src;
  // WiFi taking over from Bluetooth forgets the car's values for the step-aside's settle, and 0x273 comes a moment
  // after the hello (a short loss alike): what the car last gave is held that long, its level (when it vanished rather
  // than went stale: no 0x273 since) or the lamps' — else idle (10 %), then the day level, would flash in between.
  int64_t hold = cf->bright_stale_ms > cf->bt_settle_ms + 2000 ? cf->bright_stale_ms : cf->bt_settle_ms + 2000;
  bool held = !live && b->seen_live && now - b->live_at <= hold;
  if (live && !isnan(car)) b->car_level = car, b->car_at = now, b->car_frame_at = c->p.bright_can_at;
  else if ((held || (live && isnan(c->display_brightness))) && c->p.bright_can_at == b->car_frame_at && b->car_at >= 0 &&
           now - b->car_at <= hold)
    car = b->car_level;
  if ((live || held) && b->follow && !isnan(car)) t = car * cf->bright_follow_scale + cf->bright_follow_offset + b->offset, src = BRIGHT_CAR;
  // Car data just came, the car's own level not yet in it (0x273 comes every 500 ms): where it is for that moment,
  // not up to the lamps' day level and down again (a flash in a dark cabin at every start and reconnect)
  // (a dash started on live data, or coming back from away, has no level yet: the lowest for that moment)
  else if (b->follow && live && now - b->live_since < JUST_LIVE_MS) {
    // (nor the away rule's dark: car data ends it at once, at the lowest)
    if (b->src >= 0 && b->src != BRIGHT_AWAY) t = b->target, src = b->src;
    else t = 0, src = BRIGHT_NO_CAR;   // the lowest, not bright_no_car: the car's level is < 500 ms away (up to 20 % and
                                     // down again would flash a dark cabin, coming back from away)
  }
  else if (b->manual >= 0) t = b->manual, src = BRIGHT_MANUAL;   // a level set by hand: held with or without car data
  else if (live || held) t = b->night ? cf->bright_night : cf->bright_day, src = BRIGHT_LAMPS;
  else if (b->follow) t = cf->bright_no_car, src = BRIGHT_NO_CAR;   // following the car, no car data yet
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
