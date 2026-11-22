#!/bin/sh
# dash-aa (wireless Android Auto on the F133 screen) under busybox, against stubs that log what they are asked (iw,
# wpa_supplicant, wpa_cli, ip, udhcpd, aa, spike-vdec, mcud, S45wifi-auto, the hooks), with no device: start with the AP
# beside the station (ap0, the station kept), restore with ap0 refused by the driver (the AP on wlan0, the station
# stopped, no pairing window), an ap0 whose AP does not come up (wlan0 instead), aa-solo; the passphrase only on aa
# mcu's stdin, in the AP's config (600) and the aa-start hook's environment; the pairing window (AT#CA, a request
# accepted with AT#KA, never AT#CB); stop's order (hook, aa gone, each phone deauthenticated and 2 s for it before the
# AP goes, the AP down, the marks gone, then the station back or a settings save made during AA applied); a hook that
# hangs cut short. It writes /run, /data and /etc, so it runs in a throwaway container, with busybox and a writable
# /sys/class/net. From the root:
#   docker run --rm --privileged -e DEBIAN_FRONTEND=noninteractive -v "$PWD":/r:ro debian:stable sh -c \
#     'apt-get -qq update && apt-get -qq install -y busybox >/dev/null && sh /r/os-f133/test/test_dash_aa.sh'
set -u
D=$(cd "$(dirname "$0")/.." && pwd)/board/f133/overlay/usr/sbin/dash-aa
[ -e /.dockerenv ] || [ -e /run/.containerenv ] || { echo "dash-aa test: run it in a container (see its header)"; exit 1; }
# a tmpfs over /sys/class/net (docker's --tmpfs, or mounted here where the container may: --privileged, as CI runs it)
mkdir -p /sys/class/net/wlan0 2>/dev/null || { mount -t tmpfs tmpfs /sys/class/net 2>/dev/null && mkdir -p /sys/class/net/wlan0; } ||
	{ echo "dash-aa test: /sys/class/net not writable (--tmpfs it, or run the container --privileged)"; exit 1; }
BB=$(command -v busybox) || { echo "dash-aa test: needs busybox"; exit 1; }
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
bad=0 shown=
ok() { echo "ok    $*"; }
no() { echo "FAIL  $*"; bad=$((bad + 1)); [ -n "$shown" ] && return; shown=1
	echo "--- calls"; cat $T/calls; echo "--- dash-aa's output"; cat $T/out; echo "---"; }
mkdir -p /bb /stub /etc/wattline /etc/init.d && $BB --install -s /bb
export PATH=/stub:/bb:$PATH
: > /etc/wattline/aa.crt; : > /etc/wattline/aa.key
echo 02:00:00:00:00:01 > /sys/class/net/wlan0/address
STA1=02:11:22:33:44:01 STA2=02:11:22:33:44:02   # the phones on the AP
LOOP='while :; do sleep 1 & wait $!; done'      # a daemon: TERM answered at once

# the waits shortened, still in order: usleep logged and a tenth as long; the hook's 10 s and 2 s limits made short
cat > /stub/usleep <<EOF
#!/bin/sh
echo "usleep \$1" >> $T/calls; exec /bb/usleep \$((\$1 / 10))
EOF
cat > /stub/sleep <<'EOF'
#!/bin/sh
case "$*" in 10) set -- 0.3 ;; 2) set -- 0.2 ;; esac; exec /bb/sleep "$@"
EOF
cat > /stub/iw <<EOF
#!/bin/sh
echo "iw \$*" >> $T/calls
case "\$*" in
"dev wlan0 interface add ap0 type __ap") [ -e $T/noap0 ] && exit 1
	mkdir -p /sys/class/net/ap0; echo 02:aa:bb:cc:dd:01 > /sys/class/net/ap0/address ;;
"dev ap0 del") rm -rf /sys/class/net/ap0 ;;
esac
exit 0
EOF
# the station (--run sta) or the AP: its config kept (and its mode), its control socket made, its pid file written
cat > /stub/wpa_supplicant <<EOF
#!/bin/sh
if [ "\$1" = --run ]; then
	trap 'echo "wpa_supplicant \$2 stopped" >> $T/calls; rm -f "\${3:-/nonexistent}"; exit 0' TERM; $LOOP
