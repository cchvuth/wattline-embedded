// Panda's link and polling logic (lib/panda.dart 1.3.0), with the time passed in.
#include "sched.h"
#include "panda.h"
#include "signals_gen.h"
#include <string.h>

_Static_assert(kGroupCount <= LINK_GROUPS, "more polled groups than link_t holds");

const link_cfg_t kLinkDefaults = {
  .tick_ms = 1000, .quiet_ms = 3000, .lost_ms = 8000, .frozen_ms = 8000,
  .backoff_ms = {1000, 3000, 6000, 10000}, .ack_resub_ms = 2000, .warmup_ms = 2000, .slot_ms = 500,
  .stray_ms = 300, .strays = 3, .flood_frames = 100, .flood_ms = 5000, .hot_ms = 3000, .back_ms = 3000,
  .steer_readd_ms = 1000,
};

static bool has(const uint8_t *set, unsigned id) { return id < 2048 && set[id >> 3] >> (id & 7) & 1; }
static void put(uint8_t *set, unsigned id) { if (id < 2048) set[id >> 3] |= 1 << (id & 7); }

static bool always_on(unsigned id) {
  for (int i = 0; i < kAlwaysOnCount; i++) if (kAlwaysOn[i] == id) return true;
  return false;
}
static int group_of(unsigned id) {
  for (int g = 0; g < kGroupCount; g++)
    for (int i = 0; i < kSparse[g].n; i++) if (kSparse[g].ids[i] == id) return g;
  return -1;
}

static void out(link_t *l, const uint8_t *d, size_t n) {
  if (n && panda_out_ok(d, n) && l->port.send) l->port.send(l->port.ctx, d, n);   // the one gate
}
static void hello(link_t *l) { uint8_t b[5]; out(l, b, panda_hello(b)); }

// Panda._sendIds: add (0x0f) or remove (0x0e); a skipped id is never added. Chunked at 43 ids
// (the app sends one datagram of any length; panda_ids refuses past 43).
static void send_ids(link_t *l, uint8_t op, const uint16_t *ids, size_t n) {
  uint16_t keep[PANDA_MAX_IDS];
  uint8_t b[PANDA_DGRAM_MAX];
  size_t k = 0;
  for (size_t i = 0; i < n; i++) {
    if (op == 0x0f && has(l->skip, ids[i])) continue;
    keep[k++] = ids[i];
    if (k == PANDA_MAX_IDS) { out(l, b, panda_ids(op, keep, k, b)); k = 0; }
  }
  if (k) out(l, b, panda_ids(op, keep, k, b));
}
static void send_group(link_t *l, uint8_t op, int g) { send_ids(l, op, kSparse[g].ids, kSparse[g].n); }
static size_t set_list(const uint8_t *set, uint16_t *ids) {
  size_t n = 0;
  for (unsigned id = 0; id < 2048; id++) if (has(set, id)) ids[n++] = (uint16_t)id;
  return n;
}
static size_t polled(uint16_t *ids) {
  size_t n = 0;
  for (int g = 0; g < kGroupCount; g++) for (int i = 0; i < kSparse[g].n; i++) ids[n++] = kSparse[g].ids[i];
  return n;
}

// Panda._subscribe: clear (0x18), then the whole list, streamed ids first so a second datagram
// holds only polled ones (lose it and the wheel adds them again anyway).
static void subscribe(link_t *l) {
  l->subscribes++;
  uint8_t b[PANDA_DGRAM_MAX];
  out(l, b, panda_clear(b));
  for (int g = 0; g < LINK_GROUPS; g++) { l->sparse_on[g] = 1; l->sparse_stray[g] = 0; }
  uint16_t ids[kSubscribeCount];
  size_t n = 0;
  for (int pass = 0; pass < 2; pass++)
    for (int i = 0; i < kSubscribeCount; i++)
      if (always_on(kSubscribeIds[i]) == !pass && !has(l->skip, kSubscribeIds[i])) ids[n++] = kSubscribeIds[i];
  for (size_t o = 0; o < n; o += PANDA_MAX_IDS)
    out(l, b, panda_ids(0x0f, ids + o, n - o < PANDA_MAX_IDS ? n - o : PANDA_MAX_IDS, b));
}

static void set_state(link_t *l, link_state_t s, const char *why, int64_t until, int64_t now) {
  l->state = s;
  l->why = why;
  l->until = until;
  l->car->wifi_live = s == LINK_LIVE;
  if (s != LINK_LIVE && l->car->p.bt_up) car_apply_bt(l->car, now);   // WiFi gone: Bluetooth's values now
}

