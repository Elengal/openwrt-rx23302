// SPDX-License-Identifier: GPL-2.0-only
/*
 * RTL8197F: HAL таблиц L3/NAPT (движок таблиц + каркас шлюза).
 *
 * Порт rtl865x_asichal.c донора ADCDS/openwrt-dlink-dir842-r1 (GPL-2.0),
 * который реконструирован по стоковому ядру 3.10.90 и сверен полe-за-полем
 * с вендорским SDK (AsicDriver/rtl865x_asicBasic.c). У донора этот HAL —
 * фундамент их гигабитного NAT (891/896 Мбит/с при CPU ~0,3%).
 *
 * Отличия от донора (наша плата RX23302):
 *   - WAN = VID 8 / гнездо 0 (маска L2 0x01), LAN = VID 9 / гнёзда 3-4;
 *   - имена интерфейсов eth0.8 (WAN) / eth0.9 (LAN);
 *   - сети: LAN 192.168.0.0/24, WAN 192.168.1.0/24 (обе process=RT_ARP,
 *     как у стока), extIP живой — 192.168.1.x учится по потокам;
 *   - VLAN/PVID/MACCR1/P0GMIICR НЕ трогаем — ими владеет rtl819x_eth.c
 *     (раскладка портов и режим роутера);
 *   - PPPoE не портировали (стенд на ethernet-WAN).
 *
 * Порядок байтов — главный грабель донора, решён экспериментально:
 * ВСЁ в host-порядке (числовые значения): хеш-входы, ключ строки,
 * кодировка G (offset/selEIdx), хеш проверки. Вендор кладёт htonl/htons
 * на LE-сборке — и его «сетевой порядок» совпадает с числовым; двойное
 * преобразование у донора месяц уходило в переставленный индекс.
 */
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/string.h>
#include <linux/mutex.h>
#include <linux/netdevice.h>
#include <linux/inetdevice.h>
#include <linux/etherdevice.h>
#include <linux/moduleparam.h>
#include <linux/uaccess.h>
#include <linux/capability.h>
#include <linux/sched.h>

#include "rtl819x_regs.h"
#include "rtl865x_asichal.h"

/* Сериализация ВСЕХ изменений таблиц: gw_prog, /proc-сканеры, hwnat. */
DEFINE_MUTEX(rtl865x_hal_lock);

/* ---- ручки для опытов (по умолчанию — проверенная конфигурация) -------- */
static int swtcr1_override = -1;
module_param(swtcr1_override, int, 0644);
MODULE_PARM_DESC(swtcr1_override, "сырое значение SWTCR1 (<0 = считать 0x2200)");

static int sel_cpu_reason;
module_param(sel_cpu_reason, int, 0644);
MODULE_PARM_DESC(sel_cpu_reason,
	"SWTCR1 бит8 EN_51B_CPU_REASON: 1 = 51-битная кодировка why2cpu, "
	"расшифровываемая моделью l34Model (по умолч. 0 = легаси)");

static int swtcr1_trap_bits;
module_param(swtcr1_trap_bits, int, 0644);
MODULE_PARM_DESC(swtcr1_trap_bits,
	"OR EnNATT2LOG|ENFRAGTOACLPT в SWTCR1 (0=выкл -> 0x2200, 1=вкл -> 0x2E00)");

static int napt_collision_prefill;
module_param(napt_collision_prefill, int, 0644);
MODULE_PARM_DESC(napt_collision_prefill,
	"предзаполнить L4-таблицу collision=collision2=1, как rtl865x_nat_init "
	"стока (0=выкл; бездумное применение ломало трафик у донора)");

static int ffcr_unkuc_to_cpu;
module_param(ffcr_unkuc_to_cpu, int, 0644);
MODULE_PARM_DESC(ffcr_unkuc_to_cpu,
	"FFCR unknown-unicast: 0=флуд (вендор, по умолч.), 1=в CPU, -1=не трогать");

static int multiport_mode = 0x3F;
module_param(multiport_mode, int, 0644);
MODULE_PARM_DESC(multiport_mode,
	"SWTCR0 MultiPortModeP [13:5] (по умолч. 0x3F = порты 0-5 внешние)");

static int wan_route_mode;
module_param(wan_route_mode, int, 0644);
MODULE_PARM_DESC(wan_route_mode,
	"SWTCR0 WANRouteMode биты[4:3]: 0=Forward (по умолч.), 1=ToCpu, 2=Drop. "
	"Применяется `cat /proc/rtl865x_gw`");

/* маска гнёзд для L2-строк сторон (одно гнездо! флуд-маска ломает
 * аппаратную пересылку — донор это измерил). WAN всегда гнездо 0. */
static int l2_mask_lan = 0x08;		/* гнездо 3 = LAN1 (стендовый клиент) */
module_param(l2_mask_lan, int, 0644);
MODULE_PARM_DESC(l2_mask_lan, "маска гнезда клиента LAN (по умолч. 0x08 = LAN1)");
#define L2_MASK_WAN		0x01		/* гнездо 0 = синий WAN */

/* ---- движок таблиц ------------------------------------------------------ */
#define ASIC_READ_RETRIES	10		/* вендорский READ_MULTIPLECHECK */
#define ASIC_TBL_MAX_WORDS	11
#define ASIC_POLL_MAX		200000

#define ASIC_ADDR(type, idx)	(RTL819X_SWTBL_BASE + ((type) << 16) + ((idx) << 5))

/* Слов на таблицу (вендорский _rtl8651_asicTableSize[], ветка 8197F). */
static const u8 asic_tbl_words[] = {
	2,  /*  0 L2_SWITCH   */	1,  /*  1 ARP        */	3,  /*  2 L3_ROUTING */
	3,  /*  3 MULTICAST   */	5,  /*  4 NETIF      */	3,  /*  5 EXT_INT_IP */
	3,  /*  6 VLAN        */	3,  /*  7 VLAN1      */	4,  /*  8 SERVER_PORT*/
	3,  /*  9 L4_TCP_UDP  */	3,  /* 10 L4_ICMP    */	1,  /* 11 PPPOE      */
	11, /* 12 ACL_RULE    */	1,  /* 13 NEXT_HOP   */	3,  /* 14 RATE_LIMIT */
	1,  /* 15 ALG         */	9,  /* 16 DS_LITE    */	6,  /* 17 6RD        */
	6,  /* 18 L3_V6_ROUTE */	1,  /* 19 NEXT_HOP_V6*/	3,  /* 20 ARP_V6     */
	9,  /* 21 MULTICAST_V6*/
};

