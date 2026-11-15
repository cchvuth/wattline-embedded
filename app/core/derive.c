// Panda._onFrame, _applyBt, _forget and the getters of lib/panda.dart (1.3.0), by hand.
// One switch walks an id's signals in kSignals order: several cases read what an earlier one in
// the same frame wrote (gear before brake, indL before indR, tcFlash before escFlash...).
#include "state.h"
#include "signals_gen.h"
#include <string.h>

const car_type_t kCars[CAR_COUNT] = {           // lib/car.dart Car + Model
  [CAR_MODEL3_RWD] = {"model3rwd", 10.6, 2.875, true, SEAT_D | SEAT_P | SEAT_2L | SEAT_2C | SEAT_2R},
  [CAR_MODEL3] = {"model3", 10.6, 2.875, false, SEAT_D | SEAT_P | SEAT_2L | SEAT_2C | SEAT_2R},
  [CAR_Y5_RWD] = {"modelY5rwd", 13.7, 2.89, true, SEAT_D | SEAT_P | SEAT_2L | SEAT_2C | SEAT_2R},
  [CAR_Y5] = {"modelY5", 13.7, 2.89, false, SEAT_D | SEAT_P | SEAT_2L | SEAT_2C | SEAT_2R},
  [CAR_Y7] = {"modelY7", 13.7, 2.89, false, 0x7f},
  [CAR_YL] = {"modelYL", 13.7, 3.04, false, SEAT_D | SEAT_P | SEAT_2L | SEAT_2R | SEAT_3L | SEAT_3R},
};

