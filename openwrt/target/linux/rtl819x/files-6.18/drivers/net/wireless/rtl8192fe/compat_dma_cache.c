// SPDX-License-Identifier: GPL-2.0
/*
 * Обслуживание кэша для DMA внутри модуля.
 *
 * Вендорский драйвер на каждом кольце приёма/передачи сам зовёт
 * _dma_cache_wback / _dma_cache_inv / _dma_cache_wback_inv. Это указатели
 * на функции в arch/mips/mm/cache.c, и ядро 6.18 их модулям НЕ отдаёт
 * (EXPORT_SYMBOL нет, проверено по Module.symvers). У ядра Realtek они были
 * экспортированы.
 *
 * Не трогаем ядро - делаем то же самое здесь, теми же примитивами, что и
 * ядро для нашего процессора (MIPS 24Kc, только первичные кэши, кэш
 * данных 32 КБ PIPT без псевдонимов, строка 32 байта; см. boot.log):
 *
 *   r4k_dma_cache_wback_inv: размер >= кэша -> r4k_blast_dcache(), иначе
 *                            blast_dcache_range(); затем __sync().
 *   r4k_dma_cache_inv:       то же, но Hit_Invalidate_D по диапазону.
 *   _dma_cache_wback:        в c-r4k.c указывает на ту же wback_inv.
 *
 * r4k_blast_dcache экспортирован ядром (EXPORT_SYMBOL в c-r4k.c), а
 * blast_*dcache_range - встроенные функции из asm/r4kcache.h, им нужен
 * только cpu_data (тоже экспортирован). Вторичного кэша и bcache у 24Kc
 * нет - ветки cpu_has_inclusive_pcaches и bc_* для нас пустые.
 */
#include <linux/module.h>
#include <linux/preempt.h>
#include <asm/cacheflush.h>
#include <asm/r4kcache.h>
#include <asm/cpu-features.h>
#include <asm/barrier.h>

extern void (*r4k_blast_dcache)(void);

static inline unsigned long rtl_dcache_size(void)
{
	return cpu_data[0].dcache.waysize * cpu_data[0].dcache.ways;
}

static void rtl_dma_cache_wback_inv(unsigned long addr, unsigned long size)
{
	if (!size)
		return;
	preempt_disable();
	if (size >= rtl_dcache_size())
		r4k_blast_dcache();
	else
		blast_dcache_range(addr, addr + size);
	preempt_enable();
	__sync();
}

static void rtl_dma_cache_inv(unsigned long addr, unsigned long size)
{
	if (!size)
		return;
	preempt_disable();
	if (size >= rtl_dcache_size())
		r4k_blast_dcache();
	else
		blast_inv_dcache_range(addr, addr + size);
	preempt_enable();
	__sync();
}

/* Имена и типы - ровно те, что ждёт вендорский код (asm/io.h). */
void (*_dma_cache_wback_inv)(unsigned long start, unsigned long size) = rtl_dma_cache_wback_inv;
void (*_dma_cache_wback)(unsigned long start, unsigned long size) = rtl_dma_cache_wback_inv;
void (*_dma_cache_inv)(unsigned long start, unsigned long size) = rtl_dma_cache_inv;
