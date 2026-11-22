// The loop: the Panda UDP socket, core/sched driving the link, a frame every
// frame_ms that maps the car state to the UI, the backlight from the car's screen or lamps, the clock from CAN 0x318.
// Time is CLOCK_MONOTONIC throughout. Datagrams carry the kernel's arrival stamp (SO_TIMESTAMP), so a frame
// drawn while they queue does not shift the flasher's or the grip hold's timing.
#include "app.h"
#include "actions.h"
#include "pair.h"
#include "record.h"
#ifndef DASH_NO_RECORD   // a device profile with no room for drive recordings: never on, whatever the settings say
#define DASH_NO_RECORD 0
#endif
#include "settings.h"
#include "../core/panda.h"
#include "../ui/project_region.h"
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifdef DASH_CMDR
#include "cmdr_hook.h"
#endif

int dash_bl_pct = -1;
bool dash_bl_dark;
static volatile sig_atomic_t stop;
static void on_sig(int s) { (void)s; stop = 1; }

void (*app_stopping)(void);

int64_t mono_ms(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static int64_t real_us(void) {
  struct timespec t;
  clock_gettime(CLOCK_REALTIME, &t);
  return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

typedef struct {
  const conf_t *cf;
  car_t car;
  link_t link;
  int fd;
  uint8_t seen[256];   // ids heard since the socket opened: Panda.frames' keys
  int64_t last;        // the latest time handed to the link: arrival stamps never go back past it
  unsigned sent[4];    // ehllo, 0x18, 0x0f, 0x0e
  bool clock_done, bl_err;
  int64_t aside_until;  // Bluetooth asked to step aside: the hello waits until then (its settle, from the reply)
  bool aside_asked;
  char host[64];       // the Panda's address, from the settings (dash.conf's until set there)
  int port;
  int bl_max;
  bright_t bright;
  int frame_ms;                 // the frame period: dash.conf's frame_ms (33 ms: 30 a second, the panel's most)
  const ui_hooks_t *ui;
} dash_t;

// ---- the link's port: one connected UDP socket per attempt, as the app's RawDatagramSocket ----
// Before each attempt Bluetooth steps aside, and the hello waits its settle (main.dart: Bluetooth first, then the WiFi
// stream, took the Commander down). Asked once an attempt; the reply (cmdr_hook_sync) sets how long; 400 ms meanwhile.
#ifdef DASH_CMDR
// "Bluetooth waits for the WiFi stream", unless Android Auto holds the radio: WiFi cannot go live then, and a dropped
// Bluetooth link must redial (during AA the car rides it)
static int wifi_first_now(void) { return app_settings.wifi_first && access(PJ_AA_RADIO, F_OK) != 0; }
#endif

static int p_settle(void *x, int64_t now) {
#ifdef DASH_CMDR
  dash_t *d = x;
  if (cmdr_hook_fd() < 0) return 0;   // no Commander session (no system bus): nobody to step aside, no wait for it
  const char *w;
  const char *ss = wifi_ssid();   // "" until wpa_supplicant answers (the first look, or just after a join): not a no
  // the join's look is up to 1.5 s old: an address that came since is a stream about to start (step aside for it)
  if (!d->aside_asked && ((wifi_join(now, &w) >= 0 && !wifi_has_addr()) || (*ss && strcmp(ss, SETTINGS_CAR_SSID))))
    return 0;   // not joined, or joined to another network (home WiFi: no Panda there): no stream to make way for;
                // stepping aside then only kept Bluetooth down (main.dart steps aside on the car's network alone)
  if (!d->aside_asked) {
    d->aside_asked = true;
    d->aside_until = now + 400;
    cmdr_hook_step_aside();
  }
  int64_t s = cmdr_hook_settle_until();
  if (s > d->aside_until) d->aside_until = s;
  if (now < d->aside_until) return (int)(d->aside_until - now);
  if (cmdr_hook_aside_pending(now)) return 200;   // Bluetooth still mid-dial (up to 20 s): its answer, then the settle
  d->aside_asked = false;   // the next attempt asks again
#else
  (void)x, (void)now;
#endif
  return 0;
}

static bool p_open(void *x) {
  dash_t *d = x;
  if (d->fd >= 0) close(d->fd), d->fd = -1;
  memset(d->seen, 0, sizeof d->seen);
  d->clock_done = false;
  struct addrinfo hint = {.ai_family = AF_INET, .ai_socktype = SOCK_DGRAM, .ai_flags = AI_NUMERICHOST}, *ai;   // never DNS
  char port[8];
  snprintf(port, sizeof port, "%d", d->port);
  int e = getaddrinfo(d->host, port, &hint, &ai);
  if (e) { log_msg("panda %s: %s", d->host, gai_strerror(e)); return false; }
  int fd = socket(AF_INET, SOCK_DGRAM, 0), one = 1, buf = 256 * 1024;
  if (fd >= 0) {
    fcntl(fd, F_SETFL, O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    setsockopt(fd, SOL_SOCKET, SO_TIMESTAMP, &one, sizeof one);
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf, sizeof buf);
    if (connect(fd, ai->ai_addr, ai->ai_addrlen)) {   // no route yet: WiFi not joined
      log_msg("panda %s:%d: %s", d->host, d->port, strerror(errno));
      close(fd);
      fd = -1;
    }
  }
  freeaddrinfo(ai);
  d->fd = fd;
  return fd >= 0;
}

static void p_close(void *x) {
  dash_t *d = x;
  if (d->fd >= 0) close(d->fd);
  d->fd = -1;
  memset(d->seen, 0, sizeof d->seen);
}

static void p_send(void *x, const uint8_t *b, size_t n) {
  dash_t *d = x;
  if (d->fd < 0 || !panda_out_ok(b, n)) return;   // the gate again, where the bytes leave (listen-only)
  d->sent[n == 5 ? 0 : b[0] == 0x18 ? 1 : b[0] == 0x0f ? 2 : 3]++;
  send(d->fd, b, n, 0);   // a lost datagram is what the link's timers are for
}

static void mark_seen(void *x, uint16_t id, uint8_t bus, const uint8_t *data, uint8_t len) {
  (void)bus, (void)data;
  if (len) ((dash_t *)x)->seen[id >> 3] |= 1 << (id & 7);
}

// Everything queued, in order, each at its arrival time on the monotonic clock.
static void drain(dash_t *d, int64_t now) {
  int64_t real = real_us();
  static uint8_t b[4096];
  for (int k = 0; k < 4096 && d->fd >= 0; k++) {
    union { char buf[CMSG_SPACE(sizeof(struct timeval))]; struct cmsghdr align; } ctl;
    struct iovec iov = {b, sizeof b};
    struct msghdr m = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = ctl.buf, .msg_controllen = sizeof ctl.buf};
    ssize_t n = recvmsg(d->fd, &m, MSG_DONTWAIT);
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) return;
      continue;   // ECONNREFUSED (nothing at the port yet) and the like: the link's timers handle silence
    }
    if (m.msg_flags & MSG_TRUNC) continue;
    int64_t at = now;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c))
      if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_TIMESTAMP) {
        struct timeval tv;
        memcpy(&tv, CMSG_DATA(c), sizeof tv);
        at = now - (real - ((int64_t)tv.tv_sec * 1000000 + tv.tv_usec)) / 1000;
      }
    if (at > now) at = now;
    if (at < d->last) at = d->last;   // never back in time (and a realtime clock step lands here)
    d->last = at;
    record_panda(b, (size_t)n, at);   // nothing while recording is off
    int fd = d->fd;
    link_on_datagram(&d->link, b, (size_t)n, at);
    if (d->fd == fd) panda_parse(b, (size_t)n, mark_seen, d);   // a frozen link starts over without it
  }
}

