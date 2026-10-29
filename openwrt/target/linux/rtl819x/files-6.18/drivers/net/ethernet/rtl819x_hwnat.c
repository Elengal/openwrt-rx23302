// SPDX-License-Identifier: GPL-2.0-only
/*
 * RTL8197F: коннтрек-управляемый аппаратный NAT — клей между штатной
 * flowtable ядра 6.x и HAL таблиц (rtl865x_asichal.c).
 *
 * Порт идей rtl819x_hwnat.c донора ADCDS/openwrt-dlink-dir842-r1 (GPL-2.0):
 * там на ядре 4.14 использовался downstream ndo_flow_offload и ручное
 * удержание таймаутов. На 6.x ядро само: FLOW_CLS_REPLACE даёт правило с
 * match-ключами (кортеж направления) и mangle-действиями (переписи NAT);
 * FLOW_CLS_STATS спрашивает нас о жизни потока (наш lastused продлевает
 * таймаут), FLOW_CLS_DESTROY сносит. Образец API — mtk_ppe_offload.c.
 *
 * Форма правила для masquerade-потока LAN->WAN:
 *   ИСХОДЯЩЕЕ (original): match src=int_ip:int_port dst=rem_ip:rem_port;
 *     mangle IPv4 saddr(офсет 12) -> ext_ip; порт src(ст. половина сл. 0)
 *     -> gport; eth dst -> MAC шлюза WAN; redirect -> устройство WAN.
 *   ОТВЕТ (reply):        match src=rem_ip:rem_port dst=ext_ip:gport;
 *     mangle IPv4 daddr(офсет 16) -> int_ip; eth dst -> MAC клиента.
 * Направление узнаём по офсету IP-mangle: 12 = перепись источника
 * (исходящее), 16 = перепись получателя (ответ). Порты берём ТОЛЬКО из
 * match-ключей — кодировки port-mangle читать не нужно.
 *
 * Порядок байтов — как решено в asichal: всё числовое (host). Обе строки
 * NAPT ставим, когда собрали все шесть полей {int, rem, ext} x {ip, port}.
 */
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/if_vlan.h>
#include <net/flow_offload.h>
#include <net/pkt_cls.h>

#include "rtl865x_asichal.h"
#include "rtl819x_hwnat.h"

bool rtl819x_hwnat_enabled;
module_param_named(hwnat, rtl819x_hwnat_enabled, bool, 0644);
MODULE_PARM_DESC(hwnat, "аппаратный NAT offload (0=выкл по умолчанию: всё через софт-флоутейбл; перед включением нужен режим роутера /proc/rtl819x_router)");

static int hwnat_debug;
module_param(hwnat_debug, int, 0644);
MODULE_PARM_DESC(hwnat_debug, "журналировать отказы и установки строк (1=вкл)");

static int l2_mask_lan_default = 0x08;	/* гнездо 3 = LAN1, если fdb молчит */
module_param(l2_mask_lan_default, int, 0644);
MODULE_PARM_DESC(l2_mask_lan_default, "маска гнезда клиента LAN при незнающем fdb (по умолч. 0x08 = LAN1)");

#define HWNAT_DECLINE(fmt, ...) do {					\
	if (hwnat_debug)						\
		pr_info_ratelimited("rtl819x hwnat: отказ " fmt "\n",	##__VA_ARGS__); \
	return -EOPNOTSUPP;						\
} while (0)

/* ---- состояние потоков --------------------------------------------------- */

/* Один masquerade-поток = до двух строк NAPT + два cookie правил.
 * cookie — указатель кортежа из ядра, трактуем как непрозрачный ID. */