static int asic_wait_action_done(void)
{
	u32 g = 0;

	while ((REG32(SWTACR) & TLU_ACTION_MASK) != TLU_ACTION_DONE)
		if (++g > ASIC_POLL_MAX)
			return -ETIMEDOUT;
	return 0;
}

int rtl865x_asic_l3_engine_enable(void)
{
	u32 g = 0;

	REG32(ASIC_L3_ENGINE_CFG) = 0;		/* сброс */
	REG32(ASIC_L3_ENGINE_CFG) = 8;		/* enable */
	while (!(REG32(ASIC_L3_ENGINE_CFG) & L3_ENGINE_READY))
		if (++g > ASIC_POLL_MAX)
			return -ETIMEDOUT;
	return 0;
}

int rtl865x_asic_write_entry(u32 type, u32 idx, const void *entry, bool force)
{
	const u32 *w = entry;
	int i, n, rc = 0;
	u32 g = 0;

	if (type >= ARRAY_SIZE(asic_tbl_words) || !entry)
		return -EINVAL;
	n = asic_tbl_words[type];

	/* Заморозить поиск на время записи (вендорский RTL819X_TLU_CHECK). */
	REG32(SWTCR0) |= EN_STOP_TLU;
	while (!(REG32(SWTCR0) & STOP_TLU_READY))
		if (++g > ASIC_POLL_MAX) { rc = -ETIMEDOUT; goto out; }

	rc = asic_wait_action_done();
	if (rc)
		goto out;

	for (i = 0; i < n; i++)
		REG32(TCR(i)) = w[i];
	REG32(SWTAA) = ASIC_ADDR(type, idx);
	REG32(SWTACR) = TLU_ACTION_START | (force ? TLU_CMD_FORCE : CMD_ADD);

	rc = asic_wait_action_done();
	if (rc)
		goto out;

	if (!force && (REG32(SWTASR) & TABSTS_MASK) != TABSTS_SUCCESS)
		rc = -EIO;
out:
	REG32(SWTCR0) &= ~EN_STOP_TLU;
	return rc;
}

int rtl865x_asic_read_entry(u32 type, u32 idx, void *entry)
{
	volatile u32 *base;
	u32 *w = entry;
	u32 chk[ASIC_TBL_MAX_WORDS];
	int i, n, rc, try;

	if (type >= ARRAY_SIZE(asic_tbl_words) || !entry)
		return -EINVAL;
	n = asic_tbl_words[type];

	rc = asic_wait_action_done();
	if (rc)
		return rc;

	base = (volatile u32 *)(uintptr_t)ASIC_ADDR(type, idx);

	/* ★ Холостое чтение — ДО рабочего, не после (урок донора): иначе
	 * защёлка держит прошлый доступ и запись+чтение подтверждают сами
	 * себя, пока RAM не обновлён. Плюс двойное чтение с повтором —
	 * окно mapped-доступа на этом кремнии умеет врать. */
	for (try = 0; try < ASIC_READ_RETRIES; try++) {
		(void)REG32(ASIC_ADDR(ASIC_TYPE_ACL_RULE, 1024));
		for (i = 0; i < n; i++)
			w[i] = base[i];

		(void)REG32(ASIC_ADDR(ASIC_TYPE_ACL_RULE, 1024));
		for (i = 0; i < n; i++)
			chk[i] = base[i];

		if (!memcmp(w, chk, n * sizeof(u32)))
			return 0;
	}

	pr_warn_ratelimited("rtl865x: чтение таблицы type=%u idx=%u нестабильно после %d попыток\n",
			    type, idx, ASIC_READ_RETRIES);
	return -EIO;
}

/* ---- L2-строки: хеш и записи (донор, сверено со стоком) ------------------ */

/* L2-строка = (MAC[0]^..^MAC[5]^fh[fid]) & 0xFF, fh = {00,0f,f0,ff} */
static u32 gw_l2_row(const u8 *m, u32 fid)
{
	static const u8 fh[4] = { 0x00, 0x0f, 0xf0, 0xff };

	return (m[0] ^ m[1] ^ m[2] ^ m[3] ^ m[4] ^ m[5] ^ fh[fid]) & 0xFF;
}

/* nexthop-строка {mac -> порт, fid, static, NH}. Возвращает индекс
 * (row<<2) — он же указатель nextHop в ARP/nexthop. */
static u32 gw_write_l2(const u8 *m, u32 member_portmask, u32 fid)
{
	u32 row = gw_l2_row(m, fid);
	u32 e[8] = { 0 };

	e[0] = (m[1] << 24) | (m[2] << 16) | (m[3] << 8) | m[4];
	/* mac[0] | member<<8 | STA(1<<18) | age3(3<<19) | NH(1<<22) | fid<<23 */
	e[1] = m[0] | (member_portmask << 8) | (1u << 18) | (3u << 19) |
	       (1u << 22) | (fid << 23);
	rtl865x_asic_write_entry(ASIC_TYPE_L2_SWITCH, (row << 2) | 0, e, true);
	return (row << 2) | 0;
}

/* Полная запись: member/extmember/toCPU/fid/auth — для broadcast и CPU-MAC. */
static u32 gw_write_l2_full(const u8 *m, u32 member, u32 extmember,
			    u32 tocpu, u32 fid, u32 auth)
{
	u32 row = gw_l2_row(m, fid);
	u32 e[8] = { 0 };

	e[0] = (m[1] << 24) | (m[2] << 16) | (m[3] << 8) | m[4];
	e[1] = m[0] | (member << 8) | (extmember << 14) | (tocpu << 17) |
	       (1u << 18) | (3u << 19) | (1u << 22) | (fid << 23) | (auth << 25);
	rtl865x_asic_write_entry(ASIC_TYPE_L2_SWITCH, (row << 2) | 0, e, true);
	return (row << 2) | 0;
}

/* ---- тени идентичности (охраняются rtl865x_hal_lock) -------------------- */

/* живые MAC с интерфейсов; при их отсутствии — прошлая тень, затем константа */
static u8 gw_wan_ifmac_shadow[ETH_ALEN];
static u8 gw_lan_ifmac_shadow[ETH_ALEN];

