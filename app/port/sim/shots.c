/* Host headless renderer: draws each demo scene at the Dash canvas and writes <out>/<scene>.png.
 * It also checks the UI's partial redraws: each scene is first drawn over the previous one redrawing only what the
 * UI invalidated (as on the Dash), then redrawn whole; the two must match, or a change left stale pixels.
 * usage: shots <out dir> [width height]     exit 1 on a mismatch */
#include "lvgl.h"
#include "../../ui/dash_ui.h"
#include "../../ui/scenes.h"
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

static int W, H;
static uint8_t *buf, *part, *rgba;

/* Draws [s] over what is on screen redrawing only the dirty areas, then whole; writes the whole one. 1: they differ. */
static int shot(lv_display_t *d, const char *out, const char *name, const dash_state_t *s, int view)
{
	size_t sz = (size_t)W * H * 4;
	uint32_t t0 = ms();
	if (view >= 0) dash_ui_show(view);
	dash_ui_set(s);
	lv_refr_now(d);
	uint32_t t = ms() - t0;
	memcpy(part, buf, sz);
	lv_obj_invalidate(lv_screen_active());
	lv_obj_invalidate(lv_layer_top());
	lv_refr_now(d);
	size_t diff = 0;
	for (size_t p = 0; p < sz; p += 4) diff += memcmp(part + p, buf + p, 3) != 0;
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
	return diff != 0;
}

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

int main(int argc, char **argv)
{
	const char *out = argc > 1 ? argv[1] : "build/shots";
	W = argc > 3 ? atoi(argv[2]) : 1920, H = argc > 3 ? atoi(argv[3]) : 480;
	int bad = 0;
	lv_init();
	lv_tick_set_cb(ms);
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
		scenes[6].fill(&s);
		s.bt = BT_APPROVING;
		bad |= shot(d, out, "touch-approval", &s, DASH_VIEW_DASH);
		scenes[6].fill(&s);
		snprintf(s.toast, sizeof s.toast, "Unlock sent: unlocks with the driver inside");
		bad |= shot(d, out, "touch-car-panel", &s, DASH_VIEW_CAR_PANEL);
		dash_settings_t st = {.minor_figures = true, .hour12 = true, .brightness_pct = 70, .follow_car = true, .car = 4, .sparse = true, .wifi_first = true, .net_count = 3,
				      .net = {{"S3XY_OBD", ""}, {"Home WiFi", ""}, {"Phone hotspot", ""}},
				      .tz = "CET-1CEST,M3.5.0,M10.5.0/3", .panda_host = "192.168.4.1", .panda_port = 1338};
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
			{3, "Follow offset", "touch-settings-display-end"}, {5, "Check for updates", "touch-settings-advanced-end"}};
		for (int k = 0; k < 2; k++) {
			dash_ui_settings_tab(ends[k].tab - 1);
			lv_obj_t *l = find_label(lv_layer_top(), ends[k].last);
			if (!l) { printf("no label %s\n", ends[k].last); bad = 1; continue; }
			do l = lv_obj_get_parent(l); while (l && !lv_obj_is_scrollable(l)); /* its page (a label is "scrollable" too) */
			if (l) lv_obj_update_layout(l), lv_obj_scroll_by_bounded(l, 0, -100000, LV_ANIM_OFF); /* to the very end */
			bad |= shot(d, out, ends[k].name, &s, DASH_VIEW_SETTINGS);
			dash_ui_settings_tab(0); /* another tab: back at the top next time */
		}
		{ /* Advanced's Android Auto group in view: the lower-resolution switch over the paired phones */
			dash_ui_settings_tab(4);
			lv_obj_t *l = find_label(lv_layer_top(), "Lower resolution");
			if (!l) printf("no label Lower resolution\n"), bad = 1;
			else {
				lv_obj_update_layout(lv_layer_top()), lv_obj_scroll_to_view_recursive(l, LV_ANIM_OFF);
				lv_obj_t *pg = l;
				do pg = lv_obj_get_parent(pg); while (pg && !lv_obj_is_scrollable(pg));
				if (pg) lv_obj_scroll_by_bounded(pg, 0, -170, LV_ANIM_OFF); /* the whole group, not its first line at the foot */
			}
			bad |= shot(d, out, "touch-settings-advanced-aa", &s, DASH_VIEW_SETTINGS);
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
		tap_label(lv_layer_top(), "Panda host");
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
	return bad;
}
