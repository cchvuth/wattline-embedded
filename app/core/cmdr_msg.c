#include "cmdr_msg.h"
#include "pb.h"
#include <stdio.h>
#include <string.h>

#include "../build/car_actions_gen.h"

const cmdr_action_def *cmdr_action_at(int a) {
  return a >= 0 && a < ACT_COUNT ? &cmdr_actions[a] : a >= ACT_COUNT && a < ACT_COUNT + cmdr_car_opt_count ? &cmdr_car_opts[a - ACT_COUNT] : NULL;
}

const cmdr_action_def cmdr_actions[ACT_COUNT] = {
    [ACT_FRUNK] = {2, 0, "Frunk"},
    [ACT_TRUNK] = {3, 0, "Trunk"},
    [ACT_LIGHT] = {12, 0, "Light"},
    [ACT_MIRROR_TILT] = {17, 0, "Mirror tilt"}, // auto-tilt override; acts in Reverse only
    [ACT_RECIRC_ON] = {48, 2, "Recirculation"},
    [ACT_RECIRC_OFF] = {48, 3, "Fresh air"},
    [ACT_RECIRC_TOGGLE] = {48, 4, "Recirculation"},
    [ACT_DOOR_FL] = {40, 1, "Front left door"},
    [ACT_DOOR_FR] = {40, 2, "Front right door"},
    [ACT_DOOR_RL] = {40, 3, "Rear left door"},
    [ACT_DOOR_RR] = {40, 4, "Rear right door"},
    [ACT_UNLOCK] = {53, 1, "Unlock"},
    [ACT_LOCK] = {53, 2, "Lock"},
    [ACT_CHARGE_PORT] = {8, 0, "Charge port"},
    [ACT_HONK] = {15, 1, "Honk"},
    [ACT_FLASH] = {33, 6, "Flash"},
    [ACT_CLIMATE] = {70, 0, "Climate"},
    // the front seats' lumbar massage: 98 driver, 99 passenger; 3 soft, 2 pulse, 4 hard (the running one again: off)
    [ACT_MASSAGE_L_SOFT] = {98, 3, "Left Seat Massage: Soft"},
    [ACT_MASSAGE_L_PULSE] = {98, 2, "Left Seat Massage: Pulse"},
    [ACT_MASSAGE_L_HARD] = {98, 4, "Left Seat Massage: Hard"},
    [ACT_MASSAGE_R_SOFT] = {99, 3, "Right Seat Massage: Soft"},
    [ACT_MASSAGE_R_PULSE] = {99, 2, "Right Seat Massage: Pulse"},
    [ACT_MASSAGE_R_HARD] = {99, 4, "Right Seat Massage: Hard"},
};

int cmdr_action_opens(cmdr_action a) {
  const cmdr_action_def *d = cmdr_action_at(a);
  unsigned id = d ? d->id : 0;
  return id == 2 || id == 3 || id == 40;
}

const char *const cmdr_statuses[9] = {"ok", "invalid security scheme", "invalid proto", "too many sessions",
                                      "invalid argument", "internal error", "crypto error", "invalid session",
                                      "not supported"};

const uint8_t cmdr_lean_fields[7] = {6, 7, 8, 9, 101, 130, 131};
// _wifiLacks + Panda.btFields + Panda.btTraceFields (lib/panda.dart), + the Dash's 24 display_brightness, then the
// CMDR_NEWER fields last (Commander._newer).
const uint8_t cmdr_all_fields[56] = {6, 7, 8, 9, 101, 27, 32, 34, 35, 36, 37, 42, 43, 44, 49,
                                     51, 53, 61, 62, 63, 64, 65, 66, 80, 81, 82, 85, 105, 106, 107,
                                     108, 110, 111, 112, 113, 114, 115, 116, 117, 118, 28, 29, 77, 78, 24,
                                     125, 126, 127, 128, 130, 131, 132, 133, 134, 135, 159};

