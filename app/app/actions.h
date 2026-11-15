// The dash's touch, on the app's side: a chip's or the car panel's tap -> one Commander press, and what came of it
// back into the UI (main.dart _press, _say; car_panel.dart), and the settings screen's requests.
// Hookup (the loop's owner): main.c   dash_ui_set_action_cb(app_on_action, NULL);
//                                     dash_ui_set_settings_cb(app_on_settings, NULL);
//                            dash.c   app_actions_fill(&s, now);              right after bridge_fill
//                            cmdr_hook.c  BLE_PRESS reply: app_press_result(m.a, m.result, m.b, m.gear, now);
//                            app.mk   APP_SRC += app/actions.c app/pair.c   (and the pair_fd / pair_handle lines, pair.h)
#pragma once
#include "app.h"
#include "../ui/dash_ui.h"

void app_on_action(int action, void *ctx);                            // a tap: cmdr_action
void app_on_settings(int what, const dash_settings_t *s, void *ctx);  // DASH_SETTINGS_SAVE, _CMDR_APPROVE/_FORGET, _PAIR_START/_CANCEL
void app_press_result(int action, int result, int status, char gear, int64_t now); // the session's answer (CMDR_OK /
                                                                                   // CMDR_E_*), the Commander's status
void app_actions_fill(dash_state_t *s, int64_t now);                   // the press out and the snackbar's line
void app_tell(const char *text, int64_t now);                         // a line on the snackbar

// The settings as last saved from the screen, for the writer of /data (dirty: not written yet). A network's pass is
// what was just typed ("" keep the one on file); the writer clears it once written.
extern dash_settings_t app_settings;
extern bool app_settings_dirty, app_settings_refresh;
