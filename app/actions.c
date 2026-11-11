// See actions.h. The guards are core's (cmdr_press_guard runs on the session's thread before anything is sent:
// closures in P only, mirror tilt in R only); this side only words their answers as the app does, and keeps one
// press out at a time: a tap while one is out is dropped, and nothing is ever resent.
#include "actions.h"
#include "pair.h"
#include "../core/cmdr_msg.h"
#ifdef DASH_CMDR
#include "../core/cmdr.h"
void cmdr_hook_ask(void);     // cmdr_hook.c: post(BLE_KICK, 1, 0, 0): the owner's tap, may start the approval
void cmdr_hook_forget(void);  // cmdr_hook.c: post(BLE_FORGET, 0, 0, 0): unpin the Commander
#endif
#include <stdio.h>
#include <string.h>

enum { SAY_MS = 4000, ANSWER_MS = 20000 };  // the snackbar's 4 s; past this a press out is let go

dash_settings_t app_settings;
bool app_settings_dirty;

static int pending = -1;
static int64_t pending_at, say_until;
static char say[128];

static void tell(const char *text, int64_t now) {  // main.dart _say: in place of the last
  snprintf(say, sizeof say, "%s", text);
  say_until = now + SAY_MS;
}


void app_on_action(int a, void *ctx) {
  (void)ctx;
  int64_t now = mono_ms();
  if (a < 0 || a >= ACT_COUNT) return;
  if (pending >= 0) {  // one at a time (car_panel.dart _busy); never queued, never resent
    log_msg("press %s dropped: %s still out", cmdr_actions[a].label, cmdr_actions[pending].label);
    return;
  }
#ifdef DASH_CMDR
  pending = a, pending_at = now;
  log_msg("press %s", cmdr_actions[a].label);
  dash_press(a);  // the session guards it again and sends it once (cmdr_hook_press)
#else
  log_msg("press %s: no Commander session in this build", cmdr_actions[a].label);
  tell("Commander Bluetooth: off", now);
#endif
}

void app_press_result(int a, int result, char gear, int64_t now) {
  if (a < 0 || a >= ACT_COUNT) return;
  if (a == pending) pending = -1;
  const char *label = cmdr_actions[a].label;
  char t[128] = "";
#ifdef DASH_CMDR
  int panel = a == ACT_UNLOCK || a == ACT_LOCK || a == ACT_CLIMATE || a == ACT_FLASH || a == ACT_HONK;
  switch (result) {
  case CMDR_OK:  // the Commander's 0 only means it took the press; the chips light from the car's own state
    if (panel) snprintf(t, sizeof t, a == ACT_UNLOCK ? "%s sent: unlocks with the driver inside" : "%s sent", label);
    break;
  case CMDR_E_NOT_READY: snprintf(t, sizeof t, "Commander Bluetooth: not linked"); break;
  case CMDR_E_PARK:
    snprintf(t, sizeof t, gear == 'R' || gear == 'N' || gear == 'D' ? "%s opens in P only" : "%s: gear unknown — no live data", label);
    break;
  case CMDR_E_GEAR: snprintf(t, sizeof t, "Mirror tilt acts in R only"); break;  // S3XY: auto-tilt override, Reverse
  case CMDR_E_NO_ANSWER: snprintf(t, sizeof t, "%s: no answer: it may have gone through", label); break;
  default:
    snprintf(t, sizeof t, a == ACT_CLIMATE ? "%s failed (needs the Commander linked to the car: ScanMyTesla off)" : "%s failed", label);
  }
#else
  (void)gear;
  if (result) snprintf(t, sizeof t, "%s failed", label);
#endif
  log_msg("press %s: %d", label, result);
  if (t[0]) tell(t, now);
}

void app_actions_fill(dash_state_t *s, int64_t now) {
  if (pending >= 0 && now - pending_at > ANSWER_MS) {  // no answer at all: let it go, never resend
    char t[128];
    snprintf(t, sizeof t, "%s: no answer: it may have gone through", cmdr_actions[pending].label);
    tell(t, now);
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
  case DASH_CMDR_APPROVE:  // the owner's tap: the Dash never prompts on its own (PLAN 5.4)
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
//   -Ithird_party/lvgl-9.6.0 -Ithird_party/lvgl-9.6.0/src -Ibuild -o build/actions_check app/actions.c core/cmdr_msg.c
//   core/pb.c && build/actions_check
#include <assert.h>
static int64_t T; static int presses;
int64_t mono_ms(void) { return T; }
void log_msg(const char *f, ...) { (void)f; }
void dash_press(int a) { (void)a; presses++; }
void cmdr_hook_ask(void) {} void cmdr_hook_forget(void) {}
void dash_state_blank(dash_state_t *s) { memset(s, 0, sizeof *s); }
int main(void) {
  dash_state_t s;
  app_on_action(ACT_FRUNK, 0); app_on_action(ACT_TRUNK, 0);   // the second is dropped
  assert(presses == 1);
  app_actions_fill(&s, T); assert(s.press_pending == ACT_FRUNK && !s.toast[0]);
  app_press_result(ACT_FRUNK, CMDR_E_PARK, 'D', T);
  app_actions_fill(&s, T); assert(s.press_pending == -1 && !strcmp(s.toast, "Frunk opens in P only"));
  app_on_action(ACT_MIRROR_TILT, 0); app_press_result(ACT_MIRROR_TILT, CMDR_E_GEAR, 'P', T);
  app_actions_fill(&s, T); assert(!strcmp(s.toast, "Mirror tilt acts in R only"));
  app_on_action(ACT_UNLOCK, 0); app_press_result(ACT_UNLOCK, CMDR_OK, 'P', T);
  app_actions_fill(&s, T); assert(!strcmp(s.toast, "Unlock sent: unlocks with the driver inside"));
  app_on_action(ACT_TRUNK, 0); app_press_result(ACT_TRUNK, CMDR_OK, 'P', T);   // a chip's success: no line
  T += 4001; app_actions_fill(&s, T); assert(!s.toast[0]);
  app_on_action(ACT_HONK, 0); T += 20001; app_actions_fill(&s, T);          // no answer: let go, not resent
  assert(s.press_pending == -1 && strstr(s.toast, "may have gone through") && presses == 5);
  puts("actions ok"); return 0;
}
#endif
