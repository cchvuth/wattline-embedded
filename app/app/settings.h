// The settings screen's values, kept on /data (R3's writable mount) when there is one, else in memory only (the Mac):
//   <root>/settings.conf        minor_figures, clock_24h, brightness_pct, follow_car, ssh, record, car, sparse,
//                               wifi_first, tz, panda_host, panda_port, ota_auto   (key = value)
//   <root>/tyres                the last tyre pressures, for the next start
//   <root>/wifi/networks.conf   R3's list: two lines a network, SSID then passphrase ("" open), in order of
//                               preference; folder 0700, file 0600. No file yet: the list is the car's
//                               adapter alone (SETTINGS_CAR_SSID / _PASS), editable and deletable like any other
// Both written whole and atomically (temp, fsync, rename). Passphrases are never logged nor handed back to the UI.
#pragma once
#include "app.h"
#include "../ui/dash_ui.h"

#define SETTINGS_CAR_SSID "S3XY_OBD"   // the Commander's WiFi and its factory passphrase: the first boot's list
#define SETTINGS_CAR_PASS "12345678"

extern const char *settings_root;                                            // "/data"; NULL: memory only
extern const char *settings_wifi_cmd, *settings_ssh_on_cmd, *settings_ssh_off_cmd;   // run on a change; NULL: none
extern const char *settings_aa_cmd;   // run with "start"/"stop": wireless Android Auto on demand (the Network button)
extern const char *settings_ssh_pid;   // Dropbear's pid file: a live one loads the SSH switch as on; NULL: not checked

void settings_load(dash_settings_t *st, const conf_t *cf);   // what was saved, else dash.conf's; passes come back ""
// Takes the screen's save: a network's "" pass keeps the one on file. Writes what changed, then applies it (WiFi
// list -> settings_wifi_cmd, SSH -> on/off cmd). Clears the typed passes in st. 0 ok, -1 a write failed (logged).
int settings_save(dash_settings_t *st);
void settings_refresh(dash_settings_t *st);   // as the screen opens: SSH on if Dropbear runs (the pairing turns it on)
void settings_aa(bool start);
void settings_aa_forget(void);          // dash-aa forget: the phones paired for Android Auto removed
void settings_ota(const char *what);   // run dash-update <what> (check, auto) in the background
void settings_reboot(void);            // restart the Dash (an armed update starts)   // the Network button: run settings_aa_cmd start|stop (wireless Android Auto on demand)
extern bool settings_car_saved;   // settings.conf names the car (loaded or saved): else the first boot asks for it
bool settings_apply_tz(const char *tz);   // TZ for localtime (the clocks); true if it changed
// The last tyre pressures (bar, NAN none): the car sends them only on the move, the picture shows the last ones meanwhile
// (main.dart _saveTyres). keep: written when changed, at most once a minute, and at stop (stop true).
void settings_tyres_load(double tp[4]);
void settings_tyres_keep(const double tp[4], int64_t now, bool stop);