struct hwnat_flow {
	struct list_head	list;
	u64			cookie[2];	/* исходящее, ответ */
	bool			rows_up;	/* строки записаны */
	u8			is_tcp;
	u32			int_ip, rem_ip, ext_ip;	/* host order */
	u16			int_port, rem_port, gport;
	u16			idx_out, idx_in;
	u8			last_age;
	unsigned long		lastused;	/* jiffies последней жизни */
	bool			hw_dead;	/* строка сгинула по старению */
	u8			gw_mac[ETH_ALEN];	/* MAC шлюза WAN (из исходящего) */
	bool			have_gw;
	u8			client_mac[ETH_ALEN];	/* MAC LAN-клиента (из ответного) */
	bool			have_client;
};

static LIST_HEAD(hwnat_flows);
static DECLARE_BITMAP(hwnat_used, RTL865X_NAPT_ROWS)
	__attribute__((aligned(sizeof(unsigned long))));

/* Разобранное из FLOW_CLS_REPLACE сырье одного направления */
struct hwnat_offer {
	bool	have_src_ip, have_dst_ip, have_ports, have_basic;
	u32	src_ip, dst_ip;			/* host order */
	u16	sport, dport;
	u8	ip_proto;
	u8	eth_dst[ETH_ALEN];
	bool	have_eth_dst;
	u32	ip_mangle;				/* значение IP-mangle */
	u8	ip_mangle_off;				/* 12 или 16 */
	bool	have_ip_mangle;
	struct net_device *egress;
};

/* ---- разбор flow_rule ----------------------------------------------------- */

static void hwnat_parse_rule(const struct flow_rule *rule,
			     struct hwnat_offer *o)
{
	const struct flow_action_entry *act;
	int i;

	memset(o, 0, sizeof(*o));

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_BASIC)) {
		struct flow_match_basic m;

		flow_rule_match_basic(rule, &m);
		o->have_basic = true;
		o->ip_proto = m.key->ip_proto;
	}
	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_IPV4_ADDRS)) {
		struct flow_match_ipv4_addrs m;

		flow_rule_match_ipv4_addrs(rule, &m);
		o->have_src_ip = true;
		o->src_ip = ntohl(m.key->src);	/* __be32, не struct in_addr */
		o->dst_ip = ntohl(m.key->dst);
	}
	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_PORTS)) {
		struct flow_match_ports m;

		flow_rule_match_ports(rule, &m);
		o->have_ports = true;
		o->sport = ntohs(m.key->src);
		o->dport = ntohs(m.key->dst);
	}

	/* действия: собираем eth dst по байтам и IP-перепись по офсету */
	flow_action_for_each(i, act, &rule->action) {
		u8 *eth = o->eth_dst;

		switch (act->id) {
		case FLOW_ACTION_MANGLE:
			switch (act->mangle.htype) {
			case FLOW_ACT_MANGLE_HDR_TYPE_ETH:
				switch (act->mangle.offset) {
				case 0:	/* байты 0..3 dst */
					eth[0] = act->mangle.val & 0xff;
					eth[1] = (act->mangle.val >> 8) & 0xff;
					eth[2] = (act->mangle.val >> 16) & 0xff;
					eth[3] = (act->mangle.val >> 24) & 0xff;
					o->have_eth_dst = true;
					break;
				case 4:	/* байты 4..5 dst в младшей половине */
					eth[4] = act->mangle.val & 0xff;
					eth[5] = (act->mangle.val >> 8) & 0xff;
					o->have_eth_dst = true;
					break;
				default:	/* источник — не нужен */
					break;
				}
				break;
			case FLOW_ACT_MANGLE_HDR_TYPE_IP4:
				o->have_ip_mangle = true;
				o->ip_mangle_off = act->mangle.offset;
				o->ip_mangle = ntohl(act->mangle.val);
				break;
			default:
				break;
			}
			break;
		case FLOW_ACTION_REDIRECT:
			o->egress = act->dev;
			break;
		default:
			break;
		}
	}
}

/* ---- установка/снос строк ------------------------------------------------ */

