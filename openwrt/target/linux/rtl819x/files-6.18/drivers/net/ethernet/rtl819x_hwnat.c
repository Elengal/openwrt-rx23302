// SPDX-License-Identifier: GPL-2.0-only
/*
 * Аппаратный NAT коммутатора RTL8197F: СКЕЛЕТ.
 *
 * План, таблицы, регистры и ссылки на донора (iptime A2004MU,
 * rtl8197f_eth.c) - в docs/hwnat-plan.md. Здесь пока только то, что
 * железо не трогает:
 *
 *   - привязка к flowtable nftables (TC_SETUP_FT) на eth0 и, через
 *     indirect-обработчик, на всех прочих устройствах flowtable. Без
 *     второго fw4 не создаст flowtable с flags offload вовсе: он проверяет
 *     все устройства сразу, а у Wi-Fi rtl8192cd нет ndo_setup_tc;
 *   - разбор правил, которые присылает ядро, в struct rtl819x_hwnat_rule;
 *   - счётчики и последнее разобранное правило в /proc/rtl819x_hwnat.
 *
 * Каждое правило отвергается (-EOPNOTSUPP), поток остаётся программным.
 * Чтение и запись таблиц коммутатора - заготовка к этапу 3 плана, пока не
 * вызывается.
 */

#include <linux/etherdevice.h>
#include <linux/if_vlan.h>
#include <linux/ip.h>
#include <linux/mutex.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <net/flow_offload.h>
#include <net/pkt_cls.h>

#include "rtl819x_hwnat.h"

/* ---- таблицы и регистры коммутатора ----------------------------------- */

/* Номера таблиц - как у донора (RTL_TBL_*), см. «Таблицы» в плане. */
enum rtl819x_tbl {
	RTL819X_TBL_L2		= 0,
	RTL819X_TBL_ARP		= 1,
	RTL819X_TBL_ROUTE	= 2,
	RTL819X_TBL_NETIF	= 4,
	RTL819X_TBL_EXTIP	= 5,
	RTL819X_TBL_VLAN	= 6,
	RTL819X_TBL_NAPT	= 9,
	RTL819X_TBL_NEXTHOP	= 13,
};

/* строка таблицы - восемь слов в окне 0xBB000000, читается напрямую */
#define RTL819X_TBL_ADDR(t, i)	(0xBB000000 | ((t) << 16) | ((i) << 5))
#define RTL819X_TBL_WORDS	8

/* те же регистры, что у rtl819x_intsw_zapis_vlan() в rtl819x_eth.c */
#define RTL819X_SWTCR0		0xBB804418
#define RTL819X_STOP_TLU	BIT(18)
#define RTL819X_TLU_STOPPED	BIT(19)
#define RTL819X_SWTACR		0xBB804D00
#define RTL819X_SWTACR_WRITE	0x09	/* CMD_FORCE | ACTION_START */
#define RTL819X_SWTACR_BUSY	BIT(0)
#define RTL819X_SWTAA		0xBB804D08
#define RTL819X_TCR(n)		(0xBB804D20 + ((n) << 2))

/* метки, которые ставит swconfig (uci-defaults/06_switch_vlan_ids) */
#define RTL819X_HWNAT_WAN_VID	8
#define RTL819X_HWNAT_LAN_VID	9

static inline u32 rtl819x_hw_r(u32 adres)
{
	return *(volatile u32 *)adres;
}

static inline void rtl819x_hw_w(u32 adres, u32 znach)
{
	*(volatile u32 *)adres = znach;
}

static int rtl819x_hw_zhdat(u32 adres, u32 maska, u32 zhdem)
{
	int popytok = 100000;

	while ((rtl819x_hw_r(adres) & maska) != zhdem) {
		if (--popytok == 0)
			return -ETIMEDOUT;
		cpu_relax();
	}

	return 0;
}

/*
 * Остановить аппаратный поиск на время записи таблиц. Запись идёт
 * пачкой (донор: базовые таблицы, затем пара NAPT), поэтому останов и
 * пуск вынесены отдельно от записи строки.
 *
 * TODO этап 3: rtl819x_intsw_zapis_vlan() в rtl819x_eth.c делает то же
 * самое у себя; одновременно им работать нельзя - нужен общий замок.
 */
