#!/bin/sh
# SRNUBI T313D: the SPI NOR from Linux booted off the card, never while running from the NOR (tools/nor/norw.c writes
# a running one). Needs this tree's kernel (its MTD support and the NOR's partitions) and busybox's flashcp and
# flash_eraseall.
#
#   nor.sh read FILE     the whole chip into FILE (on the card), then the chip read once more: the two sha256 agree
#   nor.sh flash DIR     DIR/rootfs.sqfs, kernel.itb, u-boot-sunxi-with-spl.bin onto the NOR (U-Boot last, if it
#                        changed), the data partition erased; each read back and compared. While the stock firmware is
#                        still on the chip, only with DIR/stock.bin byte for byte the chip (nor.sh read DIR/stock.bin,
#                        kept off the screen, and compared with a second read before the first write). A data
#                        partition of this system's (not erased) is saved first, beside stock.bin: nor-data.bin.
#   nor.sh seed          the card's /data (no logs), Wi-Fi settings and SSH keys into the NOR's data partition
#   nor.sh restore FILE  a whole-chip image (the stock backup) or the data partition (nor-data.bin) back onto the NOR,
#                        read back and compared
#   nor.sh stock         exit 0 while the stock firmware is on the NOR (its GPT), 1 once it is not
set -e
MTD=${MTD:-/proc/mtd}
die() { echo "nor.sh: $*" >&2; exit 1; }
part() { awk -v n="\"$1\"" '$4 == n { sub(":", "", $1); print "/dev/" $1 }' $MTD; }
psize() { echo $((0x$(awk -v n="\"$1\"" '$4 == n { print $2 }' $MTD))); }
sha() { sha256sum | cut -d' ' -f1; }
CHIP=$(awk '$2 == "01000000" { sub(":", "", $1); print "/dev/" $1; exit }' $MTD)

grep -q "root=mtd:" ${CMDLINE:-/proc/cmdline} && die "running from the NOR: boot from the card first"
[ -n "$CHIP" ] || die "no 16 MiB MTD device (a kernel without the NOR's support?)"

# FILE DEVICE: flashcp erases the blocks FILE covers, writes and compares; then a read-back sha256 of FILE's length
put() {
	s=$(wc -c < "$1")
	flashcp -v "$1" "$2"
	[ "$(head -c "$s" "$2" | sha)" = "$(sha < "$1")" ] || die "$2 reads back wrong after $1"
	echo "$2: $1 ($s bytes) written and read back"
}

fits() { [ "$(wc -c < "$1")" -le "$(psize "$2")" ] || die "$1 is bigger than the $2 partition"; }
# a data partition with nothing in it: each 64 KiB erase block all 0xFF past its first 12 bytes, where flash_eraseall
# -j leaves a JFFS2 cleanmarker (what a first flash left: a retry must not save that over the backup it made)
blank() {
	n=$(( $(psize data) / 65536 )) i=0
	while [ $i -lt $n ]; do
		[ "$(dd if="$1" bs=65536 skip=$i count=1 2>/dev/null | tail -c +13 | tr -d '\377' | wc -c)" = 0 ] || return 1
		i=$((i + 1))
	done
}
stock_on_chip() { [ "$(dd if="$CHIP" bs=512 skip=97 count=1 2>/dev/null | head -c 8)" = "EFI PART" ]; }

case "$1" in
read)
	F=${2:?FILE}
	dd if="$CHIP" of="$F" bs=64k
	a=$(sha < "$F")
	b=$(sha < "$CHIP")
	echo "$F: $a"
	echo "$CHIP read again: $b"
	[ "$a" = "$b" ] || die "the two reads differ"
	;;
