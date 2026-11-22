#!/bin/sh
# The image's identity, /etc/image-id (<version>-<UTC build time>): the dash's settings show the version from it, and
# dash-update compares it with the releases'. With the OTA package (updates for the card install): the release key's
# public half (32 raw bytes), what dash-update checks a release's SHA256SUMS.sig against (the T113 Dash's key too).
set -e
B=$BR2_EXTERNAL_F133_PATH/board/f133
echo "$(cat "$B/version")-$(date -u +%Y%m%d-%H%M%S)" > "$TARGET_DIR/etc/image-id"
if grep -q '^BR2_PACKAGE_DASH_OTA=y' "$BR2_CONFIG"; then
	install -D -m 0644 "$B/wattline-ota.pub.raw" "$TARGET_DIR/etc/wattline-ota.pub.raw"
else
	rm -f "$TARGET_DIR/etc/wattline-ota.pub.raw" "$TARGET_DIR/usr/bin/sig-verify" "$TARGET_DIR/etc/init.d/S99ota" \
		"$TARGET_DIR/usr/sbin/dash-update"
fi
# files the overlay no longer has (Buildroot never deletes a file from the target): the old SSH key copier
# (S50dropbear does it now), the dash's and the Wi-Fi driver's scripts under their old places (S05dash, S05wifi now)
rm -f "$TARGET_DIR/etc/init.d/S49sshkeys" "$TARGET_DIR/etc/init.d/S99dash" "$TARGET_DIR/etc/init.d/S40wifi" \
	"$TARGET_DIR/etc/init.d/S04wifi"
# D-Bus and BlueZ (the dash's Commander link over Bluetooth LE) after the Wi-Fi's join (S45wifi-auto, a fixed name: the
# dash runs it): the car's data comes over the Wi-Fi first, and their cold start from the flash held the join up
I=$TARGET_DIR/etc/init.d
if [ -f $I/S30dbus-daemon ]; then mv -f $I/S30dbus-daemon $I/S46dbus-daemon; fi
if [ -f $I/S40bluetoothd ]; then mv -f $I/S40bluetoothd $I/S47bluetoothd; fi
# Daemons with nothing to do here, each a process in 52 MB of RAM: crond (no crontab), klogd (the kernel's log is in
# dmesg already; syslogd keeps the rest's), and the getty on the console (the text console is let go at boot: nothing
# shows it, and no serial port is wired out)
rm -f "$TARGET_DIR/etc/init.d/S50crond" "$TARGET_DIR/etc/init.d/S02klogd"
sed -i '/getty/d' "$TARGET_DIR/etc/inittab"
# wifi-connect's SAE (WPA3) test, done once here: the marker when this wpa_supplicant has sae.c built in
rm -f "$TARGET_DIR/etc/wpa_supplicant.sae"
if grep -q 'SAE: Selecting supported ECC group' "$TARGET_DIR/usr/sbin/wpa_supplicant"; then
	: > "$TARGET_DIR/etc/wpa_supplicant.sae"
fi
# nor.sh (tools/nor), for dash-nor: a card install copied to the built-in flash from the dash's settings
install -D -m 0755 "$BR2_EXTERNAL_F133_PATH/tools/nor/nor.sh" "$TARGET_DIR/usr/sbin/nor.sh"
# /etc/wattline -> /data/wattline (the AA certificate) in the image itself: the dash starts before S06wattline, which
# makes the same link, and looks for the certificate at its start
ln -sfn /data/wattline "$TARGET_DIR/etc/wattline"
# The dash in the image (/usr/lib/dash, package/dashapp): what S05dash runs when the card has none (a screen running
# from the NOR), with the image's own settings
[ -x "$TARGET_DIR/usr/lib/dash/dash" ] && install -D -m 0644 "$B/dash/dash.conf" "$TARGET_DIR/usr/lib/dash/dash.conf"
# Trims for the 16 MiB NOR: files nothing on the screen uses (Buildroot never deletes a file from the target, so also
# the leftovers of packages turned off)
T=$TARGET_DIR F=$T/lib/firmware/aic8800
# the AIC8800D80 here is the "h" chip, rev u02 (its logs): its patch files and its fmac image with Bluetooth stay (the
# driver is built with SDIO_BT, so aic_bsp_main.c loads fmacfwbt_8800d80_h_u02.bin for Wi-Fi and Bluetooth alike); the
# image without Bluetooth, the plain chip's images, the IPC copies and the RF test firmware go
rm -f $F/fmacfw_8800d80_h_u02.bin $F/fmacfw_8800d80_u02.bin $F/fmacfw_8800d80_u02_ipc.bin \
	$F/fmacfw_8800d80_h_u02_ipc.bin $F/fmacfwbt_8800d80_u02.bin $F/lmacfw_rf_8800d80_u02.bin
