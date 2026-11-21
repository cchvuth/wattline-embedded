// SPDX-License-Identifier: MIT
// fb_rot: the display's flush for a rotated panel: each area LVGL draws of the landscape canvas, turned into the
// portrait framebuffer. LVGL's fbdev driver rotates each area into a buffer of its own (walking the source rows,
// writing down the destination's columns) and then copies that buffer into the framebuffer: on the F133's C906 (32 KB
// L1, no L2, 16-bit DDR2) the rotate alone took ~70 ms of each scrolled frame.
// The F133's G2D has a rotation unit (its OS's g2drot module, /dev/g2drot): it turns each area into the framebuffer at
// ~6 ns a pixel from draw buffers the module gives (contiguous, cached), while LVGL draws the next area into the other
// one: the flush runs beside the drawing (Settings scroll: 43 -> 0.4 ms of each frame). Without the unit (or once it
// fails) the CPU does it: each tile read along its source rows and written as runs of the panel's rows, straight into
// the framebuffer (26-42 ns a pixel). Installed over LVGL's driver (which still opens and maps the device) for 90 and
// 270 degrees at 32 bits a pixel; anything else keeps LVGL's own flush. The device profile turns it on (DASH_FB_ROT).
#include "fb_rot.h"
#ifndef FB_ROT_AREA_ONLY
#include <fcntl.h>
#include <linux/fb.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

#ifndef FB_ROT_AREA_ONLY
// the g2drot module's interface (a copy of its g2drot.h)
struct g2drot_fb { uint64_t va; uint32_t len, pitch, width, height; };       // the framebuffer as mapped here
struct g2drot_blit { uint32_t off, pitch, w, h; int32_t x, y; uint32_t rot; };   // an area at off in the buffers
#define G2DROT_FB _IOW('G', 1, struct g2drot_fb)
#define G2DROT_BLIT _IOW('G', 2, struct g2drot_blit)   // starts it (after the one before) and returns
#define G2DROT_WAIT _IO('G', 3)                        // until the last one is in the framebuffer

static uint8_t *fb;          // the panel's framebuffer, mapped
static long fb_line;         // its bytes a row
static int pw, ph;           // the panel's own size (portrait: 720 x 1920 on the F133)
static int rot;              // 90 or 270
static int vfd = -1;         // FBIO_WAITFORVSYNC, when the profile asks for it
static bool first = true;    // the next flush is a frame's first
static uint32_t frame_t0, span_ms;   // when the frame's first flush began; how long the last frame's flushes ran
static int g2d = -1;         // /dev/g2drot
static uint8_t *g2d_buf;     // LVGL's two draw buffers, the unit's memory
static size_t g2d_len, g2d_bs, g2d_full;   // that mapping's length; one strip buffer's; the whole canvas's
static struct g2drot_fb g2d_fb;  // the framebuffer, as the unit is told it
static bool whole;           // the first frame, drawn in one buffer of the whole canvas and shown at once (strips() after)
static uint8_t *own;         // LVGL's own draw buffer (unused while the unit's are in): the CPU's, should the unit's go
static uint32_t own_bs;
static void strips(void *disp);
#endif

// One area of the rotated (landscape) canvas, w x h pixels of 32 bits, src_stride bytes a row, to the panel.
// 270: landscape (x, y) is panel (pw-1-y, x). 90: landscape (x, y) is panel (y, ph-1-x).
void fb_rot_area(uint8_t *dst, long dst_line, int panel_w, int panel_h, int rotation, int x1, int y1, int w, int h,
                 const uint8_t *src, long src_stride)
{
	// tiles 16 wide and up to 64 tall: each panel row gets one run of up to 64 pixels (the write buffer merges a run:
	// 47 ns a pixel at 16 tall, 23 at 60, measured on the F133), the tile's source lines staying in the L1 meanwhile
	for (int ty = 0; ty < h; ty += 64) {
		int te = ty + 64 < h ? ty + 64 : h;
		for (int tx = 0; tx < w; tx += 16) {
			int xe = tx + 16 < w ? tx + 16 : w;
			for (int x = tx; x < xe; x++) {   // landscape column x: one panel row, written as a run
				const uint8_t *s = src + (long)x * 4;
				if (rotation == 270) {         // panel row x1+x, columns pw-1-(y1+y): y descending = address rising
					uint32_t *d = (uint32_t *)(dst + (long)(x1 + x) * dst_line) + (panel_w - 1 - (y1 + te - 1));
					for (int y = te - 1; y >= ty; y--) *d++ = *(const uint32_t *)(s + (long)y * src_stride);
				} else {                       // 90: panel row ph-1-(x1+x), columns y1+y: y ascending
					uint32_t *d = (uint32_t *)(dst + (long)(panel_h - 1 - (x1 + x)) * dst_line) + (y1 + ty);
					for (int y = ty; y < te; y++) *d++ = *(const uint32_t *)(s + (long)y * src_stride);
				}
			}
		}
	}
}

