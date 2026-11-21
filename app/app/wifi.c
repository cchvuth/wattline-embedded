// The WiFi join, which wpa_supplicant does outside the app (S45wifi-auto, wifi-connect): what the WiFi button says
// until wlan0 has an address and the link's own states take over (widgets.dart _Controls: OFF, SCAN, JOIN).
// Looked at every 1.5 s: /sys/class/net/wlan0, getifaddrs, and wpa_supplicant's STATUS on its control socket (asked
// without waiting: each look reads the reply to the one before). Off Linux: nothing known, the link says it all.
#include "app.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool is(const char *a, const char *b) { return a && !strcmp(a, b); }

int wifi_join_of(wifi_track_t *t, bool exists, const char *oper, bool ipv4, const char *wpa, const char **why) {
  if (!exists) return *why = "WiFi is off (no wlan0)", WIFI_OFF;
  if (wpa) {   // the 4-way handshake falling back, again and again: the network refuses the passphrase
    if (is(wpa, "COMPLETED")) t->fails = 0;
    else if (is(t->last, "4WAY_HANDSHAKE") && (is(wpa, "DISCONNECTED") || is(wpa, "SCANNING"))) t->fails++;
    snprintf(t->last, sizeof t->last, "%s", wpa);
  }
  bool up = is(oper, "up");
  if (up && ipv4) return -1;
  const char *wrong = t->fails >= 2 ? "wrong password? the network keeps refusing the passphrase" : NULL;
  if (is(wpa, "SCANNING") || is(wpa, "DISCONNECTED") || is(wpa, "INACTIVE"))
    return *why = wrong ? wrong : "searching for a network", WIFI_SCANNING;
  *why = wrong ? wrong : up ? "joined, waiting for an address" : "joining a network";
  return WIFI_JOINING;
}

// One SCAN_RESULTS line's SSID, as wpa_supplicant prints it (printf_encode: \\ \" \e \n \r \t \xNN), decoded.
static int unescape(const char *s, size_t n, char *out, size_t cap) {
  size_t o = 0;
  for (size_t i = 0; i < n && o + 1 < cap; i++) {
    char c = s[i];
    if (c == '\\' && i + 1 < n) {
      char e = s[++i];
      unsigned v;
      if (e == 'x' && i + 2 < n + 1 && sscanf(s + i + 1, "%2x", &v) == 1) c = (char)v, i += 2;
      else c = e == 'n' ? '\n' : e == 'r' ? '\r' : e == 't' ? '\t' : e == 'e' ? 27 : e;
    }
    out[o++] = c;
  }
  out[o] = 0;
  return (int)o;
}

int wifi_scan_parse(const char *reply, char (*out)[33], int max) {
  enum { ALL = 64 };   // every distinct name first, then the strongest [max] of them
  static char all[ALL][33];
  int sig[ALL], n = 0;
  if (!reply || strncmp(reply, "bssid /", 7)) return 0;
  for (const char *l = strchr(reply, '\n'); l && *++l; l = strchr(l, '\n')) {
    const char *e = strchr(l, '\n'), *f = l;
    size_t len = e ? (size_t)(e - l) : strlen(l);
    int tab = 0, level = 0;
    for (size_t i = 0; i < len && tab < 4; i++)   // bssid, frequency, signal level, flags, ssid
      if (l[i] == '\t') { if (++tab == 2) level = atoi(l + i + 1); f = l + i + 1; }
    if (tab < 4) { if (!e) break; continue; }
    char name[33];
    if (unescape(f, len - (size_t)(f - l), name, sizeof name) == 0 || !name[0]) { if (!e) break; continue; }   // hidden
    int k = 0;
    while (k < n && strcmp(all[k], name)) k++;
    if (k < n) { if (level > sig[k]) sig[k] = level; }
    else if (n < ALL) snprintf(all[n], 33, "%s", name), sig[n++] = level;
    if (!e) break;
  }
  for (int i = 1; i < n; i++)   // strongest first (insertion sort: a handful of networks)
    for (int j = i; j > 0 && sig[j] > sig[j - 1]; j--) {
      int t = sig[j]; sig[j] = sig[j - 1], sig[j - 1] = t;
      char tmp[33]; memcpy(tmp, all[j], 33), memcpy(all[j], all[j - 1], 33), memcpy(all[j - 1], tmp, 33);
    }
  if (n > max) n = max;
  memcpy(out, all, sizeof all[0] * (size_t)(n > 0 ? n : 0));
  return n;
}

