// The dash on the S3XY Dash (T113): LVGL on the fbdev panel drawn as the 1920x480 canvas (rotation in the flush),
// evdev touch, the loop of dash.c. Display and touch set up as ui/main_dash.c (checked on the unit), from dash.conf.
// usage: dash [-c dash.conf]     default /run/dashdata/dash.conf; built-in defaults when absent
// With a writable /data (R3: the card's p3, else RAM) the log and the Commander's files default to it.
// The hardware watchdog (watchdog = 1), armed once the display is open, is petted each frame: a hung loop reboots the
// board; a clean exit disarms it.
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
// LVGL redraws, reads the touch and steps its animations at the dash's frame rate. It runs only from the dash's frame, so
// its timers are set to half the frame: at a full one, every frame that starts a millisecond early is skipped.
static void frame_ms(void *ctx, int ms) {
  (void)ctx;
  uint32_t p = ms / 2 > 0 ? (uint32_t)(ms / 2) : 1;
  lv_timer_t *t = lv_display_get_refr_timer(lv_display_get_default());
  if (t) lv_timer_set_period(t, p);
  for (lv_indev_t *i = lv_indev_get_next(NULL); i; i = lv_indev_get_next(i))
    if (lv_indev_get_read_timer(i)) lv_timer_set_period(lv_indev_get_read_timer(i), p);
  if (lv_anim_get_timer()) lv_timer_set_period(lv_anim_get_timer(), p);
}

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

  lv_init();
  lv_display_t *disp = lv_linux_fbdev_create();
  int64_t t0 = mono_ms();   // the open is the one call that waits on the display (its unblank)
  bool fb_ok = lv_linux_fbdev_set_file(disp, cf.fb) == LV_RESULT_OK;
  int64_t open_ms = mono_ms() - t0;
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
  // Armed only now, with the display open: a panel that never answers makes the open's unblank wait out the display
  // driver's timeouts (three of 10 s), longer than the watchdog, and the board would restart for ever. A dead display
  // leaves it up and reachable instead; the frames after the open only write to memory, so a hung loop still reboots.
  if (cf.watchdog) {
    wd = open("/dev/watchdog", O_WRONLY | O_CLOEXEC);
    int t = cf.watchdog_s;
    if (wd < 0) log_msg("watchdog: %s", strerror(errno));
    else if (ioctl(wd, WDIOC_SETTIMEOUT, &t)) log_msg("watchdog timeout %d s: %s", cf.watchdog_s, strerror(errno));
    else log_msg("watchdog %d s", t);
  }
  // Ready: the display opened in good time and the UI built. An update on trial (S99ota, dash-update watch) counts a
  // start healthy only while this file names the running dash, so one whose display hangs (its open waiting out the
  // driver's timeouts, the watchdog not armed yet) is rolled back rather than confirmed.
  if (fb_ok && open_ms < 10000) {
    FILE *f = fopen("/run/dash-ready.new", "w");
    bool ok = f && fprintf(f, "%d\n", (int)getpid()) > 0;
    if (f && fclose(f)) ok = false;
    if (!ok || rename("/run/dash-ready.new", "/run/dash-ready"))
      log_msg("ready mark: %s (an update on trial is not confirmed)", strerror(errno));
  } else log_msg("display %s after %lld ms: not ready (an update on trial is not confirmed)", fb_ok ? "open" : "not open",
                 (long long)open_ms);
  for (int fd = 3; fd < 256; fd++) {   // LVGL's framebuffer and touch fds are opened without O_CLOEXEC: the scripts the
    int fl = fcntl(fd, F_GETFD);        // dash starts (Wi-Fi, SSH, Android Auto) must not inherit them
    if (fl >= 0) fcntl(fd, F_SETFD, fl | FD_CLOEXEC);
  }
  dash_ui_set_action_cb(app_on_action, NULL);       // taps -> one Commander press (actions.c)
  dash_ui_set_settings_cb(app_on_settings, NULL);   // the settings screen -> actions.c, saved by dash.c (settings.c)

  ui_hooks_t ui = {set, frame, idle, NULL, show_settings, ask_car, frame_ms};
  app_stopping = watchdog_off;
  int rc = dash_run(&cf, &ui, 0);
  watchdog_off();
  return rc;
}
