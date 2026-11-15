// The stateful expectations of test/obd_test.dart (same frames, same values) and the link timings of
// test/link_test.dart (its fake Commander, in process), on a fake monotonic clock.
#include "../core/sched.h"
#include "../core/panda.h"
#include "signals_gen.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static int pass, fail;
#define CHECK(c) do { if (c) pass++; else { fail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define NEAR(a, b, tol) CHECK(fabs((double)(a) - (double)(b)) <= (tol))
#define ISNUL(x) CHECK(isnan(x))
#define STR(a, b) CHECK((a) && !strcmp((a), (b)))

static car_t C;
static link_t L;
static int64_t now = 1000000;   // any start: nothing reads an epoch

// ---- the fake Commander (link_test.dart _Commander): filters as the wiki has them ----
static struct {
  uint8_t filter[256];
  bool session, can, ack_hello, ack_commands;
  int acks_due, foreign, opens, removes, speed_frames, sent;
  int64_t settle_until;
  bool (*drop)(const uint8_t *b, size_t n);
  const uint16_t *only; int n_only;      // the ids still on the bus, whatever the filter
  uint8_t data[2048][8];
  uint8_t seen[256];                     // ids delivered
} K;
static bool in(const uint8_t *set, unsigned id) { return set[id >> 3] >> (id & 7) & 1; }

static void k_send(void *ctx, const uint8_t *b, size_t n) {
  (void)ctx;
  K.sent++;
  if (b[0] == 0x0e) K.removes++;
  if (K.drop && K.drop(b, n)) return;
  if (n == 5 && b[0] == 'e') {
    if (!K.session || K.ack_hello) { K.session = true; K.acks_due++; }
    return;
  }
  if (b[0] == 0x18) memset(K.filter, 0, sizeof K.filter);
  if (b[0] == 0x0f || b[0] == 0x0e)
    for (size_t o = 1; o + 2 < n; o += 3) {
      unsigned id = b[o + 1] << 8 | b[o + 2];
      if (b[0] == 0x0f) K.filter[id >> 3] |= 1 << (id & 7);
      else K.filter[id >> 3] &= ~(1 << (id & 7));
    }
  if (K.ack_commands) K.acks_due++;
}
static bool k_open(void *ctx) { (void)ctx; K.opens++; return true; }
static int k_settle(void *ctx, int64_t t) { (void)ctx; return t < K.settle_until ? (int)(K.settle_until - t) : 0; }

static void rec(uint8_t *b, uint16_t id, uint8_t bus, const uint8_t *d, uint8_t len) {
  memset(b, 0, 16);
  uint32_t w0 = (uint32_t)id << 21, w1 = (uint32_t)bus << 4 | len;
  for (int i = 0; i < 4; i++) b[i] = w0 >> 8 * i, b[4 + i] = w1 >> 8 * i;
  memcpy(b + 8, d, len);
}

// Time passes in 20 ms steps: the Commander's ACKs, then its frames for the filter, then the link's timers.
static void run(int ms) {
  static uint8_t dg[16 * 80];
  for (int t = 0; t < ms; t += 20) {
    now += 20;
    for (; K.acks_due > 0; K.acks_due--) {
      rec(dg, 6, 15, (const uint8_t[]){0}, 1);
      link_on_datagram(&L, dg, 16, now);
    }
    size_t n = 0;
    if (K.can && K.session) {
      for (int i = 0; i < kSubscribeCount; i++) {
        uint16_t id = kSubscribeIds[i];
        bool on = in(K.filter, id);
        if (K.only) { bool o = false; for (int k = 0; k < K.n_only; k++) o |= K.only[k] == id; on &= o; }
        if (!on) continue;
        rec(dg + 16 * n++, id, 0, K.data[id], 8);
        K.seen[id >> 3] |= 1 << (id & 7);
        if (id == 0x257) K.speed_frames++;
      }
      for (int i = 0; i < K.foreign; i++) rec(dg + 16 * n++, 0x7f0, 0, (const uint8_t[]){1, 2, 3, 4, 5, 6, 7, 8}, 8);
    }
    if (n) link_on_datagram(&L, dg, 16 * n, now);
    link_poll(&L, now);
  }
}

static void fresh(void) {
  memset(&K, 0, sizeof K);
  K.can = true;
  car_init(&C);
  link_init(&L, &C, (link_port_t){k_send, k_open, NULL, NULL, NULL}, now);
}

#define BYTES(...) (const uint8_t[]){__VA_ARGS__}, (uint8_t)sizeof((const uint8_t[]){__VA_ARGS__})
#define FR(id, bus, ...) link_on_frame(&L, id, bus, BYTES(__VA_ARGS__), now)

// A Commander push: {field, value} pairs; a negative field is present but not an int (a string, a float).
static bt_map_t bt_of(const long long (*kv)[2], int n) {
  bt_map_t m;
  memset(&m, 0, sizeof m);
  for (int i = 0; i < n; i++) {
    int f = (int)(kv[i][0] < 0 ? -kv[i][0] : kv[i][0]);
    m.has[f] = 1; m.is_int[f] = kv[i][0] >= 0; m.v[f] = kv[i][1];
  }
  return m;
}
#define BT(car, ...) do { const long long kv_[][2] = {__VA_ARGS__}; bt_map_t m_ = bt_of(kv_, sizeof kv_ / sizeof *kv_); \
  car_from_bt(car, &m_, now); } while (0)

static int group_of(unsigned id) {
  for (int g = 0; g < kGroupCount; g++) for (int i = 0; i < kSparse[g].n; i++) if (kSparse[g].ids[i] == id) return g;
  return -1;
}
static bool always_on(unsigned id) {
  for (int i = 0; i < kAlwaysOnCount; i++) if (kAlwaysOn[i] == id) return true;
  return false;
}
static int on_slots(int g) {
  int n = 0;
  for (int s = 0; s < kWheelSlots; s++) for (int k = 1; k <= kWheel[s][0]; k++) n += kWheel[s][k] == g;
  return n;
}
static void pack(double v, double a, uint8_t *b) {   // 0x132: V 0|16 x0.01, A 16|16 signed x0.1
  int av = (int)lround(a * 10) & 0xffff, vv = (int)lround(v * 100);
  uint8_t x[6] = {vv & 0xff, vv >> 8, av & 0xff, av >> 8, 0, 0};
  memcpy(b, x, 6);
}
static void fr_pack(double v, double a) { uint8_t b[6]; pack(v, a, b); link_on_frame(&L, 0x132, 1, b, 6, now); }