static const double kPi = 3.14159265358979323846, kG = 9.80665, kSteerShowDeg = 3.0, kBrakeFullNm = 4000.0;
static const char *const kDigits[11] = {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10"};

static double or0(double v) { return isnan(v) ? 0 : v; }
static double clampd(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }
static double smooth(double was, double now) { return isnan(was) ? now : was + 0.3 * (now - was); }
static bool rnd(char g) { return g == 'R' || g == 'N' || g == 'D'; }
static int ri(double v) { return (int)lround(v); }
static double *tyre(car_t *c, int w) { return w == 0 ? &c->tp_fl : w == 1 ? &c->tp_fr : w == 2 ? &c->tp_rl : &c->tp_rr; }

void car_init(car_t *c) {
  memset(c, 0, sizeof *c);
  c->cfg.gear_fresh_ms = 2000;   // 0x118 comes every 10 ms: a P older than this is a stalled link
  c->cfg.grip_hold_ms = 3000;
  c->cfg.steer_hold_ms = 2000;
  c->cfg.yaw_steer_ms = 300;
  c->cfg.bt_motor_bad_ms = 30000;
  c->cfg.bright_stale_ms = 5000;  // 0x273 comes every 500 ms
  c->car = &kCars[CAR_FALLBACK];
  c->kph = true;
  c->p.half_ms = 360;
  c->p.bms_bus = -1;
  c->p.turned_at = c->p.steer_at = c->p.gear_at = c->p.edge_at = c->p.grip_at = c->p.bt_motor_bad_at = NEVER;
  c->p.blink_off_at = NEVER;
  c->utc.at = NEVER;
  c->tp_fl = c->tp_fr = c->tp_rl = c->tp_rr = NUL;
  c->soc_pct = c->range_mi = NUL;
  c->cell_v_max = c->cell_v_min = c->pack_kwh = c->pack_left_kwh = c->pack_buffer_kwh = c->pack_new_kwh = NUL;
  c->outside_c = c->pack_temp_c = NUL;
  c->charge_limit_pct = -1;
  c->p.bt_bright_bad = -1;
  car_forget(c, NEVER);          // everything else as _forget leaves it
  c->p.gear_at = NEVER;          // Panda(): '—' with an epoch-0 stamp
}

void car_set_type(car_t *c, int type) {
  c->car = &kCars[type];
  if (c->car->rwd) c->p.front_at = c->front_kw = NUL;
}

void car_set_gear(car_t *c, char g, int64_t now) { c->gear = g; c->p.gear_at = now; }

void car_set_steer(car_t *c, double v, int64_t now) {
  c->steer_deg = v;
  if (!isnan(v) && fabs(v) >= kSteerShowDeg) c->p.turned_at = now;
}

double car_climate_kw(const car_t *c) {
  if (isnan(c->p.comp_kw) && isnan(c->p.heat_l_kw) && isnan(c->p.heat_r_kw)) return NUL;
  return or0(c->p.comp_kw) + or0(c->p.heat_l_kw) + or0(c->p.heat_r_kw);
}

static bool stopped(const car_t *c) { return c->speed == 0 && or0(c->pedal_pct) == 0; }

// Panda._motorFrame: a motor frame resets the share and nudges the averaged rest.
static void motor_frame(car_t *c) {
  double p = c->pack_kw;
  if (isnan(p) || isnan(c->p.rear_at) || (isnan(c->p.front_at) && !c->car->rwd)) return;
  double rest = clampd(p - or0(c->p.front_at) - c->p.rear_at - or0(car_climate_kw(c)) - or0(c->lv_kw), 0, 5);
  c->other_kw = isnan(c->other_kw) ? rest : c->other_kw + (rest - c->other_kw) * 0.05;
}

// Panda._blend: the motors shown are the pack now less what else it feeds, shared as the last frame.
static void blend(car_t *c) {
  double p = c->pack_kw, other = c->other_kw, f = c->p.front_at, r = c->p.rear_at;
  bool exact = isnan(p) || isnan(other) || c->gear == 'P' || stopped(c);
  if (exact || isnan(r)) {
    c->front_kw = stopped(c) && !isnan(f) ? fmax(0.0, f) : f;
    c->rear_kw = stopped(c) && !isnan(r) ? fmax(0.0, r) : r;
    return;
  }
  double motors = p - or0(car_climate_kw(c)) - or0(c->lv_kw) - other;
  double sum = fabs(or0(f)) + fabs(r), share = sum > 1 ? fabs(r) / sum : 1.0;
  c->rear_kw = motors * share;
  if (!isnan(f)) c->front_kw = motors * (1 - share);
}

// Panda._flasher: each on-edge lights the strips for half the car's own cycle, learnt from edges.
static void flasher(car_t *c, int lit, int64_t now) {
  if (lit & ~c->p.was_lit) {
    int64_t gap = c->p.edge_at == NEVER || lit != c->p.edge_lit ? 0 : now - c->p.edge_at;
    if (gap > 450 && gap < 1100) c->p.half_ms = (c->p.half_ms * 3 + (int)gap / 2) / 4;
    c->p.edge_at = now;
    c->p.edge_lit = lit;
    c->p.blink_lit = true;
    c->p.blink_off_at = now + c->p.half_ms;     // Dart: a Timer; here read lazily by car_blink_lit
  }
  c->p.was_lit = lit;
  if (!c->ind_l && !c->ind_r) {
    c->p.blink_lit = false;
    c->p.edge_at = NEVER;
  }
}

static void pack_heat(car_t *c) { c->pack_heating = c->p.heat_motors || c->p.heat_pump || c->p.heat_nav; }

void car_frame(car_t *c, uint16_t id, uint8_t bus, const uint8_t *d, uint8_t len, int64_t now) {
  if (id == 0x185) {   // all four wheels each frame: none is no reading, never the last one frozen
    for (int i = 0; i < 4; i++) c->p.brake_nm[i] = c->brake_wheel_nm[i] = NUL;
    c->brake_pct = NUL;
  }
  if (id == 0x129) { c->steer_deg = NUL; c->p.steer_at = now; }
  if (id == 0x273 && len >= 5) c->display_brightness = NUL;   // SNA (or a frame without it) is no level
  int n = sub_index(id);
  if (n < 0) return;
  int utc[6], utc_n = 0;
  for (int k = kIdFirst[n]; k < kIdFirst[n + 1]; k++) {
    int s = kIdSigs[k];
    double v;
    if (!sig_decode(&kSignals[s], d, len, &v)) continue;
    switch (s) {
    case SIG_speed: c->speed = v; break;
    case SIG_speedIsKph: c->kph = v == 1; break;
    case SIG_pedal: c->pedal_pct = clampd(v, 0, 100); break;
    case SIG_brake: c->braking = v == 1 && rnd(c->gear); break;   // 'gear' comes first in kSignals
    case SIG_gear: {
      const char *g = LUT(kGears, ri(v));
      char ng = g ? g[0] : 0;
      if (c->gear == 'P' && ng != 'P') c->g_max_accel = c->g_max_brake = c->g_max_lat = 0;  // a new drive
      car_set_gear(c, ng, now);
      break;
    }
    case SIG_accel:
      c->g_long = smooth(c->g_long, v / kG);
      if (c->g_long > c->g_max_accel) c->g_max_accel = c->g_long;
      if (-c->g_long > c->g_max_brake) c->g_max_brake = -c->g_long;
      break;
    case SIG_yaw: {
      double ms = c->kph ? c->speed / 3.6 : c->speed * 0.44704;
      c->g_lat = smooth(c->g_lat, v * ms / kG);
      if (fabs(c->g_lat) > c->g_max_lat) c->g_max_lat = fabs(c->g_lat);
      if (ms >= 3 && c->gear != 'R' && now - c->p.steer_at > c->cfg.yaw_steer_ms)
        car_set_steer(c, c->car->steer_ratio * atan(c->car->wheelbase * v / ms) * 180 / kPi, now);
      break;
    }
    case SIG_recirc: c->recirc = LUT(kRecirc, ri(v)); break;
    case SIG_autopilot: c->autopilot = LUT(kAutopilot, ri(v)); break;
    case SIG_indL: c->ind_l = v != 0; c->p.lit_l = v == 2; break;
    case SIG_indR: c->ind_r = v != 0; flasher(c, (c->p.lit_l ? 1 : 0) | (v == 2 ? 2 : 0), now); break;
    case SIG_blindL: c->blind_l = v == 1 || v == 2; break;
    case SIG_blindR: c->blind_r = v == 1 || v == 2; break;
    case SIG_sideWarn: {
      int w = ri(v);
      c->blind_l = c->blind_l || w == 1 || w == 3;
      c->blind_r = c->blind_r || w == 2 || w == 3;
      break;
    }
    // Page 5 only from VCSEC's 7-byte frame, real readings only; per wheel it wins over 0x25A.
    case SIG_tpFL: case SIG_tpFR: case SIG_tpRL: case SIG_tpRR:
      if (len == 7 && v > 0 && v <= 6.3) {
        int w = s - SIG_tpFL;
        *tyre(c, w) = v;
        c->p.tp219 |= 1 << w;
      }
      break;
    case SIG_tdFL: case SIG_tdFR: case SIG_tdRL: case SIG_tdRR: {
      int w = s - SIG_tdFL;
      if (!(c->p.tp219 & 1 << w) && v > 0 && v <= 6.3) *tyre(c, w) = v;
      break;
    }
    case SIG_navEnergyKwh: c->nav_energy_kwh = fabs(v) > 300 ? NUL : v; break;
    case SIG_navRoute: c->nav_route = v == 1; break;
    case SIG_navMinutes: c->nav_minutes = v >= 1023 ? NUL : v; break;
    case SIG_doorFL: c->door_fl = latch_open(ri(v)); break;
    case SIG_doorFR: c->door_fr = latch_open(ri(v)); break;
    case SIG_doorRL: c->door_rl = latch_open(ri(v)); break;
    case SIG_doorRR: c->door_rr = latch_open(ri(v)); break;
    case SIG_trunk: c->trunk = latch_open(ri(v)); break;
    case SIG_frunk: c->frunk = latch_open(ri(v)); break;
    case SIG_mirrorDipped: c->mirror_dipped = v == 1; break;
    case SIG_tiltAuto: c->tilt_auto = v == 1; break;
    case SIG_chargePort: if (len == 8) c->charge_port = v == 1; break;
    case SIG_mapLights:
      if (len == 7) {
        int i = ri(v);
        c->map_light = (i & 3) == 1 || (i >> 2 & 3) == 1 || (i >> 4 & 3) == 1 || (i >> 6 & 3) == 1;
      }
      break;
    case SIG_hvacPower: c->hvac_on = v != 0; break;
    case SIG_recircPct: c->recirc_on = v >= 60 || (c->recirc_on && v > 40); break;
    case SIG_beltDriver: c->belt_driver = v == 1; break;
    case SIG_beltPass: c->belt_pass = v == 1; break;
    case SIG_belt2L: c->belt_2l = v == 1; break;
    case SIG_belt2C: c->belt_2c = v == 1; break;
    case SIG_belt2R: c->belt_2r = v == 1; break;
    case SIG_belt3L: c->belt_3l = v == 1; break;
    case SIG_belt3R: c->belt_3r = v == 1; break;
    case SIG_buckleD: case SIG_buckle2L: case SIG_buckle2C: case SIG_buckleP:
    case SIG_buckle2R: case SIG_buckle3L: case SIG_buckle3R: {
      int seat = s == SIG_buckleD ? SEAT_D : s == SIG_buckle2L ? SEAT_2L : s == SIG_buckle2C ? SEAT_2C
               : s == SIG_buckleP ? SEAT_P : s == SIG_buckle2R ? SEAT_2R : s == SIG_buckle3L ? SEAT_3L : SEAT_3R;
      if (v == (id == 0x3C2 ? 2 : 1)) c->buckled |= seat;   // VCLEFT's switches 2 ON, VCRIGHT's 1 LATCHED
      else c->buckled &= ~seat;
      break;
    }
    case SIG_lowBeam: c->low_beam = v == 15 ? -1 : v == 4; break;
    case SIG_highBeam:
      c->p.high_fn = v != 0;
      c->high_beam = c->p.high_fn || c->p.high_lamp;
      break;
    case SIG_highLamp: {
      int i = ri(v);
      c->p.high_lamp = (i & 3) == 1 || i >> 2 == 1;
      c->high_beam = c->p.high_fn || c->p.high_lamp;
      break;
    }
    case SIG_drl: c->drl = v == 15 ? -1 : v >= 1 && v <= 4; break;
    case SIG_frontKw:
      if (c->car->rwd) break;
      c->p.front_at = v; motor_frame(c); blend(c);
      break;
    case SIG_rearKw: c->p.rear_at = v; motor_frame(c); blend(c); break;
    case SIG_steerSccm: car_set_steer(c, -v, now); break;   // the frame is positive to the right
    case SIG_brakeFL: case SIG_brakeFR: case SIG_brakeRL: case SIG_brakeRR: {
      c->p.brake_nm[s - SIG_brakeFL] = v;   // the sum once all four are in (after the loop)
      break;
    }
    case SIG_compV:
      c->p.comp_ok = v >= 200 && v <= 600 && fabs(v - (isnan(c->p.pack_v) ? v : c->p.pack_v)) < 40;
      break;
    case SIG_compW: if (c->p.comp_ok && len >= 7) c->p.comp_kw = v / 1000; break;
    case SIG_heatL: c->p.heat_l_kw = v / 1000; break;
    case SIG_heatR: c->p.heat_r_kw = v / 1000; break;
    case SIG_lvV: c->p.lv_v = v; break;
    case SIG_lvA: if (!isnan(c->p.lv_v)) c->lv_kw = c->p.lv_v * v / 1000; break;
    case SIG_packV: c->p.pack_v = v; break;
    case SIG_packA:
      if (!isnan(c->p.pack_v)) { c->pack_kw = -c->p.pack_v * v / 1000; blend(c); }
      break;
    case SIG_soc: c->soc_pct = clampd(v, 0, 100); break;
    case SIG_rangeMi: c->range_mi = v; break;
    case SIG_cellVMax: c->cell_v_max = v > 5 ? NUL : v; break;
    case SIG_cellVMin: c->cell_v_min = v > 5 ? NUL : v; break;
    case SIG_packKwh: c->p.bms_bus = bus; c->pack_kwh = v < 30 || v > 150 ? NUL : v; break;
    case SIG_packLeftKwh: c->pack_left_kwh = v > 150 ? NUL : v; break;
    case SIG_packBufferKwh: c->pack_buffer_kwh = v > 20 ? NUL : v; break;
    case SIG_packNewKwh: if (v >= 40 && v <= 110 && bus == c->p.bms_bus) c->pack_new_kwh = v; break;
    case SIG_domeLight: c->dome_light = LUT(kDome, ri(v)); break;
    case SIG_speedLimit: c->speed_limit = v == 0 || v >= 155 ? NUL : v; break;
    case SIG_outside: c->outside_c = v; break;
    case SIG_washer: c->washer_low = v == 1; break;
    case SIG_slippery: c->p.slip_ui = v == 1; c->slippery = c->p.slip_ui || c->p.slip_mode; break;
    case SIG_tcMode: {
      int m = ri(v);
      c->tc_mode = m == 1 ? "SLIP" : m == 6 ? "OFF-RD" : NULL;
      c->p.slip_mode = m == 7;
      c->slippery = c->p.slip_ui || c->p.slip_mode;
      break;
    }
    case SIG_tcFlash: c->p.tc_flash = v == 1; break;
    case SIG_escFlash:   // after tcFlash; held a while past the last flash
      c->p.esc_flash = v == 1;
      if (c->p.tc_flash || c->p.esc_flash) c->p.grip_at = now;
      c->grip_held = c->p.grip_at != NEVER && now - c->p.grip_at < c->cfg.grip_hold_ms;
      break;
    case SIG_regenMax: c->regen_max_kw = v; break;
    case SIG_packTMin: c->pack_temp_c = v; break;
    case SIG_motorHeat: c->p.heat_motors = v == 2; pack_heat(c); break;
    case SIG_pumpHeat: c->p.heat_pump = v > 0; pack_heat(c); break;
    case SIG_precondState:
      c->p.heat_nav = v == 1;
      pack_heat(c);
      if (!c->p.heat_nav) c->precond_c = NUL;
      break;
    case SIG_precondTarget: if (c->p.heat_nav) c->precond_c = v; break;
    case SIG_co2: c->co2ppm = v >= 5115 ? -1 : ri(v); break;
    case SIG_cabin1: c->cabin_c1 = v; break;
    case SIG_cabin2: c->cabin_c2 = v; break;
    case SIG_cabin3: c->cabin_c3 = v; break;
    case SIG_climateOn: c->p.climate_on = v != 0; break;
    case SIG_autoBlower: c->auto_blower = ri(v); break;
    case SIG_fan1: {   // after climateOn and autoBlower
      int f = ri(v);
      const char *a = LUT(kAutoBlower, c->auto_blower);
      c->fan1 = !c->p.climate_on || f == 0 ? "OFF" : f == 11 ? (a ? a : "AUTO") : f <= 10 ? kDigits[f] : NULL;
      break;
    }
    case SIG_fan2: {
      int f = ri(v);
      const char *a = LUT(kAutoBlower, c->auto_blower);
      c->fan2 = !c->p.climate_on ? "OFF" : f == 0 ? (a ? a : "AUTO") : LUT(kFan2, f);
      break;
    }
    case SIG_handsOn: c->hands_on = v == 15 ? -1 : ri(v); break;
    case SIG_cruiseState: { int m = ri(v); c->p.cruise_on = m == 2 || m == 3 || m == 4; break; }
    case SIG_cruiseSet: c->p.cruise_raw = v; break;
    case SIG_cruiseKph: {   // after the state and the speed: in the display's unit
      bool kmh = v == 1;
      double r = c->p.cruise_raw;
      c->cruise_set = !c->p.cruise_on ? NUL : kmh == c->kph ? r : kmh ? r / 1.609344 : r * 1.609344;
      break;
    }
    case SIG_followGap: c->follow_gap = ri(v) + 1; break;
    case SIG_chargeStatus: c->charging = v == 3; break;
    case SIG_chargeKw: c->charge_kw = v; break;
    case SIG_chargeMaxKw: c->charge_max_kw = v; break;
    case SIG_chargeWhy: c->charge_why = LUT(kChargeWhy, ri(v)); break;
    case SIG_chargeLimit: c->charge_limit_pct = ri(v); break;
    case SIG_chargeMinutes: c->charge_minutes = v >= 2047 ? -1 : ri(v); break;
    case SIG_displayBrightness:
      c->display_brightness = v;
      c->p.bright_at = now;
      c->p.bright_bt = false;
      break;
    // The Dash's 0x318: kept only whole and in range (order in the table: Y M s h D m).
    case SIG_utcYear: case SIG_utcMonth: case SIG_utcSeconds: case SIG_utcHour: case SIG_utcDay:
    case SIG_utcMinutes:
      utc[s - SIG_utcYear] = ri(v); utc_n++;
      break;
    }
  }
  if (id == 0x185) {   // the brake: the four wheels' sum, once the whole frame is read
    double sum = 0;
    int got = 0;
    for (int i = 0; i < 4; i++) if (!isnan(c->p.brake_nm[i])) sum += c->p.brake_nm[i], got++;
    // Every torque shows, the stability control's short touches on one wheel too (also with the accelerator down:
    // worth seeing), and each wheel on its own (the dash's four tyres)
    c->brake_pct = !got ? NUL : sqrt(clampd(sum / kBrakeFullNm, 0.0, 1.0)) * 100;
    for (int i = 0; i < 4; i++) c->brake_wheel_nm[i] = !got ? NUL : or0(c->p.brake_nm[i]);
  }
  if (utc_n == 6 && utc[0] >= 2025 && utc[0] <= 2099 && utc[1] >= 1 && utc[1] <= 12 && utc[4] >= 1 && utc[4] <= 31 &&
      utc[3] < 24 && utc[5] < 60 && utc[2] < 60) {   // a car clock not yet set (year 0, 255) sets nothing
    c->utc.year = utc[0]; c->utc.month = utc[1]; c->utc.second = utc[2];
    c->utc.hour = utc[3]; c->utc.day = utc[4]; c->utc.minute = utc[5];
    c->utc.at = now;
  }
}

void car_forget(car_t *c, int64_t now) {
  car_set_gear(c, 0, now);
  c->braking = c->ind_l = c->ind_r = c->blind_l = c->blind_r = false;
  if (!c->p.bt_up) {   // Bluetooth carrying the lamps keeps the flasher's beat
    c->p.bt_motor_bad_at = NEVER;
    c->p.blink_lit = c->p.lit_l = false;
    c->p.was_lit = 0;
    c->p.edge_at = NEVER;
  }
  c->trunk = c->frunk = c->mirror_dipped = c->tilt_auto = c->charge_port = c->map_light = false;
  c->hvac_on = c->recirc_on = false;
  c->p.tp219 = 0;
  c->door_fl = c->door_fr = c->door_rl = c->door_rr = false;
  c->belt_driver = c->belt_pass = c->belt_2l = c->belt_2c = c->belt_2r = c->belt_3l = c->belt_3r = false;
  c->buckled = 0;
  c->recirc = c->dome_light = c->autopilot = NULL;
  c->low_beam = c->high_beam = c->drl = -1;
  c->p.high_fn = c->p.high_lamp = false;
  c->nav_route = false;
  c->nav_energy_kwh = c->nav_minutes = c->speed_limit = NUL;
  c->slippery = c->grip_held = c->pack_heating = c->charging = c->washer_low = false;
  c->co2ppm = -1;
  c->cabin_c1 = c->cabin_c2 = c->cabin_c3 = NUL;
  c->fan1 = c->fan2 = NULL;
  c->g_long = c->g_lat = NUL;
  c->p.slip_ui = c->p.slip_mode = c->p.tc_flash = c->p.esc_flash = c->p.heat_motors = c->p.heat_pump = false;
  c->p.heat_nav = c->p.cruise_on = c->p.climate_on = false;
  c->auto_blower = -1;
  c->p.grip_at = NEVER;
  c->tc_mode = c->charge_why = NULL;
  c->hands_on = c->follow_gap = c->charge_minutes = -1;
  c->cruise_set = c->precond_c = c->charge_kw = c->charge_max_kw = c->regen_max_kw = NUL;
  c->speed = 0;
  c->front_kw = c->rear_kw = c->pedal_pct = c->pack_kw = c->p.pack_v = c->p.front_at = c->p.rear_at = NUL;
  c->other_kw = NUL;
  c->lv_kw = c->p.comp_kw = c->p.heat_l_kw = c->p.heat_r_kw = c->p.lv_v = NUL;
  c->brake_pct = NUL;
  for (int i = 0; i < 4; i++) c->brake_wheel_nm[i] = NUL;
  c->display_brightness = NUL;
  c->p.bright_bt = false;
  car_set_steer(c, NUL, now);
  for (int i = 0; i < 4; i++) c->p.brake_nm[i] = NUL;
  c->p.comp_ok = false;
}

// ---- Bluetooth: the Commander copies the car's codes and raw counts (Panda._applyBt) ----
static bool at(const car_t *c, int f, int64_t *v) {
  if (f >= BT_FIELDS || !c->p.bt.has[f] || !c->p.bt.is_int[f]) return false;
  *v = c->p.bt.v[f];
  return true;
}
static int lamp(const car_t *c, int f) {     // -1 none; two LightStatus nibbles, 1 is ON
  int64_t v;
  return at(c, f, &v) ? ((v & 15) == 1 || (v >> 4 & 15) == 1) : -1;
}
static int latch(const car_t *c, int f) {    // -1 none or SNA 0
  int64_t v;
  return at(c, f, &v) && v != 0 ? latch_open((int)v) : -1;
}
static bool zz(const car_t *c, int f, int64_t *v) {   // sint32 comes zigzagged
  if (!at(c, f, v)) return false;
  *v = (*v >> 1) ^ -(*v & 1);
  return true;
}
static double kw(const car_t *c, int f) {    // 0.5 kW a count; -1024 SNA
  int64_t v;
  return zz(c, f, &v) && v != -1024 ? v * 0.5 : NUL;
}

// Panda._btMotors: values past the motors' limits mark the readout bad a while; output in P is dropped.
static void bt_motors(car_t *c, double f, double r, int64_t now) {
  if (c->car->rwd) f = NUL;
  if (isnan(f) && isnan(r)) return;
#define OFF(v, max) (!isnan(v) && ((v) < -200 || (v) > (max)))
#define MOVING(v) (!isnan(v) && fabs(v) > 3)
  if (OFF(f, 210) || OFF(r, 400)) c->p.bt_motor_bad_at = now;
  int64_t bad = c->p.bt_motor_bad_at;
  if ((bad != NEVER && now - bad < c->cfg.bt_motor_bad_ms) || (c->gear == 'P' && (MOVING(f) || MOVING(r)))) {
    c->front_kw = c->rear_kw = NUL;
    return;
  }
  if (!isnan(f)) c->front_kw = f;
  if (!isnan(r)) c->rear_kw = r;
}

void car_apply_bt(car_t *c, int64_t now) {
  int64_t v;
  int b;
  if (at(c, 61, &v) && v < 511) c->speed = (double)v;   // 511 SNA
  if (at(c, 62, &v) && v != 255) c->pedal_pct = clampd(v * 0.4, 0, 100);
  if (at(c, 66, &v)) { const char *g = v >= 0 && v < 5 ? kGears[v] : NULL; car_set_gear(c, g ? g[0] : 0, now); }
  if (at(c, 65, &v)) c->braking = v == 1 && rnd(c->gear);   // 2 INVALID is not braking
  int64_t l = 0, r = 0;
  at(c, 63, &l);
  at(c, 64, &r);
  c->ind_l = l != 0;
  c->ind_r = r != 0;
  flasher(c, (l == 2 ? 1 : 0) | (r == 2 ? 2 : 0), now);
  if (at(c, 106, &v)) c->blind_l = v == 1 || v == 2;
  if (at(c, 107, &v)) c->blind_r = v == 1 || v == 2;
  bt_motors(c, kw(c, 51), kw(c, 53), now);
  if (at(c, 111, &v)) c->soc_pct = clampd((double)v, 0, 100);
  if (at(c, 112, &v)) c->range_mi = (double)v;   // miles, always
  if (at(c, 110, &v)) c->speed_limit = v == 0 || v >= 155 ? NUL : (double)v;
  if (at(c, 105, &v)) c->autopilot = v == 1 ? "TACC" : v == 2 || v == 3 ? "AUTOPILOT" : NULL;
  if ((b = lamp(c, 81)) >= 0) c->low_beam = b;
  if ((b = lamp(c, 82)) >= 0) c->high_beam = b;
  int day = lamp(c, 80), night = lamp(c, 85);
  if (day >= 0 || night >= 0) c->drl = day == 1 || night == 1;
  if ((b = latch(c, 113)) >= 0) c->door_fl = b;
  if ((b = latch(c, 114)) >= 0) c->door_fr = b;
  if ((b = latch(c, 115)) >= 0) c->door_rl = b;
  if ((b = latch(c, 116)) >= 0) c->door_rr = b;
  if ((b = latch(c, 117)) >= 0) c->frunk = b;
  if ((b = latch(c, 118)) >= 0) c->trunk = b;
  if (at(c, 34, &v) && v >= 400 && v <= 1100) c->pack_new_kwh = v * 0.1;
  if (at(c, 35, &v)) c->pack_kwh = v * 0.02 < 30 || v * 0.02 > 150 ? NUL : v * 0.02;
  if (at(c, 36, &v)) c->pack_left_kwh = v * 0.02 > 150 ? NUL : v * 0.02;
  if (at(c, 37, &v)) c->pack_buffer_kwh = v * 0.01 > 20 ? NUL : v * 0.01;
  if (at(c, 43, &v)) c->cell_v_max = v * 0.002 > 5 ? NUL : v * 0.002;
  if (at(c, 44, &v)) c->cell_v_min = v * 0.002 > 5 ? NUL : v * 0.002;
  if (zz(c, 27, &v)) c->outside_c = (double)v;
  if (at(c, 32, &v)) c->regen_max_kw = v * 0.01;
  if (zz(c, 42, &v)) c->pack_temp_c = v * 0.5;
  if (at(c, 49, &v)) c->pack_heating = v == 1;
  if (at(c, 108, &v)) c->hands_on = v == 15 ? -1 : (int)v;
  if (at(c, 24, &v)) {   // display_brightness, uint32, unit unverified: taken as % when it fits
    if (v >= 0 && v <= 100) {
      c->display_brightness = (double)v;
      c->p.bright_at = now;
      c->p.bright_bt = true;
    } else {
      c->p.bt_bright_bad = v;
    }
  }
}

void car_from_bt(car_t *c, const bt_map_t *push, int64_t now) {
  if (!push) {
    c->p.bt_up = false;
    memset(&c->p.bt, 0, sizeof c->p.bt);
    if (c->wifi_live) return;
    car_forget(c, now);   // its values go with it
    return;
  }
  for (int f = 0; f < BT_FIELDS; f++)
    if (push->has[f]) { c->p.bt.has[f] = 1; c->p.bt.is_int[f] = push->is_int[f]; c->p.bt.v[f] = push->v[f]; }
  c->p.bt_up = true;
  if (c->wifi_live) return;   // WiFi first: kept, applied once WiFi stops
  car_apply_bt(c, now);
}

// ---- getters ----
bool car_via_bt(const car_t *c) {
  if (c->wifi_live || !c->p.bt_up) return false;
  for (int i = 0; i < kBtFieldCount; i++) if (c->p.bt.has[kBtFields[i]]) return true;
  return false;
}
bool car_live(const car_t *c) { return c->wifi_live || car_via_bt(c); }

// What lets the frunk, trunk and doors open: P, under gear_fresh_ms old on the monotonic clock, or
// a live Bluetooth session pushing it. Anything else (no gear yet, stalled link, clock odd) is false.
bool car_parked(const car_t *c, int64_t now) {
  if (c->gear != 'P' || (!car_via_bt(c) && c->speed > 0.5)) return false;   // P while it moves (WiFi): a stale gear. Over
  // Bluetooth a speed only changes when a push carries it (a 0 can come as an empty push): no speed check there
  if (car_via_bt(c)) return true;
  return c->p.gear_at != NEVER && now >= c->p.gear_at && now - c->p.gear_at < c->cfg.gear_fresh_ms;
}

bool car_blink_lit(const car_t *c, int64_t now) { return c->p.blink_lit && now < c->p.blink_off_at; }

bool car_steer_lines(const car_t *c, int64_t now) {
  if (isnan(c->steer_deg)) return false;
  return fabs(c->steer_deg) >= kSteerShowDeg ||
         (c->p.turned_at != NEVER && now - c->p.turned_at < c->cfg.steer_hold_ms);
}

bool car_grip_lit(const car_t *c) { return c->speed >= 1 && (c->grip_held || c->slippery); }
bool car_front_lit(const car_t *c) { return c->drl == 1 || c->low_beam == 1 || c->high_beam == 1; }

bool car_unbuckled_at(const car_t *c, int seat) {
  switch (seat) {
  case SEAT_D: return c->belt_driver;
  case SEAT_P: return c->belt_pass;
  case SEAT_2L: return c->belt_2l;
  case SEAT_2C: return c->belt_2c;
  case SEAT_2R: return c->belt_2r;
  case SEAT_3L: return c->belt_3l;
  case SEAT_3R: return c->belt_3r;
  default: return false;
  }
}
bool car_any_belt_unbuckled(const car_t *c) {
  for (int s = 1; s <= SEAT_3R; s <<= 1) if ((c->car->seats & s) && car_unbuckled_at(c, s)) return true;
  return false;
}

double car_display_brightness(const car_t *c, int64_t now) {
  double v = c->display_brightness;
  if (isnan(v)) return NUL;
  // pushed on a change: any age while the session is up, also once WiFi went live, until its own 0x273 replaces it
  // (else the lamps' day level would flash in between)
  if (c->p.bright_bt) return c->p.bt_up ? v : NUL;
  return now >= c->p.bright_at && now - c->p.bright_at <= c->cfg.bright_stale_ms ? v : NUL;
}

double car_imbalance_mv(const car_t *c) { return (c->cell_v_max - c->cell_v_min) * 1000; }   // NUL in, NUL out
double car_range(const car_t *c) { return c->kph ? c->range_mi * 1.609344 : c->range_mi; }

double car_arrive_pct(const car_t *c) {
  double e = c->nav_energy_kwh, full = c->pack_kwh, buf = c->pack_buffer_kwh;
  if (isnan(e) || isnan(full) || isnan(buf) || full <= buf) return NUL;
  return clampd((e - buf) / (full - buf) * 100, 0, 100);
}

double car_range_at(const car_t *c, double pct) {
  double full = c->pack_kwh, left = c->pack_left_kwh, buf = c->pack_buffer_kwh, r = car_range(c);
  if (isnan(pct) || isnan(full) || isnan(left) || isnan(buf) || isnan(r)) return NUL;
  if (left <= buf || full <= buf) return NUL;
  return clampd(r * pct / ((left - buf) / (full - buf) * 100), 0, r);
}

double car_trip_kwh(const car_t *c, double pct) {
  double full = c->pack_kwh, left = c->pack_left_kwh, buf = c->pack_buffer_kwh, e = c->nav_energy_kwh;
  double to = c->wifi_live && !isnan(e) ? e
            : isnan(pct) || isnan(full) || isnan(buf) ? NUL : buf + pct / 100 * (full - buf);
  return isnan(left) || isnan(to) ? NUL : fmax(0.0, left - to);
}
