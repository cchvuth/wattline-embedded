################################################################################
#
# wattline-aa: the wireless Android Auto receiver, built from the app checkout (app/, mounted at /native)
# (/native: aa/aa.c + core/pb.c), linked against the target OpenSSL. Installs /usr/bin/aa.
#
################################################################################

WATTLINE_AA_VERSION = local
WATTLINE_AA_SITE = /native
WATTLINE_AA_SITE_METHOD = local
WATTLINE_AA_LICENSE = MIT
# libglib2 provides gio/gobject for the BlueZ RFCOMM profile (D-Bus) glue (-DAA_BT)
WATTLINE_AA_DEPENDENCIES = openssl libglib2
WATTLINE_AA_OVERRIDE_SRCDIR_RSYNC_EXCLUSIONS = --exclude=/build

WATTLINE_AA_GIO_CFLAGS = $(shell $(PKG_CONFIG_HOST_BINARY) --cflags gio-2.0 gio-unix-2.0)
WATTLINE_AA_GIO_LIBS = $(shell $(PKG_CONFIG_HOST_BINARY) --libs gio-2.0 gio-unix-2.0)

define WATTLINE_AA_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) -O2 $(TARGET_LDFLAGS) -Wall -DAA_BT \
		$(WATTLINE_AA_GIO_CFLAGS) \
		-o $(@D)/aa-bin $(@D)/aa/aa.c $(@D)/core/pb.c \
		-lssl -lcrypto $(WATTLINE_AA_GIO_LIBS)
endef

define WATTLINE_AA_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/aa-bin $(TARGET_DIR)/usr/bin/aa
endef

$(eval $(generic-package))
