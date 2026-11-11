// See pair.h. One client at a time, never blocking the loop: pair_handle accepts, reads what has come, answers once
// the request is whole, and closes. The key body never reaches the log: only its type, comment and fingerprint.
#define _GNU_SOURCE
#include "pair.h"
#include "app.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

const char *pair_keys_path = "/data/ssh/authorized_keys", *pair_enable_cmd = "/usr/sbin/dash-ssh-enable";

enum { REQ_MAX = 4096 + PAIR_KEY_MAX * 3, CLIENT_MS = 5000 };   // a form's key arrives %-encoded: room for 3x

static pair_status_t st;
static int lfd = -1, cfd = -1, tries;
static int64_t until, client_at;
static char req[REQ_MAX + 1];
static size_t req_n;
static pid_t enabler;
extern char **environ;

// ---- SHA-256 (FIPS 180-4), for the fingerprint ----
static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
    0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
    0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
#define ROR(x, n) ((x) >> (n) | (x) << (32 - (n)))
static void sha_block(uint32_t h[8], const uint8_t *p) {
  uint32_t w[64], a[8];
  for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | p[4 * i + 1] << 16 | p[4 * i + 2] << 8 | p[4 * i + 3];
  for (int i = 16; i < 64; i++)
    w[i] = w[i - 16] + (ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ w[i - 15] >> 3) + w[i - 7] +
           (ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ w[i - 2] >> 10);
  memcpy(a, h, sizeof a);
  for (int i = 0; i < 64; i++) {
    uint32_t t1 = a[7] + (ROR(a[4], 6) ^ ROR(a[4], 11) ^ ROR(a[4], 25)) + ((a[4] & a[5]) ^ (~a[4] & a[6])) + K[i] + w[i];
    uint32_t t2 = (ROR(a[0], 2) ^ ROR(a[0], 13) ^ ROR(a[0], 22)) + ((a[0] & a[1]) ^ (a[0] & a[2]) ^ (a[1] & a[2]));
    memmove(a + 1, a, 7 * sizeof a[0]);
    a[4] += t1, a[0] = t1 + t2;
  }
  for (int i = 0; i < 8; i++) h[i] += a[i];
}
static void sha256(const uint8_t *d, size_t n, uint8_t out[32]) {
  uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  uint8_t b[64];
  size_t i = 0;
  for (; n - i >= 64; i += 64) sha_block(h, d + i);
  size_t r = n - i;
  memset(b, 0, 64);
  memcpy(b, d + i, r);
  b[r] = 0x80;
  if (r >= 56) sha_block(h, b), memset(b, 0, 64);
  for (int j = 0; j < 8; j++) b[63 - j] = (uint8_t)((uint64_t)n * 8 >> (8 * j));
  sha_block(h, b);
  for (int j = 0; j < 32; j++) out[j] = (uint8_t)(h[j / 4] >> (24 - 8 * (j % 4)));
}

