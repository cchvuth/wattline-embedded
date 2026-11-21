# Wattline on the SRNUBI T313D (Allwinner F133)

Linux and the Wattline dash for the SRNUBI T313D, an 11.26" aftermarket car screen:

- **SoC:** Allwinner F133 (D1s): one RISC-V C906 core, 64 MiB of DDR2.
- **Panel:** 720x1920 MIPI DSI, shown as a 1920x720 landscape canvas, with the panel's touch controller.
- **Storage:** 16 MiB SPI NOR flash.
- **Radios:** an AIC8800D80 for Wi-Fi and Bluetooth LE (SDIO), and a Bluetrum MCU that holds the power and is also the
  classic Bluetooth.

The dash is the same program as the T113 Dash's ([app/](../app), device profile `app/device/f133.mk`). It gets the
car's data from the Commander over Wi-Fi or Bluetooth LE, and runs wireless Android Auto with your own Google-signed
head-unit certificate (see the top README). The screen boots from its NOR in about ten seconds. Over-the-air
updates need the card install (below); a screen running from its NOR is updated over SSH.

The certificate goes to `/data/wattline/` as on the Dash. The image's SSH server has no SFTP, so copy the files with
`scp -O` or `ssh root@<screen> 'cat > /data/wattline/aa.crt' < aa.crt` (and the same for `aa.key`).

## Build

Docker and about 15 GB of disk. In the repository root:

1. LVGL, vendored but not committed: `git clone --depth 1 -b v9.6.0 https://github.com/lvgl/lvgl app/third_party/lvgl-9.6.0`.
2. `make -C app -f app.mk build/signals_gen.h` (needs the Flutter app checked out as `wattline/`; see the top README).
3. `os-f133/build.sh`

The first run builds the toolchain too, which takes about an hour. The Buildroot tree and its output stay in the
Docker volume `wattline-f133`, and the images land in `out-f133/`:

| File | Where it goes |
|---|---|
| `u-boot-sunxi-with-spl.bin` | the NOR's start, or 8 KiB into an SD card |
| `kernel.itb` | the NOR's kernel partition: the kernel and its device tree, checked by U-Boot |
| `rootfs.sqfs` | the NOR's root partition, or the card |
| `Image`, `f133.dtb` | a boot from the card |
| `card/` | a card install's files, ready to copy (below) |

To rebuild one package, run e.g. `os-f133/build.sh dashapp-rebuild`, then `os-f133/build.sh` for the images. After
changing the kernel's configuration (`board/f133/linux.fragment`) or its patches, rebuild the out-of-tree modules
as well (`aic8800-dirclean g2drot-dirclean linux-dirclean`): modules built for another kernel load, but Wi-Fi stops
working.

## NOR layout

| Partition | Offset | Size | Contents |
|---|---|---|---|
| boot | 0 | 1 MiB | SPL, OpenSBI, U-Boot |
| kernel | 0x100000 | 0x440000 | `kernel.itb` |
| rootfs | 0x540000 | 0xa40000 | `rootfs.sqfs` (squashfs, xz) |
| data | 0xf80000 | 0x80000 | `/data` (JFFS2): settings, Wi-Fi networks, SSH keys, the AA certificate |

## First install: from an SD card

The boot ROM tries the SD card first, so a card boots Linux with the screen's own firmware untouched.

1. **Make the card.** Write `u-boot-sunxi-with-spl.bin` 8 KiB into it. Then copy everything in `out-f133/card/`
   onto its fourth partition, formatted FAT, which is where U-Boot and `/init` look: the boot files, and `nor/` with
   the built-in flash's files. Add a `wpa_supplicant.conf` with your network, and your SSH public key as
   `authorized_keys`.
2. **Boot and back up.** Boot the screen with the card in and SSH in. Save the stock firmware first:
   `tools/nor/nor.sh read /mnt/sd/stock.bin`. That command reads the chip twice and checks that both reads agree.
   Copy `stock.bin` off the card and keep it.
3. **Write the NOR.** Put the images in a directory on the card and run `nor.sh flash DIR` (it wants `DIR/stock.bin`
   too, the same as the chip). Then `nor.sh seed`, which copies the card's Wi-Fi settings and SSH keys into the NOR's
   `/data`. Remove the card and power-cycle: the screen now boots from its NOR.

`nor.sh restore stock.bin` from a card boot puts the stock firmware back.