static void start_count(link_t *l, int64_t now) {
  l->last_hz = now;
  l->frame_count = l->speed_frames = l->foreign = 0;
  l->speed_known = false;
}

void link_init(link_t *l, car_t *car, link_port_t port, int64_t now) {
  memset(l, 0, sizeof *l);
  l->cfg = kLinkDefaults;
  l->port = port;
  l->car = car;
  l->sparse_wanted = l->crawl = true;
  for (int g = 0; g < LINK_GROUPS; g++) { l->sparse_on[g] = 1; l->sparse_off_at[g] = NEVER; }
  l->until = l->next_tick = l->slot2_at = l->retry_at = NEVER;
  l->connected_at = l->last_recv = l->can_at = l->stream_at = l->ack_sub_at = l->hot_until = NEVER;
  l->back_until = l->skip_at = l->flooded = l->speed_at = NEVER;
  l->beat_at = l->last_hz = now;
  set_state(l, LINK_IDLE, "disconnected", NEVER, now);
}

static void forget(link_t *l, int64_t now) {
  l->frame_hz = l->speed_hz = l->speed_gap_ms = l->speed_gap = 0;
  l->speed_known = false;
  l->speed_at = NEVER;   // an outage is no gap in the stream
  car_forget(l->car, now);
}

static void teardown(link_t *l, int64_t now) {
  l->keepalive = false;
  if (l->port.close) l->port.close(l->port.ctx);
  forget(l, now);
}

static void schedule_retry(link_t *l, const char *why, int64_t now) {
  if (!l->on) return;
  l->keepalive = false;   // no more ticks until the next attempt
  int ms = l->cfg.backoff_ms[l->attempt < 3 ? l->attempt : 3];
  l->attempt++;
  set_state(l, LINK_RETRYING, why, now + ms, now);
  l->retry_at = now + ms;
}

// Panda._attemptConnect from the hello on (the WiFi join is the port's).
static void attempt_connect(link_t *l, int64_t now) {
  if (!l->on) return;
  l->retry_at = NEVER;
  teardown(l, now);
  if (l->port.open && !l->port.open(l->port.ctx)) { schedule_retry(l, "socket failed", now); return; }
  l->last_recv = NEVER;
  l->streaming = false;
  l->ack_sub_at = NEVER;   // the last socket's
  l->beat_at = l->connected_at = now;
  set_state(l, LINK_HELLO, "saying hello", now + l->cfg.lost_ms, now);
  hello(l);
  subscribe(l);
  l->keepalive = true;
  l->next_tick = now + l->cfg.tick_ms;
}

void link_connect(link_t *l, int64_t now) {
  link_disconnect(l, now);
  l->on = true;
  l->attempt = 0;
  attempt_connect(l, now);
}

void link_disconnect(link_t *l, int64_t now) {
  l->on = false;
  l->retry_at = NEVER;
  teardown(l, now);
  set_state(l, LINK_IDLE, "disconnected", NEVER, now);
}

bool link_trying(const link_t *l) { return l->state == LINK_IDLE || l->state == LINK_HELLO; }
bool link_group_on(const link_t *l, int g) { return l->sparse_on[g] != 0; }
void link_pressed(link_t *l, int64_t now) { l->hot_until = now + l->cfg.hot_ms; }

void link_set_sparse(link_t *l, bool on) {
  if (on == l->sparse_wanted) return;
  l->sparse_wanted = on;
  if (!on) l->sparse_all = true;
}

// The wheel turns while the stream runs: not in a stall, nor in a new stream's first seconds.
static bool wheel_on(const link_t *l, int64_t now) {
  return l->streaming && now - l->can_at < l->cfg.quiet_ms && now - l->stream_at >= l->cfg.warmup_ms;
}

