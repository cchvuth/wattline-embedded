// SPDX-License-Identifier: MIT
// fb_rot: the display's flush for a rotated panel: the G2D's rotation unit, else the CPU in tiles (fb_rot.c)
#pragma once
#include <stdbool.h>
#include <stdint.h>
#ifndef FB_ROT_AREA_ONLY
#include "lvgl.h"
// LVGL's flush replaced for a 90 or 270 degree panel at 32 bits a pixel; false (LVGL's own kept) otherwise
bool fb_rot_install(lv_display_t *disp, const char *path, int rotation, bool vsync);
// the G2D's rotation unit took the flush (the OS's /dev/g2drot): it runs beside the drawing
bool fb_rot_g2d(void);
#endif
// one w x h area of the rotated canvas at (x1, y1), 32-bit pixels, into the panel's framebuffer (exposed for the test)
void fb_rot_area(uint8_t *dst, long dst_line, int panel_w, int panel_h, int rotation, int x1, int y1, int w, int h,
                 const uint8_t *src, long src_stride);
