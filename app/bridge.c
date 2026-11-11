// The car state (core/state.h) as the dash shows it (ui/dash_state.h), deciding what lib/main.dart and
// lib/widgets.dart (1.3.0) decide: which link gates what, unknowns, the derived figures. What the UI itself gates
// (live / wifi_live on the lamps, chips, cue, g-meter...) is passed through as the app's value.
#include "app.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

void bt_view_none(bt_view_t *b) {
  b->link = BT_IDLE;
  b->dest = NULL;
  b->nav_minutes = b->nav_miles = b->nav_arrive_pct = NAN;
  b->recirc_mode = -1;
}

static bool seen(const bridge_in_t *in, unsigned id) { return in->seen && in->seen[id >> 3] >> (id & 7) & 1; }
static double or0(double v) { return isnan(v) ? 0 : v; }
static void text(char *dst, size_t n, const char *src) { snprintf(dst, n, "%s", src ? src : ""); }

// TimeOfDay.format: "1:51 AM" (12 h) or "01:51" (24 h), local time in the TZ set at start. "" without a clock.
static void clock_in(char *out, size_t n, const bridge_in_t *in, double seconds) {
  out[0] = 0;
  if (in->utc_ms < 0) return;
  time_t t = (time_t)(in->utc_ms / 1000 + (int64_t)lround(seconds));
  struct tm tm;
  if (!localtime_r(&t, &tm)) return;
  if (in->hour12) snprintf(out, n, "%d:%02d %s", (tm.tm_hour + 11) % 12 + 1, tm.tm_min, tm.tm_hour < 12 ? "AM" : "PM");
  else snprintf(out, n, "%02d:%02d", tm.tm_hour, tm.tm_min);
}

// main.dart _autopilot.
static dash_ap_t ap_of(const car_t *c, bool live) {
  switch (live ? c->hands_on : -1) {
  case 3: return AP_HOLD;
  case 4: case 5: case 9: case 10: return AP_HOLD_RED;
  case 6: return AP_SLOWING;
  case 7: return AP_LOCKED;
  }
  const char *a = c->autopilot;
  if (a && !strcmp(a, "AP READY") && !isnan(c->cruise_set)) return AP_TACC;   // on WiFi: ready with cruise holding
  if (!a) return AP_NONE;
  if (!strcmp(a, "TACC")) return AP_TACC;
  if (!strcmp(a, "AUTOPILOT") || !strcmp(a, "NAV ON AP") || !strcmp(a, "FSD")) return AP_STEERING;
  if (!strcmp(a, "AP ENDING")) return AP_TAKEOVER;
  if (!strcmp(a, "AP FAULT")) return AP_FAULT;
  return AP_READY;   // ready, or just ended
}

static dash_wifi_t wifi_of(const link_t *l) {
  switch (l->state) {
  case LINK_HELLO: return WIFI_HELLO;
  case LINK_LIVE: return WIFI_LIVE;
  case LINK_QUIET: return WIFI_QUIET;
  case LINK_RETRYING: return WIFI_RETRYING;
  default: return WIFI_IDLE;
  }
}

// widgets.dart _Nav (the full layout's bar; on the Dash its own column).
static void route(dash_state_t *s, const car_t *c, const bt_view_t *bt, const bridge_in_t *in) {
  bool bus = c->wifi_live;   // without WiFi its route values are stale
  double mins = bus && or0(c->nav_minutes) > 0 ? c->nav_minutes : or0(bt->nav_minutes);
  s->route = bus && seen(in, 0x238) ? c->nav_route : bt->dest != NULL || mins > 0;
  if (bus && c->charging) s->route = false;   // charging takes the route's place (_Controls)
  bool trip = mins > 0;
  double pct = NAN;
  if (trip) {
    pct = bus ? car_arrive_pct(c) : NAN;
    if (isnan(pct)) pct = bt->nav_arrive_pct;
  }
  double kwh = bus ? c->nav_energy_kwh : NAN, left = trip ? car_range_at(c, pct) : NAN, range = car_range(c);
  s->arrive_range = left;
  s->range_drop = isnan(left) || isnan(range) ? 0 : lround(range - left);
  double mi = trip ? bt->nav_miles : NAN;
  s->route_len = c->kph ? mi * 1.609344 : mi;
  s->route_kwh = trip ? car_trip_kwh(c, pct) : NAN;
  s->arrive_pct = pct;
  s->arrive_kwh = isnan(pct) ? kwh : NAN;
  s->pct_drop = isnan(pct) || isnan(c->soc_pct) ? 0 : lround(c->soc_pct - pct);
  s->nav_minutes = trip ? (int16_t)lround(mins) : -1;
  if (trip) clock_in(s->arrive_clock, sizeof s->arrive_clock, in, mins * 60);
  text(s->dest, sizeof s->dest, bt->dest);
}

