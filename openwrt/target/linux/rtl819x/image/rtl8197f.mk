#
# RTL8197F device profiles
#

# ELECTRA RX23302 — роутер Ростелекома на RTL8197FH-VG.
#
# Адреса подтверждены выводом собственного bootcode устройства:
#   "Jump to image start=0x80a00000"   → CVIMG_LOADER_START_ADDR
#   dual boot utility: linux_offset=0x30000 → CVIMG_KERNEL_BURN_ADDR
# IMAGE_SIZE = банк 8 МБ минус 0x30000 под bootcode+hwconfig.
define Device/electra_rx23302
  SOC := rtl8197f
  DEVICE_VENDOR := ELECTRA
  DEVICE_MODEL := RX23302
  IMAGE_SIZE := 7936k
  CVIMG_KERNEL_BURN_ADDR := 0x30000
  CVIMG_LOADER_START_ADDR := 0x80A00000
  CVIMG_CUSTOM_SIGNATURE := KE32
endef
TARGET_DEVICES += electra_rx23302
