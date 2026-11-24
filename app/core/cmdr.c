#include "cmdr.h"
#include "pb.h"
#include "../third_party/monocypher.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

const cmdr_cfg cmdr_cfg_default = {
    .id_path = "/data/cmdr_id", .pin_path = "/data/cmdr_pin", .wifi_first = 1,
    .settle_ms = 3000, .buttons_ms = 500, .rejoin_ms = 2000, .back_ms = 15000, .lean_for_ms = 30000,
};

enum { APPROVE_MS = 60000, APPROVE_EVERY_MS = 2000, WAIT_POLL_MS = 250, ASK_KEEP_MS = 300000 };
#define LONG_AGO (-(INT64_C(1) << 40)) // _wifiAt's epoch 0: long before any monotonic now

// ---- small files: one line each, written whole (tmp + rename) ----

static int load(const char *path, char *out, size_t cap) {
  FILE *f = path ? fopen(path, "r") : NULL;
  if (!f) return 0;
  int ok = fgets(out, (int)cap, f) != NULL;
  fclose(f);
  if (ok) out[strcspn(out, "\r\n")] = 0;
  return ok && out[0];
}

static int save(const char *path, const char *s) {
  char tmp[256];
  if (!path || snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp) return -1;
  FILE *f = fopen(tmp, "w");
  if (!f) return -1;
  int ok = fputs(s, f) >= 0 && fputc('\n', f) != EOF && fflush(f) == 0 && fsync(fileno(f)) == 0;   // the key: never a torn file
  ok = fclose(f) == 0 && ok;
  return ok && rename(tmp, path) == 0 ? 0 : -1;
}

// This unit's name on the Commander's allowlist, made once: a lowercase UUID v4 in braces.
static int phone_id(cmdr_t *c) {
  static const char *const hx = "0123456789abcdef";
  char s[48];
  if (c->id[0] == '{') return 0; // read once: a later failed read never makes a new one over the approved id
  FILE *f = c->cfg.id_path ? fopen(c->cfg.id_path, "r") : NULL;
  if (!f && c->cfg.id_path && errno != ENOENT) return -1; // there but not readable now (EIO, EMFILE): the next dial
  if (f) {
    int got = fgets(s, sizeof s, f) != NULL, bad = ferror(f);
    fclose(f);
    if (bad) return -1; // a read error: never a new id over the approved one
    if (got) s[strcspn(s, "\r\n")] = 0;
    if (got && strlen(s) == 38 && s[0] == '{' && s[37] == '}' && s[15] == '4') {
      memcpy(c->id, s, 39);
      return 0;
    }
  } // none yet, or a file that reads cleanly and holds no id: a new one
  uint8_t b[16];
  if (c->io.random(c->io.ctx, b, 16)) return -1;
  b[6] = (b[6] & 0x0f) | 0x40; // version 4
  b[8] = (b[8] & 0x3f) | 0x80; // RFC 4122 variant
  char *p = s;
  *p++ = '{';
  for (int i = 0; i < 16; i++) {
    if (i == 4 || i == 6 || i == 8 || i == 10) *p++ = '-';
    *p++ = hx[b[i] >> 4];
    *p++ = hx[b[i] & 15];
  }
  *p++ = '}';
  *p = 0;
  if (save(c->cfg.id_path, s)) return -1; // unsaved, every dial would be a new phone to approve
  memcpy(c->id, s, 39);
  return 0;
}

// ---- the one send function ----