static const u8 GW_MAC_FALLBACK[6] = { 0x02, 0xe0, 0x4c, 0x81, 0x96, 0xc1 };

static void gw_netif_mac(const char *ifname, u8 out[ETH_ALEN])
{
	struct net_device *dev = dev_get_by_name(&init_net, ifname);

	if (dev && is_valid_ether_addr(dev->dev_addr)) {
		ether_addr_copy(out, dev->dev_addr);
	} else {
		/* у eth0.8/eth0.9 адрес наследуется от eth0: читаем и его */
		struct net_device *low = dev ? NULL :
			dev_get_by_name(&init_net, "eth0");

		if (low && is_valid_ether_addr(low->dev_addr))
			ether_addr_copy(out, low->dev_addr);
		else
			ether_addr_copy(out, GW_MAC_FALLBACK);
		if (low)
			dev_put(low);
	}
	if (dev)
		dev_put(dev);
}

/* макросклон длины префикса -> asicMask вендора: 31 - младший бит маски */
static u32 gw_asicmask(u32 prefix_len)
{
	return 31 - (32 - prefix_len);
}

/* ---- ARP-окна (СТОК-ПАРИТЕТ донора): индекс НЕ хеш, а позиция:
 *     arpIndex = arpsta_raw + (ip & ~mask)
 * LAN /24 владеет сырыми строками 0..255 (поле 0..31), WAN — 256..504
 * (поле 32..63). nexthop -> ARP[20] (6 бит, окно донора). ---------------- */
#define GW_ARP_NH_IDX		20
#define GW_ARP_WIN_LAN		0
#define GW_ARP_WIN_WAN		256

static u8 gw_wan_gw_mac[6] = { 0xaa, 0xbb, 0xcc, 0x00, 0x00, 0x03 };
static u8 gw_lan_client_mac[6] = { 0xaa, 0xbb, 0xcc, 0x00, 0x00, 0x01 };
static u32 gw_lan_client_ip = 0xC0A80002;		/* 192.168.0.2 */
/* L2-строка WAN-пира (индекс строки), кладётся при программировании цепочки;
 * нужна, чтобы писать ARP-строки подключённой WAN-сети (route[3], process=ARP) */
static u32 gw_wan_nh;
u32 rtl865x_wan_extip = RTL865X_WAN_EXTIP;

/* ---- NAPT: хеш1 вендора (rtl8651_naptTcpUdpTableIndex), ДОСЛОВНО -------- */
u32 gw_napt_hash1(u32 isTCP, u32 sip, u32 sport, u32 dip, u32 dport)
{
	return ((sport & 0x3ff)
		^ (((sport & 0xfc00) >> 10) | ((sip & 0xf) << 6))
		^ ((sip >> 4) & 0x3ff)
		^ ((sip >> 14) & 0x3ff)
		^ (((sip & 0xff000000) >> 24) | ((isTCP & 1) << 8) | ((dport & 1) << 9))
		^ ((dport >> 1) & 0x3ff)
		^ (((dport >> 11) & 0x1f) | ((dip & 0x1f) << 5))
		^ ((dip >> 5) & 0x3ff)
		^ ((dip >> 15) & 0x3ff)
		^ ((dip >> 25) & 0x7f)) & 0x3ff;
}

/* ---- пер-потоковые помощники NAPT (вызывающий держит hal_lock) ---------- */
int rtl865x_napt_write(u32 idx, const struct asic_napt_tcpudp *e)
{
	if (idx >= RTL865X_NAPT_ROWS || !e)
		return -EINVAL;
	return rtl865x_asic_write_entry(ASIC_TYPE_L4_TCP_UDP, idx, e, true);
}

int rtl865x_napt_clear(u32 idx)
{
	struct asic_napt_tcpudp z;

	if (idx >= RTL865X_NAPT_ROWS)
		return -EINVAL;
	/* намеренно НОЛЬ: это путь сноса ЖИВЫХ потоков; collision-биты здесь
	 * измеренно роняли трафик в 100% потерь (донор R6/B3) */
	memset(&z, 0, sizeof(z));
	return rtl865x_asic_write_entry(ASIC_TYPE_L4_TCP_UDP, idx, &z, true);
}

void rtl865x_napt_prefill(void)
{
	struct asic_napt_tcpudp t;
	u32 i;

	/* как rtl865x_nat_init стока: ТОЛЬКО на инициализации, до потоков */
	memset(&t, 0, sizeof(t));
	t.collision = 1;
	t.collision2 = 1;
	for (i = 0; i < RTL865X_NAPT_ROWS; i++)
		rtl865x_asic_write_entry(ASIC_TYPE_L4_TCP_UDP, i, &t, true);
}

int rtl865x_napt_read(u32 idx, struct asic_napt_tcpudp *out)
{
	int rc;

	if (idx >= RTL865X_NAPT_ROWS || !out)
		return -EINVAL;
	/* 6-битный agingTime живёт на защёлке — читается вторым заходом */
	rc = rtl865x_asic_read_entry(ASIC_TYPE_L4_TCP_UDP, idx, out);
	if (rc)
		return rc;
	return rtl865x_asic_read_entry(ASIC_TYPE_L4_TCP_UDP, idx, out);
}

/* ---- extIP[0]: masquerade-адрес ----------------------------------------- */
int rtl865x_set_wan_extip(u32 ip)
{
	struct asic_extintip ext;
	int rc;

	if (!ip)
		return -EINVAL;
	if (ip == rtl865x_wan_extip)
		return 0;
	memset(&ext, 0, sizeof(ext));
	ext.externalIP = ip;	/* числовое: ASIC сверяет как слово */
	ext.valid = 1;
	ext.nextHop = 0;	/* -> nexthop[0] для обратного пути */
	rc = rtl865x_asic_write_entry(ASIC_TYPE_EXT_INT_IP, 0, &ext, true);
	if (rc)
		return rc;
	rtl865x_wan_extip = ip;
	pr_info("rtl865x gw: extIP[0] -> %pI4h\n", &rtl865x_wan_extip);
	return 1;
}