flash)
	# DIR's files, or ROOTFS= KERNEL= UBOOT= STOCK= for any of them elsewhere (dash-nor: the card install's root)
	D=${2:?DIR}
	R=${ROOTFS:-$D/rootfs.sqfs} K=${KERNEL:-$D/kernel.itb} U=${UBOOT:-$D/u-boot-sunxi-with-spl.bin} S=${STOCK:-$D/stock.bin}
	for f in "$R" "$K" "$U"; do [ -f "$f" ] || die "no $f"; done
	fits "$R" rootfs
	fits "$K" kernel
	fits "$U" boot
	# the stock GPT ("EFI PART" at 0xc200) still there: the stock firmware, kept only as an exact copy beside the images.
	# Not while a flash is under way (FL, from before the first erase to the end): one cut there leaves the GPT in
	# place (the boot area is written last) over a chip half ours, which no longer equals the copy taken before it and
	# must never be read over it; the flash is then done again.
	# The mark holds that copy's checksum, and counts only with the copy there and the chip's boot area still the
	# copy's (the GPT is in its first block, erased before anything is written there): a mark left behind on a card
	# that then meets another chip, or one without its copy, turns nothing off.
	FL=$(dirname "$S")/nor-flashing
	under_way() {
		[ -f "$S" ] && [ "$(cat "$FL" 2>/dev/null)" = "$(sha < "$S")" ] &&
			[ "$(head -c 1048576 "$CHIP" | sha)" = "$(head -c 1048576 "$S" | sha)" ]
	}
	if stock_on_chip && ! under_way; then
		[ -f "$S" ] && [ "$(sha < "$S")" = "$(sha < "$CHIP")" ] ||
			die "the stock firmware is on the NOR and $S is not an exact copy of it (nor.sh read)"
	fi
	# the NOR's /data saved before it is erased (settings, Wi-Fi networks, Bluetooth bonds made while running from
	# it), read twice; not the stock firmware's (stock.bin holds it) and not an erased one (a retry's)
	P=$(part data) B=$(dirname "$S")/nor-data.bin
	# (nor one half erased or half seeded by a flash that was cut: the copy taken before it stays)
	if ! stock_on_chip && ! blank "$P" && ! { [ -e "$FL" ] && [ -f "$B" ]; }; then
		dd if="$P" of="$B.new" bs=64k 2>/dev/null && [ "$(sha < "$B.new")" = "$(sha < "$P")" ] && mv -f "$B.new" "$B" ||
			die "the NOR's data partition could not be saved to $B"
		sync
		echo "the NOR's data partition saved to $B (nor.sh restore puts it back)"
	fi
	{ if [ -f "$S" ]; then sha < "$S"; else echo -; fi; } > "$FL.new" && mv -f "$FL.new" "$FL" && sync ||
		die "$FL could not be written"
	flash_eraseall -j "$P"
	# the kernel FIT is the commit mark: erased first, written after the rootfs, so a cut write fails U-Boot's hash
	# check (the card's way then) instead of booting a half-written root
	flash_eraseall "$(part kernel)"
	put "$R" "$(part rootfs)"
	put "$K" "$(part kernel)"
	# U-Boot last and only if it changed (the one write that leaves nothing to boot if cut); the boot partition
	# itself is read-only, so through the whole chip
	s=$(wc -c < "$U")
	if [ "$(head -c "$s" "$CHIP" | sha)" = "$(sha < "$U")" ]; then
		echo "U-Boot unchanged, not rewritten"
	else
		put "$U" "$CHIP"
	fi
	# (kept for a caller that goes on to seed, and takes it away after: a seed that was cut left /data half made, and
	# the flash done again saved that over the copy)
	[ -n "${NOR_KEEP_MARK:-}" ] || rm -f "$FL"
	sync
	echo "the NOR is ready: it boots when no card with a boot loader is in"
	;;
seed)
	# the card's /data (settings, Wi-Fi networks, the AA certificate, Bluetooth bonds, the Commander's id...) without
	# its logs and recordings, the bench's Wi-Fi, the SSH keys and host key (the same fingerprint) where the NOR's
	# system looks for them (dash-ssh-enable, wifi-connect). On the screen only: nothing leaves it.
	M=/tmp/nordata
	mkdir -p $M
	mount -t jffs2 mtd:data $M
	# unmounted however it ends: one left mounted would be erased under the mount by the next flash
	trap 'umount $M' EXIT
	trap 'exit 1' INT TERM HUP
	miss=
	for f in /mnt/sd/data/* /mnt/sd/data/.[!.]*; do
		[ -e "$f" ] || continue
		case ${f##*/} in log|rec) continue ;; esac
		# one that does not fit (a picture, in 512 KiB) is left out and named at the end, the rest still copied: the
		# networks and the keys come after it
		# (its half-made copy removed, to leave the room to the rest; not what the NOR already had under that name)
		had=; [ -e "$M/${f##*/}" ] && had=1
		cp -a "$f" $M/ || { [ -n "$had" ] || rm -rf "$M/${f##*/}"; miss="$miss ${f##*/}"; }
	done
	[ -f /mnt/sd/wpa_supplicant.conf ] && cp /mnt/sd/wpa_supplicant.conf $M/
	mkdir -p $M/ssh && chmod 700 $M/ssh
	cat /mnt/sd/authorized_keys $M/ssh/authorized_keys 2>/dev/null | tr -d '\r' | sort -u > $M/ssh/ak.new
	mv $M/ssh/ak.new $M/ssh/authorized_keys && chmod 600 $M/ssh/authorized_keys
	[ -s $M/ssh/authorized_keys ] && touch $M/ssh/enabled
	[ -f /mnt/sd/dropbear/dropbear_ed25519_host_key ] &&
		cp /mnt/sd/dropbear/dropbear_ed25519_host_key $M/ssh/ && chmod 600 $M/ssh/dropbear_ed25519_host_key
	sync
	du -s $M; df $M
	umount $M
	trap - EXIT
	[ -z "$miss" ] || die "not copied to the NOR's /data (no room?):$miss"
	;;
stock)
	stock_on_chip
	;;
restore)
	F=${2:?FILE}
	case $(($(wc -c < "$F"))) in
	16777216) put "$F" "$CHIP"; rm -f "$(dirname "$F")/nor-flashing" ;;   # (the chip is that image again: no flash under way)
	$(psize data)) put "$F" "$(part data)" ;;
	*) die "$F is neither a 16 MiB image nor a data partition (nor-data.bin)" ;;
	esac
	sync
	echo "restored: remove the card to boot it"
	;;
*)
	sed -n '2,16p' "$0"
	exit 1
	;;
esac
