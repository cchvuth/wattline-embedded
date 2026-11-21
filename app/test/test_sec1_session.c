// sec1 against test/golden_handshake.json (made by the Dart Sec1), plus the
// refusals: a liar Commander, flag 1 cleared, an all-zero shared secret, and
// the handshake shape gate (device type 1, never 64).
#include "../core/pb.h"
#include "../core/sec1.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int checks;
#define CHECK(x) (assert(x), checks++)

static char json[8192];

// The hex string of "key" in the golden file, decoded.
static size_t golden(const char *key, uint8_t *out) {
  char pat[64];
  snprintf(pat, sizeof pat, "\"%s\": \"", key);
  const char *s = strstr(json, pat);
  assert(s);
  s += strlen(pat);
  size_t n = 0;
  for (; s[2 * n] != '"'; n++) sscanf(s + 2 * n, "%2hhx", &out[n]);
  return n;
}

static int eq(const uint8_t *a, size_t an, const char *key) {
  uint8_t w[256];
  size_t wn = golden(key, w);
  return an == wn && !memcmp(a, w, wn);
}

// Builds a SessionResp0 by hand: sec1{msg 1, sr0{status 0, device_pubkey, device_random[, flags]}}.
static size_t resp0(uint8_t *out, const uint8_t pub[32], const uint8_t rnd[16], int flags) {
  uint8_t sr0[64], p1[80];
  pb_out a = {sr0, 0, sizeof sr0, 0}, b = {p1, 0, sizeof p1, 0}, o = {out, 0, 128, 0};
  pb_u(&a, 1, 0);
  pb_b(&a, 2, pub, 32);
  pb_b(&a, 3, rnd, 16);
  if (flags >= 0) pb_u(&a, 4, (unsigned)flags);
  pb_u(&b, 1, 1);
  pb_b(&b, 21, sr0, a.n);
  pb_u(&o, 2, 1);
  pb_b(&o, 11, p1, b.n);
  return o.n;
}

int main(void) {
  FILE *f = fopen("test/golden_handshake.json", "r");
  assert(f && "run from the app's root");
  json[fread(json, 1, sizeof json - 1, f)] = 0;
  fclose(f);

  uint8_t secret[32], buf[256], out[256], pub[32], rnd[16];
  size_t n;
  golden("phone_secret", secret);
  sec1_t s;
  sec1_init(&s, secret);
  CHECK(eq(s.pub, 32, "phone_pub"));
  n = sec1_hello(&s, out, sizeof out);
  CHECK(eq(out, n, "session_cmd0") && sec1_out_ok(out, n));
  n = golden("session_resp0", buf);
  size_t on = sec1_answer(&s, buf, n, out, sizeof out);
  CHECK(eq(out, on, "session_cmd1") && sec1_out_ok(out, on));
  CHECK(s.pushes_clear); // flag granted without an echo
  CHECK(sec1_answer(&s, buf, n, out, sizeof out) == 0); // once only
  n = golden("session_resp1", buf);
  CHECK(sec1_check(&s, buf, n) == 0);
  // Then requests and replies share that one stream (offsets 64..77, 78..84).
  n = golden("tryaction_frunk_plain", buf);
  sec1_crypt(&s, buf, n);
  CHECK(eq(buf, n, "tryaction_frunk_wire"));
  n = golden("resp_tryaction_wire", buf);
  sec1_crypt(&s, buf, 3); // odd split: the keystream carries over
  sec1_crypt(&s, buf + 3, n - 3);
  CHECK(eq(buf, n, "resp_tryaction_plain"));

  // A Commander that cannot prove the key is refused.
  sec1_init(&s, secret);
  n = golden("session_resp0", buf);
  CHECK(sec1_answer(&s, buf, n, out, sizeof out));
  n = golden("session_resp1", buf);
  buf[n - 1] ^= 1;
  CHECK(sec1_check(&s, buf, n) == -1);

  // Flag 1 cleared in the reply: pushes would share the stream.
  golden("dev_pub", pub);
  golden("dev_random", rnd);
  sec1_init(&s, secret);
  n = resp0(buf, pub, rnd, 0);
  CHECK(sec1_answer(&s, buf, n, out, sizeof out) && !s.pushes_clear);
  sec1_init(&s, secret);
  n = resp0(buf, pub, rnd, 1); // echoed: still clear
  CHECK(sec1_answer(&s, buf, n, out, sizeof out) && s.pushes_clear);
  sec1_init(&s, secret);
  n = resp0(buf, pub, rnd, -1);
  CHECK(eq(buf, n, "session_resp0")); // the hand builder matches the golden reply

  // An all-zero shared secret (a low-order Commander key) is refused.
  uint8_t zero[32] = {0};
  sec1_init(&s, secret);
  n = resp0(buf, zero, rnd, -1);
  CHECK(sec1_answer(&s, buf, n, out, sizeof out) == 0 && !s.keyed);
  // Short key or IV, a refused status, the wrong msg: refused.
  sec1_init(&s, secret);
  n = resp0(buf, pub, rnd, -1);
  buf[5]++; // msg 1 -> 2
  CHECK(sec1_answer(&s, buf, n, out, sizeof out) == 0);

  // The shape gate: device type exactly 1, msg 0 or 2, nothing extra.
  n = golden("session_cmd1", buf);
  CHECK(sec1_out_ok(buf, n));
  CHECK(buf[n - 2] == 0x18 && buf[n - 1] == 0x01); // device_type field 3 = 1 (phone)
  buf[n - 1] = 64;                                  // the Dash's type
  CHECK(!sec1_out_ok(buf, n));
  for (unsigned t = 0; t < 256; t++) {
    buf[n - 1] = (uint8_t)t;
    CHECK(sec1_out_ok(buf, n) == (t == 1));
  }
  n = golden("session_resp0", buf); // a reply shape (msg 1) is never sent
  CHECK(!sec1_out_ok(buf, n));
  n = golden("session_cmd0", buf);
  buf[n - 1] = 0; // flags 0
  CHECK(!sec1_out_ok(buf, n));

  printf("test_sec1_session: %d checks pass\n", checks);
  return 0;
}
