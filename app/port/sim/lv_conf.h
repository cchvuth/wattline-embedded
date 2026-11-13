/* Host (Mac) headless renderer: the dash UI's config (ui/conf/lv_conf.h) without the Linux display and
 * input drivers, plus lodepng to write the PNGs. */
#ifndef SIM_LV_CONF_H
#define SIM_LV_CONF_H
#include "../../ui/conf/lv_conf.h"
#undef LV_USE_LINUX_FBDEV
#define LV_USE_LINUX_FBDEV 0
#undef LV_USE_EVDEV
#define LV_USE_EVDEV 0
#undef LV_USE_LODEPNG
#define LV_USE_LODEPNG 1
#undef LV_USE_PERF_MONITOR
#define LV_USE_PERF_MONITOR 0
#undef LV_USE_LOG
#define LV_USE_LOG 1
#endif
