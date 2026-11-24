# Wattline on the SRNUBI T313D (Allwinner F133)

Linux and the Wattline dash for the SRNUBI T313D, an 11.26" aftermarket car screen:

- **SoC:** Allwinner F133 (D1s): one RISC-V C906 core, 64 MiB of DDR2.
- **Panel:** 720x1920 MIPI DSI, shown as a 1920x720 landscape canvas, with the panel's touch controller.
- **Storage:** an SD card, which the screen always starts from (its 16 MiB SPI NOR flash is not used).
- **Radios:** an AIC8800D80 for Wi-Fi and Bluetooth LE (SDIO), and a Bluetrum MCU that holds the power and is also the
  classic Bluetooth.

The dash is the same program as the T113 Dash's ([app/](../app), device profile `app/device/f133.mk`). It gets the
car's data from the Commander over Wi-Fi or Bluetooth LE, and runs wireless Android Auto with your own Google-signed
head-unit certificate (see the top README). Android Auto's Bluetooth is the MCU's: a phone paired for it is dropped
while Android Auto is off (it would retry Android Auto for as long as it stays connected) and called back when
Android Auto starts. Over-the-air updates come from this repository's releases (below).

The certificate goes to `/data/wattline/` as on the Dash. The image's SSH server has no SFTP, so copy the files with
`scp -O` or `ssh root@<screen> 'cat > /data/wattline/aa.crt' < aa.crt` (and the same for `aa.key`).

## Build

Docker and about 15 GB of disk. In the repository root:

1. LVGL, vendored but not committed: `git clone --depth 1 -b v9.6.0 https://github.com/lvgl/lvgl app/third_party/lvgl-9.6.0`.
2. `os-f133/build.sh`

The first run builds the toolchain too, which takes about an hour. The Buildroot tree and its output stay in the
Docker volume `wattline-f133`, and the images land in `out-f133/`:

| File | What it is |
|---|---|
| `wattline-f133-sdcard-<version>.img.xz` | the whole card, to flash with any imager (`board/f133/genimage.cfg`) |
| `u-boot-sunxi-with-spl.bin` | the SPL, OpenSBI and U-Boot, 8 KiB into the card |
| `Image`, `f133.dtb`, `rootfs.sqfs` | the kernel, its device tree and the root, on the card's first partition (and an update's bundle) |

A whole build (`os-f133/build.sh` with no arguments) compiles the programs from this checkout again (the dash, the
receiver, the decoder, `mcud`), the kernel with its out-of-tree modules when its device tree, configuration
(`board/f133/linux.fragment`), initramfs or patches changed, and U-Boot when its patches or `uboot.fragment` changed:
Buildroot would otherwise keep what it built first. To rebuild one package by hand, run e.g.
`os-f133/build.sh dashapp-rebuild`; after a kernel change by hand, the modules too
(`aic8800-dirclean g2drot-dirclean linux-dirclean`): modules built for another kernel load, but Wi-Fi stops working.

## The card

| Part | Where | What |
|---|---|---|
| U-Boot | 8 KiB (raw) | the SPL, OpenSBI and U-Boot; at 2 MiB the area the SPL saves its log to when a start fails |
| p1, FAT32 | 4 MiB, 4 GiB | `Image`, `f133.dtb`, `rootfs.sqfs`; an update's `.try` files and download (`ota/`); your own files (below) |
| p2, ext4 | the rest | `/data`: settings, Wi-Fi networks, SSH keys, the AA certificate, logs, drive recordings |

The image holds U-Boot and p1. At the first start `dash-mkdata` adds p2 over the rest of the card (any card of
8 GB or more; 256 MiB at least after p1), and every start checks it (`S01data`: `e2fsck -p`, journaled, so a power cut
loses at most the last seconds). p1's FAT is checked and repaired before it is mounted at every start (`/init`,
dosfstools' `fsck.fat -a`; about 0.6 s), as the power is always just cut and the kernel sets a damaged FAT read-only.
A card flashed again keeps its `/data`: the image rewrites only U-Boot and p1, and the first start finds the earlier
p2 where it was. Without a p2 (one that could not be made, or one that is not a Linux partition, which is never
touched) `/data` stays in RAM.

## Install

1. **Flash the card** with `wattline-f133-sdcard-<version>.img.xz` (balenaEtcher, or
   `xzcat wattline-f133-sdcard-<version>.img.xz | dd of=/dev/sdX bs=4M`).
2. **Optional, on p1 (`WATTLINE`)**: a `wpa_supplicant.conf` with your network and your SSH public key as
   `authorized_keys`, for SSH before the dash's own settings have them; `splash.bgra.gz`, your own boot picture (the
   panel's 720x1920 framebuffer, XRGB8888, gzipped); `dash/dash` with its `dash.conf`, a build of the dash to try in
   place of the image's.
3. **Put the card in and power on.** The boot ROM tries the card first; the screen's flash is never written.

A screen that ran from its flash with an earlier release starts from the card the same way: flash a card and put it
in. Its flash's `/data` is not read any more, so its settings come from the dash again. New releases do not start
from the flash, nor update a screen that does.

