// SPDX-License-Identifier: MIT
// project_region.h — where the phone's picture (Android Auto) sits on the Dash's panel, in panel pixels of the
// 1920x480 landscape canvas. One definition for the two programs that must agree on it: the dash UI (the split
// layout) and the AA receiver (what it asks the phone to render, where the decoder draws it, and how a touch maps
// back into the phone's frame).
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

// The dash UI raises PJ_VISIBLE while the picture is on screen (the split view) and drops it otherwise. The AA
// receiver reads the touchscreen itself (every finger, the hardware's own timestamps) and sends touches to the
// phone only while it exists. The touchscreen reports panel pixels of the landscape canvas.
#define PJ_VISIBLE "/run/dash-pj-visible"
// "1" after dark (the car's headlights on: the dash's own night brightness rule), "0" by day: the AA receiver tells the
// phone (its night-mode sensor), so Maps darkens with the car.
#define PJ_NIGHT "/run/dash-night"
/* dash-aa holds the radio for Android Auto's AP (station disconnected): WiFi cannot go first, the car rides Bluetooth */
#define PJ_AA_RADIO "/run/dash-aa-radio"
/* S06wattline: the head-unit certificate cannot be used this start (its key, or unreadable): no AA button */
#define PJ_AA_UNUSABLE "/run/dash-aa-unusable"
/* aa serve: a phone's session is up (its map shown or put away by AA's own Exit); the dash asks for the map back with
 * PJ_AA_FOCUS (its AA button while the map is put away), aa takes video focus again at its next ping tick */
#define PJ_AA_SESSION "/run/dash-aa-session"
#define PJ_AA_FOCUS "/run/dash-aa-focus"
/* spike-vdec: the phone's pictures shown in the last second, rewritten each second (the split's WiFi box shows it) */
#define PJ_FPS "/run/dash-pj-fps"
#define PJ_TOUCH_DEV "/dev/input/event0"
