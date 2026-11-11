// The settings screen's values, kept on /data (R3's writable mount) when there is one, else in memory only (the Mac):
//   <root>/settings.conf        minor_figures, clock_24h, brightness_pct, follow_car, ssh   (key = value)
//   <root>/wifi/networks.conf   R3's list: two lines a network, SSID then passphrase ("" open), in order of
//                               preference; folder 0700, file 0600; S3XY_OBD is built in and never written there
// Both written whole and atomically (temp, fsync, rename). Passphrases are never logged nor handed back to the UI.
#pragma once
#include "app.h"
#include "../ui/dash_ui.h"

extern const char *settings_root;                                            // "/data"; NULL: memory only
extern const char *settings_wifi_cmd, *settings_ssh_on_cmd, *settings_ssh_off_cmd;   // run on a change; NULL: none

void settings_load(dash_settings_t *st, const conf_t *cf);   // what was saved, else dash.conf's; passes come back ""
// Takes the screen's save: a network's "" pass keeps the one on file. Writes what changed, then applies it (WiFi
// list -> settings_wifi_cmd, SSH -> on/off cmd). Clears the typed passes in st. 0 ok, -1 a write failed (logged).
int settings_save(dash_settings_t *st);