// Panda._sparseTick: the next slot, each group on or off as the wheel says, whatever it was.
void link_sparse_tick(link_t *l, int64_t now) {
  uint16_t ids[2048];
  if (l->skip_stray >= l->cfg.strays) {   // a skipped id still coming: its remove was lost
    l->sparse_fixes++;
    l->skip_at = now;
    send_ids(l, 0x0e, ids, set_list(l->skip, ids));
  }
  l->skip_stray = 0;
  if (!l->sparse_wanted) {   // everything on, said again each round: a lost add would leave a group dark
    bool round = false;
    if (!l->sparse_all) {   // Dart: _sparseAll || _slot++ % n == 0 (the count stands still on a switch)
      round = l->slot == 0;
      l->slot = (l->slot + 1) % kWheelSlots;
    }
    if (l->sparse_all || round) {
      l->sparse_all = false;
      for (int g = 0; g < LINK_GROUPS; g++) l->sparse_on[g] = 1;
      l->sparse_ops++;
      send_ids(l, 0x0f, ids, polled(ids));
    }
    return;
  }
  const uint8_t *slot = kWheel[l->slot];
  l->slot = (l->slot + 1) % kWheelSlots;
  car_t *c = l->car;
  bool parked = c->gear == 'P', left = l->was_parked && !parked, hot = now < l->hot_until;
  l->was_parked = parked;
  l->crawl = c->gear == 'R' || (c->kph ? c->speed / 3.6 : c->speed * 0.44704) < (l->crawl ? 5 : 4);
  for (int i = 0; i < kGroupCount; i++) {
    bool due = false, chip = false;
    for (int k = 1; k <= slot[0]; k++) due |= slot[k] == i;
    for (size_t k = 0; k < sizeof kChipGroups; k++) chip |= kChipGroups[k] == i;
    bool gp = kSparse[i].parked;
    bool want = i == kSteerGroup ? l->crawl
              : (hot && chip && (!gp || parked)) ||
                (gp ? (due && parked) || left : due || (i == kChargeGroup && c->charging));
    if (!want && l->sparse_stray[i] >= l->cfg.strays) {   // off, still coming: the remove again
      l->sparse_fixes++;
      l->sparse_off_at[i] = now;
      send_group(l, 0x0e, i);
    }
    l->sparse_stray[i] = 0;
    if (want == (l->sparse_on[i] != 0)) {
      // On for long, unlike a slot's group: said again once its frames go quiet.
      if (want && i == kSteerGroup && now - c->p.steer_at >= l->cfg.steer_readd_ms) send_group(l, 0x0f, i);
      continue;
    }
    l->sparse_on[i] = want;
    if (!want) l->sparse_off_at[i] = now;
    l->sparse_ops++;
    send_group(l, want ? 0x0f : 0x0e, i);
  }
}

// Panda.skip =: ids nothing on screen shows now are taken off the filter.
void link_set_skip(link_t *l, const uint16_t *v, size_t n, int64_t now) {
  uint8_t nb[256] = {0}, back[256] = {0}, drop[256] = {0};
  for (size_t i = 0; i < n; i++) put(nb, v[i]);
  if (!memcmp(nb, l->skip, sizeof nb)) return;
  for (int i = 0; i < 256; i++) { back[i] = l->skip[i] & ~nb[i]; drop[i] = nb[i] & ~l->skip[i]; }
  memcpy(l->skip, nb, sizeof nb);
  if (has(drop, 0x185)) {   // a brake reading would hold while its frame is off: none rather
    for (int i = 0; i < 4; i++) l->car->p.brake_nm[i] = NUL;
    l->car->brake_pct = NUL;
  }
  l->skip_at = now;
  uint16_t ids[2048], add[2048];
  send_ids(l, 0x0e, ids, set_list(drop, ids));
  // Back on: a streamed id at once, a polled one if its group is on now, else at its slot.
  size_t nb_ = set_list(back, ids), k = 0;
  for (size_t i = 0; i < nb_; i++) {
    int g = group_of(ids[i]);
    if (always_on(ids[i]) || !l->sparse_wanted || g < 0 || l->sparse_on[g]) add[k++] = ids[i];
  }
  send_ids(l, 0x0f, add, k);
  // A lost add would leave a streamed id dark: said again by the tick a while, with those still due.
  if (!(now < l->back_until)) memset(l->back, 0, sizeof l->back);
  for (size_t i = 0; i < nb_; i++) if (always_on(ids[i])) put(l->back, ids[i]);
  l->back_until = now + l->cfg.back_ms;
}

static void flood_check(link_t *l, int64_t now) {
  if (l->foreign < l->cfg.flood_frames || now - l->flooded < l->cfg.flood_ms) return;
  l->flooded = now;
  l->resubscribes++;
  subscribe(l);
}

// Panda._push without the redraw throttle (the UI's 33 ms timer reads the state instead).
void link_push(link_t *l, int64_t now) {
  if (l->state != LINK_LIVE) start_count(l, now);   // back from a stall: count afresh
  if (now - l->last_hz >= 1000) {
    l->frame_hz = l->frame_count;
    l->speed_hz = l->speed_frames;
    l->speed_known = true;
    l->speed_gap_ms = l->speed_gap;
    l->speed_gap = 0;
    l->foreign_hz = l->foreign;
    flood_check(l, now);
    l->frame_count = l->speed_frames = l->foreign = 0;
    l->last_hz = now;
  }
  l->state = LINK_LIVE;   // as Panda: status 'live', no _set (Bluetooth's values wait)
  l->until = NEVER;
  l->car->wifi_live = true;
}