static void test_stale_and_sources(void) {   // 'Panda holds nothing stale and picks the right source'
  fresh();
  FR(0x118, 0, 0, 0, 0x80, 0, 0, 0, 0x08, 0); CHECK(C.braking);
  FR(0x118, 0, 0, 0, 0x20, 0, 0, 0, 0x08, 0); CHECK(!C.braking);
  FR(0x118, 0, 0, 0, 0x80, 0, 0, 0, 0x08, 0); CHECK(C.braking);
  FR(0x118, 0, 0, 0, 0x80, 0, 0, 0, 0, 0); CHECK(!C.braking);
  FR(0x118, 0, 0, 0, 0x20, 0, 0, 0, 0x08, 0); CHECK(!C.braking);
  FR(0x25A, 0, 0x74, 0x72, 0x70, 0x71, 0, 0); NEAR(C.tp_fl, 2.9, 1e-9);
  FR(0x219, 0, 0x05, 0, 0, 0, 0, 0, 0); NEAR(C.tp_fl, 2.9, 1e-9);
  FR(0x219, 0, 0x05, 0, 0x6F, 0x6E, 0x72, 0x70, 0); NEAR(C.tp_fl, 2.775, 1e-9);
  FR(0x25A, 0, 0x74, 0x72, 0x70, 0x71, 0, 0); NEAR(C.tp_fl, 2.775, 1e-9);
  car_t c; car_init(&c);   // the chassis bus's 3-byte 0x219 is not page 5
  car_frame(&c, 0x219, 1, BYTES(0x9D, 0x79, 0x31), now);
  car_frame(&c, 0x25A, 0, BYTES(0x70, 0x70, 0x6F, 0x6E, 0, 0), now);
  NEAR(c.tp_fl, 2.8, 1e-9);
  car_init(&c);            // per wheel
  car_frame(&c, 0x219, 0, BYTES(0x05, 0, 0x6F, 0, 0, 0, 0), now);
  car_frame(&c, 0x25A, 0, BYTES(0x74, 0x72, 0x70, 0x71, 0, 0), now);
  NEAR(c.tp_fl, 2.775, 1e-9); NEAR(c.tp_fr, 2.85, 1e-9);
  FR(0x243, 0, 0x01, 0xFC, 0, 0, 0, 0, 0, 0); CHECK(C.recirc_on);
  FR(0x243, 0, 0x01, 0x7C, 0, 0, 0, 0, 0, 0); CHECK(C.recirc_on);
  FR(0x243, 0, 0x01, 0x40, 0, 0, 0, 0, 0, 0); CHECK(!C.recirc_on);
  FR(0x25D, 0, 0x77, 0x44, 0x01, 0x86, 0x04, 0xF0, 0xCA, 0x03); CHECK(C.charge_port);
  FR(0x25D, 1, 0x37, 0, 0, 0, 0, 0); CHECK(C.charge_port);
  FR(0x25D, 0, 0x37, 0x41, 0x01, 0x06, 0x04, 0xF0, 0xCA, 0x03); CHECK(!C.charge_port);
  FR(0x3F5, 0, 0, 0, 0, 0, 0x04, 0, 0, 0); CHECK(C.low_beam == 1 && C.drl == 1 && C.high_beam == 0);
  FR(0x3F6, 0, 0x50, 0, 0, 0, 0, 0, 0, 0); CHECK(C.high_beam == 1);
  FR(0x3F6, 0, 0x00, 0, 0, 0, 0, 0, 0, 0);
  FR(0x3F5, 0, 0, 0, 0, 0, 0x02, 0, 0, 0); CHECK(C.low_beam == 0 && C.drl == 1 && C.high_beam == 0);
  FR(0x3F5, 0, 0, 0, 0, 0, 0x0F, 0, 0, 0); CHECK(C.low_beam == -1 && C.drl == -1);
  FR(0x3E2, 0, 0x40, 0x15, 0, 0, 0, 0, 0); CHECK(C.map_light);
  FR(0x3E2, 1, 0x00, 0x00, 0, 0, 0, 0, 0, 0); CHECK(C.map_light);
  FR(0x3E2, 0, 0x00, 0x10, 0, 0, 0, 0, 0); CHECK(C.map_light);
  FR(0x3E2, 0, 0x00, 0x00, 0, 0, 0, 0, 0); CHECK(!C.map_light);
  FR(0x3A1, 0, 0x05, 0, 0, 0, 0, 0, 0, 0); CHECK(C.belt_3l && !C.belt_3r);
  FR(0x3A1, 0, 0x09, 0, 0, 0, 0, 0, 0, 0); CHECK(!C.belt_3l);
}

static void test_flasher(void) {   // 'indicator strips keep step with the car's flasher' (200/400 ms steps)
  fresh();
  FR(0x3F5, 0, 0x02, 0, 0, 0, 0, 0, 0, 0); CHECK(C.ind_l && car_blink_lit(&C, now));
  now += 200;
  FR(0x3F5, 0, 0x01, 0, 0, 0, 0, 0, 0, 0); CHECK(car_blink_lit(&C, now));
  now += 200; CHECK(!car_blink_lit(&C, now));
  FR(0x3F5, 0, 0x0A, 0, 0, 0, 0, 0, 0, 0); CHECK(C.ind_l && C.ind_r && car_blink_lit(&C, now));
  FR(0x3F5, 0, 0x00, 0, 0, 0, 0, 0, 0, 0); CHECK(!C.ind_l && !C.ind_r && !car_blink_lit(&C, now));
  FR(0x3F5, 0, 0x02, 0, 0, 0, 0, 0, 0, 0);
  now += 400; CHECK(!car_blink_lit(&C, now));
  FR(0x3F5, 0, 0x08, 0, 0, 0, 0, 0, 0, 0); CHECK(!C.ind_l && C.ind_r && car_blink_lit(&C, now));
  FR(0x3F5, 0, 0x00, 0, 0, 0, 0, 0, 0, 0);
  now += 1000;
  // Not in the app's test: the half cycle learnt from a 722 ms gap between same-lamp edges.
  FR(0x3F5, 0, 0x02, 0, 0, 0, 0, 0, 0, 0); FR(0x3F5, 0, 0x01, 0, 0, 0, 0, 0, 0, 0);
  now += 722;
  FR(0x3F5, 0, 0x02, 0, 0, 0, 0, 0, 0, 0); CHECK(C.p.half_ms == (360 * 3 + 361) / 4);
}

static void test_arrival(void) {   // 'arrival % and route length from the battery frames'
  fresh();
  C.kph = true;
  FR(0x352, 0, 0x00, 0x80, 0xF8, 0x0F, 0xE3, 0x09, 0xD1, 0x09);
  FR(0x352, 0, 0x01, 0x80, 0x70, 0x01, 0xD1, 0x09, 0x00, 0x00);
  FR(0x347, 0, 0x01, 0x7F, 0xFF, 0x0D, 0xFF, 0x03, 0x50, 0x13);
  FR(0x33A, 0, 0xC0, 0xF8, 0xC3, 0xE3, 0xFD, 0xFF, 0xE5, 0x68);
  NEAR(C.pack_kwh, 81.76, 0.001); NEAR(C.pack_left_kwh, 50.62, 0.001); NEAR(C.pack_buffer_kwh, 3.68, 0.001);
  NEAR(C.nav_energy_kwh, 49.44, 0.001);
  NEAR(car_arrive_pct(&C), 58.61, 0.01);
  NEAR(car_range_at(&C, car_arrive_pct(&C)), 301.22, 0.01);
  NEAR(car_trip_kwh(&C, car_arrive_pct(&C)), 50.62 - 49.44, 0.001);
  ISNUL(car_trip_kwh(&C, NUL));
  int idx[] = {20, 4, 13, 9};
  for (int i = 0; i < 4; i++) FR(0x352, 1, idx[i], 0x00, 0xA0, 0x0F, 0x00, 0x10, 0x00, 0x00);
  NEAR(C.pack_kwh, 81.76, 0.001); NEAR(C.pack_left_kwh, 50.62, 0.001); NEAR(C.pack_buffer_kwh, 3.68, 0.001);
  // 'pack capacity on a Model 3 with TRCM_info on 0x352'
  fresh();
  FR(0x352, 1, 0x14, 0x00, 0xA0, 0x0F, 0x00, 0x10, 0x00, 0x00);
  FR(0x352, 0, 0x00, 0x80, 0xF8, 0x0F, 0xE3, 0x09, 0xD1, 0x09);
  FR(0x392, 0, 0x01, 0x00, 0x00, 0x00, 0x00, 0x14, 0x03, 0x00);
  NEAR(C.pack_new_kwh, 78.8, 0.001);
  FR(0x392, 1, 0x01, 0x00, 0x00, 0x00, 0x00, 0x15, 0x03, 0x00);   // not the battery's bus: held
  NEAR(C.pack_new_kwh, 78.8, 0.001);
}

