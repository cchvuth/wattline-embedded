// BlueZ GATT transport and the session thread (see ble_bluez.h).
#define _GNU_SOURCE
#include "ble_bluez.h"
#include <errno.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/random.h>
#endif

#define BLUEZ "org.bluez"
#define PROPS "org.freedesktop.DBus.Properties"
#define ADAPTER_IF "org.bluez.Adapter1"
#define DEV_IF "org.bluez.Device1"
#define SVC_IF "org.bluez.GattService1"
#define CHR_IF "org.bluez.GattCharacteristic1"
#define DESC_IF "org.bluez.GattDescriptor1"
#define UUID_2901 "00002901-0000-1000-8000-00805f9b34fb"
enum { PATH_MAX_ = 192 };

// ---- pure helpers ----

int ble_chr_of(const uint8_t *text, size_t n) {
  static const char *const names[] = {"prov-session", "enhapi", "push-enhapi"};  // CMDR_CHR_* order
  char s[64];
  size_t k = 0;
  for (size_t i = 0; i < n; i++) // as the phone app: only bytes >= 0x20 count
    if (text[i] >= 0x20) {
      if (k == sizeof s - 1) return -1;
      s[k++] = (char)text[i];
    }
  s[k] = 0;
  for (int i = 0; i < 3; i++)
    if (!strcmp(s, names[i])) return i;
  return -1;
}

int ble_has_for_mtu(int has, int mtu) { return mtu > 0 && mtu < BLE_MTU_MIN ? has & ~CMDR_HAS_PUSH : has; }

static int64_t now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// ---- the transport ----

typedef struct {
  GDBusConnection *bus;
  guint sub;
  char adapter[64], dev[PATH_MAX_], chr[3][PATH_MAX_];
  int mtu, notifying;
  // set by the session thread; called from BlueZ's signals on that thread
  void (*on_push)(void *u, const uint8_t *d, size_t n);
  void (*on_lost)(void *u);
  void (*on_power)(void *u, int on);
  void *u;
} bz;

static GVariant *call(bz *t, const char *path, const char *iface, const char *method, GVariant *args,
                      const char *ret, int ms) {
  GError *e = NULL;
  GVariant *r = g_dbus_connection_call_sync(t->bus, BLUEZ, path, iface, method, args, ret ? G_VARIANT_TYPE(ret) : NULL,
                                            G_DBUS_CALL_FLAGS_NONE, ms, NULL, &e);
  if (!r) {
    fprintf(stderr, "ble: %s %s: %s\n", method, path, e->message);
    g_error_free(e);
  }
  return r;
}

static int ok(GVariant *r) { // a reply we only wait for
  if (r) g_variant_unref(r);
  return r ? 0 : -1;
}

static GVariant *prop(bz *t, const char *path, const char *iface, const char *name) {
  GVariant *v, *r = call(t, path, PROPS, "Get", g_variant_new("(ss)", iface, name), "(v)", 5000);
  if (!r) return NULL;
  g_variant_get(r, "(v)", &v);
  g_variant_unref(r);
  return v;
}

static int prop_bool(bz *t, const char *path, const char *iface, const char *name) {
  GVariant *v = prop(t, path, iface, name);
  int on = v && g_variant_is_of_type(v, G_VARIANT_TYPE_BOOLEAN) && g_variant_get_boolean(v);
  if (v) g_variant_unref(v);
  return on;
}

static GVariant *no_opts(void) { return g_variant_new_array(G_VARIANT_TYPE("{sv}"), NULL, 0); }

static GVariant *objects(bz *t) { return call(t, "/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects", NULL, "(a{oa{sa{sv}}})", 5000); }

static int under(const char *path, const char *parent) {
  size_t n = strlen(parent);
  return n && !strncmp(path, parent, n) && path[n] == '/';
}

