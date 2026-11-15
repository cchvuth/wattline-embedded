/* The app's demo scenes (tesla-dash test/dash_shot_test.dart), as the values the dash shows. The derived figures
 * (range in km, kWh, arrival, climate draw, degradation) are what the app works out from each scene's frames. */
#include "scenes.h"
#include <math.h>
#include <string.h>

static void car(dash_state_t *s) /* _car(): live on WiFi, the Commander ready, DRL on, tyres, the pack */
{
	dash_state_blank(s);
	s->live = s->wifi_live = true;
	s->wifi = WIFI_LIVE;
	s->bt = BT_LEAN;
	s->drl = true;
	s->tyre_psi[0] = s->tyre_psi[1] = s->tyre_psi[3] = 2.6f * 14.5038f;
	s->tyre_psi[2] = 2.65f * 14.5038f;
	s->degradation_pct = (1 - 81.76f / 84) * 100;
	s->imbalance_mv = (3.902f - 3.898f) * 1000;
	s->car = DASH_CAR_Y7;
}
static void seen(dash_state_t *s) { s->cabin = true; }
static void buckle(dash_state_t *s, const int *seats, int n)
{
	for (int i = 0; i < n; i++) s->seat[seats[i]] = DS_SEAT_BUCKLED;
}
#define BUCKLE(s, ...) buckle(s, (const int[]){__VA_ARGS__}, sizeof((const int[]){__VA_ARGS__}) / sizeof(int))
#define TEXT(dst, src) strncpy(dst, src, sizeof dst - 1)

static void cold_morning(dash_state_t *s)
{
	car(s);
	s->speed = 38, s->gear = 'D', s->front_kw = -3, s->rear_kw = -18, s->pedal_pct = 0;
	s->soc_pct = 64, s->range = 193 * 1.609344f, s->speed_limit = 50, s->ap = AP_READY;
	s->ind_r = true, s->low_beam = true, s->outside_c = -1.5f, s->washer_low = true;
	s->grip = GRIP_SLIPPERY;
	s->pack_temp_c = 3.5f, s->pack_heating = true, s->regen_cap_kw = 24;
	s->draws = true, s->climate_kw = 2.9f, s->lv_kw = 14.63f * 54.6f / 1000, s->other_kw = 1.5f; /* the pack heating */
	s->recirc = true, s->co2_ppm = 1650;
	s->cabin_c[0] = 21, s->cabin_c[1] = 19, s->cabin_c[2] = 16;
	TEXT(s->fan[0], "MED"), TEXT(s->fan[1], "MED");
	seen(s);
	BUCKLE(s, DS_SEAT_D, DS_SEAT_P, DS_SEAT_2L, DS_SEAT_2C, DS_SEAT_2R, DS_SEAT_3R);
	s->seat[DS_SEAT_3L] = DS_SEAT_UNBUCKLED;
	s->route = true, s->nav_minutes = 14;
	TEXT(s->arrive_clock, "1:30 AM");
	s->route_kwh = 50.62f - 47.9f, s->arrive_range = 293, s->range_drop = 18, s->arrive_pct = 57, s->pct_drop = 7;
	s->g_shown = true, s->g_long = -0.1f, s->g_lat = 0.02f;
	s->g_peak_accel = 0.18f, s->g_peak_brake = 0.22f, s->g_peak_lat = 0.15f;
}

static void highway_cruise(dash_state_t *s)
{
	car(s);
	s->speed = 108, s->gear = 'D', s->front_kw = 18, s->rear_kw = 24, s->pedal_pct = 0;
	s->soc_pct = 71, s->range = 214 * 1.609344f, s->speed_limit = 110;
	s->ap = AP_HOLD, s->cruise_set = 110, s->follow_gap = 3; /* AUTOPILOT, hands-on nag 3 */
	s->low_beam = true, s->outside_c = 4.5f, s->pack_temp_c = 24;
	s->draws = true, s->climate_kw = 1.7f, s->co2_ppm = 820;
	s->lv_kw = 14.63f * 54.6f / 1000, s->other_kw = 3.4f - 1.7f - 14.63f * 54.6f / 1000; /* 3.4 kW besides the motors */
	s->cabin_c[0] = 22, s->cabin_c[1] = 21, s->cabin_c[2] = 20;
	TEXT(s->fan[0], "4"), TEXT(s->fan[1], "HIGH");
	seen(s);
	BUCKLE(s, DS_SEAT_D, DS_SEAT_P);
	s->route = true, s->nav_minutes = 96;
	TEXT(s->arrive_clock, "2:52 AM");
	s->route_kwh = 50.62f - 38, s->arrive_range = 252, s->range_drop = 93, s->arrive_pct = 44, s->pct_drop = 27;
	s->g_shown = true, s->g_long = 0.02f, s->g_lat = 0.12f;
	s->g_peak_accel = 0.31f, s->g_peak_brake = 0.42f, s->g_peak_lat = 0.28f;
}

static void city_turn(dash_state_t *s)
{
	car(s);
	s->speed = 27, s->gear = 'D', s->front_kw = -9, s->rear_kw = -41, s->pedal_pct = 0;
	s->braking = true, s->brake_pct = 30, s->ind_l = true, s->blind_r = true;
	s->brake_nm[0] = 140, s->brake_nm[1] = 130, s->brake_nm[2] = 50, s->brake_nm[3] = 40; /* the fronts do most of it */
	s->steer_deg = 150, s->steer_lines = true; /* into the turn */
	s->soc_pct = 52, s->range = 158 * 1.609344f, s->speed_limit = 50, s->ap = AP_READY;
	s->low_beam = true, s->outside_c = 12, s->pack_temp_c = 27, s->regen_cap_kw = 84, s->co2_ppm = 640;
	s->cabin_c[0] = 21, s->cabin_c[1] = 21, s->cabin_c[2] = 21;
	TEXT(s->fan[0], "AUTO"), TEXT(s->fan[1], "LOW");
	seen(s);
	BUCKLE(s, DS_SEAT_D, DS_SEAT_P, DS_SEAT_2L, DS_SEAT_2R);
	s->g_shown = true, s->g_long = -0.24f, s->g_lat = -0.05f;
	s->g_peak_accel = 0.35f, s->g_peak_brake = 0.29f, s->g_peak_lat = 0.21f;
}

