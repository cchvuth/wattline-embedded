################################################################################
#
# rtl8733bs-cc: R4 spike. The rtl8733bs source (/radio/src/work, untouched) built as 8733bs_cc.ko with
# CONFIG_CONCURRENT_MODE and the AP ops on (8733bs-cc-ap-mcc.patch, applied to the build copy). No MCC: the driver
# refuses MCC with BT coexistence, and without coex its wifi-only antenna table gives every slot to WiFi, so a running AP
# leaves classic Bluetooth deaf (pairing, the AA handshake, HFP). MCC bought nothing anyway: station + AP stayed single-channel.
#
################################################################################

RTL8733BS_CC_VERSION = 5.15.17-20250521
RTL8733BS_CC_SITE = /radio/src/work
RTL8733BS_CC_SITE_METHOD = local
RTL8733BS_CC_LICENSE = GPL-2.0
# as rtl8733bs, plus: module name, installed in extra/ (the patch adds CONCURRENT_MODE; BT coex stays the default, on)
RTL8733BS_CC_MODULE_MAKE_OPTS = CONFIG_RTL8733BS=m \
	CONFIG_PLATFORM_I386_PC=y CONFIG_P2P=n CONFIG_BR_EXT=n CONFIG_MP_INCLUDED=n \
	CONFIG_EFUSE_CONFIG_FILE=n CONFIG_LOAD_PHY_PARA_FROM_FILE=n CONFIG_WIFI_MONITOR=n \
	CONFIG_PROC_DEBUG=n \
	USER_MODULE_NAME=8733bs_cc INSTALL_MOD_DIR=extra

define RTL8733BS_CC_APPLY_SPIKE_PATCH
	patch -d $(@D) -p1 --no-backup-if-mismatch < $(RTL8733BS_CC_PKGDIR)/8733bs-cc-ap-mcc.patch
endef
RTL8733BS_CC_POST_RSYNC_HOOKS += RTL8733BS_CC_APPLY_SPIKE_PATCH

$(eval $(kernel-module))
$(eval $(generic-package))
