# Device profile: the S3XY Dash (Allwinner T113-S4, Cortex-A7). Its 1920x480 strip sits under a housing that hides
# the top corners: the Dash's own layout and icon set (ui/gen, from ui/assets/sizes.json at 1.25 px/dp).
# The default profile (DEVICE=t113). What a profile holds: docs/DEVICES.md.
# Toolchain: the T113 image's Buildroot (os/; tools/arm-app.sh runs it in Docker)
ARM_CC ?= /work/out-r0/host/bin/arm-buildroot-linux-gnueabihf-gcc
DEVICE_CPU := -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard
# LVGL's draw buffer: 240 rows of the 480-wide portrait panel (DASH_FB_ROWS; LVGL's default 60, 113 KiB): strips of 60
# canvas rows, not 15, so a frame takes a quarter of the passes over the widget tree, and the rotating flush copies a
# quarter as many runs (host count: a whole-dash frame 26 -> 15 M instructions, a Settings scroll 4.6 -> 3.1 M) for
# 338 KiB more of LVGL's heap (and its rotation buffer, as big)
DEVICE_DEFS := -DDASH_FB_ROWS=240
UI_GEN := ui/gen
BUILD := build
