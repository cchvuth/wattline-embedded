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
  int bright_follow_car;        // follow the car's own screen brightness when it is known (1)
  double bright_follow_scale, bright_follow_offset;   // backlight % = car % * scale + offset, then min..max
  int bright_ease_ms;           // a change eases over this long, never a visible step (300)
  int bright_stale_ms;          // car_t.cfg: the car's brightness this old is unknown (5000)
  char log_path[128];
  int log_kb;
  char fb[64], touch[64];
  int rotate, touch_swap, touch_cal[4];
  int frame_ms;
  char cmdr_id[128], cmdr_pin[128];   // the Commander session's files (CMDR=1 builds)
  int bt_settle_ms, bt_buttons_ms, bt_data_ms, bt_back_ms;   // cmdr_cfg's settle, buttons, rejoin and back timings
  int record_max_mb;            // drive recordings (record.c) kept to this total, oldest deleted first; 0 no cap
  int watchdog, watchdog_s;     // /dev/watchdog petted each frame (1/0), its timeout in s (main.c)
} conf_t;

void conf_defaults(conf_t *c);
int conf_load(conf_t *c, const char *path);   // 0 read, -1 absent (defaults stand); bad lines are logged and skipped

// ---- log: a small rotating file (log_path, log_path.1), also stderr while that is a terminal ----
void log_open(const char *path, int max_kb);
void log_msg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// ---- the Commander's side, as the bridge reads it (all unknown without a Bluetooth session) ----
typedef struct {
  dash_bt_t link;
  const char *why, *push_info, *car_link;   // cmdr_t's words ("" none)
  unsigned push_count;
  int64_t until;                // what its icon counts down to (CLOCK_MONOTONIC ms), or -1
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
  int wifi_join;                // wifi_join(): WIFI_OFF / _SCANNING / _JOINING until wlan0 has an address; -1 the link's
  const char *join_why;         // its words
  const char *net_line;         // wifi_net_line(): the joined network, its address and ssh's state
  const char *ssid;             // wifi_ssid(): the joined network, "" none
  int nearby_n;                 // wifi_nearby(): networks in range, strongest first
  const char (*nearby)[33];
  int ssh;                      // wifi_ssh(): -1 off, else the sessions open
  const char *host;             // the Panda's address, for the status line
  int port;
} bridge_in_t;
void bridge_fill(dash_state_t *s, const car_t *c, const link_t *l, const bt_view_t *bt, const bridge_in_t *in,
                 int64_t now);

// ---- the WiFi join (wifi.c): wlan0 and wpa_supplicant, until the link's own states take over ----
typedef struct { char last[24]; int fails; } wifi_track_t;   // the last wpa_state, handshakes that fell back since joined
// What the WiFi button says while not joined, or -1 (joined: the link's). oper: wlan0's operstate; wpa: wpa_supplicant's
// wpa_state, NULL unknown. Two handshakes falling back to DISCONNECTED/SCANNING say "wrong password?".
int wifi_join_of(wifi_track_t *t, bool exists, const char *oper, bool ipv4, const char *wpa, const char **why);
int wifi_join(int64_t now, const char **why);   // the system's, looked at every 1.5 s; -1 off Linux
bool wifi_has_addr(void);   // wlan0 has an IPv4 address on a running link now (fresh, not the 1.5 s look)
const char *wifi_net_line(void);                // refreshed by wifi_join(); "" off Linux
const char *wifi_ssid(void);                    // the joined network's name, "" none
int wifi_nearby(const char (**list)[33]);       // networks in range (latest SCAN_RESULTS), strongest first; the count
void wifi_scan(void);                           // ask wpa_supplicant for a fresh scan (the Network tab opened)
int wifi_ssh(void);                             // dropbear on port 22: -1 off, else the sessions open
// wpa_supplicant's SCAN_RESULTS reply -> distinct SSIDs (escapes decoded, hidden ones left out), strongest first
int wifi_scan_parse(const char *reply, char (*out)[33], int max);

// ---- the loop ----
typedef struct {
  void (*set)(const dash_state_t *s, void *ctx);   // each frame
  uint32_t (*frame)(void *ctx);                    // lv_timer_handler; may be NULL
  int64_t (*idle_ms)(void *ctx);                   // ms since the last touch; may be NULL
  void *ctx;
  void (*show_settings)(void *ctx);                // app_settings loaded: show them (dash_ui_settings); may be NULL
  void (*ask_car)(void *ctx);                      // first boot (no car saved): the settings' Car tab; may be NULL
} ui_hooks_t;
// ---- the backlight's level (bright.c): the car's screen brightness, else its lamps, else idle; eased ----
enum { BRIGHT_CAR, BRIGHT_LAMPS, BRIGHT_IDLE, BRIGHT_AWAY, BRIGHT_MANUAL };
typedef struct {
  int follow, manual;           // follow the car (bright_follow_car); manual: settings' %, -1 none
  int offset;                   // follow-car bias (% points added), from the settings' "Follow offset"
  bool night, lamps, bad_logged, seen_live;   // lamps: night known (the car's lamps seen since the start)
  int src;                      // BRIGHT_*, -1 none yet
  double shown, from, target, logged;
  int64_t live_at, ramp_at;
  double car_level;             // the car's own level as last in use, at car_at (-1: never): held over a takeover
  int64_t car_at, car_frame_at; // car_frame_at: car.p.bright_can_at then: a 0x273 since ends the hold
} bright_t;
void bright_init(bright_t *b, const conf_t *cf, int64_t now);
int bright_step(bright_t *b, const conf_t *cf, const car_t *c, int64_t now, int64_t idle);   // the % to set now
void bright_manual(bright_t *b, int pct);   // a brightness set by hand: stops following the car until bright_follow
void bright_follow(bright_t *b);
extern int dash_bl_pct;         // the backlight as last set, % (-1 not yet)
int dash_run(const conf_t *cf, const ui_hooks_t *ui, int64_t run_ms);   // run_ms 0: until SIGTERM/SIGINT
int64_t mono_ms(void);
void dash_press(int action, int64_t at);   // a chip's tap at `at` (its id) -> the Commander (cmdr_action; CMDR=1)

// Called once the loop has ended, before the slow teardown (main.c: disarms the watchdog). NULL: nothing.
extern void (*app_stopping)(void);
