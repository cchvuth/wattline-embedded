################################################################################
#
# dash-ota: sig-verify, the release-signature check of the over-the-air updater (the T113 Dash's)
#
################################################################################

DASH_OTA_VERSION = local
DASH_OTA_SITE = $(BR2_EXTERNAL_F133_PATH)/package/dash-ota/src
DASH_OTA_SITE_METHOD = local
DASH_OTA_LICENSE = MIT
DASH_OTA_DEPENDENCIES = openssl

define DASH_OTA_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) -O2 $(TARGET_LDFLAGS) -Wall -Werror -o $(@D)/sig-verify $(@D)/sig-verify.c -lcrypto
endef

define DASH_OTA_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/sig-verify $(TARGET_DIR)/usr/bin/sig-verify
endef

$(eval $(generic-package))
