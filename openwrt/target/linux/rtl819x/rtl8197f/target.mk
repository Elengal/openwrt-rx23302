#
# RTL8197F / RTL8197FH-VG — MIPS 24Kc, little-endian.
#
# Проверено на ELECTRA RX23302: CPU 999 МГц, 64 МБ DDR2, 16 МБ SPI-NOR.
#

SUBTARGET:=rtl8197f
BOARDNAME:=RTL8197F based boards
FEATURES+=small_flash
CPU_TYPE:=24kc

define Target/Description
	Build firmware images for Realtek RTL8197F based routers.
endef