static int __maybe_unused rtl819x_tlu_stop(u32 *bylo)
{
	*bylo = rtl819x_hw_r(RTL819X_SWTCR0);
	rtl819x_hw_w(RTL819X_SWTCR0, *bylo | RTL819X_STOP_TLU);

	return rtl819x_hw_zhdat(RTL819X_SWTCR0, RTL819X_TLU_STOPPED,
				RTL819X_TLU_STOPPED);
}

static void __maybe_unused rtl819x_tlu_start(u32 bylo)
{
	rtl819x_hw_w(RTL819X_SWTCR0, bylo & ~RTL819X_STOP_TLU);
}

/* Записать строку таблицы. Поиск должен быть остановлен. */
static int __maybe_unused rtl819x_tbl_write(enum rtl819x_tbl t, u16 idx,
					    const u32 *slovo)
{
	unsigned int i;

	for (i = 0; i < RTL819X_TBL_WORDS; i++)
		rtl819x_hw_w(RTL819X_TCR(i), slovo[i]);
	rtl819x_hw_w(RTL819X_SWTAA, RTL819X_TBL_ADDR(t, idx));
	rtl819x_hw_w(RTL819X_SWTACR, RTL819X_SWTACR_WRITE);

	return rtl819x_hw_zhdat(RTL819X_SWTACR, RTL819X_SWTACR_BUSY, 0);
}

static void __maybe_unused rtl819x_tbl_read(enum rtl819x_tbl t, u16 idx,
					    u32 *slovo)
{
	unsigned int i;

	for (i = 0; i < RTL819X_TBL_WORDS; i++)
		slovo[i] = rtl819x_hw_r(RTL819X_TBL_ADDR(t, idx) + (i << 2));
}

/* ---- разбор правил flowtable ------------------------------------------ */

/*
 * Одно направление потока, как его присылает ядро 6.18
 * (nf_flow_table_offload.c) для eth0.8/eth0.9 поверх eth0: ключ VLAN,
 * действия VLAN_POP/VLAN_PUSH, подмена MAC, адресов и портов, REDIRECT
 * обратно в eth0. orig_* - что пришло, остальное - что уйдёт.
 */
struct rtl819x_hwnat_rule {
	u16 in_vid;
	u16 out_vid;
	u8 ip_proto;
	__be32 orig_saddr, orig_daddr;
	__be32 saddr, daddr;
	__be16 orig_sport, orig_dport;
	__be16 sport, dport;
	struct ethhdr eth;
};

static struct {
	struct net_device *eth;
	struct mutex lock;		/* счётчики и last */
	unsigned long replace, parsed, rejected, destroy, stats;
	unsigned long binds, indr_binds;
	bool have_last;
	struct rtl819x_hwnat_rule last;
} rtl819x_hwnat;

static LIST_HEAD(rtl819x_hwnat_block_list);

/* Подмена MAC: разбор как у донора (rtl_ft_mangle_eth). */
static int rtl819x_hwnat_mangle_eth(const struct flow_action_entry *act,
				    struct ethhdr *eth)
{
	u8 *kuda = (u8 *)eth + act->mangle.offset;
	const u8 *otkuda = (const u8 *)&act->mangle.val;

	if (act->mangle.offset > 8)
		return -EOPNOTSUPP;
	if (act->mangle.mask == 0xffff) {
		otkuda += 2;
		kuda += 2;
	}
	memcpy(kuda, otkuda, act->mangle.mask ? 2 : 4);

	return 0;
}