int cmdr_send(cmdr_t *c, int chr, const uint8_t *m, size_t n, uint8_t *r, size_t cap, size_t *rn) {
  uint8_t buf[CMDR_MSG_MAX];
  uint64_t type, rt, st;
  *rn = 0;
  if (chr == CMDR_CHR_SESSION) {
    if (c->secure || !sec1_out_ok(m, n)) return CMDR_E_DENIED;
  } else if (chr != CMDR_CHR_API || !c->secure || !cmdr_req_ok(m, n, &type) || n > sizeof buf) {
    return CMDR_E_DENIED;
  }
  if (!c->linked) return CMDR_E_IO;
  if (chr == CMDR_CHR_SESSION) return c->io.rpc(c->io.ctx, chr, m, n, r, cap, rn) || *rn > cap ? CMDR_E_IO : 0;
  memcpy(buf, m, n);
  sec1_crypt(&c->sec, buf, n);
  if (c->io.rpc(c->io.ctx, chr, buf, n, r, cap, rn) || *rn > cap) return CMDR_E_IO;
  sec1_crypt(&c->sec, r, *rn);
  // Anything but the matching reply means the cipher streams have parted.
  if (cmdr_reply_status(r, *rn, &rt, &st) || rt != type + 1) return CMDR_E_STEP;
  return 0;
}

// One EnhAPI exchange; *status is the reply's, 0 = OK.
static int request(cmdr_t *c, const uint8_t *m, size_t n, uint64_t *status, uint8_t *r, size_t *rn) {
  uint64_t type;
  int e = cmdr_send(c, CMDR_CHR_API, m, n, r, CMDR_MSG_MAX, rn);
  if (e) return e;
  cmdr_reply_status(r, *rn, &type, status);
  return 0;
}

// ---- session ----

static void set(cmdr_t *c, cmdr_state s, const char *why) { c->state = s, c->why = why; }
// Said at once (published from inside the dial): a scan and a connect block for up to 20 s between turns.
static void dialling(cmdr_t *c, cmdr_state s, const char *why) {
  set(c, s, why);
  if (c->cfg.on_state) c->cfg.on_state(c->cfg.data_ctx);
}

static void clear_nav(cmdr_t *c) {
  c->has_dest = c->has_miles = c->has_minutes = c->has_arrive = c->has_recirc = c->has_massage_l = c->has_massage_r = 0;
  c->dest[0] = 0;
}

static void teardown(cmdr_t *c) {
  c->push_on = 0; // first: a push still in flight must not bring the car data back
  c->secure = 0;
  c->approve_at = -1;
  sec1_wipe(&c->sec);
  clear_nav(c);
  if (c->cfg.on_data) c->cfg.on_data(c->cfg.data_ctx, NULL, 0);
  c->push_info = "not connected";
  strcpy(c->car_link, "unknown");
  if (c->linked) c->io.disconnect(c->io.ctx);
  c->linked = 0;
  if (c->state == CS_READY || c->state == CS_APPROVING) c->state = CS_IDLE; // the caller says what next
}

static void schedule_retry(cmdr_t *c, const char *why, cmdr_state kind, int64_t now) {
  static const int delays[] = {2, 5, 10, 20, 30};
  if (!c->on) return;
  c->retry_at = -1;
  if (kind == CS_OFF) { // its coming back on dials (cmdr_adapter)
    c->attempt++;
    set(c, CS_OFF, why);
    return;
  }
  if (c->parked) { // a refusal is never retried on its own: wait for the owner's tap
    c->asks = 0;
    set(c, CS_REFUSED, why);
    return;
  }
  int secs = delays[c->attempt < 4 ? c->attempt : 4];
  c->attempt++;
  set(c, kind, why);
  c->retry_at = now + secs * 1000;
}

static int share(const cmdr_t *c) { return c->data_hold ? CMDR_NONE : CMDR_LEAN; }

// What a session dialled now asks for: WiFi's share while WiFi is live and for
// a spell after (a restarted Commander brings Bluetooth back first); else all.
static int dial_level(const cmdr_t *c, int64_t now) {
  return c->wifi_live || now - c->wifi_at < c->cfg.lean_for_ms ? share(c) : CMDR_FULL;
}

// What the running session should have: one that outlived WiFi takes all at once.
static int session_level(const cmdr_t *c, int64_t now) {
  return c->wifi_live || (c->subscribed_at > c->wifi_at && now - c->wifi_at < c->cfg.lean_for_ms) ? share(c)
                                                                                                    : CMDR_FULL;
}

