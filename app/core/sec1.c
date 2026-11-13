#include "sec1.h"
#include "pb.h"
#include "../third_party/monocypher.h"
#include <string.h>

// SHA-256("s3xy_enh"): the proof of possession, the same in every Commander.
// Constant, so no SHA-256 code ships (test_sec1_session checks it against the golden file).
static const uint8_t POP[32] = {
    0x59, 0xd2, 0x41, 0x4e, 0xee, 0xe3, 0x57, 0xfe, 0x14, 0x2d, 0xf7, 0x26, 0x78, 0xf2, 0x6b, 0xb3,
    0xbf, 0x3d, 0x06, 0x03, 0xc7, 0x1e, 0xdb, 0xae, 0xd7, 0x50, 0x3e, 0xb7, 0x95, 0x0e, 0x8e, 0x1c};

void sec1_init(sec1_t *s, const uint8_t secret[32]) {
  memset(s, 0, sizeof *s);
  memcpy(s->secret, secret, 32);
  crypto_x25519_public_key(s->pub, s->secret);
  s->pushes_clear = 1;
}

void sec1_wipe(sec1_t *s) { crypto_wipe(s, sizeof *s); }

// SessionData{sec_ver: 1, sec1: Sec1Payload{msg, `field`: body}}.
static size_t session(uint8_t *out, size_t cap, unsigned msg, uint32_t field, const uint8_t *body, size_t n) {
  uint8_t in[128];
  pb_out p = {in, 0, sizeof in, 0};
  pb_u(&p, 1, msg);
  pb_b(&p, field, body, n);
  pb_out o = {out, 0, cap, 0};
  pb_u(&o, 2, 1);
  pb_b(&o, 11, in, p.n);
  return o.bad || p.bad ? 0 : o.n;
}

size_t sec1_hello(const sec1_t *s, uint8_t *out, size_t cap) {
  uint8_t b[64];
  pb_out o = {b, 0, sizeof b, 0};
  pb_b(&o, 1, s->pub, 32);
  pb_u(&o, 2, 1); // flag 1: pushes in the clear, out of the shared stream
  return session(out, cap, 0, 20, b, o.n);
}

// The reply's body for msg/field with status 0, or 0 (bad shape or refused).
static int session_reply(const uint8_t *d, size_t n, unsigned msg, uint32_t field, const uint8_t **r, size_t *rn) {
  const uint8_t *p = (const uint8_t *)"";
  size_t pn = 0;
  pb_val v;
  int got = pb_get(d, n, 11, &v);
  if (got < 0 || (got && v.wt != 2)) return 0;
  if (got) p = v.p, pn = v.len;
  uint64_t m;
  if (!pb_ok(p, pn) || !pb_get_u(p, pn, 1, &m) || m != msg || !pb_get_b(p, pn, field, r, rn)) return 0;
  pb_val st;
  int has = pb_get(*r, *rn, 1, &st);
  return has == 0 || (has == 1 && st.wt == 0 && st.v == 0); // status absent or 0
}

static void ctr_init(sec1_t *s, const uint8_t key[32], const uint8_t iv[16]) {
  AES_init_ctx(&s->aes, key);
  memcpy(s->ctr, iv, 16);
  s->off = 16;
  s->keyed = 1;
}

void sec1_crypt(sec1_t *s, uint8_t *buf, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if (s->off == 16) {
      memcpy(s->ks, s->ctr, 16);
      AES_ECB_encrypt(&s->aes, s->ks);
      for (int b = 15; b >= 0 && ++s->ctr[b] == 0; b--) {} // 128-bit big-endian increment
      s->off = 0;
    }
    buf[i] ^= s->ks[s->off++];
  }
}

size_t sec1_answer(sec1_t *s, const uint8_t *resp0, size_t n, uint8_t *out, size_t cap) {
  const uint8_t *r, *dev, *iv;
  size_t rn, dn, ivn;
  if (s->keyed || !session_reply(resp0, n, 1, 21, &r, &rn)) return 0;
  uint64_t flags;
  // Known firmware grants flag 1 without echoing it; only a refusal counts.
  if (pb_get_u(r, rn, 4, &flags) && !(flags & 1)) s->pushes_clear = 0;
  if (!pb_get_b(r, rn, 2, &dev, &dn) || dn != 32 || !pb_get_b(r, rn, 3, &iv, &ivn) || ivn != 16) return 0;
  uint8_t shared[32], key[32], zero[32] = {0}, enc[32];
  crypto_x25519(shared, s->secret, dev);
  crypto_wipe(s->secret, 32); // one throwaway key per session
  if (crypto_verify32(shared, zero) == 0) return 0; // all-zero: a low-order Commander key
  for (int i = 0; i < 32; i++) key[i] = shared[i] ^ POP[i];
  ctr_init(s, key, iv);
  crypto_wipe(shared, 32);
  crypto_wipe(key, 32);
  memcpy(enc, dev, 32);
  sec1_crypt(s, enc, 32);
  uint8_t b[64];
  pb_out o = {b, 0, sizeof b, 0};
  pb_b(&o, 2, enc, 32);
  pb_u(&o, 3, SEC1_DEVICE_TYPE);
  return session(out, cap, 2, 22, b, o.n);
}

int sec1_check(sec1_t *s, const uint8_t *resp1, size_t n) {
  const uint8_t *r, *v;
  size_t rn, vn;
  if (!s->keyed || !session_reply(resp1, n, 3, 23, &r, &rn) || !pb_get_b(r, rn, 3, &v, &vn) || vn != 32) return -1;
  uint8_t mine[32];
  memcpy(mine, v, 32);
  sec1_crypt(s, mine, 32);
  return crypto_verify32(mine, s->pub) ? -1 : 0;
}

// Every field of m is one of want[] with its wire type, each exactly once.
static int exactly(const uint8_t *m, size_t n, const uint32_t *want, const uint8_t *wt, int k) {
  pb_in in = {m, n, 0};
  pb_val v;
  int r, seen = 0;
  while ((r = pb_next(&in, &v)) == 1) {
    int j = 0;
    while (j < k && want[j] != v.field) j++;
    if (j == k || wt[j] != v.wt || (seen >> j & 1)) return 0;
    seen |= 1 << j;
  }
  return r == 0 && seen == (1 << k) - 1;
}

int sec1_out_ok(const uint8_t *m, size_t n) {
  const uint8_t *p, *b;
  size_t pn, bn;
  uint64_t ver, msg, x;
  if (!exactly(m, n, (const uint32_t[]){2, 11}, (const uint8_t[]){0, 2}, 2)) return 0;
  pb_get_u(m, n, 2, &ver);
  pb_get_b(m, n, 11, &p, &pn);
  if (ver != 1 || !pb_get_u(p, pn, 1, &msg)) return 0;
  if (msg == 0) {
    if (!exactly(p, pn, (const uint32_t[]){1, 20}, (const uint8_t[]){0, 2}, 2)) return 0;
    pb_get_b(p, pn, 20, &b, &bn);
    return exactly(b, bn, (const uint32_t[]){1, 2}, (const uint8_t[]){2, 0}, 2) &&
           pb_get_u(b, bn, 2, &x) && x == 1;
  }
  if (msg == 2) {
    if (!exactly(p, pn, (const uint32_t[]){1, 22}, (const uint8_t[]){0, 2}, 2)) return 0;
    pb_get_b(p, pn, 22, &b, &bn);
    return exactly(b, bn, (const uint32_t[]){2, 3}, (const uint8_t[]){2, 0}, 2) &&
           pb_get_u(b, bn, 3, &x) && x == SEC1_DEVICE_TYPE;
  }
  return 0;
}
