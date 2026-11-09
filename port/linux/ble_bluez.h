// BlueZ GATT transport for core/cmdr.c (sketch: the interface only; ble_bluez.c
// is written and tested in the car). Fills a cmdr_io with these mappings to
// bluetoothd's D-Bus API (org.bluez, via GDBus):
//   adapter_on  Adapter1.Powered
//   scan        Adapter1.SetDiscoveryFilter{UUIDs: [CMDR_SERVICE_UUID], Transport: le},
//               StartDiscovery, watch InterfacesAdded/PropertiesChanged for 5 s
//               (pin: stop when that address shows; else the strongest RSSI),
//               StopDiscovery. Never filter on the advertised name.
//   connect     Device1.Connect (15 s); wait ServicesResolved; for each
//               GattCharacteristic1 under CMDR_SERVICE_UUID read its 0x2901
//               GattDescriptor1 (bytes >= 0x20) and map "prov-session",
//               "enhapi", "push-enhapi" to CMDR_CHR_*. Return the CMDR_HAS_* mask.
//               Large MTU: BlueZ negotiates it; check GattCharacteristic1.MTU >= the
//               longest push, as a push is one notification, never reassembled.
//   rpc         GattCharacteristic1.WriteValue(data, {type: "request"}) (BlueZ does
//               the long write) then ReadValue({}) on the same object; one at a time.
//   notify      StartNotify on push-enhapi; each PropertiesChanged "Value" goes to
//               cmdr_on_push() on the cmdr thread.
//   disconnect  Device1.Disconnect (also cancels a pending Connect).
//   random      getrandom(2).
// The cmdr_io calls block, so cmdr runs on its own thread (or the GLib loop with
// sync calls) and the UI reads a copy of cmdr_t's public fields under a lock.
// Link loss: Device1.Connected -> false calls cmdr_link_lost(); Adapter1.Powered
// changes call cmdr_adapter(). The controller keeps its own address; the
// /dev/ttyS5 module is never touched (PLAN 0.1 rule 6).
#pragma once
#include "../../core/cmdr.h"

int ble_bluez_open(cmdr_io *io, const char *adapter_path); // e.g. "/org/bluez/hci0"; 0 ok
void ble_bluez_close(cmdr_io *io);
