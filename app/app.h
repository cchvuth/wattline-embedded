// The dash program (PLAN-merged 5.1): one poll() loop over the Panda UDP socket and a 33 ms frame timer;
// core/ (sched, panda, derive) keeps the car state, bridge.c turns it into what the UI shows.
#pragma once
#include "../core/sched.h"
#include "../ui/dash_state.h"
#include <stdbool.h>
#include <stdint.h>

// ---- dash.conf (key = value; built-in defaults for every key) ----
typedef struct {
  char panda_host[64];
  int panda_port;
  link_cfg_t link;              // every sched timing (core/sched.h)
  int gear_fresh_ms, grip_hold_ms, steer_hold_ms, yaw_steer_ms, bt_motor_bad_ms;   // car_t.cfg
  int sparse;                   // the app's "Sparse polling" (on)
  int car;                      // CAR_* (car=modelY7 ...)
  int kph;                      // units until the car says (0x257 speedIsKph): 1 km, 0 mi
  char tz[64];                  // POSIX TZ ("UTC0", "CET-1CEST,M3.5.0,M10.5.0/3") or a zone name with tzdata
  int hour12;
  int minor_figures;            // the app's "Show 12 V, other power and cell imbalance"
  int set_clock;                // set the system clock from CAN 0x318, once per link
  char backlight[128];          // sysfs dir with brightness and max_brightness; "" none
  int bright_day, bright_night, bright_idle, bright_min, bright_max, away_min;   // % (away_min 0: never off)
  char log_path[128];
  int log_kb;
  char fb[64], touch[64];
  int rotate, touch_swap, touch_cal[4];
  int frame_ms;
  char cmdr_id[128], cmdr_pin[128];   // the Commander session's files (CMDR=1 builds)
} conf_t;

void conf_defaults(conf_t *c);
int conf_load(conf_t *c, const char *path);   // 0 read, -1 absent (defaults stand); bad lines are logged and skipped

// ---- log: a small rotating file (log_path, log_path.1), also stderr while that is a terminal ----
void log_open(const char *path, int max_kb);
void log_msg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// ---- the Commander's side, as the bridge reads it (all unknown without a Bluetooth session) ----
typedef struct {
  dash_bt_t link;
  const char *dest;             // NULL: none
  double nav_minutes, nav_miles, nav_arrive_pct;   // NAN: none
  int recirc_mode;              // -1: none
} bt_view_t;
void bt_view_none(bt_view_t *b);

// ---- bridge: core state -> dash_state_t, deciding as lib/main.dart and lib/widgets.dart do ----
typedef struct {
  const uint8_t *seen;          // ids heard since the socket opened (Panda.frames' keys), 2048-bit map
  int64_t utc_ms;               // the wall clock (UTC ms) now, or -1: no clock yet ("--:--")
  bool minor_figures, hour12;
} bridge_in_t;
void bridge_fill(dash_state_t *s, const car_t *c, const link_t *l, const bt_view_t *bt, const bridge_in_t *in,
                 int64_t now);

// ---- the loop ----
typedef struct {
  void (*set)(const dash_state_t *s, void *ctx);   // each frame
  uint32_t (*frame)(void *ctx);                    // lv_timer_handler; may be NULL
  int64_t (*idle_ms)(void *ctx);                   // ms since the last touch; may be NULL
  void *ctx;
} ui_hooks_t;
extern int dash_bl_pct;         // the backlight as last set, % (-1 not yet)
int dash_run(const conf_t *cf, const ui_hooks_t *ui, int64_t run_ms);   // run_ms 0: until SIGTERM/SIGINT
int64_t mono_ms(void);
void dash_press(int action);   // a chip's tap -> the Commander (cmdr_action; CMDR=1 builds only)