static int rtl819x_hwnat_mangle_ip4(const struct flow_action_entry *act,
				    struct rtl819x_hwnat_rule *r)
{
	switch (act->mangle.offset) {
	case offsetof(struct iphdr, saddr):
		memcpy(&r->saddr, &act->mangle.val, sizeof(r->saddr));
		return 0;
	case offsetof(struct iphdr, daddr):
		memcpy(&r->daddr, &act->mangle.val, sizeof(r->daddr));
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

/* Подмена портов TCP/UDP: как у донора (rtl_ft_mangle_ports). */
static int rtl819x_hwnat_mangle_ports(const struct flow_action_entry *act,
				      struct rtl819x_hwnat_rule *r)
{
	u32 val = ntohl(act->mangle.val);

	switch (act->mangle.offset) {
	case 0:
		if (act->mangle.mask == ~htonl(0xffff))
			r->dport = cpu_to_be16(val);
		else if (act->mangle.mask == ~htonl(0xffff0000))
			r->sport = cpu_to_be16(val >> 16);
		else
			return -EOPNOTSUPP;
		return 0;
	case 2:
		if (act->mangle.mask != ~htonl(0xffff))
			return -EOPNOTSUPP;
		r->dport = cpu_to_be16(val);
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static int rtl819x_hwnat_parse(struct flow_cls_offload *cls,
			       struct rtl819x_hwnat_rule *r)
{
	struct flow_rule *rule = flow_cls_offload_flow_rule(cls);
	const struct flow_action_entry *act;
	struct flow_match_control control;
	struct flow_match_ipv4_addrs addrs;
	struct flow_match_basic basic;
	struct flow_match_ports ports;
	struct flow_match_meta meta;
	struct flow_match_vlan vlan;
	bool redirect = false, pop = false;
	int i, err;

	if (!flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_META) ||
	    !flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_CONTROL) ||
	    !flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_BASIC) ||
	    !flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_IPV4_ADDRS) ||
	    !flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_PORTS) ||
	    !flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_VLAN) ||
	    flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_CVLAN))
		return -EOPNOTSUPP;

	/* правило должно прийти с eth0, а не с Wi-Fi и прочих */
	flow_rule_match_meta(rule, &meta);
	if (meta.key->ingress_ifindex != rtl819x_hwnat.eth->ifindex)
		return -EOPNOTSUPP;

	flow_rule_match_control(rule, &control);
	if (control.key->addr_type != FLOW_DISSECTOR_KEY_IPV4_ADDRS ||
	    flow_rule_has_control_flags(control.mask->flags,
					cls->common.extack))
		return -EOPNOTSUPP;

	flow_rule_match_basic(rule, &basic);
	if (basic.key->ip_proto != IPPROTO_TCP &&
	    basic.key->ip_proto != IPPROTO_UDP)
		return -EOPNOTSUPP;
	r->ip_proto = basic.key->ip_proto;

	flow_rule_match_vlan(rule, &vlan);
	r->in_vid = vlan.key->vlan_id;

	flow_rule_match_ipv4_addrs(rule, &addrs);
	r->orig_saddr = r->saddr = addrs.key->src;
	r->orig_daddr = r->daddr = addrs.key->dst;

	flow_rule_match_ports(rule, &ports);
	r->orig_sport = r->sport = ports.key->src;
	r->orig_dport = r->dport = ports.key->dst;

	flow_action_for_each(i, act, &rule->action) {
		switch (act->id) {
		case FLOW_ACTION_MANGLE:
			switch (act->mangle.htype) {
			case FLOW_ACT_MANGLE_HDR_TYPE_ETH:
				err = rtl819x_hwnat_mangle_eth(act, &r->eth);
				break;
			case FLOW_ACT_MANGLE_HDR_TYPE_IP4:
				err = rtl819x_hwnat_mangle_ip4(act, r);
				break;
			case FLOW_ACT_MANGLE_HDR_TYPE_TCP:
			case FLOW_ACT_MANGLE_HDR_TYPE_UDP:
				err = rtl819x_hwnat_mangle_ports(act, r);
				break;
			default:
				err = -EOPNOTSUPP;
			}
			if (err)
				return err;
			break;
		case FLOW_ACTION_VLAN_POP:
			pop = true;
			break;
		case FLOW_ACTION_VLAN_PUSH:
			if (act->vlan.proto != htons(ETH_P_8021Q))
				return -EOPNOTSUPP;
			r->out_vid = act->vlan.vid;
			break;
		case FLOW_ACTION_CSUM:
			break;
		case FLOW_ACTION_REDIRECT:
			if (act->dev != rtl819x_hwnat.eth)
				return -EOPNOTSUPP;
			redirect = true;
			break;
		default:
			/* PPPoE, туннели и прочее - не наше */
			return -EOPNOTSUPP;
		}
	}

	/* нас интересует только WAN <-> LAN через eth0 */
	if (!redirect || !pop ||
	    !((r->in_vid == RTL819X_HWNAT_LAN_VID &&
	       r->out_vid == RTL819X_HWNAT_WAN_VID) ||
	      (r->in_vid == RTL819X_HWNAT_WAN_VID &&
	       r->out_vid == RTL819X_HWNAT_LAN_VID)))
		return -EOPNOTSUPP;

	if (!is_valid_ether_addr(r->eth.h_source) ||
	    !is_valid_ether_addr(r->eth.h_dest))
		return -EINVAL;

	return 0;
}