/* ---- netif[1] (WAN L3-интерфейс) из теней -------------------------------- */
static void gw_wan_netif_prog_locked(void)
{
	struct asic_netif nif;
	u32 mac_hi, mac_lo;
	u8 mac[ETH_ALEN];

	memset(&nif, 0, sizeof(nif));
	gw_netif_mac("eth0.8", mac);
	memcpy(gw_wan_ifmac_shadow, mac, ETH_ALEN);
	mac_hi = (mac[0] << 21) | (mac[1] << 13) | (mac[2] << 5) | (mac[3] >> 3);
	mac_lo = ((mac[3] & 0x7) << 16) | (mac[4] << 8) | mac[5];
	nif.valid = 1; nif.vid = RTL865X_VID_WAN;
	nif.mac18_0 = mac_lo; nif.mac47_19 = mac_hi;
	nif.enHWRoute = 1; nif.macMaskL = 1; nif.macMaskH = 3;
	nif.mtu = 1500;
	/* входной ACL [4..6] — как у стока (классификатор «dst == наш WAN MAC»
	 * в слоте 4). start = (inACLStartH << 1) | inACLStartL: 4 => H=2, L=0 */
	nif.inACLStartL = 0; nif.inACLStartH = 2; nif.inACLEnd = 6;
	nif.outACLStart = 253; nif.outACLEnd = 253;
	rtl865x_asic_write_entry(ASIC_TYPE_NETIF, 1, &nif, true);
}

/* ---- цепочка исходящего WAN: L2(пир) -> ARP[20] -> nexthop[0..1] --------- */
static u32 gw_wan_nexthop_prog_locked(void)
{
	struct asic_nexthop nh;
	struct asic_arp a;
	u32 wan_nh;

	wan_nh = gw_write_l2(gw_wan_gw_mac, L2_MASK_WAN, 1);	/* WAN-гнездо, fid 1 */
	gw_wan_nh = wan_nh;

	memset(&a, 0, sizeof(a));
	a.valid = 1;
	a.nextHop = wan_nh;	/* ARP -> строка L2 (не напрямую L2!) */
	a.aging = 0x1f;		/* старение L2/ARP всё равно стоит TEACR бит0 */
	rtl865x_asic_write_entry(ASIC_TYPE_ARP, GW_ARP_NH_IDX, &a, true);

	memset(&nh, 0, sizeof(nh));
	nh.nextHop = GW_ARP_NH_IDX;	/* nexthop смотрит в ARP, не в L2 */
	/* ★ dstVid — НЕ тег 802.1Q, а ИНДЕКС netif'а (0..7), поле всего 3 бита.
	 * Вендор: asic.dvid = dstNetif->asicIdx (l3Driver/rtl865x_nexthop.c).
	 * У донора VID WAN=1 и индекс netif=1 совпали — случайность. Наш WAN —
	 * netif[1] при VID 8: писать сюда VID нельзя (8 в 3 бита не влезает). */
	nh.dstVid = RTL865X_NETIF_WAN;
	nh.IPIndex = 0;			/* -> extIP[0]: источник перезаписи */
	rtl865x_asic_write_entry(ASIC_TYPE_NEXT_HOP, 0, &nh, true);
	rtl865x_asic_write_entry(ASIC_TYPE_NEXT_HOP, 1, &nh, true);
	return wan_nh;
}

int rtl865x_wan_set_nexthop(const u8 *gw_mac)
{
	if (!gw_mac)
		return -EINVAL;
	if (!memcmp(gw_wan_gw_mac, gw_mac, 6))
		return 0;

	memcpy(gw_wan_gw_mac, gw_mac, 6);
	gw_wan_netif_prog_locked();
	gw_wan_nexthop_prog_locked();
	pr_info("rtl865x gw: WAN nexthop -> %pM\n", gw_wan_gw_mac);
	return 1;
}

int rtl865x_wan_netif_mac_sync(void)
{
	struct net_device *dev = dev_get_by_name(&init_net, "eth0.8");
	bool stale;

	if (!dev)
		return 0;
	stale = is_valid_ether_addr(dev->dev_addr) &&
		!ether_addr_equal(dev->dev_addr, gw_wan_ifmac_shadow);
	dev_put(dev);
	if (!stale)
		return 0;
	gw_wan_netif_prog_locked();
	return 1;
}

/* Лежит ли @ip в подсети живого eth0.8 (адрес и маска читаются с netdev).
 * Нужно для route[3]: WAN /24 идёт процессом RT_ARP, т.е. ASIC ищет
 * ARP-строку в окне 256.. и без неё не может резолвить egress (поток
 * к адресу из WAN-подсети уходил в CPU — причина 5 «L34 action required»). */
static bool gw_wan_ip_in_subnet(u32 ip)
{
	struct net_device *dev = dev_get_by_name(&init_net, "eth0.8");
	struct in_device *in_dev;
	struct in_ifaddr *ifa;
	bool in = false;

	if (!dev)
		return false;
	in_dev = in_dev_get(dev);
	dev_put(dev);
	if (!in_dev)
		return false;
	rcu_read_lock();
	in_dev_for_each_ifa_rcu(ifa, in_dev) {
		u32 addr = ntohl(ifa->ifa_address);
		u32 mask = ntohl(ifa->ifa_mask);

		if (addr && mask && (ip & mask) == (addr & mask)) {
			in = true;
			break;
		}
	}
	rcu_read_unlock();
	in_dev_put(in_dev);
	return in;
}

/* ARP-строка WAN-пира для адреса @dst_ip из подключённой WAN-сети:
 * route[3] (process=RT_ARP) резолвит его через окно 256..511, строка =
 * 256 + (ip & 0xff). 1 = записали, 0 = адрес вне WAN-подсети. */
int rtl865x_wan_set_arp(u32 dst_ip)
{
	struct asic_arp a;

	if (!gw_wan_nh || !gw_wan_ip_in_subnet(dst_ip))
		return 0;
	memset(&a, 0, sizeof(a));
	a.valid = 1;
	a.nextHop = gw_wan_nh;	/* L2-строка WAN-пира */
	a.aging = 0x1f;
	rtl865x_asic_write_entry(ASIC_TYPE_ARP,
				 GW_ARP_WIN_WAN + (dst_ip & 0xff), &a, true);
	return 1;
}