static void refit_in(cmdr_t *c, int64_t ms, int64_t now) { c->refit_at = now + (ms > 0 ? ms : 0); }

static void refit_after_spell(cmdr_t *c, int64_t now) {
  if (c->level < CMDR_FULL && !c->wifi_live) refit_in(c, c->cfg.lean_for_ms - (now - c->wifi_at) + 1000, now);
}

static int subscribe(cmdr_t *c, int level, int64_t now) {
  uint8_t m[CMDR_MSG_MAX], r[CMDR_MSG_MAX];
  size_t n, rn;
  // No push channel, or flag 1 refused (pushes would share the cipher stream):
  // never a subscription, from any path (the phone app's _refit lacks this check).
  if (!c->push_on) return 0;
  c->level = level;
  c->subscribed_at = now;
  refit_after_spell(c, now);
  if (level == CMDR_NONE) {
    c->push_info = "buttons only: car data once the WiFi stream has settled";
    return 0;
  }
  n = level == CMDR_LEAN ? cmdr_subscribe(m, cmdr_lean_fields, sizeof cmdr_lean_fields)
                         : cmdr_subscribe(m, cmdr_all_fields, sizeof cmdr_all_fields);
  int e = request(c, m, n, &c->sub_status, r, &rn);
  if (e) return e;
  if (c->sub_status) {   // a Commander that refuses the newer fields: the rest without them
    n = level == CMDR_LEAN ? cmdr_subscribe(m, cmdr_lean_fields, sizeof cmdr_lean_fields - CMDR_LEAN_NEWER)
                           : cmdr_subscribe(m, cmdr_all_fields, sizeof cmdr_all_fields - CMDR_NEWER);
    if ((e = request(c, m, n, &c->sub_status, r, &rn))) return e;
  }
  c->push_info = level == CMDR_FULL ? "subscribed" : "subscribed to the route and recirculation only";
  // A field is pushed when it changes: ask for what it has now (48, read-only).
  uint64_t st;
  n = cmdr_trigger_push_req(m);
  return request(c, m, n, &st, r, &rn);
}

// After SendUUID said yes: pin, pushes, subscription, the car link. 0 = ready.
static int finish_open(cmdr_t *c, int64_t now) {
  uint8_t m[CMDR_MSG_MAX], r[CMDR_MSG_MAX];
  size_t n, rn;
  uint64_t st;
  int e;
  save(c->cfg.pin_path, c->addr); // approved: from now on the Commander this unit looks for
  c->level = CMDR_NONE;
  if (!(c->has & CMDR_HAS_PUSH)) {
    c->push_info = "no push channel";
  } else if (!c->sec.pushes_clear) {
    c->push_info = "Commander wants encrypted pushes: not subscribed"; // they would share the cipher stream
  } else {
    if (c->io.notify(c->io.ctx, 1)) return CMDR_E_IO;
    c->push_on = 1;
    if ((e = subscribe(c, dial_level(c, now), now))) return e;
  }
  n = cmdr_api_status_req(m);
  if ((e = request(c, m, n, &st, r, &rn))) return e;
  const uint8_t *body;
  size_t bn;
  if (pb_get_b(r, rn, 94, &body, &bn)) cmdr_api_words(body, bn, c->car_link, sizeof c->car_link);
  else strcpy(c->car_link, "no answer");
  c->parked = 0; // approved after all: reconnects on its own again
  c->attempt = c->asks = c->asked = 0;
  set(c, CS_READY, "ready");
  return 0;
}

