/* What the dash shows, as plain values: the data layer (core/) fills it, the UI (dash_ui.c) draws it.
 * Values are already decided the way the app decides them (live / WiFi gating, units, rounding inputs):
 * NAN (or -1 for counts, "" for text) means "unknown", which the UI shows as the app does (a dash, a dim
 * figure, or nothing). Names follow the app's Panda / Commander fields. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum { DASH_CAR_Y7, DASH_CAR_Y5, DASH_CAR_YL, DASH_CAR_3 } dash_car_t;

/* The seats, in the car pictures' order; a car uses those it has (Y5 and 3: D P 2L 2C 2R; YL no 2C). */
enum { DS_SEAT_D, DS_SEAT_P, DS_SEAT_2L, DS_SEAT_2C, DS_SEAT_2R, DS_SEAT_3L, DS_SEAT_3R, DS_SEAT_COUNT };
typedef enum { DS_SEAT_PLAIN, DS_SEAT_BUCKLED, DS_SEAT_UNBUCKLED } dash_seat_t; /* as the picture / green / red */

/* The assist state on its disc by the ring (main.dart _autopilot). */
typedef enum {
	AP_NONE,
	AP_READY,    /* grey wheel: ready, or just ended */
	AP_STEERING, /* blue wheel: Autopilot, NoA, FSD */
	AP_TACC,     /* blue cruise gauge */
	AP_HOLD,     /* amber, flashing: hands on */
	AP_HOLD_RED, /* red, flashing: chiming */
	AP_SLOWING,  /* red, flashing */
	AP_TAKEOVER, /* red, flashing */
	AP_LOCKED,   /* red, struck through */
	AP_FAULT,    /* grey, struck through */
} dash_ap_t;

/* The links' buttons (widgets.dart _Controls). */
#define DASH_NEARBY 6
typedef enum { WIFI_IDLE, WIFI_OFF, WIFI_SCANNING, WIFI_JOINING, WIFI_HELLO, WIFI_LIVE, WIFI_QUIET, WIFI_AWAY, WIFI_RETRYING } dash_wifi_t;
typedef enum {
	BT_IDLE, BT_OFF, BT_NO_LOCATION, BT_SCANNING, BT_LINKING, BT_APPROVING, BT_REFUSED, BT_DASH, BT_PAUSED,
	BT_AWAY, BT_HELD, BT_RETRYING, BT_BORROWED, BT_BUTTONS, BT_LEAN, BT_FULL,
} dash_bt_t;

typedef enum { GRIP_NONE, GRIP_SLIPPERY, GRIP_HELD } dash_grip_t; /* ice blue, amber */