#ifndef FB_ROT_AREA_ONLY
// Each frame's first flush waits for the panel's vertical blank, unless the last frame's flushes ran longer than half
// a scan (a scroll, a page switch): such a frame tears whatever, and the wait only made it up to a scan longer
// (Settings scroll: frames of 33 ms, two scans, where 26 ms of work needs no more)
static void frame_begin(void)
{
	if (!first) return;
	if (vfd >= 0 && span_ms < 8) {
		int z = 0;
		if (ioctl(vfd, FBIO_WAITFORVSYNC, &z) < 0) vfd = -1;   // unsupported: stop asking
	}
	frame_t0 = lv_tick_get();
}

static void frame_end(lv_display_t *disp)
{
	first = lv_display_flush_is_last(disp);
	if (first) span_ms = lv_tick_elaps(frame_t0);
}

static void flush(lv_display_t *disp, const lv_area_t *a, uint8_t *px)
{
	frame_begin();
	int w = lv_area_get_width(a), h = lv_area_get_height(a);
	fb_rot_area(fb, fb_line, pw, ph, rot, a->x1, a->y1, w, h, px, (long)lv_draw_buf_width_to_stride(w, lv_display_get_color_format(disp)));
	frame_end(disp);
	lv_display_flush_ready(disp);
}

static void redraw_all(void *unused) { (void)unused; lv_obj_invalidate(lv_screen_active()); }

static void g2d_failed(lv_display_t *disp)   // the unit gone wrong: the CPU's tiles from now on, the whole screen again
{
	close(g2d);   // (its buffers stay mapped: LVGL goes on drawing in them)
	g2d = -1;
	lv_display_set_flush_wait_cb(disp, NULL);
	lv_display_set_flush_cb(disp, flush);
	lv_async_call(redraw_all, NULL);   // after this frame: LVGL takes no new dirty areas while it draws
	if (whole) whole = false, lv_async_call(strips, disp);   // the whole canvas's buffer: given back after it too
}

static void flush_wait_g2d(lv_display_t *disp)   // LVGL's wait before it reuses a buffer the unit may still read
{
	if (g2d >= 0 && ioctl(g2d, G2DROT_WAIT)) g2d_failed(disp);
}

static void flush_g2d(lv_display_t *disp, const lv_area_t *a, uint8_t *px)
{
	frame_begin();
	int w = lv_area_get_width(a), h = lv_area_get_height(a);
	struct g2drot_blit b = {(uint32_t)(px - g2d_buf), lv_draw_buf_width_to_stride(w, lv_display_get_color_format(disp)),
	                        (uint32_t)w, (uint32_t)h, a->x1, a->y1, (uint32_t)rot};
	if (ioctl(g2d, G2DROT_BLIT, &b)) {   // not taken: this area by the CPU, and every later one
		g2d_failed(disp);
		first = false;   // (this frame's blank waited for already)
		flush(disp, a, px);
		return;
	}
	if (whole && lv_display_flush_is_last(disp)) whole = false, lv_async_call(strips, disp);   // after this frame
	frame_end(disp);
}   // no flush_ready: LVGL's wait (flush_wait_g2d) ends it

// The unit opened with one buffer of len bytes (its memory, freed only as its fd closes) and told the framebuffer
static bool g2d_map(size_t len)
{
	g2d = open("/dev/g2drot", O_RDWR | O_CLOEXEC);
	void *b = g2d < 0 ? MAP_FAILED : mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, g2d, 0);
	if (b != MAP_FAILED && !ioctl(g2d, G2DROT_FB, &g2d_fb)) {
		g2d_buf = b, g2d_len = len;
		return true;
	}
	if (b != MAP_FAILED) munmap(b, len);
	if (g2d >= 0) close(g2d);
	g2d = -1;
	return false;
}
static void g2d_unmap(void)   // its last job waited for, its buffer back to the CMA (as the mapping and the fd both go)
{
	if (g2d >= 0) ioctl(g2d, G2DROT_WAIT), close(g2d), g2d = -1;
	if (g2d_buf) munmap(g2d_buf, g2d_len), g2d_buf = NULL;
}

