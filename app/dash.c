// The loop (PLAN-merged 5.1, 5.3, 5.6): the Panda UDP socket, core/sched driving the link, a frame every
// frame_ms that maps the car state to the UI, the backlight from the car's lamps, the clock from CAN 0x318.
// Time is CLOCK_MONOTONIC throughout. Datagrams carry the kernel's arrival stamp (SO_TIMESTAMP), so a frame
// drawn while they queue does not shift the flasher's or the grip hold's timing.
#include "app.h"
#include "../core/panda.h"
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
#include <time.h>
#include <unistd.h>
#ifdef DASH_CMDR
#include "cmdr_hook.h"
#endif

int dash_bl_pct = -1;
static volatile sig_atomic_t stop;
static void on_sig(int s) { (void)s; stop = 1; }

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
  bool clock_done, night, bl_err;
  int bl_max;
  int64_t live_at;
} dash_t;

// ---- the link's port: one connected UDP socket per attempt, as the app's RawDatagramSocket ----
static bool p_open(void *x) {
  dash_t *d = x;
  if (d->fd >= 0) close(d->fd), d->fd = -1;
  memset(d->seen, 0, sizeof d->seen);
  d->clock_done = false;
#ifdef DASH_CMDR
  cmdr_hook_step_aside();   // Bluetooth steps aside before the hello (ponytail: the hello does not wait out its settle)
#endif
  struct addrinfo hint = {.ai_family = AF_INET, .ai_socktype = SOCK_DGRAM}, *ai;
  char port[8];
  snprintf(port, sizeof port, "%d", d->cf->panda_port);
  int e = getaddrinfo(d->cf->panda_host, port, &hint, &ai);
  if (e) { log_msg("panda %s: %s", d->cf->panda_host, gai_strerror(e)); return false; }
  int fd = socket(AF_INET, SOCK_DGRAM, 0), one = 1, buf = 256 * 1024;
  if (fd >= 0) {
    fcntl(fd, F_SETFL, O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    setsockopt(fd, SOL_SOCKET, SO_TIMESTAMP, &one, sizeof one);
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf, sizeof buf);
    if (connect(fd, ai->ai_addr, ai->ai_addrlen)) {   // no route yet: WiFi not joined
      log_msg("panda %s:%d: %s", d->cf->panda_host, d->cf->panda_port, strerror(errno));
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
    int fd = d->fd;
    link_on_datagram(&d->link, b, (size_t)n, at);
    if (d->fd == fd) panda_parse(b, (size_t)n, mark_seen, d);   // a frozen link starts over without it
  }
}

// ---- the clock: CAN 0x318 (UTC, 1 s steps) carried on by the monotonic clock ----
static int64_t utc_now(const car_t *c, int64_t now) {
  if (c->utc.at == NEVER) return -1;
  struct tm tm = {.tm_year = c->utc.year - 1900, .tm_mon = c->utc.month - 1, .tm_mday = c->utc.day,
                  .tm_hour = c->utc.hour, .tm_min = c->utc.minute, .tm_sec = c->utc.second};
  return (int64_t)timegm(&tm) * 1000 + (now - c->utc.at);
}

static void clock_check(dash_t *d, int64_t now) {   // once per link: logs carry real dates (no NTP on S3XY_OBD)
  int64_t utc = utc_now(&d->car, now);
  if (!d->cf->set_clock || d->clock_done || utc < 0) return;
  d->clock_done = true;
  if (llabs(utc * 1000 - real_us()) < 2000000) return;
  struct timespec ts = {(time_t)(utc / 1000), (long)(utc % 1000) * 1000000};
  if (clock_settime(CLOCK_REALTIME, &ts)) log_msg("clock from 0x318: %s", strerror(errno));
  else log_msg("clock set from 0x318");
}

// ---- the backlight: the car's lamps say night; dim without car data; off when away ----
static void backlight(dash_t *d, int64_t now, int64_t idle) {
  const conf_t *cf = d->cf;
  const car_t *c = &d->car;
  bool live = car_live(c);
  if (live) {
    d->live_at = now;
    if (c->low_beam >= 0 || c->high_beam >= 0) d->night = c->low_beam == 1 || c->high_beam == 1;
  }
  int pct = live ? (d->night ? cf->bright_night : cf->bright_day) : cf->bright_idle;
  pct = pct < cf->bright_min ? cf->bright_min : pct > cf->bright_max ? cf->bright_max : pct;
  int64_t away = (int64_t)cf->away_min * 60000;
  if (!live && away > 0 && now - d->live_at >= away && idle >= away) pct = 0;
  if (pct == dash_bl_pct || !cf->backlight[0]) return;
  char p[160];
  if (!d->bl_max) {
    snprintf(p, sizeof p, "%s/max_brightness", cf->backlight);
    FILE *f = fopen(p, "r");
    if (!f || fscanf(f, "%d", &d->bl_max) != 1 || d->bl_max <= 0) d->bl_max = 100;
    if (f) fclose(f);
  }
  snprintf(p, sizeof p, "%s/brightness", cf->backlight);
  FILE *f = fopen(p, "w");
  if (!f || fprintf(f, "%d\n", (pct * d->bl_max + 50) / 100) < 0) {
    if (!d->bl_err) log_msg("backlight %s: %s", p, strerror(errno));
    d->bl_err = true;
  } else {
    if (pct != dash_bl_pct) log_msg("backlight %d%%", pct);
    d->bl_err = false;
  }
  if (f) fclose(f);
  dash_bl_pct = pct;
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
void dash_press(int action) { cmdr_hook_press((cmdr_action)action, &D.car, &D.link, mono_ms()); }
#endif

int dash_run(const conf_t *cf, const ui_hooks_t *ui, int64_t run_ms) {
  dash_t *d = &D;
  memset(d, 0, sizeof *d);
  d->cf = cf;
  d->fd = -1;
  struct sigaction sa = {.sa_handler = on_sig};
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);
  signal(SIGPIPE, SIG_IGN);
  setenv("TZ", cf->tz, 1);
  tzset();

  int64_t now = mono_ms(), start = now;
  d->last = d->live_at = now;
  car_init(&d->car);
  d->car.cfg.gear_fresh_ms = cf->gear_fresh_ms;
  d->car.cfg.grip_hold_ms = cf->grip_hold_ms;
  d->car.cfg.steer_hold_ms = cf->steer_hold_ms;
  d->car.cfg.yaw_steer_ms = cf->yaw_steer_ms;
  d->car.cfg.bt_motor_bad_ms = cf->bt_motor_bad_ms;
  car_set_type(&d->car, cf->car);
  d->car.kph = cf->kph;
  link_init(&d->link, &d->car, (link_port_t){p_send, p_open, p_close, d}, now);
  d->link.cfg = cf->link;
  link_set_sparse(&d->link, cf->sparse);
  log_msg("start: panda %s:%d, car %s, tz %s", cf->panda_host, cf->panda_port, kCars[cf->car].id, cf->tz);
  link_connect(&d->link, now);
  bt_view_t bt;
  bt_view_none(&bt);
#ifdef DASH_CMDR
  cmdr_hook_start(cf);
#endif

  int64_t next_frame = now, next_stat = now + 10000;
  link_state_t was = (link_state_t)-1;
  const char *why = NULL;
  dash_state_t s;
  while (!stop && (!run_ms || now - start < run_ms)) {
    int64_t wait = next_frame - now;
    struct pollfd p[2] = {{.fd = d->fd, .events = POLLIN}, {.fd = -1, .events = POLLIN}};
#ifdef DASH_CMDR
    p[1].fd = cmdr_hook_fd();   // the session's replies: pushes, press results
#endif
    poll(p, 2, (int)(wait < 0 ? 0 : wait > cf->frame_ms ? cf->frame_ms : wait));   // fd -1 entries are skipped
    now = mono_ms();
    if (d->fd >= 0) drain(d, now);
    if (now > d->last) d->last = now;
    link_poll(&d->link, now);
#ifdef DASH_CMDR
    cmdr_hook_sync(&d->car, &d->link, &bt, now);
#endif
    if (d->link.state != was || d->link.why != why) {
      log_msg("wifi: %s", link_word(&d->link));
      was = d->link.state, why = d->link.why;
    }
    if (now >= next_frame) {
      next_frame += cf->frame_ms;
      if (next_frame <= now) next_frame = now + cf->frame_ms;   // a long frame: no catching up
      clock_check(d, now);
      bridge_in_t in = {d->seen, utc_now(&d->car, now), cf->minor_figures, cf->hour12};
      bridge_fill(&s, &d->car, &d->link, &bt, &in, now);
      ui->set(&s, ui->ctx);
      if (ui->frame) ui->frame(ui->ctx);
      backlight(d, now, ui->idle_ms ? ui->idle_ms(ui->ctx) : INT64_MAX);
    }
    if (now >= next_stat) {
      stats(d);
      next_stat = now + 10000;
    }
  }
  stats(d);
#ifdef DASH_CMDR
  cmdr_hook_stop();
#endif
  link_disconnect(&d->link, now);
  log_msg("stop");
  return 0;
}