`nor.sh flash` erases the NOR's `/data`. If that holds this system's settings (a screen that ran from its NOR), it is
saved first to `nor-data.bin` beside `stock.bin`, and `nor.sh restore nor-data.bin` puts it back, e.g. after a copy
that should keep the settings, Wi-Fi networks and Bluetooth bonds made while running from the NOR.

**From the dash instead:** a card install can copy itself to the built-in flash from **Settings → Advanced →
Built-in flash** (`dash-nor`). It saves the stock firmware to the card first (`stock.bin`, the chip read twice) if it
is still on the flash. It then writes the root, the kernel and U-Boot (each read back), and copies the card's
settings, Wi-Fi networks and SSH keys into the flash's `/data` (the flash's own, if the screen ran from it, saved to
the card first as `nor-data.bin`). While the card is in, the screen still starts from the card; without it, from the
flash. Its log is `nor.txt` in the boot's log folder on the card.

## Over-the-air updates: the card install

Run from an SD card (the first install's step 1, without writing the NOR), the screen updates itself from this
repository's releases: the dash's **Settings → Advanced → Software update**, or on its own with **Automatic updates**
(internet only, never on the car's Wi-Fi). `dash-update` downloads a release's F133 bundle onto the card, checks its
signature (the project's Ed25519 key, `board/f133/wattline-ota.pub.raw`) and puts its files there as `.try` files.
Nothing restarts: at the next power-up U-Boot tries the new kernel once, with its new root (`/init` mounts that only on
the tried boot), and `S98try` keeps them once the dash is on screen and the Wi-Fi works (the screen sees networks, or
Android Auto's access point is up), with SSH listening if it is on with a key. The update replaces the card's `nor/`
files too, so a later copy to the built-in flash writes the same version. A version that does not come up within 90 s
is given up: the screen power-cycles into the old one, and automatic updates skip that version. A start that ends
sooner (the power gone in the first minute and a half) is tried again at the next power-up, three starts at most. A
screen running from its NOR keeps one system there, so its settings page says that updates need the card install.
`board/f133/mk-ota.sh` makes a release's bundle; each release carries the T113 Dash's bundle as well (see the top
README).

## Updates over SSH

`tools/nor/norw.c` writes a partition of the NOR that the running system reads from. It works block by block: each
block is compared, erased, written and read back. Build it static with the image's toolchain and run it from `/run`,
so nothing it needs is read from the flash while it overwrites it.

```
# on the screen; images staged in /run (RAM)
/etc/init.d/S05dash stop                    # frees the dash's memory: the image must fit in RAM with ~8 MB to spare
/run/norw -n /run/kernel.itb /dev/mtd2 &&   # both images checked before either is written
/run/norw -n /run/rootfs.sqfs /dev/mtd3 &&
/run/norw /run/kernel.itb /dev/mtd2 &&      # the kernel partition (U-Boot's own partition stays read only)
exec /run/norw -c /run/rootfs.sqfs /dev/mtd3
```

norw writes only a whole image to its own partition: a squashfs to the one named `rootfs`, a FIT to `kernel`, and
no bigger than it. Anything else is refused, the whole chip (`mtd0`) and U-Boot's partition included. `-n` makes
those checks and writes nothing, so a root that does not fit stops the update before the kernel is written (a new
kernel with the old root leaves the screen without Wi-Fi or SSH after the power cycle).

With `-c`, once every block matches, norw remounts `/data` read only and asks the MCU for a power cycle. A warm
reboot is no use here: it leaves the Wi-Fi chip unusable until the power goes off. Check that the screen has enough
free memory (`MemAvailable` at least the image's size plus 8 MB) before staging the image.

## Notes on the hardware

- **Power.** The MCU cuts the power about 30 s after power-on unless the SoC holds it: U-Boot sends the hold, then
  `mcud` repeats it every minute (`S03mcu`). Releasing the hold makes the MCU power-cycle the screen.
- **Wi-Fi after a reset.** The Wi-Fi chip does not come back after a warm reset. A boot that finds no Wi-Fi chip has
  the MCU cycle the power, at most three times in a row.
- **Display.** The dash draws in strips and the 2D engine's rotation unit turns them into the portrait framebuffer
  (`package/g2drot`). Without the module, the CPU rotates them.
- **Memory.** Android Auto's video decoder uses 16 MiB of CMA for its pictures and the framebuffer. The decoder keeps
  only as many pictures as the stream's references need, plus one.
