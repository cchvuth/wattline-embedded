// BlueZ GATT transport for core/cmdr.c over bluetoothd's D-Bus API (org.bluez, through GDBus),
// and the thread that runs the Commander session so the app's poll loop never blocks.
//
// Transport (cmdr_io) mappings:
//   adapter_on  Adapter1.Powered (powers it if off: this unit is its only user)
//   scan        Adapter1.SetDiscoveryFilter{UUIDs: [CMDR_SERVICE_UUID], Transport: le}, StartDiscovery,
//               every 250 ms list the Device1 objects heard in this discovery (RSSI set) whose UUIDs
//               hold the service, for 5 s (pin: stop when that address shows; else the strongest),
//               StopDiscovery. Never filters on the name.
//   connect     Device1.Connect (15 s), wait ServicesResolved, read each 0x2901 GattDescriptor1 under
//               the service (bytes >= 0x20) and map "prov-session", "enhapi", "push-enhapi".
//               GattCharacteristic1.MTU is the negotiated MTU (BlueZ exchanges it, [GATT] ExchangeMTU);
//               below BLE_MTU_MIN the MTU exchange failed and pushes would be cut: no CMDR_HAS_PUSH.
//   rpc         WriteValue(data, {type: request}) (BlueZ does the long write), then ReadValue({}).
//   notify      StartNotify/StopNotify on push-enhapi; each PropertiesChanged Value is a push.
//   disconnect  Device1.Disconnect (also cancels BlueZ's pending connect).
//   random      getrandom(2).
// The controller keeps its own address; the /dev/ttyS5 module is never touched (PLAN 0.1 rule 6).
// Every byte written goes through cmdr_send(): this file has no write path of its own.
#pragma once
#include "../../core/cmdr.h"

enum { BLE_MTU_MIN = 64, BLE_MAX = 517 };  // 517: the largest ATT MTU, so no notification is longer

// ---- the session on its own thread (what the app uses) ----
// The thread owns the cmdr_t and a GLib main loop: cmdr_tick every 100 ms, the requests below in
// order, and BlueZ's signals (pushes -> cmdr_on_push, Connected false -> cmdr_link_lost, Powered ->
// cmdr_adapter). While CS_OFF it looks at the adapter every 5 s. Times are CLOCK_MONOTONIC ms.
enum {
  BLE_CONNECT,     // cmdr_connect
  BLE_KICK,        // cmdr_kick(a = ask)
  BLE_FORGET,      // cmdr_forget
  BLE_WIFI,        // cmdr_wifi(a = live, b = trying)
  BLE_STEP_ASIDE,  // cmdr_step_aside; replied: result = ms to wait before the WiFi hello
  BLE_PAUSE,       // cmdr_pause; replied: result = 1 if a link was up or on its way
  BLE_RESUME,      // cmdr_resume
  BLE_DATA_HOLD,   // cmdr_data_hold(a = hold)
  BLE_PRESS,       // cmdr_press(a = cmdr_action, b = parked, gear); replied: result = CMDR_OK / CMDR_E_*
  BLE_DATA,        // reply only: one vehicle-data push (cfg.on_data); n 0 = the link dropped
};
typedef struct {
  int op, id;  // id is the app's own, echoed in the reply
  int a, b;
  char gear;
  int result;
  size_t n;
  uint8_t data[BLE_MAX];
} ble_msg;
typedef struct ble ble;

// cfg->on_data is replaced: pushes come back as BLE_DATA replies on the app's thread.
ble *ble_start(const cmdr_cfg *cfg, const char *adapter_path);  // e.g. "/org/bluez/hci0"; NULL: no system bus
ble *ble_start_io(const cmdr_cfg *cfg, const cmdr_io *io);      // any transport (tests)
int ble_fd(ble *b);                       // POLLIN: replies waiting
int ble_post(ble *b, const ble_msg *m);   // queue a request; never blocks; 0 ok
int ble_take(ble *b, ble_msg *m);         // 1: one reply taken; call until 0 when ble_fd is readable
int64_t ble_state(ble *b, cmdr_t *pub);   // copies the public fields (state .. push_count); returns cmdr_until()
void ble_stop(ble *b);                    // closes the link (cmdr_pause) and joins the thread

// ---- the transport alone (tools/ble_probe, tests) ----
int ble_bluez_open(cmdr_io *io, const char *adapter_path);  // 0 ok
void ble_bluez_close(cmdr_io *io);
typedef struct { char addr[CMDR_ADDR_MAX], name[64]; int rssi, match; } ble_dev;  // match: advertises the service
// Discovery for ms (uuid NULL: no service filter); the devices heard, last values, strongest first.
// Stops early when pin is heard advertising the service. Returns the count, <0 failed.
int ble_scan(cmdr_io *io, const char *uuid, int ms, const char *pin, ble_dev *out, int cap);
int ble_mtu(const cmdr_io *io);  // of the last connect, 0 unknown

// Pure helpers (tested on the Mac).
int ble_chr_of(const uint8_t *text, size_t n);  // 0x2901 bytes -> CMDR_CHR_*, -1 none
int ble_has_for_mtu(int has, int mtu);          // drops CMDR_HAS_PUSH when 0 < mtu < BLE_MTU_MIN
