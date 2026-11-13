#!/bin/sh
# Cross-build the BlueZ transport, ble_probe and test_ble for the Dash (make -f ble.mk arm) in the Buildroot container;
# the toolchain and sysroot are read from the dashbr volume. Output: build/arm-ble/{ble_probe,test_ble}(.stripped)
exec docker run --rm -v dashbr:/work:ro -v "$HOME/projs/dash-native":/native -w /native debian:stable sh -c "apt-get update -qq >/dev/null && apt-get install -y -qq make >/dev/null 2>&1; make -f ble.mk ${1:-arm}"
