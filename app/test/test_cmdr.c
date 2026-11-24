// The Commander session state machine against a simulated Commander (fake
// transport) and a fake clock. The simulated side has its own CTR code, so the
// stream is checked from outside sec1.c.
#include "../core/cmdr.h"
#include "../core/pb.h"
#include "../third_party/aes.h"
#include "../third_party/monocypher.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int checks;
#define CHECK(x) (assert(x), checks++)

static const char *ID_PATH = "build/test_cmdr_id", *PIN_PATH = "build/test_cmdr_pin";
static const uint8_t POP[32] = {0x59, 0xd2, 0x41, 0x4e, 0xee, 0xe3, 0x57, 0xfe, 0x14, 0x2d, 0xf7,
                                0x26, 0x78, 0xf2, 0x6b, 0xb3, 0xbf, 0x3d, 0x06, 0x03, 0xc7, 0x1e,
                                0xdb, 0xae, 0xd7, 0x50, 0x3e, 0xb7, 0x95, 0x0e, 0x8e, 0x1c};

// ---- the simulated Commander ----
typedef struct { struct AES_ctx aes; uint8_t ctr[16], ks[16]; unsigned off; } ctr_t;
static void ctr_xor(ctr_t *c, uint8_t *b, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if (c->off == 16) {
      memcpy(c->ks, c->ctr, 16);
      AES_ECB_encrypt(&c->aes, c->ks);
      for (int k = 15; k >= 0 && ++c->ctr[k] == 0; k--) {}
      c->off = 0;
    }
    b[i] ^= c->ks[c->off++];
  }
}

static struct {
  // set by the tests
  int adapter, advertising, has, echo_flag;  // echo_flag -1: none, 0: refuse flag 1
  int approved;                              // SendUUID says 0, else 4
  int wrong_type_for;                        // reply to this request type with a wrong type
  // seen
  int scans, connects, disconnects, notifies, writes, types[256], last_dev_type;
  uint8_t last_req[CMDR_MSG_MAX];
  size_t last_req_n, last_sub_fields;
  char last_pin[CMDR_ADDR_MAX];
  int scan_had_pin;
  // device state
  int linked;
  uint8_t secret[32], pub[32], client[32], rnd[16];
  ctr_t ctr;
} dev;

static void dev_reset(void) {
  memset(&dev, 0, sizeof dev);
  dev.adapter = dev.advertising = 1;
  dev.has = CMDR_HAS_SESSION | CMDR_HAS_API | CMDR_HAS_PUSH;
  dev.echo_flag = -1;
  dev.approved = 1;
  dev.wrong_type_for = -1;
  for (int i = 0; i < 32; i++) dev.secret[i] = (uint8_t)(200 - i);
  for (int i = 0; i < 16; i++) dev.rnd[i] = (uint8_t)(0xf0 + i);
  crypto_x25519_public_key(dev.pub, dev.secret);
}

static int f_adapter(void *x) { (void)x; return dev.adapter; }
static int f_scan(void *x, const char *pin, char addr[CMDR_ADDR_MAX]) {
  (void)x;
  dev.scans++;
  dev.scan_had_pin = pin != NULL;
  if (pin) snprintf(dev.last_pin, sizeof dev.last_pin, "%s", pin);
  if (!dev.advertising || (pin && strcmp(pin, "AA:BB:CC:00:00:01"))) return -1;
  strcpy(addr, "AA:BB:CC:00:00:01");
  return 0;
}
static int f_connect(void *x, const char *a) { (void)x; (void)a; dev.connects++; dev.linked = 1; return dev.has; }
static void f_disconnect(void *x) { (void)x; dev.disconnects++; dev.linked = 0; }
static int f_notify(void *x, int on) { (void)x; dev.notifies += on; return 0; }
static int f_random(void *x, uint8_t *b, size_t n) { (void)x; for (size_t i = 0; i < n; i++) b[i] = (uint8_t)(i * 7 + 1); return 0; }

static size_t ld(uint8_t *o, uint32_t field, const uint8_t *v, size_t n) {
  pb_out p = {o, 0, 256, 0};
  pb_b(&p, field, v, n);
  return p.n;
}