static void test_bt_stands_in(void) {   // 'Bluetooth stands in for WiFi, WiFi first'
  fresh();
  C.kph = true;
  CHECK(!car_live(&C));
  BT(&C, {-7, 0}, {101, 1});
  CHECK(!car_live(&C));
  BT(&C, {61, 87}, {62, 125}, {66, 4}, {65, 1}, {63, 2}, {51, 62}, {53, 399}, {111, 60}, {112, 192}, {105, 1},
     {110, 90}, {81, 0x11}, {82, 0x33}, {85, 0x10}, {113, 1}, {118, 2}, {34, 800}, {35, 4088},
     {36, 2531}, {37, 368}, {43, 1950}, {44, 1948});
  CHECK(car_via_bt(&C) && car_live(&C) && !C.wifi_live);
  CHECK(C.speed == 87 && C.pedal_pct == 50 && C.gear == 'D' && C.braking);
  CHECK(C.ind_l && !C.ind_r && car_blink_lit(&C, now));
  CHECK(C.front_kw == 15.5 && C.rear_kw == -100 && C.soc_pct == 60 && C.range_mi == 192 && C.speed_limit == 90);
  CHECK(C.low_beam == 1 && C.high_beam == 0 && C.drl == 1);
  CHECK(C.door_fl && !C.trunk && !car_parked(&C, now)); STR(C.autopilot, "TACC");
  NEAR(C.pack_kwh, 81.76, 1e-9); NEAR(C.pack_new_kwh, 80, 1e-9); NEAR(car_imbalance_mv(&C), 4, 1e-6);
  NEAR(car_range_at(&C, 58.61), 301.2, 0.1);
  BT(&C, {66, 1}, {113, 0}, {62, 255}, {51, 2047}, {53, 0});
  CHECK(C.gear == 'P' && C.speed == 87 && car_parked(&C, now));   // Bluetooth: a speed can stay stale (an empty push)
  CHECK(C.door_fl && C.pedal_pct == 50 && C.front_kw == 15.5);
  now += 60000; CHECK(car_parked(&C, now));   // over Bluetooth a P of any age holds
  L.state = LINK_LIVE; C.wifi_live = true;     // p.status = 'live'
  BT(&C, {61, 10});
  CHECK(C.speed == 87);
  link_disconnect(&L, now);
  CHECK(C.speed == 10 && C.gear == 'P' && car_via_bt(&C));
  car_from_bt(&C, NULL, now);
  CHECK(!car_live(&C) && C.gear == 0 && !C.door_fl); ISNUL(C.front_kw);
}

static void test_capture(void) {   // 'the newer stats decode a capture from the car as the catalog says'
  fresh();
  C.kph = true;
  FR(0x321, 0, 0x76, 0xF9, 0x25, 0x5F, 0x02, 0x5F, 0xC0, 0x38);
  FR(0x2B6, 0, 0x88, 0x0D, 0x02);
  FR(0x252, 0, 0x18, 0x18, 0x25, 0x06, 0x3A, 0x51, 0x18, 0x00);
  FR(0x2B3, 0, 0xE9, 0x5D, 0xF0, 0x4F, 0x63, 0x00, 0x06, 0x03);
  FR(0x212, 0, 0x19, 0x24, 0x00, 0x40, 0x13, 0x2D, 0x60, 0x86);
  FR(0x472, 0, 0x71, 0x00, 0x00, 0x00, 0x48, 0x7F, 0xFF, 0x07);
  FR(0x333, 0, 0x00, 0x10, 0x58, 0x06, 0x00);
  FR(0x33B, 0, 0x34, 0x00, 0x00, 0x00, 0xFF, 0x07);
  FR(0x286, 0, 0xAE, 0x04, 0x00, 0x61, 0xC0, 0x01, 0x00, 0x00);
  FR(0x3F8, 0, 0x03, 0x08, 0x08, 0x00, 0x59, 0x3D, 0x94, 0x20);
  FR(0x2F3, 0, 0x09, 0x09, 0x2B, 0x24, 0x00, 0x60, 0x00, 0x42);
  FR(0x332, 0, 0x04, 0x05, 0x60, 0x5F, 0x60, 0x00);
  FR(0x3A1, 0, 0xE4, 0x42, 0x00, 0x8A, 0x01, 0x44, 0x62, 0xFB);
  FR(0x347, 0, 0x01, 0x7F, 0xFF, 0x0D, 0xFF, 0x03, 0x50, 0x13);
  FR(0x399, 0, 0x00, 0x0A, 0xFF, 0xE0, 0xB0, 0xC8, 0x87, 0x84);
  FR(0x118, 0, 0xAF, 0x8D, 0x31, 0x10, 0x00, 0x48, 0x00, 0x80);
  CHECK(C.outside_c == 7.5 && !C.washer_low);
  CHECK(!C.slippery && !C.grip_held && C.tc_mode == NULL);
  NEAR(C.regen_max_kw, 61.68, 1e-9);
  CHECK(C.co2ppm == 935);
  CHECK(!C.charging && C.charge_kw == 0 && C.charge_max_kw == 11.25 && C.charge_why == NULL);
  CHECK(C.charge_limit_pct == 60 && C.charge_minutes == 0);
  CHECK(isnan(C.cruise_set) && C.follow_gap == 2); STR(C.fan1, "LOW"); STR(C.fan2, "OFF");
  CHECK(C.pack_temp_c == 7.5 && !C.pack_heating && isnan(C.precond_c));
  CHECK(C.hands_on == 2);
  FR(0x3C2, 0, 0x00, 0x55, 0x55, 0x55, 0x00, 0x00, 0x59, 0x41);
  FR(0x31A, 0, 0x60, 0x00, 0x70, 0x18, 0x00, 0x00, 0xD0, 0xD5);
  CHECK(C.belt_driver);
  CHECK(C.buckled == 0);
  FR(0x3C2, 0, 0x00, 0x55, 0x55, 0x55, 0x00, 0x00, 0x5A, 0x41);
  CHECK(C.buckled == SEAT_D);
}

static void test_buckles_fans(void) {   // 'buckles and fans'
  fresh();
  const int all = 0x7f;
  FR(0x3C2, 0, 0x00, 0x55, 0x55, 0x55, 0x00, 0x00, 0x6A, 0x81);
  FR(0x31A, 0, 0x68, 0x51, 0x70, 0x18, 0x00, 0x00, 0x00, 0x00);
  CHECK(C.buckled == all);
  FR(0x3C2, 0, 0x01, 0, 0, 0, 0, 0, 0, 0);
  CHECK(C.buckled == all);
  FR(0x3C2, 0, 0x00, 0x55, 0x55, 0x55, 0x00, 0x00, 0x6A, 0x41);
  FR(0x31A, 0, 0x60, 0x41, 0x70, 0x18, 0x00, 0x00, 0x00, 0x00);
  CHECK(C.buckled == (SEAT_D | SEAT_2L | SEAT_2R | SEAT_3R));
  FR(0x3C2, 0, 0x00, 0x55, 0x55, 0x55, 0x00, 0x00, 0x6A, 0x81);
  FR(0x31A, 0, 0x68, 0x51, 0x70, 0x18, 0x00, 0x00, 0x00, 0x00);
  FR(0x31A, 0, 0x60, 0x52, 0x70, 0x18, 0x00, 0x00, 0x00, 0x00);
  CHECK(C.buckled == (SEAT_D | SEAT_2L | SEAT_2C | SEAT_3L | SEAT_3R));
  FR(0x2F3, 0, 0x09, 0x09, 0x24, 0x84, 0x00, 0x60, 0x00, 0x42); STR(C.fan1, "4"); STR(C.fan2, "HIGH");
  FR(0x2F3, 0, 0x49, 0x09, 0x2B, 0x24, 0x00, 0x60, 0x00, 0x42); STR(C.fan1, "MED"); CHECK(C.auto_blower == 2);
  FR(0x2F3, 0, 0x89, 0x09, 0x2B, 0x24, 0x00, 0x60, 0x00, 0x42); STR(C.fan1, "HIGH"); CHECK(C.auto_blower == 4);
  FR(0x2F3, 0, 0x49, 0x09, 0x2B, 0x04, 0x00, 0x60, 0x00, 0x42); STR(C.fan1, "MED"); STR(C.fan2, "MED");
  FR(0x2F3, 0, 0x09, 0x09, 0x2B, 0x00, 0x00, 0x60, 0x00, 0x42); STR(C.fan1, "OFF"); STR(C.fan2, "OFF");
  link_disconnect(&L, now);
  CHECK(C.buckled == 0 && C.fan1 == NULL && C.fan2 == NULL);
}

