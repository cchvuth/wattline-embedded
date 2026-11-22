#!/bin/sh
# dash-nor (the card install copied to the built-in flash) against a stub nor.sh that logs what it is asked, with no
# device: status on a NOR boot and a card install (ready / stock), a copy that first saves the stock firmware and a
# copy that keeps an exact copy already on the card, the failures it reports, and one copy at a time. From the root:
#   sh os-f133/test/test_dash_nor.sh
set -u
D=$(cd "$(dirname "$0")/.." && pwd)/board/f133/overlay/usr/sbin/dash-nor
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
bad=0
ok() { echo "ok    $*"; }
no() { echo "FAIL  $*"; bad=$((bad + 1)); }
mkdir -p $T/sd/nor $T/run $T/log
: > $T/sd/rootfs.sqfs; : > $T/sd/nor/kernel.itb; : > $T/sd/nor/u-boot-sunxi-with-spl.bin
printf 'dev:    size   erasesize  name\nmtd0: 01000000 00010000 "spi0.0"\nmtd3: 00a40000 00010000 "rootfs"\n' > $T/mtd
head -c 4096 /dev/urandom > $T/chip   # the "chip" the stub reads
# the stub: stock while $T/stock exists; read copies the chip; flash and seed fail when $T/fail-<cmd> exists
cat > $T/nor.sh <<EOF
#!/bin/sh
echo "\$* ROOTFS=\${ROOTFS:-} STOCK=\${STOCK:-}" >> $T/calls
case "\$1" in
stock) [ -e $T/stock ] ;;
read) cp $T/chip "\$2" ;;
flash|seed) [ ! -e $T/fail-\$1 ] ;;
esac
EOF
chmod +x $T/nor.sh
run() { DASH_NOR_SD=$T/sd DASH_NOR_RUN=$T/run DASH_NOR_CMDLINE=$T/cmdline DASH_NOR_MTD=$T/mtd DASH_NOR_TOOL=$T/nor.sh \
	DASH_NOR_LOG=$T/log/nor.txt sh "$D" "$@"; }
st() { sed -n 's/^state=//p' $T/run/dash-nor.status 2>/dev/null; }

echo "console=tty0 root=mtd:rootfs" > $T/cmdline
run status; [ ! -e $T/run/dash-nor.status ] && ok "NOR boot: no status, nothing to copy" || no "NOR boot status"
echo "console=tty0" > $T/cmdline
touch $T/stock
run status; [ "$(st)" = stock ] && ok "card install, stock firmware on the NOR: stock" || no "status stock ($(st))"
rm $T/stock
run status; [ "$(st)" = ready ] && ok "card install, our system on the NOR: ready" || no "status ready ($(st))"
mv $T/sd/nor/kernel.itb $T/sd/nor/k
run status; [ ! -e $T/run/dash-nor.status ] && ok "no nor/kernel.itb on the card: nothing to copy" || no "status without nor files"
mv $T/sd/nor/k $T/sd/nor/kernel.itb

# a copy over the stock firmware: dash-nor finds the chip by its size in the mtd table: point it at the stub's chip
CHIPDEV=/dev/mtd0
if [ -w /dev ] && cp $T/chip $CHIPDEV 2>/dev/null; then
	touch $T/stock; : > $T/calls
	run copy; [ "$(st)" = done ] && cmp -s $T/sd/stock.bin $T/chip && grep -q "^read $T/sd/stock.bin.new" $T/calls &&
		grep -q "^flash $T/sd/nor ROOTFS=$T/sd/rootfs.sqfs STOCK=$T/sd/stock.bin" $T/calls && grep -q "^seed" $T/calls &&
		ok "copy over the stock firmware: saved to the card, then flash and seed" || { no "copy (stock)"; cat $T/calls; }
	: > $T/calls
	run copy; ! grep -q "^read" $T/calls && [ "$(st)" = done ] && ok "an exact copy on the card already: not read again" ||
		{ no "copy (stock kept)"; cat $T/calls; }
	# a copy that was cut: the chip half written under the stock firmware's mark; the copy on the card is not replaced
	head -c 4096 /dev/urandom > $CHIPDEV; cp $T/sd/stock.bin $T/stock.keep; : > $T/calls
	sha256sum < $T/sd/stock.bin | cut -d' ' -f1 > $T/sd/nor-flashing   # (nor.sh's mark: that copy's checksum)
	run copy; ! grep -q "^read" $T/calls && cmp -s $T/sd/stock.bin $T/stock.keep && [ "$(st)" = done ] &&
		ok "a copy done again after a cut: the stock copy on the card left as it was" || { no "copy (after a cut)"; cat $T/calls; }
	# a mark that is not this copy's (left by another flash): the chip's firmware is saved as ever, and the copy that
	# was there (another chip's) kept under a number, not written over
	echo 0123 > $T/sd/nor-flashing; : > $T/calls
	run copy; grep -q "^read" $T/calls && cmp -s $T/sd/stock.bin $T/chip && cmp -s $T/sd/stock.2.bin $T/stock.keep &&
		ok "a stale mark: the stock firmware saved all the same, the copy before it kept as stock.2.bin" ||
		{ no "copy (stale mark)"; cat $T/calls; ls $T/sd; }
	rm -f $T/sd/nor-flashing
	rm -f $T/stock $CHIPDEV
else
	echo "skip  the stock copy cases (no writable /dev: run as root in a container)"
fi
: > $T/calls
run copy; [ "$(st)" = done ] && ! grep -q "^read" $T/calls && grep -q "^flash" $T/calls && grep -q "^seed" $T/calls &&
	ok "copy over our system: flash and seed, nothing saved" || { no "copy"; cat $T/calls; }
touch $T/fail-seed
run copy; [ "$(st)" = error ] && grep -q "settings could not be copied" $T/run/dash-nor.status && ok "a seed that fails: said" || no "seed failure"
rm $T/fail-seed
touch $T/fail-flash; : > $T/calls
run copy; [ "$(st)" = error ] && grep -q "could not be written" $T/run/dash-nor.status && ! grep -q "^seed" $T/calls &&
	ok "a flash that fails: error, the card still starts the screen" || no "flash failure ($(cat $T/run/dash-nor.status))"
rm $T/fail-flash
mkdir $T/run/dash-nor.lock
run copy 2>/dev/null; [ $? -ne 0 ] && ok "one copy at a time" || no "lock"
rmdir $T/run/dash-nor.lock
: > $T/sd/Image.try; : > $T/calls
run copy; [ "$(st)" = error ] && grep -q "on trial" $T/run/dash-nor.status && ! grep -q "^flash" $T/calls &&
	ok "an update on trial: refused until it is kept" || no "copy during a try"
rm $T/sd/Image.try
echo "console=tty0 root=mtd:rootfs" > $T/cmdline
run copy; [ "$(st)" = error ] && ok "copy on a NOR boot: refused" || no "copy on a NOR boot"
[ $bad -eq 0 ] && echo "dash-nor: ok" || { echo "dash-nor: $bad failed"; exit 1; }
