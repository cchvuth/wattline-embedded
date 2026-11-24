/* Host headless renderer: draws each demo scene at the device's canvas (project_region.h: the device profile's) and
 * writes <out>/<scene>.png.
 * It also checks the UI's partial redraws: each scene is first drawn over the previous one redrawing only what the
 * UI invalidated (as on the Dash), then redrawn whole; the two must match, or a change left stale pixels. LVGL's clock is
 * the harness's own (a second on before each shot), so the renders do not hang on the Mac's speed.
 * The phone layout's swipes are a finger's touches read as the panel's are, through LVGL's gestures; the split's frames
 * after them must leave the picture's rectangle alone (the decoder draws it), and a swipe must not press a chip.
 * usage: shots <out dir> [width height]     exit 1 on a mismatch */
#include <math.h>
#include "lvgl.h"
#include "../../ui/dash_ui.h"
#include "../../ui/scenes.h"
#include "../../ui/project_region.h"
#include "../../core/cmdr_msg.h" /* the chips' actions */
#include "src/libs/lodepng/lodepng.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

static void flush(lv_display_t *d, const lv_area_t *a, uint8_t *px) { lv_display_flush_ready(d); }

static uint32_t ms(void)
{
	struct timeval t;
	gettimeofday(&t, NULL);
	return t.tv_sec * 1000 + t.tv_usec / 1000;
}

static uint32_t clock_ms = 1;
static uint32_t tick(void) { return clock_ms; }

static int W, H;
static uint8_t *buf, *part, *rgba;

/* The split's frames (keep_pj): the picture's rectangle painted first as the decoder would, then counted after the
 * frame's own redraw: 1 if any of it was painted over. */
#define PJ_MARK 0xFF00FFu
static bool keep_pj;
static void pj_paint(void)
{
	for (int y = PJ_RGN_Y; y < PJ_RGN_Y + PJ_RGN_H; y++)
		for (int x = PJ_RGN_X; x < PJ_RGN_X + PJ_RGN_W; x++) memcpy(buf + ((size_t)y * W + x) * 4, &(uint32_t){PJ_MARK}, 4);
}
static bool in_pj(int x, int y) { return x >= PJ_RGN_X && x < PJ_RGN_X + PJ_RGN_W && y >= PJ_RGN_Y && y < PJ_RGN_Y + PJ_RGN_H; }
static int pj_check(const char *name)
{
	size_t n = 0;
	int x0 = W, y0 = H, x1 = -1, y1 = -1;
	for (int y = PJ_RGN_Y; y < PJ_RGN_Y + PJ_RGN_H; y++)
		for (int x = PJ_RGN_X; x < PJ_RGN_X + PJ_RGN_W; x++) {
			uint32_t v;
			memcpy(&v, buf + ((size_t)y * W + x) * 4, 4);
			if ((v & 0xFFFFFF) == PJ_MARK) continue;
			n++;
			x0 = x < x0 ? x : x0, y0 = y < y0 ? y : y0, x1 = x > x1 ? x : x1, y1 = y > y1 ? y : y1;
		}
	if (n) printf("  %s: %zu pixels of the picture painted over, in %d,%d..%d,%d\n", name, n, x0, y0, x1, y1);
	return n != 0;
}

/* Draws [s] over what is on screen redrawing only the dirty areas, then whole; writes the whole one. 1: they differ. */
static int shot(lv_display_t *d, const char *out, const char *name, const dash_state_t *s, int view)
{
	size_t sz = (size_t)W * H * 4;
	uint32_t t0 = ms();
	clock_ms += 1000;
	if (keep_pj) pj_paint();
	if (view >= 0) dash_ui_show(view);
	dash_ui_set(s);
	lv_refr_now(d);
	int painted = keep_pj ? pj_check(name) : 0;
	uint32_t t = ms() - t0;
	memcpy(part, buf, sz);
	lv_obj_invalidate(lv_screen_active());
	lv_obj_invalidate(lv_layer_top());
	lv_refr_now(d);
	size_t diff = 0; /* (the split's guarded frames: the picture is the decoder's, the whole redraw paints its box) */
	for (size_t p = 0; p < sz; p += 4) diff += memcmp(part + p, buf + p, 3) != 0 && !(keep_pj && in_pj(p / 4 % W, p / 4 / W));
	for (size_t p = 0; p < sz; p += 4) /* XRGB8888 is B, G, R, X in memory */
		rgba[p] = buf[p + 2], rgba[p + 1] = buf[p + 1], rgba[p + 2] = buf[p], rgba[p + 3] = 255;
	char path[512];
	snprintf(path, sizeof path, "%s/%s.png", out, name);
	unsigned char *png;
	size_t len;
	unsigned err = lodepng_encode32(&png, &len, rgba, W, H);
	FILE *f = err ? NULL : fopen(path, "wb");
	if (f) fwrite(png, 1, len, f), fclose(f);
	lv_free(png);
	printf("%-20s %s  set+partial redraw %u ms%s\n", name, err ? lodepng_error_text(err) : f ? path : "can't write", t,
	       diff ? "  STALE PIXELS after the partial redraw" : "");
	if (diff) printf("  %zu pixels differ\n", diff);
	return diff != 0 || painted;
}

#if DASH_PHONE_LAYOUT
/* A finger on the panel, read as its touches are (a read a frame), through LVGL's own gestures. */
static lv_indev_t *finger;
static lv_point_t finger_at;
static bool finger_down;
static void finger_read(lv_indev_t *in, lv_indev_data_t *d)
{
	d->point = finger_at;
	d->state = finger_down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}
