################################################################################
#
# wattline-aa: the wireless Android Auto receiver, built from the app checkout (/native: aa/aa.c + core/pb.c +
# ui/project_region.h) with this screen's options, linked against the target OpenSSL. Installs /usr/bin/aa.
#
################################################################################

WATTLINE_AA_VERSION = local
WATTLINE_AA_SITE = /native
WATTLINE_AA_SITE_METHOD = local
WATTLINE_AA_LICENSE = MIT
WATTLINE_AA_DEPENDENCIES = openssl
WATTLINE_AA_OVERRIDE_SRCDIR_RSYNC_EXCLUSIONS = --include=/aa/*** --include=/core/ --include=/core/pb.* \
	--include=/ui/ --include=/ui/project_region.h --exclude=*
# the C906's own instructions; the picture's place on the 1920x720 canvas (app/device/f133.mk's); the Bluetooth start
# through the power MCU, wired AOA, and only the system audio sink: the phone keeps its audio on its own Bluetooth to
# the car, never through this screen
WATTLINE_AA_OPTS = -mcpu=thead-c906 -DPJ_PANEL_H=720 -DPJ_RGN_X=0 -DPJ_RGN_Y=180 -DPJ_RGN_W=960 -DPJ_RGN_H=540 \
	-DAA_MCU=1 -DAA_USB=1 -DAA_SYSTEM_AUDIO_ONLY=1

define WATTLINE_AA_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) -O2 $(TARGET_LDFLAGS) -Wall -Werror $(WATTLINE_AA_OPTS) \
		-o $(@D)/aa-bin $(@D)/aa/aa.c $(@D)/core/pb.c -lssl -lcrypto -lpthread
endef

define WATTLINE_AA_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/aa-bin $(TARGET_DIR)/usr/bin/aa
endef

$(eval $(generic-package))
