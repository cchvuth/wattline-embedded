################################################################################
#
# aic8800
#
################################################################################

AIC8800_VERSION = 5562c254b0231be34588dc608ec28c4f1d613285
AIC8800_SITE = $(call github,radxa-pkg,aic8800,$(AIC8800_VERSION))
# the driver GPL-2.0; the chip's firmware is AICSemi's binaries, installed as radxa-pkg/aic8800 distributes them
AIC8800_LICENSE = GPL-2.0 (driver), AICSemi firmware binaries (see debian/copyright)
AIC8800_LICENSE_FILES = LICENSE debian/copyright
AIC8800_MODULE_SUBDIRS = src/SDIO/driver_fw/driver/aic8800
# Bluetooth over SDIO: the bsp loads fmacfwbt_8800d80_h_u02.bin and sets the BT port to the mailbox (no UART), fdrv
# registers hci0 (btsdio.c, BlueZ mode). Needs CONFIG_BT in the kernel. btlpm is for UART-attached BT: not built.
AIC8800_MODULE_MAKE_OPTS = CONFIG_AIC8800_BTLPM_SUPPORT=n CONFIG_SDIO_BT=y

# The screen has an AIC8800D80 (SDIO c8a1:0082); the driver gets the folder as aic_fw_path= (/etc/init.d/S40wifi)
define AIC8800_INSTALL_FIRMWARE
	mkdir -p $(TARGET_DIR)/lib/firmware/aic8800
	cp $(@D)/src/SDIO/driver_fw/fw/aic8800D80/* $(TARGET_DIR)/lib/firmware/aic8800/
endef
AIC8800_POST_INSTALL_TARGET_HOOKS += AIC8800_INSTALL_FIRMWARE

$(eval $(kernel-module))
$(eval $(generic-package))