// Dials once. Returns 0 ready, 1 approving (continues in cmdr_tick), <0 failed (*kind, *why set).
static int open_link(cmdr_t *c, int64_t now, cmdr_state *kind, const char **why) {
  uint8_t m[CMDR_MSG_MAX], r[CMDR_MSG_MAX], secret[32];
  size_t n, rn;
  char pin[CMDR_ADDR_MAX];
  int has_pin = load(c->cfg.pin_path, pin, sizeof pin);
  *kind = CS_RETRYING;
  if (!c->io.adapter_on(c->io.ctx)) return *kind = CS_OFF, *why = "bluetooth is off", -1;
  dialling(c, CS_SCANNING, "looking for the Commander");
  if (c->io.scan(c->io.ctx, has_pin ? pin : NULL, c->addr)) {
    *kind = CS_AWAY;
    *why = has_pin ? "this car's Commander not advertising (car asleep? phone app on it?)"
                   : "no Commander advertising (car asleep? phone app on it?)";
    return -1;
  }
  c->linked = 1; // held before connecting: a link nobody closes keeps the Commander from advertising
  dialling(c, CS_LINKING, "connecting to the Commander");
  if ((c->has = c->io.connect(c->io.ctx, c->addr)) < 0) return *why = "connect failed", -1;
  if ((c->has & (CMDR_HAS_SESSION | CMDR_HAS_API)) != (CMDR_HAS_SESSION | CMDR_HAS_API))
    return *why = "unknown Commander firmware", -1;

  *why = "handshake failed";
  if (c->io.random(c->io.ctx, secret, 32)) return -1;
  sec1_init(&c->sec, secret);
  crypto_wipe(secret, sizeof secret);
  if (!(n = sec1_hello(&c->sec, m, sizeof m)) || cmdr_send(c, CMDR_CHR_SESSION, m, n, r, sizeof r, &rn) ||
      !(n = sec1_answer(&c->sec, r, rn, m, sizeof m)) || cmdr_send(c, CMDR_CHR_SESSION, m, n, r, sizeof r, &rn) ||
      sec1_check(&c->sec, r, rn))
    return -1;
  c->secure = 1;

  if (phone_id(c)) return *why = "cannot store the phone id", -1;
  n = cmdr_send_uuid(m, c->id);
  if (request(c, m, n, &c->last_status, r, &rn)) return *why = "no answer, or out of step", -1;
  if (c->last_status != 0) {
    // Any refusal means not approved now. Unlike the phone app, never a prompt on its own:
    // only the owner's tap starts the double-press wait.
    if (!c->asked) {
      c->parked = 1;
      *kind = CS_REFUSED;
      *why = "not approved: tap the Bluetooth icon, then double-press the right scroll wheel";
      return -1;
    }
    if (c->io.now) now = c->io.now(c->io.ctx); // after the scan and connect: the owner gets the whole window
    c->approve_by = now + APPROVE_MS;
    c->approve_at = now + APPROVE_EVERY_MS;
    set(c, CS_APPROVING, "approve this unit: in the car, double-press the right scroll wheel");
    return 1;
  }
  if (finish_open(c, now)) return *why = "setup failed", -1;
  return 0;
}


static int ready(const cmdr_t *c) { return c->state == CS_READY; }
static int retry_pending(const cmdr_t *c) { return c->retry_at >= 0; }

// main.dart _btWaitsFor: why a dial from scratch is not to start now, or NULL.
static const char *wait_why(const cmdr_t *c) {
  if (c->cfg.wifi_first) return c->wifi_live ? NULL : "waiting for WiFi to go live";
  return c->wifi_trying ? "waiting for WiFi to try first" : NULL;
}

static void dial(cmdr_t *c, int64_t now) {
  c->retry_at = -1;
  // A link made before WiFi's had the Commander switch its WiFi off: wait out
  // WiFi's turn. No failure, so no backoff step. A link already up is left alone.
  const char *why = ready(c) ? NULL : wait_why(c);
  if (why) {
    set(c, CS_PAUSED, why);
    c->retry_at = now + WAIT_POLL_MS;
    return;
  }
  int spent = 0;
  // A tap kept through pauses and "not seen" does not stand for ever: hours on, nobody is there for the double-press
  if (c->asks > 0 && now - c->ask_at > ASK_KEEP_MS) c->asks = 0;
  if (!c->asked && c->asks > 0) { // the retry of an owner's ask is asked too
    c->asked = 1;
    c->asks--;
    spent = 1;
  }
  teardown(c);
  cmdr_state kind;
  int e = open_link(c, now, &kind, &why);
  if (c->io.now) now = c->io.now(c->io.ctx); // the dial can block 20 s: retries count from its end
  // not seen, or a dial that failed before the Commander answered (a connect, the handshake): nothing was asked, the
  // tap still stands (for its ASK_KEEP_MS), where two such failures left the next dial refused with no prompt
  if (e < 0 && spent) c->asks++;
  if (e < 0) {
    teardown(c);
    schedule_retry(c, why, kind, now);
  }
  if (e != 1) c->asked = 0; // an approval keeps the ask until it ends
}

