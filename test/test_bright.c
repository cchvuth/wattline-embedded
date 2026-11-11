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
  CHECK(step(0) == cf.bright_idle);              // no car data: idle, at once (the first level has no ramp)

  C.wifi_live = true;                             // frames, but no 0x273 yet: the lamps' rule, day
  lamps(2);
  CHECK(step(0) == 10);                           // a change starts from where it is...
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
  brightness(4);
  step(0);
  CHECK(step(300) == cf.bright_min);              // 2 %: kept at bright_min
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

  brightness(160);                                // a level set by hand stops following until re-enabled
  bright_manual(&B, 70);
  step(0);
  CHECK(step(300) == 70 && B.src == BRIGHT_LAMPS);
  bright_follow(&B);
  step(0);
  CHECK(step(300) == 80 && B.src == BRIGHT_CAR);

  cf.bright_follow_car = 0;                       // off in dash.conf: never follows
  bright_init(&B, &cf, now);
  CHECK(step(0) == 30);
  cf.bright_follow_car = 1;
  bright_init(&B, &cf, now);

  C.wifi_live = false;                            // the link gone: idle
  step(0);
  CHECK(step(300) == cf.bright_idle && B.src == BRIGHT_IDLE);

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