static void pushes(GDBusConnection *bus, const char *sender, const char *path, const char *iface, const char *sig,
                   GVariant *p, gpointer u) {
  (void)bus, (void)sender, (void)iface, (void)sig;
  bz *t = u;
  const char *ifc;
  GVariant *ch, *v;
  g_variant_get(p, "(&s@a{sv}@as)", &ifc, &ch, NULL);
  if (t->notifying && !strcmp(path, t->chr[CMDR_CHR_PUSH]) && !strcmp(ifc, CHR_IF) &&
      (v = g_variant_lookup_value(ch, "Value", G_VARIANT_TYPE_BYTESTRING))) {
    gsize n;
    const uint8_t *d = g_variant_get_fixed_array(v, &n, 1);
    if (t->on_push) t->on_push(t->u, d, n);
    g_variant_unref(v);
  } else if (t->dev[0] && !strcmp(path, t->dev) && !strcmp(ifc, DEV_IF) &&
             (v = g_variant_lookup_value(ch, "Connected", G_VARIANT_TYPE_BOOLEAN))) {
    // Signals wait while a call blocks: a drop from before a redial is checked against the link now.
    if (!g_variant_get_boolean(v) && !prop_bool(t, t->dev, DEV_IF, "Connected") && t->on_lost) t->on_lost(t->u);
    g_variant_unref(v);
  } else if (!strcmp(path, t->adapter) && !strcmp(ifc, ADAPTER_IF) &&
             (v = g_variant_lookup_value(ch, "Powered", G_VARIANT_TYPE_BOOLEAN))) {
    if (t->on_power) t->on_power(t->u, g_variant_get_boolean(v));
    g_variant_unref(v);
  }
  g_variant_unref(ch);
}

static int io_adapter_on(void *ctx) {
  bz *t = ctx;
  GVariant *v = prop(t, t->adapter, ADAPTER_IF, "Powered");
  if (!v) return 0; // no controller (yet)
  int on = g_variant_get_boolean(v);
  g_variant_unref(v);
  if (!on) // AutoEnable normally has; nobody else switches it here
    on = !ok(call(t, t->adapter, PROPS, "Set", g_variant_new("(ssv)", ADAPTER_IF, "Powered", g_variant_new_boolean(TRUE)),
                  NULL, 5000));
  return on;
}

static int has_uuid(GVariant *props, const char *uuid) {
  const char **u;
  int hit = 0;
  if (!g_variant_lookup(props, "UUIDs", "^a&s", &u)) return 0;
  for (int i = 0; u[i] && !hit; i++) hit = !strcasecmp(u[i], uuid);
  g_free(u);
  return hit;
}

static int by_rssi(const void *a, const void *b) { return ((const ble_dev *)b)->rssi - ((const ble_dev *)a)->rssi; }

int ble_scan(cmdr_io *io, const char *uuid, int ms, const char *pin, ble_dev *out, int cap) {
  bz *t = io->ctx;
  GVariantBuilder f;
  g_variant_builder_init(&f, G_VARIANT_TYPE("a{sv}"));
  g_variant_builder_add(&f, "{sv}", "Transport", g_variant_new_string("le"));
  if (uuid) g_variant_builder_add(&f, "{sv}", "UUIDs", g_variant_new_strv(&uuid, 1));
  if (ok(call(t, t->adapter, ADAPTER_IF, "SetDiscoveryFilter", g_variant_new("(a{sv})", &f), NULL, 5000)) ||
      ok(call(t, t->adapter, ADAPTER_IF, "StartDiscovery", NULL, NULL, 5000)))
    return -1;
  int n = 0, hit = 0;
  for (int64_t end = now_ms() + ms; !hit && now_ms() < end;) {
    g_usleep(250 * 1000);
    GVariant *r = objects(t);
    if (!r) break;
    GVariantIter *it;
    const char *path;
    GVariant *ifs;
    g_variant_get(r, "(a{oa{sa{sv}}})", &it);
    while (g_variant_iter_next(it, "{&o@a{sa{sv}}}", &path, &ifs)) {
      GVariant *p = g_variant_lookup_value(ifs, DEV_IF, G_VARIANT_TYPE("a{sv}"));
      gint16 rssi;
      const char *addr, *name = "";
      // RSSI is only set while heard in a discovery (BlueZ clears it when one stops)
      if (p && under(path, t->adapter) && g_variant_lookup(p, "RSSI", "n", &rssi) && g_variant_lookup(p, "Address", "&s", &addr)) {
        g_variant_lookup(p, "Name", "&s", &name);
        int i = 0;
        while (i < n && strcmp(out[i].addr, addr)) i++;
        if (i < cap) {
          if (i == n) n++;
          snprintf(out[i].addr, sizeof out[i].addr, "%s", addr);
          snprintf(out[i].name, sizeof out[i].name, "%s", name);
          out[i].rssi = rssi;
          out[i].match = has_uuid(p, CMDR_SERVICE_UUID);
          hit |= pin && out[i].match && !strcasecmp(addr, pin);
        }
      }
      if (p) g_variant_unref(p);
      g_variant_unref(ifs);
    }
    g_variant_iter_free(it);
    g_variant_unref(r);
  }
  ok(call(t, t->adapter, ADAPTER_IF, "StopDiscovery", NULL, NULL, 5000));
  qsort(out, (size_t)n, sizeof *out, by_rssi);
  return n;
}