static int f_rpc(void *x, int chr, const uint8_t *w, size_t wn, uint8_t *r, size_t cap, size_t *rn) {
  (void)x;
  assert(dev.linked && cap >= 256 && wn <= CMDR_MSG_MAX);
  dev.writes++;
  const uint8_t *p, *b, *v;
  size_t pn, bn, vn;
  uint64_t msg, t;
  pb_out o = {r, 0, cap, 0};
  if (chr == CMDR_CHR_SESSION) {
    assert(pb_get_b(w, wn, 11, &p, &pn) && pb_get_u(p, pn, 1, &msg));
    uint8_t body[128], inner[160];
    size_t n = 0;
    if (msg == 0) { // SessionCmd0 -> SessionResp0
      assert(pb_get_b(p, pn, 20, &b, &bn) && pb_get_b(b, bn, 1, &v, &vn) && vn == 32);
      memcpy(dev.client, v, 32);
      uint8_t shared[32], key[32];
      crypto_x25519(shared, dev.secret, dev.client);
      for (int i = 0; i < 32; i++) key[i] = shared[i] ^ POP[i];
      AES_init_ctx(&dev.ctr.aes, key);
      memcpy(dev.ctr.ctr, dev.rnd, 16);
      dev.ctr.off = 16;
      body[n++] = 0x08, body[n++] = 0x00;
      n += ld(body + n, 2, dev.pub, 32);
      n += ld(body + n, 3, dev.rnd, 16);
      if (dev.echo_flag >= 0) body[n++] = 0x20, body[n++] = (uint8_t)dev.echo_flag;
      size_t k = 0;
      inner[k++] = 0x08, inner[k++] = 0x01;
      k += ld(inner + k, 21, body, n);
      pb_u(&o, 2, 1);
      pb_b(&o, 11, inner, k);
    } else { // SessionCmd1 -> SessionResp1
      assert(msg == 2 && pb_get_b(p, pn, 22, &b, &bn) && pb_get_b(b, bn, 2, &v, &vn) && vn == 32);
      assert(pb_get_u(b, bn, 3, &t));
      dev.last_dev_type = (int)t;
      uint8_t mine[32];
      memcpy(mine, v, 32);
      ctr_xor(&dev.ctr, mine, 32);
      assert(!memcmp(mine, dev.pub, 32)); // the phone used the same stream
      uint8_t enc[32];
      memcpy(enc, dev.client, 32);
      ctr_xor(&dev.ctr, enc, 32);
      n = ld(body, 3, enc, 32);
      size_t k = 0;
      inner[k++] = 0x08, inner[k++] = 0x03;
      k += ld(inner + k, 23, body, n);
      pb_u(&o, 2, 1);
      pb_b(&o, 11, inner, k);
    }
    *rn = o.n;
    return 0;
  }
  assert(chr == CMDR_CHR_API);
  memcpy(dev.last_req, w, wn);
  dev.last_req_n = wn;
  ctr_xor(&dev.ctr, dev.last_req, wn);
  assert(pb_get_u(dev.last_req, wn, 1, &t) && t < 256);
  dev.types[t]++;
  if (t == 124 && pb_get_b(dev.last_req, wn, 135, &b, &bn) && pb_get_b(b, bn, 13, &v, &vn)) dev.last_sub_fields = vn;
  uint8_t body[32];
  size_t n = 0;
  if (t == 32 && !dev.approved) body[n++] = 0x08, body[n++] = 4;
  pb_u(&o, 1, (int)t == dev.wrong_type_for ? t + 7 : t + 1);
  if (t == 82) {
    uint8_t api[] = {0x08, 0x03, 0x1a, 0x04, 0x08, 0x04, 0x10, 0x02, 0x30, 0x02};
    pb_b(&o, 94, api, sizeof api);
  } else {
    pb_b(&o, 2, body, n);
  }
  ctr_xor(&dev.ctr, r, o.n);
  *rn = o.n;
  return 0;
}

static const cmdr_io io = {0, f_adapter, f_scan, f_connect, f_rpc, f_notify, f_disconnect, f_random, NULL};

static struct { int calls, nulls; size_t last; } data;
static void on_data(void *x, const uint8_t *d, size_t n) { (void)x; data.calls++; data.nulls += !d; data.last = n; }
static int said[8], nsaid;   // the states published from inside a dial (on_state)
static void on_state(void *x) { if (nsaid < 8) said[nsaid++] = ((cmdr_t *)x)->state; }

