# SPDX-License-Identifier: GPL-2.0-only
#
# Realtek RTL819x — роутерные MIPS SoC (RTL8196E/8197D/8197F).
#
# Отдельный таргет, а не субтаргет апстримного "realtek": тот собран под
# switch-SoC (RTL838x/839x/930x/931x) и он big-endian, а роутерные 819x —
# little-endian, поэтому ARCH задаётся здесь как mipsel.

include $(TOPDIR)/rules.mk

ARCH:=mipsel
BOARD:=rtl819x
BOARDNAME:=Realtek RTL819x MIPS
SUBTARGETS:=rtl8197f
FEATURES:=squashfs gpio ramdisk

KERNEL_PATCHVER:=6.18

define Target/Description
	Build firmware images for Realtek RTL819x based routers.
endef

include $(INCLUDE_DIR)/target.mk

DEFAULT_PACKAGES += kmod-leds-gpio kmod-gpio-button-hotplug rtlmib

$(eval $(call BuildTarget))
