#!/bin/sh
# The card install's try-once with no device: S98try's verdict on a tried boot (kept, or given up: try.failed, Image.try
# gone, the power cycled) against stub Wi-Fi, SSH and dash marks; then dash-update confirm at the next start (a try
# that ended undecided armed again, 3 starts at most; the failed list only after a verdict to give up; a keep cut short
# finished; a cut apply cleared) and apply (an older try's marks removed; a card kernel.itb only at the keep). From the root:
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
chmod +x $T/bin/*
echo "/dev/mmcblk0p4 $SD vfat rw,noatime 0 0" > $T/mounts
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
	DASH_UPDATE_KEEPER=$T/bin/keeper sh $O/usr/sbin/dash-update "$@" > /dev/null; }
st() { sed -n 's/^state=//p' $R/run/dash-update.status 2>/dev/null; }
is() { [ "$(cat $SD/$1 2>/dev/null)" = "$2" ]; }   # is FILE CONTENT
none() { for f in "$@"; do [ ! -e $SD/$f ] || return 1; done; }
has() { for f in "$@"; do [ -e $SD/$f ] || return 1; done; }
all_new() { for f in Image f133.dtb rootfs.sqfs nor/kernel.itb nor/u-boot-sunxi-with-spl.bin; do is $f new || return 1; done; }
kept() { all_new && none Image.try rootfs.sqfs.try try.boot rootfs.try try.failed && ! grep -q power-cycle $T/log; }
gaveup() { has try.failed rootfs.sqfs.try && none Image.try && is Image old && grep -q power-cycle $T/log && grep -q reboot $T/log; }
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
[ $bad -eq 0 ] && echo "ota try: ok" || { echo "ota try: $bad failed"; exit 1; }
