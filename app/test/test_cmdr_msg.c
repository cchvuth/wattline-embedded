// pb + cmdr_msg against the hex vectors of test/commander_test.dart (wattline 1.3.0).
#include "../core/cmdr_msg.h"
#include "../core/pb.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int checks;
#define CHECK(x) (assert(x), checks++)

static size_t hex(const char *s, uint8_t *out) {
  size_t n = 0;
  for (; s[2 * n]; n++) sscanf(s + 2 * n, "%2hhx", &out[n]);
  return n;
}

static int same(const uint8_t *a, size_t an, const char *want) {
  uint8_t w[CMDR_MSG_MAX];
  size_t wn = hex(want, w);
  return an == wn && !memcmp(a, w, wn);
}

int main(void) {
  uint8_t m[CMDR_MSG_MAX], b[CMDR_MSG_MAX];
  size_t n;

  // TryAction: switch type 1, state 0, or 2/3/4 for recirculation on/fresh/toggle.
  static const struct { cmdr_action a; const char *hex; } ta[] = {
      {ACT_FRUNK, "081cba02090a07080210011a0100"},         {ACT_MIRROR_TILT, "081cba02090a07081110011a0100"},
      {ACT_RECIRC_ON, "081cba02090a07083010011a0102"},     {ACT_RECIRC_OFF, "081cba02090a07083010011a0103"},
      {ACT_RECIRC_TOGGLE, "081cba02090a07083010011a0104"}, {ACT_CLIMATE, "081cba02090a07084610011a0100"},
      {ACT_DOOR_FL, "081cba02090a07082810011a0101"},       {ACT_DOOR_RR, "081cba02090a07082810011a0104"},
      {ACT_UNLOCK, "081cba02090a07083510011a0101"},        {ACT_FLASH, "081cba02090a07082110011a0106"},
  };
  for (size_t i = 0; i < sizeof ta / sizeof *ta; i++) CHECK(same(m, cmdr_try_action(m, ta[i].a), ta[i].hex));
  CHECK(cmdr_try_action(m, ACT_COUNT) == 0);
  for (int a = 0; a < ACT_COUNT; a++) { // every action builds a request the allow-list passes
    uint64_t t;
    CHECK((n = cmdr_try_action(m, a)) && cmdr_req_ok(m, n, &t) && t == 28);
  }

  // SendUUID: "08 20 da02 28 0a26" then the 38-character id (niklaswa's live capture).
  const char *id = "{260ac2ce-a519-4f4f-b299-215c5c4b217a}";
  n = cmdr_send_uuid(m, id);
  CHECK(n == 7 + 38 && same(m, 7, "0820da02280a26") && !memcmp(m + 7, id, 38));
  CHECK(same(m, cmdr_subscribe(m, (const uint8_t[]){61}, 1), "087cba08036a013d"));
  CHECK(same(m, cmdr_subscribe(m, (const uint8_t[]){6, 7, 8, 9}, 4), "087cba08066a0406070809"));
  CHECK(same(m, cmdr_api_status_req(m), "0852ea0500"));   // the bytes commander.dart sends
  CHECK(same(m, cmdr_trigger_push_req(m), "0830da0300"));
  n = cmdr_subscribe(m, cmdr_all_fields, sizeof cmdr_all_fields);
  CHECK(n == 7 + 60 && m[4] == 62 && m[6] == 60);   // 48 one-byte fields, 128..159 two bytes each
  CHECK(same(m, cmdr_subscribe(m, (const uint8_t[]){159}, 1), "087cba08046a029f01"));

  // Reply status.
  uint64_t type, st;
  n = hex("0821e202080804100818062003", b);
  CHECK(cmdr_reply_status(b, n, &type, &st) == 0 && type == 33 && st == 4);
  n = hex("081d", b);
  CHECK(cmdr_reply_status(b, n, &type, &st) == 0 && type == 29 && st == 0);
  n = hex("081dc202020800", b);
  CHECK(cmdr_reply_status(b, n, &type, &st) == 0 && type == 29 && st == 0);
  n = hex("081dc20205", b); // truncated payload: malformed
  CHECK(cmdr_reply_status(b, n, &type, &st) == -1);
  n = hex("0a00", b);       // type as bytes: Dart's cast throws
  CHECK(cmdr_reply_status(b, n, &type, &st) == -1);

  // Vehicle-data pushes: trip figures.
  cmdr_push p;
  n = hex("0810ca0123321245696666656c20546f7765722050617269733d00004841450000be424d00009842", b);
  CHECK(cmdr_push_parse(b, n, &p) == 16);
  CHECK(p.has_dest && !strcmp(p.dest, "Eiffel Tower Paris") && p.miles == 12.5f && p.minutes == 95.0f &&
        p.arrive_pct == 76.0f && !p.has_recirc && p.data_len == 0x23);
  n = hex("0810ca010c32003d000000004500000000", b); // explicit empty name and zeros; 9 not sent
  CHECK(cmdr_push_parse(b, n, &p) == 16 && p.has_dest && p.dest[0] == 0 && p.has_miles && p.miles == 0 &&
        p.has_minutes && !p.has_arrive);
  n = hex("0810ca0103a80601", b); // field 101: DashRecirc RECIRC
  CHECK(cmdr_push_parse(b, n, &p) == 16 && p.has_recirc && p.recirc == 1);
  n = hex("0810ca0105", b);       // truncated: unreadable, ignored
  CHECK(cmdr_push_parse(b, n, &p) == 0);

  // PushDeviceConnectionState (push 8, payload 10).
  static const struct { const char *hex; uint64_t dev; int on; } dc[] = {
      {"0808520a0a06aabbccddeeff3040", 64, 1},     // connected: state 0 left out
      {"0808520a0a06aabbccddeeff3001", 1, 1},      // a phone
      {"0808520c0a06aabbccddeeff10023040", 64, 0}, // disconnected (2)
      {"0808520c0a06aabbccddeeff10013040", 64, -1},// connecting (1): no change
      {"0808520c0a06aabbccddeeff10083040", 64, 1}, // already connected (8)
  };
  for (size_t i = 0; i < sizeof dc / sizeof *dc; i++) {
    n = hex(dc[i].hex, b);
    CHECK(cmdr_push_parse(b, n, &p) == 8 && p.dev_type == dc[i].dev && p.dev_on == dc[i].on);
  }

  // RespGetTeslaAPIStatus in words.
  char w[96];
  n = hex("08031a04080410023002", b);
  cmdr_api_words(b, n, w, sizeof w);
  CHECK(!strcmp(w, "car link connected, infotainment ok, paired"));
  cmdr_api_words(b, 0, w, sizeof w);
  CHECK(!strcmp(w, "car link initial, infotainment \xe2\x80\x94, not paired"));
  n = hex("080110021a0208031a0208042805", b);
  cmdr_api_words(b, n, w, sizeof w);
  CHECK(!strcmp(w, "car link partly configured (not fully configured), infotainment initial, not paired"));

  // The allow-list: exactly 28, 32, 48, 82, 124 out of 0..255.
  for (unsigned t = 0; t < 256; t++) {
    int want = t == 28 || t == 32 || t == 48 || t == 82 || t == 124;
    CHECK(!!cmdr_payload_field(t) == want);
    pb_out o = {m, 0, sizeof m, 0};
    pb_u(&o, 1, t);
    pb_b(&o, want ? cmdr_payload_field(t) : 2, (const uint8_t *)"", 0);
    uint64_t got;
    CHECK(cmdr_req_ok(m, o.n, &got) == want);
  }
  // Shape: the payload must be the type's own, alone; a second type or field is refused.
  CHECK(!cmdr_req_ok((const uint8_t[]){0x08, 0x1c, 0xba, 0x08, 0x00}, 5, &type));        // 28 with Subscribe's payload
  CHECK(!cmdr_req_ok((const uint8_t[]){0x08, 0x1c, 0xba, 0x02, 0x00, 0x10, 0x01}, 7, &type)); // extra field
  CHECK(!cmdr_req_ok((const uint8_t[]){0x08, 0x1c, 0x08, 0x54, 0xba, 0x02, 0x00}, 7, &type)); // type twice (28 then 84)
  CHECK(!cmdr_req_ok((const uint8_t[]){0x08, 0x1c}, 2, &type));                          // no payload
  CHECK(!cmdr_req_ok((const uint8_t[]){0x08, 0x9c, 0x80, 0x80, 0x80, 0x80, 0x01, 0xba, 0x02, 0x00}, 10, &type)); // 28 + 2^35

  // pb edges.
  CHECK(pb_ok((const uint8_t[]){0x09, 1, 2, 3, 4, 5, 6, 7, 8}, 9));  // fixed64 skipped
  CHECK(!pb_ok((const uint8_t[]){0x09, 1, 2, 3}, 4));
  CHECK(!pb_ok((const uint8_t[]){0x0b}, 1));                          // group: unsupported
  CHECK(!pb_ok((const uint8_t[]){0x08, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01}, 12));
  pb_val v;
  CHECK(pb_get((const uint8_t[]){0x08, 1, 0x08, 2}, 4, 1, &v) == 1 && v.v == 2); // last one wins
  pb_out small = {m, 0, 3, 0};
  pb_b(&small, 1, (const uint8_t *)"abcd", 4);
  CHECK(small.bad);

  printf("test_cmdr_msg: %d checks pass\n", checks);
  return 0;
}
