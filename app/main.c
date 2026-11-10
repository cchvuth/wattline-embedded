// The dash on the S3XY Dash (T113): LVGL on the fbdev panel drawn as the 1920x480 canvas (rotation in the flush),
// evdev touch, the loop of dash.c. Display and touch set up as ui/main_dash.c (checked on the unit), from dash.conf.
// usage: dash [-c dash.conf]     default /run/dashdata/dash.conf; built-in defaults when absent
#include "app.h"
#include "lvgl.h"
#include "../ui/dash_ui.h"
#include <stdio.h>
#include <string.h>

static void set(const dash_state_t *s, void *ctx) { (void)ctx; dash_ui_set(s); }
static uint32_t frame(void *ctx) { (void)ctx; return lv_timer_handler(); }
static int64_t idle(void *ctx) { (void)ctx; return lv_display_get_inactive_time(NULL); }

int main(int argc, char **argv) {
  const char *path = argc > 2 && !strcmp(argv[1], "-c") ? argv[2] : "/run/dashdata/dash.conf";
  conf_t cf;
  conf_defaults(&cf);
  bool have = conf_load(&cf, path) == 0;
  log_open(cf.log_path, cf.log_kb);
  log_msg("dash.conf %s%s", path, have ? "" : ": absent, built-in defaults");

  lv_init();
  lv_display_t *disp = lv_linux_fbdev_create();
  lv_linux_fbdev_set_file(disp, cf.fb);
  int r = cf.rotate;
  lv_display_set_rotation(disp, r == 90 ? LV_DISPLAY_ROTATION_90 : r == 180 ? LV_DISPLAY_ROTATION_180
                                : r == 270 ? LV_DISPLAY_ROTATION_270 : LV_DISPLAY_ROTATION_0);
  log_msg("display %dx%d, rotation %d", (int)lv_display_get_horizontal_resolution(disp),
          (int)lv_display_get_vertical_resolution(disp), r);
  lv_indev_t *ts = lv_evdev_create(LV_INDEV_TYPE_POINTER, cf.touch);
  if (!ts) log_msg("no touch device %s", cf.touch);
  if (ts) {   // the Goodix reports landscape: swapped and calibrated so LVGL's rotation turns it back (main_dash.c)
    lv_evdev_set_swap_axes(ts, cf.touch_swap);
    lv_evdev_set_calibration(ts, cf.touch_cal[0], cf.touch_cal[1], cf.touch_cal[2], cf.touch_cal[3]);
  }
  dash_ui_init(lv_screen_active());

  ui_hooks_t ui = {set, frame, idle, NULL};
  return dash_run(&cf, &ui, 0);
}
