// Commander EnhAPI messages, as lib/commander.dart: request builders, the
// allow-list, reply status, push parsing, RespGetApiStatus in words.
#pragma once
#include <stddef.h>
#include <stdint.h>

// S3XY functions (S3xyAction): the action's number and the state TryAction sends.
typedef enum {
  ACT_FRUNK, ACT_TRUNK, ACT_LIGHT, ACT_MIRROR_TILT, ACT_RECIRC_ON, ACT_RECIRC_OFF, ACT_RECIRC_TOGGLE,
  ACT_DOOR_FL, ACT_DOOR_FR, ACT_DOOR_RL, ACT_DOOR_RR, ACT_UNLOCK, ACT_LOCK, ACT_CHARGE_PORT,
  ACT_HONK, ACT_FLASH, ACT_CLIMATE,
  ACT_MASSAGE_L_SOFT, ACT_MASSAGE_L_PULSE, ACT_MASSAGE_L_HARD, ACT_MASSAGE_R_SOFT, ACT_MASSAGE_R_PULSE, ACT_MASSAGE_R_HARD,
  ACT_COUNT
} cmdr_action;
typedef struct { uint8_t id, state; const char *label; } cmdr_action_def;
extern const cmdr_action_def cmdr_actions[ACT_COUNT];
// Every S3XY function, the car panel's (lib/car_actions.dart, generated into build/car_actions_gen.h): each option a
// press of its own, numbered after ACT_COUNT (ACT_COUNT + its index in cmdr_car_opts); an action its options in a row.
typedef struct { const char *group, *name; uint16_t first; uint8_t n, confirm; } cmdr_car_act;
extern const int cmdr_car_opt_count, cmdr_car_act_count;
extern const cmdr_action_def cmdr_car_opts[];
extern const char *const cmdr_car_words[]; // an option's own word ("Soft"), NULL for an action without a choice
extern const cmdr_car_act cmdr_car_acts[];
const cmdr_action_def *cmdr_action_at(int a); // an ACT_* or a car panel press; NULL for any other number
int cmdr_action_opens(cmdr_action a); // frunk, trunk, doors: P only

enum { CMDR_MSG_MAX = 512 };

// The only EnhAPI request types ever sent: 32 SendUUID, 28 TryAction,
// 124 Subscribe, 82 ReqGetTeslaAPIStatus, 48 ReqDashTriggerAllDataPush.
// Returns the payload field that type must carry, 0 = refused.
uint32_t cmdr_payload_field(uint64_t type);
// A whole request passes: field 1 = an allowed type, plus exactly its one
// payload field, nothing else. Sets *type.
int cmdr_req_ok(const uint8_t *m, size_t n, uint64_t *type);

// Builders return the length, 0 on error. out holds CMDR_MSG_MAX.
size_t cmdr_send_uuid(uint8_t *out, const char *id);
size_t cmdr_try_action(uint8_t *out, cmdr_action a);
size_t cmdr_subscribe(uint8_t *out, const uint8_t *fields, size_t n);
size_t cmdr_api_status_req(uint8_t *out);   // 08 52 ea05 00
size_t cmdr_trigger_push_req(uint8_t *out); // 08 30 da03 00

// msgType and status (field 1 of the first length-delimited field, 0 if
// absent) of a reply. 0 ok, -1 malformed.
int cmdr_reply_status(const uint8_t *r, size_t n, uint64_t *type, uint64_t *status);
extern const char *const cmdr_statuses[9]; // 'ok' .. 'not supported'

// The vehicle-data fields a session subscribes to: what WiFi lacks, or all.
extern const uint8_t cmdr_lean_fields[7];
// The last CMDR_NEWER of all, the last CMDR_LEAN_NEWER of the lean ones (the seats' massage, the tyres, the cabin) need
// recent Commander firmware: dropped again if a subscription is refused.
enum { CMDR_NEWER = 7, CMDR_LEAN_NEWER = 2 };
extern const uint8_t cmdr_all_fields[57];

// A push (notification on push-enhapi, in the clear).
typedef struct {
  int kind; // 16 vehicle data, 8 device connection, 0 other or unreadable
  // kind 16: the PushVehicleDataHolder body, for the CAN-side decoder.
  const uint8_t *data;
  size_t data_len;
  int has_dest, has_miles, has_minutes, has_arrive, has_recirc, has_massage_l, has_massage_r;
  char dest[128];   // "" = no destination
  float miles, minutes, arrive_pct;
  uint64_t recirc, massage_l, massage_r;  // 101; 130/131: the seats' massage (low 7 bits the mode as pressed)
  // kind 8: device type (64 the Dash, 1 a phone) and connected (1), gone (0), no change (-1).
  uint64_t dev_type;
  int dev_on;
} cmdr_push;
int cmdr_push_parse(const uint8_t *d, size_t n, cmdr_push *p);

// TeslaAPI.RespGetApiStatus body in words (apiWords).
void cmdr_api_words(const uint8_t *b, size_t n, char *out, size_t cap);