static void hwnat_clear_rows(struct hwnat_flow *f)
{
	if (!f->rows_up)
		return;
	rtl865x_napt_clear(f->idx_out);
	rtl865x_napt_clear(f->idx_in);
	__clear_bit(f->idx_out, hwnat_used);
	__clear_bit(f->idx_in, hwnat_used);
	f->rows_up = false;
}

static void hwnat_flush_locked(void)
{
	struct hwnat_flow *f, *tmp;

	list_for_each_entry_safe(f, tmp, &hwnat_flows, list) {
		hwnat_clear_rows(f);
		list_del(&f->list);
		kfree(f);
	}
}

static int hwnat_program_rows(struct hwnat_flow *f)
{
	struct asic_napt_tcpudp e;
	int rc;

	/* Исходящая строка: внутренний кортеж + отображение на extIP/G.
	 * Все поля числовые (host) — решение донора, проверено на кремнии. */
	memset(&e, 0, sizeof(e));
	e.intIPAddr = f->int_ip;
	e.intPort   = f->int_port;
	e.isTCP     = f->is_tcp;
	e.valid     = 1;
	e.collision = 0;	/* ДОКАЗАНО донором: без collision-битов */
	e.collision2 = 0;
	e.isStatic  = 1;	/* вендор: строки драйвера — статические */
	e.dedicate  = 0;
	e.agingTime = RTL865X_NAPT_AGING_RELOAD;
	e.selIPIdx  = 0;		/* -> extIP[0] */
	e.selEIdx   = f->gport & 0x3ff;
	e.offset    = f->gport >> 10;
	e.TCPFlag   = 0x3;		/* вендор: исходящая */

	rc = rtl865x_napt_write(f->idx_out, &e);
	if (rc)
		return rc;

	/* Ответная строка — ПРОВЕРОЧНАЯ (enhanced-hash1): offset/selIP
	 * кодируют G иначе, а selEIdx = HASH1(remIP,remPort,0,0) — кремний
	 * пересчитывает его по входящему пакету и отвергает строку при
	 * несовпадении. Копия исходящей selEIdx=G здесь роняла обратное
	 * направление в CPU (донор это ловил месяц). */
	e.offset   = f->gport & 0x3f;
	e.selEIdx  = gw_napt_hash1(f->is_tcp, f->rem_ip, f->rem_port, 0, 0) & 0x3ff;
	e.selIPIdx = (f->gport & 0x3ff) >> 6;
	e.TCPFlag  = 0x2;		/* вендор: ответная */
	rc = rtl865x_napt_write(f->idx_in, &e);
	if (rc) {
		rtl865x_napt_clear(f->idx_out);
		return rc;
	}

	/* контроль записи: ключ должен читаться назад тем же значением */
	{
		struct asic_napt_tcpudp v;

		if (rtl865x_napt_read(f->idx_out, &v) || !v.valid ||
		    v.intIPAddr != f->int_ip || v.intPort != f->int_port) {
			rtl865x_napt_clear(f->idx_out);
			rtl865x_napt_clear(f->idx_in);
			return -EIO;
		}
	}
	return 0;
}

/* ---- обработка предложений ------------------------------------------------ */

static struct hwnat_flow *hwnat_find_by_cookie(u64 cookie)
{
	struct hwnat_flow *f;

	list_for_each_entry(f, &hwnat_flows, list)
		if (f->cookie[0] == cookie || f->cookie[1] == cookie)
			return f;
	return NULL;
}

static void hwnat_take_cookie(struct hwnat_flow *f, u64 cookie, int dir)
{
	int slot = dir ? 1 : 0;

	if (!f->cookie[slot])
		f->cookie[slot] = cookie;
}

