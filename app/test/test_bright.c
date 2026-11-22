// The backlight's level (app/bright.c): the car's brightness mapped and clamped, eased, the lamps' fallback, idle,
// a manual level; and its dash.conf keys (app/conf.c). Fake clock, no sysfs.
#include "../app/app.h"
#include "signals_gen.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static int pass, fail, logs;
#define CHECK(c) do { if (c) pass++; else { fail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
void log_msg(const char *fmt, ...) { (void)fmt; logs++; }

static int64_t now = 5000000;
static conf_t cf;
static car_t C;
static bright_t B;
static int step(int ms) { now += ms; return bright_step(&B, &cf, &C, now, 0); }
static void brightness(int raw) { car_frame(&C, 0x273, 0, (const uint8_t[8]){0, 0, 0, 0, (uint8_t)raw, 0, 0, 0}, 8, now); }
static void lamps(int core) { car_frame(&C, 0x3F5, 0, (const uint8_t[8]){0, 0, 0, 0, (uint8_t)core, 0, 0, 0}, 8, now); }

int main(void) {
  conf_defaults(&cf);
  CHECK(cf.bright_follow_car == 1 && cf.bright_follow_scale == 1.0 && cf.bright_follow_offset == 0);
  car_init(&C);
  bright_init(&B, &cf, now);
  CHECK(step(0) == 0 && B.src == BRIGHT_NO_CAR);  // no car data, following the car: the lowest, at once (no ramp)

  C.wifi_live = true;                             // frames, but no 0x273 yet: where it is for a moment (the car's own
  lamps(2);                                       // level comes within its 500 ms), then the lamps' rule, day
  CHECK(step(0) == 0 && step(1400) == 0 && B.src == BRIGHT_NO_CAR);
  CHECK(step(100) == 0);                          // a change starts from where it is...
  int mid = step(150);
  CHECK(mid > 40 && mid < 70);                    // ...eases through the middle...
  CHECK(step(150) == 100);                        // ...and lands at 300 ms
  lamps(4);                                       // low beams: night
  step(0);
  CHECK(step(300) == cf.bright_night);

  brightness(120);                                // the car's screen at 60 %: followed, whatever the lamps
  step(0);
  int prev = 30, monotone = 1;
  for (int i = 0; i < 10; i++) { int p = step(33); monotone &= p >= prev; prev = p; }
  CHECK(monotone && prev == 60);                  // up from 30 without a step back
  brightness(109);
  step(0);
  CHECK(step(300) == 55);                         // 54.5 % (the capture), rounded

  cf.bright_follow_scale = 0.5, cf.bright_follow_offset = 10;
  brightness(120);
  step(0);
  CHECK(step(300) == 40);                         // 60 * 0.5 + 10
  cf.bright_follow_scale = 1, cf.bright_follow_offset = 0;
  cf.bright_min = 5;
  brightness(4);
  step(0);
  CHECK(step(300) == cf.bright_min);              // 2 %: kept at bright_min
  cf.bright_min = 0;
  brightness(255);                                // SNA: back to the lamps (still night)
  step(0);
  CHECK(step(300) == 30);

  brightness(160);                                // 80 %, then 0x273 stops: stale after bright_stale_ms
  step(0);
  CHECK(step(300) == 80);
  step(cf.bright_stale_ms - 300);
  CHECK(B.src == BRIGHT_CAR);
  step(1);
  CHECK(B.src == BRIGHT_LAMPS && step(300) == 30);

  brightness(160);                                // following at 80 %, then WiFi takes over from Bluetooth: the
  step(0);                                        // car's values forgotten for the step-aside's settle...
  CHECK(step(300) == 80);
  C.wifi_live = false, car_from_bt(&C, NULL, now);
  CHECK(step(1000) == 80 && B.src == BRIGHT_CAR); // ...held, not dimmed to idle
  C.wifi_live = true;                             // live again, its 0x273 not in yet: still held, not the day level
  CHECK(step(1000) == 80 && B.src == BRIGHT_CAR);
  C.p.bright_at = now;                            // Bluetooth re-stamping its pushed level: not a 0x273, still held
  CHECK(step(0) == 80 && B.src == BRIGHT_CAR);
  step(cf.bright_stale_ms - 2000 + 1);
  CHECK(B.src != BRIGHT_CAR);                     // never longer than bright_stale_ms
  brightness(255);                                // the lamps' level (the car's says "not available"), then a takeover:
  lamps(4);                                       // the night level held over the settle, not idle
  step(0), step(300);
  CHECK(B.src == BRIGHT_LAMPS && step(0) == 30);
  C.wifi_live = false, car_from_bt(&C, NULL, now);
  CHECK(step(2000) == 30 && B.src == BRIGHT_LAMPS);
  step(cf.bright_stale_ms);
  CHECK(B.src == BRIGHT_NO_CAR);                  // a loss that lasts: the lowest
  C.wifi_live = true;
  lamps(4);
  step(0), step(300);

  brightness(160);                                // a level set by hand stops following until re-enabled
  bright_manual(&B, 70);
  step(0);
  CHECK(step(300) == 70 && B.src == BRIGHT_MANUAL);
  C.wifi_live = false;                            // set by hand: held even with no car data, not dimmed to idle
  step(0);
  CHECK(step(300) == 70 && B.src == BRIGHT_MANUAL);
  C.wifi_live = true;
  bright_follow(&B);
  step(0);
  CHECK(step(300) == 80 && B.src == BRIGHT_CAR);

  cf.bright_follow_car = 0;                       // off in dash.conf: never follows
  bright_init(&B, &cf, now);
  CHECK(step(0) == 30);
  cf.bright_follow_car = 1;
  bright_init(&B, &cf, now);

  C.wifi_live = false;                            // the link gone: the lowest
  step(0);
  CHECK(step(300) == 0 && B.src == BRIGHT_NO_CAR);
  cf.bright_follow_car = 0;                       // not following: the idle level
  bright_init(&B, &cf, now);
  CHECK(step(0) == cf.bright_idle && B.src == BRIGHT_IDLE);
  cf.bright_follow_car = 1;
  bright_init(&B, &cf, now);
  step(0);

  // Following the car with no car data: 0 %, the lowest lit level, touched or not; a level set by hand, or following
  // switched off in dash.conf, stays as it was (above).
  now += 300;
  CHECK(bright_step(&B, &cf, &C, now, 0) == 0 && B.src == BRIGHT_NO_CAR);
  CHECK(bright_step(&B, &cf, &C, now, 600000) == 0 && B.src == BRIGHT_NO_CAR);
  C.wifi_live = true, brightness(160);            // car data back: its level
  step(0);
  CHECK(step(300) == 80 && B.src == BRIGHT_CAR);
  C.wifi_live = false, car_from_bt(&C, NULL, now); // and gone again: held for the settle, then the lowest
  CHECK(step(1000) == 80);
  step(cf.bright_stale_ms + cf.bt_settle_ms + 3000);
  CHECK(step(300) == 0 && B.src == BRIGHT_NO_CAR);

  // Away (dark), then car data without its brightness yet: lit at once, at the lowest (not dark for that moment)
  cf.away_min = 1;
  bright_init(&B, &cf, now);
  C.wifi_live = false, car_from_bt(&C, NULL, now);
  bright_step(&B, &cf, &C, now, 0);
  now += 61000;
  bright_step(&B, &cf, &C, now, 61000), now += 300;
  CHECK(bright_step(&B, &cf, &C, now, 61300) == 0 && B.src == BRIGHT_AWAY);
  C.wifi_live = true, lamps(2);
  now += 33;
  CHECK(bright_step(&B, &cf, &C, now, 61333) == 0 && B.src == BRIGHT_NO_CAR);
  cf.away_min = 0;
  C.wifi_live = false, car_from_bt(&C, NULL, now);
  bright_init(&B, &cf, now);

  // The level as the backlight's own: 0 % the floor (never 0, which is off), fine steps below 10 %, the duty itself
  // from there up; a coarse backlight (100 steps) still gets its lowest step.
  CHECK(bright_raw(&cf, 0, 2000) == 1 && bright_raw(&cf, 1, 2000) == 3 && bright_raw(&cf, 5, 2000) == 51);
  CHECK(bright_raw(&cf, 10, 2000) == 200 && bright_raw(&cf, 30, 2000) == 600 && bright_raw(&cf, 100, 2000) == 2000);
  for (int p = 1; p <= 100; p++) CHECK(bright_raw(&cf, p, 2000) > bright_raw(&cf, p - 1, 2000));
  CHECK(bright_raw(&cf, 0, 100) == 1 && bright_raw(&cf, 3, 100) == 1 && bright_raw(&cf, 10, 100) == 10 && bright_raw(&cf, 100, 100) == 100);

  // Over Bluetooth (no WiFi): field 24 as %; out of range is ignored and logged once.
  car_init(&C);
  bt_map_t m;
  memset(&m, 0, sizeof m);
  m.has[61] = m.is_int[61] = 1;
  m.has[24] = m.is_int[24] = 1, m.v[24] = 45;
  car_from_bt(&C, &m, now);
  step(0);
  CHECK(step(300) == 45 && B.src == BRIGHT_CAR);
  m.v[24] = 200;
  car_from_bt(&C, &m, now);
  int before = logs;
  step(0);
  step(0);
  CHECK(logs == before + 1 && step(300) == 45);  // one line, the last good level kept

  // Sparse logging: a 60 s drive of a car brightness wandering by half a percent logs nothing more.
  before = logs;
  car_init(&C);
  C.wifi_live = true;
  for (int i = 0; i < 1800; i++) { brightness(100 + (i / 60) % 2); step(33); }
  CHECK(logs - before == 1);                      // the move to "follows the car's screen" only

  // dash.conf: the keys parse, a bad one is refused and leaves the default.
  const char *path = "build/test_bright.conf";
  FILE *f = fopen(path, "w");
  fputs("bright_follow_car = 0\nbright_follow_scale = 0.8\nbright_follow_offset = -5.5\nbright_ease_ms = 500\n"
        "bright_stale_ms = abc\n", f);
  fclose(f);
  conf_defaults(&cf);
  before = logs;
  CHECK(conf_load(&cf, path) == 0);
  CHECK(cf.bright_follow_car == 0 && cf.bright_follow_scale == 0.8 && cf.bright_follow_offset == -5.5 &&
        cf.bright_ease_ms == 500 && cf.bright_stale_ms == 5000 && logs == before + 1);
  remove(path);

  printf("test_bright: %d/%d checks pass\n", pass, pass + fail);
  return fail != 0;
}
