/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * RTL8197F: HAL таблиц L3/NAPT аппаратного NAT. Порт донора
 * ADCDS/openwrt-dlink-dir842-r1 (rtl865x_asichal.h, GPL-2.0, донор —
 * реконструкция по стоку 3.10.90 + вендорскому SDK, поля сверены).
 * Отличия нашей платы: WAN = VID 8 / гнездо 0, LAN = VID 9 / гнёзда 3-4,
 * LAN-сеть 192.168.0.0/24, WAN-сеть 192.168.1.0/24 ( learned живьём).
 */
#ifndef _RTL865X_ASICHAL_H
#define _RTL865X_ASICHAL_H

#include <linux/types.h>
#include <linux/mutex.h>

/* Идентичность WAN/LAN, единая для HAL и rtl819x_hwnat.c */
#define RTL865X_VID_WAN		8		/* eth0.8, гнездо 0 (синий WAN) */
#define RTL865X_VID_LAN		9		/* eth0.9, гнёзда 3-4 (LAN1/LAN2) */
#define RTL865X_WAN_EXTIP	0xC0A80103	/* 192.168.1.3 — ЗАГРУЗОЧНЫЙ вариант;
						 * живое значение — rtl865x_wan_extip,
						 * учится по потокам (M7.2 донора) */
#define RTL865X_NAPT_ROWS	1024		/* глубина L4-таблицы. Программно
						 * адресуется ПЛОСКО (0..1023) при любом
						 * EnL4WayH — вендор индекс не меняет */
#define RTL865X_NAPT_AGING_RELOAD	0x11	/* перезарядка 6-битного таймера
						 * строки: ~102 с простоя до
						 * авто-очистки строка от ASIC */
/* индексы L3-интерфейсов ASIC (netif), НЕ VLAN-теги. В nexthop.dstVid едет
 * именно этот индекс (3 бита) — вендор: asic.dvid = dstNetif->asicIdx */
#define RTL865X_NETIF_LAN	0
#define RTL865X_NETIF_WAN	1

/* типы таблиц (enum TYPE_* вендорского asicBasic.h) */
#define ASIC_TYPE_L2_SWITCH	0
#define ASIC_TYPE_ARP		1
#define ASIC_TYPE_L3_ROUTING	2
#define ASIC_TYPE_MULTICAST	3
#define ASIC_TYPE_NETIF		4
#define ASIC_TYPE_EXT_INT_IP	5
#define ASIC_TYPE_VLAN		6
#define ASIC_TYPE_L4_TCP_UDP	9
#define ASIC_TYPE_PPPOE		11
#define ASIC_TYPE_ACL_RULE	12
#define ASIC_TYPE_NEXT_HOP	13

/* NAPT TCP/UDP строка (тип 9, 3 значащих слова из 32 байт). LE-ветка
 * вендорского rtl865x_asicL4.h, сверена со стоком. */
struct asic_napt_tcpudp {
	u32 intIPAddr;						/* слово 0 */
	/* слово 1 */
	u32 valid:1, collision:1, agingTime:6, offset:6,
	    collision2:1, dedicate:1, isStatic:1, selIPIdx:4,
	    selEIdx:10, reserv0:1;
	/* слово 2 (RTL8197F) */
	u32 intPort:16, TCPFlag:3, isTCP:1, priValid:1, priority:3,
	    NHIDXValid:1, NHIDX:5, reserv2:2;
	u32 reservw3, reservw4, reservw5, reservw6, reservw7;
};

/* L3-маршрут, вариант process=1 (L2/direct) */
struct asic_l3route_l2 {
	u32 ipAddr;						/* слово 0 */
	u32 ipMask:5, valid:1, process:3, internal:1, isDMZ:1,
	    netif:3, nextHop:10, reserv0:8;
	u32 reservw2;
	u32 reservw3, reservw4, reservw5, reservw6, reservw7;
};

/* nexthop (тип 13, 1 слово) */
struct asic_nexthop {
	u32 type:1, IPIndex:4, dstVid:3, PPPoEIndex:3, nextHop:10, reserv0:11;
	u32 reservw1, reservw2, reservw3, reservw4, reservw5, reservw6, reservw7;
};

/* ARP-строка (тип 1, 1 слово). nextHop здесь — указатель на СТРОКУ L2. */
struct asic_arp {
	u32 valid:1, nextHop:10, aging:5, reserv0:16;
	u32 reservw1, reservw2, reservw3, reservw4, reservw5, reservw6, reservw7;
};

/* L3-маршрут, вариант process=2 (ARP-окно подключённой сети) — как у стока
 * для LAN и WAN /24 */
struct asic_l3route_arp {
	u32 ipAddr;						/* слово 0 */
	u32 ipMask:5, valid:1, process:3, internal:1, isDMZ:1,
	    netif:3, ARPStart:6, ARPEnd:6, ARPIpIdx:3, DSLEG:1, DSL_IDX1_0:2;
	u32 reservw2, reservw3, reservw4, reservw5, reservw6, reservw7;
};