static int io_scan(void *ctx, const char *pin, char addr[CMDR_ADDR_MAX]) {
  cmdr_io io = {.ctx = ctx};
  ble_dev d[32];
  int n = ble_scan(&io, CMDR_SERVICE_UUID, 5000, pin, d, 32);
  for (int i = 0; i < n; i++) // strongest first
    if (d[i].match && (!pin || !strcasecmp(d[i].addr, pin))) {
      memcpy(addr, d[i].addr, CMDR_ADDR_MAX); // same size, NUL-terminated by ble_scan
      return 0;
    }
  return -1;
}

static void forget_link(bz *t) {
  t->dev[0] = t->chr[0][0] = t->chr[1][0] = t->chr[2][0] = 0;
  t->mtu = t->notifying = 0;
}

static int read_value(bz *t, const char *path, const char *iface, uint8_t *r, size_t cap, size_t *rn) {
  GVariant *v, *rep = call(t, path, iface, "ReadValue", g_variant_new("(@a{sv})", no_opts()), "(ay)", 10000);
  if (!rep) return -1;
  gsize n;
  g_variant_get(rep, "(@ay)", &v);
  const uint8_t *d = g_variant_get_fixed_array(v, &n, 1);
  memcpy(r, d, n < cap ? n : cap);
  *rn = n; // > cap: the caller sees it was cut
  g_variant_unref(v);
  g_variant_unref(rep);
  return 0;
}

// The service's characteristics by their 0x2901 text. Returns the CMDR_HAS_* mask.
static int map_chars(bz *t) {
  GVariant *r = objects(t);
  if (!r) return -1;
  char svc[4][PATH_MAX_];
  int ns = 0, has = 0;
  GVariant *all = g_variant_get_child_value(r, 0), *ifs, *p;
  GVariantIter it;
  const char *path, *s;
  for (int pass = 0; pass < 2; pass++) {
    g_variant_iter_init(&it, all);
    while (g_variant_iter_next(&it, "{&o@a{sa{sv}}}", &path, &ifs)) {
      if (pass == 0 && (p = g_variant_lookup_value(ifs, SVC_IF, G_VARIANT_TYPE("a{sv}")))) {
        if (ns < 4 && under(path, t->dev) && g_variant_lookup(p, "UUID", "&s", &s) && !strcasecmp(s, CMDR_SERVICE_UUID))
          snprintf(svc[ns++], PATH_MAX_, "%s", path);
        g_variant_unref(p);
      } else if (pass == 1 && (p = g_variant_lookup_value(ifs, DESC_IF, G_VARIANT_TYPE("a{sv}")))) {
        const char *chr;
        int in = 0;
        if (g_variant_lookup(p, "UUID", "&s", &s) && !strcasecmp(s, UUID_2901) && g_variant_lookup(p, "Characteristic", "&o", &chr))
          for (int i = 0; i < ns && !in; i++) in = under(chr, svc[i]);
        uint8_t text[64];
        size_t n;
        int k;
        if (in && !read_value(t, path, DESC_IF, text, sizeof text, &n) && (k = ble_chr_of(text, n < sizeof text ? n : 0)) >= 0 &&
            !t->chr[k][0]) {
          snprintf(t->chr[k], PATH_MAX_, "%s", chr);
          has |= 1 << k;
        }
        g_variant_unref(p);
      }
      g_variant_unref(ifs);
    }
  }
  g_variant_unref(all);
  g_variant_unref(r);
  return has;
}