#ifdef __linux__
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int wpa = -1;
static bool asked;
static char wpa_state[24], me_path[40], wpa_ssid[33], wpa_ip[16], net_line[128];
static char nearby[DASH_NEARBY][33];
static int nearby_n;
static bool scan_wanted;
static int ssh_state = -1;   // -1 off, else the sessions open

// a new list only when the set of names changes: a signal reshuffle must not move a row under a finger
static void nearby_take(const char *reply) {
  char got[DASH_NEARBY][33];
  int n = wifi_scan_parse(reply, got, DASH_NEARBY), same = n == nearby_n;
  for (int i = 0; i < n && same; i++) {
    int k = 0;
    while (k < nearby_n && strcmp(got[i], nearby[k])) k++;
    same = k < nearby_n;
  }
  if (!same) memcpy(nearby, got, sizeof got), nearby_n = n;
}

// one STATUS key's value from a reply ("\nssid=" so bssid= does not match)
static void wpa_key(const char *b, const char *key, char *out, size_t n) {
  const char *s = strstr(b, key);
  if (!s) { out[0] = 0; return; }
  s += strlen(key);
  snprintf(out, n, "%.*s", (int)strcspn(s, "\n"), s);
}

// wpa_supplicant's printf_encode undone in place (\\ \" \n \r \t \e \xNN), so a name like "Café" reads as typed
static void wpa_unescape(char *s) {
  static const char hex[] = "0123456789abcdef";
  char *o = s;
  while (*s) {
    if (*s != '\\' || !s[1]) { *o++ = *s++; continue; }
    char c = s[1];
    const char *h1 = c == 'x' && s[2] ? strchr(hex, s[2] | 0x20) : NULL, *h2 = h1 && s[3] ? strchr(hex, s[3] | 0x20) : NULL;
    if (h1 && h2) { *o++ = (char)((h1 - hex) << 4 | (h2 - hex)); s += 4; continue; }
    *o++ = c == 'n' ? '\n' : c == 'r' ? '\r' : c == 't' ? '\t' : c == 'e' ? 033 : c;
    s += 2;
  }
  *o = 0;
}

// ssh: dropbear's port 22 in /proc/net/tcp{,6}: listening (st 0A) and the sessions open on it (st 01)
static void ssh_look(bool *on, int *open) {
  static const char *const tabs[] = {"/proc/net/tcp", "/proc/net/tcp6"};
  char l[256];
  *on = false, *open = 0;
  for (unsigned i = 0; i < 2; i++) {
    FILE *f = fopen(tabs[i], "r");
    if (!f) continue;
    while (fgets(l, sizeof l, f)) {
      char local[64]; unsigned st;
      if (sscanf(l, " %*d: %63s %*s %x", local, &st) != 2) continue;
      const char *port = strrchr(local, ':');
      if (!port || strcmp(port + 1, "0016")) continue;
      if (st == 0x0A) *on = true;
      else if (st == 0x01) (*open)++;
    }
    fclose(f);
  }
}

// The reply to the last STATUS (none: unknown), then the next asked. A wpa_supplicant restarted by a settings save
// leaves this socket talking to nobody: the send fails, and the next look opens a new one.
static const char *wpa_look(void) {
  if (wpa < 0) {
    struct sockaddr_un me = {.sun_family = AF_UNIX}, it = {.sun_family = AF_UNIX};
    snprintf(me_path, sizeof me_path, "/tmp/dash-wpa-%d", (int)getpid());   // wpa_supplicant answers to a bound name
    snprintf(me.sun_path, sizeof me.sun_path, "%s", me_path);
    snprintf(it.sun_path, sizeof it.sun_path, "/run/wpa_supplicant/wlan0");
    unlink(me_path);
    wpa = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (wpa >= 0 && (bind(wpa, (struct sockaddr *)&me, sizeof me) || connect(wpa, (struct sockaddr *)&it, sizeof it))) {
      close(wpa), unlink(me_path), wpa = -1;
    }
    asked = false;
    if (wpa < 0) return NULL;
  }
  char b[4096], *s;
  ssize_t n;
  bool got = false;
  while ((n = recv(wpa, b, sizeof b - 1, 0)) > 0) {
    b[n] = 0;
    if (!strncmp(b, "bssid /", 7)) { nearby_take(b); continue; }   // SCAN_RESULTS
    if (!strstr(b, "wpa_state=")) continue;   // SCAN's OK / FAIL-BUSY
    char ssid[4 * 32 + 1];   // escaped: up to 4 bytes per byte
    wpa_key(b, "\nssid=", ssid, sizeof ssid);
    wpa_unescape(ssid);
    snprintf(wpa_ssid, sizeof wpa_ssid, "%.32s", ssid);   // an SSID is 32 bytes at most
    wpa_key(b, "ip_address=", wpa_ip, sizeof wpa_ip);
    if ((s = strstr(b, "wpa_state="))) s += 10, s[strcspn(s, "\n")] = 0, snprintf(wpa_state, sizeof wpa_state, "%s", s), got = true;
  }
  if (asked && !got) wpa_state[0] = wpa_ssid[0] = wpa_ip[0] = 0;   // no answer in 1.5 s: not known
  // (no scan while Android Auto holds the radio: it would take the one radio off the access point's channel, and the
  // phone's picture with it; asked again once the session is over)
  if (scan_wanted && access("/run/dash-aa-radio", F_OK) != 0 && send(wpa, "SCAN", 4, 0) == 4) scan_wanted = false;
  asked = send(wpa, "STATUS", 6, 0) == 6;
  if (asked) send(wpa, "SCAN_RESULTS", 12, 0);
  if (!asked) close(wpa), unlink(me_path), wpa = -1, wpa_state[0] = 0;
  return wpa_state[0] ? wpa_state : NULL;
}

