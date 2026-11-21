#!/bin/bash
# build.sh [make arguments]: the F133 screen's image with Buildroot 2026.02 in Docker (README.md). The Buildroot tree,
# its downloads and its output stay in a Docker volume (/work); the images land in $OUT (/out, post-image.sh).
#   ./build.sh                  everything (the first run builds the toolchain too: about an hour)
#   ./build.sh linux-rebuild    one package again, then `./build.sh` for the images
#   VOL=... OUT=... APP=...     another volume, images directory, or app checkout
# The dash, the Android Auto receiver and its decoder are built from the app checkout ($APP, mounted at /native):
# generate its build/signals_gen.h first (README.md).
set -euo pipefail
H=$(cd "$(dirname "$0")" && pwd)
APP=${APP:-$(cd "$H/../app" && pwd)}
OUT=${OUT:-$H/../out-f133}
VOL=${VOL:-wattline-f133}
BR=2026.02
mkdir -p "$OUT"
docker image inspect wattline-builder >/dev/null 2>&1 || docker build -t wattline-builder - <<'DF'
FROM debian:stable
RUN apt-get update -qq && apt-get install -y -qq build-essential git wget cpio unzip rsync bc file python3 \
    python3-setuptools libncurses-dev libssl-dev swig bison flex device-tree-compiler >/dev/null && rm -rf /var/lib/apt/lists/*
DF
docker run --rm -v "$VOL":/work -v "$H":/ext:ro -v "$APP":/native:ro -v "$(cd "$OUT" && pwd)":/out wattline-builder bash -c '
set -e
if [ ! -d /work/buildroot ]; then
	wget -q https://buildroot.org/downloads/buildroot-'"$BR"'.tar.gz -O /tmp/br.tar.gz
	mkdir -p /work/buildroot && tar xzf /tmp/br.tar.gz --strip-components=1 -C /work/buildroot && rm /tmp/br.tar.gz
fi
cd /work/buildroot
# at every build: a build folder kept from before went on with its old configuration, whatever the tree's had become
make O=/work/out BR2_EXTERNAL=/ext f133_defconfig >/dev/null
# The initramfs built into the kernel holds only /init (board/f133/init/init.c, static), at the path
# board/f133/initramfs.list names: compiled before the kernel takes it in, so the toolchain comes first
CC=/work/out/host/bin/riscv64-buildroot-linux-gnu-gcc
[ -x $CC ] || make O=/work/out toolchain
mkdir -p /work/out/f133-init && $CC -static -Os -s -Wall -o /work/out/f133-init/init /ext/board/f133/init/init.c
make O=/work/out '"$*"'
'