static int io_connect(void *ctx, const char *addr) {
  bz *t = ctx;
  forget_link(t);
  int n = snprintf(t->dev, sizeof t->dev, "%s/dev_%s", t->adapter, addr);
  for (char *c = t->dev + n - strlen(addr); *c; c++) if (*c == ':') *c = '_';
  int64_t end = now_ms() + 15000;
  if (ok(call(t, t->dev, DEV_IF, "Connect", NULL, NULL, 15000))) return -1;
  while (!prop_bool(t, t->dev, DEV_IF, "ServicesResolved")) {
    if (now_ms() >= end || !prop_bool(t, t->dev, DEV_IF, "Connected")) return -1;
    g_usleep(100 * 1000);
  }
  int has = map_chars(t);
  if (has < 0) return -1;
  for (int k = 0; k < 3 && !t->mtu; k++) {
    GVariant *v = t->chr[k][0] ? prop(t, t->chr[k], CHR_IF, "MTU") : NULL;
    if (v) t->mtu = g_variant_get_uint16(v), g_variant_unref(v);
  }
  if (ble_has_for_mtu(has, t->mtu) != has) fprintf(stderr, "ble: MTU %d: pushes would be cut, not subscribing\n", t->mtu);
  return ble_has_for_mtu(has, t->mtu);
}

static int io_rpc(void *ctx, int chr, const uint8_t *w, size_t wn, uint8_t *r, size_t cap, size_t *rn) {
  bz *t = ctx;
  *rn = 0;
  if (chr < 0 || chr > CMDR_CHR_PUSH || !t->chr[chr][0]) return -1;
  GVariantBuilder o;
  g_variant_builder_init(&o, G_VARIANT_TYPE("a{sv}"));
  g_variant_builder_add(&o, "{sv}", "type", g_variant_new_string("request"));
  GVariant *data = g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, w, wn, 1);
  if (ok(call(t, t->chr[chr], CHR_IF, "WriteValue", g_variant_new("(@aya{sv})", data, &o), NULL, 10000))) return -1;
  return read_value(t, t->chr[chr], CHR_IF, r, cap, rn);
}

static int io_notify(void *ctx, int on) {
  bz *t = ctx;
  if (!t->chr[CMDR_CHR_PUSH][0]) return -1;
  if (ok(call(t, t->chr[CMDR_CHR_PUSH], CHR_IF, on ? "StartNotify" : "StopNotify", NULL, NULL, 5000))) return -1;
  t->notifying = on;
  return 0;
}

static void io_disconnect(void *ctx) {
  bz *t = ctx;
  if (t->dev[0]) ok(call(t, t->dev, DEV_IF, "Disconnect", NULL, NULL, 5000));
  forget_link(t);
}

static int io_random(void *ctx, uint8_t *b, size_t n) {
  (void)ctx;
#ifdef __linux__
  return getrandom(b, n, 0) == (ssize_t)n ? 0 : -1;
#else
  arc4random_buf(b, n);
  return 0;
#endif
}

