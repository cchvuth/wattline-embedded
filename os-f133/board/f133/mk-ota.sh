#!/bin/sh
# mk-ota.sh IMAGES KEY OUT: a release's F133 update assets, for dash-update on the screens in the field (the T113
# Dash's mk-ota.sh, for the F133's files).
#   IMAGES  a build's images (build.sh's out-f133/: Image, f133.dtb, rootfs.sqfs, kernel.itb, U-Boot)
#   KEY     the project's Ed25519 private key (PEM). It never goes into a repo or a release: only its public half is in
#           the image (board/f133/wattline-ota.pub.raw -> /etc/wattline-ota.pub.raw)
#   OUT     the release's upload folder: gets wattline-f133-ota-<version>.tar.gz, then SHA256SUMS over every file in it
#           (put other assets there first) and SHA256SUMS.sig
# The version is the image's own (/etc/image-id inside rootfs.sqfs), so a screen that installs it reports that version
# afterwards and does not install it again. Needs: openssl, unsquashfs (squashfs-tools 4.4+: -cat), gzip, sha256sum.
set -e
I=$1; K=$2; OUT=$(mkdir -p "$3" && cd "$3" && pwd)
for f in Image f133.dtb rootfs.sqfs kernel.itb u-boot-sunxi-with-spl.bin; do
	[ -s "$I/$f" ] || { echo "mk-ota: $I has no $f" >&2; exit 1; }
done
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
V=$(unsquashfs -cat "$I/rootfs.sqfs" etc/image-id | cut -d- -f1)
case "$V" in [0-9]*.[0-9]*.[0-9]*) ;; *) echo "mk-ota: rootfs.sqfs's image-id has no version ('$V')" >&2; exit 1 ;; esac
[ -z "$(unsquashfs -cat "$I/rootfs.sqfs" etc/image-local 2>/dev/null)" ] ||
	{ echo "mk-ota: built with private additions (post-build.sh: /etc/image-local): a release comes from a clean clone" >&2; exit 1; }
# a screen that installs it must be able to take the release after it, and to judge a tried boot: an image from a
# stale build folder without the updater's parts would strand it on this version
unsquashfs -l "$I/rootfs.sqfs" > "$T/files"
for f in usr/sbin/dash-update usr/bin/sig-verify usr/bin/curl etc/wattline-ota.pub.raw etc/init.d/S01try etc/init.d/S98try; do
	grep -qx "squashfs-root/$f" "$T/files" || { echo "mk-ota: rootfs.sqfs has no /$f" >&2; exit 1; }
done
unsquashfs -cat "$I/rootfs.sqfs" etc/wattline-ota.pub.raw | cmp -s - "$(dirname "$0")/wattline-ota.pub.raw" ||
	{ echo "mk-ota: the image's /etc/wattline-ota.pub.raw is not board/f133/wattline-ota.pub.raw" >&2; exit 1; }
# what U-Boot's load addresses leave room for (dash-update refuses more)
[ "$(wc -c < "$I/Image")" -le 33554432 ] && [ "$(wc -c < "$I/f133.dtb")" -le 1048576 ] &&
	[ "$(wc -c < "$I/rootfs.sqfs")" -le 268435456 ] || { echo "mk-ota: a boot file is too big" >&2; exit 1; }
# and the built-in flash's partitions (a card install copied there: dash-nor)
[ "$(wc -c < "$I/rootfs.sqfs")" -le 10747904 ] && [ "$(wc -c < "$I/kernel.itb")" -le 4456448 ] &&
	[ "$(wc -c < "$I/u-boot-sunxi-with-spl.bin")" -le 1048576 ] || { echo "mk-ota: too big for the built-in flash" >&2; exit 1; }
# the key must be the one the image trusts
openssl pkey -in "$K" -pubout -outform DER | tail -c 32 | cmp -s - "$(dirname "$0")/wattline-ota.pub.raw" ||
	{ echo "mk-ota: $K is not the key of board/f133/wattline-ota.pub.raw" >&2; exit 1; }
mkdir -p "$T/b/nor"
cp "$I/Image" "$I/f133.dtb" "$I/rootfs.sqfs" "$T/b/"; cp "$I/kernel.itb" "$I/u-boot-sunxi-with-spl.bin" "$T/b/nor/"
echo "$V" > "$T/b/version"
B=wattline-f133-ota-$V.tar.gz
(cd "$T/b" && tar -czf "$OUT/$B" Image f133.dtb rootfs.sqfs nor/kernel.itb nor/u-boot-sunxi-with-spl.bin version)
(cd "$OUT" && rm -f SHA256SUMS SHA256SUMS.sig && sha256sum $(ls | grep -v '^SHA256SUMS') > "$T/sums" && mv "$T/sums" SHA256SUMS &&
	openssl pkeyutl -sign -inkey "$K" -rawin -in SHA256SUMS -out SHA256SUMS.sig)
echo "mk-ota: $OUT/$B (version $V), SHA256SUMS, SHA256SUMS.sig"
