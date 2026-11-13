################################################################################
#
# bt-fwcutter: Realtech/RTBTCore epatch + UART config cutter (src/bt-fwcutter.c, host test: test.py)
#
################################################################################

BT_FWCUTTER_VERSION = local
BT_FWCUTTER_SITE = $(BR2_EXTERNAL_DASH_PATH)/package/bt-fwcutter/src
BT_FWCUTTER_SITE_METHOD = local
BT_FWCUTTER_LICENSE = MIT

define BT_FWCUTTER_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -Wall -o $(@D)/bt-fwcutter $(@D)/bt-fwcutter.c
endef

define BT_FWCUTTER_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/bt-fwcutter $(TARGET_DIR)/usr/sbin/bt-fwcutter
endef

$(eval $(generic-package))