static void touch(int x, int y, int hold) /* a finger on x, y for [hold] ms, read every frame, then lifted */
{
	finger_at = (lv_point_t){x, y}, finger_down = true;
	lv_indev_read(finger);
	for (int t = 0; t < hold; t += 16) clock_ms += 16, lv_indev_read(finger);
	finger_down = false;
	lv_indev_read(finger);
}
static lv_obj_t *find_label(lv_obj_t *o, const char *text);
/* The split's bar's buttons as LVGL finds them under a finger, row by row (past the links at its left end, not Exit):
 * n of them, their boxes into [a]. */
static int bar_buttons(lv_area_t a[10])
{
	int n = 0;
	for (int y = 4; y < PJ_RGN_Y; y += 8)
		for (int x = PJ_RGN_X + W / 20; x < PJ_RGN_X + PJ_RGN_W; x += 8) {
			lv_obj_t *o = lv_indev_search_obj(lv_screen_active(), &(lv_point_t){x, y});
			if (!o || lv_obj_get_width(o) > W / 12 || lv_obj_get_height(o) > PJ_RGN_Y / 2 || !lv_obj_get_child_count(o) ||
			    lv_obj_get_x(o) < PJ_RGN_X + W / 20 || find_label(o, "EXIT")) continue;
			lv_area_t c;
			lv_obj_get_coords(o, &c);
			bool seen = false;
			for (int k = 0; k < n; k++) seen |= c.x1 == a[k].x1 && c.y1 == a[k].y1;
			if (!seen && n < 10) a[n++] = c;
		}
	return n;
}
static void swipe(int x0, int y0, int x1, int y1) /* from x0, y0 to x1, y1 in eight frames, then lifted there */
{
	finger_at = (lv_point_t){x0, y0}, finger_down = true;
	lv_indev_read(finger);
	for (int k = 1; k <= 8; k++) {
		clock_ms += 16;
		finger_at = (lv_point_t){x0 + (x1 - x0) * k / 8, y0 + (y1 - y0) * k / 8};
		lv_indev_read(finger);
	}
	clock_ms += 16, finger_down = false;
	lv_indev_read(finger);
}
static int wholes; /* frames asked whole (dash_ui_whole_frame, as a port with the G2D sets it) */
static void count_whole(void) { wholes++; }
static dash_settings_t saved; /* the settings as the UI last handed them over to be saved */
static int saves, presses;
static void on_save(int what, const dash_settings_t *st, void *ctx)
{
	if (what == DASH_SETTINGS_SAVE) saved = *st, saves++;
}
static int pressed; /* the action last pressed */
static void on_press(int action, void *ctx) { presses++, pressed = action; }
static int expect(bool ok, const char *what)
{
	if (!ok) printf("  FAIL %s\n", what);
	return !ok;
}
#endif

/* Taps the button whose label reads [text] (as a finger would): 1 found. */
static int tap_label(lv_obj_t *o, const char *text)
{
	for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++) {
		lv_obj_t *c = lv_obj_get_child(o, i);
		if (lv_obj_check_type(c, &lv_label_class) && !strcmp(lv_label_get_text(c), text)) {
			lv_obj_send_event(o, LV_EVENT_SHORT_CLICKED, NULL);
			return 1;
		}
		if (tap_label(c, text)) return 1;
	}
	return 0;
}

/* The label reading [text] under [o], or NULL. */
static lv_obj_t *find_label(lv_obj_t *o, const char *text)
{
	for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++) {
		lv_obj_t *c = lv_obj_get_child(o, i), *f;
		if (lv_obj_check_type(c, &lv_label_class) && !strcmp(lv_label_get_text(c), text)) return c;
		if ((f = find_label(c, text))) return f;
	}
	return NULL;
}
/* [text]'s label anywhere the UI puts it: the screen (the dash, the settings above it) or the top layer (the pages) */
static lv_obj_t *find_ui_label(const char *text)
{
	lv_obj_t *l = find_label(lv_screen_active(), text);
	return l ? l : find_label(lv_layer_top(), text);
}

