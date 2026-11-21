#!/bin/sh
# norw's checks (-n) against a fake /sys/class/mtd (NORW_SYS) and plain files: an image goes only whole, only to its
# own partition, and only if it fits; the whole chip and U-Boot's partition never. No device and no root needed. From
# the root, norw built for the host:
#   cc -O2 -Wall -o /tmp/norw os-f133/tools/nor/norw.c && sh os-f133/tools/nor/test_norw.sh /tmp/norw
set -u
W=${1:?norw binary}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
bad=0
ok() { echo "ok    $*"; }
no() { echo "FAIL  $*"; bad=$((bad + 1)); }
# the screen's NOR: the whole chip, then its partitions
for p in 0:spi0.0:16777216 1:boot:1048576 2:kernel:4456448 3:rootfs:10747904 4:data:524288; do
	n=${p%%:*} r=${p#*:}
	mkdir -p $T/sys/mtd$n
	echo "${r%%:*}" > $T/sys/mtd$n/name
	echo "${r#*:}" > $T/sys/mtd$n/size
done
export NORW_SYS=$T/sys
# SIZE FILE: a squashfs ("hsqs", bytes_used 4096 at 40, little-endian); a FIT (0xd00dfeed, totalsize 4096, big-endian)
sq() { { printf 'hsqs'; head -c 36 /dev/zero; printf '\000\020\000\000\000\000\000\000'; head -c $(($1 - 48)) /dev/zero; } > "$2"; }
fit() { { printf '\320\015\376\355\000\000\020\000'; head -c $(($1 - 8)) /dev/zero; } > "$2"; }
sq 8192 $T/root; fit 8192 $T/kernel; head -c 65536 /dev/urandom > $T/uboot
sq 2048 $T/root.cut; fit 2048 $T/kernel.cut; sq 10747905 $T/root.big
# WHY ARGS...: norw refuses, saying WHY
refuses() { w=$1; shift; "$W" "$@" > $T/out 2>&1 && no "$* went through" || { grep -q "$w" $T/out && ok "refused: $*"; } || no "$*: $(cat $T/out)"; }

"$W" -n $T/root /dev/mtd3 > $T/out && grep -q '"rootfs"' $T/out && ok "a squashfs to rootfs" || no "squashfs to mtd3: $(cat $T/out)"
"$W" -n $T/kernel /dev/mtd2 > $T/out && grep -q '"kernel"' $T/out && ok "a FIT to kernel" || no "FIT to mtd2: $(cat $T/out)"
refuses '/dev/mtd0 is "spi0.0"' -n $T/root /dev/mtd0
refuses '/dev/mtd2 is "kernel"' -n $T/root /dev/mtd2
refuses '/dev/mtd1 is "boot"' -n $T/kernel /dev/mtd1
refuses '/dev/mtd3 is "rootfs"' -c $T/kernel /dev/mtd3
refuses 'neither' -n $T/uboot /dev/mtd0
refuses 'neither' $T/uboot /dev/mtd1
refuses 'neither' -n $T/root.cut /dev/mtd3
refuses 'neither' -n $T/kernel.cut /dev/mtd2
refuses 'bigger than the "rootfs"' -n $T/root.big /dev/mtd3
refuses 'not an MTD partition' -n $T/root /dev/mtd3ro
refuses 'not an MTD partition' -n $T/root /dev/mtdblock3
export NORW_SYS=$T/none   # no such partition in sysfs
refuses 'not an MTD partition' -n $T/root /dev/mtd3
echo "$bad failed"
[ $bad = 0 ]
