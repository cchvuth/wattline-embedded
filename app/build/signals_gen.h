// Generated from lib/signals.dart (+ tools/extra_signals.json) by gen_signals.py: 124 signals, kSignals order.
#include <stdint.h>
typedef struct { const char *name; uint16_t id; uint8_t start, len; double scale, offset;
  uint8_t is_signed; int32_t sna; int16_t mux; uint8_t mux_start, mux_len; } sig_t; // sna/mux -1 = none
static const sig_t kSignals[] = {
  {"speed", 0x257, 24, 9, 1, 0, 0, 511, -1, 0, 3},
  {"speedIsKph", 0x257, 33, 1, 1, 0, 0, -1, -1, 0, 3},
  {"gear", 0x118, 21, 3, 1, 0, 0, -1, -1, 0, 3},
  {"pedal", 0x118, 32, 8, 0.4, 0, 0, 255, -1, 0, 3},
  {"accel", 0x118, 52, 12, 0.01, 0, 1, 2048, -1, 0, 3},
  {"yaw", 0x101, 0, 16, 0.0001, 0, 1, 32768, -1, 0, 3},
  {"brake", 0x118, 51, 1, 1, 0, 0, -1, -1, 0, 3},
  {"brakeFL", 0x185, 0, 12, 2, 0, 0, 4095, -1, 0, 3},
  {"brakeFR", 0x185, 12, 12, 2, 0, 0, 4095, -1, 0, 3},
  {"brakeRL", 0x185, 24, 12, 2, 0, 0, 4095, -1, 0, 3},
  {"brakeRR", 0x185, 36, 12, 2, 0, 0, 4095, -1, 0, 3},
  {"steerSccm", 0x129, 16, 14, 0.1, -819.2, 0, 16383, -1, 0, 3},
  {"frontKw", 0x2E5, 41, 11, 0.5, 0, 1, 1024, -1, 0, 3},
  {"rearKw", 0x266, 41, 11, 0.5, 0, 1, 1024, -1, 0, 3},
  {"packV", 0x132, 0, 16, 0.01, 0, 0, -1, -1, 0, 3},
  {"packA", 0x132, 16, 16, 0.1, 0, 1, 32768, -1, 0, 3},
  {"compV", 0x2A7, 41, 11, 0.5, 0, 0, -1, -1, 0, 3},
  {"compW", 0x2A7, 21, 11, 10, 0, 0, -1, -1, 0, 3},
  {"lvV", 0x2B4, 0, 13, 0.01, 0, 0, -1, -1, 0, 3},
  {"lvA", 0x2B4, 32, 13, 0.1, 0, 1, -1, -1, 0, 3},
  {"heatL", 0x2B3, 16, 10, 5, 0, 0, -1, 2, 0, 5},
  {"heatR", 0x2B3, 26, 10, 5, 0, 0, -1, 2, 0, 5},
  {"soc", 0x33A, 27, 7, 1, 0, 0, -1, -1, 0, 3},
  {"rangeMi", 0x33A, 0, 10, 1, 0, 0, -1, -1, 0, 3},
  {"cellVMax", 0x332, 2, 12, 0.002, 0, 0, -1, 1, 0, 2},
  {"cellVMin", 0x332, 16, 12, 0.002, 0, 0, -1, 1, 0, 2},
  {"packKwh", 0x352, 16, 16, 0.02, 0, 0, -1, 0, 0, 8},
  {"packLeftKwh", 0x352, 32, 16, 0.02, 0, 0, -1, 0, 0, 8},
  {"packBufferKwh", 0x352, 16, 16, 0.01, 0, 0, -1, 1, 0, 8},
  {"packNewKwh", 0x392, 40, 10, 0.1, 0, 0, 1023, 1, 0, 8},
  {"domeLight", 0x273, 59, 2, 1, 0, 0, -1, -1, 0, 3},
  {"tiltAuto", 0x273, 53, 1, 1, 0, 0, -1, -1, 0, 3},
  {"mapLights", 0x3E2, 6, 8, 1, 0, 0, -1, -1, 0, 3},
  {"doorFL", 0x102, 0, 4, 1, 0, 0, 0, -1, 0, 3},
  {"doorRL", 0x102, 4, 4, 1, 0, 0, 0, -1, 0, 3},
  {"doorFR", 0x103, 0, 4, 1, 0, 0, 0, -1, 0, 3},
  {"doorRR", 0x103, 4, 4, 1, 0, 0, 0, -1, 0, 3},
  {"trunk", 0x103, 56, 4, 1, 0, 0, 0, -1, 0, 3},
  {"mirrorDipped", 0x102, 63, 1, 1, 0, 0, -1, -1, 0, 3},
  {"frunk", 0x2E1, 3, 4, 1, 0, 0, 0, 0, 0, 3},
  {"chargePort", 0x25D, 6, 1, 1, 0, 0, -1, -1, 0, 3},
  {"beltDriver", 0x3A1, 32, 2, 1, 0, 0, -1, 0, 0, 1},
  {"beltPass", 0x3A1, 34, 2, 1, 0, 0, -1, 0, 0, 1},
  {"belt2L", 0x3A1, 36, 2, 1, 0, 0, -1, 0, 0, 1},
  {"belt2C", 0x3A1, 38, 2, 1, 0, 0, -1, 0, 0, 1},
  {"belt2R", 0x3A1, 40, 2, 1, 0, 0, -1, 0, 0, 1},
  {"belt3L", 0x3A1, 2, 2, 1, 0, 0, -1, 1, 0, 1},
  {"belt3R", 0x3A1, 4, 2, 1, 0, 0, -1, 1, 0, 1},
  {"buckleD", 0x3C2, 48, 2, 1, 0, 0, -1, 0, 0, 1},
  {"buckle2L", 0x3C2, 52, 2, 1, 0, 0, -1, 0, 0, 1},
  {"buckle2C", 0x3C2, 62, 2, 1, 0, 0, -1, 0, 0, 1},
  {"buckleP", 0x31A, 3, 2, 1, 0, 0, -1, -1, 0, 3},
  {"buckle2R", 0x31A, 8, 2, 1, 0, 0, -1, -1, 0, 3},
  {"buckle3L", 0x31A, 12, 2, 1, 0, 0, -1, -1, 0, 3},
  {"buckle3R", 0x31A, 14, 2, 1, 0, 0, -1, -1, 0, 3},
  {"lowBeam", 0x3F5, 32, 4, 1, 0, 0, -1, -1, 0, 3},
  {"drl", 0x3F5, 32, 4, 1, 0, 0, -1, -1, 0, 3},
  {"highBeam", 0x3F5, 36, 2, 1, 0, 0, -1, -1, 0, 3},
  {"highLamp", 0x3F6, 4, 4, 1, 0, 0, -1, -1, 0, 3},
  {"recirc", 0x243, 48, 2, 1, 0, 0, -1, 0, 0, 3},
  {"hvacPower", 0x243, 51, 3, 1, 0, 0, -1, 0, 0, 3},
  {"recircPct", 0x243, 10, 6, 1.6, 0, 0, -1, 1, 0, 3},
  {"speedLimit", 0x399, 8, 5, 5, 0, 0, -1, -1, 0, 3},
  {"autopilot", 0x399, 0, 4, 1, 0, 0, -1, -1, 0, 3},
  {"blindL", 0x399, 4, 2, 1, 0, 0, -1, -1, 0, 3},
  {"blindR", 0x399, 6, 2, 1, 0, 0, -1, -1, 0, 3},
  {"sideWarn", 0x399, 34, 2, 1, 0, 0, -1, -1, 0, 3},
  {"indL", 0x3F5, 0, 2, 1, 0, 0, -1, -1, 0, 3},
  {"indR", 0x3F5, 2, 2, 1, 0, 0, -1, -1, 0, 3},
  {"tpFL", 0x219, 16, 8, 0.025, 0, 0, 255, 5, 0, 3},
  {"tpFR", 0x219, 24, 8, 0.025, 0, 0, 255, 5, 0, 3},
  {"tpRL", 0x219, 32, 8, 0.025, 0, 0, 255, 5, 0, 3},
  {"tpRR", 0x219, 40, 8, 0.025, 0, 0, 255, 5, 0, 3},
  {"tdFL", 0x25A, 0, 8, 0.025, 0, 0, 255, -1, 0, 3},
  {"tdFR", 0x25A, 8, 8, 0.025, 0, 0, 255, -1, 0, 3},
  {"tdRL", 0x25A, 16, 8, 0.025, 0, 0, 255, -1, 0, 3},
  {"tdRR", 0x25A, 24, 8, 0.025, 0, 0, 255, -1, 0, 3},
  {"navEnergyKwh", 0x347, 48, 16, 0.01, 0, 1, -1, -1, 0, 3},
  {"navRoute", 0x238, 29, 1, 1, 0, 0, -1, -1, 0, 3},
  {"navMinutes", 0x3FD, 17, 10, 1, 0, 0, -1, 2, 0, 3},
  {"outside", 0x321, 40, 8, 0.5, -40, 0, 0, -1, 0, 3},
  {"washer", 0x321, 32, 2, 1, 0, 0, -1, -1, 0, 3},
  {"slippery", 0x2B6, 13, 1, 1, 0, 0, -1, -1, 0, 3},
  {"tcFlash", 0x2B6, 2, 1, 1, 0, 0, -1, -1, 0, 3},
  {"escFlash", 0x2B6, 0, 1, 1, 0, 0, -1, -1, 0, 3},
  {"tcMode", 0x118, 40, 3, 1, 0, 0, -1, -1, 0, 3},
  {"regenMax", 0x252, 0, 16, 0.01, 0, 0, -1, -1, 0, 3},
  {"packTMin", 0x332, 24, 8, 0.5, -40, 0, -1, 0, 0, 2},
  {"motorHeat", 0x3A1, 10, 3, 1, 0, 0, -1, 0, 0, 1},
  {"pumpHeat", 0x3A1, 41, 7, 100, 0, 0, -1, 1, 0, 1},
  {"precondState", 0x347, 5, 2, 1, 0, 0, -1, -1, 0, 3},
  {"precondTarget", 0x347, 16, 8, 0.25, 0, 0, 255, -1, 0, 3},
  {"co2", 0x2B3, 7, 10, 5, 0, 0, -1, 9, 0, 5},
  {"cabin1", 0x2B3, 24, 8, 0.5, -40, 0, 255, 1, 0, 5},
  {"cabin2", 0x2B3, 16, 8, 0.5, -40, 0, 255, 0, 0, 5},
  {"cabin3", 0x2B3, 40, 8, 0.5, -40, 0, 255, 0, 0, 5},
  {"climateOn", 0x2F3, 26, 3, 1, 0, 0, -1, -1, 0, 3},
  {"autoBlower", 0x2F3, 5, 3, 1, 0, 0, -1, -1, 0, 3},
  {"fan1", 0x2F3, 16, 4, 1, 0, 0, -1, -1, 0, 3},
  {"fan2", 0x2F3, 29, 3, 1, 0, 0, -1, -1, 0, 3},
  {"handsOn", 0x399, 42, 4, 1, 0, 0, -1, -1, 0, 3},
  {"cruiseState", 0x286, 12, 3, 1, 0, 0, -1, -1, 0, 3},
  {"cruiseSet", 0x286, 15, 9, 0.5, 0, 0, -1, -1, 0, 3},
  {"cruiseKph", 0x286, 24, 1, 1, 0, 0, -1, -1, 0, 3},
  {"followGap", 0x3F8, 45, 3, 1, 0, 0, 7, -1, 0, 3},
  {"chargeStatus", 0x212, 11, 3, 1, 0, 0, -1, -1, 0, 3},
  {"chargeKw", 0x212, 14, 16, 0.05, 0, 0, 65535, -1, 0, 3},
  {"chargeMaxKw", 0x212, 39, 12, 0.125, 0, 0, 4095, -1, 0, 3},
  {"chargeLineKw", 0x264, 24, 8, 0.1, 0, 0, -1, -1, 0, 3},
  {"chargeLineV", 0x264, 0, 14, 0.033, 0, 0, -1, -1, 0, 3},
  {"chargeLineA", 0x264, 14, 9, 0.1, 0, 0, -1, -1, 0, 3},
  {"chargeAcOn", 0x264, 23, 1, 1, 0, 0, -1, -1, 0, 3},
  {"chargeLimitA", 0x264, 32, 10, 0.1, 0, 0, -1, -1, 0, 3},
  {"chargeWhy", 0x472, 32, 3, 1, 0, 0, -1, -1, 0, 3},
  {"chargeLimit", 0x333, 16, 10, 0.1, 0, 0, -1, -1, 0, 3},
  {"chargeMinutes", 0x33B, 16, 11, 1, 0, 0, -1, -1, 0, 3},
  {"utcYear", 0x318, 0, 8, 1, 2000, 0, -1, -1, 0, 3},
  {"utcMonth", 0x318, 8, 8, 1, 0, 0, -1, -1, 0, 3},
  {"utcSeconds", 0x318, 16, 8, 1, 0, 0, -1, -1, 0, 3},
  {"utcHour", 0x318, 24, 8, 1, 0, 0, -1, -1, 0, 3},
  {"utcDay", 0x318, 32, 8, 1, 0, 0, -1, -1, 0, 3},
  {"utcMinutes", 0x318, 40, 8, 1, 0, 0, -1, -1, 0, 3},
  {"displayBrightness", 0x273, 32, 8, 0.5, 0, 0, 255, -1, 0, 3},
  {"socSoe", 0x33A, 20, 7, 1, 0, 0, -1, -1, 0, 3},
};
enum { kSignalCount = 124, kPhoneSignalCount = 116 };
enum { SIG_speed, SIG_speedIsKph, SIG_gear, SIG_pedal, SIG_accel, SIG_yaw, SIG_brake, SIG_brakeFL, SIG_brakeFR, SIG_brakeRL, SIG_brakeRR, SIG_steerSccm, SIG_frontKw, SIG_rearKw, SIG_packV, SIG_packA, SIG_compV, SIG_compW, SIG_lvV, SIG_lvA, SIG_heatL, SIG_heatR, SIG_soc, SIG_rangeMi, SIG_cellVMax, SIG_cellVMin, SIG_packKwh, SIG_packLeftKwh, SIG_packBufferKwh, SIG_packNewKwh, SIG_domeLight, SIG_tiltAuto, SIG_mapLights, SIG_doorFL, SIG_doorRL, SIG_doorFR, SIG_doorRR, SIG_trunk, SIG_mirrorDipped, SIG_frunk, SIG_chargePort, SIG_beltDriver, SIG_beltPass, SIG_belt2L, SIG_belt2C, SIG_belt2R, SIG_belt3L, SIG_belt3R, SIG_buckleD, SIG_buckle2L, SIG_buckle2C, SIG_buckleP, SIG_buckle2R, SIG_buckle3L, SIG_buckle3R, SIG_lowBeam, SIG_drl, SIG_highBeam, SIG_highLamp, SIG_recirc, SIG_hvacPower, SIG_recircPct, SIG_speedLimit, SIG_autopilot, SIG_blindL, SIG_blindR, SIG_sideWarn, SIG_indL, SIG_indR, SIG_tpFL, SIG_tpFR, SIG_tpRL, SIG_tpRR, SIG_tdFL, SIG_tdFR, SIG_tdRL, SIG_tdRR, SIG_navEnergyKwh, SIG_navRoute, SIG_navMinutes, SIG_outside, SIG_washer, SIG_slippery, SIG_tcFlash, SIG_escFlash, SIG_tcMode, SIG_regenMax, SIG_packTMin, SIG_motorHeat, SIG_pumpHeat, SIG_precondState, SIG_precondTarget, SIG_co2, SIG_cabin1, SIG_cabin2, SIG_cabin3, SIG_climateOn, SIG_autoBlower, SIG_fan1, SIG_fan2, SIG_handsOn, SIG_cruiseState, SIG_cruiseSet, SIG_cruiseKph, SIG_followGap, SIG_chargeStatus, SIG_chargeKw, SIG_chargeMaxKw, SIG_chargeLineKw, SIG_chargeLineV, SIG_chargeLineA, SIG_chargeAcOn, SIG_chargeLimitA, SIG_chargeWhy, SIG_chargeLimit, SIG_chargeMinutes, SIG_utcYear, SIG_utcMonth, SIG_utcSeconds, SIG_utcHour, SIG_utcDay, SIG_utcMinutes, SIG_displayBrightness, SIG_socSoe };
// Intel (DBC @1) bit order, as signals.dart _bits().
static inline uint64_t sig_bits(const uint8_t *d, int start, int len) {
  uint64_t v = 0;
  for (int i = 0; i < len; i++) { int b = start + i; if ((d[b >> 3] >> (b & 7)) & 1) v |= 1ull << i; }
  return v;
}
// signals.dart decode(): 0 = null (too short, other mux page, SNA).
static inline int sig_decode(const sig_t *s, const uint8_t *d, int n, double *out) {
  if ((s->start + s->len + 7) / 8 > n) return 0;
  if (s->mux >= 0 && (int64_t)sig_bits(d, s->mux_start, s->mux_len) != s->mux) return 0;
  int64_t v = (int64_t)sig_bits(d, s->start, s->len);
  if (s->sna >= 0 && v == s->sna) return 0;          // raw unsigned compare, before the sign
  if (s->is_signed && (v & (1ll << (s->len - 1)))) v -= 1ll << s->len;
  *out = (double)v * s->scale + s->offset;           // double: tests use 1e-9 tolerances
  return 1;
}

