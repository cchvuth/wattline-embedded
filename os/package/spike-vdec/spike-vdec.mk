################################################################################
#
# spike-vdec: Android Auto's video, decoded by the video engine and shown on the panel, built from the app's checkout
# (/native/vdec: one source for the Dash and the F133 screen; its parser also runs on a build host: --parse)
#
################################################################################

SPIKE_VDEC_VERSION = local
SPIKE_VDEC_SITE = /native/vdec
SPIKE_VDEC_SITE_METHOD = local
SPIKE_VDEC_LICENSE = MIT

define SPIKE_VDEC_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) -O2 $(TARGET_LDFLAGS) -pthread -Wall -Werror -o $(@D)/spike-vdec $(@D)/spike-vdec.c
endef

define SPIKE_VDEC_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/spike-vdec $(TARGET_DIR)/usr/sbin/spike-vdec
endef

$(eval $(generic-package))