/* ---- цепочка исходящего LAN: L2(клиент) + ARP-строка его хоста ----------- */
static void gw_lan_nexthop_prog_locked(u32 jack_mask)
{
	gw_write_l2(gw_lan_client_mac, jack_mask, 0);	/* LAN-гнездо, fid 0 */
	{
		struct asic_arp a;
		u32 l2 = gw_l2_row(gw_lan_client_mac, 0) << 2;

		memset(&a, 0, sizeof(a));
		a.valid = 1;
		a.nextHop = l2;
		a.aging = 0x1f;
		rtl865x_asic_write_entry(ASIC_TYPE_ARP,
					 GW_ARP_WIN_LAN + (gw_lan_client_ip & 0xff),
					 &a, true);
	}
	/* /32-маршрута к клиенту НЕТ (сток-паритет): клиент достигается через
	 * route[2] (LAN /24, process=RT_ARP) по своей ARP-строке */
	{
		struct asic_l3route_l2 dead;

		memset(&dead, 0, sizeof(dead));
		rtl865x_asic_write_entry(ASIC_TYPE_L3_ROUTING, 1, &dead, true);
	}
}

int rtl865x_lan_set_nexthop(const u8 *mac, u32 ip, u32 jack_mask)
{
	if (!mac || !ip || !jack_mask)
		return -EINVAL;
	if (!memcmp(gw_lan_client_mac, mac, 6) && gw_lan_client_ip == ip)
		return 0;

	memcpy(gw_lan_client_mac, mac, 6);
	gw_lan_client_ip = ip;
	gw_lan_nexthop_prog_locked(jack_mask);
	pr_info("rtl865x gw: LAN клиент -> %pM %pI4h (гнездо %02x)\n",
		gw_lan_client_mac, &gw_lan_client_ip, jack_mask);
	return 1;
}

/* ---- каркас шлюза: программирование + обратное чтение -------------------
 * Запускается `cat /proc/rtl865x_gw` (чтение НЕ пассивно — это запуск!). */
#define MSCR_EN_L2		(1 << 0)
#define MSCR_EN_L3		(1 << 1)
#define MSCR_EN_L4		(1 << 2)
#define MSCR_EN_IN_ACL		(1 << 4)
#define GW_ALECR_EN_TTL1	(1 << 16)
#define GW_CSCR_L3L4CHK		((1 << 4) | (1 << 5))
#define GW_FFCR_UNKUC_2CPU	(1u << 1)

