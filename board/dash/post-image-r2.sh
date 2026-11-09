#!/bin/sh
# R2: as R1, the card keeps its layout; U-Boot loads the DTB under the name dash-r0.dtb.
set -e
cp "$BINARIES_DIR/sun8i-t113s-dash-r2.dtb" "$BINARIES_DIR/dash-r0.dtb"