static int hwnat_replace(struct flow_cls_offload *cls)
{
	const struct flow_rule *rule = cls->rule;
	struct hwnat_flow *f, *tmp;
	struct hwnat_offer o;
	u64 cookie = cls->cookie;
	bool dir_reply;
	u16 idx_out, idx_in;
	int rc;

	if (!READ_ONCE(rtl819x_hwnat_enabled))
		return -EOPNOTSUPP;

	/* Аппаратный путь существует только в режиме роутера: гнёзда 8367
	 * видим своими портами 0..4, MAC-строки адресуем гнёздами. */
	if (!rtl819x_router_mode())
		HWNAT_DECLINE("нет режима роутера (/proc/rtl819x_router)");

	hwnat_parse_rule(rule, &o);

	if (!o.have_basic || !o.have_src_ip || !o.have_ports)
		HWNAT_DECLINE("не IPv4 или нет кортежа");
	if (o.ip_proto != IPPROTO_TCP && o.ip_proto != IPPROTO_UDP)
		HWNAT_DECLINE("l4 %u не TCP/UDP", o.ip_proto);
	if (!o.have_ip_mangle || (o.ip_mangle_off != 12 && o.ip_mangle_off != 16))
		HWNAT_DECLINE("нет IP-переписи (не NAT?)");

	/* дубль от другого block cb (eth0 + eth0.8 + br-lan) — уже сделано */
	f = hwnat_find_by_cookie(cookie);
	if (f) {
		if (f->rows_up)
			return 0;
		/* первый заход не дошёл до строк — перезапустим ниже */
	}

	dir_reply = (o.ip_mangle_off == 16);

	if (dir_reply) {
		/* match: src=rem, dst=ext:G; mangle daddr -> int */
		u32 rem_ip = o.src_ip, ext_ip = o.dst_ip;
		u16 rem_port = o.sport, gport = o.dport;
		u32 int_ip = o.ip_mangle;

		list_for_each_entry_safe(f, tmp, &hwnat_flows, list) {
			if (f->rows_up || !f->ext_ip)
				continue;
			/* join: исходящее уже пришло с этим же потоком */
			if (f->rem_ip == rem_ip && f->rem_port == rem_port &&
			    f->int_ip == int_ip) {
				f->ext_ip = ext_ip;
				f->gport = gport;
				if (o.have_eth_dst) {
					ether_addr_copy(f->client_mac, o.eth_dst);
					f->have_client = true;
				}
				hwnat_take_cookie(f, cookie, 1);
				goto program;
			}
		}
		/* исходящего предложения ещё не было — заведём сами */
		f = kzalloc(sizeof(*f), GFP_KERNEL);
		if (!f)
			return -ENOMEM;
		f->is_tcp   = (o.ip_proto == IPPROTO_TCP);
		f->int_ip   = int_ip;
		f->rem_ip   = rem_ip;
		f->rem_port = rem_port;
		f->ext_ip   = ext_ip;
		f->gport    = gport;
		f->int_port = 0;		/* придёт с исходящего правила */
		if (o.have_eth_dst) {
			ether_addr_copy(f->client_mac, o.eth_dst);
			f->have_client = true;
		}
		f->cookie[1] = cookie;
		list_add_tail(&f->list, &hwnat_flows);
		if (hwnat_debug)
			pr_info_ratelimited("rtl819x hwnat: ответ раньше исходящего? int=%pI4h rem=%pI4h:%u G=%u — жду int_port\n",
					    &f->int_ip, &f->rem_ip, f->rem_port,
					    f->gport);
		return -EOPNOTSUPP;	/* без int_port строку не построить */
	}

	/* исходящее направление: match src=int, dst=rem; mangle saddr -> ext */
	{
		u32 int_ip = o.src_ip, rem_ip = o.dst_ip, ext_ip = o.ip_mangle;
		u16 int_port = o.sport, rem_port = o.dport;

		list_for_each_entry_safe(f, tmp, &hwnat_flows, list) {
			if (f->int_ip == int_ip && f->rem_ip == rem_ip &&
			    (!f->int_port || f->int_port == int_port)) {
				f->int_port = int_port;
				if (!f->ext_ip) {
					f->rem_port = rem_port;
					f->ext_ip = ext_ip;
				}
				if (o.have_eth_dst) {
					ether_addr_copy(f->gw_mac, o.eth_dst);
					f->have_gw = true;
				}
				hwnat_take_cookie(f, cookie, 0);
				goto program;
			}
		}
		f = kzalloc(sizeof(*f), GFP_KERNEL);
		if (!f)
			return -ENOMEM;
		f->is_tcp   = (o.ip_proto == IPPROTO_TCP);
		f->int_ip   = int_ip;
		f->int_port = int_port;
		f->rem_ip   = rem_ip;
		f->rem_port = rem_port;
		f->ext_ip   = ext_ip;	/* gport придёт с ответного правила */
		if (o.have_eth_dst) {
			ether_addr_copy(f->gw_mac, o.eth_dst);
			f->have_gw = true;
		}
		f->cookie[0] = cookie;
		list_add_tail(&f->list, &hwnat_flows);
		if (hwnat_debug)
			pr_info_ratelimited("rtl819x hwnat: исходящее без G: int=%pI4h:%u rem=%pI4h:%u ext=%pI4h — жду ответ\n",
					    &f->int_ip, f->int_port, &f->rem_ip,
					    f->rem_port, &f->ext_ip);
		return -EOPNOTSUPP;	/* declined: ждём G из ответного правила */
	}

program:
	if (f->rows_up)
		return 0;
	if (!f->int_port || !f->gport || !f->ext_ip) {
		if (hwnat_debug)
			pr_info_ratelimited("rtl819x hwnat: поток ещё неполный (int_port=%u G=%u ext=%pI4h)\n",
					    f->int_port, f->gport, &f->ext_ip);
		return -EOPNOTSUPP;
	}

	mutex_lock(&rtl865x_hal_lock);

	/* идентичность WAN: extIP, MAC шлюза (eth_dst исходящего правила),
	 * MAC netif. Смена -> снос всех строк, потоки вернутся софтом и
	 * предложат себя заново (донор M7.2). */
	rc = rtl865x_set_wan_extip(f->ext_ip);
	if (rc < 0)
		goto out;
	if (rc > 0)
		hwnat_flush_locked();
	if (rtl865x_wan_netif_mac_sync() > 0)
		hwnat_flush_locked();
	if (f->have_gw) {
		rc = rtl865x_wan_set_nexthop(f->gw_mac);
		if (rc < 0)
			goto out;
		if (rc > 0)
			hwnat_flush_locked();
		/* route[3]: подключённая WAN-сеть идёт процессом RT_ARP —
		 * ASIC ищет ARP-строку в окне 256..; без неё поток к адресу
		 * из WAN-подсети (стенд: 192.168.1.1) уходит в CPU. */
		rtl865x_wan_set_arp(f->rem_ip);
	}

	/* идентичность LAN: MAC клиента (eth_dst ответного) + его IP и гнездо */
	if (f->have_client) {
		u32 jack = rtl819x_lan_jack_mask(f->client_mac);

		if (!jack)
			jack = l2_mask_lan_default;
		rc = rtl865x_lan_set_nexthop(f->client_mac, f->int_ip, jack);
		if (rc < 0)
			goto out;
		if (rc > 0)
			hwnat_flush_locked();
	}

	idx_out = gw_napt_hash1(f->is_tcp, f->int_ip, f->int_port,
				f->rem_ip, f->rem_port);
	idx_in  = gw_napt_hash1(f->is_tcp, f->rem_ip, f->rem_port,
				f->ext_ip, f->gport);
	if (idx_out == idx_in ||
	    test_bit(idx_out, hwnat_used) || test_bit(idx_in, hwnat_used)) {
		if (hwnat_debug)
			pr_info_ratelimited("rtl819x hwnat: строки заняты out@%u in@%u\n",
					    idx_out, idx_in);
		rc = -ENOSPC;
		goto out;
	}

	f->idx_out = idx_out;
	f->idx_in = idx_in;
	rc = hwnat_program_rows(f);
	if (rc)
		goto out;

	__set_bit(idx_out, hwnat_used);
	__set_bit(idx_in, hwnat_used);
	f->rows_up = true;
	f->last_age = RTL865X_NAPT_AGING_RELOAD;
	f->lastused = jiffies;
	f->hw_dead = false;
	rc = 0;

	pr_info_ratelimited("rtl819x hwnat: +%s %pI4h:%u -> %pI4h:%u G=%u строки out@%u in@%u\n",
			    f->is_tcp ? "tcp" : "udp",
			    &f->int_ip, f->int_port, &f->rem_ip, f->rem_port,
			    f->gport, idx_out, idx_in);

out:
	mutex_unlock(&rtl865x_hal_lock);
	return rc;
}

