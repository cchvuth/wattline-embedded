# The dash program (app/): make -f app.mk host | test | arm     (arm: in the build container, tools/arm-app.sh)
#   host  build/app-host/dash-host: the loop + the real UI drawn headless, for the bench tests on the Mac
#   test  dash-host against tools/fake_panda.py over loopback for a scripted minute (test/test_app_bench.py), then a
#         drive recorded and replayed through fake_panda.py --replay (test/test_record.py)
#   arm   build/arm-app/dash (+ .stripped) for the Dash
#   CMDR=1 (any target) adds the Commander session (port/linux/ble_bluez.c, GDBus): build/{app-host,arm-app}-cmdr/
LVGL := third_party/lvgl-9.6.0
LVGL_SRC := $(shell find $(LVGL)/src -name '*.c')
UI_SRC := ui/dash_ui.c ui/gen/assets.c ui/gen/roboto.c
APP_SRC := app/dash.c app/conf.c app/bridge.c app/log.c app/bright.c app/actions.c app/pair.c app/settings.c app/record.c app/wifi.c \
  core/panda.c core/derive.c core/sched.c core/cmdr_msg.c core/pb.c   # cmdr_msg: the actions' labels, every build
APP_H := app/app.h app/actions.h app/pair.h app/settings.h app/record.h core/state.h core/sched.h core/panda.h core/cmdr_msg.h \
  ui/dash_state.h ui/dash_ui.h build/signals_gen.h
WARN := -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
CMDR ?= 0
HA := build/app-host
AA := build/arm-app
ifeq ($(CMDR),1)   # the Commander session: port/linux/ble_bluez.c (GDBus) on its own thread, app/cmdr_hook.c in the loop
APP_SRC += app/cmdr_hook.c core/cmdr.c core/sec1.c port/linux/ble_bluez.c third_party/monocypher.c third_party/aes.c
APP_H += app/cmdr_hook.h port/linux/ble_bluez.h core/cmdr.h
CMDR_DEFS := -DDASH_CMDR=1 -DAES256=1
HA := build/app-host-cmdr
AA := build/arm-app-cmdr
endif

build/signals_gen.h: tools/gen_signals.py tools/extra_signals.json $(wildcard ../wattline/lib/signals.dart)
	python3 -I tools/gen_signals.py ../wattline build

# --- host (Mac): LVGL and UI objects shared with ui.mk (same flags, same build/host/), the app's with sanitizers
HOST_CC ?= cc
HOST_CFLAGS := -std=gnu11 -O2 -g -DLV_CONF_INCLUDE_SIMPLE -Iport/sim -I$(LVGL) -I$(LVGL)/src
build/host/%.o: %.c port/sim/lv_conf.h ui/conf/lv_conf.h port/linux/lv_conf.h
	@mkdir -p $(dir $@); $(HOST_CC) $(HOST_CFLAGS) -c $< -o $@
build/host/ui/%.o: HOST_CFLAGS += $(WARN)
build/host/ui/dash_ui.o: ui/dash_ui.h ui/dash_state.h ui/gen/assets.h
SAN := -fsanitize=address,undefined
HOST_GIO_C = $(if $(CMDR_DEFS),$(shell pkg-config --cflags gio-2.0))
HOST_GIO_L = $(if $(CMDR_DEFS),$(shell pkg-config --libs gio-2.0))
$(HA)/%.o: %.c $(APP_H)
	@mkdir -p $(dir $@); $(HOST_CC) $(HOST_CFLAGS) $(WARN) $(SAN) $(CMDR_DEFS) $(HOST_GIO_C) -Ibuild -c $< -o $@
$(HA)/dash-host: $(patsubst %.c,$(HA)/%.o,app/main_host.c $(APP_SRC)) $(patsubst %.c,build/host/%.o,$(UI_SRC) $(LVGL_SRC))
	$(HOST_CC) $(SAN) -o $@ $^ -lm $(HOST_GIO_L)
host: $(HA)/dash-host
test: host
	python3 -I test/test_app_bench.py $(HA)/dash-host
	python3 -I test/test_record.py $(HA)/dash-host

# --- ARM (the Dash): the Buildroot toolchain, LVGL and UI objects shared with ui.mk (build/arm-ui/)
ARM_CC ?= /work/out-r0/host/bin/arm-buildroot-linux-gnueabihf-gcc
ARM_CFLAGS := -std=gnu11 -O2 -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -DLV_CONF_INCLUDE_SIMPLE -Iui/conf -I$(LVGL) -I$(LVGL)/src
ARM_LIBS := -lm -lpthread
build/arm-ui/%.o: %.c ui/conf/lv_conf.h port/linux/lv_conf.h
	@mkdir -p $(dir $@); $(ARM_CC) $(ARM_CFLAGS) -c $< -o $@
build/arm-ui/ui/%.o: ARM_CFLAGS += $(WARN)
build/arm-ui/ui/dash_ui.o: ui/dash_ui.h ui/dash_state.h ui/gen/assets.h
ARM_GIO_C = $(if $(CMDR_DEFS),$(shell /work/out-r0/host/bin/pkg-config --cflags gio-2.0))
ARM_GIO_L = $(if $(CMDR_DEFS),$(shell /work/out-r0/host/bin/pkg-config --libs gio-2.0))
$(AA)/%.o: %.c $(APP_H)
	@mkdir -p $(dir $@); $(ARM_CC) $(ARM_CFLAGS) $(WARN) $(CMDR_DEFS) $(ARM_GIO_C) -Ibuild -c $< -o $@
$(AA)/dash: $(patsubst %.c,$(AA)/%.o,app/main.c $(APP_SRC)) $(patsubst %.c,build/arm-ui/%.o,$(UI_SRC) $(LVGL_SRC))
	$(ARM_CC) -o $@ $^ $(ARM_LIBS) $(ARM_GIO_L)
	$(ARM_CC:gcc=strip) -o $@.stripped $@
arm: $(AA)/dash

.PHONY: host test arm