// wlan0 has an IPv4 address now (not the 1.5 s look's), on a link that is up: a lease outlives a disconnect (udhcpc
// keeps it; dash-aa's `wpa_cli disconnect` for the AP), and that is no stream about to start
bool wifi_has_addr(void) {
  struct ifaddrs *ifs, *i;
  bool ipv4 = false;
  if (getifaddrs(&ifs)) return false;
  for (i = ifs; i; i = i->ifa_next)
    ipv4 |= i->ifa_addr && i->ifa_addr->sa_family == AF_INET && !strcmp(i->ifa_name, "wlan0") && (i->ifa_flags & IFF_RUNNING);
  freeifaddrs(ifs);
  return ipv4;
}

int wifi_join(int64_t now, const char **why) {
  static int64_t next;
  static int state = -1;
  static const char *said = "";
  static wifi_track_t t;
  if (now >= next) {
    next = now + 1500;
    char oper[16] = "";
    FILE *f = fopen("/sys/class/net/wlan0/operstate", "r");
    bool exists = f != NULL;
    if (f) { if (!fgets(oper, sizeof oper, f)) oper[0] = 0; fclose(f); oper[strcspn(oper, "\n")] = 0; }
    bool ipv4 = exists && wifi_has_addr();
    const char *ws = exists ? wpa_look() : NULL;
    state = wifi_join_of(&t, exists, oper, ipv4, ws, &said);
    bool ssh_on; int ssh_open;
    ssh_look(&ssh_on, &ssh_open);
    static int held; static int64_t held_until;   // a session just closed reads as open 10 s more: no 1/0 flicker
    if (ssh_open >= held) held = ssh_open, held_until = now + 10000;
    else if (now < held_until) ssh_open = held;
    else held = ssh_open;
    ssh_state = ssh_on ? ssh_open : -1;
    char sh[40];
    if (!ssh_on) snprintf(sh, sizeof sh, "SSH off");
    else if (!ssh_open) snprintf(sh, sizeof sh, "SSH on, no session");
    else snprintf(sh, sizeof sh, "SSH on, %d session%s open", ssh_open, ssh_open == 1 ? "" : "s");
    if (is(ws, "COMPLETED") && wpa_ssid[0])
      snprintf(net_line, sizeof net_line, "Network: %s, %s · %s", wpa_ssid, wpa_ip[0] ? wpa_ip : "no address yet", sh);
    else snprintf(net_line, sizeof net_line, "Network: not joined · %s", sh);
  }
  *why = said;
  return state;
}
const char *wifi_net_line(void) { return net_line; }
const char *wifi_ssid(void) { return wpa_state[0] && !strcmp(wpa_state, "COMPLETED") ? wpa_ssid : ""; }
int wifi_nearby(const char (**list)[33]) { *list = (const char (*)[33])nearby; return nearby_n; }
void wifi_scan(void) { scan_wanted = true; }
int wifi_ssh(void) { return ssh_state; }
#else
int wifi_join(int64_t now, const char **why) { (void)now, *why = ""; return -1; }   // the Mac: the link says it all
bool wifi_has_addr(void) { return true; }
const char *wifi_net_line(void) { return ""; }
const char *wifi_ssid(void) { return ""; }
int wifi_nearby(const char (**list)[33]) { *list = NULL; return 0; }
void wifi_scan(void) {}
int wifi_ssh(void) { return -1; }
#endif
