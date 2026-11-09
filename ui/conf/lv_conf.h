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
#endif