fi
echo "wpa_supplicant \$*" >> $T/calls
while [ \$# -gt 0 ]; do case \$1 in -i) I=\$2; shift ;; -c) C=\$2; shift ;; -P) P=\$2; shift ;; esac; shift; done
cp "\$C" $T/cap/ap.conf; stat -c %a "\$C" > $T/cap/ap.mode
mkdir -p /run/aa-ap
perl -MSocket -e 'socket(S, PF_UNIX, SOCK_STREAM, 0) && bind(S, pack_sockaddr_un(\$ARGV[0])) or die' /run/aa-ap/\$I
/stub/wpa_supplicant --run ap:\$I /run/aa-ap/\$I >/dev/null 2>&1 &
echo \$! > "\$P"
EOF
cat > /stub/wpa_cli <<EOF
#!/bin/sh
echo "wpa_cli \$*" >> $T/calls
case "\$*" in
*" status") case "\$*" in *"-i ap0 "*) [ -e $T/ap0dead ] && { echo wpa_state=SCANNING; exit 0; } ;; esac
	echo wpa_state=COMPLETED ;;
*" all_sta") [ -e $T/nosta ] || printf '$STA1\nflags=[AUTH][ASSOC][AUTHORIZED]\n$STA2\nflags=[AUTH][ASSOC][AUTHORIZED]\n' ;;
*) echo OK ;;
esac
EOF
printf '#!/bin/sh\necho "ip $*" >> %s/calls\n' $T > /stub/ip
printf '#!/bin/sh\necho "udhcpd $*" >> %s/calls\ntrap "exit 0" TERM; %s\n' $T "$LOOP" > /stub/udhcpd
cat > /stub/aa <<EOF
#!/bin/sh
[ "\$1" = certcheck ] && exit 0
echo "aa \$*" >> $T/calls
trap 'echo "aa \$1 stopped" >> $T/calls; exit 0' TERM
if [ "\$1" = serve ]; then echo "aa listens on \${AA_LISTEN:-every address}" >> $T/calls; echo \$\$ > $T/cap/serve.pid; spike-vdec >/dev/null 2>&1 & fi
if [ "\$1" = mcu ]; then read -r l; printf %s "\$l" > $T/cap/mcu-stdin; fi
$LOOP
EOF
printf '#!/bin/sh\ntrap "echo spike-vdec stopped >> %s/calls; exit 0" TERM; %s\n' $T "$LOOP" > /stub/spike-vdec
# the MCU's channel: each line it is sent logged; the pairing window's channel (121) reports a pairing request
cat > /stub/mcud <<EOF
#!/bin/sh
[ "\$2" = 121 ] && echo AP123456
while read -r l; do echo "mcud \$*: \$l" >> $T/calls; done
EOF
cat > /etc/init.d/S45wifi-auto <<EOF
#!/bin/sh
m=; { [ -e /run/dash-aa ] || [ -e /run/dash-aa-radio ]; } && m=" (the AA marks still there)"
echo "S45wifi-auto \$*\$m" >> $T/calls
EOF
chmod +x /stub/* /etc/init.d/S45wifi-auto

hooks() {   # the start hook gets the passphrase in its environment, kept aside; the stop hook HANG=1: ignores TERM
	cat > /data/hooks/aa-start <<EOF
#!/bin/sh
echo "hook aa-start IFACE=\$IFACE SSID=\$SSID IP=\$IP" >> $T/calls; printf %s "\$PSK" > $T/cap/hook-psk
EOF
	cat > /data/hooks/aa-stop <<EOF
#!/bin/sh
echo "hook aa-stop IP=\$IP PSK=\${PSK:-}" >> $T/calls; echo \$\$ > $T/cap/hook.pid
[ -n "${HANG:-}" ] && { trap '' TERM; while :; do /bb/sleep 1; done; }
exit 0
EOF
	chmod +x /data/hooks/*
}
fresh() {   # a booted screen: the station's wpa_supplicant running, nothing of AA
	killall -q -9 wpa_supplicant aa spike-vdec udhcpd mcud
	while pidof wpa_supplicant aa spike-vdec udhcpd mcud >/dev/null; do /bb/usleep 50000; done
	rm -rf /run/aa-* /run/dash-* /run/wifi-auto.* /data /sys/class/net/ap0 $T/cap $T/ap0dead $T/noap0 $T/nosta
	mkdir -p /data/log /data/hooks /data/wifi $T/cap; hooks
	/stub/wpa_supplicant --run sta & STA=$!
	: > $T/calls; : > $T/out; shown=
}
run() { /bb/sh "$D" "$@" >> $T/out 2>&1; R=$?; /bb/usleep 300000; return $R; }   # (its daemons a moment to settle)
has() { grep -qxF -- "$1" $T/calls; }
at() { grep -nxF -- "$1" $T/calls | head -n 1 | cut -d: -f1; }
before() { a=$(at "$1") b=$(at "$2"); [ -n "$a" ] && [ -n "$b" ] && [ "$a" -lt "$b" ]; }
up() { [ -e /run/dash-aa ] && [ -e /run/dash-aa-radio ] && [ -e /data/aa-on ]; }
down() { ! [ -e /run/dash-aa ] && ! [ -e /run/dash-aa-radio ] && ! [ -e /data/aa-on ] && ! [ -e /run/aa-ap.iface ] &&
	! [ -e /run/aa-ap.conf ] && ! [ -e /run/aa-ap.pid ] && ! [ -e /sys/class/net/ap0 ] && ! pidof aa spike-vdec udhcpd mcud >/dev/null; }
psk() { grep '^	psk="' $T/cap/ap.conf | cut -d'"' -f2; }
secret() {   # the passphrase: 20 hex digits; on aa mcu's stdin and in the hook's environment, in no command line or log
	P=$(psk)
	echo "$P" | grep -qxE '[0-9a-f]{20}' && [ "$(cat $T/cap/ap.mode)" = 600 ] && ok "passphrase: 20 hex digits, its file 600" ||
		no "passphrase '$P', mode $(cat $T/cap/ap.mode)"
	[ "$(cat $T/cap/mcu-stdin 2>/dev/null)" = "$P" ] && ok "aa mcu got the passphrase on its stdin" || no "aa mcu's stdin"
	[ "$(cat $T/cap/hook-psk 2>/dev/null)" = "$P" ] && ok "the aa-start hook got it in its environment" || no "hook's PSK"
	for f in /proc/[0-9]*/cmdline; do tr '\0' ' ' < $f; echo; done > $T/cmdlines 2>/dev/null
	! grep -qF "$P" $T/cmdlines $T/calls $T/out && [ "$(grep -rlF "$P" /run /data /tmp/*.log 2>/dev/null)" = /run/aa-ap.conf ] &&
		ok "the passphrase in no command line or log, in /run, /data and the logs only the AP's config" ||
		no "the passphrase leaks: $({ grep -lF "$P" $T/cmdlines $T/calls $T/out; grep -rlF "$P" /run /data /tmp/*.log; } 2>/dev/null | tr '\n' ' ')"
}
nocb() { ! grep -q 'AT#CB' $T/calls && ok "never AT#CB" || no "AT#CB sent"; }

