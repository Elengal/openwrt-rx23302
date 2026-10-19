// SPDX-License-Identifier: GPL-2.0
/*
 * Устройство для DMA-выделений.
 *
 * Вендор выделяет кольцо приёма так: dma_alloc_coherent(NULL, ...).
 * Ядро Realtek 4.4 это прощало, а 6.18 первым делом читает
 * dev->coherent_dma_mask и падает на NULL (первый запуск 01.10.2026 12:07:
 * Oops в dma_alloc_attrs+0x18, BadVA 0x98).
 *
 * Встроенное радио RTL8197G - устройство на шине процессора, без PCI,
 * описания в дереве устройств у нас тоже нет. Поэтому заводим своё
 * платформенное устройство с 32-битной DMA-маской и отдаём его вместо
 * NULL. Память берётся тем же путём, что у любого платформенного
 * драйвера: некэшируемое окно KSEG1 для некогерентного MIPS.
 */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/dma-mapping.h>

static struct platform_device *rtl_dma_pdev;

struct device *rtl_dma_dev(void)
{
	return rtl_dma_pdev ? &rtl_dma_pdev->dev : NULL;
}

int rtl_dma_dev_init(void)
{
	struct platform_device_info info = {
		.name		= "rtl8192cd-dma",
		.id		= PLATFORM_DEVID_NONE,
		.dma_mask	= DMA_BIT_MASK(32),
	};
	int rc;

	rtl_dma_pdev = platform_device_register_full(&info);
	if (IS_ERR(rtl_dma_pdev)) {
		rc = PTR_ERR(rtl_dma_pdev);
		rtl_dma_pdev = NULL;
		pr_err("rtl8192cd: DMA device register failed (%d)\n", rc);
		return rc;
	}
	rc = dma_set_mask_and_coherent(&rtl_dma_pdev->dev, DMA_BIT_MASK(32));
	if (rc) {
		pr_err("rtl8192cd: DMA mask failed (%d)\n", rc);
		platform_device_unregister(rtl_dma_pdev);
		rtl_dma_pdev = NULL;
		return rc;
	}
	pr_info("rtl8192cd: DMA device ready\n");
	return 0;
}

void rtl_dma_dev_exit(void)
{
	if (rtl_dma_pdev) {
		platform_device_unregister(rtl_dma_pdev);
		rtl_dma_pdev = NULL;
	}
}
