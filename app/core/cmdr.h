// The Commander Bluetooth session (class Commander in lib/commander.dart),
// single-threaded and clock-injected: every entry point takes now (ms, monotonic)
// and cmdr_tick() runs what is due. The transport (cmdr_io) is blocking: each
// call returns when the GATT operation is done (port/linux/ble_bluez.c later).
//
// Deliberate changes from the phone app (PLAN-merged 5.4):
// - No automatic approval prompt: a refusal always parks; only a tap
//   (cmdr_kick ask=1) starts the one-minute SendUUID approval loop.
// - No S3XY Dash borrow/lend logic (no Dash software runs on this unit), so no
//   ReqGetDashBasicInfo (130): refused by the allow-list.
// - Every request's reply must be the request's type + 1, or the session is
//   torn down (the phone app checks this only for SendUUID and TryAction).
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "cmdr_msg.h"
#include "sec1.h"

// The Commander's GATT service; characteristics are found by their 0x2901 text.
#define CMDR_SERVICE_UUID "5857a678-87c6-11eb-8dcd-0242ac130003"
enum { CMDR_CHR_SESSION, CMDR_CHR_API, CMDR_CHR_PUSH };  // "prov-session", "enhapi", "push-enhapi"
enum { CMDR_HAS_SESSION = 1, CMDR_HAS_API = 2, CMDR_HAS_PUSH = 4 };
enum { CMDR_ADDR_MAX = 32 };

typedef struct {
  void *ctx;
  int (*adapter_on)(void *ctx);  // 1 the controller is powered
  // Scan with CMDR_SERVICE_UUID as the filter for up to 5 s. pin NULL: the
  // strongest advertiser; else only that address (early out when seen). 0 found.
  int (*scan)(void *ctx, const char *pin, char addr[CMDR_ADDR_MAX]);
  // Connect (15 s), large MTU, discover, map characteristics by 0x2901 text.
  // Returns a CMDR_HAS_* mask, <0 failed.
  int (*connect)(void *ctx, const char *addr);
  // One RPC: write (long writes allowed) then read on the same characteristic. 0 ok.
  int (*rpc)(void *ctx, int chr, const uint8_t *w, size_t wn, uint8_t *r, size_t cap, size_t *rn);
  int (*notify)(void *ctx, int on);  // StartNotify on push-enhapi; pushes come back through cmdr_on_push
  void (*disconnect)(void *ctx);     // also cancels a connect in progress
  int (*random)(void *ctx, uint8_t *b, size_t n);  // 0 ok (getrandom on Linux)
  int64_t (*now)(void *ctx);  // the monotonic clock, ms: a dial's retry is timed from its end; NULL: the tick's time
} cmdr_io;

typedef struct {
  const char *id_path, *pin_path;  // the phone id (UUID v4) and the pinned Commander address
  // Sequencing, tuned on one Commander (main.dart defaults): 1 = Bluetooth waits for the
  // WiFi stream however long; else it waits out WiFi's tries and comes back after back_ms.
  int wifi_first;
  int settle_ms;    // after stepping aside with a link up, before the WiFi hello
  int buttons_ms;   // stream live -> Bluetooth back with the buttons (wifi_first: rejoin_ms)
  int rejoin_ms;    // stream live -> the data hold lifts
  int back_ms;      // no stream came -> Bluetooth back anyway (not with wifi_first)
  int lean_for_ms;  // after WiFi went, a fresh session stays lean this long
  void (*on_data)(void *ctx, const uint8_t *data, size_t n);  // each vehicle-data push; NULL when the link drops
  void *data_ctx;
  void (*on_state)(void *ctx);  // the state moved inside a dial, before a blocking call (data_ctx); NULL: none
} cmdr_cfg;
extern const cmdr_cfg cmdr_cfg_default;

typedef enum {
  CS_IDLE,       // not started
  CS_OFF,        // the controller is off: its coming on dials
  CS_PAUSED,     // stepped aside for the WiFi stream, or held while WiFi goes first
  CS_AWAY,       // no Commander advertising (asleep, or the phone app has it): backoff
  CS_RETRYING,   // a dial failed: backoff 2, 5, 10, 20, 30 s
  CS_APPROVING,  // linked, polling SendUUID: double-press the right scroll wheel
  CS_REFUSED,    // not approved: parked until a tap
  CS_SCANNING,   // dialling: looking for the Commander (up to 5 s)
  CS_LINKING,    // dialling: connecting and the handshake (up to 15 s)
  CS_READY,
} cmdr_state;
enum { CMDR_NONE, CMDR_LEAN, CMDR_FULL };  // what a session subscribes to

