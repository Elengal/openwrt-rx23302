#
# Copyright (C) 2007-2020 OpenWrt.org
#
# This is free software, licensed under the GNU General Public License v2.
# See /LICENSE for more information.
#
# cvimg — упаковщик образов для загрузчика Realtek RTL819x.
#
# Стоковый bootcode Realtek (не U-Boot) читает ядро только со своим
# 64-байтным заголовком: 4 байта сигнатуры, адрес загрузки, размер
# заголовка и длина payload. Сигнатура зависит от модели — у RX23302
# это "KE32", задаётся ключом -s.
#
# Утилита перенесена из tools/firmware-utils старого форка
# openwrt-rtl8197f: в актуальном OpenWrt firmware-utils подтягивается
# внешним git-репозиторием, куда свой исходник не положишь.
#
include $(TOPDIR)/rules.mk

PKG_NAME:=cvimg
PKG_RELEASE:=1

include $(INCLUDE_DIR)/host-build.mk

define Host/Compile
	$(HOSTCC) $(HOST_CFLAGS) -o $(HOST_BUILD_DIR)/cvimg src/cvimg.c
endef

define Host/Install
	$(CP) $(HOST_BUILD_DIR)/cvimg $(STAGING_DIR_HOST)/bin/
endef

define Host/Clean
	rm -f $(STAGING_DIR_HOST)/bin/cvimg
endef

$(eval $(call HostBuild))
