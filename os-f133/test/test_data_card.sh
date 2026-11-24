#!/bin/sh
# The F133 card's /data (dash-mkdata, S01data) on a loop device standing in for the card: a fresh image's card (p1 FAT
# only, mounted: the root runs from it) gets p2, ext4, over the rest, p1 unchanged; S01data mounts it; a card flashed
# again (p2 gone from the table) gets its earlier WATTDATA back, files and all; a p2 that is not Linux type (a FAT
# one) is never formatted, nor a second p2 made. Root, privileged, with util-linux, fdisk, e2fsprogs and dosfstools:
#   docker run --rm --privileged -v "$PWD":/r:ro debian:stable sh -c 'apt-get -qq update && apt-get -qq install -y fdisk e2fsprogs dosfstools >/dev/null && sh /r/os-f133/test/test_data_card.sh'
O=$(cd "$(dirname "$0")/.." && pwd)/board/f133/overlay
T=$(mktemp -d)
bad=0
ok() { echo "ok    $*"; }
no() { echo "FAIL  $*"; bad=$((bad + 1)); }
# a container's /dev is no devtmpfs: partition nodes made by hand as the kernel adds them (on the screen devtmpfs does)
nodes() { for p in /sys/block/${L##*/}/${L##*/}p*; do [ -e "$p/dev" ] && [ ! -b /dev/${p##*/} ] && mknod /dev/${p##*/} b $(tr : ' ' < $p/dev); done; }
truncate -s 700M $T/card
printf 'label: dos\nstart=8192, size=262144, type=c, bootable\n' | sfdisk -q $T/card
L=$(losetup -fP --show $T/card) || { echo "skip  no loop device (needs --privileged)"; exit 0; }
trap 'umount $T/p1 /data 2>/dev/null; losetup -d $L; rm -rf "$T"' EXIT
sleep 1; nodes
mkfs.vfat -F 32 -n WATTLINE ${L}p1 >/dev/null && mkdir -p $T/p1 /data && mount ${L}p1 $T/p1 && echo system > $T/p1/rootfs.sqfs
P1=$(sfdisk -d $L | grep "^${L}p1 ")
( while sleep 0.2; do nodes; done ) & H=$!
export PATH=$O/usr/sbin:$PATH DATA_CARD=$L
sh $O/usr/sbin/dash-mkdata > $T/out 2>&1; r=$?
p2=$(sfdisk -d $L | grep "^${L}p2 ")
[ $r = 0 ] && echo "$p2" | grep -q "start= *270336, size= *1163264, type=83" && [ "$(sfdisk -d $L | grep "^${L}p1 ")" = "$P1" ] &&
	blkid ${L}p2 | grep -q 'LABEL="WATTDATA"' && [ "$(cat $T/p1/rootfs.sqfs)" = system ] &&
	ok "first start, p1 in use: p2 over the rest (1 MiB aligned), ext4 WATTDATA, p1 unchanged" || { no "mkdata: $r $p2"; cat $T/out; }
sh $O/etc/init.d/S01data start > $T/out 2>&1
grep -q "^${L}p2 /data ext4 " /proc/mounts && ok "S01data: p2 checked and on /data" || { no "S01data mount"; cat $T/out; }
echo kept > /data/settings.conf; umount /data
sh $O/usr/sbin/dash-mkdata > $T/out 2>&1; [ $? = 1 ] && grep -q "exists already" $T/out && ok "a second run: nothing to do" || no "second run"
# flashed again: the image's table has p1 only, the data still there past it
sfdisk -q --no-reread --delete $L 2 && partx -d -n 2 $L 2>/dev/null; rm -f ${L}p2
sh $O/usr/sbin/dash-mkdata > $T/out 2>&1; r=$?
sh $O/etc/init.d/S01data start > /dev/null 2>&1
[ $r = 0 ] && grep -q "an earlier WATTDATA: kept" $T/out && [ "$(cat /data/settings.conf 2>/dev/null)" = kept ] &&
	ok "a card flashed again: the earlier /data kept, its files with it" || { no "reflash: $r"; cat $T/out; }
umount /data
# a FAT p2 (not Linux type): never formatted by S01data, never mounted as /data
dd if=/dev/zero of=${L}p2 bs=1M count=8 2>/dev/null; mkfs.vfat -F 32 ${L}p2 >/dev/null 2>&1
printf 'start=270336, size=1163264, type=c\n' | sfdisk -q --no-reread -N 2 $L 2>/dev/null
sh $O/etc/init.d/S01data start > $T/out 2>&1
blkid ${L}p2 | grep -q 'TYPE="vfat"' && ! grep -q " /data " /proc/mounts && ok "a FAT p2: left as it is, /data in RAM" || { no "FAT p2"; cat $T/out; }
kill $H
[ $bad = 0 ] && echo "data card: ok" || { echo "data card: $bad failed"; exit 1; }
