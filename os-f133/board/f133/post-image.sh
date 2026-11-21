#!/bin/sh
# The images, in BINARIES_DIR, and copied to /out when build.sh mounts it there:
#   u-boot-sunxi-with-spl.bin  the SPL and U-Boot: the NOR's offset 0, or 8 KiB into a card (padded to 4 KiB sectors)
#   kernel.itb                 the kernel (LZMA) and its device tree, sha256 each (kernel.its): the NOR's "kernel"
#                              partition, which U-Boot checks and boots
#   rootfs.sqfs                the root: the NOR's "rootfs" partition, or a card's
#   Image, f133.dtb            the kernel and device tree on their own (a card boot)
#   card/                      a card install's files, for its fourth partition: the boot files, and nor/ (kernel.itb,
#                              U-Boot) for copying it to the built-in flash later (dash-nor)
# tools/nor writes them: nor.sh from a card boot, norw on a running screen.
set -e
B=$1
K=$(mktemp -d)
trap 'rm -rf "$K"' EXIT
cp "$B/Image" "$K/Image"
cp "$B/sun20i-d1s-srnubi-t313d.dtb" "$K/f133.dtb"
cp "$BR2_EXTERNAL_F133_PATH/board/f133/kernel.its" "$K/kernel.its"
"$HOST_DIR/bin/xz" --format=lzma -9 -k "$K/Image"
(cd "$K" && "$BUILD_DIR/uboot-custom/tools/mkimage" -f kernel.its kernel.itb >/dev/null)
# FILE SIZE PARTITION: each image fits its NOR partition (U-Boot reads at most the kernel's: its norboot, fitsize <=
# 0x440000)
fits() {
	s=$(wc -c < "$1")
	[ "$s" -le $(($2)) ] || { echo "post-image: ${1##*/} is $s bytes, more than the NOR's $3 partition ($2)" >&2; exit 1; }
}
fits "$K/kernel.itb" 0x440000 kernel
fits "$B/rootfs.squashfs" 0xa40000 rootfs
# U-Boot: the NOR's boot partition is 1 MiB, but the same file goes 8 KiB into a card whose first partition may start
# at 1 MiB, so the card's room is the limit
fits "$B/u-boot-sunxi-with-spl.bin" 0x100000-0x2000 boot
cp "$K/kernel.itb" "$B/kernel.itb"
[ -d /out ] || exit 0
cp "$B/Image" /out/Image
cp "$B/sun20i-d1s-srnubi-t313d.dtb" /out/f133.dtb
cp "$B/rootfs.squashfs" /out/rootfs.sqfs
cp "$B/kernel.itb" /out/kernel.itb
cp "$B/u-boot-sunxi-with-spl.bin" /out/u-boot-sunxi-with-spl.bin
truncate -s %4096 /out/u-boot-sunxi-with-spl.bin   # raw disk writes are whole sectors
rm -rf /out/card && mkdir -p /out/card/nor
cp "$B/Image" /out/card/Image
cp "$B/sun20i-d1s-srnubi-t313d.dtb" /out/card/f133.dtb
cp "$B/rootfs.squashfs" /out/card/rootfs.sqfs
cp "$B/kernel.itb" "$B/u-boot-sunxi-with-spl.bin" /out/card/nor/
ls -l /out /out/card /out/card/nor
