#!/bin/sh
# dash-update-test.sh EXT: run the image's dash-update (EXT/board/dash/rootfs_overlay_r4/usr/sbin/dash-update) under
# busybox against a local HTTPS stand-in for GitHub's releases, signed with a throwaway Ed25519 key, once per case,
# and check the state it reports, ota.txt and the slots. p1 is a folder (mount/umount shimmed); nothing else is faked.
# Run as root in a scratch container (it writes /etc/image-id, /etc/wattline-ota.pub.raw, /data, /run):
#   docker run --rm -v ~/projs/dash-os:/ext:ro debian:stable sh -c 'apt-get update -qq && apt-get install -y -qq
#     busybox curl openssl ca-certificates python3 gcc libc6-dev libssl-dev >/dev/null && sh /ext/board/dash/test/dash-update-test.sh /ext'
set -u
EXT=$1; W=/tmp/dut; FAIL=0
rm -rf $W && mkdir -p $W/bin $W/bb $W/www $W/p1 && cd $W
busybox --install -s $W/bb
cc -O2 -o $W/bin/sig-verify "$EXT/package/dash-ota/src/sig-verify.c" -lcrypto || exit 1
cp "$EXT/board/dash/rootfs_overlay_r4/usr/sbin/dash-update" $W/dash-update