echo "-- start: the AP on ap0 beside the station; the pairing window"
fresh; echo de > /data/wifi/country
run start && up && ok "start: up, the marks written" || no "start ($?)"
has 'wpa_cli -i wlan0 disconnect' && has 'iw reg set DE' && has 'iw dev wlan0 interface add ap0 type __ap' &&
	grep -q '^wpa_supplicant -B -i ap0 -D nl80211 -c /run/aa-ap.conf -P /run/aa-ap.pid ' $T/calls &&
	kill -0 $STA && ! grep -q 'wpa_supplicant sta stopped' $T/calls &&
	ok "the AP on ap0, the station kept (its link dropped), the country set" || no "ap0"
for l in ctrl_interface=/run/aa-ap ap_isolate=1 '	mode=2' '	ssid=576174746c696e654141' '	frequency=5180' \
	'	key_mgmt=WPA-PSK' '	proto=RSN' '	pairwise=CCMP'; do grep -qxF "$l" $T/cap/ap.conf || no "AP config: $l"; done
has 'ip addr add 192.168.53.1/24 dev ap0' && grep -qx 'interface ap0' /run/aa-udhcpd.conf &&
	has 'udhcpd -f -a 100 /run/aa-udhcpd.conf' && [ "$(cat /run/aa-ap.iface)" = ap0 ] &&
	ok "the AP's address and DHCP on ap0" || no "address/DHCP"