static void test_g(void) {   // 'g-forces: along from the drive unit, across from yaw rate and speed'
  fresh();
  C.kph = true;
#define ALONG(b0, b1, n) for (int i = 0; i < (n); i++) FR(0x118, 0, 0, 0, 0, 0, 0, 0, b0, b1)
  ALONG(0x80, 0x0C, 40); NEAR(C.g_long, 2.00 / 9.80665, 1e-3);
  ALONG(0x40, 0xED, 40); NEAR(C.g_long, -3.00 / 9.80665, 1e-3);
  NEAR(C.g_max_accel, 0.204, 1e-3); NEAR(C.g_max_brake, 0.306, 1e-3);
  ALONG(0x00, 0x80, 1); NEAR(C.g_long, -0.306, 1e-3);
  C.speed = 72;
  for (int i = 0; i < 40; i++) FR(0x101, 0, 0xE8, 0x03, 0, 0, 0, 0, 0, 0);
  NEAR(C.g_lat, 2.0 / 9.80665, 1e-3); NEAR(C.g_max_lat, 0.204, 1e-3);
  FR(0x118, 0, 0, 0, 0x20, 0, 0, 0, 0x00, 0x80);
  FR(0x118, 0, 0, 0, 0x80, 0, 0, 0, 0x00, 0x80);
  CHECK(C.g_max_accel == 0 && C.g_max_brake == 0 && C.g_max_lat == 0);
}

static void test_held(void) {   // 'the newer stats: held, combined, over Bluetooth, gone with the link'
  fresh();
  C.kph = true;
  FR(0x2B6, 0, 0x01, 0x00, 0x00); CHECK(C.grip_held);
  FR(0x2B6, 0, 0x00, 0x00, 0x00); CHECK(C.grip_held);
  CHECK(!car_grip_lit(&C));
  C.speed = 30; CHECK(car_grip_lit(&C));
  C.speed = 0;
  now += 3000;   // not in the app's test: the hold ends 3 s after the last flash
  FR(0x2B6, 0, 0x00, 0x00, 0x00); CHECK(!C.grip_held);
  FR(0x118, 0, 0, 0, 0, 0, 0, 0x07, 0, 0); CHECK(C.slippery && C.tc_mode == NULL);
  FR(0x118, 0, 0, 0, 0, 0, 0, 0x01, 0, 0); CHECK(!C.slippery); STR(C.tc_mode, "SLIP");
  FR(0x286, 0, 0x00, 0x20, 0x6E, 0x01, 0, 0, 0, 0); CHECK(C.cruise_set == 110);
  FR(0x212, 0, 0x00, 0x18, 0xF0, 0x00, 0x00, 0xF8, 0x00, 0x00);
  FR(0x472, 0, 0, 0, 0, 0, 0x02, 0, 0, 0);
  FR(0x33B, 0, 0, 0, 35, 0, 0, 0);
  CHECK(C.charging && C.charge_kw == 48 && C.charge_max_kw == 62 && C.charge_minutes == 35); STR(C.charge_why, "COLD PACK");
  FR(0x347, 0, 0x20, 0x00, 0xB4, 0x00, 0x00, 0x00, 0x50, 0x13); CHECK(C.pack_heating && C.precond_c == 45);
  FR(0x399, 0, 0, 0, 0, 0, 0, 0x10, 0, 0); CHECK(C.hands_on == 4);
  FR(0x399, 0, 0, 0, 0, 0, 0, 0x3C, 0, 0); CHECK(C.hands_on == -1);
  FR(0x399, 0, 0, 0x0A, 0, 0, 0, 0, 0, 0); CHECK(C.speed_limit == 50);
  FR(0x399, 0, 0, 0, 0, 0, 0, 0, 0, 0); ISNUL(C.speed_limit);
  car_t b; car_init(&b);
  BT(&b, {27, 5}, {32, 2400}, {42, 14}, {49, 1}, {108, 4});
  CHECK(b.outside_c == -3 && b.regen_max_kw == 24 && b.pack_temp_c == 3.5 && b.pack_heating && b.hands_on == 4);
  link_disconnect(&L, now);
  CHECK(!C.grip_held && !C.slippery && !C.charging && isnan(C.cruise_set) && isnan(C.precond_c) && isnan(C.regen_max_kw));
}

static void test_tables(void) {
  // 'every signal is subscribed'
  bool all = true;
  for (int i = 0; i < kSignalCount; i++) all &= sub_index(kSignals[i].id) >= 0;
  CHECK(all);
  CHECK(kPhoneSignalCount == 111 && kPhoneSubscribeCount == 43);
  // 'every subscribed id has one place: the stream or a polled group'
  bool one = true;
  for (int i = 0; i < kSubscribeCount; i++) one &= always_on(kSubscribeIds[i]) != (group_of(kSubscribeIds[i]) >= 0);
  CHECK(one);
  bool stale = false;
  for (int i = 0; i < kAlwaysOnCount; i++) stale |= sub_index(kAlwaysOn[i]) < 0;
  CHECK(!stale);
  CHECK(kAlwaysOnCount <= 43);
  CHECK(group_of(0x2E1) >= 0 && group_of(0x3C2) >= 0 && group_of(0x2B3) >= 0);
  CHECK(group_of(0x257) < 0 && group_of(0x118) < 0 && group_of(0x101) < 0 && group_of(0x132) < 0 && group_of(0x3F5) < 0);
  CHECK(group_of(0x129) >= 0);               // 'steering angle...': streamed at a crawl only
  CHECK(always_on(0x185) && sub_index(0x38D) < 0);   // 'the brake from the wheels' torque'
  // 'the wheel turns one group at a time...': the round's shape
  int most = 0;
  for (int s = 0; s < kWheelSlots; s++) most = kWheel[s][0] > most ? kWheel[s][0] : most;
  CHECK(on_slots(5) == 1 && on_slots(0) == 4 && on_slots(4) == 2 && most == 2);
  CHECK(always_on(0x318));                   // the Dash's clock frame streams
}

static void test_trying(void) {   // 'WiFi is trying while it looks, joins or says hello' (the Dash's states)
  fresh();
  CHECK(link_trying(&L));
  link_connect(&L, now);
  CHECK(L.state == LINK_HELLO && link_trying(&L));
  L.state = LINK_RETRYING; CHECK(!link_trying(&L));
  L.state = LINK_QUIET; CHECK(!link_trying(&L));
  L.state = LINK_LIVE; CHECK(!link_trying(&L));
  link_disconnect(&L, now);
}

