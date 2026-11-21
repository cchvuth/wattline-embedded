#!/bin/sh
# The scripts that must go on without their log (S05dash's dash, S03mcu's mcud, S45wifi-auto's join, dash-aa's
# session): each has one fallback line, tried here in busybox ash, the screen's shell, with a log that cannot be opened.
# ash ends on a ":" whose redirection fails, in braces too, and the tests' own shell (dash) does not: only a subshell
# takes it. From the root, with busybox installed:
#   sh os-f133/test/test_log_fallback.sh
O=$(cd "$(dirname "$0")/.." && pwd)/board/f133/overlay
bad=0
for f in etc/init.d/S05dash etc/init.d/S03mcu etc/init.d/S45wifi-auto usr/sbin/dash-aa; do
	l=$(grep -o '( : >> [^)]* ) 2>/dev/null || [A-Z]*=[a-z/]*' $O/$f)
	v=${l##*|| }; v=${v%%=*}
	if [ "$(printf '%s\n' "$l" | grep -c .)" != 1 ]; then echo "FAIL  $f: not one fallback line"; bad=$((bad + 1)); continue; fi
	# its variable an unwritable path (LOG is a folder, the others a file), then the line itself, then on
	r=$(busybox ash -c "$v=/proc/none; W=/proc/none/w; $l; echo went-on \$$v" 2>&1)
	case "$r" in "went-on /dev/null" | "went-on /tmp") echo "ok    $f: goes on without its log" ;;
	*) echo "FAIL  $f: '$r'"; bad=$((bad + 1)) ;; esac
	grep -n '^[^#]*{ : >' $O/$f && { echo "FAIL  $f: a ':' redirected in braces ends ash"; bad=$((bad + 1)); }
done
[ $bad = 0 ] && echo "log fallback: ok" || { echo "log fallback: $bad failed"; exit 1; }
