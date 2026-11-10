// The Commander's Bluetooth session in the loop (CMDR=1 builds): port/linux/ble_bluez.c runs it on its own
// thread; this posts the WiFi state and presses to it and takes its replies (vehicle-data pushes -> car_from_bt).
#pragma once
#include "app.h"
#include "../core/cmdr_msg.h"

void cmdr_hook_start(const conf_t *cf);
void cmdr_hook_stop(void);
int cmdr_hook_fd(void);                    // in the poll set; -1 none
void cmdr_hook_step_aside(void);           // before each WiFi hello (main.dart beforeStream)
void cmdr_hook_sync(car_t *car, const link_t *l, bt_view_t *bt, int64_t now);   // each loop turn
void cmdr_hook_press(cmdr_action a, const car_t *car, link_t *l, int64_t now);  // a chip's tap (once)