With **Save logs** on (Settings, Advanced) each start's logs go to `/data/log/<start>/` (the 30 latest kept); off,
the default, the screen keeps no logs at all, on the card or in RAM (each log a link to `/dev/null`: `nolog`), from
the next start.

## Over-the-air updates

The screen updates itself from this repository's releases: the dash's **Settings → Advanced → Software update**, or
on its own with **Automatic updates** (internet only, never on the car's Wi-Fi: while the car's adapter is in range
the screen stays on its network, so updates happen where it is not, or from Settings after picking another network).
`dash-update` downloads a release's F133 bundle onto the card, checks its signature (the project's Ed25519 key,
`board/f133/wattline-ota.pub.raw`) and puts its files there as `.try` files. Nothing restarts: at the next power-up
U-Boot tries the new kernel once, with its new root (`/init` mounts that only on the tried boot), and `S98try` keeps
them once the dash is on screen and the Wi-Fi works (the screen sees networks, or Android Auto's access point is up),
with SSH listening if it is on with a key. A version that does not come up within 90 s is given up: the screen
power-cycles into the old one, and automatic updates skip that version. A start that ends sooner (the power gone in
the first minute and a half) is tried again at the next power-up, three starts at most. An update never writes
U-Boot.

`board/f133/mk-ota.sh` makes a release's bundle; each release carries the T113 Dash's bundle and both devices' card
images as well (see the top README).

Over SSH, a root of your own goes on p1 as `rootfs.sqfs.new`: `/init` puts it in place at the next start if it is a
whole squashfs.

## Where a start's time goes

Read off the screen itself: `/run/boot.times` has the moment each boot script starts (seconds since the kernel did; the
dash is on the screen when `S05wifi` starts), and `dmesg | grep init:` the first-stage init's steps.

## Notes on the hardware

- **Power.** The MCU cuts the power about 30 s after power-on unless the SoC holds it: U-Boot sends the hold, then
  `mcud` repeats it every minute (`S03mcu`). Releasing the hold makes the MCU power-cycle the screen.
- **Wi-Fi after a reset.** The Wi-Fi chip does not come back after a warm reset. A boot that finds no Wi-Fi chip has
  the MCU cycle the power, at most three times in a row.
- **Display.** The dash draws in strips and the 2D engine's rotation unit turns them into the portrait framebuffer
  (`package/g2drot`). Without the module, the CPU rotates them.
- **Clocks.** The SPL sets the CPU to 1296 MHz (the data sheet's rate is 1008) and the memory to 696 MHz (the stock
  firmware's is 528): measured on one board, with the reasons and the limits found, in the device tree
  (`board/f133/sun20i-d1s-srnubi-t313d.dts`) and U-Boot patches 0001 and 0002. Only on the first start since the power
  came on. A start after a reset (the watchdog's, a panic's, a `reboot`) runs at the stock clocks: the SPL leaves the
  CPU's and sets the memory to 528 MHz, and U-Boot takes the 1296 MHz operating
  point out of the kernel's device tree; the power cycle such a start ends in (S03mcu: the Wi-Fi chip is lost with a
  reset) brings the higher clocks back. The memory is also at 528 MHz if it does not come up whole at 696. The
  watchdog runs from the SPL's first lines, through U-Boot and the kernel (16 s; the kernel feeds it): a hang at these
  clocks resets the board. So a board that cannot run them hangs or crashes, starts again at the stock clocks, is
  power-cycled and tries again. Failing within its first minute each time, it stays up at the stock clocks after three
  such rounds, without Wi-Fi, and does the same after every later power-on. Failing later (Wi-Fi up a minute in clears
  S03mcu's count), it goes round for good: some minutes up, a minute and a half down. And nothing catches a clock that
  is only nearly right: wrong data without a hang or a crash. For such a board: lower numbers, three of them (the
  CPU's in U-Boot patch 0001, the same as the device tree's operating point, whose name is also in that patch's
  `ft_board_setup`; the memory's in 0002). With a boot loader older than this one the kernel raises the CPU's clock
  itself as it starts (the PLL given 2 ms to settle: patches/linux 0010), and the memory stays as that boot loader set
  it.
- **Watchdog.** Stopped for the rest of that start once a tried boot is judged (S01try's feeder lets it go), and
  the dash's own (dash.conf `watchdog = 1`, off here) takes it from the kernel when set.
- **Heat.** The SoC's temperature sensor is read by the kernel (`/sys/class/thermal/thermal_zone0/temp`, logged with
  the boot's state): from 80 °C the CPU runs at 1008 MHz (until it is under 75 °C again), at 85 °C it is held at
  408 (on a start after a reset, at the stock clocks: 408 from 80 °C). At 110 °C the system shuts down and halts with
  the backlight off; the MCU, no longer held, then cycles the power, and a start that is still that hot shuts down
  again.
- **Memory.** Android Auto's video decoder uses 16 MiB of CMA for its pictures and the framebuffer. The decoder keeps
  only as many pictures as the stream's references need, plus one.
