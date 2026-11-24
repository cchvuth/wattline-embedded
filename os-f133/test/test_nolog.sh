#!/bin/sh
# nolog, both screens' (logs off: their log folders a read-only folder of links to /dev/null): the two copies the same;
# every log name the screens' scripts and programs write is in its list (one missing would stop its writer, or land
# in RAM by its fallback); and, with mount rights (CI's privileged container, busybox installed), a folder it makes
# takes those writes into /dev/null, stores nothing, and refuses a name not listed. From the root:
#   sh os-f133/test/test_nolog.sh
R=$(cd "$(dirname "$0")/../.." && pwd)
cd "$R" || exit 1
A=os-f133/board/f133/overlay/usr/sbin/nolog B=os/board/dash/rootfs_overlay_r3/usr/sbin/nolog
O="os-f133/board/f133/overlay os/board/dash/rootfs_overlay_r3 os/board/dash/rootfs_overlay_r4"
bad=0
fail() { echo "FAIL  $*"; bad=$((bad + 1)); }
cmp -s $A $B || fail "the T113's and the F133's nolog differ"
N=" $(sed -n '/^N="/,/"$/p' $A | tr -d '"' | sed 's/^N=//' | tr '\n' ' ') "
# the names: a log folder's variable or path, then the file (lock files, rotations and the hooks' pattern left out:
# the hooks' names come from their calls)
W=$( { grep -rhoE '(\$L|\$LOG|\$HL|/data/log|/var/log/sd)/[A-Za-z0-9._$-]+' $O app/aa/aa.c app/app/main.c os-f133/board/f133/dash/dash.conf |
	sed 's|.*/||' | grep -v -e '^pid$' -e '\$' -e '\.1$' -e '^hook-$'
	grep -rhoE '(^|[;&|[:space:]])hook [a-z]+-[a-z]+' $O | sed 's/.*hook /hook-/; s/$/.log/'; } | sort -u)
[ -n "$W" ] || fail "no log names found"
for w in $W; do
	case "$N" in *" $w "*) echo "ok    $w" ;; *) fail "$w is written but not in nolog's list" ;; esac
done
if [ "$(id -u)" = 0 ] && command -v busybox >/dev/null && mount -t tmpfs none /mnt 2>/dev/null && umount /mnt; then
	D=/tmp/nolog-test
	busybox ash $A $D && busybox ash $A $D || fail "nolog failed (or a second call did)"
	busybox ash -c "echo x >> $D/dash.out && echo x > $D/messages" 2>/dev/null || fail "a listed log could not be written"
	[ -z "$(cat $D/dash.out $D/messages)" ] || fail "a listed log kept what was written"
	busybox ash -c "( : > $D/other.log ) 2>/dev/null" && fail "a name not listed was made"
	grep -q " $D tmpfs ro" /proc/mounts || fail "the folder is not read-only"
	umount $D
	[ $bad = 0 ] && echo "ok    a nolog folder writes nothing"
else
	echo "skip  the folder itself (needs root, mount rights and busybox)"
fi
[ $bad = 0 ] && echo "nolog: ok" || { echo "nolog: $bad failed"; exit 1; }