// Value tables: index by the raw value; LUT() is NULL for a value without a name (a Dart null).
static const char *const kGears[5] = {0, "P", "R", "N", "D"};
static const char *const kAutopilot[15] = {0, 0, "AP READY", "AUTOPILOT", "AUTOPILOT", "NAV ON AP", "FSD", 0, "AP ENDING", "AP ENDED", 0, 0, 0, 0, "AP FAULT"};
static const char *const kChargeWhy[5] = {0, "CHARGER", "COLD PACK", "FULL", "MOVE STALL"};
static const char *const kRecirc[3] = {"AUTO", "RECIRC", "FRESH"};
static const char *const kDome[3] = {"OFF", "ON", "AUTO"};
static const char *const kFan2[5] = {"AUTO", "OFF", "LOW", "MED", "HIGH"};
static const char *const kAutoBlower[5] = {"LOW", "LOW", "MED", "HIGH", "HIGH"};
#define LUT(t, k) ((k) >= 0 && (k) < (int)(sizeof t / sizeof *t) ? t[k] : (const char *)0)
static inline int latch_open(int v) { return v == 1 || v == 4 || v == 5; }

enum { kSubscribeCount = 45, kPhoneSubscribeCount = 44 };
static const uint16_t kSubscribeIds[] = {0x257, 0x118, 0x101, 0x185, 0x129, 0x2E5, 0x266, 0x132, 0x2A7, 0x2B4, 0x2B3, 0x33A, 0x332, 0x352, 0x392, 0x273, 0x3E2, 0x102, 0x103, 0x2E1, 0x25D, 0x3A1, 0x3C2, 0x31A, 0x3F5, 0x3F6, 0x243, 0x399, 0x219, 0x25A, 0x347, 0x238, 0x3FD, 0x321, 0x2B6, 0x252, 0x2F3, 0x286, 0x3F8, 0x212, 0x264, 0x472, 0x333, 0x33B, 0x318};
static const uint8_t kIdSigs[] = {0, 1, 2, 3, 4, 6, 85, 5, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 92, 93, 94, 95, 22, 23, 123, 24, 25, 87, 26, 27, 28, 29, 30, 31, 122, 32, 33, 34, 38, 35, 36, 37, 39, 40, 41, 42, 43, 44, 45, 46, 47, 88, 89, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 67, 68, 58, 59, 60, 61, 62, 63, 64, 65, 66, 100, 69, 70, 71, 72, 73, 74, 75, 76, 77, 90, 91, 78, 79, 80, 81, 82, 83, 84, 86, 96, 97, 98, 99, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112, 113, 114, 115, 116, 117, 118, 119, 120, 121};
static const uint8_t kIdFirst[] = {0, 2, 7, 8, 12, 13, 14, 15, 17, 19, 21, 27, 30, 33, 36, 37, 40, 41, 44, 47, 48, 49, 58, 61, 65, 70, 71, 74, 80, 84, 88, 91, 92, 93, 95, 98, 99, 103, 106, 107, 110, 115, 116, 117, 118, 124}; // kIdSigs[kIdFirst[n] .. kIdFirst[n+1]) for kSubscribeIds[n]
static inline int sub_index(unsigned id) { for (int n = 0; n < kSubscribeCount; n++) if (kSubscribeIds[n] == id) return n; return -1; }

