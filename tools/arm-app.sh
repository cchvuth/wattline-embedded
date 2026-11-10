#!/bin/sh
# Cross-build the dash program for the Dash (make -f app.mk arm) in the Buildroot container; toolchain and sysroot are
# read from the dashbr volume. Output: build/arm-app/dash (+ .stripped); with CMDR=1, build/arm-app-cmdr/.
# Usage: tools/arm-app.sh [make args, e.g. CMDR=1]
set -e
cd "$HOME/projs/dash-native"
make -f app.mk build/signals_gen.h   # python on the Mac (the container has none)
exec docker run --rm -v dashbr:/work:ro -v "$HOME/projs/dash-native":/native -w /native debian:stable sh -c "apt-get update -qq >/dev/null && apt-get install -y -qq make >/dev/null 2>&1; make -j8 -f app.mk arm $*"
