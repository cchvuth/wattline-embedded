#include "pb.h"
#include <string.h>

static void put(pb_out *o, const uint8_t *v, size_t len) {
  if (o->bad || len > o->cap - o->n) { o->bad = 1; return; }
  if (len) memcpy(o->p + o->n, v, len);
  o->n += len;
}

void pb_varint(pb_out *o, uint64_t v) {
  uint8_t b[10];
  size_t n = 0;
  for (; v >= 0x80; v >>= 7) b[n++] = (uint8_t)(v & 0x7f) | 0x80;
  b[n++] = (uint8_t)v;
  put(o, b, n);
}

void pb_u(pb_out *o, uint32_t field, uint64_t v) {
  pb_varint(o, (uint64_t)field << 3);
  pb_varint(o, v);
}

void pb_b(pb_out *o, uint32_t field, const uint8_t *v, size_t len) {
  pb_varint(o, (uint64_t)field << 3 | 2);
  pb_varint(o, len);
  put(o, v, len);
}

static int rd_varint(pb_in *in, uint64_t *v) {
  *v = 0;
  for (int s = 0; s < 70; s += 7) {
    if (in->i >= in->n) return -1;
    uint8_t x = in->b[in->i++];
    *v |= (uint64_t)(x & 0x7f) << s;
    if (x < 0x80) return 0;
  }
  return -1; // more than 10 bytes
}

int pb_next(pb_in *in, pb_val *out) {
  if (in->i >= in->n) return 0;
  uint64_t tag;
  if (rd_varint(in, &tag) || tag >> 3 > UINT32_MAX || tag >> 3 == 0) return -1;
  memset(out, 0, sizeof *out);
  out->field = (uint32_t)(tag >> 3);
  out->wt = tag & 7;
  switch (out->wt) {
  case 0: return rd_varint(in, &out->v) ? -1 : 1;
  case 2: {
    uint64_t n;
    if (rd_varint(in, &n) || n > in->n - in->i) return -1;
    out->p = in->b + in->i;
    out->len = (size_t)n;
    in->i += (size_t)n;
    return 1;
  }
  case 5: {
    if (in->n - in->i < 4) return -1;
    const uint8_t *q = in->b + in->i;
    uint32_t w = (uint32_t)q[0] | (uint32_t)q[1] << 8 | (uint32_t)q[2] << 16 | (uint32_t)q[3] << 24;
    memcpy(&out->f, &w, 4); // little-endian float32
    in->i += 4;
    return 1;
  }
  case 1: // fixed64: skipped (Dart skips without a bounds check; here a short one is malformed)
    if (in->n - in->i < 8) return -1;
    in->i += 8;
    return 1;
  default: return -1;
  }
}

int pb_ok(const uint8_t *b, size_t n) {
  pb_in in = {b, n, 0};
  pb_val v;
  int r;
  while ((r = pb_next(&in, &v)) == 1) {}
  return r == 0;
}

int pb_get(const uint8_t *b, size_t n, uint32_t field, pb_val *out) {
  pb_in in = {b, n, 0};
  pb_val v;
  int r, found = 0;
  while ((r = pb_next(&in, &v)) == 1)
    if (v.field == field) *out = v, found = 1;
  return r < 0 ? -1 : found;
}

int pb_get_u(const uint8_t *b, size_t n, uint32_t field, uint64_t *v) {
  pb_val x;
  if (pb_get(b, n, field, &x) != 1 || x.wt != 0) return 0;
  *v = x.v;
  return 1;
}

int pb_get_b(const uint8_t *b, size_t n, uint32_t field, const uint8_t **p, size_t *len) {
  pb_val x;
  if (pb_get(b, n, field, &x) != 1 || x.wt != 2) return 0;
  *p = x.p;
  *len = x.len;
  return 1;
}
