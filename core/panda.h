// Panda UDP framing, as lib/panda.dart (1.3.0). Listen-only: the only datagrams
// this code can build are "ehllo", clear 0x18, and the id list ops 0x0f (add) / 0x0e (remove).
#pragma once
#include <stddef.h>
#include <stdint.h>

enum { PANDA_MAX_IDS = 43, PANDA_DGRAM_MAX = 1 + 3 * PANDA_MAX_IDS };

// Each builder returns the datagram length, or 0 if refused.
size_t panda_hello(uint8_t *out);
size_t panda_clear(uint8_t *out);
size_t panda_ids(uint8_t op, const uint16_t *ids, size_t n, uint8_t *out);
// The one gate every outgoing datagram passes (port/ send calls it too).
int panda_out_ok(const uint8_t *d, size_t n);

typedef void (*panda_frame_fn)(void *ctx, uint16_t id, uint8_t bus, const uint8_t *data, uint8_t len);
// Returns the number of Commander ACKs (bus 15, id 6) seen; -1 if the datagram is dropped (len % 16).
int panda_parse(const uint8_t *d, size_t n, panda_frame_fn fn, void *ctx);
