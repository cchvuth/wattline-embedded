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
#endif
