/* The dash, drawn with LVGL in the Dash arrangement (PLAN-merged.md 5.2) of the app's look. */
#pragma once
#include "lvgl.h"
#include "dash_state.h"

/* Builds the dash on [scr], laid out once for the screen's size (H 480 at the example canvas). */
void dash_ui_init(lv_obj_t *scr);
/* Shows [s]; only what changed is touched, so LVGL redraws only those areas. */
void dash_ui_set(const dash_state_t *s);

/* ---- touch ----
 * A chip's tap, a car panel button: [action] is core/cmdr_msg.h's cmdr_action. The UI only asks; the app guards and
 * presses once (app/actions.c), and shows the outcome through dash_state_t's press_pending and toast. */
typedef void (*dash_action_cb)(int action, void *ctx);
void dash_ui_set_action_cb(dash_action_cb cb, void *ctx);

/* The settings screen (the link icons open it). Passphrases go only one way: typed here, handed to the callback,
 * never shown; a network with pass "" keeps the one it has. The car's adapter (S3XY_OBD at first) is an ordinary
 * entry, kept first in the list by settings.c. */
#define DASH_NETS 4
typedef struct {
	char ssid[33];
	char pass[64];
} dash_net_t;
typedef struct {
	bool minor_figures, hour12;
	int brightness_pct;       /* 5..100: the manual value */
	bool follow_car;          /* follow the car screen's brightness (0x273); moving the slider turns it off */
	int follow_offset;        /* -50..50: % points added to the followed brightness ("Follow offset") */
	int net_count;
	dash_net_t net[DASH_NETS];
	bool dev_ssh;             /* developer SSH: needs an authorized key on the card */
	bool record;              /* record drive data (CAN and Commander pushes) to /data/rec, for bench replays */
	int car;                  /* CAR_* (core/state.h): the body, seats and drive, as the phone app's picker */
	bool sparse;              /* sparse polling: steadier speed, slower doors and belts */
	bool wifi_first;          /* Bluetooth waits for the WiFi stream, no time limit */
	bool ota_auto;            /* automatic updates: with internet, download + verify + arm for the next start */
	bool aa_low_res;          /* Android Auto at 480p, scaled up: less for the phone and the WiFi (aa reads the file) */
	char tz[64];              /* POSIX TZ for the clocks (arrival, charge done) */
	char tz_conf[64];         /* dash.conf's (the owner's own, kept in the time zone button's cycle); read only */
	char panda_host[64];      /* the Commander's Panda interface (WiFi CAN) */
	int panda_port;
	char join[33];            /* not saved: join this network now (it goes first; the rest stay as fallback), "" none */
} dash_settings_t;
enum { DASH_SETTINGS_SAVE, DASH_CMDR_APPROVE, DASH_CMDR_FORGET, DASH_PAIR_START, DASH_PAIR_CANCEL,
       DASH_SETTINGS_OPEN /* the screen shown: the app may hand fresher values back (SSH turned on by pairing) */,
       DASH_NET_SCAN /* the Network tab shown: a fresh scan for its Nearby list */,
       DASH_AA_START, DASH_AA_STOP /* the on-screen button: bring wireless Android Auto up / tear it down */,
       DASH_OTA_CHECK, DASH_OTA_INSTALL, DASH_OTA_RESTART /* Advanced: software update (dash-update check / auto / reboot) */,
       DASH_AA_FORGET /* Advanced: forget the phones paired for Android Auto (dash-aa forget) */ };
typedef void (*dash_settings_cb)(int what, const dash_settings_t *s, void *ctx);
void dash_ui_set_settings_cb(dash_settings_cb cb, void *ctx);
void dash_ui_settings(const dash_settings_t *s); /* the saved values, as the screen should show them */
void dash_ui_settings_tab(int i);                 /* 0 Commander, 1 Car, 2 Display, 3 Network, 4 Advanced */
void dash_ui_ask_car(void);                       /* first boot: the Car tab, "Which car do you have?" over it */

enum { DASH_VIEW_DASH, DASH_VIEW_CAR_PANEL, DASH_VIEW_SETTINGS, DASH_VIEW_NETWORK, DASH_VIEW_PAIR, DASH_VIEW_PROJECT };
/* The phone map (Android Auto) takes the left 2/3; the dash shrinks to a strip on the right. The left region is where
 * decoded video is drawn; project_region() gives its pixel rect so the video layer can blit straight to the framebuffer. */
void dash_ui_project_region(int *x, int *y, int *w, int *h);
void dash_ui_show(int view); /* also opened and closed by touch */
extern const char *dash_ui_pj_marker; /* "/run/dash-project": an AA session is up (aa serve raises it) */