static void rear_motor(dash_state_t *s)
{
	car(s);
	seen(s);
	s->high_beam = true, s->car = DASH_CAR_Y5, s->rwd = true;
	s->speed = 64, s->gear = 'D', s->rear_kw = 23, s->pedal_pct = 18;
	s->soc_pct = 77, s->range = 231 * 1.609344f, s->speed_limit = 70, s->ap = AP_READY;
	s->outside_c = 16, s->pack_temp_c = 29, s->regen_cap_kw = 70;
	s->cabin_c[0] = 20, s->cabin_c[1] = 20, s->cabin_c[2] = 19;
	TEXT(s->fan[0], "2"), TEXT(s->fan[1], "OFF");
	BUCKLE(s, DS_SEAT_D, DS_SEAT_P, DS_SEAT_2R);
}

static void model_3(dash_state_t *s)
{
	car(s);
	seen(s);
	s->car = DASH_CAR_3, s->low_beam = true, s->braking = true, s->ind_l = true;
	s->speed = 23, s->gear = 'D', s->front_kw = -8, s->rear_kw = -14, s->brake_pct = 40;
	s->brake_nm[0] = 230, s->brake_nm[1] = 220, s->brake_nm[2] = 100, s->brake_nm[3] = 90;
	s->soc_pct = 64, s->range = 198 * 1.609344f, s->speed_limit = 50, s->outside_c = 9;
	s->cabin_c[0] = 21, s->cabin_c[1] = 21;
	TEXT(s->fan[0], "3"), TEXT(s->fan[1], "OFF");
	BUCKLE(s, DS_SEAT_D, DS_SEAT_P);
	s->seat[DS_SEAT_2L] = DS_SEAT_UNBUCKLED;
}

static void model_y_l(dash_state_t *s)
{
	car(s);
	seen(s);
	s->car = DASH_CAR_YL, s->low_beam = true, s->braking = true, s->ind_r = true;
	s->speed = 31, s->gear = 'D', s->front_kw = 6, s->rear_kw = 11;
	s->soc_pct = 71, s->range = 250 * 1.609344f, s->speed_limit = 50, s->outside_c = 14;
	s->cabin_c[0] = 21, s->cabin_c[1] = 21, s->cabin_c[2] = 20;
	TEXT(s->fan[0], "2"), TEXT(s->fan[1], "1");
	BUCKLE(s, DS_SEAT_D, DS_SEAT_P, DS_SEAT_2L, DS_SEAT_3R);
	s->seat[DS_SEAT_2R] = DS_SEAT_UNBUCKLED;
}

static void charging(dash_state_t *s)
{
	car(s);
	seen(s);
	s->speed = 0, s->gear = 'P', s->soc_pct = 46, s->range = 140 * 1.609344f;
	s->charging = true, s->charge_kw = 48, s->charge_max_kw = 62;
	TEXT(s->charge_why, "COLD PACK");
	s->charge_limit_pct = 80, s->charge_minutes = 35;
	TEXT(s->charge_clock, "1:51 AM");
	s->outside_c = 2, s->pack_temp_c = 9, s->pack_heating = true, s->co2_ppm = 560;
	s->cabin_c[0] = 20, s->cabin_c[1] = 18, s->cabin_c[2] = 17;
	TEXT(s->fan[0], "LOW"), TEXT(s->fan[1], "OFF");
}

static void no_new_data(dash_state_t *s)
{
	car(s);
	s->speed = 64, s->gear = 'D', s->front_kw = 10, s->rear_kw = 20, s->soc_pct = 58, s->range = 175 * 1.609344f;
}

static void bluetooth_only(dash_state_t *s)
{
	dash_state_blank(s);
	s->live = true, s->wifi_live = false, s->no_cabin = true;
	s->wifi = WIFI_IDLE, s->bt = BT_FULL;
	s->speed = 72, s->gear = 'D', s->rear_kw = 13, s->front_kw = 8, s->speed_limit = 70, s->ap = AP_STEERING;
	s->soc_pct = 58, s->range = 175 * 1.609344f, s->drl = true, s->low_beam = true;
	s->degradation_pct = (1 - 81.76f / 84) * 100;
	s->route = true;
	TEXT(s->dest, "Eiffel Tower, Paris");
	s->nav_minutes = 38;
	TEXT(s->arrive_clock, "1:54 AM");
	s->route_len = 31.1f * 1.609344f, s->route_kwh = 9.5f, s->arrive_range = 225, s->range_drop = 57;
	s->arrive_pct = 48, s->pct_drop = 10;
}

const scene_t scenes[] = {
	{"cold-morning", cold_morning}, {"highway-cruise", highway_cruise}, {"city-turn", city_turn},
	{"rear-motor-5-seats", rear_motor}, {"model-3", model_3}, {"model-y-l", model_y_l},
	{"charging", charging}, {"no-new-data", no_new_data}, {"bluetooth-only", bluetooth_only},
};
const int scene_count = sizeof scenes / sizeof scenes[0];