static int64_t now;
static void run(cmdr_t *c, int64_t ms) { // the main loop, 50 ms steps
  for (int64_t end = now + ms; now < end;) now += 50, cmdr_tick(c, now);
}

static void fresh(cmdr_t *c, int wifi_first) {
  dev_reset();
  remove(ID_PATH);
  remove(PIN_PATH);
  memset(&data, 0, sizeof data);
  cmdr_cfg cfg = cmdr_cfg_default;
  cfg.id_path = ID_PATH;
  cfg.pin_path = PIN_PATH;
  cfg.wifi_first = wifi_first;
  cfg.on_data = on_data;
  cfg.on_state = on_state;
  cfg.data_ctx = c;
  nsaid = 0;
  now = 1000;
  cmdr_init(c, &cfg, &io);
}

static int file_is(const char *path, const char *want) {
  char s[64] = "";
  FILE *f = fopen(path, "r");
  if (!f) return want == NULL;
  int ok = fgets(s, sizeof s, f) && want && !strncmp(s, want, strlen(want));
  fclose(f);
  return ok;
}

int main(void) {
  cmdr_t c;
  int sent;

  // --- A first link (approved already), no WiFi: all the car data. ---
  fresh(&c, 0);
  cmdr_connect(&c, now);
  CHECK(c.state == CS_READY && c.level == CMDR_FULL);
  CHECK(nsaid == 2 && said[0] == CS_SCANNING && said[1] == CS_LINKING);   // said before the scan and the connect block
  CHECK(dev.last_dev_type == 1 && !dev.scan_had_pin);
  CHECK(dev.types[32] == 1 && dev.types[124] == 1 && dev.types[48] == 1 && dev.types[82] == 1);
  CHECK(dev.last_sub_fields == 64 && dev.notifies == 1);   // the packed field list: 48 one-byte numbers, 8 two-byte
  CHECK(!strcmp(c.car_link, "car link connected, infotainment ok, paired"));
  CHECK(file_is(PIN_PATH, "AA:BB:CC:00:00:01"));
  char id[64];
  FILE *f = fopen(ID_PATH, "r");
  CHECK(f && fgets(id, sizeof id, f) && strlen(id) == 39 && id[0] == '{' && id[15] == '4' && strchr("89ab", id[20]));
  fclose(f);
  CHECK(!memcmp(c.id, id, 38));
  // The id stays: the next session sends the same one, and scans for the pin.
  cmdr_link_lost(&c, now);
  CHECK(c.state == CS_RETRYING && cmdr_until(&c) == now + 2000 && data.nulls >= 1);
  run(&c, 2000);
  CHECK(c.state == CS_READY && !memcmp(c.id, id, 38) && dev.scan_had_pin && !strcmp(dev.last_pin, "AA:BB:CC:00:00:01"));

  // --- Pushes: trip figures, then dropped with the link. ---
  uint8_t push[] = {0x08, 0x10, 0xca, 0x01, 0x0d, 0x32, 0x03, 'B', 'a', 'r', 0x3d, 0, 0, 0x48, 0x41, 0xa8, 0x06, 0x02};
  int calls = data.calls;
  cmdr_on_push(&c, push, sizeof push);
  CHECK(c.has_dest && !strcmp(c.dest, "Bar") && c.has_miles && c.miles == 12.5f && c.recirc == 2);
  CHECK(data.calls == calls + 1 && data.last == 13 && c.push_count == 1);
  cmdr_on_push(&c, push, 5); // unreadable: ignored, link kept
  CHECK(c.state == CS_READY && data.calls == calls + 1);

  // --- Presses and their guards. ---
  sent = dev.types[28];
  CHECK(cmdr_press(&c, ACT_FRUNK, 1, 'P', now) == CMDR_OK && dev.types[28] == sent + 1);
  uint8_t frunk[CMDR_MSG_MAX];
  size_t fn = cmdr_try_action(frunk, ACT_FRUNK);
  CHECK(dev.last_req_n == fn && !memcmp(dev.last_req, frunk, fn));
  static const cmdr_action opens[] = {ACT_FRUNK, ACT_TRUNK, ACT_DOOR_FL, ACT_DOOR_FR, ACT_DOOR_RL, ACT_DOOR_RR};
  for (size_t i = 0; i < 6; i++) {
    CHECK(cmdr_press(&c, opens[i], 0, 'D', now) == CMDR_E_PARK);
    CHECK(cmdr_press(&c, opens[i], 0, 0, now) == CMDR_E_PARK); // gear unknown: no
    CHECK(cmdr_press(&c, opens[i], 0, 'P', now) == CMDR_E_PARK); // a stale P is not parked
  }
  CHECK(cmdr_press(&c, ACT_MIRROR_TILT, 1, 'P', now) == CMDR_E_GEAR);
  CHECK(cmdr_press(&c, ACT_MIRROR_TILT, 0, 'N', now) == CMDR_E_GEAR);
  CHECK(cmdr_press(&c, ACT_MIRROR_TILT, 0, 'D', now) == CMDR_E_GEAR);
  CHECK(dev.types[28] == sent + 1); // none of those wrote anything
  CHECK(cmdr_press(&c, ACT_MIRROR_TILT, 0, 'R', now) == CMDR_OK);
  CHECK(cmdr_press(&c, ACT_HONK, 0, 'D', now) == CMDR_OK);
  CHECK(dev.types[28] == sent + 3);

  // --- The allow-list in the one send function: every type 0..255. ---
  for (unsigned t = 0; t < 256; t++) {
    int want = t == 28 || t == 32 || t == 48 || t == 82 || t == 124;
    uint8_t m[16], r[CMDR_MSG_MAX];
    size_t rn;
    pb_out o = {m, 0, sizeof m, 0};
    pb_u(&o, 1, t);
    pb_b(&o, want ? cmdr_payload_field(t) : 2, (const uint8_t *)"", 0);
    int writes = dev.writes;
    int e = cmdr_send(&c, CMDR_CHR_API, m, o.n, r, sizeof r, &rn);
    CHECK(want ? e == 0 && dev.writes == writes + 1 : e == CMDR_E_DENIED && dev.writes == writes);
  }
  CHECK(c.state == CS_READY); // refusals cost nothing: the stream never moved
  {
    uint8_t r[CMDR_MSG_MAX], m[CMDR_MSG_MAX];
    size_t rn, n = sec1_hello(&c.sec, m, sizeof m);
    int writes = dev.writes;
    CHECK(cmdr_send(&c, CMDR_CHR_SESSION, m, n, r, sizeof r, &rn) == CMDR_E_DENIED); // no handshake once confirmed
    CHECK(cmdr_send(&c, CMDR_CHR_PUSH, frunk, fn, r, sizeof r, &rn) == CMDR_E_DENIED); // never a write to push-enhapi
    CHECK(dev.writes == writes);
    cmdr_t x; // before a session: enhapi refused, prov-session only with device type 1
    cmdr_init(&x, &c.cfg, &io);
    CHECK(cmdr_send(&x, CMDR_CHR_API, frunk, fn, r, sizeof r, &rn) == CMDR_E_DENIED);
    uint8_t cmd1[] = {0x10, 0x01, 0x5a, 0x29, 0x08, 0x02, 0xb2, 0x01, 0x24, 0x12, 0x20, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10,
                      11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 0x18, 64};
    CHECK(cmdr_send(&x, CMDR_CHR_SESSION, cmd1, sizeof cmd1, r, sizeof r, &rn) == CMDR_E_DENIED);
    CHECK(dev.writes == writes);
  }

  // --- A reply of the wrong type tears the session down; the press is never resent. ---
  dev.wrong_type_for = 28;
  sent = dev.types[28];
  int disc = dev.disconnects;
  CHECK(cmdr_press(&c, ACT_UNLOCK, 0, 'D', now) == CMDR_E_NO_ANSWER);
  CHECK(c.state == CS_RETRYING && dev.disconnects == disc + 1 && !c.secure && !c.has_dest);
  run(&c, 60000);
  CHECK(c.state == CS_READY && dev.types[28] == sent + 1); // back, and the unlock went once
  dev.wrong_type_for = 124; // out of step on the subscribe too (the phone app skips this check)
  cmdr_link_lost(&c, now);
  run(&c, 2000);
  CHECK(c.state == CS_RETRYING && c.attempt == 2 && c.retry_at == now + 5000);
  dev.wrong_type_for = -1;

  // --- Backoff 2, 5, 10, 20, 30, 30 s while no Commander advertises. ---
  fresh(&c, 0);
  dev.advertising = 0;
  cmdr_connect(&c, now);
  static const int backoff[] = {2, 5, 10, 20, 30, 30};
  for (int i = 0; i < 6; i++) {
    CHECK(c.state == CS_AWAY && c.retry_at == now + backoff[i] * 1000);
    int scans = dev.scans;
    run(&c, backoff[i] * 1000 - 50);
    CHECK(dev.scans == scans);
    run(&c, 50);
    CHECK(dev.scans == scans + 1);
  }
  dev.advertising = 1;
  cmdr_kick(&c, 0, now); // the WiFi link found it awake: no waiting out the backoff
  CHECK(c.state == CS_READY && c.attempt == 0);

  // --- Flag 1 cleared: no subscribe, on any path. ---
  fresh(&c, 0);
  dev.echo_flag = 0;
  cmdr_connect(&c, now);
  CHECK(c.state == CS_READY && !c.sec.pushes_clear && dev.notifies == 0 && dev.types[124] == 0 && dev.types[48] == 0);
  CHECK(dev.types[82] == 1 && strstr(c.push_info, "encrypted"));
  cmdr_on_push(&c, push, sizeof push); // not listening
  CHECK(!c.has_dest && c.push_count == 0);
  cmdr_wifi(&c, 1, 0, now);
  run(&c, 5000);
  cmdr_wifi(&c, 0, 0, now);
  run(&c, 60000);
  CHECK(dev.types[124] == 0 && dev.notifies == 0);
  dev.echo_flag = 1; // echoed: subscribes
  cmdr_link_lost(&c, now);
  run(&c, 2000);
  CHECK(c.state == CS_READY && dev.types[124] == 1);

  // --- A refused link parks and waits for a tap; the tap approves. ---
  fresh(&c, 0);
  dev.approved = 0;
  cmdr_connect(&c, now);
  CHECK(c.state == CS_REFUSED && c.parked && !dev.linked && dev.types[32] == 1);
  CHECK(file_is(PIN_PATH, NULL)); // not pinned
  int scans = dev.scans;
  run(&c, 10 * 60000);
  CHECK(c.state == CS_REFUSED && dev.scans == scans && dev.types[32] == 1); // never on its own
  cmdr_kick(&c, 0, now); // a kick that is not a tap changes nothing
  cmdr_wifi(&c, 1, 0, now);
  run(&c, 5000);
  CHECK(c.state == CS_REFUSED && dev.scans == scans);
  cmdr_kick(&c, 1, now); // the owner's tap
  CHECK(c.state == CS_APPROVING && dev.linked && dev.types[32] == 2 && cmdr_until(&c) == now + 60000);
  run(&c, 1950);
  CHECK(dev.types[32] == 2);
  run(&c, 50);
  CHECK(dev.types[32] == 3); // asked again every 2 s
  run(&c, 4000);
  dev.approved = 1; // the double-press
  run(&c, 2000);
  CHECK(c.state == CS_READY && !c.parked && file_is(PIN_PATH, "AA:BB:CC:00:00:01"));
  CHECK(dev.types[124] == 1); // the session carries on from the approval

  // Approval asked for a minute, never given: parked again, nothing more on its own.
  fresh(&c, 0);
  dev.approved = 0;
  cmdr_connect(&c, now);
  cmdr_kick(&c, 1, now);
  CHECK(c.state == CS_APPROVING);
  run(&c, 61000);
  CHECK(c.state == CS_REFUSED && !dev.linked);
  sent = dev.types[32];
  scans = dev.scans;
  run(&c, 10 * 60000);
  CHECK(dev.types[32] == sent && dev.scans == scans);
  // Pinned and then refused (the owner removed it in the S3XY app): parks too.
  fresh(&c, 0);
  cmdr_connect(&c, now);
  dev.approved = 0;
  cmdr_link_lost(&c, now);
  run(&c, 2000);
  CHECK(c.state == CS_REFUSED && file_is(PIN_PATH, "AA:BB:CC:00:00:01"));
  // A press while refused is the tap: answered at once, the approval dialled at the next tick.
  CHECK(cmdr_press(&c, ACT_HONK, 0, 'D', now) == CMDR_E_NOT_READY);
  run(&c, 500);
  CHECK(c.state == CS_APPROVING);
  // A link lost while approving retries (not parked), and the tap's one retry still counts as asked.
  cmdr_link_lost(&c, now);
  CHECK(c.state == CS_RETRYING);
  run(&c, 2000);
  CHECK(c.state == CS_APPROVING);
  // ... that one only: lost again, the next dial asks nothing (no prompt at every dial after one tap)
  cmdr_link_lost(&c, now);
  for (int i = 0; i < 30 && c.state == CS_RETRYING; i++) run(&c, 1000);
  CHECK(c.state == CS_REFUSED);
  cmdr_forget(&c, now);
  CHECK(file_is(PIN_PATH, NULL));

  // --- Step-aside, buttons first, then lean, full after WiFi goes, lean again by redial. ---
  fresh(&c, 1); // wifi_first: Bluetooth waits for the stream
  cmdr_connect(&c, now);
  CHECK(c.state == CS_PAUSED && dev.scans == 0);
  run(&c, 5000);
  CHECK(c.state == CS_PAUSED && dev.scans == 0);
  CHECK(cmdr_step_aside(&c, now) == 0); // nothing was up: no settle
  cmdr_wifi(&c, 1, 0, now);
  int64_t live_at = now;
  CHECK(c.buttons_at == live_at + 2000 && c.data_at == live_at + 2000);
  run(&c, 1950);
  CHECK(dev.scans == 0);
  run(&c, 50);
  CHECK(c.state == CS_READY && c.level == CMDR_LEAN && dev.last_sub_fields == 9); // (5 one-byte, 130/131 two) the hold lifted on the same tick
  CHECK(dev.types[124] == 1 && dev.connects == 1);
  // A new stream: step aside (a link was up: settle), back with the buttons, data after.
  CHECK(cmdr_step_aside(&c, now) == 3000 && c.state == CS_PAUSED && !dev.linked);
  run(&c, 3000);
  cmdr_wifi(&c, 0, 1, now);
  cmdr_wifi(&c, 1, 0, now);
  run(&c, 2000);
  CHECK(c.state == CS_READY && c.level == CMDR_LEAN && dev.connects == 2);
  // WiFi goes: after 10 s the session that outlived it takes all, on the same link.
  for (int i = 0; i < 20; i++) cmdr_wifi(&c, 1, 0, now), run(&c, 500);
  cmdr_wifi(&c, 0, 0, now);
  run(&c, 9950);
  CHECK(c.level == CMDR_LEAN);
  run(&c, 50);
  CHECK(c.level == CMDR_FULL && dev.last_sub_fields == 64 && dev.connects == 2);
  // WiFi back: fewer fields only by dialling again.
  cmdr_wifi(&c, 1, 0, now);
  run(&c, 3000);
  CHECK(c.state == CS_READY && c.level == CMDR_LEAN && dev.connects == 3);

  // A fresh dial in the spell after WiFi went stays lean, then takes all once out.
  fresh(&c, 0);
  cmdr_wifi(&c, 1, 0, now);
  cmdr_connect(&c, now);
  CHECK(c.level == CMDR_LEAN);
  cmdr_wifi(&c, 0, 0, now);
  int64_t gone = now;
  cmdr_link_lost(&c, now); // the Commander restarting
  run(&c, 2000);
  CHECK(c.state == CS_READY && c.level == CMDR_LEAN && dev.connects == 2);
  run(&c, gone + 31000 - now - 50);
  CHECK(c.level == CMDR_LEAN);
  run(&c, 50);
  CHECK(c.level == CMDR_FULL && dev.connects == 2);

  // No stream after a step-aside (switch off): Bluetooth back after back_ms with data.
  fresh(&c, 0);
  cmdr_connect(&c, now);
  CHECK(cmdr_step_aside(&c, now) == 3000);
  run(&c, 14950);
  CHECK(c.state == CS_PAUSED);
  run(&c, 50);
  CHECK(c.state == CS_READY && c.level == CMDR_FULL && !c.data_hold);
  // Switch off: no dial while WiFi tries.
  cmdr_link_lost(&c, now);
  cmdr_wifi(&c, 0, 1, now);
  run(&c, 2000);
  CHECK(c.state == CS_PAUSED && !strcmp(c.why, "waiting for WiFi to try first"));
  cmdr_wifi(&c, 0, 0, now);
  run(&c, 250);
  CHECK(c.state == CS_READY);

  // --- The controller off: no retries; on again dials. ---
  cmdr_adapter(&c, 0, now);
  dev.adapter = 0;
  CHECK(c.state == CS_OFF && !dev.linked);
  scans = dev.scans;
  run(&c, 120000);
  CHECK(dev.scans == scans);
  dev.adapter = 1;
  cmdr_adapter(&c, 1, now);
  CHECK(c.state == CS_READY);

  // Every action has a guard verdict without a session too.
  for (int a = 0; a < ACT_COUNT; a++) {
    CHECK(cmdr_press_guard(a, 1, 'P') == (a == ACT_MIRROR_TILT ? CMDR_E_GEAR : 0));
    CHECK(cmdr_press_guard(a, 0, 'R') == (cmdr_action_opens(a) ? CMDR_E_PARK : 0));
  }

  // The owner's Approve tap while Bluetooth is stepped aside for WiFi (wifi_first): kept, and dialled with the ask
  // once WiFi is live again, so the approval comes up (it was dropped: the tap did nothing).
  fresh(&c, 1);
  dev.approved = 0;
  cmdr_connect(&c, now);
  cmdr_step_aside(&c, now), cmdr_wifi(&c, 1, 0, now), run(&c, 2500);
  CHECK(c.state == CS_REFUSED);           // the boot dial, never asked: parked
  cmdr_wifi(&c, 0, 1, now), cmdr_step_aside(&c, now), run(&c, 1000);
  cmdr_kick(&c, 1, now);                   // the tap, WiFi mid-try
  CHECK(c.state == CS_PAUSED && c.asks == 2);
  run(&c, 3000);
  cmdr_resume(&c, now), cmdr_wifi(&c, 1, 0, now), run(&c, 5000);
  CHECK(c.state == CS_APPROVING);

  // The tap while the Commander is not seen (a phone holds it): the misses do not use the ask up, so once it is seen
  // the approval is asked (it ended REFUSED with no prompt).
  fresh(&c, 1);
  dev.approved = 0;
  cmdr_connect(&c, now);
  cmdr_step_aside(&c, now), cmdr_wifi(&c, 1, 0, now), run(&c, 2500);
  dev.advertising = 0;
  cmdr_kick(&c, 1, now);
  for (int i = 0; i < 4; i++) cmdr_wifi(&c, 1, 0, now), run(&c, 1000);
  CHECK(c.state == CS_AWAY && c.asks == 2);
  dev.advertising = 1;
  for (int i = 0; i < 40 && c.state != CS_APPROVING; i++) cmdr_wifi(&c, 1, 0, now), run(&c, 1000);
  CHECK(c.state == CS_APPROVING);
  // and that tap, whose first dials missed, still has its one retry when the approval is cut short; then no more
  cmdr_link_lost(&c, now);
  for (int i = 0; i < 30 && c.state != CS_APPROVING; i++) cmdr_wifi(&c, 1, 0, now), run(&c, 1000);
  CHECK(c.state == CS_APPROVING && c.asks == 0);
  cmdr_link_lost(&c, now);
  for (int i = 0; i < 30 && c.state == CS_RETRYING; i++) cmdr_wifi(&c, 1, 0, now), run(&c, 1000);
  CHECK(c.state == CS_REFUSED);

  // Nor do dials that fail before the Commander answers (seen, the link not made): the tap stands, the approval asked
  // once one gets through.
  fresh(&c, 1);
  dev.approved = 0;
  cmdr_connect(&c, now);
  cmdr_step_aside(&c, now), cmdr_wifi(&c, 1, 0, now), run(&c, 2500);
  dev.has = 0;
  cmdr_kick(&c, 1, now);
  for (int i = 0; i < 8; i++) cmdr_wifi(&c, 1, 0, now), run(&c, 1000);
  CHECK(c.state != CS_APPROVING && c.asks == 2);
  dev.has = CMDR_HAS_SESSION | CMDR_HAS_API | CMDR_HAS_PUSH;
  for (int i = 0; i < 40 && c.state != CS_APPROVING; i++) cmdr_wifi(&c, 1, 0, now), run(&c, 1000);
  CHECK(c.state == CS_APPROVING);

  // ... but not for ever: seen only long after the tap (nobody at the unit any more), no prompt: refused, as a dial
  // nobody asked for.
  fresh(&c, 1);
  dev.approved = 0;
  cmdr_connect(&c, now);
  cmdr_step_aside(&c, now), cmdr_wifi(&c, 1, 0, now), run(&c, 2500);
  dev.advertising = 0;
  cmdr_kick(&c, 1, now);
  for (int i = 0; i < 400; i++) cmdr_wifi(&c, 1, 0, now), run(&c, 1000);
  CHECK(c.state == CS_AWAY);
  dev.advertising = 1;
  for (int i = 0; i < 60 && c.state == CS_AWAY; i++) cmdr_wifi(&c, 1, 0, now), run(&c, 1000);
  CHECK(c.state == CS_REFUSED && c.asks == 0);
  // and so a tap kept while stepped aside, WiFi letting go only long after it
  fresh(&c, 1);
  dev.approved = 0;
  cmdr_connect(&c, now);
  cmdr_step_aside(&c, now), cmdr_wifi(&c, 1, 0, now), run(&c, 2500);
  cmdr_wifi(&c, 0, 1, now), cmdr_step_aside(&c, now), run(&c, 1000);
  cmdr_kick(&c, 1, now);
  CHECK(c.state == CS_PAUSED && c.asks == 2);
  run(&c, 301000);
  cmdr_resume(&c, now), cmdr_wifi(&c, 1, 0, now), run(&c, 5000);
  CHECK(c.state == CS_REFUSED && c.asks == 0);
  // a second tap has its own time: 4 min not seen, tapped again, 4 min more, then seen: asked
  fresh(&c, 1);
  dev.approved = 0;
  cmdr_connect(&c, now);
  cmdr_step_aside(&c, now), cmdr_wifi(&c, 1, 0, now), run(&c, 2500);
  dev.advertising = 0;
  cmdr_kick(&c, 1, now);
  for (int i = 0; i < 240; i++) cmdr_wifi(&c, 1, 0, now), run(&c, 1000);
  cmdr_kick(&c, 1, now);
  for (int i = 0; i < 240; i++) cmdr_wifi(&c, 1, 0, now), run(&c, 1000);
  dev.advertising = 1;
  for (int i = 0; i < 60 && c.state != CS_APPROVING; i++) cmdr_wifi(&c, 1, 0, now), run(&c, 1000);
  CHECK(c.state == CS_APPROVING);

  // --- Stepped aside for WiFi's first try while it waits for WiFi; Android Auto then takes the radio before the
  // stream goes live (wifi_first off): back, not paused for the whole session. A ready link stepped aside alike. ---
  fresh(&c, 1);
  cmdr_connect(&c, now);
  cmdr_wifi(&c, 0, 1, now);
  run(&c, 1000);
  cmdr_step_aside(&c, now);
  run(&c, 1000);
  CHECK(c.paused);
  int scans0 = dev.scans;
  cmdr_wifi_first(&c, 0, now);
  for (int i = 0; i < 10; i++) cmdr_wifi(&c, 0, 0, now), run(&c, 1000);
  CHECK(!c.paused && dev.scans > scans0 && c.state == CS_READY);
  fresh(&c, 0);
  cmdr_connect(&c, now);
  run(&c, 100);
  cmdr_wifi_first(&c, 1, now);
  cmdr_step_aside(&c, now);
  cmdr_wifi_first(&c, 0, now);
  for (int i = 0; i < 10; i++) cmdr_wifi(&c, 0, 0, now), run(&c, 1000);
  CHECK(c.state == CS_READY);
  // Off and on again before the next tick (AA let go at once): still stepped aside, as it was.
  fresh(&c, 1);
  cmdr_connect(&c, now);
  cmdr_wifi(&c, 0, 1, now);
  run(&c, 1000);
  cmdr_step_aside(&c, now);
  run(&c, 1000);
  scans0 = dev.scans;
  cmdr_wifi_first(&c, 0, now);
  cmdr_wifi_first(&c, 1, now);
  for (int i = 0; i < 20; i++) cmdr_wifi(&c, 0, 0, now), run(&c, 1000);
  CHECK(c.paused && dev.scans == scans0);

  remove(ID_PATH);
  remove(PIN_PATH);
  printf("test_cmdr: %d checks pass\n", checks);
  return 0;
}
