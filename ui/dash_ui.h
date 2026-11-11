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
 * never shown; a network with pass "" keeps the one it has. S3XY_OBD (the car's Commander) is built in and not in
 * the list. */
#define DASH_NETS 4
typedef struct {
	char ssid[33];
	char pass[64];
} dash_net_t;
typedef struct {
	bool minor_figures, hour12;
	int brightness_pct;       /* 5..100: the manual value */
	bool follow_car;          /* follow the car screen's brightness (0x273); moving the slider turns it off */
	int net_count;
	dash_net_t net[DASH_NETS];
	bool dev_ssh;             /* developer SSH: needs an authorized key on the card */
} dash_settings_t;
enum { DASH_SETTINGS_SAVE, DASH_CMDR_APPROVE, DASH_CMDR_FORGET, DASH_PAIR_START, DASH_PAIR_CANCEL };
typedef void (*dash_settings_cb)(int what, const dash_settings_t *s, void *ctx);
void dash_ui_set_settings_cb(dash_settings_cb cb, void *ctx);
void dash_ui_settings(const dash_settings_t *s); /* the saved values, as the screen should show them */

enum { DASH_VIEW_DASH, DASH_VIEW_CAR_PANEL, DASH_VIEW_SETTINGS, DASH_VIEW_NETWORK, DASH_VIEW_PAIR };
void dash_ui_show(int view); /* also opened and closed by touch */