void bridge_fill(dash_state_t *s, const car_t *c, const link_t *l, const bt_view_t *bt, const bridge_in_t *in,
                 int64_t now) {
  dash_state_blank(s);
  bool live = car_live(c), wl = c->wifi_live;
  s->live = live;
  s->wifi_live = wl;
  s->wifi = wifi_of(l);
  s->bt = bt->link;
  s->speed_hz = l->state == LINK_LIVE && l->speed_known ? l->speed_hz : -1;
  int t = (int)(c->car - kCars);
  s->car = t == CAR_Y7 ? DASH_CAR_Y7 : t == CAR_YL ? DASH_CAR_YL : t == CAR_Y5 || t == CAR_Y5_RWD ? DASH_CAR_Y5 : DASH_CAR_3;
  s->rwd = c->car->rwd;
  s->kph = c->kph;
  s->minor_figures = in->minor_figures;

  // The ring (_powerRing, _Centre): a stalled link blanks speed, power and pedal.
  s->speed = c->speed;   // the UI shows it only while live
  s->gear = c->gear;
  s->front_kw = live ? c->front_kw : NAN;
  s->rear_kw = live ? c->rear_kw : NAN;
  s->pedal_pct = live ? c->pedal_pct : NAN;
  s->brake_pct = wl ? c->brake_pct : NAN;   // WiFi's alone: frozen once it stalls
  s->steer_lines = wl && car_steer_lines(c, now);   // WiFi's alone: ID129 or the yaw rate
  s->steer_deg = s->steer_lines ? (float)c->steer_deg : 0;
  s->regen_cap_kw = live && c->gear != 'P' ? c->regen_max_kw : NAN;   // no regen parked
  s->speed_limit = live ? c->speed_limit : NAN;
  s->ap = ap_of(c, live);
  s->cruise_set = c->cruise_set;
  s->follow_gap = (int8_t)c->follow_gap;
  s->washer_low = c->washer_low;
  s->outside_c = c->outside_c;

  // Charging: the kW in the ring, the cue, the charge card (WiFi; the UI gates on wifi_live).
  s->charging = c->charging;
  s->charge_kw = c->charge_kw;
  s->charge_max_kw = c->charge_max_kw;
  text(s->charge_why, sizeof s->charge_why, c->charge_why);
  s->charge_limit_pct = (int8_t)c->charge_limit_pct;
  s->charge_minutes = (int16_t)c->charge_minutes;
  if (c->charge_minutes >= 0) clock_in(s->charge_clock, sizeof s->charge_clock, in, c->charge_minutes * 60.0);

  // The battery dial and the figures (_Battery, _Draws, _Pack).
  s->soc_pct = c->soc_pct;
  s->range = car_range(c);
  s->pack_temp_c = c->pack_temp_c;
  s->precond_c = c->precond_c;
  s->pack_heating = c->pack_heating;
  s->draws = !isnan(c->other_kw);   // _figures: wifiLive && otherKw != null
  s->climate_kw = car_climate_kw(c);
  s->lv_kw = c->lv_kw;
  s->other_kw = c->other_kw;
  s->imbalance_mv = car_imbalance_mv(c);
  double kwh = c->pack_kwh, nw = c->pack_new_kwh;
  s->degradation_pct = isnan(kwh) || isnan(nw) ? NAN : fmin(100, fmax(0, (1 - kwh / nw) * 100));

  // The g-meter (_GBall): shown with g known; the UI adds WiFi and not P.
  s->g_shown = !isnan(c->g_long) || !isnan(c->g_lat);
  s->g_lat = c->g_lat;
  s->g_long = c->g_long;
  s->g_peak_brake = c->g_max_brake;
  s->g_peak_accel = c->g_max_accel;
  s->g_peak_lat = c->g_max_lat;

  // Lamps and glows (_Lights, the edges): the indicators lit in step with the car's flasher.
  s->drl = c->drl == 1;
  s->low_beam = c->low_beam == 1;
  s->high_beam = c->high_beam == 1;
  s->braking = c->braking;
  bool blink = car_blink_lit(c, now);
  s->ind_l = c->ind_l && blink;
  s->ind_r = c->ind_r && blink;
  s->blind_l = c->blind_l;
  s->blind_r = c->blind_r;
  if (seen(in, 0x2B6) && (car_grip_lit(c) || c->tc_mode)) {
    s->grip = car_grip_lit(c) && !c->grip_held ? GRIP_SLIPPERY : GRIP_HELD;
    text(s->grip_word, sizeof s->grip_word, c->tc_mode ? c->tc_mode : "GRIP");
  }

  // The car picture (_Cabin): the seat map once the chime's frame is in; none at all on Bluetooth alone.
  s->cabin = wl && seen(in, 0x3A1);
  s->no_cabin = car_via_bt(c);
  for (int i = 0; i < DS_SEAT_COUNT; i++) {
    int bit = 1 << i;   // DS_SEAT_* and SEAT_* share the order
    s->seat[i] = !(c->car->seats & bit) ? DS_SEAT_PLAIN : car_unbuckled_at(c, bit) ? DS_SEAT_UNBUCKLED
               : c->buckled & bit ? DS_SEAT_BUCKLED : DS_SEAT_PLAIN;
  }
  const double tp[4] = {c->tp_fl, c->tp_fr, c->tp_rl, c->tp_rr};
  for (int i = 0; i < 4; i++) s->tyre_psi[i] = tp[i] * 14.5038;   // bar to psi; NAN stays NAN
  s->cabin_c[0] = c->cabin_c1, s->cabin_c[1] = c->cabin_c2, s->cabin_c[2] = c->cabin_c3;
  text(s->fan[0], sizeof s->fan[0], c->fan1);
  text(s->fan[1], sizeof s->fan[1], c->fan2);

  // The chips (_Actions).
  s->trunk = c->trunk;
  s->mirror_tilt = c->gear == 'R' ? c->mirror_dipped : c->tilt_auto;
  s->door_fl = c->door_fl, s->door_rl = c->door_rl, s->door_fr = c->door_fr, s->door_rr = c->door_rr;
  s->charge_port = c->charge_port;
  s->frunk = c->frunk;
  s->recirc = wl && seen(in, 0x243) ? c->hvac_on && c->recirc_on : bt->recirc_mode >= 0 && (bt->recirc_mode & 0x7f) == 1;
  s->interior = (c->dome_light && !strcmp(c->dome_light, "ON")) || c->map_light;
  s->co2_ppm = (int16_t)c->co2ppm;   // the UI blanks it off WiFi, its slot kept

  route(s, c, bt, in);
}