static void test_motors(void) {
  fresh();   // 'motor power from the 2026.38 frames'
  C.speed = 30;
  FR(0x266, 1, 0x1A, 0x36, 0x18, 0x00, 0x88, 0x11, 0x00);
  FR(0x2E5, 1, 0x03, 0x0D, 0x00, 0x00, 0xD0, 0x00, 0x00);
  CHECK(C.rear_kw == 4.0 && C.front_kw == 0.0);
  FR(0x266, 2, 0x1B, 0x28, 0x19, 0x00, 0x88, 0xF1, 0x0F);
  CHECK(C.rear_kw == -4.0);

  fresh();   // 'motor power follows the pack; motor frames only set the share'
  FR(0x118, 1, 0, 0, 0x80, 0, 0, 0, 0, 0);
  C.speed = 30;
  fr_pack(400, -20);
  FR(0x2E5, 1, 0, 0, 0, 0, 0xD0, 0x00, 0);
  FR(0x266, 1, 0, 0, 0, 0, 0x88, 0x10, 0);
  CHECK(C.rear_kw == 4.0 && C.front_kw == 0.0);
  CHECK(C.other_kw == 4.0);
  fr_pack(400, -50); NEAR(C.rear_kw, 16.0, 1e-9); CHECK(C.front_kw == 0.0);
  FR(0x266, 1, 0, 0, 0, 0, 0x88, 0x14, 0); NEAR(C.rear_kw, 16.0, 0.1);
  FR(0x118, 1, 0, 0, 0x20, 0, 0, 0, 0, 0);
  fr_pack(400, -50); CHECK(C.rear_kw == 5.0);

  fresh();   // 'a rear-motor car: the rear follows the pack alone'
  car_set_type(&C, CAR_Y5_RWD);
  FR(0x118, 1, 0, 0, 0x80, 0, 0, 0, 0, 0);
  C.speed = 30;
  fr_pack(400, -20);
  FR(0x266, 1, 0, 0, 0, 0, 0x88, 0x10, 0);
  ISNUL(C.front_kw); CHECK(C.other_kw == 4.0);
  fr_pack(400, -50); NEAR(C.rear_kw, 16.0, 1e-9);
  BT(&C, {51, 96}, {53, 192});
  ISNUL(C.front_kw);

  fresh();   // 'other draws: climate, 12 V and the rest of the pack-motor gap'
  FR(0x118, 1, 0, 0, 0x80, 0, 0, 0, 0, 0);
  FR(0x2A7, 1, 150, 137, 75, 21, 46, 188, 5, 129);
  FR(0x2B4, 1, 183, 5, 88, 14, 34, 2);
  FR(0x132, 1, 0x40, 0x9C, 0x6A, 0xFF, 0, 0);
  FR(0x2E5, 1, 0, 0, 0, 0, 0xD0, 0x00, 0);
  FR(0x266, 1, 0, 0, 0, 0, 0x88, 0x08, 0);
  NEAR(C.pack_kw, 6.0, 1e-9); NEAR(car_climate_kw(&C), 1.7, 1e-9); NEAR(C.lv_kw, 14.63 * 54.6 / 1000, 1e-9);
  NEAR(C.other_kw, 4 - 1.7 - 14.63 * 54.6 / 1000, 1e-9); NEAR(C.rear_kw, 2.0, 1e-9);

  fresh();   // '0x2A7 only counts with a pack voltage: the chassis bus reuses the id'
  FR(0x2A7, 1, 150, 137, 75, 21, 46, 188, 5, 129); NEAR(car_climate_kw(&C), 1.7, 1e-9);
  FR(0x2A7, 1, 255, 255, 255, 255, 255, 1, 0, 0); NEAR(car_climate_kw(&C), 1.7, 1e-9);
  FR(0x2A7, 1, 150, 137, 75, 21, 46, 188, 5, 129);
  FR(0x2A7, 1, 255, 255, 255, 255); NEAR(car_climate_kw(&C), 1.7, 1e-9);

  fresh();   // 'stopped with the pedal off, the motors read their frames, not the pack'
  FR(0x118, 1, 0, 0, 0x80, 0, 0, 0, 0, 0);
  FR(0x132, 1, 0x40, 0x9C, 0xCE, 0xFF, 0, 0);   // -5 A: 2 kW
  FR(0x266, 1, 0, 0, 0, 0, 0x88, 0x01, 0); CHECK(C.rear_kw == 0.0);
  FR(0x132, 1, 0x40, 0x9C, 0x6A, 0xFF, 0, 0);   // -15 A: 6 kW
  CHECK(C.rear_kw == 0.0);

  fresh();   // 'stopped, a late motor frame shows no regen'
  FR(0x118, 1, 0, 0, 0x80, 0, 0, 0, 0, 0);
  FR(0x266, 1, 0, 0, 0, 0, 0x88, 0xF1, 0x0F); CHECK(C.rear_kw == 0.0);

  fresh();   // '0x2A7 must match the pack it runs on'
  FR(0x132, 1, 0x40, 0x9C, 0, 0, 0, 0);
  FR(0x2A7, 1, 150, 137, 75, 21, 46, 188, 5, 129); NEAR(car_climate_kw(&C), 1.7, 1e-9);
  FR(0x2A7, 1, 255, 255, 255, 255, 255, 0x90, 0x03, 0); NEAR(car_climate_kw(&C), 1.7, 1e-9);

  fresh();   // 'a rear-motor car takes no front motor frame'
  car_set_type(&C, CAR_Y5_RWD);
  FR(0x2E5, 1, 0, 0, 0, 0, 0xD0, 0x48, 0); ISNUL(C.front_kw);
  fresh();
  FR(0x2E5, 1, 0, 0, 0, 0, 0xD0, 0x48, 0); CHECK(C.front_kw == 18);
  car_set_type(&C, CAR_Y5_RWD); ISNUL(C.front_kw);
}

static void test_bt_motors(void) {
  fresh();   // 'BT motor power from a Commander that still reads the old layout is dropped'
  BT(&C, {66, 1}, {61, 0}, {51, 1529}, {53, 1028}); CHECK(isnan(C.front_kw) && isnan(C.rear_kw));
  BT(&C, {53, 1019}); CHECK(isnan(C.front_kw) && isnan(C.rear_kw));
  BT(&C, {53, 258}); ISNUL(C.rear_kw);
  BT(&C, {66, 4}, {61, 20}, {51, 40}, {53, 40});
  CHECK(isnan(C.front_kw) && isnan(C.rear_kw));
  now += 30000;   // not in the app's test: the 30 s hold runs out
  BT(&C, {51, 40}); CHECK(C.front_kw == 10 && C.rear_kw == 10);   // 40 zigzag = 20 counts = 10 kW
  car_t q; car_init(&q);
  BT(&q, {66, 4}, {61, 20}, {65, 2}); CHECK(!q.braking);
  BT(&q, {65, 1}); CHECK(q.braking);

  fresh();   // 'BT limits are the car's own, parked output is not held against it'
  BT(&C, {66, 4}, {61, 90}, {51, 400}, {53, 700}); CHECK(C.front_kw == 100.0 && C.rear_kw == 175.0);
  BT(&C, {53, 1200}); CHECK(C.rear_kw == 300.0);
  BT(&C, {66, 1}, {51, 0}, {53, 8}); CHECK(C.rear_kw == 2.0);
  BT(&C, {53, 16}); ISNUL(C.rear_kw);
  BT(&C, {66, 4}, {53, 80}); CHECK(C.rear_kw == 20.0);
  BT(&C, {53, 419}); CHECK(C.rear_kw == -105.0);

  fresh();   // 'fields only the trace reads do not pass for car data'
  BT(&C, {28, 36000}, {29, 12}); CHECK(!car_via_bt(&C));
  BT(&C, {61, 30}); CHECK(car_via_bt(&C));
}

