################################################################################
#
# rtl8733bs: Realtek vendor WiFi driver v5.15.17 (20250521) patched for Linux 7.3.4.
# Source in radio/src/work (GPL-2.0, see radio/README.md); mount radio/ at /radio in the build container.
#
################################################################################

RTL8733BS_VERSION = 5.15.17-20250521
RTL8733BS_SITE = /radio/src/work
RTL8733BS_SITE_METHOD = local
RTL8733BS_LICENSE = GPL-2.0
# station only (AP refused in cfg80211 by the source patch), no MP/test mode, no calibration or PHY tables from files
RTL8733BS_MODULE_MAKE_OPTS = CONFIG_RTL8733BS=m \
	CONFIG_PLATFORM_I386_PC=y CONFIG_P2P=n CONFIG_BR_EXT=n CONFIG_MP_INCLUDED=n \
	CONFIG_EFUSE_CONFIG_FILE=n CONFIG_LOAD_PHY_PARA_FROM_FILE=n CONFIG_WIFI_MONITOR=n \
	CONFIG_PROC_DEBUG=n

$(eval $(kernel-module))
$(eval $(generic-package))
