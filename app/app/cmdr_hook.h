// The Commander's Bluetooth session in the loop (CMDR=1 builds): port/linux/ble_bluez.c runs it on its own
// thread; this posts the WiFi state and presses to it and takes its replies (vehicle-data pushes -> car_from_bt).
#pragma once
#include "app.h"
#include "../core/cmdr_msg.h"

bool cmdr_hook_start(const conf_t *cf, int wifi_first);   // false: no system bus (yet): to be tried again
void cmdr_hook_wifi_first(int on);
int64_t cmdr_hook_settle_until(void);
bool cmdr_hook_aside_pending(int64_t now);  // a step-aside asked, not yet answered (capped at 20 s)       // when the last step-aside's settle ends (0: none)          // the settings' switch: Bluetooth waits for the WiFi stream
void cmdr_hook_stop(void);
int cmdr_hook_fd(void);                    // in the poll set; -1 none
void cmdr_hook_step_aside(void);           // before each WiFi hello (main.dart beforeStream)
void cmdr_hook_ask(void);                  // the settings' approve tap (BLE_KICK, ask)
void cmdr_hook_forget(void);               // the settings' forget tap (BLE_FORGET)
const char *cmdr_hook_why(void);           // the session's state in words, as last taken (a press's "Commander Bluetooth: ...")
int cmdr_hook_ready(void);                 // linked (CS_READY), as last taken: a press goes to the session
void cmdr_hook_sync(car_t *car, const link_t *l, bt_view_t *bt, int64_t now);   // each loop turn
void cmdr_hook_press(cmdr_action a, const car_t *car, link_t *l, int64_t now);  // a chip's tap (once)