static void test_steer_brake(void) {
  fresh();   // 'steering angle reaches the path lines'
  FR(0x129, 1, 0, 0, 0x0E, 0x21, 0, 0, 0, 0); NEAR(C.steer_deg, -27.0, 1e-9);
  fresh();   // 'moving, the path lines follow the yaw rate'
  C.kph = true; C.speed = 72;
  FR(0x101, 0, 0xE8, 0x03, 0, 0, 0, 0, 0, 0); NEAR(C.steer_deg, 11.34, 0.01);
  FR(0x129, 1, 0, 0, 0x0E, 0x21, 0, 0, 0, 0);
  FR(0x101, 0, 0xE8, 0x03, 0, 0, 0, 0, 0, 0); NEAR(C.steer_deg, -27.0, 1e-9);
  fresh();   // 'the path lines stay 2 s after the wheel is fairly straight'
  CHECK(!car_steer_lines(&C, now));
  car_set_steer(&C, 2, now); CHECK(!car_steer_lines(&C, now));
  car_set_steer(&C, 200, now); CHECK(car_steer_lines(&C, now));
  car_set_steer(&C, 1, now); CHECK(car_steer_lines(&C, now));
  now += 2100; CHECK(!car_steer_lines(&C, now));
  fresh();   // 'the brake from the wheels' torque'
  FR(0x185, 1, 0x14, 0x40, 0x01, 0x0A, 0xA0, 0x00, 0x54, 0xD9); NEAR(C.brake_pct, sqrt(120 / 4000.0) * 100, 1e-9);
  FR(0x185, 1, 0, 0, 0, 0, 0, 0, 0x04, 0x8A); CHECK(C.brake_pct == 0);
  FR(0x185, 1, 0xFE, 0xEF, 0xFF, 0xFE, 0xEF, 0xFF, 0, 0); CHECK(C.brake_pct == 100);
  fresh();   // 'every torque shows, each wheel on its own, the accelerator down too (the stability control)'
  C.pedal_pct = 20;
  FR(0x185, 1, 0x0A, 0, 0, 0, 0, 0, 0, 0); CHECK(C.brake_pct > 0 && C.brake_wheel_nm[0] == 20 && C.brake_wheel_nm[3] == 0);
  FR(0x185, 1, 0x08, 0x80, 0x00, 0x07, 0x70, 0x00, 0, 0);   // 16 / 16 / 14 / 14 Nm
  CHECK(C.brake_wheel_nm[0] == 16 && C.brake_wheel_nm[1] == 16 && C.brake_wheel_nm[2] == 14 && C.brake_wheel_nm[3] == 14);
  FR(0x185, 1, 0, 0, 0, 0, 0, 0, 0, 0); CHECK(C.brake_pct == 0 && C.brake_wheel_nm[1] == 0);
  fresh();   // 'brake torque and steering ignore their not-available value'
  FR(0x185, 1, 0x14, 0x40, 0x01, 0x0A, 0xA0, 0x00, 0x54, 0xD9);
  FR(0x129, 1, 0, 0, 0x0E, 0x21, 0, 0, 0, 0);
  CHECK(C.brake_pct > 0);
  FR(0x185, 1, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0);
  FR(0x129, 1, 0, 0, 0xFF, 0x3F, 0, 0, 0, 0);
  ISNUL(C.brake_pct);
  CHECK(isnan(C.steer_deg) && !car_steer_lines(&C, now));
}

static void test_parked(void) {   // PLAN 5.3: fails closed (the app's getter, its clock now one monotonic)
  fresh();
  CHECK(!car_parked(&C, now));                      // nothing heard
  FR(0x118, 0, 0, 0, 0x20, 0, 0, 0, 0, 0); CHECK(car_parked(&C, now));
  now += 1999; CHECK(car_parked(&C, now));
  now += 1; CHECK(!car_parked(&C, now));            // a P 2 s old: the link stalled
  FR(0x118, 0, 0, 0, 0x20, 0, 0, 0, 0, 0); CHECK(car_parked(&C, now));
  CHECK(!car_parked(&C, now - 1));                  // a clock behind the stamp is no proof
  FR(0x118, 0, 0, 0, 0x80, 0, 0, 0, 0, 0); CHECK(!car_parked(&C, now));
  FR(0x118, 0, 0, 0, 0x20, 0, 0, 0, 0, 0);
  link_disconnect(&L, now); CHECK(!car_parked(&C, now));   // the link gone
}

static void test_utc(void) {   // the Dash's 0x318 clock
  fresh();
  CHECK(C.utc.at == NEVER);
  FR(0x318, 0, 26, 11, 7, 10, 13, 42, 0, 0);
  CHECK(C.utc.year == 2026 && C.utc.month == 11 && C.utc.day == 13 && C.utc.hour == 10 && C.utc.minute == 42 &&
        C.utc.second == 7 && C.utc.at == now);
  int64_t at = now;
  now += 100;
  FR(0x318, 0, 26, 13, 7, 10, 13, 42, 0, 0); CHECK(C.utc.at == at && C.utc.month == 11);   // month 13: kept out
  FR(0x318, 0, 26, 11, 7); CHECK(C.utc.at == at);                                          // too short
}

static void test_brightness(void) {   // the Dash's own: 0x273 UI_displayBrightnessLevel, Commander field 24
  fresh();
  ISNUL(car_display_brightness(&C, now));
  FR(0x273, 0, 0, 0, 0, 0, 109, 0, 0x20, 0x10);   // 54.5 %, with the dome and tilt bits beside it
  NEAR(car_display_brightness(&C, now), 54.5, 1e-9);
  STR(C.dome_light, "AUTO"); CHECK(C.tilt_auto);   // the row added after them leaves them alone
  FR(0x273, 0, 0, 0, 0, 0, 255, 0, 0, 0); ISNUL(car_display_brightness(&C, now));   // SNA
  FR(0x273, 0, 0, 0, 0, 0, 60, 0, 0, 0); NEAR(car_display_brightness(&C, now), 30, 1e-9);
  now += 5000; NEAR(car_display_brightness(&C, now), 30, 1e-9);
  now += 1; ISNUL(car_display_brightness(&C, now));   // stale: 0x273 stopped coming
  FR(0x273, 0, 0, 0, 0, 0, 60, 0, 0, 0);
  link_disconnect(&L, now); ISNUL(car_display_brightness(&C, now));   // the link gone
  // Over Bluetooth: field 24 as % when 0..100, any age while the session pushes; else ignored, kept for one log line.
  fresh();
  BT(&C, {61, 0}, {24, 70});
  NEAR(car_display_brightness(&C, now), 70, 1e-9);
  now += 60000; NEAR(car_display_brightness(&C, now), 70, 1e-9);
  BT(&C, {24, 180}); NEAR(car_display_brightness(&C, now), 70, 1e-9); CHECK(C.p.bt_bright_bad == 180);
  L.state = LINK_LIVE; C.wifi_live = true;   // WiFi live: the push held until its own frame comes
  NEAR(car_display_brightness(&C, now), 70, 1e-9);
  FR(0x273, 0, 0, 0, 0, 0, 100, 0, 0, 0); NEAR(car_display_brightness(&C, now), 50, 1e-9);
  car_from_bt(&C, NULL, now);
  NEAR(car_display_brightness(&C, now), 50, 1e-9);
}

static void test_foreign(void) {   // 'ids nobody asked for are dropped and counted, not decoded'
  fresh();
  uint8_t b[16];
  rec(b, 0x118, 0, (const uint8_t[8]){0}, 8); link_on_packet(&L, b, 16, now);   // the stream's start resets the counts
  rec(b, 0x7ff, 0, (const uint8_t[8]){0}, 8); link_on_packet(&L, b, 16, now);
  CHECK(L.foreign == 1 && L.frame_count == 2 && L.streaming);
}