// ---- the clock: CAN 0x318 (UTC, 1 s steps) carried on by the monotonic clock ----
// The car's clock (0x318), run on from when it was read; without one, the Dash's own clock once it is valid (set from
// the car earlier this boot, or otherwise past 2025): Bluetooth alone brings no car clock.
static int64_t utc_now(const car_t *c, int64_t now) {
  if (c->utc.at == NEVER) { int64_t r = real_us() / 1000; return r > 1735689600000LL ? r : -1; }
  struct tm tm = {.tm_year = c->utc.year - 1900, .tm_mon = c->utc.month - 1, .tm_mday = c->utc.day,
                  .tm_hour = c->utc.hour, .tm_min = c->utc.minute, .tm_sec = c->utc.second};
  return (int64_t)timegm(&tm) * 1000 + (now - c->utc.at);
}

static void clock_check(dash_t *d, int64_t now) {   // once per link: logs carry real dates (no NTP on S3XY_OBD)
  if (d->car.utc.at == NEVER) return;   // only the car's own clock sets the Dash's, never the Dash's fallback
  int64_t utc = utc_now(&d->car, now);
  if (!d->cf->set_clock || d->clock_done || utc < 0) return;
  d->clock_done = true;
  if (llabs(utc * 1000 - real_us()) < 2000000) return;
  struct timespec ts = {(time_t)(utc / 1000), (long)(utc % 1000) * 1000000};
  if (clock_settime(CLOCK_REALTIME, &ts)) log_msg("clock from 0x318: %s", strerror(errno));
  else log_msg("clock set from 0x318");
}

