# SPDX-License-Identifier: GPL-2.0-only
#
# Образы для Realtek RTL819x.
#
# Стоковый bootcode Realtek — не U-Boot. Он ждёт ядро со своим 64-байтным
# заголовком, который ставит утилита cvimg (tools/cvimg). Сигнатура заголовка
# зависит от модели: у ELECTRA RX23302 это "KE32" — подсмотрено в дампе
# флеша по смещению 0x30000 и задаётся через CVIMG_CUSTOM_SIGNATURE.
#
# Само ядро упаковано lzma-loader'ом: bootcode кладёт образ по
# CVIMG_LOADER_START_ADDR и прыгает на него, дальше loader разжимает LZMA
# и передаёт управление ядру.

include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/image.mk

DEVICE_VARS += CVIMG_KERNEL_BURN_ADDR CVIMG_LOADER_START_ADDR
DEVICE_VARS += CVIMG_CUSTOM_SIGNATURE CVIMG_ROOTFS_BURN_ADDR

define Build/loader-common
	rm -rf $@.src
	$(MAKE) -C lzma-loader \
		PKG_BUILD_DIR="$@.src" \
		TARGET_DIR="$(dir $@)" LOADER_NAME="$(notdir $@)" \
		BOARD="$(DEVICE_NAME)" \
		SUBTARGET="$(SUBTARGET)" \
		LZMA_TEXT_START=0x81000000 LOADADDR=0x80000000 \
		$(1) compile loader.bin
	mv "$@.bin" "$@"
	rm -rf $@.src
endef

define Build/loader-cmdline-compile
	$(call Build/loader-common,LOADER_DATA="$@" KERNEL_CMDLINE="$(CMDLINE)")
endef

define Build/cvimg
	$(STAGING_DIR_HOST)/bin/cvimg \
		-i $@ \
		-o $@.new \
		$(1) && mv $@.new $@ || rm -f $@
endef

# Заголовок с явной сигнатурой: ключ -s перекрывает -t/-c.
# Без него cvimg ставит дефолтную "cs6c", которую наш bootcode не принимает.
define Build/cvimg-custom
	$(call Build/cvimg, \
		-e $(CVIMG_LOADER_START_ADDR) \
		-b $(CVIMG_KERNEL_BURN_ADDR) \
		-s $(CVIMG_CUSTOM_SIGNATURE) \
		$(1))
endef

# Хвост, который загрузчик требует от корневой системы.
#
# Он проверяет её по фиксированному смещению и, не найдя верной суммы, пишет
# "rootfs checksum error at 00300000!", объявляет набор негодным и копирует
# поверх заводскую копию из второго банка. Своя прошивка при этом молча
# исчезает после первой же перезагрузки — записалась, а на второй загрузке
# её уже нет.
#
# Формат снят с заводского образа: тело squashfs добито нулями до 4096, следом
# два байта суммы. Подробности и самопроверка — в самом скрипте.
define Build/rootfs-chksum
	$(TOPDIR)/scripts/rtl819x-rootfs-chksum.py $@
endef

# Один файл под флеш с 0x30000 (см. IMAGE/full.bin): ядро | 0xFF до начала
# раздела rootfs (0x300000 - 0x30000 = 0x2d0000) | rootfs с суммой загрузчика.
define Build/rx23302-full
	$(TOPDIR)/scripts/rx23302-full-image.py $(IMAGE_KERNEL) $(IMAGE_ROOTFS) $@
endef

define Build/append-fake-rootfs
	$(call Build/cvimg, \
		-a $(BLOCKSIZE) \
		-f)
endef

define Build/cvimg-pad-rootfs
	$(call Build/cvimg, \
		-b $(CVIMG_KERNEL_BURN_ADDR) \
		-c new \
		-a $(BLOCKSIZE) \
		-t rootfs \
		-j)
endef

define Device/Default
  PROFILES := Default
  DEVICE_DTS_DIR := ../dts
  DEVICE_DTS = $$(SOC)_$(1)
  BLOCKSIZE := 64k
  CVIMG_ROOTFS_BURN_ADDR := 0
  CMDLINE := console=ttyS0,38400
  KERNEL := kernel-bin | append-dtb | lzma | loader-cmdline-compile | cvimg-custom
  KERNEL_INITRAMFS := kernel-bin | append-dtb | lzma | loader-cmdline-compile | cvimg-custom
  # kernel.bin и rootfs.bin — раздельные образы под разделы флеша: загрузчику
  # нужно ядро на 0x30000, а корневая система лежит отдельно на 0x300000.
  # Ими же удобно обновляться по сети из уже работающей системы (mtd write).
  IMAGES := sysupgrade.bin kernel.bin rootfs.bin full.bin
  # sysupgrade.bin = full.bin + метаданные fwtool (имя платы для проверки).
  # Пишет его base-files/lib/upgrade/platform.sh: rootfs, затем ядро.
  IMAGE/sysupgrade.bin := rx23302-full | append-metadata
  IMAGE/kernel.bin := append-kernel
  IMAGE/rootfs.bin := append-rootfs | rootfs-chksum
  # full.bin (RX23302) - ОДИН файл под флеш с адреса 0x30000:
  # ядро, добивка 0xFF до 0x300000 (начало раздела rootfs), rootfs с суммой
  # загрузчика. Пишется целиком: `mtd write full.bin firmware` или загрузчиком
  # через UART (XMOD в ОЗУ + FLW 0x30000). Прежний sysupgrade.bin клал rootfs
  # сразу за ядром (0x25aa82), а загрузчик ждёт его на 0x300000 - так не
  # загрузится. Размер упирается в kernel+rootfs = 0x2d0000 + 0x300000.
  IMAGE/full.bin := rx23302-full
endef

include $(SUBTARGET).mk

$(eval $(call BuildImage))
