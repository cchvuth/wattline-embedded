# Device profile: the SRNUBI T313D screen (Allwinner F133, C906). A 720x1920 MIPI-DSI panel shown in landscape
# (dash.conf rotate = 270): a 1920x720 canvas, drawn with the phone app's layouts (full, basic, split) and their icons
# at 1.875 px/dp (ui/gen/f133, from ui/assets/sizes-f133.json). What a profile holds: docs/DEVICES.md.
# Toolchain: the F133 image's Buildroot (os-f133/build.sh; its dashapp package passes its own ARM_CC)
ARM_CC ?= /work/out/host/bin/riscv64-buildroot-linux-gnu-gcc
# The C906's own: its compressed instructions (the image's toolchain defaults to rv64imafd) and T-Head scalar extensions
# (indexed and paired loads, th.extu, ...): rendering ~15% faster on the C906 (Settings scroll: 29 -> 25 ms a frame)
DEVICE_CPU := -mcpu=thead-c906
# The Commander's Bluetooth session (GDBus to BlueZ, the AIC8800's own Bluetooth): the buttons' actions need it, as on
# the T113 (Bluetooth stays linked to the Commander beside Android Auto, whose Bluetooth is the power MCU's)
CMDR ?= 1
# Its session built as a plugin (libdashble.so, installed beside the dash) that the dash loads on a thread once the system bus
# is there, not linked in: GLib and GIO (~4 MB) read and relocated before main() were 0.9 s of the 1.9 s from the
# dash's start to its first frame at boot, for a session that cannot start before dbus-daemon, seconds later
BLE_DL := 1
# The phone app's layouts; the canvas and the Android Auto picture (project_region.h: the left half's lower 3/4, as
# the receiver's build has it); each frame from the panel's vertical blank, rotated into the framebuffer by the G2D's
# rotation unit (the OS's g2drot module), else by the CPU in tiles (DASH_FB_ROT: port/linux/fb_rot.c; LVGL's own rotate
# took ~70 ms a scrolled frame on the C906); draw buffers of 120 panel rows (DASH_FB_ROWS: 2 x 338 KiB of the module's
# CMA with the unit, else LVGL's one); no drive recordings (DASH_NO_RECORD: no room in the 512 KiB data partition);
# software updates only with the card install (dash-update says so from the NOR); a restart is the power MCU's power
# cycle (S03mcu: a warm reboot loses the Wi-Fi chip).
DEVICE_DEFS := -DDASH_PHONE_LAYOUT=1 -DPJ_PANEL_H=720 -DPJ_RGN_X=0 -DPJ_RGN_Y=180 -DPJ_RGN_W=960 -DPJ_RGN_H=540 \
  -DDASH_VSYNC=1 -DDASH_FB_ROT=1 -DDASH_FB_ROWS=120 -DDASH_NO_RECORD=1 \
  -DDASH_REBOOT='"/etc/init.d/S03mcu"' -DDASH_REBOOT_ARG='"power-cycle"' \
  -DDASH_RESTART_NOTE='"The screen goes dark for about a minute (its power cut and restored), then starts again; settings and pairings stay"'
UI_GEN := ui/gen/f133
BUILD := build/f133
