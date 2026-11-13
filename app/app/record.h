// Drive recording (the settings' "Record drives"), for replays at the desk (tools/fake_panda.py --replay):
//   <record_dir>/<session>.panda  per datagram: u32 ms since the session start, u16 length (both little-endian),
//                                 then the datagram's 16-byte Panda records, only those of ids the app subscribes to
//   <record_dir>/<session>.cmdr   per Commander push, a line: "ms field=value ..." (value decimal for a varint,
//                                 x<hex> otherwise as bt_map_t holds it; "ms -" the link dropped). Made on the first push.
// Session: YYYYMMDD-HHMMSS (local) when the real clock is past 2025, else rec-NNNN. Total capped at record_max_mb,
// oldest sessions deleted first. Buffered, flushed about once a second; any error is logged once and stops it.
#pragma once
#include "app.h"

extern const char *record_dir;   // "/data/rec"
void record_sync(bool on, const conf_t *cf, int64_t now);   // each loop turn: starts, stops, flushes, caps
void record_panda(const uint8_t *d, size_t n, int64_t at);   // a received datagram (nothing while off)
void record_cmdr(const bt_map_t *m, int64_t at);             // a Commander push, NULL: the link dropped
