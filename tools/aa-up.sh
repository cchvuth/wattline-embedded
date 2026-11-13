#!/bin/sh
# aa-up.sh — bring up wireless Android Auto on the Dash for a live test.
# Pair the phone once beforehand (bluetoothctl), then run this: it launches the
# 5 GHz AP, registers the RFCOMM profile that hands the phone the AP creds, and
# starts the receiver (TCP 5288 + TLS + video -> the decode/blit sink).
#   SSID/PSK/IP overridable via env.  Ctrl-C tears it all down.
set -e
SSID=${SSID:-WattlineAA}
PSK=${PSK:-wattline-aa}
IP=${IP:-192.168.53.1}

# AP-capable iface: wlan1 if the concurrent driver made one, else wlan0
IFACE=wlan1; ip link show wlan1 >/dev/null 2>&1 || IFACE=wlan0
BSSID=$(cat /sys/class/net/"$IFACE"/address)
echo "aa-up: AP on $IFACE ($BSSID) ssid=$SSID ip=$IP"

cleanup() { kill "$BT_PID" 2>/dev/null; killall hostapd udhcpd 2>/dev/null; }
trap cleanup EXIT INT TERM

aa ap "$IFACE" "$SSID" "$PSK" "$IP"

# let the phone find us for pairing / the AA SDP record
bluetoothctl -- discoverable on >/dev/null 2>&1 || true
bluetoothctl -- pairable on   >/dev/null 2>&1 || true

aa bt "$IP" "$SSID" "$PSK" "$BSSID" &
BT_PID=$!

aa serve   # auto self-signed cert at /etc/wattline/ on first run; blocks on 5288