static void lost(cmdr_t *c, const char *why, int64_t now) {
  if (!ready(c) && c->state != CS_APPROVING) return;
  teardown(c);
  c->asked = 0;   // an approval cut short ends the ask (its one retry is asks'): no prompt at every later dial
  schedule_retry(c, why, CS_RETRYING, now);
}

static void approve_step(cmdr_t *c, int64_t now) {
  uint8_t m[CMDR_MSG_MAX], r[CMDR_MSG_MAX];
  size_t rn, n = cmdr_send_uuid(m, c->id);
  c->approve_at = -1;
  if (request(c, m, n, &c->last_status, r, &rn)) {
    c->asked = 0;
    lost(c, "Commander out of step", now);
  } else if (c->last_status == 0) {
    c->asked = 0;
    if (finish_open(c, now)) lost(c, "setup failed", now);
  } else if (now >= c->approve_by) {
    c->asked = 0;
    c->parked = 1; // asked for a minute; again only when the owner taps
    lost(c, "not approved: tap the Bluetooth icon to try again", now);
  } else {
    c->approve_at = now + APPROVE_EVERY_MS;
  }
}

static void refit(cmdr_t *c, int64_t now) {
  c->refit_at = -1;
  if (!ready(c)) return;
  int want = session_level(c, now);
  if (want == c->level) {
    refit_after_spell(c, now); // short of all of it in the spell after WiFi went: look again once out
  } else if (want < c->level) {
    dial(c, now); // fewer fields only by dialling again
  } else if (subscribe(c, want, now)) {
    lost(c, "subscribe failed", now); // more on the same session, so the car data has no gap
  }
}

void cmdr_init(cmdr_t *c, const cmdr_cfg *cfg, const cmdr_io *io) {
  memset(c, 0, sizeof *c);
  c->cfg = cfg ? *cfg : cmdr_cfg_default;
  c->io = *io;
  c->why = "off";
  c->push_info = "not connected";
  strcpy(c->car_link, "unknown");
  c->wifi_at = c->subscribed_at = LONG_AGO;
  c->retry_at = c->approve_at = c->approve_by = c->refit_at = c->back_at = c->buttons_at = c->data_at = -1;
}

void cmdr_connect(cmdr_t *c, int64_t now) {
  c->on = 1;
  c->attempt = 0;
  dial(c, now);
}

void cmdr_tick(cmdr_t *c, int64_t now) {
  if (c->back_at >= 0 && now >= c->back_at) { // no stream came: Bluetooth carries the dash
    c->back_at = -1;
    cmdr_data_hold(c, 0, now);
    cmdr_resume(c, now);
  }
  if (c->buttons_at >= 0 && now >= c->buttons_at) {
    c->buttons_at = -1;
    int paused = c->paused, redial = c->redial;
    cmdr_resume(c, now);
    if (!(paused && redial)) cmdr_kick(c, 0, now); // the resume dialled already: a failed dial waits its backoff
  }
  if (c->data_at >= 0 && now >= c->data_at) {
    c->data_at = -1;
    cmdr_data_hold(c, 0, now);
  }
  if (c->on && c->retry_at >= 0 && now >= c->retry_at) dial(c, now);
  if (c->approve_at >= 0 && now >= c->approve_at) approve_step(c, now);
  if (c->refit_at >= 0 && now >= c->refit_at) refit(c, now);
}

