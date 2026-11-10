// The dash's loop on the Mac, for the bench tests: the real UI drawn headless (LVGL, no panel), and every
// -i ms a JSON line of what it was given (the test reads these). -p DIR: a PNG of the screen every 5 s.
// usage: dash-host [-c dash.conf] [-t seconds] [-i ms] [-p dir]
#include "app.h"
#include "lvgl.h"
#include "../ui/dash_ui.h"
#include "src/libs/lodepng/lodepng.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { W = 1920, H = 480 };
static int64_t start, next_line, next_png, every = 200;
static const char *png_dir;
static uint8_t *fb;

static uint32_t tick(void) { return (uint32_t)mono_ms(); }
static void flush(lv_display_t *d, const lv_area_t *a, uint8_t *px) { (void)a, (void)px; lv_display_flush_ready(d); }

static void num(const char *k, double v) { printf(isnan(v) ? ",\"%s\":null" : ",\"%s\":%.3f", k, v); }

static void png(int64_t t) {
  static uint8_t rgba[W * H * 4];
  for (size_t p = 0; p < sizeof rgba; p += 4)   // XRGB8888 is B, G, R, X in memory
    rgba[p] = fb[p + 2], rgba[p + 1] = fb[p + 1], rgba[p + 2] = fb[p], rgba[p + 3] = 255;
  char path[512];
  snprintf(path, sizeof path, "%s/t%03lld.png", png_dir, (long long)(t / 1000));
  unsigned char *data;
  size_t len;
  unsigned err = lodepng_encode32(&data, &len, rgba, W, H);
  FILE *f = err ? NULL : fopen(path, "wb");
  if (f) fwrite(data, 1, len, f), fclose(f);
  else fprintf(stderr, "%s: %s\n", path, err ? lodepng_error_text(err) : "can't write");
  if (!err) lv_free(data);
}

static void set(const dash_state_t *s, void *ctx) {
  (void)ctx;
  dash_ui_set(s);
  int64_t now = mono_ms();
  if (now < next_line) return;
  next_line = now + every;
  printf("{\"t\":%.3f,\"wifi\":%d,\"bt\":%d,\"live\":%d,\"wifi_live\":%d,\"gear\":\"%c\"", (now - start) / 1000.0,
         s->wifi, s->bt, s->live, s->wifi_live, s->gear ? s->gear : '-');
  num("speed", s->speed), num("front_kw", s->front_kw), num("rear_kw", s->rear_kw), num("pedal", s->pedal_pct);
  num("brake_pct", s->brake_pct), num("regen_cap", s->regen_cap_kw), num("limit", s->speed_limit);
  num("cruise_set", s->cruise_set), num("soc", s->soc_pct), num("range", s->range), num("outside", s->outside_c);
  num("charge_kw", s->charge_kw), num("tyre_fl", s->tyre_psi[0]), num("climate_kw", s->climate_kw);
  num("other_kw", s->other_kw), num("imbalance", s->imbalance_mv), num("degradation", s->degradation_pct);
  printf(",\"ap\":%d,\"ind_l\":%d,\"ind_r\":%d,\"braking\":%d,\"low\":%d,\"drl\":%d,\"charging\":%d,\"limit_pct\":%d,"
         "\"minutes\":%d,\"charge_clock\":\"%s\",\"cabin\":%d,\"seat_d\":%d,\"door_fl\":%d,\"charge_port\":%d,"
         "\"speed_hz\":%d,\"g_shown\":%d,\"draws\":%d,\"backlight\":%d}\n",
         s->ap, s->ind_l, s->ind_r, s->braking, s->low_beam, s->drl, s->charging, s->charge_limit_pct,
         s->charge_minutes, s->charge_clock, s->cabin, s->seat[DS_SEAT_D], s->door_fl, s->charge_port, s->speed_hz,
         s->g_shown, s->draws, dash_bl_pct);
  fflush(stdout);
}

static uint32_t frame(void *ctx) {
  (void)ctx;
  uint32_t r = lv_timer_handler();
  int64_t now = mono_ms();
  if (png_dir && now >= next_png) {
    next_png = now + 5000;
    lv_refr_now(NULL);
    png(now - start);
  }
  return r;
}

int main(int argc, char **argv) {
  const char *path = NULL;
  int secs = 0, o;
  while ((o = getopt(argc, argv, "c:t:i:p:")) != -1)
    switch (o) {
    case 'c': path = optarg; break;
    case 't': secs = atoi(optarg); break;
    case 'i': every = atoi(optarg); break;
    case 'p': png_dir = optarg; break;
    default: fprintf(stderr, "usage: dash-host [-c dash.conf] [-t seconds] [-i ms] [-p dir]\n"); return 2;
    }
  conf_t cf;
  conf_defaults(&cf);
  if (path && conf_load(&cf, path)) { fprintf(stderr, "%s: can't read\n", path); return 2; }
  log_open(cf.log_path, cf.log_kb);

  lv_init();
  lv_tick_set_cb(tick);
  lv_display_t *d = lv_display_create(W, H);
  lv_display_set_color_format(d, LV_COLOR_FORMAT_XRGB8888);
  fb = malloc((size_t)W * H * 4);
  lv_display_set_buffers(d, fb, NULL, (size_t)W * H * 4, LV_DISPLAY_RENDER_MODE_DIRECT);
  lv_display_set_flush_cb(d, flush);
  dash_ui_init(lv_screen_active());

  start = mono_ms();
  ui_hooks_t ui = {set, frame, NULL, NULL};
  return dash_run(&cf, &ui, (int64_t)secs * 1000);
}
