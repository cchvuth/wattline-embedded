################################################################################
#
# g2drot: the G2D rotation module (/dev/g2drot) the dash's flush uses (port/linux/fb_rot.c); built with the kernel so
# its version always matches. Loaded by S04g2drot, before the dash.
#
################################################################################

G2DROT_VERSION = 1
G2DROT_SITE = $(BR2_EXTERNAL_F133_PATH)/package/g2drot/src
G2DROT_SITE_METHOD = local
G2DROT_LICENSE = GPL-2.0

$(eval $(kernel-module))
$(eval $(generic-package))