/* L3-маршрут, вариант process=5 (NAPT-NextHop, дефолтные маршруты) */
struct asic_l3route_nxthop {
	u32 ipAddr;						/* слово 0 */
	u32 ipMask:5, valid:1, process:3, internal:1, isDMZ:1,
	    nhNum:3, nhStart:4, nhNxt:5, nhAlgo:2, IPDomain:3, reserv0:1,
	    DSLEG:1, DSL_IDX1_0:2;
	u32 reservw2, reservw3, reservw4, reservw5, reservw6, reservw7;
};

/* VLAN-строка (тип 6, 3 слова) — для справки; нашей раскладкой владеет
 * rtl819x_eth.c, HAL её не трогает */
struct asic_vlan {
	u32 memberPort:6, extMemberPort:3, egressUntag:6, extEgressUntag:3,
	    fid:2, hp:3, reserved2:9;
	u32 reservw1, reservw2, reservw3, reservw4, reservw5, reservw6, reservw7;
};

/* netif (L3-интерфейс ASIC, тип 4, 5 слов). MAC разделён 18/29 бит. */
struct asic_netif {
	u32 valid:1, vid:12, mac18_0:19;				/* слово 0 */
	u32 mac47_19:29, enHWRoute:1, enHWRouteV6:1, inACLStartL:1;	/* слово 1 */
	u32 inACLStartH:7, inACLEnd:8, outACLStart:8, outACLEnd:8, macMaskL:1;	/* слово 2 */
	u32 macMaskH:2, mtu:15, mtuV6:15;				/* слово 3 */
	u32 reservw4, reservw5, reservw6, reservw7;
};

/* extIP (masquerade) строка (тип 5, 3 слова): internalIP=0 + isOne2One=0 =
 * many-to-one на externalIP */
struct asic_extintip {
	u32 internalIP;						/* слово 0 */
	u32 externalIP;						/* слово 1 */
	u32 valid:1, isOne2One:1, isLocalPublic:1,		/* слово 2 */
	    nextHop:5, reserv0:24;
	u32 reservw3, reservw4, reservw5, reservw6, reservw7;
};

/* ---- низкоуровневый движок таблиц ---------------------------------------
 * @force: вендорский CMD_FORCE (писать ровно в индекс) против CMD_ADD
 * (вставка по хешу с отчётом). Оба замораживают TLU. 0 = успех. */
int rtl865x_asic_write_entry(u32 type, u32 idx, const void *entry, bool force);
int rtl865x_asic_read_entry(u32 type, u32 idx, void *entry);

/* Включить L3-движок ASIC (сброс + enable + готовность). NAPT-пересылку и L2
 * не трогает — безопасно на живом мосту. */
int rtl865x_asic_l3_engine_enable(void);

/* ---- пер-потоковый API NAPT для rtl819x_hwnat.c -------------------------
 * Сериилизует ВСЕ изменения таблиц. Функции ниже без собственной блокировки:
 * вызывающий держит rtl865x_hal_lock на всю транзакцию. */
extern struct mutex rtl865x_hal_lock;

/* NAPT-индекс исходящего = HASH1(proto,intIP,intPort,remIP,remPort),
 * вендор дословно. Возвращаемое 10-битное значение = физическая строка. */
u32 gw_napt_hash1(u32 isTCP, u32 sip, u32 sport, u32 dip, u32 dport);

int rtl865x_napt_write(u32 idx, const struct asic_napt_tcpudp *e);
int rtl865x_napt_read(u32 idx, struct asic_napt_tcpudp *out);	/* двойное чтение */
int rtl865x_napt_clear(u32 idx);
void rtl865x_napt_prefill(void);

/* ---- динамическая идентичность WAN (M7.2 донора) ------------------------
 * Сеттеры теневые и идемпотентные: 0 = ASIC уже так настроен, 1 = таблицы
 * переписаны (вызывающий обязан сбросить строки NAPT), <0 = ошибка.
 * Вызывающий держит rtl865x_hal_lock. */
extern u32 rtl865x_wan_extip;	/* живой extIP[0] (host order) */
int rtl865x_set_wan_extip(u32 ip);
int rtl865x_wan_netif_mac_sync(void);
int rtl865x_wan_set_nexthop(const u8 *gw_mac);
int rtl865x_lan_set_nexthop(const u8 *mac, u32 ip, u32 jack_mask);

/* ARP-строка WAN-пира для адреса @dst_ip из подключённой WAN-сети (route[3],
 * process=RT_ARP): без неё поток к адресу WAN-подсети не резолвит egress и
 * трапает в CPU. 1 = записали, 0 = адрес вне WAN-подсети. Держать hal_lock. */
int rtl865x_wan_set_arp(u32 dst_ip);

#endif /* _RTL865X_ASICHAL_H */
