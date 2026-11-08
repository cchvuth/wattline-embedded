#!/bin/sh
# R1: the card keeps its layout; only zImage, the DTB (under the name U-Boot loads: dash-r0.dtb) and the rootfs change.
set -e
cp "$BINARIES_DIR/sun8i-t113s-dash-r1.dtb" "$BINARIES_DIR/dash-r0.dtb"