static int gw_prog(struct seq_file *m, void *v)
{
	struct asic_netif nif;
	struct asic_l3route_arp rt;
	struct asic_l3route_nxthop nr;
	u32 mac_hi, mac_lo, rb[8];
	u32 mscr0, mscr1, wan_nh;
	int rc, ok = 1;

	mutex_lock(&rtl865x_hal_lock);

	seq_puts(m, "[rtl865x каркас шлюза — программирование + чтение]\n");

	/* 1. L3-движок + слои L2|L3|L4, входной ACL ВКЛ (это классификатор
	 * обратного NAPT «dst == наш WAN IP»; без него ответы транзитом
	 * уходят обратно в WAN — флуд, измерено донором) */
	rc = rtl865x_asic_l3_engine_enable();
	if (rc)
		pr_err("rtl865x gw: L3-движок не поднялся (%d)\n", rc);
	mscr0 = REG32(MSCR);
	REG32(MSCR) = REG32(MSCR) | MSCR_EN_L2 | MSCR_EN_L3 | MSCR_EN_L4 |
		      MSCR_EN_IN_ACL;
	mscr1 = REG32(MSCR);
	REG32(ALECR) |= GW_ALECR_EN_TTL1;	/* TTL-- при маршрутизации */

	/* 2. FFCR: unknown-unicast флудит (вендорный режим шлюза) — строка с
	 * битом «в CPU» отдаёт маршрутизированный кадр с промахом L2
	 * процессору и выключает аппаратную пересылку (донор измерил) */
	if (ffcr_unkuc_to_cpu == 0)
		REG32(FFCR) &= ~GW_FFCR_UNKUC_2CPU;
	else if (ffcr_unkuc_to_cpu > 0)
		REG32(FFCR) |= GW_FFCR_UNKUC_2CPU;
	pr_info("rtl865x gw: FFCR=%08x (unkUC2CPU ручка=%d)\n",
		REG32(FFCR), ffcr_unkuc_to_cpu);

	/* 3. пересчёт сумм L3/L4 после TTL-- и перезаписи адресов — иначе
	 * получатель выбрасывает кадр */
	REG32(CSCR) |= GW_CSCR_L3L4CHK;

	/* 4. SWTCR0: политика NAPT — промах в CPU (бит0=0), авто-обучение
	 * выкл (бит1=0), авто-удаление строк по старению ВКЛ (бит2=1),
	 * не-TCP/UDP L4 в CPU (бит14=1); MultiPortModeP из ручки */
	REG32(SWTCR0) = ((REG32(SWTCR0) & ~(3u << 16) & ~(3u << 3) & ~0x7u
			  & ~(0x1FFu << 5))
			 | (((u32)multiport_mode & 0x1FFu) << 5))
		      | (1u << 14) | (1u << 2)
		      | ((u32)(wan_route_mode & 3u) << 3);

	/* 5. старение: L2/ARP стоят (TEACR бит0), L4 тикает (бит1) — поле
	 * agingTime строки единственный признак активности; TEATCR = перезарядка
	 * 0x11 (~102 с) по всем протоколам */
	REG32(TEACR) = 0x1;
	REG32(TEATCR) = (RTL865X_NAPT_AGING_RELOAD << 24) |
			(RTL865X_NAPT_AGING_RELOAD << 18) |
			(RTL865X_NAPT_AGING_RELOAD << 12) |
			(RTL865X_NAPT_AGING_RELOAD << 6) |
			RTL865X_NAPT_AGING_RELOAD;

	/* 6. входной ACL = permit-all на все 256 строк (нулевая строка НЕ
	 * permit: её pktOpApp=0 «ни на каком слое» роняет маршрутизацию).
	 * слово7 = 0x07000000: MAC-правило, PERMIT, все слои */
	{
		u32 acl_permit[11] = { 0 };
		int a;

		acl_permit[7] = 0x07000000u;
		for (a = 0; a < 256; a++)
			rtl865x_asic_write_entry(ASIC_TYPE_ACL_RULE, a,
						 acl_permit, true);
		/* ACL по умолчанию: вендор пишет PERMIT_ALL(253) во все четыре
		 * поля — 0xFDFDFDFD */
		REG32(DACLRCR) = (253u << 0) | (253u << 8) | (253u << 16) |
				 (253u << 24);
	}

	/* 7. netif[0] = LAN (VID 9), netif[1] = WAN (VID 8) */
	memset(&nif, 0, sizeof(nif));
	{
		u8 lmac[ETH_ALEN];

		gw_netif_mac("eth0.9", lmac);
		memcpy(gw_lan_ifmac_shadow, lmac, ETH_ALEN);
		mac_hi = (lmac[0] << 21) | (lmac[1] << 13) | (lmac[2] << 5) |
			 (lmac[3] >> 3);
		mac_lo = ((lmac[3] & 0x7) << 16) | (lmac[4] << 8) | lmac[5];
	}
	nif.valid = 1; nif.vid = RTL865X_VID_LAN;
	nif.mac18_0 = mac_lo; nif.mac47_19 = mac_hi;
	nif.enHWRoute = 1; nif.macMaskL = 1; nif.macMaskH = 3;
	nif.mtu = 1500;
	nif.inACLStartL = 0; nif.inACLStartH = 0; nif.inACLEnd = 3;
	nif.outACLStart = 253; nif.outACLEnd = 253;
	rtl865x_asic_write_entry(ASIC_TYPE_NETIF, 0, &nif, true);
	gw_wan_netif_prog_locked();

	/* 8. базовые строки L2: broadcast (гнёзда + CPU) и CPU-MAC-ловушка
	 * (промахи route/ARP попадают в процессор через неё), обе FID */
	{
		static const u8 BC[6]  = { 0xff,0xff,0xff,0xff,0xff,0xff };
		static const u8 CPM[6] = { 0x00,0x00,0x0a,0x00,0x00,0x0f };
		u32 bc_mbr = 0x1f;	/* гнёзда 0-4 */
		u32 ext    = 0x4;	/* порт процессора 8 */

		gw_write_l2_full(BC,  bc_mbr, ext, 1, 0, 1);
		gw_write_l2_full(CPM, 0x00,   ext, 1, 0, 1);
		gw_write_l2_full(CPM, 0x00,   ext, 1, 1, 1);
		gw_write_l2_full(BC,  bc_mbr, ext, 1, 1, 1);
	}

	/* 9. цепочки сторон из теней */
	wan_nh = gw_wan_nexthop_prog_locked();

	if (napt_collision_prefill)
		rtl865x_napt_prefill();

	/* 10. маршруты:
	 *  [2] LAN 192.168.0.0/24 -> netif0, process=RT_ARP, окно 0..31
	 *      (сырые ARP-строки 0..255)
	 *  [3] WAN 192.168.1.0/24 -> netif1, process=RT_ARP, окно 32..63
	 *      (сырые 256..504) — ВКЛЮЧЁН по умолчанию: наша WAN — настоящая
	 *      подсеть, и сток всегда так делает для подключённых сетей
	 *  [6]/[7] дефолт 0/0 (обе половины) -> process=5 NAPT-NextHop,
	 *      nexthop[0..1] -> ARP[20] -> L2 WAN-пира; промах NAPT — в CPU */
	memset(&rt, 0, sizeof(rt));
	rt.ipAddr = 0xC0A80000;			/* 192.168.0.0/24 -> LAN */
	rt.ipMask = gw_asicmask(24); rt.valid = 1; rt.process = 2; rt.netif = 0;
	rt.internal = 1; rt.ARPStart = 0; rt.ARPEnd = 31;
	rtl865x_asic_write_entry(ASIC_TYPE_L3_ROUTING, 2, &rt, true);

	memset(&rt, 0, sizeof(rt));
	rt.ipAddr = 0xC0A80100;			/* 192.168.1.0/24 -> WAN */
	rt.ipMask = gw_asicmask(24); rt.valid = 1; rt.process = 2; rt.netif = 1;
	rt.internal = 0; rt.ARPStart = 32; rt.ARPEnd = 63; rt.ARPIpIdx = 0;
	rtl865x_asic_write_entry(ASIC_TYPE_L3_ROUTING, 3, &rt, true);

	memset(&nr, 0, sizeof(nr));
	nr.ipAddr = 0x80000000;			/* 128.0.0.0 (верхняя половина) */
	nr.ipMask = 0; nr.valid = 1;
	nr.process = 5;				/* NAPT NextHop: включает NAT */
	nr.internal = 0;
	nr.nhStart = 0; nr.nhNum = 0;		/* nexthop[0..1] (nhNum=0 => 2) */
	nr.nhNxt = 0; nr.nhAlgo = 2; nr.IPDomain = 6;
	rtl865x_asic_write_entry(ASIC_TYPE_L3_ROUTING, 6, &nr, true);
	nr.ipAddr = 0;				/* 0.0.0.0 (нижняя половина) */
	rtl865x_asic_write_entry(ASIC_TYPE_L3_ROUTING, 7, &nr, true);

	/* 11. SWTCR1 = 0x2200: EnL4WayH(бит9) | L4EnHash1(бит13) — вендор
	 * включает оба; enhanced-hash1 нужен для обратного NAPT, снятие
	 * EnL4WayH валило весь L4 (донор). Размещение строк не меняет: индекс
	 * остаётся HASH1 при обоих режимах. */
	REG32(SWTCR1) = (swtcr1_override >= 0) ? (u32)swtcr1_override
		      : ((1u << 9) | (1u << 13)
			 | (sel_cpu_reason ? (1u << 8) : 0u)
			 | (swtcr1_trap_bits ? ((1u << 10) | (1u << 11)) : 0u));

	/* 12. extIP[0] из тени (динамический) */
	{
		struct asic_extintip ext;

		memset(&ext, 0, sizeof(ext));
		ext.externalIP = rtl865x_wan_extip;
		ext.valid = 1;
		ext.nextHop = 0;
		rtl865x_asic_write_entry(ASIC_TYPE_EXT_INT_IP, 0, &ext, true);
	}

	/* ---- обратное чтение ---- */
	seq_puts(m, "чтение:\n");
	rtl865x_asic_read_entry(ASIC_TYPE_NETIF, 0, rb);
	seq_printf(m, "  NETIF0 w0=%08x w1=%08x w2=%08x w3=%08x (LAN VID9; inACLEnd=(w2>>7)&0xff хотим 3)\n",
		   rb[0], rb[1], rb[2], rb[3]);
	rtl865x_asic_read_entry(ASIC_TYPE_NETIF, 1, rb);
	seq_printf(m, "  NETIF1 w0=%08x w1=%08x w2=%08x w3=%08x (WAN VID8; inACL[%u..%u] хотим [4..6])\n",
		   rb[0], rb[1], rb[2], rb[3],
		   (((rb[2] & 0x7f) << 1) | ((rb[1] >> 31) & 1)),
		   ((rb[2] >> 7) & 0xff));
	rtl865x_asic_read_entry(ASIC_TYPE_L3_ROUTING, 2, rb);
	seq_printf(m, "  ROUTE2 w0=%08x w1=%08x (192.168.0.0/24 process=ARP)\n",
		   rb[0], rb[1]);
	rtl865x_asic_read_entry(ASIC_TYPE_L3_ROUTING, 3, rb);
	seq_printf(m, "  ROUTE3 w0=%08x w1=%08x (192.168.1.0/24 process=ARP)\n",
		   rb[0], rb[1]);
	rtl865x_asic_read_entry(ASIC_TYPE_L3_ROUTING, 6, rb);
	seq_printf(m, "  ROUTE6 w0=%08x w1=%08x (дефолт process=5 NAPT)\n",
		   rb[0], rb[1]);
	rtl865x_asic_read_entry(ASIC_TYPE_L2_SWITCH, wan_nh, rb);
	seq_printf(m, "  L2[WAN-пир nh=%u] w0=%08x w1=%08x\n", wan_nh, rb[0], rb[1]);
	rtl865x_asic_read_entry(ASIC_TYPE_EXT_INT_IP, 0, rb);
	seq_printf(m, "  EXTIP0 w0=%08x w1=%08x w2=%08x (w1 хотим %08x = живой extIP)\n",
		   rb[0], rb[1], rb[2], rtl865x_wan_extip);
	rtl865x_asic_read_entry(ASIC_TYPE_NEXT_HOP, 0, rb);
	seq_printf(m, "  NH0    w0=%08x (dstVid=%u arp=%u)\n", rb[0],
		   (rb[0] >> 5) & 0x7, (rb[0] >> 11) & 0x3ff);
	rtl865x_asic_read_entry(ASIC_TYPE_NETIF, 0, rb);
	if (!(rb[0] & 1) || ((rb[0] >> 1) & 0xFFF) != RTL865X_VID_LAN) ok = 0;
	rtl865x_asic_read_entry(ASIC_TYPE_NETIF, 1, rb);
	if (!(rb[0] & 1) || ((rb[0] >> 1) & 0xFFF) != RTL865X_VID_WAN) ok = 0;
	seq_printf(m, "ИТОГ %s (каркас %s; MSCR %08x -> %08x SWTCR0=%08x SWTCR1=%08x)\n",
		   ok ? "PASS" : "FAIL", ok ? "жив" : "не-жив",
		   mscr0, mscr1, REG32(SWTCR0), REG32(SWTCR1));
	pr_info("rtl865x gw: каркас запрограммирован, чтение netif %s\n",
		ok ? "PASS" : "FAIL");
	mutex_unlock(&rtl865x_hal_lock);
	return 0;
}