int main(int argc, char **argv)
{
	const char *out = argc > 1 ? argv[1] : "build/shots";
	W = argc > 3 ? atoi(argv[2]) : PJ_PANEL_W, H = argc > 3 ? atoi(argv[3]) : PJ_PANEL_H;
	int bad = 0;
	lv_init();
	lv_tick_set_cb(tick);
	lv_display_t *d = lv_display_create(W, H);
	lv_display_set_color_format(d, LV_COLOR_FORMAT_XRGB8888);
	size_t sz = (size_t)W * H * 4;
	buf = malloc(sz), part = malloc(sz), rgba = malloc(sz);
	lv_display_set_buffers(d, buf, NULL, sz, LV_DISPLAY_RENDER_MODE_DIRECT); /* redraws only the dirty areas */
	lv_display_set_flush_cb(d, flush);

	dash_ui_init(lv_screen_active());
	for (int i = 0; i < scene_count; i++) {
		dash_state_t s;
		scenes[i].fill(&s);
		bad |= shot(d, out, scenes[i].name, &s, -1);
	}
	/* The phone layout's basic layout (the ring between cards): its four slots as the full layout's places, then the
	 * route in the first; the settings handed back as they were after. */
	if (DASH_PHONE_LAYOUT) {
		dash_settings_t bs = {.minor_figures = true, .basic = true,
				      .cards = {DASH_CARD_BUTTONS, DASH_CARD_CAR, DASH_CARD_ENERGY, DASH_CARD_G}};
		const char *names[][2] = {{"highway-cruise", "basic-highway"}, {"city-turn", "basic-city"},
					  {"charging", "basic-charging"}, {"plugged-at-limit", "basic-plugged"},
					  {"bluetooth-only", "basic-bluetooth"}};
		dash_ui_settings(&bs);
		for (size_t k = 0; k < sizeof names / sizeof names[0]; k++)
			for (int i = 0; i < scene_count; i++)
				if (!strcmp(scenes[i].name, names[k][0])) {
					dash_state_t s;
					scenes[i].fill(&s);
					bad |= shot(d, out, names[k][1], &s, -1);
				}
		bs.cards[0] = DASH_CARD_ROUTE;
		dash_ui_settings(&bs);
		dash_state_t s;
		scenes[1].fill(&s);
		bad |= shot(d, out, "basic-route", &s, -1);
		/* nothing to show (the stream gone, no route): the route and drive cards' skeletons, not empty slots */
		for (int i = 0; i < scene_count; i++)
			if (!strcmp(scenes[i].name, "no-new-data")) {
				scenes[i].fill(&s);
				s.wifi_live = s.live = false; /* no link at all */
				for (int k = 0; k < 4; k++) s.brake_temp_c[k] = NAN;
				bad |= shot(d, out, "basic-no-data", &s, -1);
			}
		scenes[1].fill(&s); /* and back: NO ROUTE to the route, the skeleton to the meter (each change redrawn) */
		bad |= shot(d, out, "basic-data-back", &s, -1);
		dash_settings_t full = {.minor_figures = true, .cards = {DASH_CARD_BUTTONS, DASH_CARD_CAR, DASH_CARD_ENERGY, DASH_CARD_G}};
		dash_ui_settings(&full);
	}
	/* The touch parts, over a scene: a press out, a refused one, the approval prompt, the car panel, the settings and
	 * the network editor. */
	{
		dash_state_t s;
		scenes[6].fill(&s); /* charging, in P */
		s.press_pending = 1; /* ACT_TRUNK */
		bad |= shot(d, out, "touch-press-pending", &s, DASH_VIEW_DASH);
		scenes[1].fill(&s); /* highway */
		snprintf(s.toast, sizeof s.toast, "Frunk opens in P only");
		bad |= shot(d, out, "touch-press-refused", &s, DASH_VIEW_DASH);
		s.pack_temp_c = 9, s.precond_c = 30; /* heating the pack for a charger on the route: its target after the temperature */
		bad |= shot(d, out, "edge-precondition", &s, DASH_VIEW_DASH);
		scenes[6].fill(&s);
		s.bt = BT_APPROVING;
		bad |= shot(d, out, "touch-approval", &s, DASH_VIEW_DASH);
		scenes[6].fill(&s);
		snprintf(s.toast, sizeof s.toast, "Unlock sent: unlocks with the driver inside");
		bad |= shot(d, out, "touch-car-panel", &s, DASH_VIEW_CAR_PANEL);
		s.bt = BT_APPROVING; /* a press there while unlinked asked for the approval: its step in the panel's line */
		bad |= shot(d, out, "touch-car-panel-approval", &s, DASH_VIEW_CAR_PANEL);
		dash_settings_t st = {.minor_figures = true, .hour12 = true, .brightness_pct = 70, .follow_car = true, .car = 4, .sparse = true, .wifi_first = true, .net_count = 3,
				      .net = {{"S3XY_OBD", ""}, {"Home WiFi", ""}, {"Phone hotspot", ""}},
				      .tz = "CET-1CEST,M3.5.0,M10.5.0/3", .panda_host = "192.168.4.1", .panda_port = 1338,
				      .unit_miles = -1, .unit_f = -1, .unit_psi = -1};
		dash_ui_settings(&st);
		scenes[6].fill(&s);
		s.bt = BT_LEAN, s.backlight_pct = 45; /* following the car: the live 45 % */
		snprintf(s.wifi_line, sizeof s.wifi_line, "WiFi: live, 48 speed frames/s, 612 frames/s in all, from 192.168.4.1:1338");
		snprintf(s.bt_line, sizeof s.bt_line, "Bluetooth: connected. Car data: subscribed to the route and recirculation only, "
			 "1284 pushes. Car link: connected to the car, Tesla API ready.");
		bad |= shot(d, out, "touch-settings", &s, DASH_VIEW_SETTINGS);
		char marker[600]; /* an AA session up: the split stays (without the marker the UI goes back to the dash) */
		snprintf(marker, sizeof marker, "%s/.dash-project", out);
		fclose(fopen(marker, "w"));
		dash_ui_pj_marker = marker;
		scenes[1].fill(&s); /* highway: a live drive, shown in the strip beside the map */
		bad |= shot(d, out, "project", &s, DASH_VIEW_PROJECT);
		s.blind_l = true, s.ind_r = true; /* a car in the left blind spot, the right indicator on */
		bad |= shot(d, out, "project-blind", &s, DASH_VIEW_PROJECT);
		s.blind_l = false, s.ind_r = false;
		s.bt = BT_APPROVING; /* the approval's step in the split: on the dash's side, off the picture */
		bad |= shot(d, out, "project-approval", &s, DASH_VIEW_PROJECT);
		if (!DASH_PHONE_LAYOUT) { /* the strip's one line again after the long form elsewhere (its label ends in dots only there) */
			dash_ui_settings_tab(1), dash_ui_show(DASH_VIEW_SETTINGS), dash_ui_set(&s);
			dash_ui_show(DASH_VIEW_PROJECT), dash_ui_set(&s);
			if (!find_ui_label("Double-press the right scroll wheel"))
				bad |= 1, printf("  FAIL the strip's approval step again after the settings' long one\n");
		}
		scenes[1].fill(&s);
		snprintf(s.toast, sizeof s.toast, "Climate: refused (the car's WiFi stream is off: ScanMyTesla off in the S3XY app)");
		bad |= shot(d, out, "project-toast", &s, DASH_VIEW_PROJECT); /* a long one: within the dash's side, off the picture */
		s.toast[0] = 0;
		s.wifi = WIFI_IDLE; /* the phone on the Dash's own AP: Wi-Fi says AA (the wide box), Bluetooth settled (narrow) */
		bad |= shot(d, out, "project-aa-links", &s, DASH_VIEW_PROJECT);
		scenes[1].fill(&s);
		{
			dash_state_t off; /* no car data: the strip's dim / placeholder state */
			dash_state_blank(&off);
			bad |= shot(d, out, "project-offline", &off, DASH_VIEW_PROJECT);
		}
		remove(marker);
		dash_ui_set(&s); /* back to the dash */
		static const char *const tabs[] = {"touch-settings-car", "touch-settings-display", "touch-settings-network",
						   "touch-settings-advanced"};
		for (int t = 1; t <= 4; t++) {
			dash_ui_settings_tab(t);
			bad |= shot(d, out, tabs[t - 1], &s, DASH_VIEW_SETTINGS);
		}
		/* the long pages scrolled to their ends (the bar shows where) */
		static const struct { int tab; const char *last, *name; } ends[] = {
			{2, "Time zone", "touch-settings-display-end"}, {4, "Restart the screen", "touch-settings-advanced-end"}};
		for (int k = 0; k < 2; k++) {
			dash_ui_settings_tab(ends[k].tab);
			lv_obj_t *l = find_ui_label(ends[k].last);
			if (!l) { printf("no label %s\n", ends[k].last); bad = 1; continue; }
			do l = lv_obj_get_parent(l); while (l && !lv_obj_is_scrollable(l)); /* its page (a label is "scrollable" too) */
			if (l) lv_obj_update_layout(l), lv_obj_scroll_by_bounded(l, 0, -100000, LV_ANIM_OFF); /* to the very end */
			bad |= shot(d, out, ends[k].name, &s, DASH_VIEW_SETTINGS);
			dash_ui_settings_tab(0); /* another tab: back at the top next time */
		}
		static const struct { int tab; const char *label, *name; } rows[] = {   /* a group's title (unique) at its page's top */
			{4, "Android Auto", "touch-settings-advanced-aa"}, {2, "Dash", "touch-settings-display-dash"}};
		for (int k = 0; k < (int)(sizeof rows / sizeof rows[0]); k++) {
			dash_ui_settings_tab(rows[k].tab);
			lv_obj_t *l = find_ui_label(rows[k].label);
			if (!l) printf("no label %s\n", rows[k].label), bad = 1;
			else {
				lv_obj_t *pg = l;
				do pg = lv_obj_get_parent(pg); while (pg && !lv_obj_is_scrollable(pg));
				if (pg) lv_obj_update_layout(pg), lv_obj_scroll_to_y(pg, lv_obj_get_y(l), LV_ANIM_OFF); /* the group whole */
			}
			bad |= shot(d, out, rows[k].name, &s, DASH_VIEW_SETTINGS);
			dash_ui_settings_tab(0); /* another tab: back at the top next time */
		}
		dash_ui_settings_tab(0);
		s.bt = BT_APPROVING; /* the approval's turn: the step said, the wheel lit */
		bad |= shot(d, out, "touch-settings-approving", &s, DASH_VIEW_SETTINGS);
		s.bt = BT_IDLE;
		snprintf(s.bt_line, sizeof s.bt_line, "Bluetooth: this car's Commander not advertising (car asleep? phone app on it?)");
		bad |= shot(d, out, "touch-settings-unlinked", &s, DASH_VIEW_SETTINGS);
		s.wifi = WIFI_SCANNING; /* the join, with the wrong-passphrase hint */
		snprintf(s.wifi_line, sizeof s.wifi_line, "WiFi: wrong password? the network keeps refusing the passphrase");
		dash_ui_settings_tab(3);
		bad |= shot(d, out, "touch-settings-network-join", &s, DASH_VIEW_SETTINGS);
		dash_ui_settings_tab(0);
		s.bt = BT_LEAN;
		bad |= shot(d, out, "touch-network", &s, DASH_VIEW_NETWORK);
		dash_ui_show(DASH_VIEW_SETTINGS); /* the Panda's address: the same editor, one field */
		dash_ui_settings_tab(4);
		bad |= shot(d, out, "touch-settings-advanced-host", &s, DASH_VIEW_SETTINGS);
		if (!tap_label(lv_screen_active(), "Panda host")) tap_label(lv_layer_top(), "Panda host");
		bad |= shot(d, out, "touch-network-host", &s, -1);
		dash_ui_show(DASH_VIEW_SETTINGS);
		dash_ui_settings_tab(0);
		scenes[6].fill(&s); /* pairing: a dummy window */
		s.pair_open = true, s.pair_secs = 287;
		snprintf(s.pair_url, sizeof s.pair_url, "http://192.168.1.50:8022/pair?code=482913");
		snprintf(s.pair_code, sizeof s.pair_code, "482913");
		bad |= shot(d, out, "touch-pair", &s, DASH_VIEW_PAIR);
		s.pair_open = false;
		snprintf(s.pair_result, sizeof s.pair_result, "Key added: dev@laptop");
		bad |= shot(d, out, "touch-pair-done", &s, DASH_VIEW_PAIR);
		dash_ui_show(DASH_VIEW_DASH);
		/* the first boot: which car */
		scenes[0].fill(&s);
		dash_ui_ask_car();
		bad |= shot(d, out, "touch-settings-ask-car", &s, -1);
		dash_ui_show(DASH_VIEW_DASH);
		/* a WiFi retry's countdown and Bluetooth's back-off: rings round the icons, the seconds beside */
		scenes[0].fill(&s);
		s.wifi = WIFI_RETRYING, s.wifi_secs = 4, s.wifi_span = 6, s.bt = BT_AWAY, s.bt_secs = 18, s.bt_span = 20;
		bad |= shot(d, out, "links-countdown", &s, DASH_VIEW_DASH);
		/* the gear unknown: a dim dash */
		scenes[1].fill(&s);
		s.gear = 0;
		bad |= shot(d, out, "gear-unknown", &s, DASH_VIEW_DASH);
		/* edge cases: the widest figures and words the app can be given */
		scenes[1].fill(&s); /* highway */
		s.speed = 199, s.speed_limit = 120, s.cruise_set = 199, s.rear_kw = 288, s.front_kw = -120, s.outside_c = -28.5f;
		s.soc_pct = 100, s.range = 999, s.climate_kw = 11.8f, s.lv_kw = 2.4f, s.other_kw = 10.9f, s.imbalance_mv = 1234;
		s.degradation_pct = 23.4f, s.nav_minutes = 1439, s.route_len = 1234, s.route_kwh = 288.8f, s.arrive_range = -12;
		snprintf(s.dest, sizeof s.dest, "%s", "Sankt Peterburg Pulkovo International Airport, Terminal 1");
		snprintf(s.arrive_clock, sizeof s.arrive_clock, "%s", "12:59 PM");
		for (int k = 0; k < 4; k++) s.tyre_psi[k] = k ? 22.5f : 51;
		s.door_fl = s.door_fr = s.door_rl = s.door_rr = s.trunk = s.frunk = s.charge_port = true;
		snprintf(s.toast, sizeof s.toast, "%s", "Climate: refused (not allowed): needs the Commander linked to the car (ScanMyTesla off)");
		bad |= shot(d, out, "edge-extremes", &s, DASH_VIEW_DASH);
		scenes[1].fill(&s); /* miles and a 12-hour clock: the other units' widths */
		s.kph = false, s.speed = 68, s.range = 214, s.route_len = 6.4f, s.arrive_range = 141;
		bad |= shot(d, out, "edge-miles", &s, DASH_VIEW_DASH);
		scenes[2].fill(&s); /* braking at full lock to the right: the turned fronts whole, clear of their figures */
		s.steer_deg = -520, s.brake_nm[0] = 400, s.brake_nm[1] = 12, s.brake_nm[2] = 0, s.brake_nm[3] = 160;
		bad |= shot(d, out, "edge-brake-full-lock", &s, DASH_VIEW_DASH);
		scenes[8].fill(&s); /* Bluetooth only, no route: what the Commander's pushes alone fill */
		s.route = false, s.outside_c = 7, s.imbalance_mv = 6, s.pack_temp_c = 18;
		bad |= shot(d, out, "edge-bt-no-route", &s, DASH_VIEW_DASH);
		dash_state_blank(&s); /* nothing known at all: every figure its placeholder */
		bad |= shot(d, out, "edge-nothing", &s, DASH_VIEW_DASH);
		{ /* the Android Auto button (only with a head-unit certificate): off, the white icon; on, the logo's colours */
			char crt[600], up[600];
			snprintf(crt, sizeof crt, "%s/.aa.crt", out), snprintf(up, sizeof up, "%s/.dash-aa", out);
			fclose(fopen(crt, "w"));
			dash_ui_aa_cert = crt, dash_ui_aa_up = up;
			scenes[0].fill(&s);
			bad |= shot(d, out, "aa-button-off", &s, DASH_VIEW_DASH);
			fclose(fopen(up, "w"));
			bad |= shot(d, out, "aa-button-on", &s, DASH_VIEW_DASH);
			remove(up), remove(crt);
			bad |= shot(d, out, "aa-button-none", &s, DASH_VIEW_DASH); /* the certificate gone: no button */
		}
		scenes[6].fill(&s); /* charging, in P, while the phone's map is up */
		char pjm[600];
		snprintf(pjm, sizeof pjm, "%s/.dash-project", out);
		fclose(fopen(pjm, "w"));
		dash_ui_pj_marker = pjm;
		bad |= shot(d, out, "edge-project-charging", &s, DASH_VIEW_PROJECT);
		scenes[1].fill(&s); /* the strip at its widest: three digits, a long nav line, the AP's words */
		s.speed = 188, s.rear_kw = -88, s.front_kw = 188, s.nav_minutes = 1439, s.route_len = 1234, s.arrive_range = 999;
		s.wifi = WIFI_IDLE, s.bt = BT_RETRYING, s.bt_secs = 12, s.bt_span = 20, s.blind_r = true;
		bad |= shot(d, out, "edge-project-wide", &s, DASH_VIEW_PROJECT);
		dash_state_blank(&s);
		bad |= shot(d, out, "edge-project-nothing", &s, DASH_VIEW_PROJECT);
		scenes[8].fill(&s); /* Bluetooth only (no WiFi stream) beside the phone's map: what the Commander's pushes fill */
		bad |= shot(d, out, "edge-project-bt-only", &s, DASH_VIEW_PROJECT);
		{ /* the phone's pictures per second under the WiFi icon, once the decoder writes them */
			char fps[600];
			snprintf(fps, sizeof fps, "%s/.dash-pj-fps", out);
			FILE *f = fopen(fps, "w");
			if (f) fputs("58\n", f), fclose(f);
			dash_ui_pj_fps = fps;
			bad |= shot(d, out, "edge-project-aa-fps", &s, DASH_VIEW_PROJECT);
			remove(fps);
		}
		remove(pjm);
		dash_ui_set(&s);
		/* the Network tab with the longest names an SSID can have */
		scenes[6].fill(&s);
		dash_settings_t ln = {.minor_figures = true, .brightness_pct = 70, .car = 4, .net_count = 3,
				      .net = {{"S3XY_OBD", ""}, {"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", ""}, {"Wi-Fi with spaces & 'quotes'", ""}},
				      .tz = "UTC0", .panda_host = "192.168.200.254", .panda_port = 65535};
		dash_ui_settings(&ln);
		s.nearby_n = 3;
		snprintf(s.nearby[0], sizeof s.nearby[0], "%s", "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB");
		snprintf(s.nearby[1], sizeof s.nearby[1], "%s", "Café Nördlingen Gäste");
		snprintf(s.nearby[2], sizeof s.nearby[2], "%s", "x");
		snprintf(s.net_ssid, sizeof s.net_ssid, "%s", "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA");
		dash_ui_settings_tab(3);
		bad |= shot(d, out, "edge-settings-network-long", &s, DASH_VIEW_SETTINGS);
		dash_ui_settings_tab(4);
		bad |= shot(d, out, "edge-settings-advanced-host", &s, DASH_VIEW_SETTINGS);
		dash_ui_settings_tab(0);
		dash_ui_show(DASH_VIEW_DASH);
	}
	/* Every link state in its corner, on one sheet (links.png): WiFi state i beside Bluetooth state i (and i + 9). */
	enum { CW = 300, CH = 170 };
	uint8_t *sheet = calloc((size_t)CW * 8 * CH * 2 * 4, 1);
	for (int i = 0; i < 16; i++) {
		dash_state_t s;
		scenes[0].fill(&s);
		s.wifi = (dash_wifi_t)(i % 9), s.wifi_live = s.wifi == WIFI_LIVE, s.bt = (dash_bt_t)i;
		dash_ui_set(&s);
		lv_obj_invalidate(lv_screen_active());
		lv_refr_now(d);
		int ox = (i % 8) * CW, oy = (i / 8) * CH;
		for (int y = 0; y < CH; y++)
			for (int x = 0; x < CW; x++) {
				uint8_t *p = buf + ((size_t)(H - CH + y) * W + x) * 4, *o = sheet + ((size_t)(oy + y) * CW * 8 + ox + x) * 4;
				o[0] = p[2], o[1] = p[1], o[2] = p[0], o[3] = 255;
			}
	}
	char path[512];
	snprintf(path, sizeof path, "%s/links.png", out);
	unsigned char *png;
	size_t len;
	FILE *f = lodepng_encode32(&png, &len, sheet, CW * 8, CH * 2) ? NULL : fopen(path, "wb");
	if (f) fwrite(png, 1, len, f), fclose(f), lv_free(png);
	printf("%-20s %s\n", "link states", f ? path : "can't write");
#if DASH_PHONE_LAYOUT
	/* The phone layout's cards and faces: the split with each other card right of its ring, the speedometer on the
	 * split's ring and basic's, the power card, the drive card beside it and on Bluetooth alone. */
	{
		dash_state_t s;
		char marker[600];
		snprintf(marker, sizeof marker, "%s/.dash-project", out);
		dash_settings_t st = {.minor_figures = true, .cards = {DASH_CARD_BUTTONS, DASH_CARD_CAR, DASH_CARD_ENERGY, DASH_CARD_G}};
		static const struct { int scene, card; const char *name; } split[] = {
			{1, DASH_CARD_G, "project-card-drive"}, {2, DASH_CARD_CAR, "project-card-car"},
			{8, DASH_CARD_ROUTE, "project-card-route"}, {1, DASH_CARD_POWER, "project-card-power"}};
		fclose(fopen(marker, "w"));
		dash_ui_pj_marker = marker;
		for (size_t k = 0; k < sizeof split / sizeof split[0]; k++) {
			st.split_card = split[k].card;
			dash_ui_settings(&st);
			scenes[split[k].scene].fill(&s);
			bad |= shot(d, out, split[k].name, &s, DASH_VIEW_PROJECT);
		}
		st.split_card = DASH_CARD_ENERGY, st.ring_page = DASH_RING_SPEEDO;
		dash_ui_settings(&st);
		scenes[1].fill(&s); /* highway: the needle at 108 */
		bad |= shot(d, out, "project-speedo", &s, DASH_VIEW_PROJECT);
		scenes[6].fill(&s); /* charging: the kW in the figure's place, the cue under the raised gear */
		bad |= shot(d, out, "project-speedo-charging", &s, DASH_VIEW_PROJECT);
		remove(marker);
		st.basic = true;
		dash_ui_settings(&st);
		scenes[1].fill(&s);
		bad |= shot(d, out, "basic-speedo", &s, DASH_VIEW_DASH);
		scenes[3].fill(&s); /* a rear-motor car: the pedal's line along the top (the speedometer has no pedal) */
		bad |= shot(d, out, "basic-speedo-rwd", &s, DASH_VIEW_DASH);
		scenes[1].fill(&s); /* the power card left of the ring, the drive card right: the two circles alike */
		st.cards[0] = DASH_CARD_ROUTE, st.cards[1] = DASH_CARD_POWER, st.cards[2] = DASH_CARD_G, st.cards[3] = DASH_CARD_ENERGY;
		dash_ui_settings(&st);
		bad |= shot(d, out, "basic-dials", &s, DASH_VIEW_DASH);
		st.ring_page = DASH_RING_POWER;
		dash_ui_settings(&st);
		scenes[0].fill(&s); /* regen into the pack: the power card's fill green, the other way */
		bad |= shot(d, out, "basic-power-regen", &s, DASH_VIEW_DASH);
		scenes[8].fill(&s); /* Bluetooth alone: the motors' power, the drive card's brakes over the pedal bar */
		bad |= shot(d, out, "basic-drive-bt", &s, DASH_VIEW_DASH);

		/* By finger. The split: its card swiped up to the drive card, the car's, the route's (past the buttons: in its
		 * bar), the power card, round to the battery's again and back; its ring to the speedometer; a swipe on the
		 * picture, on the bar over it or across changes nothing; the ten buttons in the bar between the links and Exit, off
		 * the picture: a tap on one presses it, a swipe on one does not, a hold names it. Each frame after a touch leaves
		 * the picture alone. Then basic: its ring back, a slot past the cards the others show. */
		finger = lv_indev_create();
		lv_indev_set_type(finger, LV_INDEV_TYPE_POINTER);
		lv_indev_set_read_cb(finger, finger_read);
		lv_timer_pause(lv_indev_get_read_timer(finger));
		dash_ui_set_settings_cb(on_save, NULL);
		dash_ui_set_action_cb(on_press, NULL);
		st = (dash_settings_t){.minor_figures = true, .cards = {DASH_CARD_BUTTONS, DASH_CARD_CAR, DASH_CARD_ENERGY, DASH_CARD_G}};
		dash_ui_settings(&st);
		fclose(fopen(marker, "w"));
		scenes[2].fill(&s); /* the city: the car's card has its path lines */
		bad |= shot(d, out, "swipe-split", &s, DASH_VIEW_PROJECT);
		keep_pj = true;
		int card_x = W - W / 10, ring_x = PJ_RGN_X + PJ_RGN_W + (W - PJ_RGN_X - PJ_RGN_W) / 4, pic_x = PJ_RGN_X + PJ_RGN_W / 2;
		static const struct { int card; const char *name; } up[] = {
			{DASH_CARD_G, "swipe-split-drive"}, {DASH_CARD_CAR, "swipe-split-car"}, {DASH_CARD_ROUTE, "swipe-split-route"},
			{DASH_CARD_POWER, "swipe-split-power"}, {DASH_CARD_ENERGY, "swipe-split-energy"}};
		for (size_t k = 0; k < sizeof up / sizeof up[0]; k++) {
			swipe(card_x, H * 5 / 6, card_x, H / 3);
			bad |= expect(saved.split_card == up[k].card, up[k].name);
			bad |= shot(d, out, up[k].name, &s, -1);
		}
		swipe(card_x, H / 3, card_x, H * 5 / 6);
		bad |= expect(saved.split_card == DASH_CARD_POWER, "down: back to the power card");
		swipe(ring_x, H * 5 / 6, ring_x, H / 3);
		bad |= expect(saved.ring_page == DASH_RING_SPEEDO && saved.split_card == DASH_CARD_POWER, "the split's ring: the speedometer");
		bad |= shot(d, out, "swipe-split-speedo", &s, -1);
		lv_area_t b[10];
		int nb = bar_buttons(b);
		lv_obj_t *exit_l = find_ui_label("EXIT");
		lv_area_t ex = {0};
		if (exit_l) lv_obj_get_coords(lv_obj_get_parent(exit_l), &ex);
		bool in_bar = nb == 10 && exit_l;
		for (int k = 0; k < nb; k++) in_bar &= b[k].y1 >= 0 && b[k].y2 < PJ_RGN_Y && b[k].x2 < ex.x1 && b[k].x1 > PJ_RGN_X + W / 20;
		bad |= expect(in_bar, "the ten buttons in the bar, between the links and Exit, off the picture");
		int was = saves;
		swipe(pic_x, PJ_RGN_Y + PJ_RGN_H * 4 / 5, pic_x, PJ_RGN_Y + PJ_RGN_H / 5); /* the phone's */
		swipe(pic_x, PJ_RGN_Y - 10, pic_x, 10);                                  /* the bar over the picture */
		swipe(W - W / 4, H / 2, card_x + W / 20, H / 2);                         /* across the card */
		if (nb) swipe((b[0].x1 + b[0].x2) / 2, b[0].y2 - 10, (b[0].x1 + b[0].x2) / 2, b[0].y1 + 10); /* on a button, ending on it */
		bad |= expect(saves == was && !presses, "on the picture, the bar, across or a button: nothing, no press");
		bad |= shot(d, out, "swipe-split-none", &s, -1);
		if (nb > 1) touch((b[1].x1 + b[1].x2) / 2, (b[1].y1 + b[1].y2) / 2, 48); /* the mirror's button: a tap */
		bad |= expect(presses == 1 && pressed == ACT_MIRROR_TILT, "a tap on the mirror's button in the bar: pressed");
		s.press_pending = ACT_MIRROR_TILT; /* out: its side in the accent; a refusal's words in the right half */
		snprintf(s.toast, sizeof s.toast, "Mirror tilt: refused (moving): the car tilts it in R only");
		bad |= shot(d, out, "swipe-split-press", &s, -1);
		s.press_pending = -1, s.toast[0] = 0;
		presses = 0;
		keep_pj = false;
		remove(marker);
		st.basic = true, st.ring_page = DASH_RING_SPEEDO, st.split_card = DASH_CARD_POWER;
		dash_ui_settings(&st);
		scenes[1].fill(&s);
		bad |= shot(d, out, "swipe-basic", &s, DASH_VIEW_DASH);
		swipe(W / 2, H * 2 / 3, W / 2, H / 4);
		bad |= expect(saved.ring_page == DASH_RING_POWER, "basic's ring: the power ring again");
		swipe(W * 3 / 4, H * 2 / 3, W * 3 / 4, H / 4); /* the battery's slot: past the drive, car and buttons cards shown */
		bad |= expect(saved.cards[2] == DASH_CARD_ROUTE && saved.split_card == DASH_CARD_POWER, "basic's slot: the route's card");
		bad |= shot(d, out, "swipe-basic-route", &s, -1);
		swipe(W * 3 / 4, H / 2, W * 3 / 4 - W / 5, H / 2); /* sideways, from the slot to over the ring: its next card */
		int next = saved.cards[2];
		swipe(W * 3 / 4, H / 2, W * 3 / 4 + W / 8, H / 2);
		bad |= expect(next != DASH_CARD_ROUTE && saved.cards[2] == DASH_CARD_ROUTE, "basic's slot swiped left, then right: its next card, and back");
		swipe(W / 2 + W / 20, H / 2, W / 2 + W / 5, H / 2); /* from the ring to over the slot */
		bad |= expect(saved.ring_page == DASH_RING_POWER && saved.cards[2] == DASH_CARD_ROUTE, "sideways on the ring: nothing");
		swipe(W / 2 + W / 40, H - 20, W / 2 + W / 40, H / 2); /* up from the bar under the ring: not the ring's */
		bad |= expect(saved.ring_page == DASH_RING_POWER, "a swipe that begins on the bar under the ring: the ring's face stays");
		{ /* and one that begins there slowly (2 px a read, where LVGL starts its gesture's sum again), then flicks */
			int x = W / 2 + W / 40, y = H - 20;
			finger_at = (lv_point_t){x, y}, finger_down = true;
			lv_indev_read(finger);
			for (int k = 0; k < 60; k++) clock_ms += 16, finger_at.y -= 2, lv_indev_read(finger);
			for (int k = 0; k < 8; k++) clock_ms += 16, finger_at.y -= 25, lv_indev_read(finger);
			clock_ms += 16, finger_down = false;
			lv_indev_read(finger);
			bad |= expect(saved.ring_page == DASH_RING_POWER, "a slow start on the bar, then a flick over the ring: its face stays");
		}
		{ /* from the ring out to slot 2's card, every place is the ring's or the slot's: no strip between that is neither */
			bool dead = false;
			for (int x = W / 2 + H / 3; x < W / 2 + H / 2; x += 3) {
				int r = saved.ring_page, c = saved.cards[2];
				swipe(x, H * 2 / 3, x, H / 4);
				dead |= saved.ring_page == r && saved.cards[2] == c;
			}
			bad |= expect(!dead, "a swipe up anywhere from the ring to the slot beside it flips one of them");
		}
		bad |= expect(!presses, "no press from any swipe");
		st.basic = false; /* last, the split again: a hold on a button names it, in the bar (its tooltip stays: no timers here) */
		dash_ui_settings(&st);
		fclose(fopen(marker, "w"));
		bad |= shot(d, out, "swipe-split-again", &s, DASH_VIEW_PROJECT);
		keep_pj = true;
		nb = bar_buttons(b);
		if (nb > 6) touch((b[6].x1 + b[6].x2) / 2, (b[6].y1 + b[6].y2) / 2, 600);
		bad |= expect(nb > 6 && !presses, "a hold on a button: no press");
		bad |= shot(d, out, "swipe-split-hold", &s, -1);
		{ /* Exit where the port shows a frame whole: the split stays, its last picture up, until the session has gone;
		   * then the dash, asked whole. (Without the hook the dash comes at once: the shots' way.) Last: nothing follows */
			lv_obj_t *xl = find_ui_label("EXIT");
			lv_area_t xa = {0};
			if (xl) lv_obj_get_coords(lv_obj_get_parent(xl), &xa);
			dash_ui_whole_frame = count_whole;
			if (xl) touch((xa.x1 + xa.x2) / 2, (xa.y1 + xa.y2) / 2, 48);
			dash_ui_set(&s);
			bad |= expect(xl && lv_obj_is_visible(xl) && !wholes, "Exit with a whole-frame port: the split stays while the session is up");
			remove(marker);
			dash_ui_set(&s);
			bad |= expect(xl && !lv_obj_is_visible(xl) && wholes == 1, "the session gone: the dash, asked whole once");
			dash_ui_whole_frame = NULL;
		}
		keep_pj = false;
		remove(marker);
		dash_ui_set_settings_cb(NULL, NULL);
		dash_ui_set_action_cb(NULL, NULL);
		lv_indev_delete(finger);
		dash_ui_settings(&(dash_settings_t){.minor_figures = true,
						    .cards = {DASH_CARD_BUTTONS, DASH_CARD_CAR, DASH_CARD_ENERGY, DASH_CARD_G}});
	}
#endif
	return bad;
}