// Panda.alwaysOn (+ the Dash's), _sparse, _wheel, _chargeGroup/_steerGroup/_chipGroups, btFields.
enum { kAlwaysOnCount = 31, kGroupCount = 8, kWheelSlots = 16, kChargeGroup = 5, kSteerGroup = 7, kBtFieldCount = 44 };
static const uint16_t kAlwaysOn[] = {0x257, 0x118, 0x101, 0x132, 0x3F5, 0x2B6, 0x286, 0x399, 0x3F6, 0x3F8, 0x2E5, 0x266, 0x33A, 0x332, 0x352, 0x392, 0x347, 0x238, 0x3FD, 0x321, 0x2F3, 0x25A, 0x273, 0x472, 0x333, 0x33B, 0x264, 0x185, 0x219, 0x243, 0x318};
typedef struct { uint8_t parked, n; uint16_t ids[8]; } group_t;
static const group_t kSparse[] = {
  {0, 1, {0x3A1}},
  {0, 2, {0x3C2, 0x31A}},
  {0, 3, {0x102, 0x103, 0x3E2}},
  {0, 3, {0x252, 0x2A7, 0x2B4}},
  {0, 1, {0x2B3}},
  {0, 2, {0x25D, 0x212}},
  {1, 1, {0x2E1}},
  {0, 1, {0x129}},
};
static const uint8_t kWheel[][3] = {{1, 0}, {1, 1}, {1, 2}, {1, 3}, {1, 0}, {1, 1}, {1, 2}, {1, 4}, {2, 0, 4}, {1, 1}, {1, 2}, {1, 3}, {1, 0}, {1, 1}, {1, 2}, {2, 5, 6}}; // count, groups
static const uint8_t kChipGroups[] = {2, 5, 6};
static const uint8_t kBtFields[] = {27, 32, 34, 35, 36, 37, 42, 43, 44, 49, 51, 53, 61, 62, 63, 64, 65, 66, 80, 81, 82, 85, 105, 106, 107, 108, 110, 111, 112, 113, 114, 115, 116, 117, 118, 125, 126, 127, 128, 132, 133, 134, 135, 159};
