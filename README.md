# Wattline — embedded T113 head unit

- `os/`  — Buildroot firmware (kernel, packages, overlays; R0–R4)
- `app/` — LVGL head-unit application (dashboard UI, wireless Android Auto receiver)

See the [latest release](https://github.com/cchvuth/wattline-embedded-t113/releases) to download a card image and burn it (`sdcard.img.xz`). The stock eMMC is never touched — pull the card and the stock firmware boots.

## Android Auto certificate

Wireless Android Auto requires a **Google-signed head-unit certificate**: the phone pins Google's Android Auto CA, so a self-signed certificate is rejected at the TLS handshake ("communication error 7"). Supply your own `aa.crt` / `aa.key` (PEM).

Place them on the Dash at — and they persist across reboot **and** reflash (`/data` is a separate partition):

```
/data/wattline/aa.crt    # PEM certificate (leaf; append the CA chain if the phone wants it)
/data/wattline/aa.key    # PEM private key   (chmod 600)
```

How to put the files there:

- **Over SSH** (enable SSH in **Settings → Advanced**, pair a key):
  ```
  scp aa.crt aa.key root@<dash-ip>:/data/wattline/
  ```
- **Or** power off, take the card out, and mount its data partition (ext4, label `DASHDATA2`) on a computer, then copy the files into `wattline/`.

If no cert is present the Dash generates a self-signed placeholder so the on-screen **Android Auto** button still appears; a real certificate replaces it. The button is in the dashboard's bottom-right — tapping it starts Android Auto.

## How to build

### Dash app (`app/`)

1. The signal tables are generated from the Flutter app ([cchvuth/wattline](https://github.com/cchvuth/wattline)): clone it as `tesla-dash/` in the repo root (next to `app/`, as CI does), then `make -C app -f app.mk build/signals_gen.h`.
2. LVGL is vendored but gitignored: `git clone --depth 1 -b v9.6.0 https://github.com/lvgl/lvgl app/third_party/lvgl-9.6.0`.
3. Host: `mkdir -p app/build && make -C app test` (unit tests) and `make -C app -f app.mk host` (the UI on the desktop).
4. ARM, with the firmware's Buildroot toolchain: `make -C app -f app.mk ARM_CC=<buildroot>/host/bin/arm-buildroot-linux-gnueabihf-gcc CMDR=1 arm` → `app/build/arm-app-cmdr/dash` (and `dash.stripped`). Use the toolchain of the tree that built it: a copied/relocated Buildroot host tree fails to link libc, even with `--sysroot`.

Icons and pictures (`app/ui/gen/`) are rendered from the Flutter app: `APP=<flutter app checkout> app/ui/assets/export.sh` (needs `flutter` and python3 with Pillow).

Android Auto receiver self-test (no phone needed; TLS-in-frames, messages, split-frame reassembly, the Bluetooth handshake):

```
cc -O2 -Wall -DAA_BT $(pkg-config --cflags gio-2.0 gio-unix-2.0) -o aa app/aa/aa.c app/core/pb.c \
   -lssl -lcrypto $(pkg-config --libs gio-2.0 gio-unix-2.0) && ./aa --selftest
```

### Firmware image (`os/`)

Buildroot 2026.02 in a Docker container (`debian:stable`), with a persistent volume for the Buildroot tree and its output. Container packages: `build-essential make rsync pkg-config file bc cpio python3 git wget unzip` (plus `libssl-dev libglib2.0-dev` for host builds of the receiver).

Mounts the build expects: `os/` at `/ext` (the `BR2_EXTERNAL` tree), `app/` at `/native` (the dash program and the Android Auto receiver are built from it; generate `app/build/signals_gen.h` first), and the RTL8733BS Wi-Fi driver source at `/radio` (Realtek's vendor driver, GPL-2.0, kept outside this repository: supply it separately). Because the image needs that tree, CI builds and tests the app and the firmware's tools, but not the image.

```
make O=<output dir> BR2_EXTERNAL=/ext dash_r4_defconfig
make O=<output dir>
```

Rebuild single packages in an existing output tree: `make BR2_EXTERNAL=/ext wattline-aa-rebuild spike-vdec-rebuild` (run in the output dir).

### Updating a running Dash

The root filesystem is an initramfs: it runs from RAM and is read back from the card at every boot, so a file copied onto a running Dash is gone after a power cycle. To make a change stay:

- **Replace the image**: put a new `rootfs.cpio.uboot` on the card's boot partition (p1, FAT), keeping the old one as `rootfs.cpio.uboot.prev` to roll back.
- **Or use the overlay** (images from v0.2.0 on): put the file under `/data/overlay/root/` at its path on the Dash (e.g. `/data/overlay/root/usr/bin/aa`) and write the image's `/etc/image-id` to `/data/overlay/image-id`. `S05overlay` copies the overlay over the root at boot, and drops an overlay made for a different image, so it never shadows newer firmware.
