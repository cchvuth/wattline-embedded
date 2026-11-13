// app/wifi.c's mapping: wlan0 and wpa_supplicant's state -> the WiFi button's OFF / SCAN / JOIN, the link's once joined.
#include "../app/app.h"
#include <stdio.h>
#include <string.h>

static int pass, fail;
#define CHECK(c) do { if (c) pass++; else { fail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

int main(void) {
  wifi_track_t t = {0};
  const char *why = "";
  CHECK(wifi_join_of(&t, false, NULL, false, NULL, &why) == WIFI_OFF && strstr(why, "off"));
  CHECK(wifi_join_of(&t, true, "dormant", false, NULL, &why) == WIFI_JOINING);           // no wpa_supplicant answer
  CHECK(wifi_join_of(&t, true, "down", false, "SCANNING", &why) == WIFI_SCANNING && strstr(why, "searching"));
  CHECK(wifi_join_of(&t, true, "dormant", false, "ASSOCIATING", &why) == WIFI_JOINING && strstr(why, "joining"));
  CHECK(wifi_join_of(&t, true, "up", false, "COMPLETED", &why) == WIFI_JOINING && strstr(why, "address"));   // DHCP
  CHECK(wifi_join_of(&t, true, "up", true, "COMPLETED", &why) == -1);                    // joined: the link says
  CHECK(wifi_join_of(&t, true, "up", true, NULL, &why) == -1);
  // a wrong passphrase: the handshake falls back, twice
  CHECK(wifi_join_of(&t, true, "dormant", false, "4WAY_HANDSHAKE", &why) == WIFI_JOINING);
  CHECK(wifi_join_of(&t, true, "down", false, "DISCONNECTED", &why) == WIFI_SCANNING && !strstr(why, "password"));
  wifi_join_of(&t, true, "dormant", false, "4WAY_HANDSHAKE", &why);
  CHECK(wifi_join_of(&t, true, "down", false, "SCANNING", &why) == WIFI_SCANNING && strstr(why, "wrong password?"));
  CHECK(wifi_join_of(&t, true, "dormant", false, "ASSOCIATING", &why) == WIFI_JOINING && strstr(why, "wrong password?"));
  CHECK(wifi_join_of(&t, true, "up", true, "COMPLETED", &why) == -1 && t.fails == 0);   // joined after all: forgotten
  CHECK(wifi_join_of(&t, true, "down", false, "SCANNING", &why) == WIFI_SCANNING && !strstr(why, "password"));
  // SCAN_RESULTS -> distinct names, strongest first; escapes decoded; hidden (empty) left out
  char nb[6][33];
  const char *sr = "bssid / frequency / signal level / flags / ssid\n"
                   "aa:aa:aa:aa:aa:01\t5580\t-71\t[WPA2-PSK-CCMP][ESS]\tHome Net\n"
                   "aa:aa:aa:aa:aa:02\t2412\t-40\t[WPA2-PSK-CCMP][ESS]\tS3XY_OBD\n"
                   "aa:aa:aa:aa:aa:03\t5180\t-55\t[WPA2-PSK-CCMP][ESS]\tHome Net\n"
                   "aa:aa:aa:aa:aa:04\t2437\t-30\t[ESS]\t\n"
                   "aa:aa:aa:aa:aa:05\t2462\t-80\t[WPA2-PSK-CCMP][ESS]\tAP \\xe2\\x9c\\x93 \\\"Net\\\"";
  int nn = wifi_scan_parse(sr, nb, 6);
  CHECK(nn == 3);
  CHECK(nn == 3 && !strcmp(nb[0], "S3XY_OBD") && !strcmp(nb[1], "Home Net"));   // Home Net at its best, -55
  CHECK(nn == 3 && !strcmp(nb[2], "AP \xe2\x9c\x93 \"Net\""));
  CHECK(wifi_scan_parse(sr, nb, 1) == 1 && !strcmp(nb[0], "S3XY_OBD"));
  CHECK(wifi_scan_parse("OK\n", nb, 6) == 0 && wifi_scan_parse(NULL, nb, 6) == 0);
  printf("test_wifi: %d/%d checks pass\n", pass, pass + fail);
  return fail != 0;
}
