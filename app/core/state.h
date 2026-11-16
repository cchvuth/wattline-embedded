// The car's state as lib/panda.dart (1.3.0) derives it from CAN frames (car_frame) and Commander
// pushes (car_from_bt). Field names follow Panda's. Plain data: the UI reads it, derive.c writes it.
// Time is passed in: CLOCK_MONOTONIC milliseconds from the caller, never read here.
#pragma once
#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#define NUL NAN                    // a double with no value (Dart null): test with isnan()
#define NEVER (INT64_MIN / 4)      // a time that never was: any age check against it fails

// lib/car.dart: what tells the cars apart. seats: SEAT_* bits of the seats the car has.
enum { SEAT_D = 1, SEAT_P = 2, SEAT_2L = 4, SEAT_2C = 8, SEAT_2R = 16, SEAT_3L = 32, SEAT_3R = 64 };
typedef struct { const char *id; double steer_ratio, wheelbase; bool rwd; uint8_t seats; } car_type_t;
enum { CAR_MODEL3_RWD, CAR_MODEL3, CAR_Y5_RWD, CAR_Y5, CAR_Y7, CAR_YL, CAR_COUNT, CAR_FALLBACK = CAR_Y7 };
extern const car_type_t kCars[CAR_COUNT];

// A Commander push: field number -> value (only ints are read; has[] alone counts for viaBt).
enum { BT_FIELDS = 160 };   // up to the cabin's 159
typedef struct { uint8_t has[BT_FIELDS], is_int[BT_FIELDS]; int64_t v[BT_FIELDS]; } bt_map_t;

typedef struct {
  // Times the derivations hold, configurable (PLAN 5.3: nothing hard-coded in the logic).
  struct { int gear_fresh_ms, grip_hold_ms, steer_hold_ms, yaw_steer_ms, bt_motor_bad_ms, bright_stale_ms; } cfg;
  const car_type_t *car;
  bool wifi_live;               // written by the link (sched.c): frames are arriving over WiFi

  double speed;                 // display units; 0 when none
  double front_kw, rear_kw, pack_kw, lv_kw, other_kw;   // other_kw: Panda.otherKw
  double pedal_pct, brake_pct, steer_deg;
  double brake_wheel_nm[4];     // each wheel's brake torque (FL FR RL RR), Nm; NUL unknown
  double soc_pct, range_mi, speed_limit;
  bool braking, kph;
  const char *recirc, *autopilot, *dome_light, *tc_mode, *charge_why, *fan1, *fan2;
  char gear;                    // 'P' 'R' 'N' 'D', or 0 for none ("—")
  bool ind_l, ind_r;            // the flasher's strips: car_blink_lit(now)
  bool blind_l, blind_r;
  double tp_fl, tp_fr, tp_rl, tp_rr;
  bool nav_route;
  double nav_energy_kwh, nav_minutes;
  bool door_fl, door_fr, door_rl, door_rr, trunk, frunk, mirror_dipped, tilt_auto, charge_port, map_light;
  bool hvac_on, recirc_on;
  bool belt_driver, belt_pass, belt_2l, belt_2c, belt_2r, belt_3l, belt_3r;  // the chime: unbuckled
  uint8_t buckled;              // SEAT_* bits
  int8_t low_beam, high_beam, drl;   // -1 unknown, 0, 1
  double cell_v_max, cell_v_min, pack_kwh, pack_left_kwh, pack_buffer_kwh, pack_new_kwh;
  double outside_c;
  bool washer_low, slippery, grip_held;
  double g_long, g_lat, g_max_accel, g_max_brake, g_max_lat;
  double pack_temp_c, precond_c;
  bool pack_heating;
  int co2ppm;                   // -1 none, as every int below
  double cabin_c1, cabin_c2, cabin_c3;
  int auto_blower;
  double regen_max_kw;
  int hands_on;
  double cruise_set;
  int follow_gap;
  bool charging;
  double charge_kw, charge_max_kw;
  int charge_limit_pct, charge_minutes;
  // The Dash's own: the car's screen brightness, % (0x273 UI_displayBrightnessLevel, or Commander field 24).
  // NUL unknown or SNA; read it through car_display_brightness(now), which also drops a stale one.
  double display_brightness;
  // The Dash's own: 0x318 SystemTimeUTC (tools/extra_signals.json). utc_at: NEVER until a valid frame.
  struct { int year, month, day, hour, minute, second; int64_t at; } utc;

  // What one frame says that the next needs (Panda's private fields).
  struct {
    double comp_kw, heat_l_kw, heat_r_kw, lv_v, pack_v, front_at, rear_at;
    bool comp_ok, lit_l, high_fn, high_lamp;
    double brake_nm[4];         // NUL: that wheel not in the last frame
    int64_t turned_at, steer_at, gear_at, edge_at, blink_off_at, grip_at, bt_motor_bad_at;
    bool blink_lit;
    int was_lit, edge_lit, half_ms;
    uint8_t tp219;              // bits FL FR RL RR: 0x219 page 5 has given that wheel
    int bms_bus;                // -1 until the battery's own 0x352
    bool slip_ui, slip_mode, tc_flash, esc_flash, heat_motors, heat_pump, heat_nav, cruise_on, climate_on;
    double cruise_raw;
    bool bt_up;
    int64_t bright_at;          // display_brightness's last write
    int64_t bright_can_at;      // the last 0x273 frame's (the car's own word on it over WiFi; Bluetooth never moves it)
    bool bright_bt;             // ... and it came from the Commander (pushed on a change: no age limit)
    int64_t bt_bright_bad;      // field 24 seen out of 0..100 (unit unverified): the raw value, for one log line; -1 none
    bt_map_t bt;                // the Commander's pushes merged
  } p;
} car_t;

void car_init(car_t *c);                             // Panda(): fallback car, km/h, nothing known
void car_set_type(car_t *c, int type);               // Panda.car =
void car_frame(car_t *c, uint16_t id, uint8_t bus, const uint8_t *d, uint8_t len, int64_t now);
void car_forget(car_t *c, int64_t now);              // the link gone: nothing stale stays
void car_from_bt(car_t *c, const bt_map_t *push, int64_t now);  // NULL: the Bluetooth link dropped
void car_apply_bt(car_t *c, int64_t now);            // the link changed state: BT values stand in
void car_set_gear(car_t *c, char g, int64_t now);
void car_set_steer(car_t *c, double deg, int64_t now);

bool car_parked(const car_t *c, int64_t now);        // fails closed (PLAN 5.3)
bool car_via_bt(const car_t *c);
bool car_live(const car_t *c);
bool car_bt_has(const car_t *c, int f);   // the Commander pushed field f this session
bool car_blink_lit(const car_t *c, int64_t now);
bool car_steer_lines(const car_t *c, int64_t now);
bool car_grip_lit(const car_t *c);
bool car_front_lit(const car_t *c);
bool car_unbuckled_at(const car_t *c, int seat);     // a SEAT_* bit
bool car_any_belt_unbuckled(const car_t *c);
double car_display_brightness(const car_t *c, int64_t now);   // %, NUL unknown, SNA or stale
double car_climate_kw(const car_t *c);
double car_imbalance_mv(const car_t *c);
double car_range(const car_t *c);
double car_arrive_pct(const car_t *c);
double car_range_at(const car_t *c, double pct);
double car_trip_kwh(const car_t *c, double pct);
