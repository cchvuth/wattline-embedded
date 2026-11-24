// See cmdr_hook.h. Nothing here writes to the Commander: requests go to ble_post, whose thread sends only
// through cmdr_send's allow-list.
#include "cmdr_hook.h"
#include "actions.h"
#include "record.h"
#include "../core/pb.h"
#include "../port/linux/ble_bluez.h"
#include <string.h>

static ble *B;
static int64_t settle_until;   // a step-aside's settle ends (CLOCK_MONOTONIC ms); 0 none
static cmdr_t pub;   // the session's public fields, copied each turn (bt_view_t.dest points in here)
static int last_live = -1, last_trying = -1;
static int64_t wifi_sent;

static void post_at(int op, int a, int b, char gear, int64_t at) {
  ble_msg m = {.op = op, .a = a, .b = b, .gear = gear, .at = at};
  if (B && ble_post(B, &m)) log_msg("bluetooth: request %d not queued", op);
}
static void post(int op, int a, int b, char gear) { post_at(op, a, b, gear, 0); }

bool cmdr_hook_start(const conf_t *cf, int wifi_first) {
  cmdr_cfg cfg = cmdr_cfg_default;
  cfg.wifi_first = wifi_first;
  cfg.id_path = cf->cmdr_id;
  cfg.pin_path = cf->cmdr_pin;
  cfg.settle_ms = cf->bt_settle_ms, cfg.buttons_ms = cf->bt_buttons_ms;   // dash.conf's bt_* timings
  cfg.rejoin_ms = cf->bt_data_ms, cfg.back_ms = cf->bt_back_ms;
  B = ble_start(&cfg, "/org/bluez/hci0");
  if (!B) {   // (the caller tries again: said once)
    static bool said;
    if (!said) log_msg("bluetooth: no system bus yet: no Commander session until there is one"), said = true;
    return false;
  }
  post(BLE_WIFI, 0, 1, 0);   // until the next sync says how the link is: the session must not open with "not trying"
  post(BLE_CONNECT, 0, 0, 0);
  log_msg("bluetooth: Commander session started");
  return true;
}

void cmdr_hook_stop(void) {
  if (B) ble_stop(B);
  B = NULL;
}

int cmdr_hook_fd(void) { return B ? ble_fd(B) : -1; }
static int64_t aside_asked_at = -1;   // a step-aside posted, its reply not yet taken
void cmdr_hook_step_aside(void) { post(BLE_STEP_ASIDE, 0, 0, 0), aside_asked_at = B ? mono_ms() : -1; }
bool cmdr_hook_aside_pending(int64_t now) { return aside_asked_at >= 0 && now - aside_asked_at < 20000; }
int64_t cmdr_hook_settle_until(void) { return settle_until; }
void cmdr_hook_wifi_first(int on) {
  static int sent = -1;
  if (on != sent) post(BLE_WIFI_FIRST, on, 0, 0), sent = on;
}
void cmdr_hook_ask(void) { post(BLE_KICK, 1, 0, 0); }      // the owner's tap: may start the approval
void cmdr_hook_forget(void) { post(BLE_FORGET, 0, 0, 0); }  // unpin the Commander
const char *cmdr_hook_why(void) { return pub.why ? pub.why : B ? "starting" : "off"; }
int cmdr_hook_ready(void) { return pub.state == CS_READY; }

// A PushVehicleDataHolder body as Panda.fromBt's map: field -> value (varints are the ints it reads).
static void fields(bt_map_t *m, const uint8_t *d, size_t n) {
  memset(m, 0, sizeof *m);
  pb_in in = {d, n, 0};
  pb_val v;
  while (pb_next(&in, &v) == 1)
    if (v.field < BT_FIELDS) m->has[v.field] = 1, m->is_int[v.field] = v.wt == 0, m->v[v.field] = (int64_t)v.v;
}

static dash_bt_t link_of(const cmdr_t *c) {
  switch (c->state) {
  case CS_OFF: return BT_OFF;
  case CS_PAUSED: return BT_PAUSED;
  case CS_AWAY: return BT_AWAY;
  case CS_RETRYING: return BT_RETRYING;
  case CS_APPROVING: return BT_APPROVING;
  case CS_REFUSED: return BT_REFUSED;
  case CS_SCANNING: return BT_SCANNING;
  case CS_LINKING: return BT_LINKING;
  case CS_READY: return c->level == CMDR_FULL ? BT_FULL : c->level == CMDR_LEAN ? BT_LEAN : BT_BUTTONS;
  default: return BT_IDLE;
  }
}

void cmdr_hook_sync(car_t *car, const link_t *l, bt_view_t *bt, int64_t now) {
  if (!B) return;
  // trying: the link's hello, or wpa_supplicant joining (associated / getting an address, which the link cannot see:
  // its connects fail meanwhile), for 20 s at most (a network that never gives an address must not hold Bluetooth)
  static int64_t join_from = -1;
  const char *jw;
  if (wifi_join(now, &jw) != WIFI_JOINING) join_from = -1;
  else if (join_from < 0) join_from = now;
  int live = car->wifi_live, trying = link_trying(l) || (join_from >= 0 && now - join_from < 20000);
  if (live != last_live || trying != last_trying || now - wifi_sent >= 1000) {   // main.dart _linksChanged
    post(BLE_WIFI, live, trying, 0);
    last_live = live, last_trying = trying, wifi_sent = now;
  }
  ble_msg m;
  while (ble_take(B, &m) == 1) {
    if (m.op == BLE_DATA && m.n == 0) car_from_bt(car, NULL, now), record_cmdr(NULL, now);
    else if (m.op == BLE_DATA) {
      static bt_map_t push;
      fields(&push, m.data, m.n);
      car_from_bt(car, &push, now);
      record_cmdr(&push, now);
    } else if (m.op == BLE_PRESS) {
      log_msg("bluetooth: press %d: %d", m.a, m.result);
      app_press_result(m.a, m.result, m.b, m.gear, m.at, now);
    }
    else if (m.op == BLE_STEP_ASIDE && (aside_asked_at = -1, m.result > 0)) {
      settle_until = now + m.result;   // the WiFi hello waits this out (dash.c p_settle)
      log_msg("bluetooth: stepped aside, settle %d ms", m.result);
    }
  }
  bt->until = ble_state(B, &pub);
  static int said_state = -1;   // each move in the log: why a link took many tries (a scan that missed, a connect cut)
  static const char *said_why;
  if ((int)pub.state != said_state || pub.why != said_why)
    log_msg("bluetooth: %s", pub.why ? pub.why : "?"), said_state = (int)pub.state, said_why = pub.why;
  bt->link = link_of(&pub);
  bt->why = pub.why ? pub.why : "";
  bt->push_info = pub.push_info ? pub.push_info : "";
  bt->car_link = pub.car_link;
  bt->push_count = pub.push_count;
  bt->dest = pub.has_dest && pub.dest[0] ? pub.dest : NULL;
  bt->nav_minutes = pub.has_minutes ? pub.minutes : NAN;
  bt->nav_miles = pub.has_miles ? pub.miles : NAN;
  bt->nav_arrive_pct = pub.has_arrive ? pub.arrive_pct : NAN;
  bt->recirc_mode = pub.has_recirc ? (int)pub.recirc : -1;
}

void cmdr_hook_press(cmdr_action a, const car_t *car, link_t *l, int64_t now) {
  link_pressed(l, now);   // the chip's groups polled a while (Panda.pressed)
  post_at(BLE_PRESS, a, car_parked(car, now), car->gear, now);   // the guards run again on the session's thread
}
