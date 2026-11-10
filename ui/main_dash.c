/* The dash UI on the S3XY Dash (T113): LVGL on /dev/fb0 (480x1920 portrait panel) drawn as the 1920x480 canvas,
 * rotated in the flush, touch from evdev; display and touch set up as ui/bringup.c (checked on the unit).
 * Demo mode until the data layer is wired: cycles the app's demo scenes, a tap moves on; within a scene the
 * speed, power and pedal wobble and the indicators flash, so the ring and glows redraw as in a drive (LVGL's perf
 * monitor shows the cost). Usage: dash-ui [rotation 0|90|180|270] [touch event device] [seconds per scene] */
#include "lvgl.h"
#include "dash_ui.h"
#include "scenes.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static int scene, per_scene = 6;
static uint32_t scene_start;
static dash_state_t base;

static void load(int i)
{
	scene = (i + scene_count) % scene_count;
	scenes[scene].fill(&base);
	scene_start = lv_tick_get();
	printf("scene %s\n", scenes[scene].name);
}

static void tick(lv_timer_t *t)
{
	(void)t;
	uint32_t now = lv_tick_get(), ms = now - scene_start;
	if (ms > (uint32_t)per_scene * 1000) load(scene + 1), ms = 0;
	dash_state_t s = base;
	float w = sinf(ms / 1000.0f * 1.3f);
	if (!isnan(s.speed) && s.gear != 'P') s.speed = fmaxf(0, s.speed + 3 * w);
	if (!isnan(s.rear_kw)) s.rear_kw += 8 * w;
	if (!isnan(s.front_kw)) s.front_kw += 5 * w;
	if (!isnan(s.pedal_pct) && s.gear != 'P') s.pedal_pct = fmaxf(0, 30 + 25 * w);
	bool lit = (now / 375) % 2 == 0; /* the car's flasher, about 80 a minute */
	s.ind_l = base.ind_l && lit;
	s.ind_r = base.ind_r && lit;
	dash_ui_set(&s);
}

static void on_tap(lv_event_t *e)
{
	(void)e;
	load(scene + 1);
}

int main(int argc, char **argv)
{
	int rot = argc > 1 ? atoi(argv[1]) : 270; /* 270: upright on the Dash (checked on the unit) */
	const char *touch = argc > 2 ? argv[2] : "/dev/input/event0";
	if (argc > 3) per_scene = atoi(argv[3]);

	lv_init();
	lv_display_t *disp = lv_linux_fbdev_create();
	lv_linux_fbdev_set_file(disp, "/dev/fb0");
	lv_display_set_rotation(disp, rot == 90 ? LV_DISPLAY_ROTATION_90 : rot == 180 ? LV_DISPLAY_ROTATION_180 :
				      rot == 270 ? LV_DISPLAY_ROTATION_270 : LV_DISPLAY_ROTATION_0);
	printf("display %dx%d (rotation %d)\n", (int)lv_display_get_horizontal_resolution(disp),
	       (int)lv_display_get_vertical_resolution(disp), rot);

	lv_indev_t *ts = lv_evdev_create(LV_INDEV_TYPE_POINTER, touch);
	if (!ts) printf("no touch device %s\n", touch);
	/* The Goodix reports landscape coordinates; LVGL rotates pointer input with the display: feed it (479 - y, x),
	 * which its 270 rotation turns back into (x, y). */
	if (ts) {
		lv_evdev_set_swap_axes(ts, true);
		lv_evdev_set_calibration(ts, 479, 0, 0, 1919);
	}

	lv_obj_t *scr = lv_screen_active();
	dash_ui_init(scr);
	lv_obj_set_clickable(scr, true);
	lv_obj_add_event_cb(scr, on_tap, LV_EVENT_CLICKED, NULL);
	load(0);
	lv_timer_create(tick, 33, NULL); /* the state pushed to the widgets at about 30 Hz, as the app's fast tick */

	for (;;) {
		uint32_t ms = lv_timer_handler();
		usleep((ms > 33 ? 33 : ms) * 1000);
	}
}
