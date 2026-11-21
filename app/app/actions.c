// See actions.h. The guards are core's (cmdr_press_guard runs on the session's thread before anything is sent:
// closures in P only, mirror tilt in R only); this side only words their answers as the app does, and keeps one
// press out at a time: a tap while one is out is dropped, and nothing is ever resent.
#include "actions.h"
#include "pair.h"
#include "settings.h"
#include "../core/cmdr_msg.h"
#ifdef DASH_CMDR
#include "../core/cmdr.h"
void cmdr_hook_ask(void);     // cmdr_hook.c: post(BLE_KICK, 1, 0, 0): the owner's tap, may start the approval
void cmdr_hook_forget(void);  // cmdr_hook.c: post(BLE_FORGET, 0, 0, 0): unpin the Commander
const char *cmdr_hook_why(void);  // cmdr_hook.c: the session's state in words
int cmdr_hook_ready(void);        // cmdr_hook.c: linked, as last taken
#endif
#include <stdio.h>
#include <string.h>

enum { SAY_MS = 4000, ANSWER_MS = 30000 };  // the snackbar's 4 s; past this a press out is let go (a slow redial: ~21 s)

dash_settings_t app_settings;
bool app_settings_dirty, app_settings_refresh;

static int pending = -1;
static int64_t pending_at, say_until;
static char say[128];

void app_tell(const char *text, int64_t now) {  // main.dart _say: in place of the last
  snprintf(say, sizeof say, "%s", text);
  say_until = now + SAY_MS;
}


void app_on_action(int a, void *ctx) {
  (void)ctx;
  int64_t now = mono_ms();
  if (a < 0 || a >= ACT_COUNT) return;
  if (pending >= 0) {  // one at a time (car_panel.dart _busy); never queued, never resent
    char t[128];
    log_msg("press %s dropped: %s still out", cmdr_actions[a].label, cmdr_actions[pending].label);
    snprintf(t, sizeof t, "%s: waiting for %s's answer first", cmdr_actions[a].label, cmdr_actions[pending].label);
    app_tell(t, now);   // said, on the dash and the car panel's line: else the tap seems to do nothing
    return;
  }
#ifdef DASH_CMDR
  if (!cmdr_hook_ready()) {  // main.dart _press: look now (the tap is the owner's), and say what the link is doing
    char t[128];
    snprintf(t, sizeof t, "Commander Bluetooth: %s", cmdr_hook_why());
    log_msg("press %s: not linked", cmdr_actions[a].label);
    cmdr_hook_ask();
    app_tell(t, now);
    return;
  }
  pending = a, pending_at = now;
  log_msg("press %s", cmdr_actions[a].label);
  dash_press(a, now);  // the session guards it again and sends it once (cmdr_hook_press); `now` is its id
#else
  log_msg("press %s: no Commander session in this build", cmdr_actions[a].label);
  app_tell("Commander Bluetooth: off", now);
#endif
}

void app_press_result(int a, int result, int status, char gear, int64_t at, int64_t now) {
  if (a < 0 || a >= ACT_COUNT) return;
  if (a == pending && at == pending_at) pending = -1;   // its own answer, not a late one of an earlier press
  const char *label = cmdr_actions[a].label;
  char t[128] = "";
#ifdef DASH_CMDR
  int panel = a == ACT_UNLOCK || a == ACT_LOCK || a == ACT_CLIMATE || a == ACT_FLASH || a == ACT_HONK;
  switch (result) {
  case CMDR_OK:  // the Commander's 0 only means it took the press; the chips light from the car's own state
    if (panel) snprintf(t, sizeof t, a == ACT_UNLOCK ? "%s sent: unlocks with the driver inside" : "%s sent", label);
    break;
  case CMDR_E_NOT_READY: snprintf(t, sizeof t, "Commander Bluetooth: %s", cmdr_hook_why()); break;
  case CMDR_E_STALE: snprintf(t, sizeof t, "%s: not sent: Bluetooth was busy, tap again", label); break;
  case CMDR_E_PARK:
    snprintf(t, sizeof t, gear == 'R' || gear == 'N' || gear == 'D' ? "%s opens in P only" : "%s: gear unknown — no live data", label);
    break;
  case CMDR_E_GEAR: snprintf(t, sizeof t, "Mirror tilt acts in R only"); break;  // S3XY: auto-tilt override, Reverse
  case CMDR_E_NO_ANSWER: snprintf(t, sizeof t, "%s: no answer: it may have gone through", label); break;
  case CMDR_E_STATUS: {   // main.dart: '<label>: <error>', the Commander's refusal named (commander.dart _statuses)
    char why[40];
    if (status >= 0 && status < 9) snprintf(why, sizeof why, "%s", cmdr_statuses[status]);
    else snprintf(why, sizeof why, "status %d", status);
    snprintf(t, sizeof t, a == ACT_CLIMATE ? "%s: refused (%s): needs the Commander linked to the car (ScanMyTesla off)"
                                           : "%s: refused (%s)", label, why);
    break;
  }
  case CMDR_E_DENIED: snprintf(t, sizeof t, "%s: not allowed", label); break;
  default: snprintf(t, sizeof t, "%s: error %d", label, result);
  }
#else
  (void)gear, (void)status;
  if (result) snprintf(t, sizeof t, "%s: error %d", label, result);
#endif
  log_msg("press %s: %d", label, result);
  if (t[0]) app_tell(t, now);
}

