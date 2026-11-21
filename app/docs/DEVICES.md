# Devices

One dash program serves every head unit: the loop, the link, Bluetooth, the settings, the Android Auto control and
what the screen shows from the car's data are the same code everywhere. What differs per device lives in two places
only:

- its **profile**, `device/<name>.mk`, read by `app.mk` and `ui.mk` at build time (`DEVICE=<name>`, `t113` by
  default);
- its **dash.conf**, shipped by the device's OS layer, read at start.

A difference neither can express becomes a profile define or a dash.conf key, never a copy of app logic.

## What a profile holds

| | T113: the S3XY Dash (`device/t113.mk`) | F133: the SRNUBI T313D screen (`device/f133.mk`) |
|---|---|---|
| Toolchain, CPU flags (`ARM_CC`, `DEVICE_CPU`) | the T113 image's Buildroot ARM gcc (`os/`), Cortex-A7 NEON | the F133 image's Buildroot RISC-V gcc (`os-f133/`), the C906's own instructions |
| Canvas (`PJ_PANEL_W` x `PJ_PANEL_H`) | 1920x480, under a housing that hides the top corners | 1920x720 (the 720x1920 panel turned) |
| Layout set (`DASH_PHONE_LAYOUT`) | the Dash's strip and its split | the phone app's: full, basic (tap the gear; two cards a side, swipe up or down; the ring to the speedometer), split (the buttons over the picture; the ring and a card beside it, swiped likewise) |
| Android Auto picture (`PJ_RGN_X/Y/W/H`, `ui/project_region.h`) | x 120, y 0, 1160x480 | x 0, y 180, 960x540 |
| Icon set (`UI_GEN`) | `ui/gen`, from `ui/assets/sizes.json` at 1.25 px/dp | `ui/gen/f133`, from `ui/assets/sizes-f133.json` at 1.875 px/dp |
| Vertical blank wait (`DASH_VSYNC`) | off | on (not after a frame whose flushes ran past half a scan) |
| Flush (`DASH_FB_ROT`, `DASH_FB_ROWS`) | LVGL's fbdev driver (its rotation), 240-row buffer | `port/linux/fb_rot.c`: the G2D's rotation unit when the OS has `/dev/g2drot`, else the CPU in tiles; 120-row buffers (two from the module's CMA with the unit) |
| Restart (`DASH_REBOOT`, `DASH_REBOOT_ARG`) | `/sbin/reboot` | `/etc/init.d/S03mcu power-cycle` (a warm reboot loses its Wi-Fi chip) |
| Build outputs (`BUILD`) | `build/` | `build/f133/` |

The fonts (`ui/gen/roboto.c`) are shared. The defines reach every build of that device, the host's too, so the
renders and the bench test draw what the device draws. The Android Auto receiver must use the same `PJ_*` values.

## Building

T113, as before:

- the device: `tools/arm-app.sh [CMDR=1]` → `build/arm-app[-cmdr]/dash.stripped`
- on the host: `make test`, `make -f cmdr.mk test`, `make -f ble.mk test`, `make -f app.mk test [CMDR=1]`,
  `make -f ui.mk shots` (`build/shots`, 1920x480)

F133:

- the device: the F133 image builds it (`os-f133/build.sh`, its `dashapp` package). By hand, with that image's
  toolchain (`build/signals_gen.h` made first: `make -f app.mk build/signals_gen.h`):

  ```
  make -f app.mk -j8 DEVICE=f133 ARM_CC=<buildroot>/host/bin/riscv64-buildroot-linux-gnu-gcc CMDR=1 arm
  ```

  → `build/f133/arm-app-cmdr/dash.stripped` (`CMDR=0`: `build/f133/arm-app/`)
- on the host: `make -f ui.mk shots DEVICE=f133` (`build/f133/shots`, 1920x720), `make -f app.mk test DEVICE=f133`

The icons, when the app's glyphs, car pictures or lights change: `ui/assets/export.sh` and `ui/assets/aa_icon.py`
for the T113; for the F133 the same with `PX=1.875 SIZES=ui/assets/sizes-f133.json GEN=ui/gen/f133` set.

## dash.conf keys that differ per device

The built-in defaults (`app/conf.c`) are the T113's. The F133's image ships its own dash.conf.

| key | T113 | F133 |
|---|---|---|
| `fb`, `touch` | `/dev/fb0`, `/dev/input/event0` | the same |
| `rotate` | 270 | 270 |
| `touch_swap` | 1 | 0 |
| `touch_cal` | 479, 0, 0, 1919 | 0, 0, 719, 1919 (the panel's own portrait coordinates) |
| `frame_ms` | 33 | 16 (the panel's 58.5 Hz) |
| `backlight` | `/sys/class/backlight/backlight` | the same (PWM) |
| `watchdog` | 1 | 0 (a reset loses the Wi-Fi chip until power off and on) |
| `log` | `/data/log/dash.log` when `/data` is writable, else `/run/dash.log` | `/run/dash.log` |
| `panda_host` | 192.168.4.1 (the Commander's Panda on S3XY_OBD) | the same |

## Below the app

Each OS layer provides the same interfaces, so the app calls them alike: `/usr/sbin/dash-aa start|stop|restore|forget`,
`/etc/init.d/S45wifi-auto`, `dash-ssh-enable` / `dash-ssh-disable`, `dash-update`, and the `/run` markers in
`ui/project_region.h` (`/run/dash-aa-radio` while Android Auto holds the radio: the WiFi link waits, unless live).
How each does it (BlueZ RFCOMM and the RTL8733BS on the T113; the power MCU's UART and the AIC8800 on the F133) stays
in that layer. The F133's may add `/dev/g2drot` (its g2drot module: the G2D's rotation unit for the flush); the dash
runs without it.
