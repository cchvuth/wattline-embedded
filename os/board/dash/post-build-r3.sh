#!/bin/sh
# R3: no Bluetooth firmware in the image (S10bt-firmware cuts it on the device from its own original software), and
# no SSH keys from any overlay.
set -e
rm -rf "$TARGET_DIR/root/.ssh" "$TARGET_DIR/lib/firmware/rtl_bt"
