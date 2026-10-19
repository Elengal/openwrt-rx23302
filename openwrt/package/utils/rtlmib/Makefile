# SPDX-License-Identifier: GPL-2.0-only
#
# rtlmib — замена «iwpriv wlanX set_mib/get_mib» для драйвера rtl8192fe
# (в ядре 6.18 нет WEXT, iwpriv не работает). Нужна /lib/netifd/wireless/rtl8192cd.sh.

include $(TOPDIR)/rules.mk

PKG_NAME:=rtlmib
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-only

include $(INCLUDE_DIR)/package.mk

define Package/rtlmib
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=set_mib/get_mib for the Realtek rtl8192fe Wi-Fi driver
  DEPENDS:=@TARGET_rtl819x
endef

define Package/rtlmib/description
  Small replacement for "iwpriv wlanX set_mib/get_mib" used by the
  rtl8192cd netifd wireless script on RTL819x boards.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -Wall \
		-o $(PKG_BUILD_DIR)/rtlmib $(PKG_BUILD_DIR)/rtlmib.c
endef

define Package/rtlmib/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/rtlmib $(1)/usr/sbin/
endef

$(eval $(call BuildPackage,rtlmib))
