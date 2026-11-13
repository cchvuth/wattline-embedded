// Pairing a computer for developer SSH, without touching the card: the settings' "Pair a computer" tap opens a
// one-time window (5 min, one key, 5 wrong codes close it) with a tiny HTTP/1.0 server on <wlan0 address>:8022.
// GET /pair?code=NNNNNN: a page to paste a public key (a phone works); POST /pair?code=NNNNNN: the key as the body
// (raw, as curl --data-binary sends it, or the page's key=...). An accepted key is appended to the authorized keys
// and the SSH enabler is run. The window exists only while the pairing view is open: nothing listens otherwise.
// Hookup (the loop's owner, app/dash.c): the poll set gains   {.fd = pair_fd(), .events = POLLIN}
//                                          each loop turn:      pair_handle(now);
#pragma once
#include <stdbool.h>
#include <stdint.h>

enum { PAIR_PORT = 8022, PAIR_WINDOW_MS = 5 * 60 * 1000, PAIR_TRIES = 5, PAIR_KEY_MAX = 2048 };

typedef struct {
  bool open;                 // the window is up (the server listens)
  char url[96];              // http://<ip>:8022/pair?code=<code>
  char ip[48], code[8];
  int secs_left;
  char result[112];          // "Key added: <comment>", "Wrong code", "Expired", ...; "" none yet
} pair_status_t;

// Paths, overridable for tests: the authorized keys (its folder made 0700, the file 0600) and the enabler run once a
// key is added (the image's: makes the host key if missing, starts Dropbear).
extern const char *pair_keys_path, *pair_enable_cmd;

int pair_open(const char *iface, int64_t now);   // 0: listening; -1: no IPv4 address on iface, or no socket (result says)
void pair_close(const char *why);                // why: the result line ("Cancelled"); the server goes away
int pair_fd(void);                               // POLLIN: the listening socket or the client being served; -1 closed
void pair_handle(int64_t now);                   // serve what is ready, close on expiry; call every loop turn
const pair_status_t *pair_status(int64_t now);

// The key check alone: 1 if [line] is one OpenSSH public key we accept (ssh-ed25519, ecdsa-sha2-nistp256, ssh-rsa of
// 3072 bits or more; no options; under PAIR_KEY_MAX). Fills the type, the comment (printable, cut) and the SHA256
// fingerprint as ssh-keygen -l prints it.
int pair_key_ok(const char *line, char *type, int type_n, char *comment, int comment_n, char *fp, int fp_n);