typedef struct {
	/* Links. live: any car stream (WiFi or the Commander's Bluetooth); wifi_live: the WiFi CAN stream. */
	bool live, wifi_live;
	dash_wifi_t wifi;
	dash_bt_t bt;
	int16_t speed_hz; /* speed frames a second on the settled WiFi button; -1: none */
	/* A link's timer (a retry, a back-off, the hello's wait): whole seconds left, 0 none, and the seconds it started
	 * from (the ring round the icon drains from full). Whole seconds: the ring is redrawn once a second at most. */
	int16_t wifi_secs, wifi_span, bt_secs, bt_span;
	char wifi_line[128];             /* the Network tab's status line (main.dart's connection page) */
	char net_line[128];              /* the Network tab's: the joined network, its address, ssh */
	char net_ssid[33];               /* the joined network's name, "" none (marks its row Connected) */
	int8_t ssh;                      /* dropbear: -1 off, else the sessions open */
	int8_t nearby_n;                 /* networks in range, strongest first (the Network tab's Nearby list) */
	char nearby[DASH_NEARBY][33];
	char bt_line[224];               /* the Commander tab's */

	dash_car_t car;
	bool rwd; /* a rear-motor car: the ring's right arc is the pedals */
	bool kph; /* km/h and km, else mph and mi */

	/* The ring. */
	float speed;                     /* NAN: no data (a dim dash) */
	char gear;                       /* 'P' 'R' 'N' 'D', 0 none */
	float front_kw, rear_kw;         /* NAN: unknown */
	float pedal_pct, brake_pct;      /* NAN: unknown */
	float brake_nm[4];               /* each wheel's brake torque while braking (FL FR RL RR), Nm; NAN unknown */
	float regen_cap_kw;              /* NAN: none (parked, unknown) */
	float speed_limit;               /* NAN: blank sign */
	dash_ap_t ap;
	char ap_word[12];                /* its words, on a long press: "TACC", "HOLD WHEEL" ... */
	float cruise_set;                /* NAN: no cruise */
	int8_t follow_gap;               /* -1: none */
	bool washer_low;
	float outside_c;                 /* NAN: a dash */

	/* Charging (WiFi): the kW in the ring, the cue under it, the charge card. */
	bool charging;
	float charge_kw, charge_max_kw;  /* NAN: unknown */
	char charge_why[16];             /* "COLD PACK"; "" none */
	int8_t charge_limit_pct;         /* -1: unknown */
	int16_t charge_minutes;          /* -1: unknown */
	char charge_clock[12];           /* when it is done, "1:51 AM"; "" no clock */

	/* The battery dial and the figures under it. */
	float soc_pct, range;            /* range in the car's unit; NAN unknown */
	float pack_temp_c, precond_c;    /* NAN: unknown / not preconditioning */
	bool pack_heating;
	bool draws;                      /* the draws' rows show (WiFi, the other draws known) */
	float climate_kw;                /* NAN: a dash */
	bool minor_figures;              /* the app's "Show 12 V, other power and cell imbalance"; on by default here */
	float lv_kw, other_kw;           /* the 12 V side, and the rest; NAN a dash */
	float imbalance_mv;              /* strongest less weakest cell group; NAN a dash */
	float degradation_pct;           /* NAN: a dash */

	/* The g-meter (WiFi, moving, g known). */
	bool g_shown;
	float g_lat, g_long, g_peak_brake, g_peak_accel, g_peak_lat;

	/* Lights and glows. */
	bool drl, low_beam, high_beam, braking;
	bool ind_l, ind_r;               /* lit now, in step with the car's flasher */
	bool blind_l, blind_r;
	dash_grip_t grip;
	char grip_word[12];              /* "GRIP" or the traction mode's name */

	/* The car picture's figures. cabin: the stream's seat map is in (WiFi and the belt frame seen). */
	bool cabin;                      /* the seat map (WiFi, the chime's frame in) */
	uint8_t seat[DS_SEAT_COUNT];        /* dash_seat_t */
	float tyre_psi[4];               /* FL FR RL RR; NAN a dash */
	float cabin_c[3];                /* each row's air; NAN none */
	char fan[2][8];                  /* rows 1 and 2: "MED", "4", "OFF"; "" none */

	/* The action chips' lit states, and the cabin CO2 on the recirculation chip. */
	bool trunk, mirror_tilt, door_fl, door_rl, charge_port;
	bool frunk, recirc, door_fr, door_rr, interior;
	int16_t co2_ppm;                 /* -1: no figure yet; 0 and up shown */
	bool co2_stale;                  /* the slot stays, blank */

	/* The car's path either side, dotted (widgets.dart _CarLights): the wheel's angle, deg, left positive, and
	 * whether to draw them (core car_steer_lines: turned past 3 deg and 2 s after). WiFi. */
	float steer_deg;
	bool steer_lines;

	/* Presses (main.dart _press): the action a press is out for (cmdr_action, -1 none), and the snackbar's line
	 * ("" none; on the car panel its message line). The approval prompt shows while bt is BT_APPROVING. */
	int8_t press_pending;
	char toast[128];

	/* Pairing a computer for SSH (app/pair.c): the window is open, its URL and code, seconds left, the result line. */
	int8_t backlight_pct;            /* the backlight as set now, % (the settings' slider while it follows the car); -1 */
	bool pair_open;
	char pair_url[96], pair_code[8];
	int16_t pair_secs;
	char pair_result[112];

	/* The route (its own column on the Dash). */
	bool route;
	char dest[64];                   /* "" none */
	int16_t nav_minutes;             /* time to go; -1 none */
	char arrive_clock[12];           /* "1:30 AM"; "" none */
	float route_len;                 /* in the car's unit; NAN none */
	float route_kwh;                 /* energy the route takes; NAN none */
	float arrive_range, range_drop;  /* range at arrival and what the route takes of it; NAN none */
	float arrive_pct, pct_drop;      /* battery at arrival; NAN none */
	float arrive_kwh;                /* shown in place of arrive_pct when that is unknown; NAN none */
} dash_state_t;

void dash_state_blank(dash_state_t *s); /* everything unknown, nothing lit */