void link_on_frame(link_t *l, uint16_t id, uint8_t bus, const uint8_t *d, uint8_t len, int64_t now) {
  int g = group_of(id);
  if (!l->streaming || g < 0) l->can_at = now;   // the stream proper: not a polled id
  if (!l->streaming) {
    l->streaming = true;
    l->stream_at = l->can_at;
    start_count(l, l->can_at);
  }
  l->frame_count++;
  if (has(l->skip, id) && now - l->skip_at > l->cfg.stray_ms) l->skip_stray++;
  if (g >= 0 && !l->sparse_on[g] && l->sparse_off_at[g] != NEVER && now - l->sparse_off_at[g] > l->cfg.stray_ms)
    l->sparse_stray[g]++;
  if (id == 0x257) {
    l->speed_frames++;
    if (l->speed_at != NEVER && l->state == LINK_LIVE && now - l->speed_at > l->speed_gap)
      l->speed_gap = (int)(now - l->speed_at);
    l->speed_at = now;
  }
  car_frame(l->car, id, bus, d, len, now);
}

typedef struct { link_t *l; int64_t now; } pk_ctx_t;
static void on_record(void *x, uint16_t id, uint8_t bus, const uint8_t *d, uint8_t len) {
  pk_ctx_t *p = x;
  link_t *l = p->l;
  if (len == 0) {   // the Commander's ACK: it wants the list. Not while frames flow, and not too often
    l->acks++;
    if ((!l->streaming || p->now - l->can_at >= l->cfg.ack_resub_ms) && p->now - l->ack_sub_at >= l->cfg.ack_resub_ms) {
      l->ack_sub_at = p->now;
      subscribe(l);
    }
    return;
  }
  if (sub_index(id) < 0) { l->frame_count++; l->foreign++; return; }   // never asked for: counted, dropped
  link_on_frame(l, id, bus, d, len, p->now);
}

void link_on_packet(link_t *l, const uint8_t *d, size_t n, int64_t now) {
  pk_ctx_t p = {l, now};
  panda_parse(d, n, on_record, &p);
}

void link_on_datagram(link_t *l, const uint8_t *d, size_t n, int64_t now) {
  if (l->keepalive && now - l->beat_at >= l->cfg.frozen_ms) {   // frozen: what waits is old
    l->beat_at = now;
    attempt_connect(l, now);
    return;
  }
  l->last_recv = now;
  int64_t was = l->can_at;
  link_on_packet(l, d, n, now);
  bool stream = l->can_at != was;
  if (stream) l->attempt = 0;   // the stream eases the backoff, not an ACK
  if (stream && !l->keepalive && l->on) {   // back while a retry was pending: keep this link
    l->retry_at = NEVER;
    l->beat_at = now;
    l->keepalive = true;
    l->next_tick = now + l->cfg.tick_ms;
  }
  if (stream || l->state == LINK_HELLO) link_push(l, now);   // an ACK alone answers a hello
}

// Panda._tick.
static void tick(link_t *l, int64_t now) {
  bool frozen = now - l->beat_at >= l->cfg.frozen_ms;
  l->beat_at = now;
  if (frozen) { attempt_connect(l, now); return; }
  int64_t base = l->streaming ? l->can_at : l->connected_at, since = now - base;
  if (since >= l->cfg.lost_ms) {
    schedule_retry(l, l->last_recv == NEVER ? "Commander not answering" : "link lost", now);
    return;
  }
  if (now < l->back_until) { uint16_t ids[2048]; send_ids(l, 0x0f, ids, set_list(l->back, ids)); }
  if (wheel_on(l, now)) {
    link_sparse_tick(l, now);
    l->slot2_at = now + l->cfg.slot_ms;
  }
  hello(l);
  if (since >= l->cfg.quiet_ms && l->last_recv != NEVER) {
    set_state(l, LINK_QUIET, "link quiet, resubscribing", base + l->cfg.lost_ms, now);
    subscribe(l);
  }
}

void link_poll(link_t *l, int64_t now) {
  if (l->keepalive && now >= l->next_tick) {
    l->next_tick = now + l->cfg.tick_ms;
    tick(l, now);
  }
  if (l->slot2_at != NEVER && now >= l->slot2_at) {
    l->slot2_at = NEVER;
    if (wheel_on(l, now)) link_sparse_tick(l, now);
  }
  if (l->retry_at != NEVER && now >= l->retry_at) {
    l->retry_at = NEVER;
    attempt_connect(l, now);
  }
}