before 'hook aa-start IFACE=ap0 SSID=WattlineAA IP=192.168.53.1' 'aa serve' && before \
	'hook aa-start IFACE=ap0 SSID=WattlineAA IP=192.168.53.1' 'aa mcu 192.168.53.1 WattlineAA - 02:aa:bb:cc:dd:01' &&
	ok "the hook, then aa serve and aa mcu (the passphrase as -)" || no "hook/aa serve/aa mcu"
has 'mcud ctl 121: AT#CA' && has 'mcud ctl 1: AT#KA' && ok "the pairing window: AT#CA, the request accepted (AT#KA)" ||
	no "pairing window"
secret
has 'aa listens on 192.168.53.1' && ok "the receiver listens on the AP's address only" || no "AA_LISTEN"
# the receiver dying mid-session (killed, as a crash): its markers go (the split let go) and it is started again
: > /run/dash-project; : > /run/dash-aa-session
kill -9 "$(cat $T/cap/serve.pid)"
i=0; while [ "$(grep -c '^aa serve$' $T/calls)" -lt 2 ] && [ $i -lt 40 ]; do /bb/usleep 100000; i=$((i + 1)); done
[ "$(grep -c '^aa serve$' $T/calls)" = 2 ] && ! [ -e /run/dash-project ] && ! [ -e /run/dash-aa-session ] &&
	ok "a receiver that died: its markers gone, started again" || no "receiver restart"
W=$(cat /run/aa-pair.pid)
run start; [ "$(grep -c '^wpa_supplicant -B' $T/calls)" = 1 ] && ok "a second start while up: nothing" || no "second start"
echo "-- its stop"
run stop && down && kill -0 $STA && ! kill -0 "$W" 2>/dev/null && ok "stop: down, the window ended, the station kept" ||
	no "stop"
before 'aa serve stopped' "wpa_cli -p /run/aa-ap -i ap0 all_sta" && has 'spike-vdec stopped' &&
	before 'hook aa-stop IP=192.168.53.1 PSK=' "wpa_cli -p /run/aa-ap -i ap0 all_sta" &&
	ok "aa, the decoder and the hook before the AP" || no "stop's start"
before "wpa_cli -p /run/aa-ap -i ap0 deauthenticate $STA1" "wpa_cli -p /run/aa-ap -i ap0 deauthenticate $STA2" &&
	before "wpa_cli -p /run/aa-ap -i ap0 deauthenticate $STA2" 'usleep 2000000' &&
	before 'usleep 2000000' 'wpa_supplicant ap:ap0 stopped' && ok "each phone deauthenticated, 2 s, then the AP gone" ||
	no "deauthentication"
before 'wpa_supplicant ap:ap0 stopped' 'ip addr del 192.168.53.1/24 dev ap0' && before 'ip addr del 192.168.53.1/24 dev ap0' \
	'iw dev ap0 del' && before 'iw dev ap0 del' 'wpa_cli -i wlan0 reconnect' && ! grep -q S45wifi-auto $T/calls &&
	ok "its address and ap0 gone, then the station's link back" || no "AP down/station back"
nocb

echo "-- restore, ap0 refused: the AP on wlan0, the station stopped, no window; a settings save applied at stop"
fresh; : > $T/noap0; : > $T/nosta
run restore && up && ok "restore: up" || no "restore ($?)"
before 'wpa_supplicant sta stopped' 'wpa_supplicant -B -i wlan0 -D nl80211 -c /run/aa-ap.conf -P /run/aa-ap.pid -f /tmp/aa-ap.log' &&
	has 'ip addr add 192.168.53.1/24 dev wlan0' && [ "$(cat /run/aa-ap.iface)" = wlan0 ] &&
	has 'aa mcu 192.168.53.1 WattlineAA - 02:00:00:00:00:01' && ok "the station stopped, the AP on wlan0" || no "wlan0"
! grep -q '^mcud' $T/calls && ! [ -e /run/aa-pair.pid ] && ok "no pairing window" || no "a window on restore"
secret
echo Home > /run/wifi-auto.pending
rm /data/hooks/aa-stop; HANG=1 hooks
run stop && down && ! [ -e /run/wifi-auto.pending ] && ok "stop: down" || no "stop"
p=$(cat $T/cap/hook.pid); ! kill -0 $p 2>/dev/null && has 'aa serve stopped' && ok "a hanging aa-stop hook: killed" ||
	no "the hook's limit"
