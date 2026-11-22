// port/linux/ble_dl.c: nothing loaded while the system bus's socket is absent; once it is there, the load on its
// thread and then the six calls go to the plugin; a missing or incomplete plugin said once, nothing started.
// Built three ways (Makefile): the test, with BLE_DL_BUS in build/; FAKE_PLUGIN, a plugin that counts its calls;
// FAKE_PLUGIN FAKE_BAD, one without ble_state. Each case in a child of its own (the loader's outcome is for good).
#include "../port/linux/ble_bluez.h"
#include <stdio.h>

struct ble { int start, stop, fd, post, take, state, op; char adapter[32]; };

#ifdef FAKE_PLUGIN
static ble the;
ble *ble_start(const cmdr_cfg *cfg, const char *adapter_path) {
  (void)cfg, the.start++, snprintf(the.adapter, sizeof the.adapter, "%s", adapter_path);
  return &the;
}
void ble_stop(ble *b) { b->stop++; }
int ble_fd(ble *b) { return b->fd++, 7; }
int ble_post(ble *b, const ble_msg *m) { return b->post++, b->op = m->op, 0; }
int ble_take(ble *b, ble_msg *m) { return b->take++, m->op = BLE_DATA, 1; }
#ifndef FAKE_BAD
int64_t ble_state(ble *b, cmdr_t *pub) { return (void)pub, b->state++, 42; }
#endif

#else
#include <dlfcn.h>
#include <limits.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int logs;
static char said[400];
void log_msg(const char *fmt, ...) {
  va_list a;
  va_start(a, fmt);
  vsnprintf(said, sizeof said, fmt, a);
  va_end(a);
  logs++;
  printf("  log: %s\n", said);
}

static int fails;
#define CHECK(c) ((c) ? (void)0 : (void)(fails++, printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c)))
static char dir[PATH_MAX], so[PATH_MAX + 16];   // the test's folder (build/); libdashble.so in it

static ble *try_for(int ms, int want_logs) {   // ble_start as the dash's retries call it, every 5 ms
  for (int t = 0; t < ms; t += 5) {
    ble *b = ble_start(NULL, "/org/bluez/hci0");
    if (b || (want_logs && logs >= want_logs)) return b;
    usleep(5000);
  }
  return NULL;
}

static void case_loads(void) {
  for (int i = 0; i < 3; i++) CHECK(!ble_start(NULL, "/org/bluez/hci0"));
  usleep(100000);
  CHECK(!dlopen(so, RTLD_LAZY | RTLD_NOLOAD));   // not read while the bus is absent
  CHECK(logs == 0);
  fclose(fopen(BLE_DL_BUS, "w"));
  CHECK(!ble_start(NULL, "/org/bluez/hci0"));   // this try starts the load: still none
  ble *b = try_for(5000, 0);
  CHECK(b && dlopen(so, RTLD_LAZY | RTLD_NOLOAD));
  if (!b) return;
  CHECK(logs == 1 && strstr(said, "libdashble.so loaded in"));
  CHECK(b->start == 1 && !strcmp(b->adapter, "/org/bluez/hci0"));
  ble_msg m = {.op = BLE_CONNECT};
  CHECK(ble_fd(b) == 7 && ble_post(b, &m) == 0 && b->op == BLE_CONNECT);
  m.op = -1;
  cmdr_t pub;
  CHECK(ble_take(b, &m) == 1 && m.op == BLE_DATA && ble_state(b, &pub) == 42);
  ble_stop(b);
  CHECK(b->fd == 1 && b->post == 1 && b->take == 1 && b->state == 1 && b->stop == 1);
  CHECK(ble_start(NULL, "/org/bluez/hci0") == b && b->start == 2 && logs == 1);   // said once
}

static void case_fails(const char *why) {   // the bus there from the start
  CHECK(!try_for(5000, 1) && logs == 1 && strstr(said, why));
  for (int i = 0; i < 20; i++) CHECK(!ble_start(NULL, "/org/bluez/hci0"));
  CHECK(logs == 1);
}

static void run(const char *name, const char *plugin, int bus, void (*fn)(const char *), const char *arg) {
  unlink(so), unlink(BLE_DL_BUS);
  if (plugin) {
    char t[PATH_MAX + 32];
    snprintf(t, sizeof t, "%s/%s", dir, plugin);
    CHECK(!symlink(t, so));
  }
  if (bus) fclose(fopen(BLE_DL_BUS, "w"));
  fflush(stdout);
  pid_t p = fork();
  if (!p) fn(arg), fflush(stdout), _exit(fails ? 1 : 0);
  int st = 0;
  waitpid(p, &st, 0);
  printf("%s %s\n", WIFEXITED(st) && !WEXITSTATUS(st) ? "ok  " : "FAIL", name);
  if (!WIFEXITED(st) || WEXITSTATUS(st)) fails++;
}

static void loads(const char *arg) { (void)arg, case_loads(); }

int main(int argc, char **argv) {
  (void)argc;
  if (!realpath(argv[0], dir) || !strrchr(dir, '/')) return printf("FAIL no folder\n"), 1;
  *strrchr(dir, '/') = 0;
  snprintf(so, sizeof so, "%s/libdashble.so", dir);
  run("bus absent: not loaded; then loaded and delegated", "test_ble_dl_fake.so", 0, loads, NULL);
  run("no plugin: said once", NULL, 1, case_fails, "libdashble.so");
  run("plugin without ble_state: said once", "test_ble_dl_bad.so", 1, case_fails, "ble_state");
  unlink(so), unlink(BLE_DL_BUS);
  printf(fails ? "test_ble_dl: FAILED\n" : "test_ble_dl: all passed\n");
  return fails ? 1 : 0;
}
#endif
