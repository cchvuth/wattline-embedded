# The dash program (app/): make -f app.mk host | test | arm     (arm: in the build container, tools/arm-app.sh)
#   host  build/app-host/dash-host: the loop + the real UI drawn headless, for the bench tests on the Mac
#   test  dash-host against tools/fake_panda.py over loopback for a scripted minute (test/test_app_bench.py), then a
#         drive recorded and replayed through fake_panda.py --replay (test/test_record.py)
#   arm   build/arm-app/dash (+ .stripped) for the Dash
#   CMDR=1 (any target) adds the Commander session (port/linux/ble_bluez.c, GDBus): build/{app-host,arm-app}-cmdr/
#   DEVICE=f133 (any target): the device profile device/$(DEVICE).mk (t113 by default), its outputs under its BUILD
#   (build/f133/...); arm is then the F133's (RISC-V) build. docs/DEVICES.md
DEVICE ?= t113
include device/$(DEVICE).mk
LVGL := third_party/lvgl-9.6.0
LVGL_SRC := $(shell find $(LVGL)/src -name '*.c')
UI_SRC := ui/dash_ui.c $(UI_GEN)/assets.c $(UI_GEN)/aa_icon.c ui/gen/roboto.c
APP_SRC := app/dash.c app/conf.c app/bridge.c app/log.c app/bright.c app/actions.c app/pair.c app/settings.c app/record.c app/wifi.c \
  core/panda.c core/derive.c core/sched.c core/cmdr_msg.c core/pb.c   # cmdr_msg: the actions' labels, every build
APP_H := app/app.h app/actions.h app/pair.h app/settings.h app/record.h core/state.h core/sched.h core/panda.h core/cmdr_msg.h \
  ui/dash_state.h ui/dash_ui.h ui/project_region.h build/signals_gen.h
WARN := -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
CMDR ?= 0
HA := $(BUILD)/app-host
AA := $(BUILD)/arm-app
ifeq ($(CMDR),1)   # the Commander session: port/linux/ble_bluez.c (GDBus) on its own thread, app/cmdr_hook.c in the loop
APP_SRC += app/cmdr_hook.c core/cmdr.c core/sec1.c port/linux/ble_bluez.c third_party/monocypher.c third_party/aes.c
APP_H += app/cmdr_hook.h port/linux/ble_bluez.h core/cmdr.h
CMDR_DEFS := -DDASH_CMDR=1 -DAES256=1
HA := $(BUILD)/app-host-cmdr
AA := $(BUILD)/arm-app-cmdr
endif

build/signals_gen.h: tools/gen_signals.py tools/extra_signals.json $(wildcard ../wattline/lib/signals.dart ../wattline/lib/panda.dart)
	python3 -I tools/gen_signals.py ../wattline build

# --- host (Mac): LVGL and UI objects shared with ui.mk (same flags, same $(BUILD)/host/), the app's with sanitizers
HOST_CC ?= cc
HOST_CFLAGS := -std=gnu11 -O2 -g -DLV_CONF_INCLUDE_SIMPLE -Iport/sim -I$(LVGL) -I$(LVGL)/src
# the profile's, every build (the device's compiler adds DEVICE_CPU)
DEV_FLAGS := $(DEVICE_DEFS) $(EXTRA_DEFS) -I$(UI_GEN)   # EXTRA_DEFS: a one-off build's extra -D (measuring)
$(BUILD)/host/%.o: %.c port/sim/lv_conf.h ui/conf/lv_conf.h port/linux/lv_conf.h
	@mkdir -p $(dir $@); $(HOST_CC) $(HOST_CFLAGS) $(DEV_FLAGS) -c $< -o $@
$(BUILD)/host/ui/%.o: HOST_CFLAGS += $(WARN)
$(BUILD)/host/ui/dash_ui.o: ui/dash_ui.h ui/dash_state.h ui/project_region.h $(UI_GEN)/assets.h $(UI_GEN)/aa_icon.h
SAN := -fsanitize=address,undefined
HOST_GIO_C = $(if $(CMDR_DEFS),$(shell pkg-config --cflags gio-2.0))
HOST_GIO_L = $(if $(CMDR_DEFS),$(shell pkg-config --libs gio-2.0))
$(HA)/%.o: %.c $(APP_H)
	@mkdir -p $(dir $@); $(HOST_CC) $(HOST_CFLAGS) $(DEV_FLAGS) $(WARN) $(SAN) $(CMDR_DEFS) $(HOST_GIO_C) -Ibuild -c $< -o $@
$(HA)/dash-host: $(patsubst %.c,$(HA)/%.o,app/main_host.c $(APP_SRC)) $(patsubst %.c,$(BUILD)/host/%.o,$(UI_SRC) $(LVGL_SRC))
	$(HOST_CC) $(SAN) -o $@ $^ -lm $(HOST_GIO_L)
host: $(HA)/dash-host
test: host
	python3 -I test/test_app_bench.py $(HA)/dash-host
	$(if $(findstring DASH_NO_RECORD=1,$(DEVICE_DEFS)),@echo "test_record: skipped (this profile records no drives)",python3 -I test/test_record.py $(HA)/dash-host)

# --- the device (arm: the Dash's ARM, or the profile's CPU): its Buildroot toolchain (the profile's ARM_CC), LVGL and UI
# objects shared with ui.mk ($(BUILD)/arm-ui/)
ARM_CFLAGS := -std=gnu11 -O2 -DLV_CONF_INCLUDE_SIMPLE -Iui/conf -I$(LVGL) -I$(LVGL)/src
ARM_LIBS := -lm -lpthread
$(BUILD)/arm-ui/%.o: %.c ui/conf/lv_conf.h port/linux/lv_conf.h
	@mkdir -p $(dir $@); $(ARM_CC) $(ARM_CFLAGS) $(DEVICE_CPU) $(DEV_FLAGS) -c $< -o $@
$(BUILD)/arm-ui/ui/%.o: ARM_CFLAGS += $(WARN)
$(BUILD)/arm-ui/ui/dash_ui.o: ui/dash_ui.h ui/dash_state.h ui/project_region.h $(UI_GEN)/assets.h $(UI_GEN)/aa_icon.h
# the toolchain's own pkg-config, beside its gcc (Buildroot's host/bin)
ARM_PKG_CONFIG ?= $(dir $(lastword $(ARM_CC)))pkg-config
ARM_GIO_C = $(if $(CMDR_DEFS),$(shell $(ARM_PKG_CONFIG) --cflags gio-2.0))
ARM_GIO_L = $(if $(CMDR_DEFS),$(shell $(ARM_PKG_CONFIG) --libs gio-2.0))
$(AA)/%.o: %.c $(APP_H)
	@mkdir -p $(dir $@); $(ARM_CC) $(ARM_CFLAGS) $(DEVICE_CPU) $(DEV_FLAGS) $(WARN) $(CMDR_DEFS) $(ARM_GIO_C) -Ibuild -c $< -o $@
$(AA)/dash: $(patsubst %.c,$(AA)/%.o,app/main.c port/linux/fb_rot.c $(APP_SRC)) $(patsubst %.c,$(BUILD)/arm-ui/%.o,$(UI_SRC) $(LVGL_SRC))
	$(ARM_CC) -o $@ $^ $(ARM_LIBS) $(ARM_GIO_L)
	$(ARM_CC:gcc=strip) -o $@.stripped $@
arm: $(AA)/dash

.PHONY: host test arm
