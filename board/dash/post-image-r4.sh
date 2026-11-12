#!/bin/sh
# R4: U-Boot (unchanged on the card) loads the DTB under the name dash-r0.dtb: the R4 DT goes there.
set -e
cp "$BINARIES_DIR/sun8i-t113s-dash-r4.dtb" "$BINARIES_DIR/dash-r0.dtb"
