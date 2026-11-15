CC ?= cc
HOSTFLAGS = -std=gnu11 -Wall -Wextra -fsanitize=address,undefined

test:
	python3 -I tools/gen_signals.py ../wattline build
	$(CC) $(HOSTFLAGS) -Ibuild -o build/test_decode build/test_decode.c -lm && build/test_decode
	$(CC) $(HOSTFLAGS) -o build/test_panda test/test_panda.c core/panda.c && build/test_panda
	$(CC) $(HOSTFLAGS) -Ibuild -o build/test_derive test/test_derive.c core/derive.c core/sched.c core/panda.c -lm && build/test_derive
	$(CC) $(HOSTFLAGS) -Ibuild -o build/test_bright test/test_bright.c app/bright.c app/conf.c core/derive.c core/sched.c core/panda.c -lm && build/test_bright
	$(CC) $(HOSTFLAGS) -DLV_CONF_INCLUDE_SIMPLE -Iport/sim -Ithird_party/lvgl-9.6.0 -Ithird_party/lvgl-9.6.0/src -Ibuild \
	  -o build/test_settings test/test_settings.c app/settings.c app/conf.c core/derive.c core/sched.c core/panda.c -lm && build/test_settings
	$(CC) $(HOSTFLAGS) -Ibuild -o build/test_wifi test/test_wifi.c app/wifi.c && build/test_wifi
	$(CC) $(HOSTFLAGS) -std=gnu11 -DACTIONS_SELFTEST -DDASH_CMDR=1 -DLV_CONF_INCLUDE_SIMPLE -Iport/sim -Ithird_party/lvgl-9.6.0 \
	  -Ithird_party/lvgl-9.6.0/src -Ibuild -o build/actions_check app/actions.c app/pair.c app/wifi.c core/cmdr_msg.c core/pb.c && build/actions_check
	cd test && $(CC) $(HOSTFLAGS) -DAES256=1 -I../third_party -o ../build/test_sec1 sec1_crypto_check.c ../third_party/aes.c ../third_party/monocypher.c && ../build/test_sec1

.PHONY: test

# --- ARM (the Dash): Buildroot toolchain from the dashbr volume (run inside the build container, see tools/arm.sh)
ARM_CC ?= /work/out-r0/host/bin/arm-buildroot-linux-gnueabihf-gcc
LVGL := third_party/lvgl-9.6.0
ARM_CFLAGS = -O2 -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -DLV_CONF_INCLUDE_SIMPLE -Iport/linux -I$(LVGL) -I$(LVGL)/src
LVGL_SRC := $(shell find $(LVGL)/src -name '*.c' 2>/dev/null)
LVGL_OBJ := $(patsubst %.c,build/arm/%.o,$(LVGL_SRC))
build/arm/%.o: %.c port/linux/lv_conf.h
	@mkdir -p $(dir $@); $(ARM_CC) $(ARM_CFLAGS) -c $< -o $@
build/arm/dash-bringup: build/arm/ui/bringup.o $(LVGL_OBJ)
	$(ARM_CC) -o $@ $^ -lm -lpthread
arm: build/arm/dash-bringup
.PHONY: arm
