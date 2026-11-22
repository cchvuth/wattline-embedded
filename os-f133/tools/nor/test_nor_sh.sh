#!/bin/sh
# nor.sh against fake MTD devices (regular files) and stub flashcp / flash_eraseall (mount, umount for seed). Root in a
# throwaway container:
#   docker run --rm -v "$PWD/os-f133/tools/nor":/n:ro debian:stable sh /n/test_nor_sh.sh
set -e
N=$(cd "$(dirname "$0")" && pwd)
T=/tmp/nortest
rm -rf $T && mkdir -p $T/bin $T/img
# the NOR's partitions (sun20i-d1s-srnubi-t313d.dts)
cat > $T/mtd <<'EOF'
dev:    size   erasesize  name
mtd0: 01000000 00010000 "spi0.0"
mtd1: 000f0000 00010000 "boot"
mtd2: 00440000 00010000 "kernel"
mtd3: 00a40000 00010000 "rootfs"
mtd4: 00080000 00010000 "data"
mtd5: 00010000 00010000 "try"
EOF
echo "console=tty0 panic=5" > $T/cmdline
ff() { tr '\000' '\377' < /dev/zero | head -c "$1"; }
# the stock chip: random, its GPT signature at 0xc200
head -c 16777216 /dev/urandom > /dev/mtd0
printf 'EFI PART' | dd of=/dev/mtd0 bs=1 seek=49664 conv=notrunc 2>/dev/null
for p in 1:983040 2:4456448 3:10747904 4:524288 5:65536; do head -c ${p#*:} /dev/urandom > /dev/mtd${p%%:*}; done
head -c 885489 /dev/urandom > $T/img/u-boot-sunxi-with-spl.bin
head -c 4030000 /dev/urandom > $T/img/kernel.itb
head -c 5955584 /dev/urandom > $T/img/rootfs.sqfs
printf '#!/bin/sh\n[ -e %s/fail-cp ] && exit 1\n[ "$1" = -v ] && shift\ndd if="$1" of="$2" conv=notrunc 2>/dev/null\n' $T > $T/bin/flashcp
cat > $T/bin/flash_eraseall <<'EOF'
#!/bin/sh
# as busybox's: every byte 0xFF; with -j, a JFFS2 cleanmarker (12 bytes) at each 64 KiB erase block's start
j=; [ "$1" = -j ] && j=1 && shift
s=$(wc -c < "$1")
tr '\000' '\377' < /dev/zero | head -c $s | dd of="$1" conv=notrunc 2>/dev/null
i=0; while [ -n "$j" ] && [ $i -lt $s ]; do
	printf '\205\031\003\040\014\000\000\000\261\260\036\344' | dd of="$1" bs=1 seek=$i conv=notrunc 2>/dev/null
	i=$((i + 65536))
done
EOF
chmod +x $T/bin/*
export PATH=$T/bin:$PATH MTD=$T/mtd CMDLINE=$T/cmdline
bad=0
ok() { echo "ok    $*"; }
no() { echo "FAIL  $*"; bad=$((bad + 1)); }
same() { [ "$(head -c "$(wc -c < "$1")" "$2" | sha256sum)" = "$(sha256sum < "$1")" ]; }
cm() { printf '\205\031\003\040\014\000\000\000\261\260\036\344'; }   # the JFFS2 cleanmarker of flash_eraseall -j
erased() {   # as flash_eraseall -j leaves /data: each 64 KiB block a cleanmarker, then 0xFF
	i=0; while [ $i -lt 8 ]; do
		[ "$(dd if=/dev/mtd4 bs=65536 skip=$i count=1 2>/dev/null | sha256sum)" = "$( { cm; ff 65524; } | sha256sum)" ] || return 1
		i=$((i + 1))
	done
}

sh $N/nor.sh read $T/r.bin > /dev/null && cmp -s $T/r.bin /dev/mtd0 && ok "read: the file is the chip" || no read
sh $N/nor.sh stock && ok "stock: the stock firmware is on the chip" || no "stock (stock chip)"
cp /dev/mtd3 $T/rootfs.before
sh $N/nor.sh flash $T/img > $T/out 2>&1 && no "flash without stock.bin went on" ||
	{ grep -q "stock firmware is on the NOR" $T/out && cmp -s /dev/mtd3 $T/rootfs.before && ok "flash refused, nothing written: no copy of the stock chip"; } || no "guard"
head -c 100 /dev/urandom >> $T/img/rootfs.sqfs; cp /dev/mtd0 $T/img/stock.bin
truncate -s 10747905 $T/img/rootfs.sqfs
sh $N/nor.sh flash $T/img > $T/out 2>&1 && no "an oversized rootfs went on" ||
	{ grep -q "bigger than the rootfs" $T/out && cmp -s /dev/mtd3 $T/rootfs.before && ok "oversized rootfs refused, nothing written"; } || no size
head -c 5955584 /dev/urandom > $T/img/rootfs.sqfs
# a mark left behind (another chip's flash, cut) with no copy of this chip beside it: the guard stands
echo 0123 > $T/img/nor-flashing; mv $T/img/stock.bin $T/stock.away
sh $N/nor.sh flash $T/img > $T/out 2>&1 && no "a stale mark let a flash over the stock firmware go on" ||
	{ grep -q "stock firmware is on the NOR" $T/out && cmp -s /dev/mtd3 $T/rootfs.before && ok "a stale mark without the chip's copy: refused, nothing written"; } || no "stale mark"
mv $T/stock.away $T/img/stock.bin; rm $T/img/nor-flashing
# a flash cut after its erases (a write that fails): the chip is no longer the stock copy, its GPT still there; the
# flash done again goes on from the copy taken before, which stays as it was
cp $T/img/stock.bin $T/stock.keep; touch $T/fail-cp
sh $N/nor.sh flash $T/img > $T/out 2>&1 && no "a flash whose write fails went on" ||
	{ [ -e $T/img/nor-flashing ] && sh $N/nor.sh stock &&
		ok "a flash cut short: marked as under way, the stock firmware's mark still on the chip"; } || no "cut flash"
rm $T/fail-cp
printf 'ours' | dd of=/dev/mtd0 bs=1 seek=2097152 conv=notrunc 2>/dev/null   # (the fake's whole chip is its own file)
sh $N/nor.sh flash $T/img > $T/out 2>&1 && same $T/img/rootfs.sqfs /dev/mtd3 && same $T/img/kernel.itb /dev/mtd2 &&
	cmp -s $T/img/stock.bin $T/stock.keep && [ ! -e $T/img/nor-flashing ] &&
	same $T/img/u-boot-sunxi-with-spl.bin /dev/mtd0 && erased && [ ! -e $T/img/nor-data.bin ] &&
	ok "flash with the stock copy: rootfs, kernel, U-Boot written, data erased (the stock firmware's: not saved)" || { no flash; cat $T/out; }
sh $N/nor.sh stock && no "stock after the flash" || ok "stock: not once flashed"
rm $T/img/stock.bin
sh $N/nor.sh flash $T/img > $T/out 2>&1 && grep -q "U-Boot unchanged" $T/out && same $T/img/kernel.itb /dev/mtd2 &&
	[ ! -e $T/img/nor-data.bin ] &&
	ok "flash again (no stock firmware left): no copy needed, U-Boot left alone, kernel rewritten" || no reflash
mkdir -p $T/root && head -c 5955584 /dev/urandom > $T/root/rootfs.sqfs
ROOTFS=$T/root/rootfs.sqfs sh $N/nor.sh flash $T/img > $T/out 2>&1 && same $T/root/rootfs.sqfs /dev/mtd3 &&
	ok "flash with ROOTFS= elsewhere (the card install's root beside nor/)" || { no "ROOTFS="; cat $T/out; }
# the NOR's /data in use (a screen that ran from its NOR): saved beside stock.bin before the erase
head -c 524288 /dev/urandom > /dev/mtd4; cp /dev/mtd4 $T/data.before
sh $N/nor.sh flash $T/img > $T/out 2>&1 && cmp -s $T/img/nor-data.bin $T/data.before && erased &&
	ok "flash saves the NOR's /data as nor-data.bin, then erases it" || { no "data backup"; cat $T/out; }
sh $N/nor.sh flash $T/img > $T/out 2>&1 && cmp -s $T/img/nor-data.bin $T/data.before &&
	ok "flash again (its /data erased): the saved copy kept" || { no "backup kept"; cat $T/out; }
# a flash cut in its erase of /data, or a seed after it, and done again; a second copy later: what is there then goes
# beside the first copy, which stays
head -c 524288 /dev/urandom > /dev/mtd4; cp /dev/mtd4 $T/data.second
sh $N/nor.sh flash $T/img > $T/out 2>&1 && cmp -s $T/img/nor-data.bin $T/data.before && cmp -s $T/img/nor-data.2.bin $T/data.second &&
	ok "a later flash with /data in use again: saved beside the first copy, which stays" || { no "second backup"; cat $T/out; }
head -c 200000 /dev/urandom > /dev/mtd4   # that flash cut in its erase, and done again: a third file, the two before as they were
sh $N/nor.sh flash $T/img > $T/out 2>&1 && cmp -s $T/img/nor-data.bin $T/data.before && cmp -s $T/img/nor-data.2.bin $T/data.second &&
	[ -f $T/img/nor-data.3.bin ] && ok "a flash done again after a cut: no copy written over" || { no "third backup"; cat $T/out; }
cp $T/data.before /dev/mtd4; cp /dev/mtd3 $T/rootfs.before
STOCK=$T/nodir/stock.bin sh $N/nor.sh flash $T/img > $T/out 2>&1 && no "flash went on without saving /data" ||
	{ grep -q "could not be saved" $T/out && cmp -s /dev/mtd4 $T/data.before && cmp -s /dev/mtd3 $T/rootfs.before &&
		ok "/data that cannot be saved: flash refused, nothing erased or written"; } || no "backup failure"
printf '#!/bin/sh\n[ "$1" = -v ] && shift\nhead -c 4096 /dev/zero | dd of="$2" conv=notrunc 2>/dev/null\n' > $T/bin/flashcp
sh $N/nor.sh restore $T/r.bin > $T/out 2>&1 && no "a bad write passed" ||
	{ grep -q "reads back wrong" $T/out && ok "a write that reads back wrong stops it"; } || no readback
printf '#!/bin/sh\n[ "$1" = -v ] && shift\ndd if="$1" of="$2" conv=notrunc 2>/dev/null\n' > $T/bin/flashcp
sh $N/nor.sh restore $T/r.bin > /dev/null && cmp -s $T/r.bin /dev/mtd0 && ok "restore: the chip is the stock image again" || no restore
# a restore marks itself as a flash does (dash-nor then leaves the stock copy alone if it is cut), and ends it
printf '#!/bin/sh\nexit 1\n' > $T/bin/flashcp
sh $N/nor.sh restore $T/r.bin > $T/out 2>&1 && no "a restore whose write fails went on" ||
	{ [ "$(cat $T/nor-flashing 2>/dev/null)" = "$(sha256sum < $T/r.bin | cut -d' ' -f1)" ] && ok "a restore cut short: marked with its image's checksum"; } || { no "restore mark"; cat $T/out; }
printf '#!/bin/sh\n[ "$1" = -v ] && shift\ndd if="$1" of="$2" conv=notrunc 2>/dev/null\n' > $T/bin/flashcp
sh $N/nor.sh restore $T/r.bin > /dev/null && [ ! -e $T/nor-flashing ] && ok "the restore done again: the mark gone" || no "restore again"
sh $N/nor.sh read $T/r.bin > $T/out 2>&1 && no "read wrote over a file" || { grep -q "there already" $T/out && ok "read does not write over a file"; } || no "read over"
ff 524288 > /dev/mtd4
sh $N/nor.sh restore $T/img/nor-data.bin > /dev/null && cmp -s /dev/mtd4 $T/data.before &&
	ok "restore nor-data.bin: the data partition back" || no "restore data"
head -c 1000 /dev/urandom > $T/odd.bin
sh $N/nor.sh restore $T/odd.bin > $T/out 2>&1 && no "an odd-sized restore went on" ||
	{ grep -q "neither" $T/out && cmp -s /dev/mtd4 $T/data.before && ok "restore of an odd size refused"; } || no "restore size"
# seed with stub mount and umount (the NOR's /data a plain directory); a cp that fails stops it half-way
printf '#!/bin/sh\necho "mount $*" >> %s/calls\n' $T > $T/bin/mount
printf '#!/bin/sh\necho "umount $*" >> %s/calls\n' $T > $T/bin/umount
chmod +x $T/bin/mount $T/bin/umount
mkdir -p /mnt/sd/data && echo x > /mnt/sd/data/settings
printf '#!/bin/sh\nexit 1\n' > $T/bin/cp && chmod +x $T/bin/cp && : > $T/calls
sh $N/nor.sh seed > $T/out 2>&1 && no "a seed whose copy fails went on" ||
	{ grep -q "^umount /tmp/nordata" $T/calls && ok "a seed that stops half-way unmounts the NOR's /data"; } || { no "seed trap"; cat $T/calls; }
# one file that does not fit: left out and named, the rest copied
echo y > /mnt/sd/data/big
printf '#!/bin/sh\ncase "$*" in *data/big*) exit 1 ;; esac\nexec /bin/cp "$@"\n' > $T/bin/cp; : > $T/calls
sh $N/nor.sh seed > $T/out 2>&1 && no "a seed that left a file out said nothing" ||
	{ grep -q "not copied.*big" $T/out && [ -f /tmp/nordata/settings ] && [ ! -e /tmp/nordata/big ] &&
		[ "$(grep -c '^umount' $T/calls)" = 1 ] && ok "a file that does not fit: left out and named, the rest copied"; } ||
	{ no "seed skip"; cat $T/out; }
rm /mnt/sd/data/big
rm $T/bin/cp; : > $T/calls
sh $N/nor.sh seed > $T/out 2>&1 && [ "$(grep -c '^umount' $T/calls)" = 1 ] && [ -f /tmp/nordata/settings ] &&
	ok "a seed that completes: the card's /data copied, unmounted once" || { no seed; cat $T/out $T/calls; }
# SSH turned off on the card's system (dash-ssh-disable's flag in its /data): off on the NOR's too, the card's key out
mkdir -p /mnt/sd/data/ssh && touch /mnt/sd/data/ssh/disabled && echo "ssh-ed25519 AAAA card" > /mnt/sd/authorized_keys; : > $T/calls
sh $N/nor.sh seed > $T/out 2>&1 && [ -e /tmp/nordata/ssh/disabled ] && [ ! -e /tmp/nordata/ssh/enabled ] &&
	! grep -qs card /tmp/nordata/ssh/authorized_keys && ok "a seed with SSH turned off: it stays off" || { no "seed ssh off"; cat $T/out; }
rm -rf /mnt/sd/data/ssh /mnt/sd/authorized_keys
# a flash clears the try mark (dash-update's, for U-Boot); update: the root and the kernel only, on a NOR that is ours
printf '\0\0\0\0\0\0\0\0' | dd of=/dev/mtd0 bs=1 seek=49664 conv=notrunc 2>/dev/null   # (no stock GPT: ours)
printf TRY1 | dd of=/dev/mtd5 conv=notrunc 2>/dev/null
sh $N/nor.sh flash $T/img > $T/out 2>&1 && [ "$(head -c 4 /dev/mtd5)" != TRY1 ] && ok "flash: the try mark cleared" || { no "flash, try mark"; cat $T/out; }
mkdir -p $T/new; head -c 6000000 /dev/urandom > $T/new/rootfs.sqfs; head -c 4000000 /dev/urandom > $T/new/kernel.itb
cp /dev/mtd4 $T/data.before; cp /dev/mtd1 $T/boot.before
sh $N/nor.sh update $T/new > $T/out 2>&1 && same $T/new/rootfs.sqfs /dev/mtd3 && same $T/new/kernel.itb /dev/mtd2 &&
	cmp -s /dev/mtd4 $T/data.before && cmp -s /dev/mtd1 $T/boot.before &&
	ok "update: the root and the kernel written, /data and the boot loader left alone" || { no update; cat $T/out; }
printf '#!/bin/sh\nexit 1\n' > $T/bin/flashcp   # (any write fails)
sh $N/nor.sh update $T/new > $T/out 2>&1 && grep -q "already" $T/out && ok "update again: nothing written" || { no "update twice"; cat $T/out; }
head -c 4000000 /dev/urandom > $T/new/kernel.itb
sh $N/nor.sh update $T/new > $T/out 2>&1 && no "an update whose write fails went on" ||
	{ [ "$(head -c 4 /dev/mtd2 | od -An -tx1 | tr -d ' ')" = ffffffff ] && same $T/new/rootfs.sqfs /dev/mtd3 &&
		ok "an update cut short: the kernel erased (U-Boot goes the card's way), the root as it was"; } || { no "cut update"; cat $T/out; }
printf '#!/bin/sh\n[ "$1" = -v ] && shift\ndd if="$1" of="$2" conv=notrunc 2>/dev/null\n' > $T/bin/flashcp
sh $N/nor.sh update $T/new > $T/out 2>&1 && same $T/new/kernel.itb /dev/mtd2 && same $T/new/rootfs.sqfs /dev/mtd3 &&
	ok "the update done again: the kernel written" || { no "update resumed"; cat $T/out; }
truncate -s 10747905 $T/new/rootfs.sqfs; cp /dev/mtd2 $T/kernel.before
sh $N/nor.sh update $T/new > $T/out 2>&1 && no "an oversized root went on" ||
	{ cmp -s /dev/mtd2 $T/kernel.before && ok "update: an oversized root refused, nothing erased"; } || no "update size"
head -c 6000000 /dev/urandom > $T/new/rootfs.sqfs
printf 'EFI PART' | dd of=/dev/mtd0 bs=1 seek=49664 conv=notrunc 2>/dev/null
sh $N/nor.sh update $T/new > $T/out 2>&1 && no "an update over the stock firmware went on" ||
	{ grep -q "stock firmware" $T/out && cmp -s /dev/mtd2 $T/kernel.before && ok "update refused while the stock firmware is on the chip"; } || no "update on stock"
printf '\0\0\0\0\0\0\0\0' | dd of=/dev/mtd0 bs=1 seek=49664 conv=notrunc 2>/dev/null
# the card's boot for the flash's update (data=nor): the NOR's /data is mounted there
echo "console=tty0 data=nor" > $T/cmdline; cp /dev/mtd3 $T/rootfs.before
for c in "flash $T/img" seed "restore $T/img/stock.bin"; do
	sh $N/nor.sh $c > $T/out 2>&1 && no "$c ran with the NOR's /data in use" ||
		{ grep -q "data partition is in use" $T/out && cmp -s /dev/mtd4 $T/data.before && cmp -s /dev/mtd3 $T/rootfs.before; } || no "data=nor: $c"
done
[ $bad = 0 ] && ok "with the NOR's /data in use: flash, seed and restore refused"
sh $N/nor.sh update $T/new > $T/out 2>&1 && same $T/new/rootfs.sqfs /dev/mtd3 && cmp -s /dev/mtd4 $T/data.before &&
	ok "and update goes on, /data untouched" || { no "update with data=nor"; cat $T/out; }
echo "console=tty0 root=mtd:rootfs" > $T/cmdline
sh $N/nor.sh read $T/x.bin > $T/out 2>&1 && no "ran while booted from the NOR" ||
	{ grep -q "running from the NOR" $T/out && ok "refuses while running from the NOR"; } || no nor-boot
echo "$bad failed"
[ $bad = 0 ]
