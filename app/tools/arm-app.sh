#!/bin/sh
# Cross-build the dash program for the Dash (make -f app.mk arm) in the Buildroot container; toolchain and sysroot are
# read from the dashbr volume. Output: build/arm-app/dash (+ .stripped); with CMDR=1, build/arm-app-cmdr/.
# Usage: tools/arm-app.sh [make args, e.g. CMDR=1]
set -e
cd "$(dirname "$0")/.."
make -f app.mk build/signals_gen.h   # from the Flutter app if it is there (python on the Mac: the container has none)
exec docker run --rm -v dashbr:/work:ro -v "$(cd "$(dirname "$0")/.." && pwd)":/native -w /native debian:stable sh -c "apt-get update -qq >/dev/null && apt-get install -y -qq make >/dev/null 2>&1; make -j8 -f app.mk arm $*"