// ---- base64 ----
static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static int b64_decode(const char *s, size_t n, uint8_t *out, size_t cap) {   // strict; -1 bad
  if (n % 4) return -1;
  size_t o = 0;
  for (size_t i = 0; i < n; i += 4) {
    int v[4];
    for (int j = 0; j < 4; j++) {
      const char *p = s[i + j] ? strchr(B64, s[i + j]) : NULL;
      v[j] = s[i + j] == '=' && i + 4 == n && j >= 2 ? 0 : p ? (int)(p - B64) : -1;
      if (v[j] < 0) return -1;
    }
    if (s[i + 2] == '=' && s[i + 3] != '=') return -1;
    uint32_t x = (uint32_t)v[0] << 18 | v[1] << 12 | v[2] << 6 | v[3];
    int k = s[i + 2] == '=' ? 1 : s[i + 3] == '=' ? 2 : 3;
    if (o + k > cap) return -1;
    for (int j = 0; j < k; j++) out[o++] = (uint8_t)(x >> (16 - 8 * j));
  }
  return (int)o;
}
static void b64_encode_nopad(const uint8_t *d, size_t n, char *out) {
  size_t o = 0;
  for (size_t i = 0; i < n; i += 3) {
    uint32_t x = (uint32_t)d[i] << 16 | (i + 1 < n ? d[i + 1] << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
    out[o++] = B64[x >> 18 & 63], out[o++] = B64[x >> 12 & 63];
    if (i + 1 < n) out[o++] = B64[x >> 6 & 63];
    if (i + 2 < n) out[o++] = B64[x & 63];
  }
  out[o] = 0;
}

// ---- the key ----
static int ssh_string(const uint8_t *b, size_t n, size_t *at, const uint8_t **s, uint32_t *len) {
  if (n - *at < 4) return 0;
  uint32_t l = (uint32_t)b[*at] << 24 | b[*at + 1] << 16 | b[*at + 2] << 8 | b[*at + 3];
  if (l > n - *at - 4) return 0;
  *s = b + *at + 4, *len = l, *at += 4 + l;
  return 1;
}

int pair_key_ok(const char *line, char *type, int type_n, char *comment, int comment_n, char *fp, int fp_n) {
  size_t n = strlen(line);
  while (n && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' ')) n--;
  if (!n || n >= PAIR_KEY_MAX || memchr(line, '\n', n) || memchr(line, '\r', n)) return 0;
  const char *sp = memchr(line, ' ', n);   // the first word is the type: an options prefix fails here
  if (!sp) return 0;
  size_t tl = (size_t)(sp - line);
  static const char *const types[] = {"ssh-ed25519", "ecdsa-sha2-nistp256", "ssh-rsa"};
  int t = -1;
  for (int i = 0; i < 3; i++)
    if (tl == strlen(types[i]) && !memcmp(line, types[i], tl)) t = i;
  if (t < 0) return 0;
  const char *b = sp + 1, *e = memchr(b, ' ', n - (size_t)(b - line));
  size_t bl = e ? (size_t)(e - b) : n - (size_t)(b - line);
  uint8_t blob[PAIR_KEY_MAX];
  int bn = b64_decode(b, bl, blob, sizeof blob);
  if (bn <= 0) return 0;
  size_t at = 0;
  const uint8_t *s;
  uint32_t l;
  if (!ssh_string(blob, bn, &at, &s, &l) || l != tl || memcmp(s, line, tl)) return 0;   // the blob names its type
  if (t == 0) {
    if (!ssh_string(blob, bn, &at, &s, &l) || l != 32) return 0;
  } else if (t == 1) {
    if (!ssh_string(blob, bn, &at, &s, &l) || l != 8 || memcmp(s, "nistp256", 8)) return 0;
    if (!ssh_string(blob, bn, &at, &s, &l) || l != 65 || s[0] != 4) return 0;
  } else {
    if (!ssh_string(blob, bn, &at, &s, &l) || !l) return 0;   // e
    if (!ssh_string(blob, bn, &at, &s, &l) || !l) return 0;   // n: its bits
    while (l && !*s) s++, l--;
    int bits = l ? (int)l * 8 : 0;
    for (uint8_t top = l ? s[0] : 0; bits && !(top & 0x80); top <<= 1) bits--;
    if (bits < 3072) return 0;
  }
  if (at != (size_t)bn) return 0;
  snprintf(type, type_n, "%s", types[t]);
  int c = 0;   // the comment, printable only
  if (e)
    for (const char *p = e + 1; p < line + n && c < comment_n - 1; p++) comment[c++] = *p >= 0x20 && *p < 0x7f ? *p : '?';
  comment[c] = 0;
  uint8_t h[32];
  char enc[48];
  sha256(blob, bn, h);
  b64_encode_nopad(h, 32, enc);
  snprintf(fp, fp_n, "SHA256:%s", enc);
  return 1;
}