uint32_t cmdr_payload_field(uint64_t type) {
  switch (type) {
  case 32: return 43;   // SendUUID
  case 28: return 39;   // TryAction
  case 124: return 135; // SubscribeVehicleData
  case 82: return 93;   // ReqGetTeslaAPIStatus (read-only)
  case 48: return 59;   // ReqDashTriggerAllDataPush (read-only)
  default: return 0;    // everything else, 84, 130, 132/134, 86/88/90 included
  }
}

int cmdr_req_ok(const uint8_t *m, size_t n, uint64_t *type) {
  pb_in in = {m, n, 0};
  pb_val v;
  int r, have_type = 0, have_body = 0;
  uint32_t want = 0;
  while ((r = pb_next(&in, &v)) == 1) {
    if (v.field == 1 && v.wt == 0 && !have_type) {
      have_type = 1;
      *type = v.v;
      if (!(want = cmdr_payload_field(v.v))) return 0;
    } else if (have_type && v.field == want && v.wt == 2 && !have_body) {
      have_body = 1;
    } else {
      return 0;
    }
  }
  return r == 0 && have_type && have_body;
}

static size_t req(uint8_t *out, unsigned type, const uint8_t *body, size_t n) {
  pb_out o = {out, 0, CMDR_MSG_MAX, 0};
  pb_u(&o, 1, type);
  pb_b(&o, cmdr_payload_field(type), body, n);
  return o.bad ? 0 : o.n;
}

size_t cmdr_send_uuid(uint8_t *out, const char *id) {
  uint8_t b[64];
  pb_out o = {b, 0, sizeof b, 0};
  pb_b(&o, 1, (const uint8_t *)id, strlen(id));
  return o.bad ? 0 : req(out, 32, b, o.n);
}

// ReqTryAction{PressActionConfig{action, switch type 1 (on/off), [state]}}.
size_t cmdr_try_action(uint8_t *out, cmdr_action a) {
  const cmdr_action_def *d = cmdr_action_at(a);
  if (!d) return 0;
  uint8_t st[10], cfg[32], b[40];
  pb_out s = {st, 0, sizeof st, 0}, c = {cfg, 0, sizeof cfg, 0}, o = {b, 0, sizeof b, 0};
  pb_varint(&s, d->state);
  pb_u(&c, 1, d->id);
  pb_u(&c, 2, 1);
  pb_b(&c, 3, st, s.n);
  pb_b(&o, 1, cfg, c.n);
  return req(out, 28, b, o.n);
}

size_t cmdr_subscribe(uint8_t *out, const uint8_t *fields, size_t n) {
  uint8_t packed[128], b[140];
  pb_out p = {packed, 0, sizeof packed, 0}, o = {b, 0, sizeof b, 0};
  for (size_t i = 0; i < n; i++) pb_varint(&p, fields[i]);
  pb_b(&o, 13, packed, p.n);
  return p.bad || o.bad ? 0 : req(out, 124, b, o.n);
}

size_t cmdr_api_status_req(uint8_t *out) { return req(out, 82, 0, 0); }
size_t cmdr_trigger_push_req(uint8_t *out) { return req(out, 48, 0, 0); }

int cmdr_reply_status(const uint8_t *r, size_t n, uint64_t *type, uint64_t *status) {
  pb_val v, t;
  if (!pb_ok(r, n)) return -1;
  int has = pb_get(r, n, 1, &t);
  if (has && t.wt != 0) return -1; // Dart's `as int?` throws
  *type = has ? t.v : 0;
  *status = 0;
  // The first field (in order) whose last value is bytes: Dart's f.values.whereType<List<int>>().first.
  pb_in in = {r, n, 0};
  while (pb_next(&in, &v) == 1) {
    pb_val last;
    pb_get(r, n, v.field, &last);
    if (last.wt != 2) continue;
    if (!pb_ok(last.p, last.len)) return -1;
    has = pb_get(last.p, last.len, 1, &t);
    if (has && t.wt != 0) return -1;
    *status = has ? t.v : 0;
    break;
  }
  return 0;
}