void app_actions_fill(dash_state_t *s, int64_t now) {
  if (pending >= 0 && now - pending_at > ANSWER_MS) {  // no answer at all: let it go, never resend
    char t[128];
    snprintf(t, sizeof t, "%s: no answer: it may have gone through", cmdr_actions[pending].label);
    app_tell(t, now);
    pending = -1;
  }
  s->press_pending = (int8_t)pending;
  s->backlight_pct = (int8_t)dash_bl_pct;
  const pair_status_t *p = pair_status(now);   // the SSH pairing window, while its view is up
  s->pair_open = p->open;
  snprintf(s->pair_url, sizeof s->pair_url, "%s", p->url);
  snprintf(s->pair_code, sizeof s->pair_code, "%s", p->code);
  s->pair_secs = (int16_t)p->secs_left;
  snprintf(s->pair_result, sizeof s->pair_result, "%s", p->result);
  snprintf(s->toast, sizeof s->toast, "%s", now < say_until ? say : "");
}

void app_on_settings(int what, const dash_settings_t *st, void *ctx) {
  (void)ctx;
  switch (what) {
  case DASH_SETTINGS_SAVE:
    app_settings = *st;
    app_settings_dirty = true;
    log_msg("settings: minor %d, 24 h %d, brightness %d %%, %d networks, ssh %d", st->minor_figures, !st->hour12,
            st->brightness_pct, st->net_count, st->dev_ssh);  // SSIDs and passphrases stay out of the log
    break;
  case DASH_NET_SCAN:
    wifi_scan();
    break;
  case DASH_AA_START:  // the on-screen button: advertise + serve AA only while the user wants it (not always-on)
    log_msg("settings: start Android Auto");
    settings_aa(true);
    break;
  case DASH_AA_STOP:
    log_msg("settings: stop Android Auto");
    settings_aa(false);
    break;
  case DASH_AA_FORGET:     // Advanced: the paired phones go (each gets the AP's passphrase whenever AA is on)
    log_msg("settings: forget the phones paired for Android Auto");
    settings_aa_forget();
    break;
  case DASH_OTA_CHECK:     // Advanced, software update: look for a newer release (status in /run/dash-update.status)
    settings_ota("check");
    break;
  case DASH_OTA_INSTALL:   // download (if not already), verify, arm for the next start: no reboot
    settings_ota("install");   // by hand: also retries a version that failed to start before
    break;
  case DASH_OTA_RESTART:   // the update is armed: start it now
    log_msg("settings: restart for the update");
    settings_reboot();
    break;
  case DASH_NOR_COPY:      // Advanced, built-in flash: the card install written there (status in /run/dash-nor.status)
    log_msg("settings: copy the card install to the built-in flash");
    settings_nor("copy");
    break;
  case DASH_SETTINGS_OPEN:  // looked at again by the loop (settings_refresh), not from inside the UI's own call
    app_settings_refresh = true;
    break;
  case DASH_CMDR_APPROVE:  // a tap: the Dash never prompts on its own
    log_msg("settings: approve the Commander");
#ifdef DASH_CMDR
    cmdr_hook_ask();
#endif
    break;
  case DASH_PAIR_START:  // only from the on-screen tap: someone is at the Dash
    pair_open("wlan0", mono_ms());
    break;
  case DASH_PAIR_CANCEL:
    pair_close("Cancelled");
    break;
  case DASH_CMDR_FORGET:
    log_msg("settings: forget the Commander");
#ifdef DASH_CMDR
    cmdr_hook_forget();
#endif
    break;
  }
}

