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

int main(int argc, char **argv)
{
	const char *out = argc > 1 ? argv[1] : "build/shots";
	int W = argc > 3 ? atoi(argv[2]) : 1920, H = argc > 3 ? atoi(argv[3]) : 480, bad = 0;
	lv_init();
	lv_tick_set_cb(ms);
	lv_display_t *d = lv_display_create(W, H);
	lv_display_set_color_format(d, LV_COLOR_FORMAT_XRGB8888);
	size_t sz = (size_t)W * H * 4;
	uint8_t *buf = malloc(sz), *part = malloc(sz), *rgba = malloc(sz);
	lv_display_set_buffers(d, buf, NULL, sz, LV_DISPLAY_RENDER_MODE_DIRECT); /* redraws only the dirty areas */
	lv_display_set_flush_cb(d, flush);

	dash_ui_init(lv_screen_active());
	for (int i = 0; i < scene_count; i++) {
		dash_state_t s;
		scenes[i].fill(&s);
		uint32_t t0 = ms();
		dash_ui_set(&s);
		lv_refr_now(d);
		uint32_t t = ms() - t0;
		memcpy(part, buf, sz);
		lv_obj_invalidate(lv_screen_active());
		lv_refr_now(d);
		size_t diff = 0;
		for (size_t p = 0; p < sz; p += 4) diff += memcmp(part + p, buf + p, 3) != 0;
		if (diff) bad = 1;
		for (size_t p = 0; p < sz; p += 4) /* XRGB8888 is B, G, R, X in memory */
			rgba[p] = buf[p + 2], rgba[p + 1] = buf[p + 1], rgba[p + 2] = buf[p], rgba[p + 3] = 255;
		char path[512];
		snprintf(path, sizeof path, "%s/%s.png", out, scenes[i].name);
		unsigned char *png;
		size_t len;
		unsigned err = lodepng_encode32(&png, &len, rgba, W, H);
		FILE *f = err ? NULL : fopen(path, "wb");
		if (f) fwrite(png, 1, len, f), fclose(f);
		lv_free(png);
		printf("%-20s %s  set+partial redraw %u ms%s\n", scenes[i].name, err ? lodepng_error_text(err) : f ? path : "can't write", t,
		       diff ? "  STALE PIXELS after the partial redraw" : "");
		if (diff) printf("  %zu pixels differ\n", diff);
	}
	return bad;
}