static int hwnat_destroy(struct flow_cls_offload *cls)
{
	struct hwnat_flow *f, *tmp;
	u64 cookie = cls->cookie;
	int rc = 0;

	/* ядро зовёт DESTROY для каждого направления — чистим по любому cookie,
	 * и без break: битые указатели cookie у донора алиасили новые потоки */
	mutex_lock(&rtl865x_hal_lock);
	list_for_each_entry_safe(f, tmp, &hwnat_flows, list) {
		if (f->cookie[0] != cookie && f->cookie[1] != cookie)
			continue;
		hwnat_clear_rows(f);
		list_del(&f->list);
		kfree(f);
		rc = 1;
	}
	mutex_unlock(&rtl865x_hal_lock);
	return 0;
}

static int hwnat_stats(struct flow_cls_offload *cls)
{
	struct hwnat_flow *f = hwnat_find_by_cookie(cls->cookie);
	struct asic_napt_tcpudp e;

	if (!f || !f->rows_up)
		return -ENOENT;

	mutex_lock(&rtl865x_hal_lock);
	if (!rtl865x_napt_read(f->idx_out, &e) && e.valid) {
		if (e.agingTime >= RTL865X_NAPT_AGING_RELOAD ||
		    e.agingTime > f->last_age) {
			/* строка жива: таймер у потолка или тикнул вверх —
			 * значит ASIC перезарядил его реальным трафиком */
			f->lastused = jiffies;
			f->hw_dead = false;
		}
		f->last_age = e.agingTime;
	} else {
		/* строка сгинула (авто-удаление по старению): честно уводим
		 * lastused в прошлое — GC ядра снесёт поток и DESTROY уберёт
		 * нашу тень */
		f->hw_dead = true;
	}
	mutex_unlock(&rtl865x_hal_lock);

	cls->stats.pkts = 0;		/* побайтовых счётчиков на строку нет */
	cls->stats.bytes = 0;
	cls->stats.lastused = f->lastused;
	return 0;
}

