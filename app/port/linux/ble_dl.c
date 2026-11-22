// ble_bluez.h's session calls through libdashble.so (ble_bluez.c with its own copy of the session's code, app.mk
// BLE_SO_SRC): the profile's BLE_DL builds (the F133) compile this instead of ble_bluez.c, so the dash links no GLib. 0.9 s of the
// dash's start at boot was the loader reading GLib and GIO from the flash, for a session that cannot start until
// dbus-daemon is up, seconds after the first frame.
// ble_start returns NULL ("no system bus yet", as ble_bluez.c's does) until the system bus's socket is there, then
// starts the load on a thread of its own (the read takes up to a second: never on the loop's thread) and returns NULL
// until that is done; the next try after it starts the session. A plugin that cannot be loaded is said once and the
// dash goes on without Bluetooth, as when ble_start fails for good. Never unloaded.
#define _GNU_SOURCE
#include "ble_bluez.h"
#include <dlfcn.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

void log_msg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));   // app/log.c

#ifndef BLE_DL_BUS   // the system bus's address (os-f133's dbus system.conf listen; GDBus's default, /var/run -> /run)
#define BLE_DL_BUS "/run/dbus/system_bus_socket"
#endif
#define PLUGIN "libdashble.so"

static struct {
  ble *(*start)(const cmdr_cfg *, const char *);
  void (*stop)(ble *);
  int (*fd)(ble *);
  int (*post)(ble *, const ble_msg *);
  int (*take)(ble *, ble_msg *);
  int64_t (*state)(ble *, cmdr_t *);
} fn;
enum { IDLE, LOADING, READY, FAILED, SAID };
// Written by the loader thread once (READY or FAILED) after fn, why and took_ms, and read by the loop's thread before
// them: seq_cst stores and loads order those plain writes and reads (release / acquire), and nothing else is shared.
static atomic_int phase;
static char why[PATH_MAX];   // the plugin's path, or dlerror's words
static long took_ms;

static long ms_now(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000L + ts.tv_nsec / 1000000;
}

static void *load(void *arg) {
  (void)arg;
  long t0 = ms_now();
  char path[PATH_MAX];
  ssize_t n = -1;   // beside the running dash (/usr/lib/dash, a card's /mnt/sd/dash), else the image's
#ifdef __APPLE__
  uint32_t cap = sizeof path;
  if (!_NSGetExecutablePath(path, &cap)) n = (ssize_t)strlen(path);
#else
  n = readlink("/proc/self/exe", path, sizeof path - 1);
#endif
  char *slash = n > 0 ? (path[n] = 0, strrchr(path, '/')) : NULL;
  if (slash && (size_t)(slash + 1 - path) + sizeof PLUGIN <= sizeof path) memcpy(slash + 1, PLUGIN, sizeof PLUGIN);
  else snprintf(path, sizeof path, "/usr/lib/dash/" PLUGIN);
  void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);   // every relocation here, none later on the loop's thread
  if (h && (fn.start = dlsym(h, "ble_start")) && (fn.stop = dlsym(h, "ble_stop")) && (fn.fd = dlsym(h, "ble_fd")) &&
      (fn.post = dlsym(h, "ble_post")) && (fn.take = dlsym(h, "ble_take")) && (fn.state = dlsym(h, "ble_state"))) {
    snprintf(why, sizeof why, "%s", path);
    took_ms = ms_now() - t0;
    atomic_store(&phase, READY);
  } else {   // dlerror names the file or the symbol; h stays loaded (no dlclose: nothing of it is used)
    const char *e = dlerror();
    snprintf(why, sizeof why, "%s", e ? e : path);
    atomic_store(&phase, FAILED);
  }
  return NULL;
}

ble *ble_start(const cmdr_cfg *cfg, const char *adapter_path) {
  switch (atomic_load(&phase)) {
  case IDLE: {   // the loader thread is detached: it touches only this file's statics, so the dash may exit under it
                 // (exit's destructors wait for a dlopen under way, then run the plugin's too)
    pthread_t th;
    pthread_attr_t at;
    if (access(BLE_DL_BUS, F_OK)) return NULL;
    atomic_store(&phase, LOADING);
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&th, &at, load, NULL)) atomic_store(&phase, IDLE);   // tried again on the next start
    pthread_attr_destroy(&at);
    return NULL;
  }
  case READY: {
    static int said;
    if (!said) log_msg("bluetooth: %s loaded in %ld ms", why, took_ms), said = 1;
    return fn.start(cfg, adapter_path);
  }
  case FAILED:
    log_msg("bluetooth: no Commander session: %s", why);
    atomic_store(&phase, SAID);
    return NULL;
  default: return NULL;
  }
}

// Only ever given a ble from fn.start, so fn is set (READY was read before it).
void ble_stop(ble *b) { fn.stop(b); }
int ble_fd(ble *b) { return fn.fd(b); }
int ble_post(ble *b, const ble_msg *m) { return fn.post(b, m); }
int ble_take(ble *b, ble_msg *m) { return fn.take(b, m); }
int64_t ble_state(ble *b, cmdr_t *pub) { return fn.state(b, pub); }
