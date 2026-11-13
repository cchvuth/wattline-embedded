// Bench tool: the adapter, a scan, and (only with an address AND --connect) the GATT tree of one
// device, read-only. Never writes to a device and never switches notifications on.
//   ble_probe [-s SECS] [--any] [-a /org/bluez/hci0] [ADDR --connect]
// Default: SECS 10, only devices advertising the Commander's service; --any lists every device heard.
#include "../port/linux/ble_bluez.h"
#include <gio/gio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static GDBusConnection *bus;

static GVariant *call(const char *path, const char *iface, const char *method, GVariant *args, const char *ret) {
  GError *e = NULL;
  GVariant *r = g_dbus_connection_call_sync(bus, "org.bluez", path, iface, method, args, ret ? G_VARIANT_TYPE(ret) : NULL,
                                            G_DBUS_CALL_FLAGS_NONE, 10000, NULL, &e);
  if (!r) fprintf(stderr, "%s %s: %s\n", method, path, e->message), g_error_free(e);
  return r;
}

static void adapter(const char *path) {
  GVariant *r = call(path, "org.freedesktop.DBus.Properties", "GetAll", g_variant_new("(s)", "org.bluez.Adapter1"), "(a{sv})");
  if (!r) return;
  GVariant *p = g_variant_get_child_value(r, 0);
  const char *addr = "?", *type = "?", *name = "?";
  gboolean powered = 0, disc = 0;
  g_variant_lookup(p, "Address", "&s", &addr);
  g_variant_lookup(p, "AddressType", "&s", &type);
  g_variant_lookup(p, "Name", "&s", &name);
  g_variant_lookup(p, "Powered", "b", &powered);
  g_variant_lookup(p, "Discovering", "b", &disc);
  printf("adapter %s: %s (%s) name %s powered %s discovering %s\n", path, addr, type, name, powered ? "yes" : "no",
         disc ? "yes" : "no");
  g_variant_unref(p);
  g_variant_unref(r);
}

static const char *bytes_text(GVariant *v, char *s, size_t cap) {
  gsize n;
  const uint8_t *d = g_variant_get_fixed_array(v, &n, 1);
  size_t k = 0;
  for (gsize i = 0; i < n && k + 1 < cap; i++) if (d[i] >= 0x20 && d[i] < 0x7f) s[k++] = (char)d[i];
  s[k] = 0;
  return s;
}

// Services, characteristics (flags, MTU), descriptors (0x2901 text read) under the device, in path order.
static void tree(const char *dev) {
  GVariant *r = call("/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects", NULL, "(a{oa{sa{sv}}})");
  if (!r) return;
  GVariant *all = g_variant_get_child_value(r, 0);
  size_t n = g_variant_n_children(all), dn = strlen(dev);
  const char **paths = g_new0(const char *, n + 1);
  for (size_t i = 0; i < n; i++) g_variant_get_child(all, i, "{&o@a{sa{sv}}}", &paths[i], NULL);
  for (size_t i = 0; i < n; i++) {
    if (strncmp(paths[i], dev, dn) || paths[i][dn] != '/') continue;
    GVariant *ifs, *p;
    g_variant_get_child(all, i, "{&o@a{sa{sv}}}", NULL, &ifs);
    const char *uuid = "?";
    if ((p = g_variant_lookup_value(ifs, "org.bluez.GattService1", G_VARIANT_TYPE("a{sv}")))) {
      g_variant_lookup(p, "UUID", "&s", &uuid);
      printf("  service %s%s\n", uuid, !strcasecmp(uuid, CMDR_SERVICE_UUID) ? "  <- Commander" : "");
    } else if ((p = g_variant_lookup_value(ifs, "org.bluez.GattCharacteristic1", G_VARIANT_TYPE("a{sv}")))) {
      const char **flags = NULL;
      guint16 mtu = 0;
      g_variant_lookup(p, "UUID", "&s", &uuid);
      g_variant_lookup(p, "Flags", "^a&s", &flags);
      g_variant_lookup(p, "MTU", "q", &mtu);
      gchar *f = flags ? g_strjoinv(",", (gchar **)flags) : g_strdup("");
      printf("    char %s [%s] mtu %u\n", uuid, f, mtu);
      g_free(f), g_free(flags);
    } else if ((p = g_variant_lookup_value(ifs, "org.bluez.GattDescriptor1", G_VARIANT_TYPE("a{sv}")))) {
      g_variant_lookup(p, "UUID", "&s", &uuid);
      printf("      desc %s", uuid);
      if (!strcasecmp(uuid, "00002901-0000-1000-8000-00805f9b34fb")) { // a read, never a write
        GVariant *v = call(paths[i], "org.bluez.GattDescriptor1", "ReadValue",
                           g_variant_new("(@a{sv})", g_variant_new_array(G_VARIANT_TYPE("{sv}"), NULL, 0)), "(ay)");
        if (v) {
          GVariant *b = g_variant_get_child_value(v, 0);
          char s[80];
          gsize bn;
          const uint8_t *bd = g_variant_get_fixed_array(b, &bn, 1);
          int k = ble_chr_of(bd, bn);
          printf(" \"%s\"%s", bytes_text(b, s, sizeof s), k < 0 ? "" : "  <- mapped");
          g_variant_unref(b), g_variant_unref(v);
        }
      }
      printf("\n");
    }
    if (p) g_variant_unref(p);
    g_variant_unref(ifs);
  }
  g_free(paths);
  g_variant_unref(all);
  g_variant_unref(r);
}