/*
 * Обработчик правил блока eth0. Пока только разбирает и считает:
 * TODO этап 4 - пары правил, запись в таблицы, снятие и статистика
 * (донор: rtl_hwnat_replace, _destroy, _stats).
 */
static int rtl819x_hwnat_cls_cb(enum tc_setup_type type, void *type_data,
				void *cb_priv)
{
	struct flow_cls_offload *cls = type_data;
	struct rtl819x_hwnat_rule r = {};

	if (type != TC_SETUP_CLSFLOWER)
		return -EOPNOTSUPP;

	mutex_lock(&rtl819x_hwnat.lock);
	switch (cls->command) {
	case FLOW_CLS_REPLACE:
		rtl819x_hwnat.replace++;
		if (rtl819x_hwnat_parse(cls, &r)) {
			rtl819x_hwnat.rejected++;
			break;
		}
		rtl819x_hwnat.parsed++;
		rtl819x_hwnat.last = r;
		rtl819x_hwnat.have_last = true;
		break;
	case FLOW_CLS_DESTROY:
		rtl819x_hwnat.destroy++;
		break;
	case FLOW_CLS_STATS:
		rtl819x_hwnat.stats++;
		break;
	default:
		break;
	}
	mutex_unlock(&rtl819x_hwnat.lock);

	/* ничего не поставлено: поток остаётся в программном flowtable */
	return -EOPNOTSUPP;
}

/* ---- привязка к flowtable --------------------------------------------- */

