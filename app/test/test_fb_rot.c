// SPDX-License-Identifier: MIT
// test_fb_rot: fb_rot_area's tiles against LVGL's two steps (lv_draw_utils.c rotate90/270_argb8888, then the area
// placed by lv_display_rotate_area), on random areas of a 1920x720 canvas turned onto a 720x1920 panel
#define FB_ROT_AREA_ONLY
#include "../port/linux/fb_rot.c"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
enum { PW = 720, PH = 1920, LINE = PW * 4 };
static void ref(uint32_t *fbp, int rot, int x1, int y1, int w, int h, const uint32_t *src)
{
	static uint32_t tmp[1920 * 720];
	int x1r, y1r;   // lv_display_rotate_area with the panel's own (unrotated) hor_res PW and ver_res PH
	if (rot == 90) {   // rotate90_argb8888: dst[(w-1-x) * h + y]; placed at (y1, PH - x1 - w)
		for (int x = 0; x < w; x++) for (int y = 0; y < h; y++) tmp[(w - 1 - x) * h + y] = src[y * w + x];
		x1r = y1, y1r = PH - x1 - w;
	} else {           // rotate270_argb8888: dst[x * h + (h-1-y)]; placed at (PW - (y1+h-1) - 1, x1)
		for (int x = 0; x < w; x++) for (int y = 0; y < h; y++) tmp[x * h + (h - 1 - y)] = src[y * w + x];
		x1r = PW - (y1 + h - 1) - 1, y1r = x1;
	}
	for (int r = 0; r < w; r++) memcpy(fbp + (y1r + r) * PW + x1r, tmp + r * h, (size_t)h * 4);   // h wide, w tall
}
int main(void)
{
	static uint32_t a[PW * PH], b[PW * PH], src[1920 * 720];
	int fails = 0, n = 0;
	srand(7);
	for (int rot = 90; rot <= 270; rot += 180)
		for (int t = 0; t < 300; t++, n++) {
			int w = 1 + rand() % 1920, h = 1 + rand() % 720, x1 = rand() % (1921 - w), y1 = rand() % (721 - h);
			if (t < 4) w = t & 1 ? 1920 : 17, h = t & 2 ? 720 : 33, x1 = 1920 - w, y1 = 720 - h;   // edges, odd sizes
			for (int i = 0; i < w * h; i++) src[i] = (uint32_t)rand() * 2654435761u;
			memset(a, 0x5a, sizeof a), memset(b, 0x5a, sizeof b);
			ref(a, rot, x1, y1, w, h, src);
			fb_rot_area((uint8_t *)b, LINE, PW, PH, rot, x1, y1, w, h, (const uint8_t *)src, (long)w * 4);
			if (memcmp(a, b, sizeof a)) { if (fails++ < 5) printf("FAIL rot %d area %d,%d %dx%d\n", rot, x1, y1, w, h); }
		}
	printf("test_fb_rot: %d areas, %d failed\n", n, fails);
	return fails != 0;
}