/* ---- пассивный дамп (без программирования) ------------------------------ */
static void dump_table(struct seq_file *m, const char *tag, int type, int lo, int hi)
{
	u32 rb[8];
	int i;

	for (i = lo; i <= hi; i++) {
		memset(rb, 0, sizeof(rb));
		if (rtl865x_asic_read_entry(type, i, rb))
			continue;
		if (!(rb[0] | rb[1] | rb[2] | rb[3]))
			continue;
		seq_printf(m, "  %s[%4d] %08x %08x %08x %08x\n",
			   tag, i, rb[0], rb[1], rb[2], rb[3]);
	}
}

static int gw_dump_show(struct seq_file *m, void *v)
{
	int i;

	mutex_lock(&rtl865x_hal_lock);
	seq_printf(m, "[rtl865x дамп каркаса — ТОЛЬКО чтение]\n");
	seq_printf(m, "MSCR=%08x SWTCR0=%08x SWTCR1=%08x ALECR=%08x DACLRCR=%08x TEATCR=%08x FFCR=%08x\n",
		   REG32(MSCR), REG32(SWTCR0), REG32(SWTCR1), REG32(ALECR),
		   REG32(DACLRCR), REG32(TEATCR), REG32(FFCR));
	dump_table(m, "NETIF", ASIC_TYPE_NETIF,      0, 7);
	dump_table(m, "ROUTE", ASIC_TYPE_L3_ROUTING, 0, 7);
	dump_table(m, "EXTIP", ASIC_TYPE_EXT_INT_IP, 0, 15);
	dump_table(m, "NH   ", ASIC_TYPE_NEXT_HOP,   0, 31);
	dump_table(m, "ARP  ", ASIC_TYPE_ARP,        0, 511);
	dump_table(m, "L2   ", ASIC_TYPE_L2_SWITCH,  0, 1023);
	seq_puts(m, "NAPT-строки, валидные хотя бы в одном из 3 проходов:\n");
	for (i = 0; i < 1024; i++) {
		u32 rb[8] = { 0 }, rc[8] = { 0 }, rd[8] = { 0 };

		rtl865x_asic_read_entry(ASIC_TYPE_L4_TCP_UDP, i, rb);
		rtl865x_asic_read_entry(ASIC_TYPE_L4_TCP_UDP, i, rc);
		rtl865x_asic_read_entry(ASIC_TYPE_L4_TCP_UDP, i, rd);
		if ((rb[1] | rc[1] | rd[1]) & 1)
			seq_printf(m, "  NAPT[%4d] %08x/%08x/%08x w0=%08x w2=%08x%s\n",
				   i, rb[1], rc[1], rd[1], rd[0], rd[2],
				   (rb[1] != rc[1] || rc[1] != rd[1]) ?
				   "  НЕСТАБИЛЬНО" : "");
	}
	mutex_unlock(&rtl865x_hal_lock);
	seq_puts(m, "[конец]\n");
	return 0;
}

