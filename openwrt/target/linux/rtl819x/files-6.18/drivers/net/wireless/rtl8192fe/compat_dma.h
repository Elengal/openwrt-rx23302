/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Переходник со старого набора вызовов работы с памятью для PCI.
 *
 * Ядро 5.18 убрало весь старый набор pci_* в пользу общего dma_*. Вендорский
 * драйвер написан на старом и пользуется им примерно в 230 местах: одних
 * только PCI_DMA_TODEVICE сто четыре. Править каждое место вручную значит
 * почти наверняка где-то ошибиться в направлении передачи, а ошибка в
 * направлении даёт порчу данных, которую потом не поймать.
 *
 * Поэтому отображаем старые имена в новые здесь, одним местом, а вендорский
 * код оставляем дословно. Файл подключается ключом -include ко всем файлам
 * драйвера, чтобы порядок включения не имел значения.
 *
 * О замене адресов: virt_to_bus и bus_to_virt на этой платформе всегда были
 * равны virt_to_phys и phys_to_virt — устройство ходит по физическим
 * адресам напрямую, промежуточного преобразователя адресов у него нет.
 */

#ifndef _RTL8192CD_COMPAT_DMA_H
#define _RTL8192CD_COMPAT_DMA_H

#include <linux/dma-mapping.h>
#include <linux/pci.h>

/* Направления передачи. Числовые значения у старых и новых имён совпадают,
 * так что переменные типа int, гуляющие по драйверу, остаются верными. */
#ifndef PCI_DMA_BIDIRECTIONAL
#define PCI_DMA_BIDIRECTIONAL	DMA_BIDIRECTIONAL
#endif
#ifndef PCI_DMA_TODEVICE
#define PCI_DMA_TODEVICE	DMA_TO_DEVICE
#endif
#ifndef PCI_DMA_FROMDEVICE
#define PCI_DMA_FROMDEVICE	DMA_FROM_DEVICE
#endif
#ifndef PCI_DMA_NONE
#define PCI_DMA_NONE		DMA_NONE
#endif

/* Отображение вызовов. Первым доводом у новых идёт устройство, а не карта. */
#ifndef pci_map_single
#define pci_map_single(pdev, p, sz, dir) \
	dma_map_single(&(pdev)->dev, (p), (sz), (dir))
#endif
#ifndef pci_unmap_single
#define pci_unmap_single(pdev, a, sz, dir) \
	dma_unmap_single(&(pdev)->dev, (a), (sz), (dir))
#endif
#ifndef pci_dma_sync_single_for_cpu
#define pci_dma_sync_single_for_cpu(pdev, a, sz, dir) \
	dma_sync_single_for_cpu(&(pdev)->dev, (a), (sz), (dir))
#endif
#ifndef pci_dma_sync_single_for_device
#define pci_dma_sync_single_for_device(pdev, a, sz, dir) \
	dma_sync_single_for_device(&(pdev)->dev, (a), (sz), (dir))
#endif

/* Старая pci_alloc_consistent выделяла память в неспящем режиме — сохраняем
 * это поведение явным GFP_ATOMIC, иначе драйвер уснёт там, где не имеет права. */
#ifndef pci_alloc_consistent
#define pci_alloc_consistent(pdev, sz, dma) \
	dma_alloc_coherent(&(pdev)->dev, (sz), (dma), GFP_ATOMIC)
#endif
#ifndef pci_free_consistent
#define pci_free_consistent(pdev, sz, va, dma) \
	dma_free_coherent(&(pdev)->dev, (sz), (va), (dma))
#endif

#ifndef virt_to_bus
/* Старая virt_to_bus принимала и указатель, и число: вендор передаёт
 * то одно, то другое. Приводим довод сами, чтобы сохранить эту
 * терпимость — иначе половина вызовов перестаёт собираться. */
#define virt_to_bus(p)		virt_to_phys((void *)(unsigned long)(p))
#endif
#ifndef bus_to_virt
#define bus_to_virt(a)		phys_to_virt((phys_addr_t)(unsigned long)(a))
#endif

#endif /* _RTL8192CD_COMPAT_DMA_H */
