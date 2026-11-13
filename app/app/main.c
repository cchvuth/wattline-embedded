// The dash on the S3XY Dash (T113): LVGL on the fbdev panel drawn as the 1920x480 canvas (rotation in the flush),
// evdev touch, the loop of dash.c. Display and touch set up as ui/main_dash.c (checked on the unit), from dash.conf.
// usage: dash [-c dash.conf]     default /run/dashdata/dash.conf; built-in defaults when absent
// With a writable /data (R3: the card's p3, else RAM) the log and the Commander's files default to it.
// The hardware watchdog (watchdog = 1) is petted each frame: a hung loop reboots the board; a clean exit disarms it.
#include "app.h"
#include "settings.h"
#include "actions.h"
#include "lvgl.h"
#include "../ui/dash_ui.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/watchdog.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

static int wd = -1;

static void set(const dash_state_t *s, void *ctx) { (void)ctx; dash_ui_set(s); }
static uint32_t frame(void *ctx) {
  (void)ctx;
  if (wd >= 0) ioctl(wd, WDIOC_KEEPALIVE, 0);
  return lv_timer_handler();
}
static int64_t idle(void *ctx) { (void)ctx; return lv_display_get_inactive_time(NULL); }
static void show_settings(void *ctx) { (void)ctx; dash_ui_settings(&app_settings); }
static void ask_car(void *ctx) { (void)ctx; dash_ui_ask_car(); }

static void watchdog_off(void) {   // the magic close: a clean stop does not reboot
  if (wd >= 0 && write(wd, "V", 1) == 1) log_msg("watchdog disarmed");
  if (wd >= 0) close(wd);
  wd = -1;
}

int main(int argc, char **argv) {
  const char *path = argc > 2 && !strcmp(argv[1], "-c") ? argv[2] : "/run/dashdata/dash.conf";
  conf_t cf;
  conf_defaults(&cf);
  if (access("/data", W_OK) == 0) {   // dash.conf may still point them elsewhere
    mkdir("/data/log", 0755);
    snprintf(cf.log_path, sizeof cf.log_path, "/data/log/dash.log");
    snprintf(cf.cmdr_id, sizeof cf.cmdr_id, "/data/cmdr_id");
    snprintf(cf.cmdr_pin, sizeof cf.cmdr_pin, "/data/cmdr_pin");
  }
  bool have = conf_load(&cf, path) == 0;
  log_open(cf.log_path, cf.log_kb);
  log_msg("dash.conf %s%s", path, have ? "" : ": absent, built-in defaults");
  if (cf.watchdog) {
    wd = open("/dev/watchdog", O_WRONLY | O_CLOEXEC);
    int t = cf.watchdog_s;
    if (wd < 0) log_msg("watchdog: %s", strerror(errno));
    else if (ioctl(wd, WDIOC_SETTIMEOUT, &t)) log_msg("watchdog timeout %d s: %s", cf.watchdog_s, strerror(errno));
    else log_msg("watchdog %d s", t);
  }

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
  dash_ui_set_action_cb(app_on_action, NULL);       // taps -> one Commander press (actions.c)
  dash_ui_set_settings_cb(app_on_settings, NULL);   // the settings screen -> actions.c, saved by dash.c (settings.c)

  ui_hooks_t ui = {set, frame, idle, NULL, show_settings, ask_car};
  app_stopping = watchdog_off;
  int rc = dash_run(&cf, &ui, 0);
  watchdog_off();
  return rc;
}
