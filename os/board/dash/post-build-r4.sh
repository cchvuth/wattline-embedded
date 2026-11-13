#!/bin/sh
# R4 spike: the H.264 test clips and their software-decode CRCs (made by br-r4.sh in $BASE_DIR/spike-media).
set -e
M="$BASE_DIR/spike-media"
ls "$M"/vdec-*.h264 "$M"/vdec-*.crc >/dev/null
mkdir -p "$TARGET_DIR/usr/share/spike"
cp "$M"/vdec-*.h264 "$M"/vdec-*.crc "$TARGET_DIR/usr/share/spike/"
# hostapd ships an example /etc/hostapd.conf; the spike writes its own AP config at runtime, so drop the stock one
rm -f "$TARGET_DIR/etc/hostapd.conf"