/* eth0: ndo_setup_tc(TC_SETUP_FT), как rtl_setup_tc у донора */
int rtl819x_hwnat_setup_tc(struct net_device *dev, enum tc_setup_type type,
			   void *type_data)
{
	struct flow_block_offload *f = type_data;
	struct flow_block_cb *block_cb;

	if (type != TC_SETUP_FT ||
	    f->binder_type != FLOW_BLOCK_BINDER_TYPE_CLSACT_INGRESS)
		return -EOPNOTSUPP;

	f->driver_block_list = &rtl819x_hwnat_block_list;

	switch (f->command) {
	case FLOW_BLOCK_BIND:
		block_cb = flow_block_cb_lookup(f->block, rtl819x_hwnat_cls_cb,
						dev);
		if (block_cb) {
			flow_block_cb_incref(block_cb);
			return 0;
		}
		block_cb = flow_block_cb_alloc(rtl819x_hwnat_cls_cb, dev, dev,
					       NULL);
		if (IS_ERR(block_cb))
			return PTR_ERR(block_cb);
		flow_block_cb_incref(block_cb);
		flow_block_cb_add(block_cb, f);
		list_add_tail(&block_cb->driver_list,
			      &rtl819x_hwnat_block_list);
		rtl819x_hwnat.binds++;
		return 0;
	case FLOW_BLOCK_UNBIND:
		block_cb = flow_block_cb_lookup(f->block, rtl819x_hwnat_cls_cb,
						dev);
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

/* блок indirect-привязки живёт до отвязки, освобождать нечего */
static void rtl819x_hwnat_indr_release(void *cb_priv)
{
}

/*
 * Обработчик правил для indirect-блоков. Ядро предлагает каждое правило
 * всем блокам flowtable по очереди, так что правила eth0 сюда тоже
 * приходят; разбирает и ставит их только блок eth0 (rtl819x_hwnat_cls_cb),
 * а этот всегда отказывается.
 */
static int rtl819x_hwnat_indr_cls_cb(enum tc_setup_type type,
				     void *type_data, void *cb_priv)
{
	return -EOPNOTSUPP;
}

/*
 * Прочие устройства flowtable (Wi-Fi rtl8192cd и т. п.), у которых нет
 * своего ndo_setup_tc. Привязка нужна только для того, чтобы flowtable с
 * flags offload вообще создался; потоки этих устройств остаются
 * программными.
 */
static int rtl819x_hwnat_indr_cb(struct net_device *dev, struct Qdisc *sch,
				 void *cb_priv, enum tc_setup_type type,
				 void *type_data, void *data,
				 void (*cleanup)(struct flow_block_cb *block_cb))
{
	struct flow_block_offload *f = type_data;
	struct flow_block_cb *block_cb;

	if (type != TC_SETUP_FT ||
	    f->binder_type != FLOW_BLOCK_BINDER_TYPE_CLSACT_INGRESS)
		return -EOPNOTSUPP;

	f->driver_block_list = &rtl819x_hwnat_block_list;

	switch (f->command) {
	case FLOW_BLOCK_BIND:
		if (flow_block_cb_lookup(f->block, rtl819x_hwnat_indr_cls_cb,
					 dev))
			return -EEXIST;
		block_cb = flow_indr_block_cb_alloc(rtl819x_hwnat_indr_cls_cb,
						    dev, cb_priv,
						    rtl819x_hwnat_indr_release,
						    f, dev, sch, data, cb_priv,
						    cleanup);
		if (IS_ERR(block_cb))
			return PTR_ERR(block_cb);
		flow_block_cb_add(block_cb, f);
		list_add_tail(&block_cb->driver_list,
			      &rtl819x_hwnat_block_list);
		rtl819x_hwnat.indr_binds++;
		return 0;
	case FLOW_BLOCK_UNBIND:
		block_cb = flow_block_cb_lookup(f->block,
						rtl819x_hwnat_indr_cls_cb, dev);
		if (!block_cb)
			return -ENOENT;
		flow_indr_block_cb_remove(block_cb, f);
		list_del(&block_cb->driver_list);
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

/* ---- /proc/rtl819x_hwnat ------------------------------------------------ */

static int rtl819x_hwnat_pokaz(struct seq_file *m, void *v)
{
	const struct rtl819x_hwnat_rule *r = &rtl819x_hwnat.last;

	mutex_lock(&rtl819x_hwnat.lock);
	seq_puts(m, "skeleton: nothing is offloaded, every rule is rejected\n");
	seq_printf(m, "bind: eth0 %lu, indirect %lu\n",
		   rtl819x_hwnat.binds, rtl819x_hwnat.indr_binds);
	seq_printf(m, "rules: replace %lu, parsed %lu, rejected %lu, destroy %lu, stats %lu\n",
		   rtl819x_hwnat.replace, rtl819x_hwnat.parsed,
		   rtl819x_hwnat.rejected, rtl819x_hwnat.destroy,
		   rtl819x_hwnat.stats);
	if (rtl819x_hwnat.have_last)
		seq_printf(m, "last: %s vid %u -> %u, %pI4:%u -> %pI4:%u as %pI4:%u -> %pI4:%u, %pM -> %pM\n",
			   r->ip_proto == IPPROTO_TCP ? "tcp" : "udp",
			   r->in_vid, r->out_vid,
			   &r->orig_saddr, ntohs(r->orig_sport),
			   &r->orig_daddr, ntohs(r->orig_dport),
			   &r->saddr, ntohs(r->sport),
			   &r->daddr, ntohs(r->dport),
			   r->eth.h_source, r->eth.h_dest);
	mutex_unlock(&rtl819x_hwnat.lock);

	return 0;
}

/* ---- жизненный цикл ----------------------------------------------------- */

int rtl819x_hwnat_init(struct net_device *dev)
{
	int ret;

	mutex_init(&rtl819x_hwnat.lock);
	rtl819x_hwnat.eth = dev;

	ret = flow_indr_dev_register(rtl819x_hwnat_indr_cb, &rtl819x_hwnat);
	if (ret)
		return ret;

	proc_create_single("rtl819x_hwnat", 0444, NULL, rtl819x_hwnat_pokaz);

	return 0;
}

void rtl819x_hwnat_exit(struct net_device *dev)
{
	remove_proc_entry("rtl819x_hwnat", NULL);
	flow_indr_dev_unregister(rtl819x_hwnat_indr_cb, &rtl819x_hwnat,
				 rtl819x_hwnat_indr_release);
}
