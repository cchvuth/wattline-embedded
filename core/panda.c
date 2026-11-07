#include "panda.h"
#include <string.h>

static const uint8_t kHello[5] = {'e', 'h', 'l', 'l', 'o'};

size_t panda_hello(uint8_t *out) { memcpy(out, kHello, 5); return 5; }
size_t panda_clear(uint8_t *out) { out[0] = 0x18; return 1; }

size_t panda_ids(uint8_t op, const uint16_t *ids, size_t n, uint8_t *out) {
  if ((op != 0x0f && op != 0x0e) || n == 0 || n > PANDA_MAX_IDS) return 0;
  size_t k = 0;
  out[k++] = op;
  for (size_t i = 0; i < n; i++) {
    if (ids[i] > 0x7ff) return 0;               // 11-bit CAN ids only
    out[k++] = 0xff; out[k++] = ids[i] >> 8; out[k++] = ids[i] & 0xff;  // 0xff = any bus
  }
  return k;
}

int panda_out_ok(const uint8_t *d, size_t n) {
  if (n == 5) return memcmp(d, kHello, 5) == 0;
  if (n == 1) return d[0] == 0x18;
  if (n < 4 || (n - 1) % 3 || (n - 1) / 3 > PANDA_MAX_IDS || (d[0] != 0x0f && d[0] != 0x0e)) return 0;
  for (size_t o = 1; o < n; o += 3)
    if (d[o] != 0xff || ((d[o + 1] << 8) | d[o + 2]) > 0x7ff) return 0;
  return 1;
}

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

int panda_parse(const uint8_t *d, size_t n, panda_frame_fn fn, void *ctx) {
  if (n % 16) return -1;
  int acks = 0;
  for (size_t o = 0; o < n; o += 16) {
    uint32_t w0 = le32(d + o), w1 = le32(d + o + 4);
    uint16_t id = w0 >> 21;
    uint8_t len = w1 & 15, bus = (w1 >> 4) & 0xff;
    if (id == 0) continue;
    if (bus == 15 && id == 6) { acks++; continue; }
    if (len < 1 || len > 8) continue;
    fn(ctx, id, bus, d + o + 8, len);
  }
  return acks;
}
