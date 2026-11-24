#!/bin/sh
# The scripts that must go on without their log (the F133's S05dash, S05wifi, S03mcu, S45wifi-auto and dash-aa; the
# T113's dash-supervise and dash-aa): each fallback line of theirs, tried here in busybox ash, the screens' shell, with
# a log that cannot be opened. ash ends on a ":" whose redirection fails, in braces too, and does not run a command
# whose redirection fails; the tests' own shell (dash) differs: only a subshell takes it. From the root, with busybox
# installed:
#   sh os-f133/test/test_log_fallback.sh
R=$(cd "$(dirname "$0")/../.." && pwd)
F=os-f133/board/f133/overlay
bad=0
for f in $F/etc/init.d/S05dash $F/etc/init.d/S05wifi $F/etc/init.d/S03mcu $F/etc/init.d/S45wifi-auto $F/usr/sbin/dash-aa \
	os/board/dash/rootfs_overlay_r3/usr/sbin/dash-supervise os/board/dash/rootfs_overlay_r3/etc/init.d/S45wifi-auto \
	os/board/dash/rootfs_overlay_r4/usr/sbin/dash-aa; do
	grep -o '( : >> [^)]* ) 2>/dev/null || [A-Z]*=[a-z/.]*' $R/$f > /tmp/fallbacks
	[ -s /tmp/fallbacks ] || { echo "FAIL  $f: no fallback line"; bad=$((bad + 1)); continue; }
	while read -r l; do
		v=${l##*|| }; v=${v%%=*}
		# its variable an unwritable path (a folder or a file), then the line itself, then on
		r=$(busybox ash -c "$v=/proc/none; W=/proc/none/w; set -- x; $l; echo went-on \$$v" 2>&1)
		case "$r" in "went-on /dev/null" | "went-on /tmp" | "went-on /run" | "went-on /run/dash.out") echo "ok    $f: goes on without its log ($v)" ;;
		*) echo "FAIL  $f: '$r'"; bad=$((bad + 1)) ;; esac
	done < /tmp/fallbacks
	# and no ":" in a command's place redirected to a file outside a subshell (/run and /tmp, RAM, are always there)
	grep -nE '(^|[;{]|&&|\|\||then|do|else)[[:space:]]*:[[:space:]]*[0-9]*>' $R/$f | grep -v '^[0-9]*:[[:space:]]*#' |
		grep -v ':[[:space:]]*>>*[[:space:]]*/run/\|:[[:space:]]*>>*[[:space:]]*/tmp/' &&
		{ echo "FAIL  $f: a ':' redirected outside a subshell ends ash when the file cannot be opened"; bad=$((bad + 1)); }
done
[ $bad = 0 ] && echo "log fallback: ok" || { echo "log fallback: $bad failed"; exit 1; }
