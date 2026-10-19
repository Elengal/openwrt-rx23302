/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Переходник для того, что из ядра убрали или переименовали.
 *
 * В отличие от compat_dma.h и compat_timer.h, здесь собрано разное — то,
 * что не складывается в одну тему.
 */

#ifndef _RTL8192CD_COMPAT_KERNEL_H
#define _RTL8192CD_COMPAT_KERNEL_H

#include <linux/proc_fs.h>
#include <linux/types.h>

/*
 * Своя hmac_sha1 у вендора (1x_kmsm_hmac.c) совпадает по имени с hmac_sha1
 * ядра (lib/crypto/sha1.c, другая сигнатура). Модулем это не мешало, а при
 * сборке драйвера В ЯДРО - «multiple definition». Переименовываем вендорскую
 * макросом для всего драйвера (файл подключается -include к каждому .c).
 * Риск: макрос задел бы и <crypto/sha1.h>, если его кто-то включит после.
 * Драйвер его не включает ни прямо, ни через netdevice/skbuff/sock
 * (проверено grep, 02.10.2026); включит - компилятор сразу ругнётся на
 * сигнатуру, молча не сломается.
 */
#define hmac_sha1 rtl_hmac_sha1

/*
 * PDE_DATA переименована в pde_data в ядре 5.17. Переименование, а не смена
 * поведения: возвращает те же данные, привязанные к записи в /proc.
 */
#ifndef PDE_DATA
#define PDE_DATA(inode) pde_data(inode)
#endif

/*
 * Поддержку протокола IPX удалили из ядра в версии 5.15 вместе с заголовком
 * net/ipx.h. Драйвер использует оттуда только описание заголовка пакета —
 * оно нужно ему, чтобы разбирать проходящие кадры в режиме моста.
 *
 * Описание задано самим протоколом и меняться не может, поэтому объявляем
 * его здесь дословно, как оно было в ядре до удаления.
 */
#ifndef IPX_NODE_LEN
#define IPX_NODE_LEN 6

struct ipx_address {
	__be32	net;
	__u8	node[IPX_NODE_LEN];
	__be16	sock;
};

struct ipxhdr {
	__be16			ipx_checksum;
#define IPX_NO_CHECKSUM	cpu_to_be16(0xFFFF)
	__be16			ipx_pktsize;
	__u8			ipx_tctrl;
	__u8			ipx_type;
	struct ipx_address	ipx_dest __packed;
	struct ipx_address	ipx_source __packed;
};
#endif /* IPX_NODE_LEN */

/*
 * Типы корпуса RTL8197F. Значения из вендорского bspchip.h, который
 * относится к коду процессора и в наш порт целиком не переносился.
 * Сама функция rtl819x_bond_option перенесена в arch/mips/realtek/setup.c.
 */
#ifndef BSP_BOND_97FB
#define BSP_BOND_97FB	1
#define BSP_BOND_97FN	2
#define BSP_BOND_97FS	3
#endif

/* ---- Дальше дописано Новой для rtl8192fe 3.8.0 (01.10.2026) ---- */

/*
 * Коды категорий кадров действия 802.11 (IEEE 802.11-2016, табл. 9-76).
 * Вендор берёт их из linux/ieee80211.h, который у него приходит вместе
 * с cfg80211. Мы собираем без cfg80211 (как сток RX23302: управление через
 * iwpriv), поэтому значения - здесь. Числа совпадают с ядром 6.18.
 */
#ifndef WLAN_CATEGORY_HT
#define WLAN_CATEGORY_RADIO_MEASUREMENT	5
#define WLAN_CATEGORY_HT		7
#define WLAN_CATEGORY_WNM		10
#define WLAN_CATEGORY_WNM_UNPROTECTED	11
#endif

/*
 * Ядро Realtek метило пакеты гостевой сети в skb->__unused = 0xe5, и его
 * мост по этой метке отделял гостей. В обычном ядре такого поля нет.
 * Гостевую сеть на первом запуске не делаем: метку просто отбрасываем.
 * Если понадобится - класть в skb->mark и ловить правилом nftables.
 */
#define RTL_SKB_GUEST_MARK(skb, v)	do { (void)(skb); } while (0)

/*
 * Запас под заголовок в начале приёмного буфера. В ядре Realtek он
 * задан как RTL_PRIV_DATA_SIZE (= 128, комментарий вендора рядом:
 * "dev_alloc_8190_skb: reserve 128"). Берём то же число.
 */
#ifndef RTL_PRIV_DATA_SIZE
#define RTL_PRIV_DATA_SIZE	128
#endif

/*
 * С 5.17 dev->dev_addr - const, и писать в него можно только через
 * eth_hw_addr_set(): ядро держит копию в dev_addr_shadow и при выгрузке
 * сверяет. Вендор пишет memcpy'ем - отсюда WARNING "Incorrect
 * netdev->dev_addr" на каждом интерфейсе при rmmod (01.10.2026).
 * Брат в rtl8192cd делал так же (eth_hw_addr_set).
 */
#include <linux/etherdevice.h>
static inline void rtl_dev_addr_set(struct net_device *dev, const u8 *addr)
{
	eth_hw_addr_set(dev, addr);
}

#endif /* _RTL8192CD_COMPAT_KERNEL_H */