enum {
  CMDR_OK = 0,
  CMDR_E_IO = -1,         // transport failed
  CMDR_E_STEP = -2,       // reply type != request type + 1: the streams parted
  CMDR_E_DENIED = -3,     // the allow-list refused the message; nothing was written
  CMDR_E_NOT_READY = -4,
  CMDR_E_PARK = -5,       // opens a closure: P only
  CMDR_E_GEAR = -6,       // mirror tilt: never in P, N or D
  CMDR_E_NO_ANSWER = -7,  // the press may have gone through; never resent
  CMDR_E_STALE = -20,     // waited over 2 s behind a redial or an approval step: dropped unsent, tap again
  CMDR_E_STATUS = -8,     // the Commander refused (last_status says why)
};

typedef struct {
  // Read by the UI.
  cmdr_state state;
  int level;         // when ready: CMDR_NONE (buttons), CMDR_LEAN, CMDR_FULL
  const char *why;   // in words, for the icon and the log
  const char *push_info;
  char car_link[96]; // ReqGetTeslaAPIStatus in words
  uint64_t last_status, sub_status;
  int has_dest, has_miles, has_minutes, has_arrive, has_recirc;
  char dest[128];
  float miles, minutes, arrive_pct;
  uint64_t recirc;
  unsigned push_count;
  // Internal.
  cmdr_cfg cfg;
  cmdr_io io;
  sec1_t sec;
  int linked, has, secure, push_on;
  int on, paused, redial, asked, asks, parked, attempt;
  int wifi_live, wifi_trying, data_hold;
  int64_t wifi_at, subscribed_at;
  int64_t retry_at, approve_at, approve_by, refit_at, back_at, buttons_at, data_at;  // -1 = not set
  char addr[CMDR_ADDR_MAX], id[40];
} cmdr_t;

void cmdr_init(cmdr_t *c, const cmdr_cfg *cfg, const cmdr_io *io);
void cmdr_connect(cmdr_t *c, int64_t now);           // start, and keep reconnecting
void cmdr_tick(cmdr_t *c, int64_t now);              // run what is due (call every 50-250 ms)
void cmdr_kick(cmdr_t *c, int ask, int64_t now);     // try now; ask = the owner's tap (unparks, may approve)
void cmdr_forget(cmdr_t *c, int64_t now);            // unpin: the next dial takes the strongest
void cmdr_adapter(cmdr_t *c, int on, int64_t now);   // the controller went on or off
void cmdr_link_lost(cmdr_t *c, int64_t now);         // the port saw the link drop
void cmdr_on_push(cmdr_t *c, const uint8_t *d, size_t n);
int64_t cmdr_until(const cmdr_t *c);                 // what the icon counts down to, or -1

// WiFi side (main.dart _linksChanged/_stepAside/_btWaitsFor). Call cmdr_wifi
// whenever the WiFi link's state is known (each tick is fine).
void cmdr_wifi(cmdr_t *c, int live, int trying, int64_t now);
// Before each WiFi stream starts: returns how long to wait before the hello.
int cmdr_step_aside(cmdr_t *c, int64_t now);
int cmdr_pause(cmdr_t *c, int64_t now);              // 1 if a link was up or on its way
void cmdr_resume(cmdr_t *c, int64_t now);
void cmdr_data_hold(cmdr_t *c, int hold, int64_t now);

// One press, once. parked: the car is known to be in P now; gear 'P','R','N','D' or 0.
int cmdr_press_guard(cmdr_action a, int parked, char gear);
int cmdr_press(cmdr_t *c, cmdr_action a, int parked, char gear, int64_t now);

// The one function that writes to the Commander (rule 6). prov-session takes
// only SessionCmd0/Cmd1 with device type 1, and only before the session is
// confirmed; enhapi only cmdr_req_ok() requests, encrypted, after it; the reply
// is decrypted in place and must be type + 1. Nothing else is ever written.
int cmdr_send(cmdr_t *c, int chr, const uint8_t *m, size_t n, uint8_t *r, size_t cap, size_t *rn);
