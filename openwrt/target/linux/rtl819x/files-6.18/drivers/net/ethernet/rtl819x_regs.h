/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Регистры внутреннего свитча RTL8197F — общие для rtl819x_eth.c (своя копия
 * базовых адресов там же), rtl865x_asichal.c (таблицы L2/L3/L4) и
 * rtl819x_hwnat.c. Значения из вендорского SDK (rtl865xc_asicregs.h) и донора
 * ADCDS openwrt-dlink-dir842-r1 (rtl819x_regs.h, GPL-2.0).
 */
#ifndef _RTL819X_REGS_H
#define _RTL819X_REGS_H

#define RTL819X_SWCORE_BASE		0xBB800000
#define RTL819X_SWTBL_BASE		0xBB000000	/* окно таблиц свитча */

/* ---- доступ: как вендорский REG32, KSEG1 без кэша ---------------------- */
#define REG32(reg)			(*(volatile u32 *)(uintptr_t)(reg))

/* ---- таблицы: интерфейс TLU (TACI) ------------------------------------- */
#define TACI_BASE			(RTL819X_SWCORE_BASE + 0x4D00)
#define SWTACR				(TACI_BASE + 0x000)	/* команда таблицы */
#define SWTASR				(TACI_BASE + 0x004)	/* статус команды */
#define SWTAA				(TACI_BASE + 0x008)	/* адрес строки */
#define TCR0				(TACI_BASE + 0x020)	/* слова 0..7 содержимого */
#define TCR(n)				(TACI_BASE + 0x020 + ((n) << 2))
#define TLU_ACTION_MASK			0x1
#define TLU_ACTION_DONE			0x0
#define TLU_ACTION_START		0x1
#define TLU_CMD_FORCE			(1 << 3)
#define CMD_ADD				(1 << 1)		/* вставка по хешу */
#define TABSTS_MASK			0x1
#define TABSTS_SUCCESS			0x0

/* ---- ALE: управление слоями свитча -------------------------------------- */
#define ALE_BASE			(RTL819X_SWCORE_BASE + 0x4400)
#define TEACR				(ALE_BASE + 0x00)	/* старение L2/ARP/PPPoE */
#define TEATCR				(ALE_BASE + 0x04)	/* перезарядка NAPT, по протоколам */
#define MSCR				(ALE_BASE + 0x10)	/* включение L2/L3/L4/ACL */
#define SWTCR0				(ALE_BASE + 0x18)	/* политика таблиц */
#define SWTCR1				(ALE_BASE + 0x1C)	/* хеш L4 (EnL4WayH/L4EnHash1) */
#define ALECR				(ALE_BASE + 0x0C)	/* == TTLCR: TTL-- и PPPoE */
#define DACLRCR				(ALE_BASE + 0x24)	/* ACL по умолчанию */
#define FFCR				(ALE_BASE + 0x28)	/* кадры: unknown UC/MC */
#define CSCR				(RTL819X_SWCORE_BASE + 0x4048)	/* пересчёт сумм L3/L4 */
#define MACCR1				(RTL819X_SWCORE_BASE + 0x4058)	/* PORT0_ROUTER_MODE бит 0 */

/* SWTCR0 биты, что нужны asichal */
#define EN_STOP_TLU			(1u << 18)		/* заморозить поиск таблиц */
#define STOP_TLU_READY			(1u << 19)
#define SWTCR0_UNKVID_2CPU		(1u << 15)

/* ---- сброс и SRAM таблиц ------------------------------------------------ */
#define SWMISC_BASE			(RTL819X_SWCORE_BASE + 0x4200)
#define SIRR				(0x04 + SWMISC_BASE)
#define SIRR_FULL_RST			(1 << 2)
#define ASIC_L3_ENGINE_CFG		(RTL819X_SWCORE_BASE + 0x4234)	/* == MEMCR */
#define L3_ENGINE_READY			(1 << 11)

/* ---- диагностика фабрики (DESCDIAG) ------------------------------------- */
#define DESCDIAG_BASE			(RTL819X_SWCORE_BASE + 0x6100)
#define GDSR0				(DESCDIAG_BASE + 0x000)
#define GDSR0_USEDDSC_MASK		(0x3ff << 16)
#define GDSR0_DSCRUNOUT			(1 << 27)
#define GDSR0_TOTALDSC_FC		(1 << 26)
#define GDSR0_SHAREDBUF_FCON		(1 << 14)
#define GDSR0_MAXUSEDDSC_MASK		(0x3fff << 0)
#define GDSR1				(DESCDIAG_BASE + 0x004)
#define PCSR0				(DESCDIAG_BASE + 0x008)
#define PCSR1				(DESCDIAG_BASE + 0x00C)
#define Pn_DCR0(p)			(DESCDIAG_BASE + 0x010 + (p) * 0x10)
#define SBFCR0				(RTL819X_SWCORE_BASE + 0x4500)
#define SBFCR1				(SBFCR0 + 0x04)
#define SBFCR2				(SBFCR0 + 0x08)

/* ---- PVID: два порта на регистр ----------------------------------------- */
#define PVCR0				(RTL819X_SWCORE_BASE + 0x4A08)
#define PVCR(port)			(RTL819X_SWCORE_BASE + 0x4A08 + ((port) >> 1) * 4)

#endif /* _RTL819X_REGS_H */