// ---- the backlight: the level from bright.c (the car's screen, its lamps, idle, away), written to sysfs ----
static void backlight(dash_t *d, int64_t now, int64_t idle) {
  const conf_t *cf = d->cf;
  int pct = bright_step(&d->bright, cf, &d->car, now, idle);
  static int night = -1;   // day or night for the AA receiver (PJ_NIGHT): written when it changes
  if (d->bright.lamps && d->bright.night != night) {   // not before the lamps are known: a restart keeps the last
    FILE *f = fopen(PJ_NIGHT, "w");
    if (f) fputs(d->bright.night ? "1\n" : "0\n", f), fclose(f), night = d->bright.night;
  }
  bool dark = pct == 0 && d->bright.src == BRIGHT_AWAY;   // off is the away rule's alone: 0 % is the lowest lit level
  if ((pct == dash_bl_pct && dark == dash_bl_dark) || !cf->backlight[0]) return;
  char p[160];
  if (!d->bl_max) {
    snprintf(p, sizeof p, "%s/max_brightness", cf->backlight);
    FILE *f = fopen(p, "r");
    if (!f || fscanf(f, "%d", &d->bl_max) != 1 || d->bl_max <= 0) d->bl_max = 100;
    if (f) fclose(f);
  }
  snprintf(p, sizeof p, "%s/brightness", cf->backlight);
  FILE *f = fopen(p, "w");
  if (!f || fprintf(f, "%d\n", dark ? 0 : bright_raw(cf, pct, d->bl_max)) < 0) {
    if (!d->bl_err) log_msg("backlight %s: %s", p, strerror(errno));
    d->bl_err = true;
  } else {
    d->bl_err = false;   // the level's changes are logged by bright.c, not each eased step
  }
  if (f) fclose(f);
  dash_bl_pct = pct, dash_bl_dark = dark;
}