void cmdr_kick(cmdr_t *c, int ask, int64_t now) {
  if (ask && c->paused && !ready(c)) {   // stepped aside for WiFi: the tap is kept, and dialled as WiFi lets go
    c->asks = 2, c->ask_at = now, c->parked = 0, c->attempt = 0, c->redial = 1;
    set(c, CS_PAUSED, "waiting for the WiFi stream");
    return;
  }
  if (!c->on || ready(c)) return;
  if (ask) {
    // A tap is two asks, the ask and its one retry (an approval cut short), whichever way it came: turned into "asked"
    // by dial alone, so a tap whose first dial misses, or one kept while stepped aside, has its retry as any other
    // (they had none). An approval already running is asked as it is.
    if (c->state == CS_APPROVING) c->asked = 1;
    c->asks = c->state == CS_APPROVING ? 1 : 2, c->ask_at = now;
    c->parked = 0;
    c->attempt = 0;
  }
  int waiting = retry_pending(c);
  if (c->state == CS_APPROVING || !(waiting || ask)) return;
  dial(c, now);
}

void cmdr_forget(cmdr_t *c, int64_t now) {
  if (c->cfg.pin_path) remove(c->cfg.pin_path);
  c->attempt = 0;
  if (ready(c)) lost(c, "looking for another Commander", now);
  else cmdr_kick(c, 1, now);
}

void cmdr_adapter(cmdr_t *c, int on, int64_t now) {
  if (on) {
    if (c->on && c->state == CS_OFF) dial(c, now); // off has no retry of its own: the look is ours
    else cmdr_kick(c, 0, now);
  } else if (c->on) {
    teardown(c);
    c->retry_at = -1;
    c->asked = 0;   // the controller back later: no prompt on its own (the tap's one retry stands for its 5 minutes)
    set(c, CS_OFF, "bluetooth is off");
  }
}

void cmdr_link_lost(cmdr_t *c, int64_t now) { lost(c, "link lost", now); }

void cmdr_on_push(cmdr_t *c, const uint8_t *d, size_t n) {
  if (!c->push_on) return;
  c->push_count++;
  cmdr_push p;
  // PushDeviceConnectionState (8) is read but not acted on: no Dash software runs here.
  if (cmdr_push_parse(d, n, &p) != 16) return; // a push we cannot read costs one update, not the link
  if (p.has_dest) { // proto3 optional: an explicit "" means no destination
    c->has_dest = p.dest[0] != 0;
    memcpy(c->dest, p.dest, sizeof c->dest);
  }
  if (p.has_miles) c->miles = p.miles, c->has_miles = 1;
  if (p.has_minutes) c->minutes = p.minutes, c->has_minutes = 1;
  if (p.has_arrive) c->arrive_pct = p.arrive_pct, c->has_arrive = 1;
  if (p.has_recirc) c->recirc = p.recirc, c->has_recirc = 1;
  if (p.has_massage_l) c->massage_l = p.massage_l, c->has_massage_l = 1;
  if (p.has_massage_r) c->massage_r = p.massage_r, c->has_massage_r = 1;
  if (c->cfg.on_data) c->cfg.on_data(c->cfg.data_ctx, p.data, p.data_len);
}

int64_t cmdr_until(const cmdr_t *c) {
  switch (c->state) {
  case CS_PAUSED: return c->buttons_at >= 0 ? c->buttons_at : c->back_at;
  case CS_AWAY: case CS_RETRYING: return c->retry_at;
  case CS_APPROVING: return c->approve_by;
  case CS_READY: return c->level == CMDR_NONE ? c->data_at : c->level == CMDR_LEAN && !c->wifi_live ? c->refit_at : -1;
  default: return -1;
  }
}