int ble_bluez_open(cmdr_io *io, const char *adapter_path) {
  GError *e = NULL;
  GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &e);
  if (!bus) {
    fprintf(stderr, "ble: system bus: %s\n", e->message);
    g_error_free(e);
    return -1;
  }
  bz *t = g_new0(bz, 1);
  t->bus = bus;
  snprintf(t->adapter, sizeof t->adapter, "%s", adapter_path);
  *io = (cmdr_io){t, io_adapter_on, io_scan, io_connect, io_rpc, io_notify, io_disconnect, io_random};
  return 0;
}

void ble_bluez_close(cmdr_io *io) {
  bz *t = io->ctx;
  if (!t) return;
  if (t->sub) g_dbus_connection_signal_unsubscribe(t->bus, t->sub);
  g_object_unref(t->bus);
  g_free(t);
  io->ctx = NULL;
}

int ble_mtu(const cmdr_io *io) { return ((const bz *)io->ctx)->mtu; }

// ---- the session thread ----

struct ble {
  cmdr_t c;  // the thread's alone
  cmdr_io io;
  int own_io;
  int64_t look_at;
  GMainContext *ctx;
  GMainLoop *loop;
  GThread *th;
  GAsyncQueue *rep;
  int pfd[2];
  GMutex mu;  // guards pub, until
  cmdr_t pub;
  int64_t until;
};
enum { BLE_STOP = -1 };
typedef struct { ble *b; ble_msg m; } req;

static void publish(ble *b) {
  g_mutex_lock(&b->mu);
  memcpy(&b->pub, &b->c, offsetof(cmdr_t, cfg));
  b->until = cmdr_until(&b->c);
  g_mutex_unlock(&b->mu);
}

// ponytail: unbounded queue; an app that stops draining grows it ~5 KB/s (pushes) until the watchdog.
static void reply(ble *b, const ble_msg *m) {
  g_async_queue_push(b->rep, g_memdup2(m, sizeof *m));
  if (write(b->pfd[1], "", 1) < 0 && errno != EAGAIN) perror("ble: reply pipe");
}

static void on_data(void *u, const uint8_t *d, size_t n) {
  ble_msg m = {.op = BLE_DATA, .n = d ? n : 0};
  if (m.n > sizeof m.data) return; // cannot be: a push is one notification
  if (m.n) memcpy(m.data, d, m.n);
  reply(u, &m);
}

static void on_push(void *u, const uint8_t *d, size_t n) { cmdr_on_push(&((ble *)u)->c, d, n), publish(u); }
static void on_lost(void *u) { cmdr_link_lost(&((ble *)u)->c, now_ms()), publish(u); }
static void on_power(void *u, int on) { cmdr_adapter(&((ble *)u)->c, on, now_ms()), publish(u); }

static gboolean tick(gpointer u) {
  ble *b = u;
  int64_t now = now_ms();
  if (b->c.state == CS_OFF && now >= b->look_at) { // a controller late, or switched off, comes back here
    b->look_at = now + 5000;
    if (b->io.adapter_on(b->io.ctx)) cmdr_adapter(&b->c, 1, now);
  }
  cmdr_tick(&b->c, now);
  publish(b);
  return G_SOURCE_CONTINUE;
}

static gboolean request(gpointer u) {
  ble *b = ((req *)u)->b;
  ble_msg *m = &((req *)u)->m;
  cmdr_t *c = &b->c;
  int64_t now = now_ms();
  m->result = 0;
  switch (m->op) {
  case BLE_CONNECT: cmdr_connect(c, now); break;
  case BLE_KICK: cmdr_kick(c, m->a, now); break;
  case BLE_FORGET: cmdr_forget(c, now); break;
  case BLE_WIFI: cmdr_wifi(c, m->a, m->b, now); break;
  case BLE_STEP_ASIDE: m->result = cmdr_step_aside(c, now), reply(b, m); break;
  case BLE_PAUSE: m->result = cmdr_pause(c, now), reply(b, m); break;
  case BLE_RESUME: cmdr_resume(c, now); break;
  case BLE_DATA_HOLD: cmdr_data_hold(c, m->a, now); break;
  case BLE_PRESS: m->result = cmdr_press(c, (cmdr_action)m->a, m->b, m->gear, now), reply(b, m); break;
  case BLE_STOP: cmdr_pause(c, now), g_main_loop_quit(b->loop); break;
  }
  publish(b);
  return G_SOURCE_REMOVE;
}