int main(int argc, char **argv) {
  const char *ad = "/org/bluez/hci0", *addr = NULL;
  int secs = 10, any = 0, conn = 0;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-s") && i + 1 < argc) secs = atoi(argv[++i]);
    else if (!strcmp(argv[i], "-a") && i + 1 < argc) ad = argv[++i];
    else if (!strcmp(argv[i], "--any")) any = 1;
    else if (!strcmp(argv[i], "--connect")) conn = 1;
    else if (argv[i][0] != '-' && strlen(argv[i]) == 17) addr = argv[i];
    else return fprintf(stderr, "usage: %s [-s SECS] [--any] [-a ADAPTER_PATH] [AA:BB:CC:DD:EE:FF --connect]\n", argv[0]), 2;
  }
  if (conn && !addr) return fprintf(stderr, "--connect needs an address\n"), 2;
  cmdr_io io;
  if (ble_bluez_open(&io, ad)) return 1;
  bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, NULL); // the same shared connection
  adapter(ad);

  ble_dev d[128];
  printf("scanning %d s%s\n", secs, any ? ", every device" : " for the Commander's service");
  int n = ble_scan(&io, any ? NULL : CMDR_SERVICE_UUID, secs * 1000, NULL, d, 128);
  if (n < 0) return fprintf(stderr, "scan failed (adapter off? bluetoothctl power on)\n"), 1;
  int shown = 0;
  for (int i = 0; i < n; i++)
    if (any || d[i].match)
      printf("  %s  %4d dBm  %-24s%s\n", d[i].addr, d[i].rssi, d[i].name, d[i].match ? "  Commander service" : ""), shown++;
  printf("%d device(s)%s\n", shown, n > shown ? " (others heard: --any lists them)" : "");

  if (conn) { // the app's own connect, then the tree; no writes, no notifications
    printf("connecting %s\n", addr);
    int has = io.connect(io.ctx, addr);
    if (has < 0) printf("connect failed\n");
    else {
      printf("connected: mtu %d, mapped %s%s%s\n", ble_mtu(&io), has & CMDR_HAS_SESSION ? "prov-session " : "",
             has & CMDR_HAS_API ? "enhapi " : "", has & CMDR_HAS_PUSH ? "push-enhapi" : "");
      char path[96];
      snprintf(path, sizeof path, "%s/dev_%s", ad, addr);
      for (char *c = path + strlen(ad); *c; c++) if (*c == ':') *c = '_';
      tree(path);
    }
    io.disconnect(io.ctx);
    printf("disconnected\n");
  }
  g_object_unref(bus);
  ble_bluez_close(&io);
  return 0;
}
