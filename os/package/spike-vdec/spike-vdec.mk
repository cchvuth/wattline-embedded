################################################################################
#
# spike-vdec: R4 spike decoder/rotate test (src/spike-vdec.c; the parser also runs on a build host: --parse)
#
################################################################################

SPIKE_VDEC_VERSION = local
SPIKE_VDEC_SITE = $(BR2_EXTERNAL_DASH_PATH)/package/spike-vdec/src
SPIKE_VDEC_SITE_METHOD = local
SPIKE_VDEC_LICENSE = MIT

define SPIKE_VDEC_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) -O2 $(TARGET_LDFLAGS) -Wall -Werror -o $(@D)/spike-vdec $(@D)/spike-vdec.c
endef

define SPIKE_VDEC_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/spike-vdec $(TARGET_DIR)/usr/sbin/spike-vdec
endef

$(eval $(generic-package))
