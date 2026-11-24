#!/bin/sh
# The images, in BINARIES_DIR, and copied to /out when build.sh mounts it there:
#   u-boot-sunxi-with-spl.bin  the SPL and U-Boot, 8 KiB into the card (padded to 4 KiB sectors)
#   Image, f133.dtb            the kernel and its device tree, on the card's first partition
#   rootfs.sqfs                the root (squashfs, zstd), beside them
#   wattline-f133-sdcard-<version>.img.xz  the whole card (genimage.cfg), flashed with any imager: U-Boot and the first
#                              partition; /data is made at the first start
# Image, f133.dtb and rootfs.sqfs are also what mk-ota.sh bundles for an update.
set -e
B=$1 E=$BR2_EXTERNAL_F133_PATH/board/f133
V=$(cat "$E/version")
cp "$B/sun20i-d1s-srnubi-t313d.dtb" "$B/f133.dtb"
cp "$B/rootfs.squashfs" "$B/rootfs.sqfs"
truncate -s %4096 "$B/u-boot-sunxi-with-spl.bin"   # raw disk writes are whole sectors
support/scripts/genimage.sh -c "$E/genimage.cfg"
echo "post-image: card image $(wc -c < "$B/sdcard.img") bytes"
[ -d /out ] || exit 0
cp "$B/Image" "$B/f133.dtb" "$B/rootfs.sqfs" "$B/u-boot-sunxi-with-spl.bin" /out/
rm -f /out/kernel.itb /out/wattline-f133-sdcard-*.img.xz && rm -rf /out/card
xz -T0 -6 -c "$B/sdcard.img" > "/out/wattline-f133-sdcard-$V.img.xz"
ls -l /out
