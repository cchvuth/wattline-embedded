#!/bin/sh
# Cross-build the Dash binaries in the Buildroot container (toolchain in the dashbr volume). Usage: tools/arm.sh [make target]
exec docker run --rm -v dashbr:/work -v "$HOME/projs/dash-native":/native -w /native debian:stable sh -c "apt-get update -qq >/dev/null && apt-get install -y -qq make >/dev/null 2>&1; make -j8 ${1:-arm}"