#if defined ACTIONS_SELFTEST && defined DASH_CMDR
// The one check, stubs and all (with app/pair.c): cc -std=gnu11 -DACTIONS_SELFTEST -DDASH_CMDR=1 -DLV_CONF_INCLUDE_SIMPLE -Iport/sim
//   -Ithird_party/lvgl-9.6.0 -Ithird_party/lvgl-9.6.0/src -Ibuild -o build/actions_check app/actions.c app/pair.c core/cmdr_msg.c
//   core/pb.c && build/actions_check
#include <assert.h>
static int64_t T; static int presses; int dash_bl_pct = -1;
int64_t mono_ms(void) { return T; }
void log_msg(const char *f, ...) { (void)f; }
void dash_press(int a, int64_t at) { (void)a, (void)at; presses++; }
static int ready = 1, asks;
void cmdr_hook_ask(void) { asks++; } void cmdr_hook_forget(void) {}
void settings_aa(bool start) { (void)start; }
void settings_aa_forget(void) {}
void settings_ota(const char *what) { (void)what; }
void settings_nor(const char *what) { (void)what; }
void settings_reboot(void) {}
const char *cmdr_hook_why(void) { return "no Commander advertising"; }
int cmdr_hook_ready(void) { return ready; }
void dash_state_blank(dash_state_t *s) { memset(s, 0, sizeof *s); }
int main(void) {
  dash_state_t s;
  app_on_action(ACT_FRUNK, 0); app_on_action(ACT_TRUNK, 0);   // the second is dropped
  assert(presses == 1);
  app_actions_fill(&s, T); assert(s.press_pending == ACT_FRUNK && !strcmp(s.toast, "Trunk: waiting for Frunk's answer first"));
  app_press_result(ACT_FRUNK, CMDR_E_PARK, 0, 'D', pending_at, T);
  app_actions_fill(&s, T); assert(s.press_pending == -1 && !strcmp(s.toast, "Frunk opens in P only"));
  app_on_action(ACT_MIRROR_TILT, 0); app_press_result(ACT_MIRROR_TILT, CMDR_E_GEAR, 0, 'P', pending_at, T);
  app_actions_fill(&s, T); assert(!strcmp(s.toast, "Mirror tilt acts in R only"));
  app_on_action(ACT_UNLOCK, 0); app_press_result(ACT_UNLOCK, CMDR_OK, 0, 'P', pending_at, T);
  app_actions_fill(&s, T); assert(!strcmp(s.toast, "Unlock sent: unlocks with the driver inside"));
  ready = 0; app_on_action(ACT_FLASH, 0); ready = 1;   // not linked: looked for at once, its state said, nothing sent
  app_actions_fill(&s, T); assert(!strcmp(s.toast, "Commander Bluetooth: no Commander advertising") && asks == 1);
  assert(s.press_pending == -1);
  app_on_action(ACT_FLASH, 0); app_press_result(ACT_FLASH, CMDR_E_STATUS, 5, 'P', pending_at, T);       // refused: the reason named
  app_actions_fill(&s, T); assert(!strcmp(s.toast, "Flash: refused (internal error)"));
  app_on_action(ACT_TRUNK, 0); app_press_result(ACT_TRUNK, CMDR_OK, 0, 'P', pending_at, T);   // a chip's success: no line
  T += 4001; app_actions_fill(&s, T); assert(!s.toast[0]);
  app_on_action(ACT_HONK, 0); T += 30001; app_actions_fill(&s, T);          // no answer: let go, not resent
  assert(s.press_pending == -1 && strstr(s.toast, "may have gone through") && presses == 6);
  app_on_action(ACT_HONK, 0); int64_t first = pending_at; T += 30001; app_actions_fill(&s, T);   // let go...
  app_on_action(ACT_HONK, 0);                                                     // ...the same chip again
  app_press_result(ACT_HONK, CMDR_E_STALE, 0, 'P', first, T);                     // the first's late answer:
  app_actions_fill(&s, T); assert(s.press_pending == ACT_HONK);                   // not taken for the new one
  puts("actions ok"); return 0;
}
#endif