// The settings screen's brightness: following the car, or the slider's level (until the switch is on again).
static bool started;   // the car and the link set up: the settings' car and polling can be applied to them
static void apply_settings(dash_t *d, int64_t now) {
  int fm = d->cf->frame_ms >= 10 && d->cf->frame_ms <= 200 ? d->cf->frame_ms : 33;   // dash.conf's, when sane
  if (fm != d->frame_ms) {
    d->frame_ms = fm;
    if (d->ui && d->ui->frame_ms) d->ui->frame_ms(d->ui->ctx, fm);   // LVGL's redraw alike
    log_msg("frame: every %d ms", fm);
  }
  if (app_settings.follow_car) bright_follow(&d->bright);
  else bright_manual(&d->bright, app_settings.brightness_pct);
  d->bright.offset = app_settings.follow_offset;   // the settings' "Follow offset", applied when following
  settings_apply_tz(app_settings.tz);   // the clocks follow at the next frame
  bool moved = strcmp(d->host, app_settings.panda_host) || d->port != app_settings.panda_port;
  snprintf(d->host, sizeof d->host, "%s", app_settings.panda_host);
  d->port = app_settings.panda_port;
  if (!started) return;
  if (moved) {   // a new Panda address: the link starts over on it
    log_msg("panda now %s:%d", d->host, d->port);
    link_connect(&d->link, now);
  }
  if (d->car.car != &kCars[app_settings.car]) car_set_type(&d->car, app_settings.car);   // the picker: at once
  link_set_sparse(&d->link, app_settings.sparse);
#ifdef DASH_CMDR
  cmdr_hook_wifi_first(wifi_first_now());
#endif
}

static const char *link_word(const link_t *l) { return l->state == LINK_LIVE ? "live" : l->why; }   // live: no _set

static void stats(const dash_t *d) {
  const link_t *l = &d->link;
  const car_t *c = &d->car;
  log_msg("wifi %s: %d frames/s, speed %d/s (gap %d ms), foreign %d/s; subscribes %d acks %d resubs %d sparse %d/%d; "
          "sent ehllo %u clear %u add %u remove %u; speed %.0f gear %c soc %.0f",
          link_word(l), l->frame_hz, l->speed_hz, l->speed_gap_ms, l->foreign_hz, l->subscribes, l->acks, l->resubscribes,
          l->sparse_ops, l->sparse_fixes, d->sent[0], d->sent[1], d->sent[2], d->sent[3], c->speed,
          c->gear ? c->gear : '-', c->soc_pct);
}

static dash_t D;

#ifdef DASH_CMDR
void dash_press(int action, int64_t at) { cmdr_hook_press((cmdr_action)action, &D.car, &D.link, at); }
#endif

int dash_run(const conf_t *cf, const ui_hooks_t *ui, int64_t run_ms) {
  dash_t *d = &D;
  memset(d, 0, sizeof *d);
  d->cf = cf;
  d->ui = ui;
  d->fd = -1;
  struct sigaction sa = {.sa_handler = on_sig};
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);
  signal(SIGPIPE, SIG_IGN);

  int64_t now = mono_ms(), start = now;
  d->last = now;
  bright_init(&d->bright, cf, now);
  settings_load(&app_settings, cf);
  apply_settings(d, now);   // the time zone too (dash.conf's until set in the settings)
  if (settings_aa_kept() && access("/etc/wattline/aa.crt", F_OK) == 0) {   // left on: up again (the phone rejoins;
    log_msg("start: Android Auto was on: brought up again");                // the car rides Bluetooth)
    settings_aa_restore();
  }
  if (ui->show_settings) ui->show_settings(ui->ctx);
  if (!settings_car_saved && ui->ask_car) ui->ask_car(ui->ctx);   // first boot: which car (main.dart _askCar)
  car_init(&d->car);
  d->car.cfg.gear_fresh_ms = cf->gear_fresh_ms;
  d->car.cfg.grip_hold_ms = cf->grip_hold_ms;
  d->car.cfg.steer_hold_ms = cf->steer_hold_ms;
  d->car.cfg.yaw_steer_ms = cf->yaw_steer_ms;
  d->car.cfg.bt_motor_bad_ms = cf->bt_motor_bad_ms;
  d->car.cfg.bright_stale_ms = cf->bright_stale_ms;
  car_set_type(&d->car, app_settings.car);   // the settings' (dash.conf's until set there)
  d->car.kph = d->car.p.car_kph = cf->kph;   // dash.conf's units until the car says (the owner's pick over both)
  int unit = settings_car_unit_load();       // what the car last said: Bluetooth's speeds are in it
  if (unit >= 0) d->car.kph = d->car.p.car_kph = unit, d->car.p.car_kph_known = true;
  double tp[4];   // the last pressures: shown until the car sends new ones (only on the move)
  settings_tyres_load(tp);
  d->car.tp_fl = tp[0], d->car.tp_fr = tp[1], d->car.tp_rl = tp[2], d->car.tp_rr = tp[3];
  link_init(&d->link, &d->car, (link_port_t){p_send, p_open, p_close, d, p_settle}, now);
  d->link.cfg = cf->link;
  link_set_sparse(&d->link, app_settings.sparse);
  log_msg("start: panda %s:%d, car %s, tz %s", d->host, d->port, kCars[app_settings.car].id, app_settings.tz);
  bt_view_t bt;
  bt_view_none(&bt);
  link_connect(&d->link, now);
  started = true;

  int64_t next_frame = now, next_stat = now + 10000;
  link_state_t was = (link_state_t)-1;
  const char *why = NULL;
