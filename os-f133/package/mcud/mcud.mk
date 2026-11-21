################################################################################
#
# mcud: the screen's power MCU and its Bluetooth, on ttyS1 (src/mcud.c; src/test_mcud.py tests it on the build host
# against a pty that plays the MCU: cc -O2 -Wall -o /tmp/mcud src/mcud.c && python3 -I src/test_mcud.py /tmp/mcud)
#
################################################################################

MCUD_VERSION = local
MCUD_SITE = $(BR2_EXTERNAL_F133_PATH)/package/mcud/src
MCUD_SITE_METHOD = local
MCUD_LICENSE = MIT

define MCUD_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) -O2 $(TARGET_LDFLAGS) -Wall -Werror -o $(@D)/mcud $(@D)/mcud.c
endef

define MCUD_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/mcud $(TARGET_DIR)/usr/sbin/mcud
endef

$(eval $(generic-package))
