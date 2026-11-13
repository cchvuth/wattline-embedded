// Framing and the listen-only gate. Vectors as test/obd_test.dart 'panda records unpack...'.
#include "../core/panda.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void rec(uint8_t *b, uint16_t id, const uint8_t *data, uint8_t len, uint8_t bus) {
  memset(b, 0, 16);
  uint32_t w0 = (uint32_t)id << 21, w1 = (uint32_t)bus << 4 | len;
  for (int i = 0; i < 4; i++) b[i] = w0 >> 8 * i, b[4 + i] = w1 >> 8 * i;
  memcpy(b + 8, data, len);
}

static struct { int n; uint16_t id[4]; uint8_t bus[4], len[4], d0[4]; } got;
static void on(void *c, uint16_t id, uint8_t bus, const uint8_t *d, uint8_t len) {
  (void)c; got.id[got.n] = id; got.bus[got.n] = bus; got.len[got.n] = len; got.d0[got.n++] = d[0];
}

int main(void) {
  uint8_t pk[64];
  rec(pk, 0x257, (const uint8_t[]){1, 2, 3, 4, 5, 6, 7, 8}, 8, 1);
  rec(pk + 16, 0x118, (const uint8_t[]){9, 9}, 2, 0);
  rec(pk + 32, 6, (const uint8_t[]){0}, 0, 15);               // the Commander's ACK
  assert(panda_parse(pk, 48, on, 0) == 1);
  assert(got.n == 3 && got.id[2] == 6 && got.bus[2] == 15 && got.len[2] == 0);  // the ACK, in its place, len 0
  assert(got.id[0] == 0x257 && got.bus[0] == 1 && got.len[0] == 8 && got.d0[0] == 1);
  assert(got.id[1] == 0x118 && got.len[1] == 2 && got.d0[1] == 9);
  assert(panda_parse(pk, 47, on, 0) == -1 && got.n == 3);     // not a multiple of 16: dropped whole

  uint8_t out[PANDA_DGRAM_MAX];
  assert(panda_hello(out) == 5 && panda_out_ok(out, 5));
  assert(panda_clear(out) == 1 && panda_out_ok(out, 1));
  uint16_t ids[44];
  for (int i = 0; i < 44; i++) ids[i] = 0x100 + i;
  assert(panda_ids(0x0f, ids, 43, out) == 130 && panda_out_ok(out, 130));
  assert(panda_ids(0x0e, ids, 1, out) == 4 && panda_out_ok(out, 4) && out[2] == 0x01 && out[3] == 0x00);
  assert(panda_ids(0x0f, ids, 44, out) == 0);                 // past 43: refused
  // Every other op is refused, at every length: 0x0c (all ids), CAN sends, anything else.
  for (int op = 0; op < 256; op++) {
    assert(panda_ids(op, ids, 1, out) == 0 || op == 0x0f || op == 0x0e);
    uint8_t d[4] = {op, 0xff, 0x01, 0x00};
    for (size_t n = 1; n <= 4; n++)
      assert(!panda_out_ok(d, n) || (n == 1 && op == 0x18) || (n == 4 && (op == 0x0f || op == 0x0e)));
  }
  uint8_t bad[4] = {0x0f, 0x00, 0x01, 0x00};                  // a bus byte other than 0xff
  assert(!panda_out_ok(bad, 4));
  puts("test_panda: ok");
}
