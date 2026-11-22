/* The dash UI's LVGL config: the device's (port/linux/lv_conf.h), plus TrueType text. The host renderer
 * (port/sim/lv_conf.h) builds on this one, so what it draws is what the Dash draws. */
#ifndef UI_LV_CONF_H
#define UI_LV_CONF_H
#include "../../port/linux/lv_conf.h"
#undef LV_USE_TINY_TTF
#define LV_USE_TINY_TTF 1
#undef LV_TINY_TTF_CACHE_GLYPH_CNT
#define LV_TINY_TTF_CACHE_GLYPH_CNT 64
#undef LV_MEM_SIZE
#define LV_MEM_SIZE (12 * 1024 * 1024U)
#undef LV_USE_QRCODE
#define LV_USE_QRCODE 1 /* the SSH pairing view */
/* Areas to redraw per frame before LVGL gives up and redraws the whole screen (32 by default): in the Android Auto
 * split a whole-screen redraw paints over the phone's picture, and a frame where all the car data arrives at once
 * reached 33 */
#define LV_INV_BUF_SIZE 128
/* DASH_VSYNC (the device profile's, device/<name>.mk): each frame's first flush waits for the panel's vertical blank
 * (FBIO_WAITFORVSYNC, DRM's fbdev has it; the driver stops asking if not): the F133 panel scans across the landscape
 * picture, so a frame drawn from the blank is mostly ahead of the scan (less tearing at 60 frames a second). The
 * profile's own flush (DASH_FB_ROT, port/linux/fb_rot.c) skips the wait after a frame whose flushes ran past half a
 * scan */
#ifdef DASH_VSYNC
#undef LV_LINUX_FBDEV_VSYNC
#define LV_LINUX_FBDEV_VSYNC 1
#endif
/* DASH_FB_ROWS (the profile's): the draw buffer, in rows of the panel's own width (LVGL's default 60); taller strips of
 * the turned canvas mean fewer passes over the widget tree a frame (F133, Settings scroll: 29 -> 59 lines a strip,
 * render 27 -> 25 ms) */
#ifdef DASH_FB_ROWS
#undef LV_LINUX_FBDEV_BUFFER_SIZE
#define LV_LINUX_FBDEV_BUFFER_SIZE DASH_FB_ROWS
#endif
/* Measuring (EXTRA_DEFS=-DDASH_PERF_LOG): the perf monitor's FPS and render/flush times to the log each second */
#ifdef DASH_PERF_LOG
#undef LV_USE_PERF_MONITOR_LOG_MODE
#define LV_USE_PERF_MONITOR_LOG_MODE 1
#endif
/* DASH_TTF_GLYPHS: the TrueType glyph cache, per font: a page with more distinct glyphs than this re-rasterizes text
 * each frame it redraws (a scroll) */
#ifdef DASH_TTF_GLYPHS
#undef LV_TINY_TTF_CACHE_GLYPH_CNT
#define LV_TINY_TTF_CACHE_GLYPH_CNT DASH_TTF_GLYPHS
#endif
#endif