void cmdr_wifi(cmdr_t *c, int live, int trying, int64_t now) {
  c->wifi_trying = trying;
  if (live) c->wifi_at = now;
  if (live == c->wifi_live) return;
  c->wifi_live = live;
  // Once the change has held: less soon, all of it only once WiFi stayed away.
  refit_in(c, live ? 3000 : 10000, now);
  if (!live) return;
  // The stream runs: Bluetooth back after a moment, onto a Commander already streaming.
  c->back_at = -1;
  c->buttons_at = now + (c->cfg.wifi_first ? c->cfg.rejoin_ms : c->cfg.buttons_ms);
  c->data_at = now + c->cfg.rejoin_ms;
}

void cmdr_wifi_first(cmdr_t *c, int on, int64_t now) {
  c->cfg.wifi_first = on;
  // stepped aside while it waited for the WiFi stream, which now does not go first (Android Auto took the radio before
  // the stream came): back at the next tick. Set again before that tick (AA let go at once): stepped aside as it was
  // (a step-aside with wifi_first leaves no back_at)
  if (!on && c->paused && c->back_at < 0 && c->buttons_at < 0) c->back_at = now;
  else if (on && c->paused) c->back_at = -1;
}

int cmdr_step_aside(cmdr_t *c, int64_t now) {
  c->back_at = c->buttons_at = c->data_at = -1; // a new WiFi try: none of the last stream's timers fire in it
  cmdr_data_hold(c, 1, now);
  if (!c->cfg.wifi_first) c->back_at = now + c->cfg.back_ms;
  return cmdr_pause(c, now) ? c->cfg.settle_ms : 0; // a link was up: a moment before the hello
}

int cmdr_pause(cmdr_t *c, int64_t now) {
  (void)now;
  if (!c->on) return 0;
  int was = ready(c) || c->state == CS_APPROVING;
  // Left idle on purpose (parked after a refusal) stays idle; off stays said.
  c->redial = was || retry_pending(c) || c->state == CS_OFF;
  c->on = 0;
  c->paused = 1;
  c->asked = 0;
  c->retry_at = c->refit_at = -1;
  teardown(c);
  if (c->redial && c->state != CS_OFF) set(c, CS_PAUSED, "stepped aside for the WiFi stream");
  return was;
}

void cmdr_resume(cmdr_t *c, int64_t now) {
  if (!c->paused) return;
  c->paused = 0;
  c->on = 1;
  if (c->redial) dial(c, now);
}

void cmdr_data_hold(cmdr_t *c, int hold, int64_t now) {
  if (hold == c->data_hold) return;
  c->data_hold = hold;
  if (!hold && c->wifi_live) refit_in(c, 0, now);
}

int cmdr_press_guard(cmdr_action a, int parked, char gear) {
  if (!cmdr_action_at(a)) return CMDR_E_DENIED;
  if (cmdr_action_opens(a) && !parked) return CMDR_E_PARK; // a lid rising at speed blinds the driver
  if (cmdr_action_at(a)->id == 17 && (gear == 'P' || gear == 'N' || gear == 'D')) return CMDR_E_GEAR; // tilt: acts in R only
  return 0;
}

int cmdr_press(cmdr_t *c, cmdr_action a, int parked, char gear, int64_t now) {
  if (!ready(c)) { // answered now; the look is the next tick's (a dial here would hold the answer up to 20 s)
    if (c->paused) cmdr_kick(c, 1, now); // kept for when WiFi lets go (no dial)
    else if (c->on && c->state != CS_APPROVING) c->asks = 2, c->ask_at = now, c->parked = 0, c->attempt = 0, c->retry_at = now;
    return CMDR_E_NOT_READY;
  }
  int g = cmdr_press_guard(a, parked, gear);
  if (g) return g;
  uint8_t m[CMDR_MSG_MAX], r[CMDR_MSG_MAX];
  size_t rn, n = cmdr_try_action(m, a);
  if (request(c, m, n, &c->last_status, r, &rn)) {
    // The session is spent either way. The Commander acts on the write, so a
    // lost reply may mean it went through: never resent on its own.
    lost(c, "no answer: it may have gone through", now);
    return CMDR_E_NO_ANSWER;
  }
  return c->last_status ? CMDR_E_STATUS : CMDR_OK;
}