int cmdr_push_parse(const uint8_t *d, size_t n, cmdr_push *p) {
  memset(p, 0, sizeof *p);
  p->dev_on = -1;
  uint64_t type;
  const uint8_t *b;
  size_t bn;
  pb_val v;
  if (!pb_ok(d, n) || !pb_get_u(d, n, 1, &type)) return 0; // unreadable: one update lost, not the link
  if (type == 8 && pb_get_b(d, n, 10, &b, &bn)) {
    if (!pb_ok(b, bn)) return 0;
    uint64_t st = 0;
    pb_get_u(b, bn, 2, &st);
    p->dev_on = st == 0 || st == 8 ? 1 : st == 2 || st == 3 || st == 4 || st == 5 || st == 7 ? 0 : -1;
    pb_get_u(b, bn, 6, &p->dev_type);
    return p->kind = 8;
  }
  if (type != 16 || !pb_get_b(d, n, 25, &b, &bn) || !pb_ok(b, bn)) return 0;
  p->data = b;
  p->data_len = bn;
  if (pb_get(b, bn, 6, &v) == 1 && v.wt == 2) {
    size_t k = v.len < sizeof p->dest - 1 ? v.len : sizeof p->dest - 1;
    if (k < v.len) while (k && (v.p[k] & 0xc0) == 0x80) k--; // cut on a UTF-8 boundary
    memcpy(p->dest, v.p, k);
    p->dest[k] = 0;
    p->has_dest = 1;
  }
  if (pb_get(b, bn, 7, &v) == 1 && v.wt == 5) p->miles = v.f, p->has_miles = 1;
  if (pb_get(b, bn, 8, &v) == 1 && v.wt == 5) p->minutes = v.f, p->has_minutes = 1;
  if (pb_get(b, bn, 9, &v) == 1 && v.wt == 5) p->arrive_pct = v.f, p->has_arrive = 1;
  if (pb_get_u(b, bn, 101, &p->recirc)) p->has_recirc = 1;
  if (pb_get_u(b, bn, 130, &p->massage_l)) p->has_massage_l = 1;
  if (pb_get_u(b, bn, 131, &p->massage_r)) p->has_massage_r = 1;
  return p->kind = 16;
}

static const char *name(const char *const *names, size_t k, long long v, char *tmp) {
  if (v >= 0 && (size_t)v < k) return names[v];
  if (v < 0) return "\xe2\x80\x94"; // em dash: absent
  snprintf(tmp, 24, "%lld", v);
  return tmp;
}

void cmdr_api_words(const uint8_t *b, size_t n, char *out, size_t cap) {
  static const char *const link[] = {"initial", "partly configured", "configured", "connected", "disconnected", "error"};
  static const char *const errs[] = {"", "unknown error", "not fully configured", "failed to configure",
                                     "car not found", "failed to connect"};
  static const char *const sess[] = {"initial", "waiting", "ok", "error"};
  static const char *const ui[] = {"not paired", "awaiting connection", "paired", "not available"};
  uint64_t u;
  long long l = pb_get_u(b, n, 1, &u) ? (long long)u : 0, e = pb_get_u(b, n, 2, &u) ? (long long)u : 0,
            pair = pb_get_u(b, n, 6, &u) ? (long long)u : 0, info = -1;
  // Session statuses repeat as field 3: keep the infotainment one (domain 4).
  // Walked as apiWords does: one-byte tags and lengths.
  for (size_t i = 0; i < n;) {
    uint8_t tag = b[i++];
    if ((tag & 7) == 0) {
      while (i < n && b[i++] & 0x80) {}
    } else if ((tag & 7) == 2 && i < n) {
      size_t k = b[i++];
      if (k > n - i) break;
      uint64_t dom, st = 0;
      if (tag >> 3 == 3 && pb_ok(b + i, k) && pb_get_u(b + i, k, 1, &dom) && dom == 4) {
        pb_get_u(b + i, k, 2, &st); // proto3 leaves 0 (initial) out
        info = (long long)st;
      }
      i += k;
    } else {
      break;
    }
  }
  char t1[24], t2[24], t3[24], t4[24], err[48] = "";
  if (e > 0) snprintf(err, sizeof err, " (%s)", name(errs, 6, e, t2));
  snprintf(out, cap, "car link %s%s, infotainment %s, %s", name(link, 6, l, t1), err, name(sess, 4, info, t3),
           name(ui, 4, pair, t4));
}