static void test_sparse(void) {
  fresh();   // 'a trimmed id that keeps arriving gets its remove sent again'
  FR(0x118, 1, 0, 0, 0x20, 0, 0, 0, 0, 0);
  link_sparse_tick(&L, now);
  CHECK(L.sparse_ops == 6 && L.sparse_fixes == 0);
  now += 350;
  for (int i = 0; i < 3; i++) FR(0x3C2, 1, 0x29, 0x55, 0, 0, 0, 0, 0x31, 0);
  link_sparse_tick(&L, now); CHECK(L.sparse_fixes == 0);
  link_sparse_tick(&L, now);
  now += 350;
  for (int i = 0; i < 3; i++) FR(0x3C2, 1, 0x29, 0x55, 0, 0, 0, 0, 0x31, 0);
  link_sparse_tick(&L, now); CHECK(L.sparse_fixes == 1);
  link_sparse_tick(&L, now); CHECK(L.sparse_fixes == 1);

  fresh();   // 'the wheel turns one group at a time, the car's parked ones in P alone'
  FR(0x118, 1, 0, 0, 0x80, 0, 0, 0, 0, 0);
  link_sparse_tick(&L, now);
  int start = L.sparse_ops;
  for (int i = 0; i < 15; i++) link_sparse_tick(&L, now);
  CHECK(L.sparse_ops - start >= 20 && L.sparse_ops - start <= 32);

  fresh();   // 'charging never sticks, and the frunk leaves P with its last state'
  FR(0x118, 1, 0, 0, 0x20, 0, 0, 0, 0, 0);
  C.charging = true;
  bool on = true;
  for (int i = 0; i < 16; i++) { link_sparse_tick(&L, now); on &= link_group_on(&L, 5); }
  CHECK(on);
  C.charging = false;
  FR(0x118, 1, 0, 0, 0x80, 0, 0, 0, 0, 0);
  link_sparse_tick(&L, now); CHECK(link_group_on(&L, 6));
  link_sparse_tick(&L, now); CHECK(!link_group_on(&L, 6));
  int seen = 0, frunk = 0;
  for (int i = 0; i < 16; i++) { link_sparse_tick(&L, now); seen += link_group_on(&L, 5); frunk += link_group_on(&L, 6); }
  CHECK(seen == 1 && frunk == 0);

  fresh();   // 'a Commander ACK mid-stream leaves the polling alone'
  uint8_t ack[16];
  rec(ack, 6, 15, (const uint8_t[1]){0}, 0);
  link_on_packet(&L, ack, 16, now); CHECK(L.acks == 1 && L.subscribes == 1);
  FR(0x257, 1, 0, 0, 0, 0, 0, 0, 0, 0);
  link_sparse_tick(&L, now);
  bool off = link_group_on(&L, 1);
  link_on_packet(&L, ack, 16, now); CHECK(L.acks == 2 && L.subscribes == 1 && link_group_on(&L, 1) == off);

  fresh();   // 'a press wakes the groups its buttons light from, for a moment'
  FR(0x118, 1, 0, 0, 0x80, 0, 0, 0, 0, 0);
  link_sparse_tick(&L, now); CHECK(!link_group_on(&L, 5) && !link_group_on(&L, 6));
  link_pressed(&L, now);
  link_sparse_tick(&L, now); CHECK(link_group_on(&L, 2) && link_group_on(&L, 5));
  CHECK(!link_group_on(&L, 3)); CHECK(!link_group_on(&L, 6));
  FR(0x118, 1, 0, 0, 0x20, 0, 0, 0, 0, 0);
  link_sparse_tick(&L, now); CHECK(link_group_on(&L, 6));

  fresh();   // 'ACKs over a silent stream bring it back, and the rate says when it is known'
  FR(0x257, 1, 0, 0, 0, 0, 0, 0, 0, 0); CHECK(!L.speed_known);
  link_on_packet(&L, ack, 16, now); CHECK(L.subscribes == 0);
  now += 2100;
  link_on_packet(&L, ack, 16, now); CHECK(L.subscribes == 1);
  link_push(&L, now); CHECK(!L.speed_known && L.speed_hz == 0);
  now += 1050;
  link_push(&L, now); CHECK(L.speed_known && L.speed_hz == 0);
}

// The subscribe list itself: streamed ids first, 43 a datagram, only the gate's ops.
static struct { int n; uint8_t b[16][PANDA_DGRAM_MAX]; size_t len[16]; } S;
static void s_send(void *ctx, const uint8_t *b, size_t n) { (void)ctx; if (S.n < 16) { memcpy(S.b[S.n], b, n); S.len[S.n++] = n; } }
static void test_subscribe_list(void) {
  memset(&S, 0, sizeof S);
  car_init(&C);
  link_init(&L, &C, (link_port_t){s_send, NULL, NULL, NULL, NULL}, now);
  link_connect(&L, now);
  CHECK(S.n == 4 && S.len[0] == 5 && S.len[1] == 1 && S.b[1][0] == 0x18);   // hello, clear, two lists
  CHECK(S.len[2] == 1 + 3 * 43 && S.len[3] == 1 + 3 * (kSubscribeCount - 43));
  bool first = true;   // the first datagram holds every streamed id
  for (int i = 0; i < kAlwaysOnCount; i++) {
    bool f = false;
    for (size_t o = 1; o < S.len[2]; o += 3) f |= (S.b[2][o + 1] << 8 | S.b[2][o + 2]) == kAlwaysOn[i];
    first &= f;
  }
  CHECK(first);
  bool polled_only = true;
  for (size_t o = 1; o < S.len[3]; o += 3) polled_only &= group_of(S.b[3][o + 1] << 8 | S.b[3][o + 2]) >= 0;
  CHECK(polled_only);
  link_disconnect(&L, now);
}

// ---- link_test.dart, against the fake Commander; time in 20 ms steps ----
static int lost;
static bool drop_first_3f6_add(const uint8_t *b, size_t n) {
  return b[0] == 0x0f && n == 4 && (b[2] << 8 | b[3]) == 0x3F6 && lost++ == 0;
}
static bool drop_first_remove(const uint8_t *b, size_t n) { (void)n; return b[0] == 0x0e && lost++ == 0; }
static bool drop_first_add(const uint8_t *b, size_t n) { (void)n; return b[0] == 0x0f && lost++ == 0; }
static int subs_mark;
static bool drop_resub_list(const uint8_t *b, size_t n) {
  if (L.subscribes > subs_mark && lost == 0 && b[0] == 0x0f && n > 16) { lost++; return true; }
  return false;
}
static bool filter_has_always(void) {
  bool all = true;
  for (int i = 0; i < kAlwaysOnCount; i++) all &= in(K.filter, kAlwaysOn[i]);
  return all;
}
static int filter_polled(void) {
  int n = 0;
  for (int g = 0; g < kGroupCount; g++) for (int i = 0; i < kSparse[g].n; i++) n += in(K.filter, kSparse[g].ids[i]);
  return n;
}

