#!/bin/sh
# mk-ota.sh IMAGES KEY OUT: a release's update assets, for dash-update on the Dashes in the field.
#   IMAGES  a build's images folder (zImage, dash-r0.dtb, rootfs.cpio.uboot)
#   KEY     the project's Ed25519 private key (PEM). It never goes into the repo or a release: only its public half
#           is in the image (board/dash/wattline-ota.pub.raw -> /etc/wattline-ota.pub.raw)
#   OUT     the release's upload folder: gets wattline-ota-<version>.tar.gz, then SHA256SUMS over every file in it
#           (put sdcard.img.xz etc. there first) and SHA256SUMS.sig
# The version is the image's own (/etc/image-id inside rootfs.cpio.uboot), so a Dash that installs it reports that
# version afterwards and does not install it again. Needs: openssl, cpio, gzip, sha256sum.
set -e
I=$1; K=$2; OUT=$(mkdir -p "$3" && cd "$3" && pwd)
[ -s "$I/zImage" ] && [ -s "$I/dash-r0.dtb" ] && [ -s "$I/rootfs.cpio.uboot" ] || { echo "mk-ota: $I lacks the three boot files" >&2; exit 1; }
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
# rootfs.cpio.uboot: a 64-byte U-Boot header over the gzip'd cpio
dd if="$I/rootfs.cpio.uboot" bs=64 skip=1 2>/dev/null | gzip -dc | (cd "$T" && cpio -id --quiet etc/image-id)
V=$(cut -d- -f1 "$T/etc/image-id")
case "$V" in [0-9]*.[0-9]*.[0-9]*) ;; *) echo "mk-ota: image-id '$(cat "$T/etc/image-id")' has no version" >&2; exit 1 ;; esac
# what U-Boot's load addresses leave room for (dash-update refuses more)
[ "$(wc -c < "$I/zImage")" -le 8388608 ] && [ "$(wc -c < "$I/dash-r0.dtb")" -le 1048576 ] &&
	[ "$(wc -c < "$I/rootfs.cpio.uboot")" -le 100663296 ] || { echo "mk-ota: a boot file is too big" >&2; exit 1; }
# the key must be the one the image trusts
openssl pkey -in "$K" -pubout -outform DER | tail -c 32 | cmp -s - "$(dirname "$0")/wattline-ota.pub.raw" ||
	{ echo "mk-ota: $K is not the key of board/dash/wattline-ota.pub.raw" >&2; exit 1; }
mkdir -p "$T/b"
cp "$I/zImage" "$I/dash-r0.dtb" "$I/rootfs.cpio.uboot" "$T/b/"; echo "$V" > "$T/b/version"
B=wattline-ota-$V.tar.gz
(cd "$T/b" && tar -czf "$OUT/$B" zImage dash-r0.dtb rootfs.cpio.uboot version)
(cd "$OUT" && rm -f SHA256SUMS SHA256SUMS.sig && sha256sum $(ls | grep -v '^SHA256SUMS') > "$T/sums" && mv "$T/sums" SHA256SUMS &&
	openssl pkeyutl -sign -inkey "$K" -rawin -in SHA256SUMS -out SHA256SUMS.sig)
echo "mk-ota: $OUT/$B (version $V), SHA256SUMS, SHA256SUMS.sig"