#ifdef DASH_CMDR
  bool was_ready = false;
#endif
  dash_state_t s;
  while (!stop && (!run_ms || now - start < run_ms)) {
    int64_t wait = next_frame - now;
    struct pollfd p[3] = {{.fd = d->fd, .events = POLLIN}, {.fd = -1, .events = POLLIN}, {.fd = pair_fd(), .events = POLLIN}};
#ifdef DASH_CMDR
    p[1].fd = cmdr_hook_fd();   // the session's replies: pushes, press results
#endif
    poll(p, 3, (int)(wait < 0 ? 0 : wait > d->frame_ms ? d->frame_ms : wait));   // fd -1 entries are skipped
    now = mono_ms();
    if (d->fd >= 0) drain(d, now);
    if (now > d->last) d->last = now;
    // Android Auto holds the radio (dash-aa: /run/dash-aa-radio): no WiFi link attempts, so no RETRY on the dash. A link
    // that is up rides on (the AP beside the car's WiFi; a quiet one subscribes again); one that is down waits for AA to
    // let go, then tries at once. Parked, the link is not "trying": the Commander's Bluetooth goes ahead meanwhile.
    if (now >= next_frame) {
      static bool aa_parked;
      bool aa = access(PJ_AA_RADIO, F_OK) == 0;
      // (parked in the middle of Bluetooth's step-aside: that ask is spent, the next attempt asks again, where an ask
      // left standing let its hello out with Bluetooth still linked)
      if (aa && !aa_parked && d->link.state == LINK_RETRYING) link_disconnect(&d->link, now), aa_parked = true, d->aside_asked = false;
      else if (!aa && aa_parked) link_connect(&d->link, now), aa_parked = false;
    }
    link_poll(&d->link, now);
    pair_handle(now);   // the SSH pairing window's server, while it is open
    while (waitpid(-1, NULL, WNOHANG) > 0) {}   // the scripts settings.c and pair.c start (none is waited on)
    if (app_settings_refresh) {   // the settings screen opened: SSH may have come on since (the pairing turns it on)
      app_settings_refresh = false;
      bool was = app_settings.dev_ssh;
      settings_refresh(&app_settings);
      if (app_settings.dev_ssh != was && ui->show_settings) ui->show_settings(ui->ctx);
    }
    if (app_settings_dirty) {   // the settings screen's save (taken in the last frame's touch handling)
      app_settings_dirty = false;
      if (settings_save(&app_settings)) app_tell("Settings not saved: the card could not be written", now);
      apply_settings(d, now);
    }
    record_sync(app_settings.record && !DASH_NO_RECORD, cf, now);   // the switch takes effect here, at once
    {   // the WiFi join just ended (wlan0 has its address): the link tries now, not after its retry delay, in which
        // Bluetooth (waiting for WiFi to try) would dial; before cmdr_hook_sync, so it sees the try
      static int join_was = -1;
      const char *jw;
      int j = wifi_join(now, &jw);
      if (join_was >= 0 && j < 0) link_kick(&d->link, now);
      join_was = j;
    }
#ifdef DASH_CMDR
    if (now >= next_frame) cmdr_hook_wifi_first(wifi_first_now());   // a frame's look at the AA radio (sent on change)
    cmdr_hook_sync(&d->car, &d->link, &bt, now);
    bool bt_ready = bt.link == BT_BUTTONS || bt.link == BT_LEAN || bt.link == BT_FULL;
    if (bt_ready && !was_ready) link_kick(&d->link, now);   // it still scans: no WiFi with ScanMyTesla off (main.dart)
    was_ready = bt_ready;
#endif
    settings_tyres_keep((double[4]){d->car.tp_fl, d->car.tp_fr, d->car.tp_rl, d->car.tp_rr}, now, false);
    if (d->car.p.car_kph_known) settings_car_unit_keep(d->car.p.car_kph);
    if (d->link.state != was || d->link.why != why) {
      log_msg("wifi: %s", link_word(&d->link));
      was = d->link.state, why = d->link.why;
    }
    if (now >= next_frame) {
      next_frame += d->frame_ms;
      if (next_frame <= now) next_frame = now + d->frame_ms;   // a long frame: no catching up
      clock_check(d, now);
      car_set_km_pick(&d->car, app_settings.unit_miles < 0 ? -1 : !app_settings.unit_miles);   // before the bridge reads it
      bridge_in_t in = {d->seen, utc_now(&d->car, now), app_settings.minor_figures, app_settings.hour12,
                        app_settings.unit_f, app_settings.unit_psi};
      in.wifi_join = wifi_join(now, &in.join_why);
      in.net_line = wifi_net_line();
      in.ssid = wifi_ssid();
      in.nearby_n = wifi_nearby(&in.nearby);
      in.ssh = wifi_ssh();
      in.host = d->host, in.port = d->port;
      bridge_fill(&s, &d->car, &d->link, &bt, &in, now);
      app_actions_fill(&s, now);   // the press out and the snackbar's line
      ui->set(&s, ui->ctx);
      if (ui->frame) ui->frame(ui->ctx);
#ifdef DASH_CMDR
      // The Commander's session: after the first frame, and tried again each 2 s for a minute until the system bus is
      // there. Started before the loop, its start (GLib and GIO, read in from the flash: over a second on the F133)
      // came before anything was drawn; and where D-Bus starts after the dash (the F133's boot) that one try found no
      // bus, and the session stayed off until the dash was started again. (The link's hellos before the session have
      // nothing to step aside.)
      static int cmdr_tries;   // -1: started
      static int64_t cmdr_at;
      if (cmdr_tries >= 0 && now >= cmdr_at)   // every 2 s for a minute, then every 10 s (a try with no bus is cheap)
        cmdr_tries = cmdr_hook_start(cf, wifi_first_now()) ? -1 : cmdr_tries + 1,
        cmdr_at = now + (cmdr_tries < 30 ? 2000 : 10000);
#endif
      backlight(d, now, ui->idle_ms ? ui->idle_ms(ui->ctx) : INT64_MAX);
    }
    if (now >= next_stat) {
      stats(d);
      next_stat = now + 10000;
    }
  }
  stats(d);
  settings_tyres_keep((double[4]){d->car.tp_fl, d->car.tp_fr, d->car.tp_rl, d->car.tp_rr}, now, true);
  if (app_stopping) app_stopping();   // the watchdog disarmed first: the Bluetooth thread's join can outlast it
  record_sync(false, cf, now);   // flushed and closed
#ifdef DASH_CMDR
  cmdr_hook_stop();
#endif
  link_disconnect(&d->link, now);
  log_msg("stop");
  return 0;
}