bool fb_rot_install(lv_display_t *disp, const char *path, int rotation, bool vsync)
{
	if (rotation != 90 && rotation != 270) return false;
	int fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0) return false;
	struct fb_var_screeninfo v;
	struct fb_fix_screeninfo f;
	lv_color_format_t cf = lv_display_get_color_format(disp);
	if (ioctl(fd, FBIOGET_VSCREENINFO, &v) || ioctl(fd, FBIOGET_FSCREENINFO, &f) || v.bits_per_pixel != 32 ||
	    (cf != LV_COLOR_FORMAT_XRGB8888 && cf != LV_COLOR_FORMAT_ARGB8888)) {
		close(fd);
		return false;
	}
	void *m = mmap(NULL, f.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (m == MAP_FAILED) { close(fd); return false; }
	fb = m, fb_line = f.line_length, pw = (int)v.xres, ph = (int)v.yres, rot = rotation;
	vfd = vsync ? fd : -1;
	lv_display_set_flush_cb(disp, flush);
	// the unit, when the OS has it: two draw buffers of LVGL's size from it (LVGL's own one stays unused)
	// The first frame drawn whole where the panel does not show it, then turned into the framebuffer at once: in strips
	// it took ~1 s top to bottom over the boot splash (everything cold from the flash). The first frame is the loop's
	// first (main.c draws nothing before it: the settings applied by then, so no default layout flashes up); then
	// strips() goes back to the two strip buffers, the whole canvas's 5.3 MB of CMA left for the Android Auto decoder.
	g2d_bs = (size_t)f.line_length * LV_LINUX_FBDEV_BUFFER_SIZE;
	g2d_fb = (struct g2drot_fb){(uintptr_t)m, f.smem_len, f.line_length, v.xres, v.yres};
	size_t full = (size_t)lv_draw_buf_width_to_stride(lv_display_get_horizontal_resolution(disp), cf) *
	              lv_display_get_vertical_resolution(disp);
	lv_draw_buf_t *b = lv_display_get_buf_active(disp);
	own = b->data, own_bs = b->data_size;
	g2d_full = full;
	if (g2d_map(full)) {
		whole = true;
		lv_display_set_buffers(disp, g2d_buf, NULL, (uint32_t)full, LV_DISPLAY_RENDER_MODE_PARTIAL);
	} else if (g2d_map(2 * g2d_bs))
		lv_display_set_buffers(disp, g2d_buf, g2d_buf + g2d_bs, (uint32_t)g2d_bs, LV_DISPLAY_RENDER_MODE_PARTIAL);
	if (g2d >= 0) {
		lv_display_set_flush_wait_cb(disp, flush_wait_g2d);
		lv_display_set_flush_cb(disp, flush_g2d);
	}
	return true;
}

static void strips(void *d)   // after the first frame: the two strip buffers again, the whole canvas's back to the CMA
{
	lv_display_t *disp = d;
	if (g2d >= 0) {
		ioctl(g2d, G2DROT_WAIT);
		lv_display_flush_ready(disp);   // (LVGL waits for the unit only before it draws again)
		g2d_unmap();
		if (g2d_map(2 * g2d_bs)) {
			lv_display_set_buffers(disp, g2d_buf, g2d_buf + g2d_bs, (uint32_t)g2d_bs, LV_DISPLAY_RENDER_MODE_PARTIAL);
			return;
		}
	}
	// the unit gone, or its strips not to be had: the CPU's tiles from LVGL's own buffer (the big one is unmapped)
	lv_display_set_buffers(disp, own, NULL, own_bs, LV_DISPLAY_RENDER_MODE_PARTIAL);
	lv_display_set_flush_wait_cb(disp, NULL);
	lv_display_set_flush_cb(disp, flush);
	g2d_unmap();
}

bool fb_rot_g2d(void) { return g2d >= 0; }

// The next frame drawn whole, in one buffer of the whole canvas, and turned into the framebuffer at once, as the first
// was: a change of the whole view crawled down the screen in strips. Between frames only. If the unit's memory does not
// allow it now (the Android Auto decoder holds it), the strips stay, and the frame comes in strips as before; after
// the frame, strips() gives the big buffer back.
void fb_rot_whole_next(lv_display_t *disp)
{
	if (g2d < 0 || whole || !g2d_full) return;
	ioctl(g2d, G2DROT_WAIT);
	lv_display_flush_ready(disp);
	g2d_unmap();
	if (g2d_map(g2d_full)) {
		whole = true;
		lv_display_set_buffers(disp, g2d_buf, NULL, (uint32_t)g2d_full, LV_DISPLAY_RENDER_MODE_PARTIAL);
		return;
	}
	if (g2d_map(2 * g2d_bs)) {   // not now: the strips again
		lv_display_set_buffers(disp, g2d_buf, g2d_buf + g2d_bs, (uint32_t)g2d_bs, LV_DISPLAY_RENDER_MODE_PARTIAL);
		return;
	}
	strips(disp);   // nor they: the CPU's tiles
}
#endif