// Appends [line] (type and key, the comment kept) unless that key is there already. 0 added, 1 had it, -1 failed.
static int add_key(const char *line) {
  char dir[256];
  snprintf(dir, sizeof dir, "%s", pair_keys_path);
  char *slash = strrchr(dir, '/');
  if (slash && slash != dir) {
    *slash = 0;
    for (char *p = dir + 1; *p; p++)   // mkdir -p, each 0700
      if (*p == '/') *p = 0, mkdir(dir, 0700), *p = '/';
    if (mkdir(dir, 0700) && errno != EEXIST) return -1;
    chmod(dir, 0700);
  }
  const char *k = strchr(line, ' ') + 1, *ke = strchr(k, ' ');
  size_t kl = ke ? (size_t)(ke - k) : strcspn(k, "\r\n ");
  FILE *f = fopen(pair_keys_path, "r");
  char have[PAIR_KEY_MAX + 64];
  int dup = 0;
  while (f && fgets(have, sizeof have, f)) {   // the same key body under any comment counts
    char *hk = strchr(have, ' ');
    if (hk && !strncmp(hk + 1, k, kl) && (hk[1 + kl] == ' ' || hk[1 + kl] == '\n' || !hk[1 + kl])) dup = 1;
  }
  if (f) fclose(f);
  if (dup) return 1;
  int fd = open(pair_keys_path, O_WRONLY | O_CREAT | O_APPEND, 0600);
  if (fd < 0) return -1;
  fchmod(fd, 0600);
  size_t n = strcspn(line, "\r\n");
  int ok = write(fd, line, n) == (ssize_t)n && write(fd, "\n", 1) == 1;
  close(fd);
  return ok ? 0 : -1;
}

// ---- the window ----
static void set_result(const char *r) { snprintf(st.result, sizeof st.result, "%s", r); }
static void drop_client(void) {
  if (cfd >= 0) close(cfd);
  cfd = -1, req_n = 0;
}

void pair_close(const char *why) {
  drop_client();
  if (lfd >= 0) {
    close(lfd);
    lfd = -1;
    log_msg("pair: window closed%s%s", why ? ": " : "", why ? why : "");
  }
  st.open = false;
  if (why) set_result(why);
}

int pair_open(const char *iface, int64_t now) {
  pair_close(NULL);
  memset(&st, 0, sizeof st);
  tries = 0;
  struct ifaddrs *ifs, *i;
  struct sockaddr_in addr = {0};
  if (getifaddrs(&ifs) == 0) {
    for (i = ifs; i; i = i->ifa_next)
      if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET && !strcmp(i->ifa_name, iface)) {
        addr = *(struct sockaddr_in *)i->ifa_addr;
        break;
      }
    freeifaddrs(ifs);
  }
  if (!addr.sin_addr.s_addr) {
    set_result("No WiFi address: join a network first");
    return -1;
  }
  inet_ntop(AF_INET, &addr.sin_addr, st.ip, sizeof st.ip);
  addr.sin_port = htons(PAIR_PORT);
  lfd = socket(AF_INET, SOCK_STREAM, 0);
  int one = 1;
  if (lfd >= 0) setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  if (lfd < 0 || bind(lfd, (struct sockaddr *)&addr, sizeof addr) || listen(lfd, 4)) {   // that address only
    if (lfd >= 0) close(lfd);
    lfd = -1;
    set_result("Could not listen on port 8022");
    log_msg("pair: listen %s:%d: %s", st.ip, PAIR_PORT, strerror(errno));
    return -1;
  }
  fcntl(lfd, F_SETFL, fcntl(lfd, F_GETFL) | O_NONBLOCK);
  fcntl(lfd, F_SETFD, FD_CLOEXEC);
  uint8_t r[4];
  if (getentropy(r, sizeof r)) {
    pair_close("No randomness");
    return -1;
  }
  snprintf(st.code, sizeof st.code, "%06u", (unsigned)((uint32_t)r[0] << 24 | r[1] << 16 | r[2] << 8 | r[3]) % 1000000u);
  snprintf(st.url, sizeof st.url, "http://%s:%d/pair?code=%s", st.ip, PAIR_PORT, st.code);
  st.open = true;
  until = now + PAIR_WINDOW_MS;
  log_msg("pair: window open on %s:%d for 5 min", st.ip, PAIR_PORT);   // the code stays on the screen only
  return 0;
}

int pair_fd(void) { return cfd >= 0 ? cfd : lfd; }

const pair_status_t *pair_status(int64_t now) {
  st.secs_left = st.open ? (int)((until - now + 999) / 1000) : 0;
  if (st.secs_left < 0) st.secs_left = 0;
  return &st;
}

