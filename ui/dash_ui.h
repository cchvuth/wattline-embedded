/* The dash, drawn with LVGL in the Dash arrangement (PLAN-merged.md 5.2) of the app's look. */
#pragma once
#include "lvgl.h"
#include "dash_state.h"

/* Builds the dash on [scr], laid out once for the screen's size (H 480 at the example canvas). */
void dash_ui_init(lv_obj_t *scr);
/* Shows [s]; only what changed is touched, so LVGL redraws only those areas. */
void dash_ui_set(const dash_state_t *s);
