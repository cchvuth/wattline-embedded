#!/bin/sh
# The images, in BINARIES_DIR, and copied to /out when build.sh mounts it there:
#   u-boot-sunxi-with-spl.bin  the SPL and U-Boot: the NOR's offset 0, or 8 KiB into a card (padded to 4 KiB sectors)
#   kernel.itb                 the kernel (gzip) and its device tree, a crc32 each (kernel.its): the NOR's "kernel"
#                              partition, which U-Boot checks and boots
#   rootfs.sqfs                the root: the NOR's "rootfs" partition, or a card's (zstd when it fits, else xz)
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
gzip -9 -n -k "$K/Image"
(cd "$K" && "$BUILD_DIR/uboot-custom/tools/mkimage" -f kernel.its kernel.itb >/dev/null)
# FILE SIZE PARTITION: each image fits its NOR partition (U-Boot reads no more than 0x440000 of a kernel: its
# norboot's fitsize, from when the partition was that big)
fits() {
	s=$(wc -c < "$1")
	[ "$s" -le $(($2)) ] || { echo "post-image: ${1##*/} is $s bytes, more than the NOR's $3 partition ($2)" >&2; exit 1; }
}
fits "$K/kernel.itb" 0x400000 kernel
# The root again in zstd, kept when that fits the partition: the one core unpacks it far faster than xz (a cold start
# of the dash 1.3-1.9 s where it took 2.9, the dash on the screen 1.4 s sooner after power-on), but it is a tenth
# bigger, so an image with more in it stays in xz. The kernel reads either (linux.fragment). Only as root (the build
# container): unpacked by anyone else, the files would lose their owners and device nodes.
if [ "$(id -u)" = 0 ]; then
	"$HOST_DIR/bin/unsquashfs" -q -d "$K/r" "$B/rootfs.squashfs" >/dev/null
	# in the smallest blocks that fit (bigger ones pack tighter, and unpack more for each read)
	z=
	for b in 131072 262144 524288; do
		"$HOST_DIR/bin/mksquashfs" "$K/r" "$K/zstd.sqfs" -comp zstd -Xcompression-level 22 -b $b -noappend \
			-no-progress >/dev/null
		z=$(wc -c < "$K/zstd.sqfs")
		[ "$z" -le $((0xa80000)) ] && break
	done
	if [ "$z" -le $((0xa80000)) ]; then
		cp "$K/zstd.sqfs" "$B/rootfs.squashfs"
		echo "post-image: the root in zstd ($z bytes, $((b / 1024)) KiB blocks)"
	else
		echo "post-image: the root stays in xz (in zstd $z bytes: more than its partition)"
	fi
	rm -rf "$K/r" "$K/zstd.sqfs"
fi
fits "$B/rootfs.squashfs" 0xa80000 rootfs
# U-Boot: the NOR's boot partition is 1 MiB, but the same file goes 8 KiB into a card whose first partition may start
# at 1 MiB, so the card's room is the limit
fits "$B/u-boot-sunxi-with-spl.bin" 0xf0000 boot
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
# (U-Boot as the update bundle has it, padded: nor.sh then finds an unchanged one unchanged whichever the card's came
# from, where the unpadded copy had it written again after the first update)
cp "$B/kernel.itb" /out/u-boot-sunxi-with-spl.bin /out/card/nor/
ls -l /out /out/card /out/card/nor