static void reply(int code, const char *ctype, const char *body) {
  char h[256];
  const char *word = code == 200 ? "OK" : code == 400 ? "Bad Request" : code == 403 ? "Forbidden" : code == 404 ? "Not Found"
                   : code == 410 ? "Gone" : "Payload Too Large";
  int n = snprintf(h, sizeof h, "HTTP/1.0 %d %s\r\nContent-Type: %s; charset=utf-8\r\nContent-Length: %zu\r\nConnection: close\r\n"
                   "Cache-Control: no-store\r\n\r\n", code, word, ctype, strlen(body));
  if (write(cfd, h, n) < 0 || write(cfd, body, strlen(body)) < 0) log_msg("pair: reply: %s", strerror(errno));
}

static int same_code(const char *a, const char *b) {   // constant time over the 6 digits
  unsigned d = 0;
  for (int i = 0; i < 6; i++) d |= (unsigned char)a[i] ^ (unsigned char)b[i];
  return d == 0;
}

static void url_decode(char *s) {   // the form's key=...: %XX and + in place
  char *o = s;
  for (; *s; s++) {
    if (*s == '+') *o++ = ' ';
    else if (*s == '%' && s[1] && s[2]) {
      char hx[3] = {s[1], s[2], 0};
      *o++ = (char)strtol(hx, NULL, 16), s += 2;
    } else *o++ = *s;
  }
  *o = 0;
}

static const char PAGE[] =
    "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'><title>Pair with the Dash</title>"
    "<body style='font:16px sans-serif;background:#141218;color:#e6e0e9;max-width:40em;margin:2em auto;padding:0 1em'>"
    "<h2>Pair with the Dash</h2><p>Paste one OpenSSH public key (the <code>.pub</code> file: ssh-ed25519, "
    "ecdsa-sha2-nistp256 or ssh-rsa of 3072 bits or more).</p><form method=post>"
    "<textarea name=key rows=6 style='width:100%;font:14px monospace' required></textarea><p><button>Add key</button>"
    "</form>";

static void serve(int64_t now) {
  req[req_n] = 0;
  char *eoh = strstr(req, "\r\n\r\n");
  if (!eoh) return;   // not whole yet
  char *body = eoh + 4;
  size_t clen = 0;
  for (char *h = strstr(req, "\r\n"); h && h < eoh; h = strstr(h + 2, "\r\n"))
    if (!strncasecmp(h + 2, "Content-Length:", 15)) clen = strtoul(h + 17, NULL, 10);
  if (clen > REQ_MAX || (size_t)(req + req_n - body) < clen) {
    if (clen > REQ_MAX) reply(413, "text/plain", "Too long\n"), drop_client();
    return;   // the body is still coming
  }
  body[clen] = 0;
  int post = !strncmp(req, "POST ", 5);
  if (!post && strncmp(req, "GET ", 4)) return reply(400, "text/plain", "GET or POST\n"), drop_client();
  char *path = req + (post ? 5 : 4), *q = strchr(path, '?'), *end = strchr(path, ' ');
  if (strncmp(path, "/pair", 5) || (path[5] != '?' && path[5] != ' '))
    return reply(404, "text/plain", "Not found\n"), drop_client();
  char *code = q && q < end ? strstr(q, "code=") : NULL;
  int good = code && code < end && (code + 11 == end || code[11] == '&') && same_code(code + 5, st.code);
  if (!good) {
    reply(403, "text/plain", "Wrong code\n");
    drop_client();
    log_msg("pair: wrong code (%d of %d)", tries + 1, PAIR_TRIES);
    set_result("Wrong code");
    if (++tries >= PAIR_TRIES) pair_close("Too many wrong codes: closed");
    return;
  }
  if (!post) return reply(200, "text/html", PAGE), drop_client();
  if (!strncmp(body, "key=", 4)) url_decode(body += 4);
  char type[32], comment[64], fp[64], msg[160];
  if (!pair_key_ok(body, type, sizeof type, comment, sizeof comment, fp, sizeof fp)) {
    reply(400, "text/plain", "Not an accepted OpenSSH public key: one line, ssh-ed25519, ecdsa-sha2-nistp256 or ssh-rsa "
                             "of 3072 bits or more, no options\n");
    drop_client();
    set_result("Not an accepted public key");
    log_msg("pair: refused a key that is not an accepted OpenSSH public key");
    return;
  }
  int r = add_key(body);
  log_msg("pair: %s %s %s: %s", type, comment, fp, r == 0 ? "added" : r == 1 ? "already there" : "could not write");
  if (r < 0) {
    reply(400, "text/plain", "The Dash could not store the key\n");
    drop_client();
    set_result("Could not store the key");
    return;
  }
  snprintf(msg, sizeof msg, "Key added: %s\n", comment[0] ? comment : type);
  reply(200, "text/plain", msg);
  drop_client();
  char *argv[] = {(char *)pair_enable_cmd, NULL};
  if (posix_spawn(&enabler, pair_enable_cmd, NULL, NULL, argv, environ)) {
    enabler = 0;
    log_msg("pair: %s did not start", pair_enable_cmd);
  }
  msg[strcspn(msg, "\n")] = 0;
  pair_close(msg);   // one key per window
  (void)now;
}

