// What of port/linux/ble_bluez.c runs without bluetoothd: the 0x2901 text mapping, the MTU rule,
// and the session thread's requests, replies and state copy over a fake transport.
#include "../port/linux/ble_bluez.h"
#include <assert.h>
#include <glib.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static int checks;
#define CHECK(x) (assert(x), checks++)

static gint adapter = 1, scans, disconnects;
static int f_adapter(void *x) { (void)x; return g_atomic_int_get(&adapter); }
static int f_scan(void *x, const char *pin, char addr[CMDR_ADDR_MAX]) {
  (void)x, (void)pin, (void)addr;
  g_atomic_int_inc(&scans);
  g_usleep(300 * 1000); // a blocking scan: the app's side must not wait for it
  return -1;            // nobody advertising
}
static int f_connect(void *x, const char *a) { (void)x, (void)a; return -1; }
static int f_rpc(void *x, int chr, const uint8_t *w, size_t wn, uint8_t *r, size_t cap, size_t *rn) {
  (void)x, (void)chr, (void)w, (void)wn, (void)r, (void)cap, (void)rn;
  return -1;
}
static int f_notify(void *x, int on) { (void)x, (void)on; return -1; }
static void f_disconnect(void *x) { (void)x; g_atomic_int_inc(&disconnects); }
static int f_random(void *x, uint8_t *b, size_t n) { (void)x; memset(b, 1, n); return 0; }
static const cmdr_io fake = {0, f_adapter, f_scan, f_connect, f_rpc, f_notify, f_disconnect, f_random};

// The next reply that is not a BLE_DATA, waiting on ble_fd like the app's poll loop; counts DATA seen.
static int datas;
static int next(ble *b, ble_msg *m, int ms) {
  for (int64_t end = g_get_monotonic_time() / 1000 + ms; g_get_monotonic_time() / 1000 < end;) {
    struct pollfd p = {ble_fd(b), POLLIN, 0};
    poll(&p, 1, 50);
    while (ble_take(b, m)) {
      if (m->op != BLE_DATA) return 1;
      datas += m->n == 0;
    }
  }
  return 0;
}

static int wait_state(ble *b, cmdr_state s, int ms) {
  cmdr_t pub;
  for (int i = 0; i < ms / 20; i++, g_usleep(20 * 1000))
    if (ble_state(b, &pub), pub.state == s) return 1;
  return 0;
}

int main(void) {
  // ---- 0x2901 text -> characteristic ----
  CHECK(ble_chr_of((const uint8_t *)"prov-session", 12) == CMDR_CHR_SESSION);
  CHECK(ble_chr_of((const uint8_t *)"enhapi", 6) == CMDR_CHR_API);
  CHECK(ble_chr_of((const uint8_t *)"push-enhapi", 11) == CMDR_CHR_PUSH);
  CHECK(ble_chr_of((const uint8_t *)"enhapi\0", 7) == CMDR_CHR_API);           // trailing NUL dropped
  CHECK(ble_chr_of((const uint8_t *)"\x01push-enhapi\n", 13) == CMDR_CHR_PUSH);  // control bytes dropped
  CHECK(ble_chr_of((const uint8_t *)"push-enhap", 10) == -1);
  CHECK(ble_chr_of((const uint8_t *)"enhapi2", 7) == -1);
  CHECK(ble_chr_of((const uint8_t *)"ENHAPI", 6) == -1);  // exact, as the phone app's map
  CHECK(ble_chr_of((const uint8_t *)"", 0) == -1);
  uint8_t big[300];
  memset(big, 'a', sizeof big);
  CHECK(ble_chr_of(big, sizeof big) == -1);  // longer than any name: no overflow

  // ---- MTU ----
  int all = CMDR_HAS_SESSION | CMDR_HAS_API | CMDR_HAS_PUSH;
  CHECK(ble_has_for_mtu(all, 0) == all);                  // unknown: left to the push parser
  CHECK(ble_has_for_mtu(all, 23) == (all & ~CMDR_HAS_PUSH));  // no exchange happened
  CHECK(ble_has_for_mtu(all, BLE_MTU_MIN) == all);
  CHECK(ble_has_for_mtu(all, 517) == all);
  CHECK(ble_has_for_mtu(CMDR_HAS_API, 23) == CMDR_HAS_API);

  // ---- the session thread ----
  remove("/tmp/test_ble_pin");
  cmdr_cfg cfg = cmdr_cfg_default;
  cfg.id_path = "/tmp/test_ble_id", cfg.pin_path = "/tmp/test_ble_pin", cfg.wifi_first = 0;
  ble *b = ble_start_io(&cfg, &fake);
  CHECK(b);
  cmdr_t pub;
  CHECK(ble_state(b, &pub) == -1 && pub.state == CS_IDLE);

  ble_msg m = {.op = BLE_PRESS, .id = 7, .a = ACT_COUNT - 1, .b = 1, .gear = 'P'};
  CHECK(ble_post(b, &m) == 0);
  CHECK(next(b, &m, 1000) && m.op == BLE_PRESS && m.id == 7 && m.result == CMDR_E_NOT_READY);

  // A dial blocks the thread in the scan; posting never waits for it.
  CHECK(ble_post(b, &(ble_msg){.op = BLE_CONNECT}) == 0);
  g_usleep(50 * 1000);
  int64_t t0 = g_get_monotonic_time();
  for (int i = 0; i < 100; i++) CHECK(ble_post(b, &(ble_msg){.op = BLE_WIFI, .a = 0, .b = 0}) == 0);
  CHECK(g_get_monotonic_time() - t0 < 50 * 1000);
  CHECK(wait_state(b, CS_AWAY, 2000));
  CHECK(ble_state(b, &pub) > 0 && !strcmp(pub.why, "no Commander advertising (car asleep? phone app on it?)"));

  // Replies come back in the order asked; the dial's teardown said "no car data" as BLE_DATA n 0.
  CHECK(ble_post(b, &(ble_msg){.op = BLE_STEP_ASIDE, .id = 1}) == 0);
  CHECK(ble_post(b, &(ble_msg){.op = BLE_PAUSE, .id = 2}) == 0);
  CHECK(next(b, &m, 2000) && m.op == BLE_STEP_ASIDE && m.id == 1 && m.result == 0);  // no link was up
  CHECK(next(b, &m, 2000) && m.op == BLE_PAUSE && m.id == 2 && m.result == 0);
  CHECK(datas >= 1);
  CHECK(wait_state(b, CS_PAUSED, 1000));
  CHECK(!ble_take(b, &m));

  // Adapter off: CS_OFF, then its coming back on (looked at every 5 s) dials.
  g_atomic_int_set(&adapter, 0);
  CHECK(ble_post(b, &(ble_msg){.op = BLE_RESUME}) == 0);
  CHECK(wait_state(b, CS_OFF, 2000));
  int before = g_atomic_int_get(&scans);
  g_atomic_int_set(&adapter, 1);
  CHECK(wait_state(b, CS_AWAY, 7000));
  CHECK(g_atomic_int_get(&scans) == before + 1);

  ble_stop(b);
  CHECK(g_atomic_int_get(&disconnects) == 0);  // never linked: nothing to close
  printf("test_ble: %d checks passed\n", checks);
  return 0;
}
