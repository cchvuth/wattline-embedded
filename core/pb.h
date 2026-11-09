// Protobuf wire format, just the shapes the Commander messages use: the
// _varint/_u/_b builders and the _fields parser of lib/commander.dart.
#pragma once
#include <stddef.h>
#include <stdint.h>

// Builder into a fixed buffer. Overflow sets bad and stops writing.
typedef struct { uint8_t *p; size_t n, cap; int bad; } pb_out;
void pb_varint(pb_out *o, uint64_t v);
void pb_u(pb_out *o, uint32_t field, uint64_t v);                       // varint field
void pb_b(pb_out *o, uint32_t field, const uint8_t *v, size_t len);    // length-delimited field

// One field as read: wt 0 sets v, wt 2 sets p/len, wt 5 sets f (the only
// fixed32s here are floats), wt 1 is skipped.
typedef struct { uint32_t field; uint8_t wt; uint64_t v; const uint8_t *p; size_t len; float f; } pb_val;
typedef struct { const uint8_t *b; size_t n, i; } pb_in;
int pb_next(pb_in *in, pb_val *out);   // 1 a field, 0 the end, -1 malformed
int pb_ok(const uint8_t *b, size_t n); // the whole message parses
// The field's last occurrence (last one wins, as _fields): 1 found, 0 absent,
// -1 the message does not parse (Dart throws there).
int pb_get(const uint8_t *b, size_t n, uint32_t field, pb_val *out);
// Shorthands: 1 only if present with that wire type.
int pb_get_u(const uint8_t *b, size_t n, uint32_t field, uint64_t *v);
int pb_get_b(const uint8_t *b, size_t n, uint32_t field, const uint8_t **p, size_t *len);