void pair_handle(int64_t now) {
  if (enabler > 0 && waitpid(enabler, NULL, WNOHANG) == enabler) enabler = 0;   // reaped, never waited on
  if (lfd < 0) return;
  if (now >= until) return pair_close("Expired");
  if (cfd >= 0 && now - client_at > CLIENT_MS) drop_client();   // a slow client gives way
  if (cfd < 0) {
    cfd = accept(lfd, NULL, NULL);
    if (cfd < 0) return;
    fcntl(cfd, F_SETFL, fcntl(cfd, F_GETFL) | O_NONBLOCK);
    fcntl(cfd, F_SETFD, FD_CLOEXEC);
    client_at = now, req_n = 0;
  }
  for (;;) {
    ssize_t n = read(cfd, req + req_n, REQ_MAX - req_n);
    if (n > 0) req_n += (size_t)n;
    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) return drop_client();
    if (n < 0) break;
    if (req_n >= REQ_MAX) return reply(413, "text/plain", "Too long\n"), drop_client();
  }
  serve(now);
}

#ifdef PAIR_SELFTEST
// The one check, on loopback with a scratch key file: cc -std=gnu11 -DPAIR_SELFTEST -o build/pair_check app/pair.c
//   && build/pair_check     (keys: throwaway public halves; their private halves were deleted)
#include <assert.h>
#include <stdarg.h>
#include <sys/time.h>
static int64_t T;
int64_t mono_ms(void) { return T; }
void log_msg(const char *f, ...) {
  va_list a;
  va_start(a, f);
  vprintf(f, a), puts("");
  va_end(a);
}
static const char ED[] = "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIL2VJrmcv852c7reeZpaC7+IKc64o+TYllAdTievesES test@laptop";
static const char EC[] = "ecdsa-sha2-nistp256 AAAAE2VjZHNhLXNoYTItbmlzdHAyNTYAAAAIbmlzdHAyNTYAAABBBE625ieQDEN2j0EUEfUaUQGgzqSmRKCwayGpriR7Dfy1ri3YJ5m3O3/RXO9iZCA+H51qNy14n568I3Z9UrtJEvI= ec@x";
static const char RSA2048[] = "ssh-rsa AAAAB3NzaC1yc2EAAAADAQABAAABAQC2bt5SiF74PEEWq0AOhdsPssJhmvs+cFKGdgijTdQBRtjvcM5E7SjsU/NfIuqB5DYo9dhCgnM9ZAr3kosJDJc3tDpO8SspbHy9vB2Qzo+cJJ1kSuDFMCaqbNbCX4wTkR02N2H3zSwIDjqIpzdh+vj17QqH6HOy2iW3LBmXsQA9kFZ5WANdSXkCTQE3JX2bb5LN/lH371t/0qCEOehxAhfgaCjEatzrbnqor/zr8yDm3X+R9p2Vpwc+ITpJRTXmgpB0ebFLnDW6xMo/HTdjvA105EHYRd9y2dxeoG2JYQlSFIvhCe5iwEBwJTDUncux2pDy1XJJXID6aDdU1c6Zb4S7 short@rsa";
static char rq[8192], rs[4096];
static int ask(const char *method, const char *code, const char *body) {   // one request; the status code
  int s = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(PAIR_PORT)};
  inet_pton(AF_INET, st.ip, &a.sin_addr);
  assert(!connect(s, (struct sockaddr *)&a, sizeof a));
  int n = snprintf(rq, sizeof rq, "%s /pair?code=%s HTTP/1.0\r\nContent-Length: %zu\r\n\r\n%s", method, code, strlen(body), body);
  assert(write(s, rq, n) == n);
  for (int i = 0; i < 50 && cfd < 0; i++) pair_handle(T), usleep(1000);
  for (int i = 0; i < 50 && cfd >= 0; i++) pair_handle(T), usleep(1000);
  n = (int)read(s, rs, sizeof rs - 1);
  close(s);
  rs[n > 0 ? n : 0] = 0;
  return atoi(rs + 9);
}
int main(void) {
  char t[32], c[64], fp[64];
  assert(pair_key_ok(ED, t, sizeof t, c, sizeof c, fp, sizeof fp) && !strcmp(c, "test@laptop"));
  assert(!strcmp(fp, "SHA256:XgV+TJ4OmK70RVFkmdGKnOde9VZm5UJc8X9P9Ml5c2w"));   // as ssh-keygen -l
  assert(pair_key_ok(EC, t, sizeof t, c, sizeof c, fp, sizeof fp));
  assert(!pair_key_ok("command=\"x\" ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIL2VJrmcv852c7reeZpaC7+IKc64o+TYllAdTievesES", t, 32, c, 64, fp, 64));   // an options prefix
  assert(!pair_key_ok(RSA2048, t, 32, c, 64, fp, 64));   // RSA under 3072 bits
  assert(!pair_key_ok("ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIL2VJrmcv852c7reeZpaC7+IKc64o+TYllAdTiev", t, 32, c, 64, fp, 64));   // a cut key
  char path[] = "/tmp/pair_check_XXXXXX";
  assert(mkdtemp(path));
  static char keys[64];
  snprintf(keys, sizeof keys, "%s/ssh/authorized_keys", path);
  pair_keys_path = keys, pair_enable_cmd = "/usr/bin/true";
  const char *lo = "lo0";
  struct ifaddrs *ifs, *i;
  getifaddrs(&ifs);
  for (i = ifs; i; i = i->ifa_next) if (!strcmp(i->ifa_name, "lo")) lo = "lo";
  freeifaddrs(ifs);
  assert(pair_open(lo, T) == 0 && st.open && strlen(st.code) == 6);
  assert(ask("GET", "000000", "") == 403 || !strcmp(st.code, "000000"));
  assert(ask("GET", st.code, "") == 200 && strstr(rs, "<textarea"));
  assert(ask("POST", st.code, "ssh-dss AAAA x") == 400 && st.open);
  assert(ask("POST", st.code, ED) == 200 && strstr(rs, "Key added: test@laptop") && !st.open);   // one key, then closed
  struct stat sb;
  assert(!stat(keys, &sb) && (sb.st_mode & 0777) == 0600);
  assert(pair_open(lo, T) == 0 && ask("POST", st.code, "key=ssh-ed25519+AAAAC3NzaC1lZDI1NTE5AAAAIL2VJrmcv852c7reeZpaC7%2BIKc64o%2BTYllAdTievesES+again") == 200);   // the form; a duplicate: not added twice
  FILE *f = fopen(keys, "r");
  int lines = 0;
  for (char l[512]; fgets(l, sizeof l, f);) lines++;
  fclose(f);
  assert(lines == 1);
  assert(pair_open(lo, T) == 0);
  for (int k = 0; k < PAIR_TRIES; k++) ask("GET", "99999x", "");
  assert(!st.open && strstr(st.result, "Too many"));   // five wrong codes close it
  assert(pair_open(lo, T) == 0);
  T += PAIR_WINDOW_MS, pair_handle(T);
  assert(!st.open && !strcmp(st.result, "Expired"));
  puts("pair ok");
  return 0;
}
#endif