before 'ip addr del 192.168.53.1/24 dev wlan0' 'S45wifi-auto start Home' && ! grep -q 'usleep 2000000' $T/calls &&
	! has 'wpa_cli -i wlan0 reconnect' && ok "no phone, no wait; the save made during AA applied, after the marks" ||
	no "stop on wlan0"
nocb

echo "-- start, ap0 made but its AP does not come up: wlan0 instead"
fresh; : > $T/ap0dead
run start && up && grep -q 'no AP beside the station' $T/out && before 'iw dev ap0 del' 'wpa_supplicant sta stopped' &&
	before 'wpa_supplicant ap:ap0 stopped' 'wpa_supplicant -B -i wlan0 -D nl80211 -c /run/aa-ap.conf -P /run/aa-ap.pid -f /tmp/aa-ap.log' &&
	[ "$(cat /run/aa-ap.iface)" = wlan0 ] && ok "ap0 removed, the AP on wlan0" || no "the fallback"
run stop && down && has 'S45wifi-auto start' && ok "stop: the station back" || no "stop"
nocb

echo "-- aa-solo: the AP on wlan0, ap0 not tried"
fresh; : > /data/wifi/aa-solo
run start && up && ! grep -q 'interface add' $T/calls && [ "$(cat /run/aa-ap.iface)" = wlan0 ] && ok "start" || no "aa-solo"
run stop && down && has 'S45wifi-auto start' && ok "stop" || no "stop"
nocb

echo "-- a passphrase kept on the unit is the AP's; one that is not 8 to 63 letters and digits is passed over"
fresh; mkdir -p /data/wattline; printf '0123456789abcde\n' > /data/wattline/ap-psk
run start && up && [ "$(psk)" = 0123456789abcde ] && [ "$(cat $T/cap/mcu-stdin 2>/dev/null)" = 0123456789abcde ] &&
	ok "the kept passphrase, to the AP and to aa mcu" || no "the kept passphrase: '$(psk)'"
run stop && down && ok "stop" || no "stop"
fresh; mkdir -p /data/wattline; printf 'a"b c\\d\n' > /data/wattline/ap-psk
run start && up && psk | grep -qxE '[0-9a-f]{20}' && ok "other characters: a fresh one" || no "a kept one of other characters: '$(psk)'"
run stop && down && ok "stop" || no "stop"
fresh; mkdir -p /data/wattline; printf 'short\n' > /data/wattline/ap-psk
run start && up && psk | grep -qxE '[0-9a-f]{20}' && ok "too short: a fresh one" || no "a short kept one: '$(psk)'"
run stop && down && ok "stop" || no "stop"
nocb

echo "-- a stop in a start's last moments (the split's Exit): all of it down, and not on at the next start"
# the start held just before it writes /data/aa-on; the stop's first sync made slow, so the start goes on inside it
cat > /stub/touch <<EOT
#!/bin/sh
[ "\$1" = /data/aa-on ] && [ -e $T/hold ] && { : > $T/touchwait; while ! [ -e $T/go ]; do /bb/usleep 50000; done; }
exec /bb/touch "\$@"
EOT
cat > /stub/sync <<EOT
#!/bin/sh
[ -e $T/slow ] && { rm -f $T/slow; /bb/sleep 1.5; }
exec /bb/sync
EOT
chmod +x /stub/touch /stub/sync
fresh; : > $T/hold; rm -f $T/go $T/touchwait
/bb/sh "$D" start >> $T/out 2>&1 & SP=$!
i=0; while ! [ -e $T/touchwait ] && [ $i -lt 200 ]; do /bb/usleep 50000; i=$((i + 1)); done
: > $T/slow
/bb/sh "$D" stop >> $T/out 2>&1 & TP=$!
/bb/usleep 500000; : > $T/go
wait $SP; wait $TP; /bb/usleep 300000
down && ! [ -e /run/aa-keep ] && ok "the start's marks taken back: nothing left up, nor on at the next start" || no "a stop during a start"
rm -f /stub/touch /stub/sync $T/hold $T/go $T/touchwait
kill $STA 2>/dev/null   # (the station this case kept: left running, it holds a pipe this test's output goes through)

[ $bad -eq 0 ] && echo "dash-aa: ok" || { echo "dash-aa: $bad failed"; exit 1; }
