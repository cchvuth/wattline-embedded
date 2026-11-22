#!/bin/sh
# The card install's try-once with no device: S98try's verdict on a tried boot (kept, or given up: try.failed, Image.try
# gone, the power cycled) against stub Wi-Fi, SSH and dash marks; then dash-update confirm at the next start (a try
# that ended undecided armed again, 3 starts at most; the failed list only after a verdict to give up; a keep cut short
# finished; a cut apply cleared) and apply (an older try's marks removed; a card kernel.itb only at the keep). Then a
# screen that runs from its flash: the try mark set by apply and kept up while the try waits, cleared with a try given
# up; from the card's boot for it (data=nor) the kept version written to the flash (a stub nor.sh) and the mark
# cleared; no update where U-Boot does not look at the mark. From the root:
#   sh os-f133/test/test_ota_try.sh
set -u
O=$(cd "$(dirname "$0")/.." && pwd)/board/f133/overlay
T=$(mktemp -d) && [ -d "$T" ] || exit 1
trap 'rm -rf "$T"' EXIT
bad=0
ok() { echo "ok    $*"; }
no() { echo "FAIL  $*"; bad=$((bad + 1)); }
R=$T/r SD=$T/r/mnt/sd
mkdir -p $T/bin
# stubs: no waiting, the reboot logged, the station's scan and the SSH listener from files, nothing to the syslog
printf '#!/bin/sh\n' > $T/bin/sleep
printf '#!/bin/sh\n' > $T/bin/logger
printf '#!/bin/sh\n' > $T/bin/keeper
printf '#!/bin/sh\necho reboot >> %s/log\n' "$T" > $T/bin/reboot
printf '#!/bin/sh\nshift\nexec "$@"\n' > $T/bin/timeout   # (S98try's limit on wpa_cli: not everywhere this test runs)
cat > $T/bin/wpa_cli <<EOF
#!/bin/sh
echo "bssid / frequency / signal level / flags / ssid"
[ -e $T/scan ] && printf '12:34:56:78:9a:bc\t2437\t-50\t[ESS]\tS3XY_OBD\n'
exit 0
EOF
cat > $T/bin/netstat <<EOF
#!/bin/sh
[ -e $T/sshd ] && echo "tcp        0      0 0.0.0.0:22              0.0.0.0:*               LISTEN"
exit 0
EOF
# the flash's try partition as a file: erased to 0xFF; nor.sh logged, failing on demand
printf '#!/bin/sh\n[ -e %s/noerase ] && exit 1\nprintf "\\377\\377\\377\\377\\377\\377\\377\\377" > "$1"\n' "$T" > $T/bin/flash_eraseall
printf '#!/bin/sh\necho "nor.sh $* ROOTFS=$ROOTFS KERNEL=$KERNEL" >> %s/log\n[ ! -e %s/norfail ]\n' "$T" "$T" > $T/bin/norsh
chmod +x $T/bin/*
echo "/dev/mmcblk0p4 $SD vfat rw,noatime 0 0" > $T/mounts
printf 'dev:    size   erasesize  name\nmtd2: 00400000 00010000 "kernel"\nmtd3: 00a80000 00010000 "rootfs"\n' > $T/mtd
echo 0.1.0-20261120-120000 > $T/image-id

# card [MARKS...]: version 0.2.0 put in place on a card running 0.1.0: the old files ("old"), the new as .try ("new"),
# the given marks (try.boot, rootfs.try...); the next start not a try, no dash, no Wi-Fi, SSH off
card() {
	rm -rf "$T/r" "$T/scan" "$T/sshd"
	mkdir -p $SD/nor $SD/ota $R/run/ssh-keys $R/proc $R/dev $R/etc/init.d
	for f in Image f133.dtb rootfs.sqfs nor/kernel.itb nor/u-boot-sunxi-with-spl.bin; do
		echo old > $SD/$f; echo new > $SD/$f.try
	done
	echo 0.2.0 > $SD/ota/trying
	for m in "$@"; do : > $SD/$m; done
	echo console=tty0 > $R/proc/cmdline
	: > $R/dev/kmsg; : > $T/log
	printf '#!/bin/sh\necho "$1" >> %s/log\n' "$T" > $R/etc/init.d/S03mcu; chmod +x $R/etc/init.d/S03mcu
}
tryboot() { echo "console=tty0 try=kernel" > $R/proc/cmdline; : > $SD/try.boot; : > $SD/rootfs.try; : > $R/.rootfs-tried; }
healthy() { : > $R/run/dash-ready; : > $R/run/dash-shown; : > $T/scan; }
s98() {   # S98try start, then its verdict: kept, or the reboot after giving up (<= 20 s)
	PATH=$T/bin:$PATH S98TRY_ROOT=$R sh $O/etc/init.d/S98try start
	i=0; while [ $i -lt 20 ] && ! grep -q kept $R/dev/kmsg && ! grep -q reboot $T/log; do sleep 1; i=$((i + 1)); done
}
upd() { PATH=$T/bin:$PATH DASH_UPDATE_SD=$SD DASH_UPDATE_RUN=$R/run DASH_UPDATE_CMDLINE=$R/proc/cmdline \
	DASH_UPDATE_MOUNTS=$T/mounts DASH_UPDATE_TRIED=$R/.rootfs-tried DASH_UPDATE_IMAGEID=$T/image-id \
	DASH_UPDATE_KEEPER=$T/bin/keeper DASH_UPDATE_TRYDEV=${TRYF-$T/try} DASH_UPDATE_NORSH=$T/bin/norsh DASH_UPDATE_NORLOG=$T/norlog \
	DASH_UPDATE_MTD=$T/mtd sh $O/usr/sbin/dash-update "$@" > /dev/null; }
st() { sed -n 's/^state=//p' $R/run/dash-update.status 2>/dev/null; }
is() { [ "$(cat $SD/$1 2>/dev/null)" = "$2" ]; }   # is FILE CONTENT
none() { for f in "$@"; do [ ! -e $SD/$f ] || return 1; done; }
has() { for f in "$@"; do [ -e $SD/$f ] || return 1; done; }
all_new() { for f in Image f133.dtb rootfs.sqfs nor/kernel.itb nor/u-boot-sunxi-with-spl.bin; do is $f new || return 1; done; }
kept() { all_new && none Image.try rootfs.sqfs.try try.boot rootfs.try try.failed && ! grep -q power-cycle $T/log && [ -e $R/run/try-judging ] && [ -e $R/run/try-judged ]; }
gaveup() { has try.failed rootfs.sqfs.try && none Image.try && is Image old && grep -q power-cycle $T/log && grep -q reboot $T/log && [ -e $R/run/try-judging ] && [ -e $R/run/try-judged ]; }
nexta() { echo console=tty0 > $R/proc/cmdline; rm -f $R/.rootfs-tried; rm -rf "$R/run"; mkdir -p $R/run; }   # the next start

# S98try
card; healthy; PATH=$T/bin:$PATH S98TRY_ROOT=$R sh $O/etc/init.d/S98try start; sleep 1
is Image.try new && [ ! -s $R/dev/kmsg ] && ok "not a try: nothing done" || no "not a try"
card; tryboot; healthy; s98
kept && ok "kept: the dash drawn, the station sees networks, SSH off" || no "kept (station)"
card; tryboot; healthy; echo old > $SD/kernel.itb; s98
kept && none kernel.itb && ok "kept: a card-root kernel.itb (an old U-Boot boots it) gone with the old Image" || no "kept (kernel.itb)"
card; tryboot; healthy; rm $R/run/dash-shown; echo old > $SD/kernel.itb; s98
gaveup && is kernel.itb old && ok "given up: the card-root kernel.itb left, for an old U-Boot's way back" || no "given up (kernel.itb)"
card; tryboot; healthy; rm $T/scan; : > $R/run/dash-aa; echo "ssh-ed25519 AAAA k" > $R/run/ssh-keys/authorized_keys; : > $T/sshd; s98
kept && ok "kept: Android Auto's AP up and no station, SSH on and listening" || no "kept (AA, SSH)"
card; tryboot; healthy; echo "ssh-ed25519 AAAA k" > $R/run/ssh-keys/authorized_keys; s98
gaveup && ok "given up: SSH on with a key, nothing listening (try.failed, Image.try gone, the power cycled)" || no "given up (SSH)"
card; tryboot; healthy; rm $R/run/dash-shown; s98
gaveup && ok "given up: the dash never drew" || no "given up (dash)"
card; tryboot; healthy; rm $T/scan; s98
gaveup && ok "given up: no Wi-Fi (no network seen, no AA)" || no "given up (Wi-Fi)"
card; tryboot; healthy; rm $R/.rootfs-tried; s98
gaveup && ok "given up: the kernel tried without its root" || no "given up (no root)"

# dash-update confirm at the next start, and apply
card; tryboot; healthy; echo "ssh-ed25519 AAAA k" > $R/run/ssh-keys/authorized_keys; s98; nexta
upd confirm; [ "$(st)" = rolled-back ] && grep -qx 0.2.0 $SD/ota/failed && [ -z "$(cd $SD && ls *.try nor/*.try 2>/dev/null)" ] &&
	none try.failed try.boot rootfs.try ota/trying && ok "after S98try gave up: rolled back, on the failed list, cleared" ||
	no "confirm after giving up ($(st))"
card try.boot rootfs.try
upd confirm; [ "$(st)" = trial-failed ] && is ota/tries 1 && none try.boot rootfs.try ota/failed && is Image.try new && is rootfs.sqfs.try new &&
	ok "a try cut short (no verdict): armed again, not failed" || no "re-arm 1 ($(st))"
upd confirm; [ "$(st)" = armed ] && ok "then armed for the next power-up" || no "armed after re-arm ($(st))"
: > $SD/try.boot; upd confirm; [ "$(st)" = trial-failed ] && is ota/tries 2 && none ota/failed && ok "cut short again: armed again" ||
	no "re-arm 2 ($(st))"
: > $SD/try.boot; upd confirm; [ "$(st)" = rolled-back ] && grep -qx 0.2.0 $SD/ota/failed && none Image.try rootfs.sqfs.try ota/tries ota/trying &&
	ok "the third start cut short: given up" || no "re-arm bound ($(st))"
card; printf '0.2.0\n0.1.5\n' > $SD/ota/failed
for f in Image f133.dtb rootfs.sqfs nor/kernel.itb nor/u-boot-sunxi-with-spl.bin; do mv $SD/$f.try $SD/$f; done
echo 0.2.0-20261121-120000 > $T/image-id
upd confirm; [ "$(st)" = ok ] && ! grep -qx 0.2.0 $SD/ota/failed && grep -qx 0.1.5 $SD/ota/failed && none ota/trying &&
	ok "kept: ok, off the failed list" || no "kept ($(st))"
card try.boot rootfs.try; mv $SD/rootfs.sqfs.try $SD/rootfs.sqfs; mv $SD/nor/kernel.itb.try $SD/nor/kernel.itb
mv $SD/nor/u-boot-sunxi-with-spl.bin.try $SD/nor/u-boot-sunxi-with-spl.bin
upd confirm; [ "$(st)" = ok ] && all_new && none Image.try f133.dtb.try try.boot rootfs.try && ok "a keep cut short: finished" ||
	no "cut keep ($(st))"
echo 0.1.0-20261120-120000 > $T/image-id
card
upd confirm; [ $? = 0 ] && [ "$(st)" = armed ] && is Image.try new && ok "armed: left alone" || no "armed ($(st))"
card; tryboot
upd confirm; [ $? = 1 ] && [ "$(st)" = trial ] && is Image.try new && ok "on the tried boot: on trial" || no "trial ($(st))"
card; rm $SD/Image.try $SD/f133.dtb.try; mkdir -p $SD/ota/0.2.0/b; echo new > $SD/ota/0.2.0/b/Image
upd confirm; [ "$(st)" = error ] && none ota/failed rootfs.sqfs.try ota/0.2.0 ota/trying && ok "a cut apply: cleared, not failed" ||
	no "cut apply ($(st))"
card try.boot rootfs.try try.failed ota/tries kernel.itb
for f in Image f133.dtb rootfs.sqfs nor/kernel.itb nor/u-boot-sunxi-with-spl.bin; do rm $SD/$f.try; done; rm $SD/ota/trying
mkdir -p $SD/ota/0.2.0/b/nor; echo 0.2.0 > $SD/ota/0.2.0/b/version
for f in Image f133.dtb rootfs.sqfs nor/kernel.itb nor/u-boot-sunxi-with-spl.bin; do echo new > $SD/ota/0.2.0/b/$f; done
upd apply; [ "$(st)" = armed ] && is Image.try new && is rootfs.sqfs.try new && is nor/kernel.itb.try new && is ota/trying 0.2.0 &&
	none try.boot rootfs.try try.failed ota/tries ota/0.2.0 && has kernel.itb &&
	ok "apply: the .try files in place, older marks gone, a card-root kernel.itb kept until the keep" ||
	no "apply ($(st))"

# a screen that runs from its built-in flash
mark() { [ "$(head -c 4 $T/try 2>/dev/null)" = TRY1 ]; }
flashboot() { echo "console=tty0 root=mtd:rootfs ota=flash f133t=1,2,50" > $R/proc/cmdline; }
forflash() { echo "console=tty0 $* data=nor" > $R/proc/cmdline; }
unpacked() {   # version 0.2.0 fetched, nothing in place
	for f in Image f133.dtb rootfs.sqfs nor/kernel.itb nor/u-boot-sunxi-with-spl.bin; do rm $SD/$f.try; done; rm $SD/ota/trying
	mkdir -p $SD/ota/0.2.0/b/nor; echo 0.2.0 > $SD/ota/0.2.0/b/version
	for f in Image f133.dtb rootfs.sqfs nor/kernel.itb nor/u-boot-sunxi-with-spl.bin; do echo new > $SD/ota/0.2.0/b/$f; done
	( cd $SD/ota/0.2.0/b && sha256sum rootfs.sqfs nor/kernel.itb > sums )
}
rm -f $T/try; card; unpacked; echo "console=tty0 root=mtd:rootfs f133t=1,2,50" > $R/proc/cmdline
upd apply; [ $? = 1 ] && [ "$(st)" = card-only ] && none Image.try && ! mark && ok "from the flash, a U-Boot that does not look at the mark: no update" ||
	no "old U-Boot ($(st))"
card; unpacked; flashboot
echo stale > $SD/ota/sums; : > $SD/nor.once
upd apply; none nor.once || no "apply left an older once-mark (nor.once)"
 [ "$(st)" = armed ] && is Image.try new && is rootfs.sqfs.try new && mark && grep -q " rootfs.sqfs$" $SD/ota/sums && ! grep -q stale $SD/ota/sums &&
	ok "from the flash: apply puts the files on the card, keeps their checksums and sets the mark" || no "flash apply ($(st))"
upd confirm; [ "$(st)" = armed ] && mark && is Image.try new && ok "looked at again on that start: armed" || no "armed twice ($(st))"
rm $T/try; upd confirm; [ "$(st)" = armed ] && mark && ok "armed with the mark missing (a cut apply): set" || no "armed, no mark ($(st))"
upd confirm; [ "$(st)" = armed ] && mark && is Image.try new && none ota/failed && ok "and looked at again on that start: still armed" || no "armed, mark set, again ($(st))"
card; unpacked; flashboot; rm -f $T/try; : > $T/noerase
upd apply; [ $? = 1 ] && [ "$(st)" = error ] && ! mark && ok "the mark cannot be written: the apply fails" || no "no mark ($(st))"
rm -f $T/noerase
card try.boot rootfs.try; flashboot; printf TRY1 > $T/try
: > $SD/nor.once
upd confirm; [ "$(st)" = trial-failed ] && mark && none try.boot nor.once && ok "a try cut short, seen from the flash: armed again, the mark kept, an old once-mark gone" ||
	no "flash re-arm ($(st))"
upd confirm; [ "$(st)" = armed ] && mark && is Image.try new && none ota/failed && ok "and looked at again on that start (a tap on Check): armed, nothing given up" ||
	no "flash re-arm, again ($(st))"
card try.boot rootfs.try try.failed; rm $SD/Image.try; flashboot; printf TRY1 > $T/try
upd confirm; [ "$(st)" = rolled-back ] && ! mark && none rootfs.sqfs.try try.boot try.failed ota/trying && grep -qx 0.2.0 $SD/ota/failed &&
	ok "a try given up, back on the flash: cleared, the mark too" || no "flash rollback ($(st))"
card try.boot rootfs.try try.failed; rm $SD/Image.try; flashboot; printf TRY1 > $T/try; : > $T/noerase
upd confirm; [ "$(st)" = error ] && mark && has try.boot rootfs.sqfs.try && ok "the mark cannot be cleared: the card's marks stay (U-Boot still starts the flash)" ||
	no "rollback, mark stuck ($(st))"
rm -f $T/noerase
card; unpacked; rm -rf $SD/ota/0.2.0; flashboot; printf TRY1 > $T/try
upd confirm; ! mark && ok "a mark with nothing on the card for it: cleared" || no "stale mark"
# the card's boot for the flash: the try, kept, then onto the flash
card; forflash try=kernel; : > $SD/try.boot; : > $R/.rootfs-tried; printf TRY1 > $T/try
upd flash; [ $? = 1 ] && ! grep -q nor.sh $T/log && mark && ok "on trial: not to the flash yet" || no "flash during the trial"
upd install; [ $? = 1 ] && ok "the card's boot for the flash: no other update from it" || no "install on the card's boot"
card; forflash; printf TRY1 > $T/try; echo 0.2.0-20261121-120000 > $T/image-id
for f in Image f133.dtb rootfs.sqfs nor/kernel.itb nor/u-boot-sunxi-with-spl.bin; do mv $SD/$f.try $SD/$f; done
upd confirm; [ "$(st)" = ok ] && mark && is ota/to-flash 0.2.0 && ok "kept, seen from the card's boot: the mark stays until the flash holds it" || no "kept for the flash ($(st))"
: > $T/norfail; upd flash; [ $? = 1 ] && [ "$(st)" = error ] && mark && ok "the flash cannot be written: the mark stays (the card starts the screen)" ||
	no "flash write failed ($(st))"
rm $T/norfail; : > $T/log; : > $SD/nor.once
upd flash; [ "$(st)" = ok ] && ! mark && none nor.once && grep -qx "nor.sh update $SD/nor ROOTFS=$SD/rootfs.sqfs KERNEL=$SD/nor/kernel.itb" $T/log &&
	none ota/to-flash && ok "the kept version written to the flash (nor.sh update), the mark cleared" || no "to the flash ($(st)): $(cat $T/log)"
: > $T/log; upd flash; [ "$(st)" = ok ] && ! grep -q nor.sh $T/log && ok "again on that boot: nothing more to write" || no "flash twice"
card; echo console=tty0 > $R/proc/cmdline; printf TRY1 > $T/try; : > $T/log
upd flash; ! grep -q nor.sh $T/log && mark && ok "a card install: nothing to the flash" || no "flash on a card install"
# on the try's own start, once S98try has kept it: confirmed and written to the flash from that same start
card; forflash try=kernel; tryboot; forflash try=kernel; healthy; printf TRY1 > $T/try; echo 0.2.0-20261121-120000 > $T/image-id; : > $T/log; s98
upd confirm; upd flash; [ "$(st)" = ok ] && ! mark && all_new && grep -q "nor.sh update" $T/log && none ota/trying ota/to-flash &&
	ok "the try's own start: kept by S98try, confirmed, written to the flash" || no "flash on the kept try's start ($(st))"
card; forflash; echo 0.2.0-20261121-120000 > $T/image-id; rm $SD/ota/trying; echo 0.2.0 > $SD/ota/to-flash; rm -f $T/try; : > $T/log
for f in Image f133.dtb rootfs.sqfs nor/kernel.itb nor/u-boot-sunxi-with-spl.bin; do mv $SD/$f.try $SD/$f; done
touch $SD/nor.once
upd flash; [ "$(st)" = ok ] && none ota/to-flash nor.once && ! grep -q nor.sh $T/log && ok "written before, its last step cut: nothing more, said as done" || no "flash, mark off ($(st))"
printf TRY1 > $T/try; echo 0.2.0 > $SD/ota/to-flash
TRYF=; upd flash; r=$?; unset TRYF
[ $r = 1 ] && [ "$(st)" = error ] && ! grep -q nor.sh $T/log && ok "a kernel without the try partition: nothing written" || no "flash without the partition ($(st))"
# a version that does not fit its own partitions (this kernel's /proc/mtd): given up before nor.sh, the flash kept
card; forflash; printf TRY1 > $T/try; : > $T/log; rm $SD/ota/trying
for f in Image f133.dtb rootfs.sqfs nor/kernel.itb nor/u-boot-sunxi-with-spl.bin; do mv $SD/$f.try $SD/$f; done
head -c 300 /dev/zero > $SD/nor/kernel.itb; cp $T/mtd $T/mtd.real
printf 'dev:    size   erasesize  name\nmtd2: 00000100 00010000 "kernel"\nmtd3: 00a80000 00010000 "rootfs"\n' > $T/mtd
upd flash; [ $? = 1 ] && [ "$(st)" = error ] && ! mark && ! grep -q nor.sh $T/log && ok "a kernel bigger than its partition: given up before writing" || no "size check ($(st))"
mv $T/mtd.real $T/mtd
# the card's files changed since the download: not to the flash
card; forflash; printf TRY1 > $T/try; : > $T/log
for f in Image f133.dtb rootfs.sqfs nor/kernel.itb nor/u-boot-sunxi-with-spl.bin; do mv $SD/$f.try $SD/$f; done
( cd $SD && sha256sum rootfs.sqfs nor/kernel.itb > ota/sums ); echo damaged > $SD/rootfs.sqfs; rm $SD/ota/trying
upd flash; [ $? = 1 ] && [ "$(st)" = error ] && ! mark && ! grep -q nor.sh $T/log && ok "the card's root is not the download's any more: nothing written, the flash keeps its version" ||
	no "sums ($(st))"
echo new > $SD/rootfs.sqfs; printf TRY1 > $T/try
upd flash; [ "$(st)" = ok ] && ! mark && grep -q "nor.sh update" $T/log && none ota/sums && ok "files that match the download: written" || no "sums ok ($(st))"
# the card's start for the flash, the try not made (U-Boot could not write try.boot): given up, the mark cleared
echo 0.1.0-20261120-120000 > $T/image-id
card; forflash; printf TRY1 > $T/try
: > $SD/nor.once
upd confirm; upd flash; [ "$(st)" = error ] && ! mark && none Image.try rootfs.sqfs.try ota/trying nor.once && grep -qx 0.2.0 $SD/ota/failed && is Image old &&
	! grep -q nor.sh $T/log && ok "a try U-Boot could not mark on the card: cleared and said so, the flash starts the screen again" || no "unmarked try ($(st))"
card; flashboot; printf TRY1 > $T/try
upd confirm; [ "$(st)" = error ] && ! mark && none Image.try rootfs.sqfs.try ota/trying ota/failed &&
	ok "the same seen from the flash (a card with files only: U-Boot had nothing else to start), not held against the version" ||
	no "unmarked try, from the flash ($(st))"
# seen from the flash: a keep cut before its end, and a kept version whose start from the card did not come
card try.boot rootfs.try; flashboot; printf TRY1 > $T/try; mv $SD/rootfs.sqfs.try $SD/rootfs.sqfs; mv $SD/nor/kernel.itb.try $SD/nor/kernel.itb
upd confirm; [ "$(st)" = armed ] && mark && all_new && none try.boot rootfs.try Image.try ota/trying && is ota/to-flash 0.2.0 &&
	ok "from the flash, a keep cut short: finished, the card starts the screen next (to the flash)" || no "cut keep, from the flash ($(st))"
rm $T/try; upd confirm; mark && [ "$(st)" = armed ] && ok "from the flash, a kept version not yet there: still armed, the mark set again" || no "to-flash mark ($(st))"
rm $R/run/dash-update.armed; : > $SD/nor.once   # the next start, from the flash again: the card did not start the screen
upd confirm; [ "$(st)" = error ] && ! mark && none ota/to-flash nor.once && ok "the card never started it: given up, not armed for good" || no "to-flash, no card start ($(st))"
echo 0.2.0 > $SD/ota/to-flash; printf TRY1 > $T/try; : > $R/run/dash-update.armed; rm -f $R/run/dash-update.status
echo 0.2.0-20261121-120000 > $T/image-id
upd confirm; ! mark && none ota/to-flash && [ "$(st)" = ok ] && ok "the flash runs that version by now: nothing left to write" || no "to-flash, done ($(st))"
echo 0.1.0-20261120-120000 > $T/image-id
card; flashboot; printf TRY1 > $T/try; rm -rf $SD/ota $SD/*.try $SD/nor/*.try; : > $T/mounts
upd confirm; ! mark && ok "no card in the screen: the mark cleared" || no "mark without a card"
echo "/dev/mmcblk0p4 $SD vfat rw,noatime 0 0" > $T/mounts
card; flashboot; printf TRY1 > $T/try
for f in Image f133.dtb rootfs.sqfs nor/kernel.itb nor/u-boot-sunxi-with-spl.bin; do mv $SD/$f.try $SD/$f; done
upd confirm; [ "$(st)" = armed ] && mark && is ota/to-flash 0.2.0 && none ota/trying &&
	ok "from the flash, kept and never confirmed: the card starts the screen next (to the flash)" || no "kept, unconfirmed, from the flash ($(st))"
card; flashboot; printf TRY1 > $T/try; rm $SD/Image.try $SD/f133.dtb.try $SD/rootfs.sqfs.try $SD/nor/*.try; mkdir -p $SD/ota/0.2.0/b; echo new > $SD/ota/0.2.0/b/Image
upd confirm; [ "$(st)" = error ] && ! mark && none ota/trying ota/to-flash ota/0.2.0 && ok "from the flash, a cut apply: cleared, the mark too" || no "cut apply, from the flash ($(st))"
echo 0.1.0-20261120-120000 > $T/image-id
[ $bad -eq 0 ] && echo "ota try: ok" || { echo "ota try: $bad failed"; exit 1; }
