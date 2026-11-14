#!/bin/sh
# R4: name the DTB as U-Boot loads it (dash-r0.dtb), then build the burnable release card image (genimage-sd-release.cfg:
# SPL+U-Boot, p1 boot, p2 DASHDATA small; /data is carved on first boot by dash-mkdata).
set -e
cp "$BINARIES_DIR/sun8i-t113s-dash-r4.dtb" "$BINARIES_DIR/dash-r0.dtb"
echo "Wattline T113 — data partition (settings, AA cert); /data (p3) is carved here on first boot." > "$BINARIES_DIR/DASHDATA.txt"
exec support/scripts/genimage.sh -c "$BR2_EXTERNAL_DASH_PATH/board/dash/genimage-sd-release.cfg"
