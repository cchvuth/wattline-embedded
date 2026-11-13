// The WiFi link's logic from lib/panda.dart (1.3.0): subscribe list, sparse polling wheel, skip set,
// ACK handling, keep-alive tick, quiet / lost / frozen / retry backoff, frame rate counters.
// No OS calls: time comes in as monotonic ms, datagrams go out through link_port_t.send, every one
// through panda_out_ok first. Not here: the WiFi join (port/), Diagnostics' capture-all (0x0C).
#pragma once
#include "state.h"
#include <stddef.h>

typedef struct {                // PLAN 5.3: kept in dash.conf, not hard-coded in the logic
  int tick_ms;                  // keep-alive hello and the link checks (1000)
  int quiet_ms;                 // stream silence before resubscribing; the wheel stops too (3000)
  int lost_ms;                  // silence before the link is given up and retried (8000)
  int frozen_ms;                // a gap between ticks this long: the app was frozen, start over (8000)
  int backoff_ms[4];            // retry waits (1, 3, 6, 10 s)
  int ack_resub_ms;             // an ACK sends the list again at most this often, and only after
                                // this much stream silence (2000)
  int warmup_ms;                // a new stream reads every group this long before the wheel turns (2000)
  int slot_ms;                  // the wheel's slot (500)
  int stray_ms, strays;         // frames of a removed id this long after its remove, this many: remove again (300, 3)
  int flood_frames, flood_ms;   // foreign frames a second that mean the filter did not take; resend at most every (100, 5000)
  int hot_ms;                   // a button press keeps its chip's groups on (3000)
  int back_ms;                  // an unskipped streamed id's add is said again this long (3000)
  int steer_readd_ms;           // the steering group's add said again once its frames stop this long (1000)
} link_cfg_t;
extern const link_cfg_t kLinkDefaults;

typedef struct {
  void (*send)(void *ctx, const uint8_t *d, size_t n);
  bool (*open)(void *ctx);      // before a hello: step Bluetooth aside, (re)open the socket. false: retry. May be NULL
  void (*close)(void *ctx);     // teardown. May be NULL
  void *ctx;
  int (*settle)(void *ctx, int64_t now);   // ms to wait before the open (Bluetooth stepping aside); 0 go. May be NULL
} link_port_t;

typedef enum { LINK_IDLE, LINK_HELLO, LINK_LIVE, LINK_QUIET, LINK_RETRYING } link_state_t;
enum { LINK_GROUPS = 8 };

typedef struct {
  link_cfg_t cfg;
  link_port_t port;
  car_t *car;
  link_state_t state;
  const char *why;              // the state in words
  int64_t until;                // what the state counts down to, or NEVER
  bool on, keepalive, streaming, sparse_wanted, sparse_all, was_parked, crawl;
  int attempt, slot, skip_stray;
  int64_t next_tick, slot2_at, retry_at;
  int64_t connected_at, last_recv, can_at, stream_at, ack_sub_at, beat_at, hot_until, back_until, skip_at;
  int64_t flooded, last_hz, speed_at;
  int8_t sparse_on[LINK_GROUPS];
  int64_t sparse_off_at[LINK_GROUPS];
  int sparse_stray[LINK_GROUPS];
  uint8_t skip[256], back[256]; // id bitmaps (11-bit ids)
  // Rates, for the dash's lag alarm and the log.
  int frame_hz, speed_hz, speed_gap_ms, foreign_hz;
  bool speed_known;
  int frame_count, speed_frames, foreign, speed_gap;
  int subscribes, acks, resubscribes, sparse_ops, sparse_fixes;
} link_t;

void link_init(link_t *l, car_t *car, link_port_t port, int64_t now);
void link_connect(link_t *l, int64_t now);       // start, and keep retrying until link_disconnect
void link_disconnect(link_t *l, int64_t now);
void link_poll(link_t *l, int64_t now);          // run what is due: keep-alive tick, second slot, retry
void link_kick(link_t *l, int64_t now);          // a retry pending: try now (Bluetooth came ready, main.dart 774)
void link_on_datagram(link_t *l, const uint8_t *d, size_t n, int64_t now);   // Panda._onEvent
void link_on_packet(link_t *l, const uint8_t *d, size_t n, int64_t now);     // Panda._onPacket
void link_on_frame(link_t *l, uint16_t id, uint8_t bus, const uint8_t *d, uint8_t len, int64_t now);
void link_push(link_t *l, int64_t now);          // a stream packet: count rates, say live
void link_sparse_tick(link_t *l, int64_t now);
void link_set_skip(link_t *l, const uint16_t *ids, size_t n, int64_t now);
void link_set_sparse(link_t *l, bool on);
void link_pressed(link_t *l, int64_t now);
bool link_group_on(const link_t *l, int group);
bool link_trying(const link_t *l);               // Bluetooth starts no link while true
