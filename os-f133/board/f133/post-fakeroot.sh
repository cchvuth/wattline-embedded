#!/bin/sh
# On the copy of the target the root's image is made from, never the target itself: the CA bundle is one file
# (/etc/ssl/certs/ca-certificates.crt: what curl reads), and the same certificates again one by one
# (/usr/share/ca-certificates, 600 KiB) with their links are for programs that look one up by its hash: none here. The
# build makes the bundle from those files at every run, so they stay in the target.
set -e
T=$1
[ -s "$T/etc/ssl/certs/ca-certificates.crt" ] || exit 0   # (no CA package in this build)
rm -rf "$T/usr/share/ca-certificates"
find "$T/etc/ssl/certs" -mindepth 1 ! -name ca-certificates.crt -delete