# shims: p1 is a folder; the Wi-Fi network is $SSID; nothing touches a real clock or reboots
cat > $W/bin/mount <<'EOF'
#!/bin/sh
case "$*" in *"-o rw"*) [ -n "${RW_FAIL:-}" ] && exit 32 ;; esac   # RW_FAIL: p1 cannot be mounted to write
[ -n "${P1_FAIL:-}" ] && exit 32   # P1_FAIL: p1 cannot be mounted at all
eval "dir=\${$#}"; eval "dev=\${$(($# - 1))}"; rmdir "$dir" 2>/dev/null; ln -sfn "$dev" "$dir"
EOF
printf '#!/bin/sh\nrm -f "$1"\n' > $W/bin/umount
printf '#!/bin/sh\necho "wpa_state=COMPLETED"; echo "ssid=${SSID:-home}"\n' > $W/bin/wpa_cli
printf '#!/bin/sh\nexit 0\n' > $W/bin/hwclock
printf '#!/bin/sh\necho REBOOT >> /tmp/dut/rebooted\n' > $W/bin/reboot
printf '#!/bin/sh\n[ "$1" = dash ] && { [ -n "${DASH_UP:-}" ] || [ -f /tmp/dash-up ]; } && echo 4242\n' > $W/bin/pidof   # the dash runs (pid 4242) with DASH_UP or /tmp/dash-up; no fsck
chmod +x $W/bin/*

# a throwaway signing key (the image's public half where dash-update looks for it)
openssl genpkey -algorithm ed25519 -out $W/k.pem 2>/dev/null
openssl pkey -in $W/k.pem -pubout -outform DER | tail -c 32 > /etc/wattline-ota.pub.raw
echo "0.3.0-20261114-120000" > /etc/image-id

# HTTPS on 127.0.0.1:8443 with a CA the system trusts (curl checks it like GitHub's)
openssl req -x509 -newkey rsa:2048 -nodes -keyout $W/tls.key -out $W/tls.crt -days 2 -subj /CN=127.0.0.1 \
	-addext subjectAltName=IP:127.0.0.1 2>/dev/null
cp $W/tls.crt /usr/local/share/ca-certificates/dut.crt && update-ca-certificates >/dev/null 2>&1
cat > $W/srv.py <<'EOF'
import http.server, ssl, functools
class H(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *a): pass
h = functools.partial(H, directory='/tmp/dut/www')
s = http.server.ThreadingHTTPServer(('127.0.0.1', 8443), h)
c = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); c.load_cert_chain('/tmp/dut/tls.crt', '/tmp/dut/tls.key')
s.socket = c.wrap_socket(s.socket, server_side=True); s.serve_forever()
EOF
python3 $W/srv.py & SRV=$!
trap 'kill $SRV' EXIT
sleep 1

mkrel() {   # mkrel VERSION [bad-sig|wrong-version|big-kernel]: a release's three assets under www/rel/VERSION
	R=$W/www/rel/$1; rm -rf $R; mkdir -p $R/b; B=wattline-ota-$1.tar.gz
	echo "kernel $1" > $R/b/zImage; echo "dtb $1" > $R/b/dash-r0.dtb; echo "rootfs $1" > $R/b/rootfs.cpio.uboot
	echo "$1" > $R/b/version
	case "${2:-}" in wrong-version) echo 0.9.9 > $R/b/version ;; big-kernel) head -c 8388609 /dev/zero > $R/b/zImage ;; esac
	(cd $R/b && tar -czf ../$B zImage dash-r0.dtb rootfs.cpio.uboot version); rm -rf $R/b
	(cd $R && sha256sum $B > SHA256SUMS && openssl pkeyutl -sign -inkey $W/k.pem -rawin -in SHA256SUMS -out SHA256SUMS.sig)
	[ "${2:-}" = bad-sig ] && head -c 64 /dev/urandom > $R/SHA256SUMS.sig
	printf '[{"assets":[{"browser_download_url": "https://127.0.0.1:8443/rel/%s/%s"}]}]\n' "$1" "$B" > $W/www/api.json
}
reset() {   # reset "OTA_TXT" "CMDLINE" [MOUNTS]
	rm -rf /data/boot /data/ota /run/dash-update.status /run/dash-update.lock /run/dash-ready $W/p1/* $W/rebooted; mkdir -p /data
	[ -n "$1" ] && printf '%s\n' $1 > $W/p1/ota.txt
	echo "console=ttyS0,115200 $2" > $W/cmdline
	echo "${3:-/dev/mmcblk0p3 /data ext4 rw,relatime 0 0}" > $W/mounts
}
upd() { DASH_UPDATE_API=https://127.0.0.1:8443/api.json DASH_UPDATE_CMDLINE=$W/cmdline DASH_UPDATE_MOUNTS=$W/mounts \
	DASH_UPDATE_P1=$W/p1 PATH=$W/bin:$W/bb:$PATH busybox sh $W/dash-update "$@" >$W/out 2>&1; }
state() { sed -n 's/^state=//p' /run/dash-update.status 2>/dev/null; }
ota() { tr -d '\000\r' 2>/dev/null < $W/p1/ota.txt | grep -v '^$' | sort | tr '\n' ' '; }
check() { if [ "$2" = "$3" ]; then echo "PASS $1"; else echo "FAIL $1: got [$2] want [$3]"; sed 's/^/    /' $W/out | tail -4; FAIL=1; fi; }

mkrel 0.4.0
reset "" "dash.slot="
upd install
check "factory start: installs into A, armed"      "$(state) $(ota)$(cat /data/boot/A/version 2>/dev/null)" "armed bootcount=0 slot=A trial=1 0.4.0"

reset "slot=A good=A bootcount=0" "dash.slot=A"
upd install
check "slot A start: installs into B, A kept good" "$(state) $(ota)$(cat /data/boot/B/version 2>/dev/null)" "armed bootcount=0 good=A slot=B trial=1 0.4.0"

reset "slot=A good=A bootcount=0" "dash.slot=A"; mkdir -p /data/boot/A; echo keep > /data/boot/A/version
upd install
check "the booted slot is never written"            "$(cat /data/boot/A/version)" "keep"

reset "slot=B good=B bootcount=0" ""
upd install
check "a boot loader without dash.slot=: refused"   "$(state) $(ota)" "error bootcount=0 good=B slot=B "

reset "" "dash.slot=" "tmpfs /data tmpfs rw 0 0"
upd install
check "no ext4 /data: refused"                      "$(state) $(ls /data/boot 2>/dev/null)" "error "

reset "slot=B good=A trial=1 bootcount=1" "dash.slot=B"
upd install
check "this start on trial: not touched"            "$(state) $(ota)" "trial bootcount=1 good=A slot=B trial=1 "

reset "slot=B good=A trial=1 bootcount=1" "dash.slot="
upd install
check "another slot on trial: not re-armed"         "$(state) $(ota)" "trial bootcount=1 good=A slot=B trial=1 "

reset "" "dash.slot="
SSID=S3XY_OBD upd install
check "on the car's network: nothing fetched"       "$(state) $(ls /data/boot 2>/dev/null)" "skipped "

mkrel 0.4.0 bad-sig; reset "" "dash.slot="
upd install
check "bad signature: discarded"                    "$(state) $(ota)$(ls /data/boot 2>/dev/null)" "error "

mkrel 0.4.0 wrong-version; reset "" "dash.slot="
upd install
check "bundle naming another version: refused"      "$(state) $(ota)$(ls /data/boot 2>/dev/null)" "error "

mkrel 0.4.0 big-kernel; reset "" "dash.slot="
upd install
check "kernel past U-Boot's 8 MiB: refused"         "$(state) $(ota)$(ls /data/boot 2>/dev/null)" "error "

mkrel 0.3.0; reset "" "dash.slot="
upd install
check "already on the newest: nothing"              "$(state) $(ota)" "current "

mkrel 0.4.0; reset "" "dash.slot="; mkdir -p /data/ota; echo 0.4.0 > /data/ota/failed
upd auto
check "auto skips a version that failed here"       "$(state) $(ota)" "failed-before "
upd install
check "install retries it by hand"                  "$(state)" "armed"

reset "" "dash.slot="; mkdir /run/dash-update.lock; sleep 30 & SL=$!; echo $SL > /run/dash-update.lock/pid
upd install
check "a second run while one runs: refused"        "$? $(ota)$(grep -c busy $W/out)" "1 1"
kill $SL; wait $SL 2>/dev/null
upd install
check "a lock left by a dead run: taken over"       "$(state) $([ -d /run/dash-update.lock ] && echo left)" "armed "

reset "slot=B good=A trial=1 bootcount=1" "dash.slot=B"
RW_FAIL=1 upd confirm; R=$?
check "confirm with ota.txt unwritable: says it did not" "$R $(state) $(ota)" "1 error bootcount=1 good=A slot=B trial=1 "
upd confirm; R=$?
check "confirm: the trial start kept"               "$(state) $(ota) $R" "ok bootcount=0 good=B slot=B  0"

reset "slot=A good=A bootcount=0 failed=B" "dash.slot=A"; mkdir -p /data/boot/B; echo 0.4.0 > /data/boot/B/version
upd confirm
check "confirm after a rollback: version recorded"  "$(state) $(ota)$(cat /data/ota/failed)" "rolled-back bootcount=0 good=A slot=A 0.4.0"

mkrel 0.3.0; reset "slot=A good=A bootcount=0" "dash.slot=A"; mkdir -p /data/boot/B; echo 0.5.0 > /data/boot/B/version
upd install
check "a pulled release: the slot's other version not armed" "$(state) $(ota)" "current bootcount=0 good=A slot=A "

mkrel 0.4.0; reset "slot=B good=A trial=1 bootcount=0" "dash.slot=A"; mkdir -p /data/boot/B; echo 0.4.0 > /data/boot/B/version
upd auto
check "armed by Install, then auto: still armed"   "$(state) $(ota)" "armed bootcount=0 good=A slot=B trial=1 "

mkrel 0.4.0; reset "slot=A good=A bootcount=0 failed=B" "dash.slot=A"; mkdir -p /data/boot/B; echo 0.4.0 > /data/boot/B/version
upd auto
check "rolled back, not confirmed yet: auto skips it" "$(state) $(ota)$(cat /data/ota/failed 2>/dev/null)" "failed-before bootcount=0 good=A slot=A 0.4.0"

mkrel 0.5.0; reset "slot=A good=A bootcount=0 failed=B" "dash.slot=A"; mkdir -p /data/boot/B; echo 0.4.0 > /data/boot/B/version
upd install
check "a release fetched into the failed slot is not taken for the failure" "$(state) $(ota)$(cat /data/ota/failed 2>/dev/null)" "armed bootcount=0 good=A slot=B trial=1 0.4.0"

reset "slot=B good=A trial=1 bootcount=1" "dash.slot=B" "tmpfs /data tmpfs rw 0 0"
upd confirm
check "a trial that found no /data is not kept"    "$(state) $(ota)" "trial bootcount=1 good=A slot=B trial=1 "

reset "slot=B good=A trial=1 bootcount=0" "dash.slot=A"; mkdir -p /data/boot/B; echo 0.4.0 > /data/boot/B/version
upd confirm
check "confirm right after an Install: still armed" "$(state) $(ota)" "armed bootcount=0 good=A slot=B trial=1 "

mkrel 0.4.0; reset "slot=A good=A bootcount=0" "dash.slot="
upd install
check "after a fall-back to the factory image: A stays the way back" "$(state) $(ota)" "armed bootcount=0 good=A slot=B trial=1 "

reset "" "dash.slot="; mv $W/www/api.json $W/www/api.json.x
upd check
check "GitHub answering with an error: said as such" "$(state) $(grep -c '404' /run/dash-update.status)" "offline 1"
mv $W/www/api.json.x $W/www/api.json

reset "slot=B good=A trial=1 bootcount=0" "dash.slot=A"; mkdir -p /data/boot/B; echo 0.4.0 > /data/boot/B/version
upd check
check "Check while an update is armed: armed"      "$(state) $(sed -n 's/^latest=//p' /run/dash-update.status)" "armed 0.4.0"

echo "0.4.0-20261114-120000" > /etc/image-id   # running 0.4.0 now: a retry by hand that took
mkrel 0.4.0; reset "slot=B good=B bootcount=0" "dash.slot=B"; mkdir -p /data/ota; echo 0.4.0 > /data/ota/failed
upd auto
check "auto while running a version once failed: current" "$(state)" "current"
reset "slot=B good=A trial=1 bootcount=1" "dash.slot=B"; mkdir -p /data/ota; echo 0.4.0 > /data/ota/failed
upd confirm
check "confirmed: off the failed list"             "$(state) [$(cat /data/ota/failed)]" "ok []"
echo "0.3.0-20261114-120000" > /etc/image-id

mkrel 0.4.0; reset "slot=A good=A bootcount=0 failed=B" "dash.slot=A" "tmpfs /data tmpfs rw 0 0"
upd confirm
check "a rollback seen without /data: kept for later" "$(ota)" "bootcount=0 failed=B good=A slot=A "

mkrel 0.4.0; reset "slot=A good=A bootcount=0" "dash.slot=A"; mkdir -p /data/ota /data/boot/B; echo 0.4.0 > /data/ota/failed
echo 0.4.0 > /data/boot/B/version; echo damaged > /data/boot/B/zImage   # what the failed start left in its slot
upd install
check "a retry of a failed version: fetched and checked anew" "$(state) $(cat /data/boot/B/zImage)" "armed kernel 0.4.0"

reset "slot=B good=A trial=1 bootcount=1" "dash.slot=B"
upd watch
check "watch: this start on trial, unconfirmed: restarted" "$(cat $W/rebooted 2>/dev/null) $(ota)" "REBOOT bootcount=1 good=A slot=B trial=1 "
reset "slot=B good=A trial=1 bootcount=1" "dash.slot=B"
echo 4242 > /run/dash-ready; DASH_UP=1 upd watch
check "watch: on trial, unconfirmed, the dash running: left be" "[$(cat $W/rebooted 2>/dev/null)]" "[]"
reset "slot=B good=A trial=1 bootcount=1" "dash.slot=B"
DASH_UP=1 upd watch
check "watch: the dash running but never ready (its display hung): restarted" "$(cat $W/rebooted 2>/dev/null)" "REBOOT"
reset "slot=B good=A trial=1 bootcount=1" "dash.slot=B"
echo 4242 > /run/dash-ready; rm -f /tmp/dash-up; ( sleep 15; touch /tmp/dash-up ) & upd watch; rm -f /tmp/dash-up
check "watch: a dash up late (after a /data repair), ready: left be" "[$(cat $W/rebooted 2>/dev/null)]" "[]"
reset "slot=B good=A trial=1 bootcount=1" "dash.slot=B"
echo 4141 > /run/dash-ready; DASH_UP=1 upd watch
check "watch: the ready mark an earlier dash left: restarted" "$(cat $W/rebooted 2>/dev/null)" "REBOOT"
reset "slot=B good=B bootcount=0" "dash.slot=B"
upd watch
check "watch: a confirmed start: left be"            "[$(cat $W/rebooted 2>/dev/null)]" "[]"
reset "slot=B good=A trial=1 bootcount=0" "dash.slot=A"
upd watch
check "watch: another slot armed for the next start: left be" "[$(cat $W/rebooted 2>/dev/null)]" "[]"
reset "slot=A trial=1 bootcount=1" "dash.slot="
upd watch
check "watch: the factory image (the trial fell through): left be" "[$(cat $W/rebooted 2>/dev/null)]" "[]"

reset "slot=B good=A trial=1 bootcount=1" "dash.slot=B"
P1_FAIL=1 upd confirm; R=$?
check "confirm on a slot start, p1 unreadable: says it did not" "$R $(state) $(ota)" "1 error bootcount=1 good=A slot=B trial=1 "
reset "" "dash.slot="
P1_FAIL=1 upd confirm; R=$?
check "confirm on the factory image, p1 unreadable: nothing to confirm" "$R" "0"

[ $FAIL = 0 ] && echo "ALL PASS" || { echo "SOME FAILED"; exit 1; }
