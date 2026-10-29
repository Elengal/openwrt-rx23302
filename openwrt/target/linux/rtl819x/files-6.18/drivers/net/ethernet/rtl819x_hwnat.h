/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * RTL8197F: мост между штатной flowtable ядра (ndo_setup_tc/TC_SETUP_FT +
 * TC_SETUP_CLSFLOWER) и таблицами NAPT (rtl865x_asichal.c).
 *
 * Донор ADCDS DIR-842 использовал старый downstream-интерфейс
 * ndo_flow_offload (ядро 4.14); здесь то же самое на штатном API 6.x,
 * по образцу mtk_ppe_offload.c:
 *   - bind:      flowtable с `flags offload` зове ndo_setup_tc(TC_SETUP_FT)
 *                (наши VLAN/мост — через flow_indr);
 *   - на поток:  FLOW_CLS_REPLACE / FLOW_CLS_DESTROY / FLOW_CLS_STATS
 *                с cookie = указатель кортежа потока;
 *   - жизненный цикл и таймауты: ГОВОРИТ САМО ЯДРО (flow_offload_work_stats
 *                продлевает timeout по нашему lastused) — донорский
 *                рукопожатный хак не нужен.
 */
#ifndef _RTL819X_HWNAT_H
#define _RTL819X_HWNAT_H

#include <linux/netdevice.h>

/* Рантайм-гейт: /sys/module/rtl819x_eth/parameters/hwnat, по умолчанию 0.
 * Выключен = все предложения потока отклоняются, строки не ставятся,
 * поведение как у драйвера без хуков (софт-флоутейбл работает как был). */
extern bool rtl819x_hwnat_enabled;

/* из rtl819x_eth.c: состояние режима роутера, гнездо MAC клиента, eth0 */
bool rtl819x_router_mode(void);
u32 rtl819x_lan_jack_mask(const u8 *mac);
struct net_device *rtl819x_eth_dev(void);

/* ndo_setup_tc драйвера eth0 (TC_SETUP_FT) */
int rtl819x_hwnat_setup_tc(struct net_device *dev, enum tc_setup_type type,
			   void *type_data);

#endif /* _RTL819X_HWNAT_H */
