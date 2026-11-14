// SPDX-License-Identifier: MIT
// project_region.h — where the phone's picture (Android Auto) sits on the Dash's panel, in panel pixels of the
// 1920x480 landscape canvas. One definition for the two programs that must agree on it: the dash UI (the split
// layout, and the touches it forwards) and the AA receiver (what it asks the phone to render, where the decoder
// draws it, and how a touch maps back into the phone's frame).
//
// The panel is a trapezoid: its left and right edges slant in by 115 px at the top (dash_ui.c inset()). The
// picture is a rectangle clear of the left slant, up to the 2/3 line; the slanted side beside it keeps the Wi-Fi
// and Bluetooth status, and the right third the compact dash.
#pragma once
#include <stdint.h>

#define PJ_PANEL_W 1920
#define PJ_RGN_X 120    /* left edge: just inside the slant's widest point (115 px, at the top) */
#define PJ_RGN_W 1160   /* to the 2/3 line (1280) */
#define PJ_RGN_H 480    /* the panel's full height */

// Touches on the picture, sent by the dash UI (which owns the touchscreen) to the AA receiver: one datagram per
// event on this UNIX socket, coordinates in picture pixels (0..PJ_RGN_W-1, 0..PJ_RGN_H-1).
#define PJ_TOUCH_SOCK "/run/dash-touch"
enum { PJ_TOUCH_PRESS = 0, PJ_TOUCH_RELEASE = 1, PJ_TOUCH_DRAG = 2 };   /* = Android Auto's TouchAction */
typedef struct { uint8_t action; uint8_t pad; uint16_t x, y; } pj_touch_t;
