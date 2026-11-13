#!/bin/sh
# Cross-build the dash UI for the Dash (make -f ui.mk arm) in the Buildroot container; the toolchain is read from the
# dashbr volume. Output: build/arm-ui/dash-ui (and .stripped). Usage: tools/arm-ui.sh [ui.mk target]
exec docker run --rm -v dashbr:/work -v "$HOME/projs/dash-native":/native -w /native debian:stable sh -c "apt-get update -qq >/dev/null && apt-get install -y -qq make >/dev/null 2>&1; make -j8 -f ui.mk ${1:-arm}"