static gpointer run(gpointer u) {
  ble *b = u;
  g_main_context_push_thread_default(b->ctx);
  g_main_loop_run(b->loop);
  g_main_context_pop_thread_default(b->ctx);
  return NULL;
}

ble *ble_start_io(const cmdr_cfg *cfg, const cmdr_io *io) {
  ble *b = g_new0(ble, 1);
  if (pipe(b->pfd)) return g_free(b), NULL;
  for (int i = 0; i < 2; i++) fcntl(b->pfd[i], F_SETFL, O_NONBLOCK), fcntl(b->pfd[i], F_SETFD, FD_CLOEXEC);
  cmdr_cfg cf = cfg ? *cfg : cmdr_cfg_default;
  cf.on_data = on_data, cf.data_ctx = b;
  b->io = *io;
  cmdr_init(&b->c, &cf, io);
  g_mutex_init(&b->mu);
  publish(b);
  b->rep = g_async_queue_new_full(g_free);
  b->ctx = g_main_context_new();
  b->loop = g_main_loop_new(b->ctx, FALSE);
  GSource *s = g_timeout_source_new(100);
  g_source_set_callback(s, tick, b, NULL);
  g_source_attach(s, b->ctx);
  g_source_unref(s);
  b->th = g_thread_new("ble", run, b);
  return b;
}

ble *ble_start(const cmdr_cfg *cfg, const char *adapter_path) {
  cmdr_io io;
  if (ble_bluez_open(&io, adapter_path)) return NULL;
  bz *t = io.ctx;
  ble *b = ble_start_io(cfg, &io);
  if (!b) return ble_bluez_close(&io), NULL;
  b->own_io = 1;
  t->on_push = on_push, t->on_lost = on_lost, t->on_power = on_power, t->u = b;
  // Subscribed with the thread's context as the default: the signals run on that thread.
  g_main_context_push_thread_default(b->ctx);
  t->sub = g_dbus_connection_signal_subscribe(t->bus, BLUEZ, PROPS, "PropertiesChanged", NULL, NULL,
                                              G_DBUS_SIGNAL_FLAGS_NONE, pushes, t, NULL);
  g_main_context_pop_thread_default(b->ctx);
  return b;
}

int ble_fd(ble *b) { return b->pfd[0]; }

int ble_post(ble *b, const ble_msg *m) {
  req *q = g_new(req, 1);
  q->b = b, q->m = *m;
  g_main_context_invoke_full(b->ctx, G_PRIORITY_DEFAULT, request, q, g_free); // FIFO at one priority
  return 0;
}

int ble_take(ble *b, ble_msg *m) {
  ble_msg *q = g_async_queue_try_pop(b->rep);
  if (!q) { // drained: clear the wake bytes, then look once more for one pushed meanwhile
    char buf[64];
    while (read(b->pfd[0], buf, sizeof buf) > 0) {}
    if (!(q = g_async_queue_try_pop(b->rep))) return 0;
  }
  *m = *q;
  g_free(q);
  return 1;
}

int64_t ble_state(ble *b, cmdr_t *pub) {
  g_mutex_lock(&b->mu);
  memcpy(pub, &b->pub, offsetof(cmdr_t, cfg));
  int64_t until = b->until;
  g_mutex_unlock(&b->mu);
  return until;
}

void ble_stop(ble *b) {
  ble_post(b, &(ble_msg){.op = BLE_STOP});
  g_thread_join(b->th);
  if (b->own_io) ble_bluez_close(&b->io);
  g_main_loop_unref(b->loop);
  g_main_context_unref(b->ctx);
  g_async_queue_unref(b->rep);
  g_mutex_clear(&b->mu);
  close(b->pfd[0]), close(b->pfd[1]);
  g_free(b);
}
