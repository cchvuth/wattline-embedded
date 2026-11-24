################################################################################
#
# dashapp: the dash program, built from the app checkout (app/, mounted at /native) with its app.mk.
# build/signals_gen.h and build/car_actions_gen.h are committed (made from the Flutter app on the Mac, app.mk's WATTLINE): they come along and
# are never remade here; every other build output of the checkout stays behind.
#
################################################################################

DASHAPP_VERSION = local
DASHAPP_SITE = /native
DASHAPP_SITE_METHOD = local
DASHAPP_DEPENDENCIES = libglib2 host-pkgconf
DASHAPP_OVERRIDE_SRCDIR_RSYNC_EXCLUSIONS = --include=/build/ --include=/build/signals_gen.h --include=/build/car_actions_gen.h --exclude=/build/*

define DASHAPP_BUILD_CMDS
	for g in signals_gen.h car_actions_gen.h; do test -s $(@D)/build/$$g || { echo "dashapp: the checkout has no build/$$g (it is committed: git checkout app/build/$$g)"; exit 1; }; done
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) -f app.mk -o build/signals_gen.h -o build/car_actions_gen.h ARM_CC="$(TARGET_CC)" ARM_PKG_CONFIG="$(PKG_CONFIG_HOST_BINARY)" CMDR=1 arm
endef

define DASHAPP_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/build/arm-app-cmdr/dash $(TARGET_DIR)/usr/bin/dash
endef

$(eval $(generic-package))