static int napt_scan_show(struct seq_file *m, void *v)
{
	int i, n = 0;
	u32 rb[8];

	mutex_lock(&rtl865x_hal_lock);
	for (i = 0; i < 1024; i++) {
		rtl865x_asic_read_entry(ASIC_TYPE_L4_TCP_UDP, i, rb);
		if (rb[1] & 1) {
			u32 gport = ((rb[1] >> 8) & 0x3f) << 10 |
				    ((rb[1] >> 21) & 0x3ff);

			seq_printf(m, "  [%4d] w0=%08x(intIP) w1=%08x w2=%08x selIP=%u selE=%u off=%u G=0x%x age=%u\n",
				   i, rb[0], rb[1], rb[2],
				   (rb[1] >> 17) & 0xf, (rb[1] >> 21) & 0x3ff,
				   (rb[1] >> 8) & 0x3f, gport,
				   (rb[1] >> 2) & 0x3f);
			n++;
		}
	}
	mutex_unlock(&rtl865x_hal_lock);
	seq_printf(m, "всего валидных NAPT-строк: %d\n", n);
	return 0;
}

static int fabric_show(struct seq_file *m, void *v)
{
	u32 g0, g1, c0, c1;
	int p;

	mutex_lock(&rtl865x_hal_lock);
	g0 = REG32(GDSR0); g1 = REG32(GDSR1);
	c0 = REG32(PCSR0); c1 = REG32(PCSR1);
	seq_printf(m, "GDSR0=%08x USEDDSC=%u MaxUsedDsc=%u %s%s%s\n", g0,
		   (g0 & GDSR0_USEDDSC_MASK) >> 16, g0 & GDSR0_MAXUSEDDSC_MASK,
		   (g0 & GDSR0_DSCRUNOUT) ? "DSCRUNOUT " : "",
		   (g0 & GDSR0_TOTALDSC_FC) ? "TotalDscFC " : "",
		   (g0 & GDSR0_SHAREDBUF_FCON) ? "SharedBufFCON " : "");
	seq_printf(m, "PCSR0=%08x (P0oq=%02x P1oq=%02x P2oq=%02x P3oq=%02x)\n",
		   c0, c0 & 0x7f, (c0 >> 8) & 0x7f, (c0 >> 16) & 0x7f,
		   (c0 >> 24) & 0x7f);
	seq_printf(m, "PCSR1=%08x (P4oq=%02x P5oq=%02x P6oq=%02x IQcong=%02x)\n",
		   c1, c1 & 0x7f, (c1 >> 8) & 0x7f, (c1 >> 16) & 0x7f,
		   (c1 >> 24) & 0x7f);
	for (p = 0; p <= 6; p++) {
		u32 d0 = REG32(Pn_DCR0(p) + 0x00);
		u32 d3 = REG32(Pn_DCR0(p) + 0x0c);

		seq_printf(m, "  P%d: OQ0=%u OQ1=%u IQ=%u\n", p,
			   d0 & 0x3ff, (d0 >> 16) & 0x3ff, d3 & 0x3ff);
	}
	mutex_unlock(&rtl865x_hal_lock);
	return 0;
}

/* ---- /proc/rtl865x_l2flush: сброс выученных строк L2 --------------------- */
#define L2_ENTRIES	1024
#define L2_STA_BIT	(1u << 18)
#define L2_NH_BIT	(1u << 22)

static bool l2_entry_is_learned(const u32 *e)
{
	if (!e[0] && !e[1])
		return false;
	return !(e[1] & (L2_STA_BIT | L2_NH_BIT));
}

static int l2_flush_all_learned(void)
{
	u32 e[8];
	int idx, n = 0;

	for (idx = 0; idx < L2_ENTRIES; idx++) {
		if (rtl865x_asic_read_entry(ASIC_TYPE_L2_SWITCH, idx, e))
			continue;
		if (!l2_entry_is_learned(e))
			continue;
		memset(e, 0, sizeof(e));
		rtl865x_asic_write_entry(ASIC_TYPE_L2_SWITCH, idx, e, true);
		n++;
		if (!(idx & 0x3f))
			cond_resched();
	}
	return n;
}

static ssize_t l2flush_write(struct file *file, const char __user *ubuf,
			     size_t len, loff_t *off)
{
	char kbuf[32];
	char *arg;
	int n;

	if (!capable(CAP_NET_ADMIN))
		return -EPERM;
	if (len == 0 || len >= sizeof(kbuf))
		return -EINVAL;
	if (copy_from_user(kbuf, ubuf, len))
		return -EFAULT;
	kbuf[len] = '\0';
	arg = strim(kbuf);

	mutex_lock(&rtl865x_hal_lock);
	if (!strcasecmp(arg, "all")) {
		n = l2_flush_all_learned();
		pr_info("rtl865x l2flush: снято выученных L2-строк: %d\n", n);
	} else {
		mutex_unlock(&rtl865x_hal_lock);
		return -EINVAL;
	}
	mutex_unlock(&rtl865x_hal_lock);
	return len;
}

static int l2flush_show(struct seq_file *m, void *v)
{
	seq_puts(m, "echo all > /proc/rtl865x_l2flush — снять выученные L2-строки (STA/NH остаются)\n");
	return 0;
}

/* ---- регистрация -------------------------------------------------------- */
static int gw_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, gw_prog, NULL);
}

static const struct proc_ops gw_proc_fops = {
	.proc_open	= gw_proc_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

static int l2flush_open(struct inode *inode, struct file *file)
{
	return single_open(file, l2flush_show, NULL);
}

static const struct proc_ops l2flush_fops = {
	.proc_open	= l2flush_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
	.proc_write	= l2flush_write,
};

static int __init rtl865x_asichal_init(void)
{
	proc_create("rtl865x_gw", 0444, NULL, &gw_proc_fops);
	proc_create_single("rtl865x_dump", 0444, NULL, gw_dump_show);
	proc_create_single("rtl865x_napt", 0444, NULL, napt_scan_show);
	proc_create_single("rtl865x_fabric", 0444, NULL, fabric_show);
	proc_create("rtl865x_l2flush", 0644, NULL, &l2flush_fops);
	pr_info("rtl865x asic-hal: /proc/rtl865x_{gw,dump,napt,fabric,l2flush} готовы\n");
	return 0;
}
late_initcall(rtl865x_asichal_init);
