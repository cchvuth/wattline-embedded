// ESP-IDF protocomm "Security1" as Enhauto runs it (Sec1 in lib/commander.dart):
// X25519, then key = shared secret XOR SHA-256("s3xy_enh") for AES-256-CTR with
// the Commander's random as IV. ONE counter stream runs through both directions:
// every message is processed once, in order. Build aes.c with -DAES256=1.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "../third_party/aes.h"

enum { SEC1_DEVICE_TYPE = 1 }; // phone. 64 is the Dash and is never written.

typedef struct {
  uint8_t secret[32], pub[32];
  struct AES_ctx aes;
  uint8_t ctr[16], ks[16];
  unsigned off;
  int keyed;        // answer() succeeded: crypt() may run
  int pushes_clear; // 0 if the Commander refused flag 1: then never subscribe
} sec1_t;

void sec1_init(sec1_t *s, const uint8_t secret[32]);
// Each writer returns the message length, 0 on error. out holds >= 64 bytes.
size_t sec1_hello(const sec1_t *s, uint8_t *out, size_t cap);                         // SessionCmd0
size_t sec1_answer(sec1_t *s, const uint8_t *resp0, size_t n, uint8_t *out, size_t cap); // Resp0 -> Cmd1
int sec1_check(sec1_t *s, const uint8_t *resp1, size_t n);                            // 0 = the key is confirmed
void sec1_crypt(sec1_t *s, uint8_t *buf, size_t n);                                   // in place, continues the stream
void sec1_wipe(sec1_t *s);
// SessionCmd0/Cmd1 shape check for the one send function: sec_ver 1, msg 0 or
// 2, nothing else, and Cmd1's device type exactly SEC1_DEVICE_TYPE.
int sec1_out_ok(const uint8_t *m, size_t n);