/* ---- block cb и привязка -------------------------------------------------- */

static int hwnat_block_cb(enum tc_setup_type type, void *type_data,
			  void *cb_priv)
{
	struct flow_cls_offload *cls = type_data;

	if (type != TC_SETUP_CLSFLOWER)
		return -EOPNOTSUPP;

	switch (cls->command) {
	case FLOW_CLS_REPLACE:
		return hwnat_replace(cls);
	case FLOW_CLS_DESTROY:
		return hwnat_destroy(cls);
	case FLOW_CLS_STATS:
		return hwnat_stats(cls);
	default:
		return -EOPNOTSUPP;
	}
}

static LIST_HEAD(hwnat_block_cb_list);

static int hwnat_setup_block(struct net_device *dev,
			     struct flow_block_offload *f)
{
	struct flow_block_cb *block_cb;
	flow_setup_cb_t *cb = hwnat_block_cb;

	if (f->binder_type != FLOW_BLOCK_BINDER_TYPE_CLSACT_INGRESS)
		return -EOPNOTSUPP;

	f->driver_block_list = &hwnat_block_cb_list;

	switch (f->command) {
	case FLOW_BLOCK_BIND:
		block_cb = flow_block_cb_lookup(f->block, cb, dev);
		if (block_cb) {
			flow_block_cb_incref(block_cb);
			return 0;
		}
		block_cb = flow_block_cb_alloc(cb, dev, dev, NULL);
		if (IS_ERR(block_cb))
			return PTR_ERR(block_cb);
		flow_block_cb_incref(block_cb);
		flow_block_cb_add(block_cb, f);
		list_add_tail(&block_cb->driver_list, &hwnat_block_cb_list);
		return 0;
	case FLOW_BLOCK_UNBIND:
		block_cb = flow_block_cb_lookup(f->block, cb, dev);
		if (!block_cb)
			return -ENOENT;
		if (!flow_block_cb_decref(block_cb)) {
			flow_block_cb_remove(block_cb, f);
			list_del(&block_cb->driver_list);
		}
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

int rtl819x_hwnat_setup_tc(struct net_device *dev, enum tc_setup_type type,
			   void *type_data)
{
	if (type == TC_SETUP_FT)
		return hwnat_setup_block(dev, type_data);
	return -EOPNOTSUPP;
}

/* ---- indr: eth0.8 / eth0.9 / br-lan не имеют своего ndo_setup_tc ---------- */

/* Наше ли это устройство: eth0, VLAN поверх него или мост с ним внутри.
 * Образец indr-привязки — sfc/tc_bindings.c: flow_indr_block_cb_alloc +
 * flow_block_cb_add, снятие — flow_indr_block_cb_remove. */
static bool hwnat_dev_is_ours(struct net_device *dev)
{
	struct net_device *eth = rtl819x_eth_dev();
	struct net_device *lower;
	struct list_head *iter;

	if (!eth || !dev)
		return false;
	if (dev == eth)
		return true;
	if (is_vlan_dev(dev))
		return vlan_dev_real_dev(dev) == eth;
	if (netif_is_bridge_master(dev)) {
		netdev_for_each_lower_dev(dev, lower, iter) {
			if (lower == eth)
				return true;
			if (is_vlan_dev(lower) &&
			    vlan_dev_real_dev(lower) == eth)
				return true;
		}
	}
	return false;
}

static int hwnat_indr_cb(struct net_device *dev, struct Qdisc *sch,
			 void *indr_block_cb_priv,
			 enum tc_setup_type type, void *type_data,
			 void *data,
			 void (*cleanup)(struct flow_block_cb *block_cb))
{
	struct flow_block_offload *f = type_data;
	struct flow_block_cb *block_cb;

	if (type != TC_SETUP_FT)
		return -EOPNOTSUPP;

	if (!dev)
		return -EOPNOTSUPP;

	if (f->binder_type != FLOW_BLOCK_BINDER_TYPE_CLSACT_INGRESS)
		return -EOPNOTSUPP;

	switch (f->command) {
	case FLOW_BLOCK_BIND:
		if (!hwnat_dev_is_ours(dev))
			return -EOPNOTSUPP;
		block_cb = flow_indr_block_cb_alloc(hwnat_block_cb, dev, dev,
						    NULL, f, dev, sch, data,
						    indr_block_cb_priv, cleanup);
		if (IS_ERR(block_cb))
			return PTR_ERR(block_cb);
		flow_block_cb_add(block_cb, f);
		return 0;
	case FLOW_BLOCK_UNBIND:
		block_cb = flow_block_cb_lookup(f->block, hwnat_block_cb, dev);
		if (!block_cb)
			return -ENOENT;
		flow_indr_block_cb_remove(block_cb, f);
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static int __init hwnat_init(void)
{
	int ret = flow_indr_dev_register(hwnat_indr_cb, NULL);

	if (ret)
		pr_warn("rtl819x hwnat: indr-регистрация не удалась: %d\n", ret);
	return ret;
}
late_initcall(hwnat_init);
