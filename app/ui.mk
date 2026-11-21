# The dash UI (ui/): host shots and the Dash binary. make -f ui.mk shots | compare | arm (arm: in the build container, tools/arm-ui.sh)
# DEVICE=f133 (any target): the device profile device/$(DEVICE).mk (t113 by default), its outputs under its BUILD
# (build/f133/shots: the F133's 1920x720 canvas). docs/DEVICES.md
DEVICE ?= t113
include device/$(DEVICE).mk
LVGL := third_party/lvgl-9.6.0
LVGL_SRC := $(shell find $(LVGL)/src -name '*.c')
UI_SRC := ui/dash_ui.c ui/scenes.c $(UI_GEN)/assets.c $(UI_GEN)/aa_icon.c ui/gen/roboto.c
WARN := -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers

# --- host (Mac): headless renderer -> $(BUILD)/shots/<scene>.png
HOST_CC ?= cc
HOST_CFLAGS := -std=gnu11 -O2 -g -DLV_CONF_INCLUDE_SIMPLE -Iport/sim -I$(LVGL) -I$(LVGL)/src
# the profile's, every build (the device's compiler adds DEVICE_CPU)
DEV_FLAGS := $(DEVICE_DEFS) $(EXTRA_DEFS) -I$(UI_GEN)   # EXTRA_DEFS: a one-off build's extra -D (measuring)
$(BUILD)/host/%.o: %.c port/sim/lv_conf.h ui/conf/lv_conf.h port/linux/lv_conf.h
	@mkdir -p $(dir $@); $(HOST_CC) $(HOST_CFLAGS) $(DEV_FLAGS) -c $< -o $@
$(BUILD)/host/ui/%.o $(BUILD)/host/port/sim/%.o: HOST_CFLAGS += $(WARN)
$(BUILD)/host/ui/dash_ui.o $(BUILD)/host/port/sim/shots.o $(BUILD)/host/ui/scenes.o: ui/dash_ui.h ui/dash_state.h ui/scenes.h \
  ui/project_region.h $(UI_GEN)/assets.h $(UI_GEN)/aa_icon.h
$(BUILD)/host/dash-shots: $(patsubst %.c,$(BUILD)/host/%.o,port/sim/shots.c $(UI_SRC) $(LVGL_SRC))
	$(HOST_CC) -o $@ $^ -lm

shots: $(BUILD)/host/dash-shots
	@mkdir -p $(BUILD)/shots
	$(BUILD)/host/dash-shots $(BUILD)/shots

compare: shots   # against the app's dash scenes at the Dash's canvas (tools/app_goldens.sh): the T113's
	python3 -I tools/compare_shots.py $(BUILD)/shots test/goldens $(BUILD)/shots/compare

# --- the device (arm: the Dash's ARM, or the profile's CPU): the profile's Buildroot toolchain, run inside the build
# container (tools/arm-ui.sh)
ARM_CFLAGS := -std=gnu11 -O2 -DLV_CONF_INCLUDE_SIMPLE -Iui/conf -I$(LVGL) -I$(LVGL)/src
$(BUILD)/arm-ui/%.o: %.c ui/conf/lv_conf.h port/linux/lv_conf.h
	@mkdir -p $(dir $@); $(ARM_CC) $(ARM_CFLAGS) $(DEVICE_CPU) $(DEV_FLAGS) -c $< -o $@
$(BUILD)/arm-ui/ui/%.o: ARM_CFLAGS += $(WARN)
$(BUILD)/arm-ui/ui/dash_ui.o $(BUILD)/arm-ui/ui/main_dash.o $(BUILD)/arm-ui/ui/scenes.o: ui/dash_ui.h ui/dash_state.h \
  ui/scenes.h ui/project_region.h $(UI_GEN)/assets.h $(UI_GEN)/aa_icon.h
$(BUILD)/arm-ui/dash-ui: $(patsubst %.c,$(BUILD)/arm-ui/%.o,ui/main_dash.c $(UI_SRC) $(LVGL_SRC))
	$(ARM_CC) -o $@ $^ -lm -lpthread
	$(ARM_CC:gcc=strip) -o $@.stripped $@
arm: $(BUILD)/arm-ui/dash-ui

.PHONY: shots compare arm

# The app/ files this UI work adds (not yet in app.mk): compile-only, both ways, with the Dash's compiler.
check-app:
	$(ARM_CC) $(ARM_CFLAGS) $(DEVICE_CPU) $(DEV_FLAGS) $(WARN) -Ibuild -fsyntax-only app/pair.c
	$(ARM_CC) $(ARM_CFLAGS) $(DEVICE_CPU) $(DEV_FLAGS) $(WARN) -Ibuild -fsyntax-only app/actions.c
	$(ARM_CC) $(ARM_CFLAGS) $(DEVICE_CPU) $(DEV_FLAGS) $(WARN) -Ibuild -DDASH_CMDR=1 -DAES256=1 -fsyntax-only app/actions.c
.PHONY: check-app
