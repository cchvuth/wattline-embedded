#!/bin/sh
# Start the dash by hand (until the image runs it): mount the card's data partition (p2, DASHDATA) read-only at
# /run/dashdata for dash.conf if S45wifi-auto has not left it mounted, then run the dash from this script's folder.
# usage: dash-run.sh [dash args]     e.g. from /tmp: sh /tmp/dash-run.sh
M=/run/dashdata
if ! grep -q " $M " /proc/mounts; then
	for d in /sys/block/mmcblk[0-9]; do [ "$(cat $d/device/type 2>/dev/null)" = SD ] && P=/dev/${d##*/}p2; done
	[ -b "$P" ] && mkdir -p $M && mount -t vfat -o ro "$P" $M
fi
exec "$(dirname "$0")/dash" "$@"
