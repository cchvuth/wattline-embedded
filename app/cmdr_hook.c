// See cmdr_hook.h. Nothing here writes to the Commander: requests go to ble_post, whose thread sends only
// through cmdr_send's allow-list.
#include "cmdr_hook.h"
#include "actions.h"
#include "../core/pb.h"
#include "../port/linux/ble_bluez.h"
#include <string.h>

static ble *B;
static cmdr_t pub;   // the session's public fields, copied each turn (bt_view_t.dest points in here)
static int last_live = -1, last_trying = -1;
static int64_t wifi_sent;

static void post(int op, int a, int b, char gear) {
  ble_msg m = {.op = op, .a = a, .b = b, .gear = gear};
  if (B && ble_post(B, &m)) log_msg("bluetooth: request %d not queued", op);
}

void cmdr_hook_start(const conf_t *cf) {
  cmdr_cfg cfg = cmdr_cfg_default;
  cfg.id_path = cf->cmdr_id;
  cfg.pin_path = cf->cmdr_pin;
  B = ble_start(&cfg, "/org/bluez/hci0");
  if (!B) { log_msg("bluetooth: no system bus, no Commander session"); return; }
  post(BLE_CONNECT, 0, 0, 0);
}

void cmdr_hook_stop(void) {
  if (B) ble_stop(B);
  B = NULL;
}

int cmdr_hook_fd(void) { return B ? ble_fd(B) : -1; }
void cmdr_hook_step_aside(void) { post(BLE_STEP_ASIDE, 0, 0, 0); }
void cmdr_hook_ask(void) { post(BLE_KICK, 1, 0, 0); }      // the owner's tap: may start the approval
void cmdr_hook_forget(void) { post(BLE_FORGET, 0, 0, 0); }  // unpin the Commander

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
  case CS_READY: return c->level == CMDR_FULL ? BT_FULL : c->level == CMDR_LEAN ? BT_LEAN : BT_BUTTONS;
  default: return BT_IDLE;
  }
}

void cmdr_hook_sync(car_t *car, const link_t *l, bt_view_t *bt, int64_t now) {
  if (!B) return;
  int live = car->wifi_live, trying = link_trying(l);
  if (live != last_live || trying != last_trying || now - wifi_sent >= 1000) {   // main.dart _linksChanged
    post(BLE_WIFI, live, trying, 0);
    last_live = live, last_trying = trying, wifi_sent = now;
  }
  ble_msg m;
  while (ble_take(B, &m) == 1) {
    if (m.op == BLE_DATA && m.n == 0) car_from_bt(car, NULL, now);
    else if (m.op == BLE_DATA) {
      static bt_map_t push;
      fields(&push, m.data, m.n);
      car_from_bt(car, &push, now);
    } else if (m.op == BLE_PRESS) {
      log_msg("bluetooth: press %d: %d", m.a, m.result);
      app_press_result(m.a, m.result, m.gear, now);
    }
    else if (m.op == BLE_STEP_ASIDE && m.result > 0) log_msg("bluetooth: stepped aside, settle %d ms", m.result);
  }
  ble_state(B, &pub);
  bt->link = link_of(&pub);
  bt->dest = pub.has_dest && pub.dest[0] ? pub.dest : NULL;
  bt->nav_minutes = pub.has_minutes ? pub.minutes : NAN;
  bt->nav_miles = pub.has_miles ? pub.miles : NAN;
  bt->nav_arrive_pct = pub.has_arrive ? pub.arrive_pct : NAN;
  bt->recirc_mode = pub.has_recirc ? (int)pub.recirc : -1;
}

void cmdr_hook_press(cmdr_action a, const car_t *car, link_t *l, int64_t now) {
  link_pressed(l, now);   // the chip's groups polled a while (Panda.pressed)
  post(BLE_PRESS, a, car_parked(car, now), car->gear);   // the guards run again on the session's thread
}
