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

No certificate ships with the Dash, and none is generated: the on-screen **Android Auto** button (bottom-right of the dashboard) appears only once a Google-signed certificate is in place. A certificate that cannot work is set aside at boot as `aa.crt.unusable` / `aa.key.unusable`: a self-signed one (phones reject it), or one whose `aa.key` is missing or belongs to another certificate. `aa certcheck /data/wattline/aa.crt /data/wattline/aa.key` says which; once fixed, rename the files back. Tapping the button starts Android Auto.

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

Mounts the build expects: `os/` at `/ext` (the `BR2_EXTERNAL` tree), `app/` at `/native` (the dash program and the Android Auto receiver are built from it; generate `app/build/signals_gen.h` first), and `os/radio/` at `/radio` (the RTL8733BS Wi-Fi driver source, Realtek's vendor driver, GPL-2.0: see [os/radio/README.md](os/radio/README.md)). CI builds and tests the app and the firmware's tools, not the image (a full Buildroot build).

```
make O=<output dir> BR2_EXTERNAL=/ext dash_r4_defconfig
make O=<output dir>
```

Rebuild single packages in an existing output tree: `make BR2_EXTERNAL=/ext wattline-aa-rebuild spike-vdec-rebuild` (run in the output dir).

### Updating a running Dash

The root filesystem is an initramfs: it runs from RAM and is read back from the card at every boot, so a file copied onto a running Dash is gone after a power cycle. To make a change stay:

- **Replace the image**: put a new `rootfs.cpio.uboot` on the card's boot partition (p1, FAT), keeping the old one as `rootfs.cpio.uboot.prev` to roll back.
- **Or use the overlay** (images from v0.2.0 on): put the file under `/data/overlay/root/` at its path on the Dash (e.g. `/data/overlay/root/usr/bin/aa`) and write the image's `/etc/image-id` to `/data/overlay/image-id`. `S05overlay` copies the overlay over the root at boot, and drops an overlay made for a different image, so it never shadows newer firmware.

### Over-the-air updates

Cards burned from v0.3.0 on update themselves from this repo's releases (`dash-update`; Settings → Advanced → Software update, with an automatic option). The factory image on the boot partition is never rewritten: an update is unpacked into a slot on `/data` (`/data/boot/A` or `B`) and starts at the next power-up, on trial. A version that does not run healthy for 30 s within 3 starts is rolled back by U-Boot to the last good one, and automatic updates skip it from then on. The Dash only looks for updates with internet access (never on the car's adapter network), and installs nothing that is not signed with the project's key.

To publish an update, build the image, then make the release's update assets and upload everything in the folder to the release:

```
os/board/dash/mk-ota.sh <output dir>/images <private key> <release folder>
```

This writes `wattline-ota-<version>.tar.gz` (the version comes from the image's own `/etc/image-id`), then `SHA256SUMS` over every file in the folder (put `sdcard.img.xz` there first) and `SHA256SUMS.sig` (Ed25519). The private key is never in the repo or a release; the image carries only its public half (`os/board/dash/wattline-ota.pub.raw`). Tests, each run in a scratch container (see their headers): `os/board/dash/test/dash-update-test.sh` (the updater against a local HTTPS release server) and `os/board/dash/test/dash-scr-test.sh` (the boot script in U-Boot's sandbox).
