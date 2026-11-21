/* ui/conf/blend_xrgb.h against LVGL's own blends into a 32-bit buffer, pixel for pixel: the masked fills (with and
 * without an opacity) and the ARGB8888 images (alone, with an opacity, a mask, both), on random pixels, masks with runs
 * of empty and full bytes at every alignment, and rows of every width. The reference is LVGL's arithmetic as
 * lv_draw_sw_blend_to_rgb888.c has it (lv_color_24_24_mix, its loops): the fourth byte must stay as it was. */
#include "lvgl.h"
#include "src/draw/sw/blend/lv_draw_sw_blend.h" /* (with it, the blends: ui/conf/blend_xrgb.h) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void ref_mix(const uint8_t *src, uint8_t *dest, uint8_t mix) /* lv_color_24_24_mix */
{
	if (mix == 0) return;
	if (mix >= LV_OPA_MAX) {
		dest[0] = src[0], dest[1] = src[1], dest[2] = src[2];
		return;
	}
	lv_opa_t inv = 255 - mix;
	uint32_t rb = ((((uint32_t)src[2] << 16) | src[0]) * mix) + ((((uint32_t)dest[2] << 16) | dest[0]) * inv);
	dest[0] = (uint8_t)(rb >> 8);
	dest[1] = (uint32_t)((uint32_t)src[1] * mix + dest[1] * inv) >> 8;
	dest[2] = (uint8_t)(rb >> 24);
}
static uint32_t rnd(void)
{
	static uint32_t s = 12345;
	return s = s * 1103515245u + 12345u, s >> 8;
}
static uint8_t mask_byte(int kind)
{
	return kind == 0 ? 0 : kind == 1 ? 255 : (uint8_t)rnd();
}

int main(void)
{
	enum { W = 70, H = 5, ROUNDS = 4000 };
	static uint32_t dst[H][W + 3], ref[H][W + 3], src[H * (W + 3) + 1];
	static uint8_t mask[H * (W + 8) + 8];
	int bad = 0;
	for (int r = 0; r < ROUNDS && bad < 5; r++) {
		int w = 1 + rnd() % W, h = 1 + rnd() % H, ofs = rnd() % 4, mstride = w + rnd() % 5, kind = rnd() % 3;
		int opa = rnd() % 3 == 0 ? 255 : (int)(rnd() % 253);
		for (size_t i = 0; i < sizeof dst / 4; i++) ((uint32_t *)dst)[i] = ((uint32_t *)ref)[i] = rnd() ^ (rnd() << 16);
		for (size_t i = 0; i < sizeof mask; i++) mask[i] = i % 7 == 0 ? mask_byte(rnd() % 3) : mask_byte(kind);
		for (size_t i = 0; i < sizeof src / 4; i++) src[i] = rnd() ^ (rnd() << 16), src[i] = rnd() % 4 == 0 ? src[i] & 0xFFFFFF : src[i];
		const uint8_t *m = mask + ofs;
		uint32_t c = 0xFF000000u | (rnd() & 0xFFFFFF);
		int test = rnd() % 2, masked = rnd() % 2;
		if (test == 0) { /* a colour through the mask */
			xrgb_color_mask((uint8_t *)dst, w, h, sizeof dst[0], m, mstride, c, 4, opa);
			for (int y = 0; y < h; y++)
				for (int x = 0; x < w; x++)
					ref_mix((const uint8_t *)&c, (uint8_t *)&ref[y][x], opa < 255 ? LV_OPA_MIX2(opa, m[y * mstride + x]) : m[y * mstride + x]);
		} else { /* an image, unaligned at times (the generated assets are byte arrays) */
			const uint8_t *s = (const uint8_t *)src + (rnd() % 2 ? 0 : 1);
			int sstride = (w + rnd() % 3) * 4;
			xrgb_argb((uint8_t *)dst, w, h, sizeof dst[0], s, sstride, masked ? m : NULL, mstride, 4, opa);
			for (int y = 0; y < h; y++)
				for (int x = 0; x < w; x++) {
					const uint8_t *p = s + y * sstride + x * 4;
					uint8_t a = p[3], mk = masked ? m[y * mstride + x] : 255;
					uint8_t mix = masked ? (opa < 255 ? LV_OPA_MIX3(a, mk, opa) : LV_OPA_MIX2(a, mk)) : opa < 255 ? LV_OPA_MIX2(a, opa) : a;
					ref_mix(p, (uint8_t *)&ref[y][x], mix);
				}
		}
		if (memcmp(dst, ref, sizeof dst)) {
			printf("FAIL round %d: %s w %d h %d mask at +%d opa %d\n", r, test ? "image" : "colour", w, h, ofs, opa);
			bad++;
		}
	}
	if (xrgb_color_mask((uint8_t *)dst, 1, 1, 12, mask, 1, 0, 3, 255) != LV_RESULT_INVALID) printf("FAIL 24-bit taken\n"), bad++;
	printf("test_blend_xrgb: %d rounds, %s\n", ROUNDS, bad ? "FAILED" : "all as LVGL's");
	return bad != 0;
}