static void test_link(void) {
  fresh();   // 'a steady stream: polled ids come and go, the rest stay'
  link_connect(&L, now);
  run(3200);
  CHECK(C.wifi_live && L.state == LINK_LIVE);
  CHECK(filter_has_always());
  CHECK(filter_polled() < 8);
  link_disconnect(&L, now);

  fresh();   // 'skipped ids leave the filter and come back'
  link_connect(&L, now);
  run(3200);
  CHECK(in(K.filter, 0x3F6) && in(K.filter, 0x101));
  link_set_skip(&L, (const uint16_t[]){0x3F6, 0x101, 0x3A1}, 3, now);
  run(9000);
  CHECK(!in(K.filter, 0x3F6) && !in(K.filter, 0x101) && !in(K.filter, 0x3A1));
  bool rest = true;
  for (int i = 0; i < kAlwaysOnCount; i++)
    if (kAlwaysOn[i] != 0x3F6 && kAlwaysOn[i] != 0x101) rest &= in(K.filter, kAlwaysOn[i]);
  CHECK(rest);
  link_set_skip(&L, NULL, 0, now);
  run(300);
  CHECK(in(K.filter, 0x3F6) && in(K.filter, 0x101));
  lost = 0;
  K.drop = drop_first_3f6_add;
  link_set_skip(&L, (const uint16_t[]){0x3F6, 0x101}, 2, now);
  run(1000);
  CHECK(!in(K.filter, 0x3F6) && !in(K.filter, 0x101));
  link_set_skip(&L, (const uint16_t[]){0x101}, 1, now);
  link_set_skip(&L, NULL, 0, now);
  run(2500);
  CHECK(lost == 1);
  CHECK(in(K.filter, 0x3F6) && in(K.filter, 0x101));
  link_disconnect(&L, now);

  fresh();   // 'the steering angle leaves the stream once moving'
  link_connect(&L, now);
  run(3200);
  CHECK(in(K.filter, 0x129));
  memcpy(K.data[0x257], (uint8_t[8]){0, 0, 0, 30, 0x02, 0, 0, 0}, 8);
  run(1500);
  CHECK(!in(K.filter, 0x129));
  memcpy(K.data[0x257], (uint8_t[8]){0, 0, 0, 5, 0x02, 0, 0, 0}, 8);
  run(1500);
  CHECK(in(K.filter, 0x129));
  link_disconnect(&L, now);

  fresh();   // 'sparse off: a skipped id whose remove was lost goes after all'
  link_set_sparse(&L, false);
  link_connect(&L, now);
  run(3200);
  CHECK(in(K.filter, 0x129));
  lost = 0;
  K.drop = drop_first_remove;
  link_set_skip(&L, (const uint16_t[]){0x129}, 1, now);
  run(3000);
  CHECK(lost > 1);
  CHECK(!in(K.filter, 0x129));
  link_disconnect(&L, now);

  fresh();   // 'a new stream reads everything before the wheel turns'
  link_connect(&L, now);
  run(1500);
  CHECK(K.removes == 0);
  bool every = true;
  for (int g = 0; g < kGroupCount; g++) for (int i = 0; i < kSparse[g].n; i++) every &= in(K.seen, kSparse[g].ids[i]);
  CHECK(every);
  run(2000);
  CHECK(K.removes > 0);
  link_disconnect(&L, now);

  fresh();   // 'sparse off streams everything, on the fly, and on trims again'
  link_set_sparse(&L, false);
  link_connect(&L, now);
  run(3500);
  int all_polled = 0;
  for (int g = 0; g < kGroupCount; g++) all_polled += kSparse[g].n;
  CHECK(filter_polled() == all_polled);
  link_set_sparse(&L, true);
  run(1500);
  CHECK(filter_polled() < 8);
  lost = 0;
  K.drop = drop_first_add;
  link_set_sparse(&L, false);
  run(9500);
  CHECK(lost > 1);
  CHECK(filter_polled() == all_polled);
  link_disconnect(&L, now);

  fresh();   // 'a Commander that only ACKs backs off like a silent one'
  K.can = false; K.ack_hello = true;
  link_connect(&L, now);
  bool backed = false;
  for (int t = 0; t < 21000; t += 20) {
    run(20);
    backed |= L.state == LINK_RETRYING && L.until - now == 3000;   // 'retrying in 3s', as it is said
  }
  CHECK(backed);
  CHECK(L.attempt >= 2);
  link_disconnect(&L, now);

  fresh();   // 'a list lost while the wheel runs: speed comes back by itself'
  link_connect(&L, now);
  run(2500);
  subs_mark = L.subscribes;
  lost = 0;
  K.drop = drop_resub_list;
  K.foreign = 4;
  run(2400);
  K.foreign = 0;
  CHECK(lost == 1);
  run(6000);
  CHECK(filter_has_always());
  int was = K.speed_frames;
  run(500);
  CHECK(K.speed_frames > was);
  CHECK(C.wifi_live);
  CHECK(L.resubscribes >= 1);
  link_disconnect(&L, now);

  fresh();   // 'ACKs over a silent stream do not pass for a live link'
  K.ack_hello = true;
  link_connect(&L, now);
  run(2500);
  CHECK(C.wifi_live);
  K.can = false;
  run(4500);
  CHECK(!C.wifi_live);
  link_disconnect(&L, now);

  fresh();   // 'polled ids alone cannot hold off the reconnect'
  link_connect(&L, now);
  run(2500);
  static uint16_t polled_ids[32];
  int np = 0;
  for (int g = 0; g < kGroupCount; g++) for (int i = 0; i < kSparse[g].n; i++) polled_ids[np++] = kSparse[g].ids[i];
  K.only = polled_ids; K.n_only = np;
  run(14000);
  CHECK(K.opens > 1);
  link_disconnect(&L, now);

  fresh();   // an ACK alone answers a hello (Panda._onEvent); 3 s on with no frame it is quiet
  K.can = false;
  link_connect(&L, now);
  run(40);
  CHECK(L.state == LINK_LIVE && C.wifi_live && L.acks == 1);
  run(3000);
  CHECK(L.state == LINK_QUIET && !C.wifi_live);
  link_disconnect(&L, now);

  fresh();   // 'a Commander that ACKs commands cannot start a subscribe storm'
  K.can = false; K.ack_commands = true;
  link_connect(&L, now);
  run(4000);
  CHECK(L.subscribes < 8);
  link_disconnect(&L, now);

  fresh();   // 'frozen with old frames waiting: a new stream, through the step aside'
  link_connect(&L, now);
  run(2500);
  CHECK(K.opens == 1);
  K.session = false;   // it dropped us while we were frozen
  now += 9000;         // frozen 9 s: no tick ran
  uint8_t old[16];
  rec(old, 0x257, 0, (const uint8_t[8]){0}, 8);
  for (int i = 0; i < 20; i++) link_on_datagram(&L, old, 16, now);
  run(1500);
  CHECK(K.opens == 2);
  CHECK(K.session);
  link_disconnect(&L, now);

  // the settle (main.dart): Bluetooth stepping aside holds the open back, without a backoff step
  fresh();
  L.port.settle = k_settle;
  K.settle_until = now + 700;
  link_connect(&L, now);
  CHECK(K.opens == 0 && L.state == LINK_RETRYING && L.attempt == 0);
  run(500);
  CHECK(K.opens == 0);   // still within the settle
  run(400);
  CHECK(K.opens == 1 && K.session);   // then the hello, at once
  link_disconnect(&L, now);

  fresh();   // link_kick: a pending retry goes now (Bluetooth came ready); nothing pending, nothing happens
  K.can = false;
  link_connect(&L, now);
  link_kick(&L, now);
  CHECK(K.opens == 1);   // saying hello: not a retry
  run(8100);
  CHECK(L.state == LINK_RETRYING && K.opens == 1);
  link_kick(&L, now);
  CHECK(K.opens == 2 && L.state == LINK_HELLO && L.retry_at == NEVER);
  link_disconnect(&L, now);
  link_kick(&L, now);
  CHECK(K.opens == 2 && L.state == LINK_IDLE);

  fresh();   // backoff: 1, 3, 6, 10, 10 s while nothing answers; the stream resets it
  K.can = false;
  link_connect(&L, now);
  int64_t waits[6];
  int nw = 0;
  link_state_t last = L.state;
  for (int t = 0; t < 90000 && nw < 6; t += 20) {
    run(20);
    if (L.state == LINK_RETRYING && last != LINK_RETRYING) waits[nw++] = L.until - now;
    last = L.state;
  }
  CHECK(nw == 6 && waits[0] == 1000 && waits[1] == 3000 && waits[2] == 6000 && waits[3] == 10000 && waits[4] == 10000 && waits[5] == 10000);   // each after 8 s of silence
  K.can = true;
  run(12000);
  CHECK(C.wifi_live && L.attempt == 0);
  link_disconnect(&L, now);

  fresh();   // quiet: 3 s of silence resubscribes and drops live; the stream back is live again
  link_connect(&L, now);
  run(2500);
  K.can = false;
  int subs = L.subscribes;
  run(3600);   // silent from 2.5 s; the tick at 6 s sees 3.5 s of it
  CHECK(L.state == LINK_QUIET && !C.wifi_live && L.subscribes > subs);
  K.can = true;
  run(200);
  CHECK(L.state == LINK_LIVE && C.wifi_live);
  link_disconnect(&L, now);
}

int main(void) {
  test_stale_and_sources();
  test_flasher();
  test_arrival();
  test_bt_stands_in();
  test_capture();
  test_buckles_fans();
  test_g();
  test_held();
  test_tables();
  test_trying();
  test_motors();
  test_bt_motors();
  test_steer_brake();
  test_parked();
  test_utc();
  test_brightness();
  test_foreign();
  test_sparse();
  test_subscribe_list();
  test_link();
  printf("test_derive: %d/%d checks pass\n", pass, pass + fail);
  return fail != 0;
}
