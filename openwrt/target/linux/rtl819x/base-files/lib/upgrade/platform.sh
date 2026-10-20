# SPDX-License-Identifier: GPL-2.0-only
#
# sysupgrade для ELECTRA RX23302 (Ротек RX-23302).
#
# Образ sysupgrade.bin = full.bin + метаданные fwtool:
#   0x000000  ядро с заголовком KE32 (cvimg), добито 0xFF до 0x2d0000
#   0x2d0000  rootfs (squashfs + 2 байта суммы загрузчика)
# Во флеше это разделы kernel (0x30000) и rootfs (0x300000); настройки
# живут отдельно, в rootfs_data (0x600000), а не за rootfs.
#
# Порядок записи выбран так, чтобы прерванное обновление (выключили свет)
# не превращало роутер в кирпич:
#   1. стереть kernel - с этой секунды набор 1 для загрузчика негоден;
#   2. записать rootfs;
#   3. записать kernel - последним.
# Если запись оборвётся на шагах 1-3, загрузчик не найдёт годного ядра,
# скопирует заводскую прошивку из второго банка (0x830000) и загрузит её.
# Старое ядро с новым rootfs (или наоборот) не загрузится никогда.
#
# sysupgrade for the RX23302. The image is full.bin plus fwtool metadata.
# Order: erase kernel, write rootfs, write kernel last - an interrupted
# upgrade leaves no valid kernel, and the bootloader restores the stock
# firmware from bank 2 instead of booting a mismatched kernel/rootfs pair.

REQUIRE_IMAGE_METADATA=1

RX23302_KERNEL_SIZE=$((0x2d0000))
RX23302_ROOTFS_SIZE=$((0x300000))

# Размер образа без хвоста метаданных fwtool.
rx23302_image_size() {
	fwtool -q -T -i /dev/null "$1" | wc -c
}

platform_check_image() {
	local img="$1"
	local size magic burn sq

	[ "$#" -gt 1 ] && return 1

	magic=$(get_magic_long "$img")
	[ "$magic" = "4b453332" ] || {
		v "Not an RX23302 image: no KE32 kernel header (got $magic)."
		v "Use openwrt-*-electra_rx23302-squashfs-sysupgrade.bin."
		return 1
	}

	# Адрес записи ядра из заголовка KE32 (байты 8..11) - только 0x30000.
	burn=$(dd if="$img" bs=1 skip=8 count=4 2>/dev/null | hexdump -v -n 4 -e '1/1 "%02x"')
	[ "$burn" = "00030000" ] || {
		v "Kernel burn address $burn is not 00030000 - wrong image."
		return 1
	}

	sq=$(dd if="$img" bs=4096 skip=$((RX23302_KERNEL_SIZE / 4096)) count=1 2>/dev/null | \
		hexdump -v -n 4 -e '1/1 "%02x"')
	case "$sq" in
	68737173|73717368) ;;
	*)
		v "No squashfs at offset 0x2d0000 (got $sq) - this is not a full image."
		return 1
		;;
	esac

	size=$(rx23302_image_size "$img")
	[ "$size" -gt "$RX23302_KERNEL_SIZE" ] && \
	[ "$size" -le $((RX23302_KERNEL_SIZE + RX23302_ROOTFS_SIZE)) ] || {
		v "Image size $size is out of range."
		return 1
	}

	return 0
}

platform_do_upgrade() {
	local img="$1"
	local kblocks=$((RX23302_KERNEL_SIZE / 4096))

	# Хвост метаданных во флеш не нужен.
	fwtool -q -t -i /dev/null "$img"

	sync
	echo 3 > /proc/sys/vm/drop_caches

	v "Invalidating kernel (interrupted upgrade -> bootloader restores stock)..."
	mtd erase kernel || exit 1

	# Настройки: rootfs_data - отдельный раздел, mtd write его не трогает.
	# Стираем всегда и до записи ядра, иначе старый overlay перекроет
	# новую систему.
	v "Erasing rootfs_data..."
	mtd erase rootfs_data || exit 1

	v "Writing rootfs..."
	dd if="$img" bs=4096 skip=$kblocks 2>/dev/null | mtd write - rootfs || exit 1

	v "Writing kernel..."
	dd if="$img" bs=4096 count=$kblocks 2>/dev/null | mtd write - kernel || exit 1
}

# Сохранённые настройки кладём в чистый rootfs_data файлом sysupgrade.tgz;
# mount_root перенесёт его в overlay, а preinit распакует при загрузке.
platform_copy_config() {
	v "Saving config to rootfs_data..."
	mtd jffs2write "$UPGRADE_BACKUP" rootfs_data
}
