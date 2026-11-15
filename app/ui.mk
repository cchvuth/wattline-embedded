# The dash UI (ui/): host shots and the Dash binary. make -f ui.mk shots | compare | arm (arm: in the build container, tools/arm-ui.sh)
LVGL := third_party/lvgl-9.6.0
LVGL_SRC := $(shell find $(LVGL)/src -name '*.c')
UI_SRC := ui/dash_ui.c ui/scenes.c ui/gen/assets.c ui/gen/aa_icon.c ui/gen/roboto.c
WARN := -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers

# --- host (Mac): headless renderer -> build/shots/<scene>.png
HOST_CC ?= cc
HOST_CFLAGS := -std=gnu11 -O2 -g -DLV_CONF_INCLUDE_SIMPLE -Iport/sim -I$(LVGL) -I$(LVGL)/src
build/host/%.o: %.c port/sim/lv_conf.h ui/conf/lv_conf.h port/linux/lv_conf.h
	@mkdir -p $(dir $@); $(HOST_CC) $(HOST_CFLAGS) -c $< -o $@
build/host/ui/%.o build/host/port/sim/%.o: HOST_CFLAGS += $(WARN)
build/host/ui/dash_ui.o build/host/port/sim/shots.o build/host/ui/scenes.o: ui/dash_ui.h ui/dash_state.h ui/scenes.h ui/gen/assets.h ui/gen/aa_icon.h
build/host/dash-shots: $(patsubst %.c,build/host/%.o,port/sim/shots.c $(UI_SRC) $(LVGL_SRC))
	$(HOST_CC) -o $@ $^ -lm

shots: build/host/dash-shots
	@mkdir -p build/shots
	build/host/dash-shots build/shots

compare: shots
	python3 -I tools/compare_shots.py build/shots test/goldens build/shots/compare

# --- ARM (the Dash): the Buildroot toolchain, run inside the build container (tools/arm-ui.sh)
ARM_CC ?= /work/out-r0/host/bin/arm-buildroot-linux-gnueabihf-gcc
ARM_CFLAGS := -std=gnu11 -O2 -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -DLV_CONF_INCLUDE_SIMPLE -Iui/conf -I$(LVGL) -I$(LVGL)/src
build/arm-ui/%.o: %.c ui/conf/lv_conf.h port/linux/lv_conf.h
	@mkdir -p $(dir $@); $(ARM_CC) $(ARM_CFLAGS) -c $< -o $@
build/arm-ui/ui/%.o: ARM_CFLAGS += $(WARN)
build/arm-ui/ui/dash_ui.o build/arm-ui/ui/main_dash.o build/arm-ui/ui/scenes.o: ui/dash_ui.h ui/dash_state.h ui/scenes.h ui/gen/assets.h ui/gen/aa_icon.h
build/arm-ui/dash-ui: $(patsubst %.c,build/arm-ui/%.o,ui/main_dash.c $(UI_SRC) $(LVGL_SRC))
	$(ARM_CC) -o $@ $^ -lm -lpthread
	$(ARM_CC:gcc=strip) -o $@.stripped $@
arm: build/arm-ui/dash-ui

.PHONY: shots compare arm

# The app/ files this UI work adds (not yet in app.mk): compile-only, both ways, with the Dash's compiler.
check-app:
	$(ARM_CC) $(ARM_CFLAGS) $(WARN) -Ibuild -fsyntax-only app/pair.c
	$(ARM_CC) $(ARM_CFLAGS) $(WARN) -Ibuild -fsyntax-only app/actions.c
	$(ARM_CC) $(ARM_CFLAGS) $(WARN) -Ibuild -DDASH_CMDR=1 -DAES256=1 -fsyntax-only app/actions.c
.PHONY: check-app
