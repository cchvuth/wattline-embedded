// Bring-up for the native dash on the S3XY Dash (T113): LVGL 9.6 on /dev/fb0 (480x1920 portrait panel),
// drawn as a 1920x480 landscape canvas and rotated in the flush (panel pixel = (479 - y, x)).
// Shows corner labels (orientation check), a title, LVGL's perf monitor, and a dot that follows touch.
// Usage: dash-bringup [rotation 0|90|180|270] [touch event device]
#include "lvgl.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static lv_obj_t *dot, *touch_lbl;

static void on_touch(lv_event_t *e)
{
	lv_indev_t *indev = lv_indev_active();
	lv_point_t p;
	if (!indev) return;
	lv_indev_get_point(indev, &p);
	lv_obj_set_pos(dot, p.x - 20, p.y - 20);
	lv_label_set_text_fmt(touch_lbl, "touch %d, %d", (int)p.x, (int)p.y);
	(void)e;
}

static lv_obj_t *corner(lv_obj_t *scr, const char *txt, lv_align_t al, int dx, int dy)
{
	lv_obj_t *l = lv_label_create(scr);
	lv_label_set_text(l, txt);
	lv_obj_set_style_text_font(l, &lv_font_montserrat_28, 0);
	lv_obj_set_style_text_color(l, lv_color_hex(0xffffff), 0);
	lv_obj_align(l, al, dx, dy);
	return l;
}

int main(int argc, char **argv)
{
	int rot = argc > 1 ? atoi(argv[1]) : 270;	/* 270: upright on the Dash (checked on the unit) */
	const char *touch = argc > 2 ? argv[2] : "/dev/input/event0";

	lv_init();
	lv_display_t *disp = lv_linux_fbdev_create();
	lv_linux_fbdev_set_file(disp, "/dev/fb0");
	lv_display_set_rotation(disp, rot == 90 ? LV_DISPLAY_ROTATION_90 : rot == 180 ? LV_DISPLAY_ROTATION_180 :
				      rot == 270 ? LV_DISPLAY_ROTATION_270 : LV_DISPLAY_ROTATION_0);
	printf("display %dx%d (rotation %d)\n", (int)lv_display_get_horizontal_resolution(disp),
	       (int)lv_display_get_vertical_resolution(disp), rot);

	lv_indev_t *ts = lv_evdev_create(LV_INDEV_TYPE_POINTER, touch);
	if (!ts) printf("no touch device %s\n", touch);
	/* The Goodix already reports landscape coordinates (x 0..1919 left->right, y 0..479 top->bottom), but LVGL rotates
	 * pointer input with the display. Feed it (479 - y, x), which its 270 rotation turns back into (x, y). */
	if (ts) {
		lv_evdev_set_swap_axes(ts, true);
		lv_evdev_set_calibration(ts, 479, 0, 0, 1919);
	}

	lv_obj_t *scr = lv_screen_active();
	lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);
	lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(scr, on_touch, LV_EVENT_PRESSING, NULL);
	lv_obj_add_event_cb(scr, on_touch, LV_EVENT_PRESSED, NULL);

	corner(scr, "TOP-LEFT", LV_ALIGN_TOP_LEFT, 140, 20);
	corner(scr, "TOP-RIGHT", LV_ALIGN_TOP_RIGHT, -140, 20);
	corner(scr, "BOTTOM-LEFT", LV_ALIGN_BOTTOM_LEFT, 20, -20);
	corner(scr, "BOTTOM-RIGHT", LV_ALIGN_BOTTOM_RIGHT, -20, -20);

	lv_obj_t *title = lv_label_create(scr);
	lv_label_set_text(title, "Wattline - our own dash");
	lv_obj_set_style_text_font(title, &lv_font_montserrat_48, 0);
	lv_obj_set_style_text_color(title, lv_color_hex(0x31d158), 0);
	lv_obj_center(title);

	touch_lbl = corner(scr, "touch the screen", LV_ALIGN_CENTER, 0, 70);

	dot = lv_obj_create(scr);
	lv_obj_set_size(dot, 40, 40);
	lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(dot, lv_color_hex(0xff9500), 0);
	lv_obj_set_style_border_width(dot, 0, 0);
	lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_pos(dot, 940, 220);

	for (;;) {
		uint32_t ms = lv_timer_handler();
		usleep((ms > 33 ? 33 : ms) * 1000);
	}
}
