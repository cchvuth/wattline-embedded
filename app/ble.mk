# BlueZ transport (port/linux/ble_bluez.c) and the bench probe.
#   make -f ble.mk test   Mac, sanitizers (needs brew glib)
#   make -f ble.mk arm    in the build container: tools/arm-ble.sh
CORE := core/cmdr.c core/cmdr_msg.c core/sec1.c core/pb.c third_party/monocypher.c third_party/aes.c
BLE := port/linux/ble_bluez.c $(CORE)
DEPS := $(BLE) port/linux/ble_bluez.h core/cmdr.h
FLAGS := -std=gnu11 -Wall -Wextra -DAES256=1

HOST_CC ?= cc
test: $(DEPS) test/test_ble.c
	@mkdir -p build
	$(HOST_CC) $(FLAGS) -g -fsanitize=address,undefined -o build/test_ble test/test_ble.c $(BLE) $(shell pkg-config --cflags --libs gio-2.0) && build/test_ble

ARM_CC ?= /work/out-r0/host/bin/arm-buildroot-linux-gnueabihf-gcc
ARM_GIO = $(shell /work/out-r0/host/bin/pkg-config --cflags --libs gio-2.0)
ARM_FLAGS := $(FLAGS) -O2 -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard
build/arm-ble/%: tools/%.c $(DEPS)
	@mkdir -p $(dir $@)
	$(ARM_CC) $(ARM_FLAGS) -o $@ $< $(BLE) $(ARM_GIO)
	$(ARM_CC:gcc=strip) -o $@.stripped $@
build/arm-ble/test_ble: test/test_ble.c $(DEPS)
	@mkdir -p $(dir $@)
	$(ARM_CC) $(ARM_FLAGS) -o $@ $< $(BLE) $(ARM_GIO)
	$(ARM_CC:gcc=strip) -o $@.stripped $@
arm: build/arm-ble/ble_probe build/arm-ble/test_ble

.PHONY: test arm