# BlueZ's interactive tools (turned off in the defconfig)
rm -f $T/usr/bin/bluetoothctl $T/usr/bin/btmon
# readline and ncurses once nothing selects them (bluetoothctl did; wpa_cli then builds without)
grep -q '^BR2_PACKAGE_READLINE=y' "$BR2_CONFIG" || rm -f $T/usr/lib/libreadline.so* $T/usr/lib/libhistory.so*
grep -q '^BR2_PACKAGE_NCURSES=y' "$BR2_CONFIG" || rm -f $T/usr/lib/libncurses.so* $T/usr/lib/libform.so* \
	$T/usr/lib/libmenu.so* $T/usr/lib/libpanel.so*
# curl and the CA bundle once nothing selects them
grep -q '^BR2_PACKAGE_LIBCURL=y' "$BR2_CONFIG" || rm -f $T/usr/bin/curl $T/usr/lib/libcurl.so*
grep -q '^BR2_PACKAGE_CA_CERTIFICATES=y' "$BR2_CONFIG" || rm -rf $T/etc/ssl/certs $T/usr/share/ca-certificates
# OpenSSL's module of retired ciphers (the CA certificates one by one: post-fakeroot.sh)
rm -f $T/usr/lib/ossl-modules/legacy.so
# D-Bus's and GLib's tools for desktops and debugging (the system bus needs dbus-daemon and dbus-uuidgen: S46dbus-
# daemon), curl's wcurl wrapper, and the files only they or development tools read (D-Bus's activation helper:
# post-fakeroot.sh, after its permissions are set)
rm -f $T/usr/bin/dbus-send $T/usr/bin/dbus-monitor $T/usr/bin/dbus-launch $T/usr/bin/dbus-run-session \
	$T/usr/bin/dbus-cleanup-sockets $T/usr/bin/dbus-test-tool $T/usr/bin/dbus-update-activation-environment \
	$T/usr/bin/gio-querymodules $T/usr/libexec/gio-launch-desktop \
	$T/usr/bin/wcurl
rm -rf $T/usr/share/gettext $T/usr/share/glib-2.0 $T/usr/share/xml $T/usr/lib/gio
# libnl's families nothing links (wpa_supplicant and iw use libnl-3 and libnl-genl-3)
rm -f $T/usr/lib/libnl-route-3.so* $T/usr/lib/libnl-nf-3.so* $T/usr/lib/libnl-xfrm-3.so* \
	$T/usr/lib/libnl-idiag-3.so* $T/usr/lib/libnl-cli-3.so*
# glib's introspection and its command-line tools, pcre2's tools
rm -f $T/usr/lib/libgirepository-2.0.so* $T/usr/bin/gi-* $T/usr/bin/gapplication $T/usr/bin/gdbus $T/usr/bin/gio \
	$T/usr/bin/gresource $T/usr/bin/gsettings $T/usr/bin/pcre2test $T/usr/bin/pcre2grep $T/usr/lib/libpcre2-posix.so*
# Private additions, both git-ignored: patches in board/f133/patches-local (BR2_GLOBAL_PATCH_DIR), and
# board/f133/local/post-build.sh, run here with the target's path. A build with either says so in /etc/image-local,
# and mk-ota.sh makes no release of it.
rm -f "$TARGET_DIR/etc/image-local"
L=
if find "$B/patches-local" -name '*.patch' 2>/dev/null | grep -q .; then L=" board/f133/patches-local"; fi
if [ -f "$B/local/post-build.sh" ]; then sh "$B/local/post-build.sh" "$TARGET_DIR"; L="$L board/f133/local"; fi
[ -z "$L" ] || echo "built with$L" > "$TARGET_DIR/etc/image-local"
