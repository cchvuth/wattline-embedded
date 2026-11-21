################################################################################
#
# spike-vdec: the projection's H.264 decoder sink, built from the app checkout (/native/vdec/spike-vdec.c) for this
# screen. Installs /usr/sbin/spike-vdec.
#
################################################################################

SPIKE_VDEC_VERSION = local
SPIKE_VDEC_SITE = /native/vdec
SPIKE_VDEC_SITE_METHOD = local
SPIKE_VDEC_LICENSE = MIT
# the C906's own instructions
SPIKE_VDEC_OPTS = -mcpu=thead-c906

define SPIKE_VDEC_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) -O2 $(TARGET_LDFLAGS) -pthread -Wall -Werror $(SPIKE_VDEC_OPTS) \
		-o $(@D)/spike-vdec $(@D)/spike-vdec.c
endef

define SPIKE_VDEC_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/spike-vdec $(TARGET_DIR)/usr/sbin/spike-vdec
endef

$(eval $(generic-package))
