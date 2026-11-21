/* LVGL's custom software blends (LV_USE_DRAW_SW_ASM CUSTOM, ui/conf/lv_conf.h) for the dash's XRGB8888 draw buffer: the
 * masked fills (text, arcs, round corners, the A8 icons) and the ARGB8888 images. LVGL's own go byte by byte through
 * each pixel, empty mask or not; these take a pixel as one word and step over four empty mask bytes at a time (a glyph's
 * or an arc's box is mostly empty). The arithmetic is LVGL's (lv_color_24_24_mix: 16-bit lanes, >> 8, a copy from
 * LV_OPA_MAX up), so every pixel comes out the same, its fourth byte left as it was. Any other buffer: LVGL's own
 * (LV_RESULT_INVALID). The hooks are macros over the descriptors' fields: LVGL includes this before its blend types
 * are complete (its config includes it first of all, before any type: nothing here until its colours are in). */
#if defined(LV_DRAW_SW_BLEND_H) && defined(LV_OPA_MIX2) && !defined(BLEND_XRGB_H) && !LV_BIG_ENDIAN_SYSTEM /* (words: little-endian) */
#define BLEND_XRGB_H

typedef union { uint32_t u32; uint8_t u8[4]; } xrgb_word_t; /* a word read through a union, as LVGL's lv_draw_sw_word_t */
/* lv_color_24_24_mix on whole words, mix 1..252: red and blue in one multiplication, green in another */
static inline uint32_t xrgb_mix(uint32_t c, uint32_t d, uint32_t mix)
{
	uint32_t inv = 255 - mix;
	return ((((c & 0xFF00FFu) * mix + (d & 0xFF00FFu) * inv) >> 8) & 0xFF00FFu) |
	       ((((c & 0xFF00u) * mix + (d & 0xFF00u) * inv) >> 8) & 0xFF00u) | (d & 0xFF000000u);
}
static inline void xrgb_px(uint32_t *d, uint32_t c, uint32_t mix)
{
	if (mix >= LV_OPA_MAX) *d = (*d & 0xFF000000u) | (c & 0xFFFFFFu);
	else if (mix) *d = xrgb_mix(c, *d, mix);
}
/* A colour through a mask, [opa] over it all (255: none): the mask a word at a time once aligned, four empty bytes
 * stepped over (four full ones copied, at full opacity) at once. */
#define XRGB_MASK_PX(x, m) xrgb_px(&d[x], c, opa < 255 ? LV_OPA_MIX2(opa, m) : (m))
static inline lv_result_t xrgb_color_mask(uint8_t *row, int32_t w, int32_t h, int32_t stride, const lv_opa_t *mask,
					  int32_t mask_stride, uint32_t c, uint32_t px_size, uint32_t opa)
{
	if (px_size != 4) return LV_RESULT_INVALID;
	uint32_t full = c & 0xFFFFFFu;
	for (int32_t y = 0; y < h; y++, row += stride, mask += mask_stride) {
		uint32_t *d = (uint32_t *)row;
		int32_t x = 0;
		for (; x < w && ((lv_uintptr_t)(mask + x) & 3); x++) XRGB_MASK_PX(x, mask[x]);
		for (; x + 4 <= w; x += 4) {
			uint32_t m4 = ((const xrgb_word_t *)(mask + x))->u32;
			if (m4 == 0) continue;
			if (m4 == 0xFFFFFFFFu && opa == 255) {
				d[x] = (d[x] & 0xFF000000u) | full, d[x + 1] = (d[x + 1] & 0xFF000000u) | full;
				d[x + 2] = (d[x + 2] & 0xFF000000u) | full, d[x + 3] = (d[x + 3] & 0xFF000000u) | full;
				continue;
			}
			const uint8_t *m = mask + x;
			XRGB_MASK_PX(x, m[0]), XRGB_MASK_PX(x + 1, m[1]), XRGB_MASK_PX(x + 2, m[2]), XRGB_MASK_PX(x + 3, m[3]);
		}
		for (; x < w; x++) XRGB_MASK_PX(x, mask[x]);
	}
	return LV_RESULT_OK;
}
#define XRGB_FILL(dsc, px_size, opa) xrgb_color_mask((dsc)->dest_buf, (dsc)->dest_w, (dsc)->dest_h, (dsc)->dest_stride, \
						     (dsc)->mask_buf, (dsc)->mask_stride, lv_color_to_u32((dsc)->color), px_size, opa)
#define LV_DRAW_SW_COLOR_BLEND_TO_RGB888_WITH_MASK(dsc, px_size) XRGB_FILL(dsc, px_size, 255)
#define LV_DRAW_SW_COLOR_BLEND_TO_RGB888_MIX_MASK_OPA(dsc, px_size) XRGB_FILL(dsc, px_size, (dsc)->opa)

/* An ARGB8888 image (each pixel's alpha; [opa], 255 none; the mask's, if any). Its pixels read as words where the image
 * is aligned (the generated assets are byte arrays: not always). */
static inline uint32_t xrgb_src(const uint8_t *s, bool aligned)
{
	if (aligned) return ((const xrgb_word_t *)s)->u32;
	uint32_t v;
	__builtin_memcpy(&v, s, 4);
	return v;
}
static inline lv_result_t xrgb_argb(uint8_t *row, int32_t w, int32_t h, int32_t stride, const uint8_t *src,
				    int32_t src_stride, const lv_opa_t *mask, int32_t mask_stride, uint32_t px_size, uint32_t opa)
{
	if (px_size != 4) return LV_RESULT_INVALID;
	bool aligned = ((lv_uintptr_t)src & 3) == 0 && (src_stride & 3) == 0;
	for (int32_t y = 0; y < h; y++, row += stride, src += src_stride) {
		uint32_t *d = (uint32_t *)row;
		for (int32_t x = 0; x < w; x++) {
			uint32_t s = xrgb_src(src + 4 * x, aligned), a = s >> 24;
			if (mask) a = opa < 255 ? LV_OPA_MIX3(a, mask[x], opa) : LV_OPA_MIX2(a, mask[x]);
			else if (opa < 255) a = LV_OPA_MIX2(a, opa);
			xrgb_px(&d[x], s, a);
		}
		if (mask) mask += mask_stride;
	}
	return LV_RESULT_OK;
}
#define XRGB_IMAGE(dsc, px_size, opa, mask) xrgb_argb((dsc)->dest_buf, (dsc)->dest_w, (dsc)->dest_h, (dsc)->dest_stride, \
						      (dsc)->src_buf, (dsc)->src_stride, mask, (dsc)->mask_stride, px_size, opa)
#define LV_DRAW_SW_ARGB8888_BLEND_NORMAL_TO_RGB888(dsc, px_size) XRGB_IMAGE(dsc, px_size, 255, NULL)
#define LV_DRAW_SW_ARGB8888_BLEND_NORMAL_TO_RGB888_WITH_OPA(dsc, px_size) XRGB_IMAGE(dsc, px_size, (dsc)->opa, NULL)
#define LV_DRAW_SW_ARGB8888_BLEND_NORMAL_TO_RGB888_WITH_MASK(dsc, px_size) XRGB_IMAGE(dsc, px_size, 255, (dsc)->mask_buf)
#define LV_DRAW_SW_ARGB8888_BLEND_NORMAL_TO_RGB888_MIX_MASK_OPA(dsc, px_size) XRGB_IMAGE(dsc, px_size, (dsc)->opa, (dsc)->mask_buf)
#endif
