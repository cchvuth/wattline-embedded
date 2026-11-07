// Host check of the proposed Sec1 crypto set: Monocypher X25519 + tiny-AES-c
// block cipher with a continuous CTR stream, against the same vectors as
// test/commander_test.dart (RFC 7748; NIST SP 800-38A F.5.5 with odd splits).
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "monocypher.h"
#include "aes.h"

static void hex(const char *s, uint8_t *out) {
  for (size_t i = 0; s[2 * i]; i++) sscanf(s + 2 * i, "%2hhx", &out[i]);
}

// Security1 runs ONE counter stream through every message, so the unused
// tail of a keystream block must carry over to the next call (mbedtls
// nc_off/stream_block semantics). tiny-AES-c's AES_CTR_xcrypt_buffer drops it.
typedef struct { struct AES_ctx aes; uint8_t ctr[16], ks[16]; unsigned off; } ctr_t;

static void ctr_init(ctr_t *c, const uint8_t key[32], const uint8_t iv[16]) {
  AES_init_ctx(&c->aes, key);
  memcpy(c->ctr, iv, 16);
  c->off = 16;
}

static void ctr_xor(ctr_t *c, uint8_t *buf, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if (c->off == 16) {
      memcpy(c->ks, c->ctr, 16);
      AES_ECB_encrypt(&c->aes, c->ks);
      for (int b = 15; b >= 0 && ++c->ctr[b] == 0; b--) {} // 128-bit big-endian increment
      c->off = 0;
    }
    buf[i] ^= c->ks[c->off++];
  }
}

int main(void) {
  uint8_t k[32], u[32], want[32], got[32], nine[32] = {9};
  hex("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4", k);
  hex("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c", u);
  hex("c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552", want);
  crypto_x25519(got, k, u);
  assert(!memcmp(got, want, 32));
  uint8_t alice[32], bob[32], bobpub[32];
  hex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a", alice);
  hex("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb", bob);
  crypto_x25519(got, alice, nine);
  hex("8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a", want);
  assert(!memcmp(got, want, 32));
  crypto_x25519(bobpub, bob, nine);
  crypto_x25519(got, alice, bobpub);
  hex("4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742", want);
  assert(!memcmp(got, want, 32));

  uint8_t key[32], iv[16], pt[32], ct[32], buf[32];
  hex("603deb1015ca71be2b73aef0857d77811f352c073b6108d72d9810a30914dff4", key);
  hex("f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff", iv);
  hex("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51", pt);
  hex("601ec313775789a5b7a7f504bbf3d228f443e3ca4d62b59aca84e990cacaf5c5", ct);
  ctr_t c;
  ctr_init(&c, key, iv);
  memcpy(buf, pt, 32);
  ctr_xor(&c, buf, 5);       // odd split, as in commander_test.dart
  ctr_xor(&c, buf + 5, 27);
  assert(!memcmp(buf, ct, 32));

  // The library's own CTR helper, same split: shows why the wrapper exists.
  struct AES_ctx t;
  AES_init_ctx_iv(&t, key, iv);
  memcpy(buf, pt, 32);
  AES_CTR_xcrypt_buffer(&t, buf, 5);
  AES_CTR_xcrypt_buffer(&t, buf + 5, 27);
  printf("tiny-AES-c AES_CTR_xcrypt_buffer across a 5+27 split: %s\n",
         memcmp(buf, ct, 32) ? "WRONG (restarts the keystream each call)" : "ok");
  printf("x25519 RFC 7748 + continuous AES-256-CTR NIST F.5.5: ok\n");
  return 0;
}
