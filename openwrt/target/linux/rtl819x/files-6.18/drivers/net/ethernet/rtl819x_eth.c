// SPDX-License-Identifier: GPL-2.0-only
/*
 * Ethernet-контроллер Realtek RTL819x (RTL8196E / RTL8197D / RTL8197F).
 *
 * Структура драйвера повторяет rtl838x_eth.c из OpenWrt (там та же схема:
 * NAPI + phylink + DSA), но слой работы с DMA свой: у RTL838x кольцо это
 * массив адресов пакетных заголовков, а у RTL819x — обычные дескрипторы
 * с полями opts1..opts5.
 *
 * Значения регистров и логика инициализации сверены с вендорским SDK
 * (drivers/net/rtl819x: rtl819x_swNic.c, AsicDriver/), но код написан
 * заново под API ядра 6.x: вендорский рассчитан на 3.10 и требует
 * патчей в сетевой стек, которые нам не нужны.
 *
 * Проверено на ELECTRA RX23302 (RTL8197FH-VG).
 */

#include <linux/bitfield.h>
#include <linux/dma-mapping.h>
#include <linux/etherdevice.h>
#include <linux/if_vlan.h>
#include <linux/interrupt.h>
#include <linux/ipv6.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/of.h>
#include <linux/of_mdio.h>
#include <linux/of_net.h>
#include <linux/phylink.h>
#include <linux/platform_device.h>
#include <net/ip.h>

#include "rtl819x_eth.h"

#define RTL819X_DRV_NAME	"rtl819x-eth"

/* Кадр + заголовок свитча + VLAN + FCS */
#define RTL819X_FRAME_OVERHEAD	(ETH_HLEN + VLAN_HLEN + ETH_FCS_LEN)
#define RTL819X_MAX_FRAME	2000
/* буфер под пакет: кадр + заголовок свитча, с запасом */
#define RTL819X_RX_BUF_SIZE	2048

struct rtl819x_eth;

static void rtl819x_rings_free(struct rtl819x_eth *eth);
static int rtl819x_rings_init(struct rtl819x_eth *eth);
static void rtl819x_irq_enable_rx(struct rtl819x_eth *eth);
static int rtl819x_poll(struct napi_struct *napi, int budget);
static void rtl819x_tx_reclaim(struct rtl819x_eth *eth, unsigned int r,
			       int budget);

/* группы внутреннего свитча: 9 = LAN (eth0.9), 8 = WAN (eth0.8) */
#define RTL_NASH_VID		9
#define RTL_WAN_VID		8
/* режим роутера, см. rtl819x_tx_gnezda() */
static bool rtl819x_router;
#define RTL_WAN_GNEZDO		0	/* порт 8367 синего гнезда WAN */
static void rtl819x_fdb_learn(const u8 *mac, unsigned int port);

/**
 * struct rtl819x_ring - кольцо дескрипторов с сопутствующими буферами
 * @desc:	дескрипторы, видимые железу
 * @dma:	физический адрес кольца (железо знает только его)
 * @skb:	буферы, соответствующие дескрипторам
 * @size:	число дескрипторов
 * @head:	индекс, с которым работает драйвер
 * @tail:	индекс, до которого дошло железо
 */
struct rtl819x_ring {
	void		*desc;
	dma_addr_t	dma;
	struct sk_buff	**skb;
	unsigned int	size;
	unsigned int	head;
	unsigned int	tail;
};

/**
 * struct rtl819x_soc - различия между чипами семейства
 * @cpu_port:		номер порта, которым свитч подключён к процессору
 * @max_mtu:		максимальный MTU
 * @rx_rings:		сколько колец приёма использовать
 * @tx_rings:		сколько колец передачи использовать
 * @hw_init:		инициализация конкретного чипа
 * @create_tx_header:	добавление служебного заголовка свитча к пакету
 */
struct rtl819x_soc {
	int	cpu_port;
	int	max_mtu;
	int	rx_rings;
	int	tx_rings;
	void	(*hw_init)(struct rtl819x_eth *eth);
	void	(*create_tx_header)(struct rtl819x_tx_desc *desc,
				    unsigned int port, int prio);
};

/**
 * struct rtl819x_eth - состояние драйвера
 * @base:	регистры CPU-интерфейса (ioremap из device tree)
 * @dev:	сетевое устройство
 * @pdev:	платформенное устройство
 * @soc:	описание конкретного чипа
 * @napi:	контекст NAPI
 * @rx / @tx:	кольца
 * @lock:	защита регистров
 */
struct rtl819x_eth {
	void __iomem		*base;
	struct net_device	*dev;
	struct platform_device	*pdev;
	const struct rtl819x_soc *soc;
	struct napi_struct	napi;
	struct phylink		*phylink;
	struct phylink_config	phylink_config;
	struct rtl819x_ring	rx[RTL819X_RX_RING_COUNT];
	struct rtl819x_ring	tx[RTL819X_TX_RING_COUNT];
	spinlock_t		lock;
	int			irq;

};

/* ---- доступ к регистрам ---------------------------------------------- */

static inline u32 rtl819x_r32(struct rtl819x_eth *eth, unsigned int reg)
{
	return readl(eth->base + reg);
}

static inline void rtl819x_w32(struct rtl819x_eth *eth, u32 val, unsigned int reg)
{
	writel(val, eth->base + reg);
}

static inline void rtl819x_m32(struct rtl819x_eth *eth, u32 clear, u32 set,
			       unsigned int reg)
{
	u32 val = rtl819x_r32(eth, reg);

	val &= ~clear;
	val |= set;
	rtl819x_w32(eth, val, reg);
}

/* ---- прерывания ------------------------------------------------------- */

static void rtl819x_irq_disable_all(struct rtl819x_eth *eth)
{
	rtl819x_w32(eth, 0, RTL819X_CPUIIMR);
}

/*
 * На время работы NAPI вендор снимает только приёмные биты, оставляя
 * передачу и смену линка разрешёнными (rtl_nic.c:7443).
 */
static void rtl819x_irq_mask_rx(struct rtl819x_eth *eth)
{
	/*
	 * Вместе с приёмом снимаем и «всё передано»: освобождение
	 * отправленных буферов теперь тоже делает NAPI. Раньше оно шло прямо
	 * в обработчике прерывания, и на каждый отправленный пакет приходилось
	 * своё прерывание — 105-130 тыс. в секунду при 600 Мбит/с (замер
	 * 28.10.2026), заметная доля процессора на одном только входе-выходе.
	 */
	rtl819x_m32(eth, RTL819X_IIMR_RX_BITS | RTL819X_TX_ALL_DONE_IE_ALL, 0,
		    RTL819X_CPUIIMR);
}

static void rtl819x_irq_enable_rx(struct rtl819x_eth *eth)
{
	rtl819x_w32(eth, RTL819X_IIMR_NORMAL, RTL819X_CPUIIMR);
}


static irqreturn_t rtl819x_irq(int irq, void *dev_id)
{
	struct net_device *dev = dev_id;
	struct rtl819x_eth *eth = netdev_priv(dev);
	u32 status;

	status = rtl819x_r32(eth, RTL819X_CPUIISR);
	if (!status)
		return IRQ_NONE;


	/* регистр статуса очищается записью единиц */
	rtl819x_w32(eth, status, RTL819X_CPUIISR);

	/*
	 * Оставляем только разрешённые источники. Без этого обработчик
	 * реагирует на биты, которых не просил, и линия висит взведённой
	 * (rtl_nic.c:7507).
	 */
	status &= rtl819x_r32(eth, RTL819X_CPUIIMR);
	if (!status)
		return IRQ_HANDLED;

	/* и приём, и конец передачи разбирает NAPI (rtl819x_poll) */
	if (status & (RTL819X_RX_DONE_IP_ALL | RTL819X_PKTHDR_RUNOUT_IP_ALL |
		      RTL819X_TX_ALL_DONE_IP_ALL)) {
		if (napi_schedule_prep(&eth->napi)) {
			rtl819x_irq_mask_rx(eth);
			__napi_schedule(&eth->napi);
		}
	}

	return IRQ_HANDLED;
}

/* ---- кольца дескрипторов ---------------------------------------------- */

/*
 * Железу отдаётся физический адрес кольца, драйвер работает по виртуальному.
 * dma_alloc_coherent() даёт оба сразу и гарантирует, что процессор и DMA
 * видят одинаковые данные — на MIPS это некэшируемое отображение.
 *
 * Вендорский код вместо этого выделял обычную память и вручную ставил
 * бит некэшируемости (UNCACHE_MASK), полагаясь на то, что железо само
 * отбросит старшие биты адреса. Работает, но завязано на устройство
 * адресных сегментов MIPS, поэтому здесь штатный DMA API.
 *
 * Выравнивание на 256 байт — требование контроллера, не оптимизация.
 */

static void rtl819x_free_ring(struct rtl819x_eth *eth, struct rtl819x_ring *ring,
			      size_t desc_size, bool is_rx)
{
	unsigned int i;

	if (ring->skb) {
		for (i = 0; i < ring->size; i++) {
			if (!ring->skb[i])
				continue;
			dev_kfree_skb_any(ring->skb[i]);
			ring->skb[i] = NULL;
		}
		kfree(ring->skb);
		ring->skb = NULL;
	}

	if (ring->desc) {
		dma_free_coherent(&eth->pdev->dev,
				  ring->size * desc_size + RTL819X_DESC_ALIGN,
				  ring->desc, ring->dma);
		ring->desc = NULL;
	}
}

static int rtl819x_alloc_ring(struct rtl819x_eth *eth, struct rtl819x_ring *ring,
			      unsigned int count, size_t desc_size)
{
	size_t bytes = count * desc_size + RTL819X_DESC_ALIGN;

	ring->size = count;
	ring->head = 0;
	ring->tail = 0;

	ring->desc = dma_alloc_coherent(&eth->pdev->dev, bytes, &ring->dma,
					GFP_KERNEL);
	if (!ring->desc)
		return -ENOMEM;

	/* контроллер требует, чтобы кольцо начиналось с границы 256 байт */
	if (!IS_ALIGNED(ring->dma, RTL819X_DESC_ALIGN)) {
		dma_addr_t aligned = ALIGN(ring->dma, RTL819X_DESC_ALIGN);
		unsigned int shift = aligned - ring->dma;

		ring->desc += shift;
		ring->dma = aligned;
	}

	ring->skb = kcalloc(count, sizeof(*ring->skb), GFP_KERNEL);
	if (!ring->skb) {
		rtl819x_free_ring(eth, ring, desc_size, false);
		return -ENOMEM;
	}

	return 0;
}

/* Подвесить буфер к приёмному дескриптору и отдать его железу. */
static int rtl819x_rx_refill(struct rtl819x_eth *eth, struct rtl819x_ring *ring,
			     unsigned int idx, bool in_napi)
{
	struct rtl819x_rx_desc *desc = (struct rtl819x_rx_desc *)ring->desc + idx;
	struct sk_buff *skb;
	dma_addr_t dma;

	/*
	 * Из NAPI берём буфер из его собственного запаса: туда же
	 * napi_consume_skb() возвращает отправленные. Отступ под выравнивание
	 * IP тот же, что у netdev_alloc_skb_ip_align(). При открытии
	 * интерфейса (не в softirq) — как раньше.
	 */
	if (in_napi)
		skb = napi_alloc_skb(&eth->napi, RTL819X_RX_BUF_SIZE);
	else
		skb = netdev_alloc_skb_ip_align(eth->dev, RTL819X_RX_BUF_SIZE);
	if (!skb)
		return -ENOMEM;

	dma = dma_map_single(&eth->pdev->dev, skb->data, RTL819X_RX_BUF_SIZE,
			     DMA_FROM_DEVICE);
	if (dma_mapping_error(&eth->pdev->dev, dma)) {
		dev_kfree_skb_any(skb);
		return -ENOMEM;
	}

	ring->skb[idx] = skb;
	desc->addr = cpu_to_le32(dma);
	desc->opts2 = 0;
	/*
	 * opts4/opts5 тоже чистим: по ним rtl819x_rx_csum_ok() решает, верить
	 * ли сумме. Если железо на следующем кадре их не перепишет (например,
	 * не разберёт метку 802.1Q), там остались бы биты «IPv4, сумма верна»
	 * от прошлого кадра в этом же дескрипторе.
	 */
	desc->opts4 = 0;
	desc->opts5 = 0;

	/*
	 * Бит own отдаём последним: до этого момента дескриптор наш,
	 * после — железа. Барьер гарантирует, что адрес буфера уже
	 * записан, когда контроллер увидит own.
	 */
	dma_wmb();
	desc->opts1 = cpu_to_le32(RTL819X_DESC_SWCORE_OWNED |
				  (idx == ring->size - 1 ? RTL819X_DESC_WRAP : 0) |
				  (RTL819X_RX_BUF_SIZE << RTL819X_RX_EXTSIZE_SHIFT));

	return 0;
}

static int rtl819x_rings_init(struct rtl819x_eth *eth)
{
	unsigned int i, j;
	int ret;

	for (i = 0; i < eth->soc->rx_rings; i++) {
		ret = rtl819x_alloc_ring(eth, &eth->rx[i], RTL819X_RX_RING_SIZE,
					 sizeof(struct rtl819x_rx_desc));
		if (ret)
			goto err;

		for (j = 0; j < eth->rx[i].size; j++) {
			ret = rtl819x_rx_refill(eth, &eth->rx[i], j, false);
			if (ret)
				goto err;
		}

		/* сообщаем железу, где лежит кольцо */
		rtl819x_w32(eth, eth->rx[i].dma, RTL819X_CPURPDCR(i));
	}

	for (i = 0; i < eth->soc->tx_rings; i++) {
		struct rtl819x_tx_desc *desc;

		ret = rtl819x_alloc_ring(eth, &eth->tx[i], RTL819X_TX_RING_SIZE,
					 sizeof(struct rtl819x_tx_desc));
		if (ret)
			goto err;

		/* все дескрипторы передачи изначально свободны */
		desc = (struct rtl819x_tx_desc *)eth->tx[i].desc;
		for (j = 0; j < eth->tx[i].size; j++) {
			desc[j].opts1 = cpu_to_le32(j == eth->tx[i].size - 1 ?
						    RTL819X_DESC_WRAP : 0);
			desc[j].addr = 0;
		}

		rtl819x_w32(eth, eth->tx[i].dma, RTL819X_CPUTPDCR(i));
	}

	/*
	 * Граница колец передачи. ВАЖНО: регистр содержит ДВА поля по
	 * шестнадцать бит, по одному на кольцо — так прямо и написано у
	 * вендора: "the register only provide 16-bit for each tx ring"
	 * (rtl819x_swNic.c:374). Младшая половина — кольцо 0, старшая —
	 * кольцо 1.
	 *
	 * Проверено на живом загрузчике (30.08): у него в этом регистре
	 * лежит 0x00180048, то есть 0x0048 кольцу 0 и 0x0018 кольцу 1.
	 * Мы раньше писали одно число, и старшая половина оставалась нулём
	 * при ВКЛЮЧЁННОМ кольце 1 — контроллер видел кольцо, ведущее в
	 * никуда, и не забирал дескрипторы ни из одного.
	 */
	rtl819x_w32(eth,
		    (((eth->tx[0].size - 1) * sizeof(struct rtl819x_tx_desc))
		     & 0xFFFF) |
		    ((((eth->tx[1].size - 1) * sizeof(struct rtl819x_tx_desc))
		      & 0xFFFF) << 16),
		    RTL819X_DMA_CR1);
	rtl819x_w32(eth, RTL819X_TX_RING0_TAIL_AWARE, RTL819X_DMA_CR4);

	/*
	 * Кольца 2 и 3 нам не нужны, а вот кольцо 1 вендор оставляет
	 * включённым вместе с нулевым: без этого конечный автомат передачи
	 * работает неправильно (rtl865x_asicL2.c:7670).
	 */
	rtl819x_m32(eth, RTL819X_TX_RING3_EN | RTL819X_TX_RING2_EN,
		    RTL819X_TX_RING0_EN | RTL819X_TX_RING1_EN,
		    RTL819X_TXRINGCR);

	return 0;

err:
	rtl819x_rings_free(eth);
	return ret;
}

static void rtl819x_rings_free(struct rtl819x_eth *eth)
{
	unsigned int i;

	for (i = 0; i < RTL819X_RX_RING_COUNT; i++)
		rtl819x_free_ring(eth, &eth->rx[i],
				  sizeof(struct rtl819x_rx_desc), true);

	for (i = 0; i < RTL819X_TX_RING_COUNT; i++)
		rtl819x_free_ring(eth, &eth->tx[i],
				  sizeof(struct rtl819x_tx_desc), false);
}

/* ---- приём -------------------------------------------------------------- */

/*
 * Обходим кольцо, пока встречаются дескрипторы, которые железо вернуло
 * процессору (бит own снят). Для каждого: отвязать буфер от DMA, отдать
 * пакет в стек, подвесить на его место новый буфер.
 */
/*
 * РАЗВЕДКА v6.3: откуда пришёл кадр.
 *
 * С включённым CPU-тегом внешний RTL8367 ставит на кадр номер гнезда, а
 * 8197F (P0GMIICR биты 25/26) снимает тег и, по донору iptime A2004MU,
 * кладёт номер в приёмный дескриптор: opts4.spa, биты 13..15. Проверяем
 * это на своей плате: считаем кадры по spa и храним сырые opts последнего
 * кадра каждого значения вместе с MAC-адресом отправителя.
 *
 *   cat /proc/rtl819x_spa
 *
 * Только наблюдение: на пересылку кадров ничего не влияет.
 */
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>

static unsigned long rtl819x_spa_schet[8];
static u32 rtl819x_spa_opts[8][5];
static u8 rtl819x_spa_mac[8][ETH_ALEN];
/* v6.5: сколько кадров ушло в какие гнёзда (см. rtl819x_tx_gnezda) */
static unsigned long rtl819x_tx_wan, rtl819x_tx_lan, rtl819x_tx_bez_metki;
/* сумму TCP/UDP проверило железо / проверит ядро (см. rtl819x_rx_csum_ok) */
static unsigned long rtl819x_rx_csum_hw, rtl819x_rx_csum_sw;

/*
 * Почему свитч отдал кадр процессору: поле reason (opts3 биты 0..15)
 * приёмного дескриптора, по вендорскому rtl819x_swNic.h (struct rx_desc).
 * Это тот же why2cpu, что в HSA. Считаем по коду причины (биты 1..4) и
 * помним последний полный код для каждой — см. /proc/rtl819x_spa.
 */
static unsigned long rtl819x_why_schet[16];
static u16 rtl819x_why_posl[16];

static void rtl819x_spa_uchet(struct rtl819x_rx_desc *desc, struct sk_buff *skb)
{
	u32 o4 = le32_to_cpu(desc->opts4);
	unsigned int spa = (o4 >> 13) & 7;
	u16 why = le32_to_cpu(desc->opts3) & 0xffff;

	rtl819x_why_schet[(why >> 1) & 0xf]++;
	rtl819x_why_posl[(why >> 1) & 0xf] = why;
	rtl819x_spa_schet[spa]++;
	/*
	 * Снимок дескриптора — отладка, на каждом кадре он не нужен:
	 * дескрипторы лежат в некэшируемой памяти, и пять лишних чтений
	 * на пакет при 50 тыс. пакетов в секунду заметны. Берём первые
	 * 64 кадра каждого источника, потом каждый 64-й.
	 */
	if (rtl819x_spa_schet[spa] > 64 && (rtl819x_spa_schet[spa] & 63))
		return;
	rtl819x_spa_opts[spa][0] = le32_to_cpu(desc->opts1);
	rtl819x_spa_opts[spa][1] = le32_to_cpu(desc->opts2);
	rtl819x_spa_opts[spa][2] = le32_to_cpu(desc->opts3);
	rtl819x_spa_opts[spa][3] = o4;
	rtl819x_spa_opts[spa][4] = le32_to_cpu(desc->opts5);
	if (skb->len >= 2 * ETH_ALEN)
		memcpy(rtl819x_spa_mac[spa], skb->data + ETH_ALEN, ETH_ALEN);
}

static int rtl819x_spa_pokaz(struct seq_file *m, void *v)
{
	unsigned int i;

	seq_printf(m, "P0GMIICR(414c)=%08x\n", *(volatile u32 *)0xBB80414C);
	seq_printf(m, "tx: wan(vid 8 -> port 0) %lu, lan(vid 9 -> 3,4) %lu, untagged(-> 3,4) %lu\n",
		   rtl819x_tx_wan, rtl819x_tx_lan, rtl819x_tx_bez_metki);
	seq_printf(m, "rx csum: hw %lu, sw %lu\n",
		   rtl819x_rx_csum_hw, rtl819x_rx_csum_sw);
	for (i = 0; i < 16; i++)
		if (rtl819x_why_schet[i])
			seq_printf(m, "why2cpu prichina %u: %lu frames, last code %04x\n",
				   i, rtl819x_why_schet[i], rtl819x_why_posl[i]);
	for (i = 0; i < 8; i++)
		seq_printf(m, "spa %u: %lu frames, last from %pM, opts1 %08x opts2 %08x opts3 %08x opts4 %08x opts5 %08x\n",
			   i, rtl819x_spa_schet[i], rtl819x_spa_mac[i],
			   rtl819x_spa_opts[i][0], rtl819x_spa_opts[i][1],
			   rtl819x_spa_opts[i][2], rtl819x_spa_opts[i][3],
			   rtl819x_spa_opts[i][4]);
	return 0;
}

/*
 * Контрольные суммы приёма.
 *
 * Без них каждый пакет TCP приходит в GRO с CHECKSUM_NONE, и ядро считает
 * сумму по всему его телу (tcp4_gro_receive -> skb_gro_checksum_validate).
 * Это лишний проход по данным, которые только что пришли по DMA и в кэше
 * не лежат. При маршрутизации тело пакета больше никто не читает, так что
 * для проходящего трафика это чистая потеря.
 *
 * Донор проверял биты на кадрах без метки 802.1Q, а у нас почти всё идёт
 * с меткой (eth0.8, eth0.9). Поэтому железу верим, только если и сами
 * видим в кадре TCP или UDP, не фрагмент. Во всех прочих случаях остаётся
 * CHECKSUM_NONE, то есть ядро проверяет само, как раньше. Если железо
 * метку не разбирает, оно просто не выставит биты - хуже, чем было, не
 * станет. Сколько кадров проверило железо - в /proc/rtl819x_spa.
 */
static bool rtl819x_rx_csum_ok(const struct rtl819x_rx_desc *desc,
			       const struct sk_buff *skb)
{
	u32 o4 = le32_to_cpu(desc->opts4);
	u32 o5 = le32_to_cpu(desc->opts5);
	const u8 *d = skb->data;
	unsigned int off = ETH_HLEN;
	u16 proto;

	if (skb->len < ETH_HLEN)
		return false;

	proto = (d[12] << 8) | d[13];
	if (proto == ETH_P_8021Q) {
		if (skb->len < VLAN_ETH_HLEN)
			return false;
		proto = (d[16] << 8) | d[17];
		off = VLAN_ETH_HLEN;
	}

	if (proto == ETH_P_IP) {
		const struct iphdr *iph = (const struct iphdr *)(d + off);

		if (!(o4 & RTL819X_RX_IPV4) ||
		    (o5 & (RTL819X_RX_L3CS_OK | RTL819X_RX_L4CS_OK)) !=
		    (RTL819X_RX_L3CS_OK | RTL819X_RX_L4CS_OK))
			return false;
		if (skb->len < off + sizeof(*iph) || ip_is_fragment(iph))
			return false;

		return iph->protocol == IPPROTO_TCP ||
		       iph->protocol == IPPROTO_UDP;
	}

	if (proto == ETH_P_IPV6) {
		const struct ipv6hdr *ip6h = (const struct ipv6hdr *)(d + off);

		if (!(o4 & RTL819X_RX_IPV6) || !(o5 & RTL819X_RX_L4CS_OK))
			return false;
		if (skb->len < off + sizeof(*ip6h))
			return false;

		return ip6h->nexthdr == IPPROTO_TCP ||
		       ip6h->nexthdr == IPPROTO_UDP;
	}

	return false;
}

static int rtl819x_rx_ring_poll(struct rtl819x_eth *eth, unsigned int r,
				int budget)
{
	struct rtl819x_ring *ring = &eth->rx[r];
	struct net_device *dev = eth->dev;
	struct rtl819x_rx_desc *descs = ring->desc;
	int done = 0;
	u32 cdp;
	unsigned int hw;

	/*
	 * Контроллер держит собственную позицию в CPURPDCR и возобновляет
	 * обход кольца не обязательно там, где мы его оставили. Если наш
	 * индекс смотрит на дескриптор, всё ещё принадлежащий железу, а по
	 * его позиции лежит принятый пакет, значит указатели разошлись:
	 * железо упёрлось в занятый дескриптор и ждёт, а мы ждём пакет,
	 * которого в нашей точке не будет никогда.
	 */
	cdp = rtl819x_r32(eth, RTL819X_CPURPDCR(r));
	hw = (cdp - (u32)ring->dma) / sizeof(*descs);

	if (hw < ring->size &&
	    (le32_to_cpu(descs[ring->head].opts1) & RTL819X_DESC_SWCORE_OWNED) &&
	    !(le32_to_cpu(descs[hw].opts1) & RTL819X_DESC_SWCORE_OWNED))
		ring->head = hw;

	while (done < budget) {
		struct rtl819x_rx_desc *desc;
		struct sk_buff *skb;
		unsigned int len;
		u32 opts1;

		desc = (struct rtl819x_rx_desc *)ring->desc + ring->head;
		opts1 = le32_to_cpu(desc->opts1);

		/* дескриптор всё ещё у железа — пакетов больше нет */
		if (opts1 & RTL819X_DESC_SWCORE_OWNED)
			break;

		/* железо записало данные до того, как сняло own */
		dma_rmb();

		skb = ring->skb[ring->head];
		len = le32_to_cpu(desc->opts2) & RTL819X_RX_LEN_MASK;
		/*
		 * Длина в дескрипторе включает контрольную сумму кадра:
		 * бит EXCLUDE_CRC в CPUICR вендор не выставляет, и в приёмнике
		 * длина уменьшается на 4 вручную (rtl819x_swNic.c:539).
		 */
		if (len > ETH_FCS_LEN)
			len -= ETH_FCS_LEN;
		else
			len = 0;

		if (unlikely(!skb || !len || len > RTL819X_RX_BUF_SIZE)) {
			dev->stats.rx_errors++;
			goto next;
		}

		if (unlikely(opts1 & (RTL819X_RX_CRC_ERR | RTL819X_RX_FAE))) {
			dev->stats.rx_errors++;
			if (opts1 & RTL819X_RX_CRC_ERR)
				dev->stats.rx_crc_errors++;
			if (opts1 & RTL819X_RX_FAE)
				dev->stats.rx_frame_errors++;
			goto next;
		}

		dma_unmap_single(&eth->pdev->dev, le32_to_cpu(desc->addr),
				 RTL819X_RX_BUF_SIZE, DMA_FROM_DEVICE);
		ring->skb[ring->head] = NULL;

		skb_put(skb, len);
		rtl819x_spa_uchet(desc, skb);
		if ((dev->features & NETIF_F_RXCSUM) &&
		    rtl819x_rx_csum_ok(desc, skb)) {
			skb->ip_summed = CHECKSUM_UNNECESSARY;
			rtl819x_rx_csum_hw++;
		} else {
			rtl819x_rx_csum_sw++;
		}
		skb->protocol = eth_type_trans(skb, dev);
		/*
		 * Режим роутера: кадр приходит без метки, номер гнезда — в
		 * дескрипторе (opts4 биты 13..15, как в rtl819x_spa_uchet).
		 * Ставим VID сами, чтобы 8021q раздал кадры в eth0.8 / eth0.9.
		 * Если метка всё же есть в данных — отдаём её стеку как есть.
		 */
		/*
		 * Гнездо источника учим всегда, с меткой кадр или без: 8367
		 * отдаёт кадры к процессору С МЕТКОЙ (порт 7 — tagged-член
		 * VLAN 8/9), так что без этого таблица оставалась пустой
		 * (замер 28.10.2026 11:58: fdb пуст, LAN2 получал весь поток).
		 */
		if (READ_ONCE(rtl819x_router))
			rtl819x_fdb_learn(eth_hdr(skb)->h_source,
					  (le32_to_cpu(desc->opts4) >> 13) & 7);
		if (READ_ONCE(rtl819x_router) &&
		    skb->protocol != htons(ETH_P_8021Q)) {
			unsigned int spa = (le32_to_cpu(desc->opts4) >> 13) & 7;

			__vlan_hwaccel_put_tag(skb, htons(ETH_P_8021Q),
					       spa == RTL_WAN_GNEZDO ?
					       RTL_WAN_VID : RTL_NASH_VID);
		}
		dev->stats.rx_packets++;
		dev->stats.rx_bytes += len;

		napi_gro_receive(&eth->napi, skb);
		done++;

next:
		/* вернуть дескриптор в оборот с новым буфером */
		if (ring->skb[ring->head]) {
			dma_unmap_single(&eth->pdev->dev,
					 le32_to_cpu(desc->addr),
					 RTL819X_RX_BUF_SIZE, DMA_FROM_DEVICE);
			dev_kfree_skb_any(ring->skb[ring->head]);
			ring->skb[ring->head] = NULL;
		}

		if (rtl819x_rx_refill(eth, ring, ring->head, true)) {
			/*
			 * Буфера нет — дескриптор и индекс не трогаем вовсе,
			 * пакет останется в кольце до следующего захода
			 * (rtl819x_swNic.c:581-600). Двигать индекс здесь
			 * значило бы потерять место в кольце навсегда.
			 */
			dev->stats.rx_dropped++;
			break;
		}

		ring->head = (ring->head + 1) % ring->size;

		/*
		 * Признак нехватки дескрипторов снимаем здесь, а не в
		 * обработчике прерывания: там он снимался раньше, чем
		 * появлялся свободный дескриптор, и взводился снова
		 * (rtl819x_swNic.c:576).
		 */
		rtl819x_w32(eth, RTL819X_PKTHDR_RUNOUT_IP_ALL,
			    RTL819X_CPUIISR);
	}

	return done;
}

static int rtl819x_poll(struct napi_struct *napi, int budget)
{
	struct rtl819x_eth *eth = container_of(napi, struct rtl819x_eth, napi);
	struct net_device *dev = eth->dev;
	int done = 0;
	unsigned int r;

	/*
	 * Сначала освобождаем отправленное: буферы вернутся в запас NAPI
	 * (napi_consume_skb), и приёмник ниже возьмёт их оттуда же.
	 * Блокировка та же, что у start_xmit: он тоже зовёт reclaim.
	 */
	spin_lock_bh(&eth->lock);
	rtl819x_tx_reclaim(eth, 0, budget);
	spin_unlock_bh(&eth->lock);
	if (netif_queue_stopped(dev))
		netif_wake_queue(dev);

	for (r = 0; r < eth->soc->rx_rings && done < budget; r++)
		done += rtl819x_rx_ring_poll(eth, r, budget - done);

	/* бюджет не выбран — пакеты кончились, можно снова спать на прерывании */
	if (done < budget && napi_complete_done(napi, done))
		rtl819x_irq_enable_rx(eth);

	return done;
}

/* ---- передача ------------------------------------------------------------ */

/*
 * Освободить буферы пакетов, которые железо уже отправило.
 * budget > 0 — зовут из NAPI: буфер уходит в запас NAPI, откуда его снова
 * возьмёт приёмник; budget == 0 — из start_xmit (кольцо забито).
 */
static void rtl819x_tx_reclaim(struct rtl819x_eth *eth, unsigned int r,
			       int budget)
{
	struct rtl819x_ring *ring = &eth->tx[r];
	u32 cdp = rtl819x_r32(eth, RTL819X_CPUTPDCR(r));
	unsigned int hw;

	/*
	 * Докуда железо дошло, спрашиваем у него самого: CPUTPDCR держит
	 * текущую позицию в кольце передачи, ровно как CPURPDCR в приёмном
	 * (rtl819x_swNic.c:1109). Ждать снятия бита владения нельзя —
	 * именно на этом кольцо передачи вставало намертво.
	 */
	hw = (cdp - (u32)ring->dma) / sizeof(struct rtl819x_tx_desc);

	/* ВРЕМЕННО (29.08): первые десять разборов печатаем целиком.
	 * Надо понять, движется ли железо по кольцу и сходится ли
	 * арифметика адресов: cdp из регистра против ring->dma. */
	{
		static int _n;

		if (_n < 10) {
			_n++;
			pr_info("rtl819x-tx: cdp=%08x dma=%08x hw=%u head=%u tail=%u size=%u\n",
				cdp, (u32)ring->dma, hw, ring->head,
				ring->tail, ring->size);
		}
	}

	if (hw >= ring->size)
		return;

	while (ring->tail != hw) {
		if (ring->skb[ring->tail]) {
			struct rtl819x_tx_desc *desc =
				(struct rtl819x_tx_desc *)ring->desc + ring->tail;
			dma_unmap_single(&eth->pdev->dev,
					 le32_to_cpu(desc->addr),
					 ring->skb[ring->tail]->len,
					 DMA_TO_DEVICE);

			/* отправленным считаем то, что железо прошло,
			 * а не то, что мы положили в кольцо */
			eth->dev->stats.tx_packets++;
			eth->dev->stats.tx_bytes += ring->skb[ring->tail]->len;

			napi_consume_skb(ring->skb[ring->tail], budget);
			ring->skb[ring->tail] = NULL;
		}

		ring->tail = (ring->tail + 1) % ring->size;
	}
}

/* ---- внутренний коммутатор процессора ---------------------------------
 *
 * ОТКРЫТО 30.08.2026 на заводской прошивке, которая на этой же плате
 * работает. Её собственные таблицы говорят прямо:
 *
 *     /proc/rtl865x/l2    3c:97:0e:c7:0d:f5 FID:0 mbr(3) FWD DYN
 *     /proc/rtl865x/vlan  VID[9] member ports: 1 2 3 4 8
 *                         VID[1] member ports: 0
 *     /proc/rtl865x/port_status  Port3 LinkUp | NWay | Speed 1G
 *
 * MAC клиента в LAN1 учтён ВНУТРЕННИМ коммутатором на порту 3. Значит гнёзда
 * LAN висят на портах 1..4 внутреннего коммутатора, порт процессора у
 * него восьмой (RTL865XC_PORT_NUMBER = 9, порты 0..8), а внешний
 * RTL8367D сидит на внутреннем порту 0 и обслуживает WAN — отсюда и
 * PCRP0 с принудительным гигабитом MAC-к-MAC.
 *
 * Пока этой таблицы нет, кадры не ходят ни туда, ни обратно: коммутатор
 * не знает ни одной группы, в которую входил бы порт процессора.
 *
 * Порт 0 в группу ВХОДИТ, и это выяснено опытом 30.08, а не рассуждением.
 * Сначала я его исключил, боясь сшить WAN с LAN. С группой из портов
 * 1..4 и 8 вышло вот что: eth0 передал 30 кадров и принял ровно те же
 * 30, а получатель не увидел ничего. То есть в группе не было ни одного
 * живого получателя, кроме нас самих.
 *
 * Порт 0 — единственный живой внешний порт внутреннего коммутатора:
 * только у него PSRP0 = 0x307A, у портов 1..4 всех одинаково 0x30E0.
 * Через него идёт RGMII на внешний RTL8367D, а гнёзда сидят на 8367D —
 * это видно и на снимке платы: крупный корпус стоит вплотную к ряду
 * развязывающих трансформаторов.
 *
 * Гнездо WAN пустое, так что сшивания домашней сети с собой не будет.
 *
 * Код перенесён дословно из вендорского SDK:
 *     rtl8651_setAsicVlan        AsicDriver/rtl865x_asicCom.c:99
 *     _rtl8651_forceAddAsicEntry AsicDriver/rtl865x_asicBasic.c:177
 * Отступление одно: вендорские ожидания готовности написаны бесконечным
 * while, здесь они ограничены счётчиком. Зависнуть на загрузке хуже, чем
 * не настроить коммутатор и сказать об этом в журнале.
 */

#define RTL_SWCORE_BASE		0xBB800000
#define RTL_TACI_BASE		(RTL_SWCORE_BASE + 0x4D00)
#define RTL_SWTACR		(RTL_TACI_BASE + 0x000)
#define RTL_SWTASR		(RTL_TACI_BASE + 0x004)
#define RTL_SWTAA		(RTL_TACI_BASE + 0x008)
#define RTL_TCR(n)		(RTL_TACI_BASE + 0x020 + ((n) << 2))
#define RTL_ALE_BASE		(RTL_SWCORE_BASE + 0x4400)
#define RTL_SWTCR0		(RTL_ALE_BASE + 0x18)
#define RTL_PCRAM_BASE		(RTL_SWCORE_BASE + 0x4100)
#define RTL_PCRP(n)		(RTL_PCRAM_BASE + 0x004 + ((n) << 2))
#define RTL_PSRP(n)		(RTL_PCRAM_BASE + 0x028 + ((n) << 2))
/* PVCR0..PVCR4: по два порта на регистр, младшая половина и старшая */
#define RTL_PVCR(n)		(RTL_SWCORE_BASE + 0x4A00 + 0x08 + ((n) << 2))

/* таблицы коммутатора адресуются собственным окном, не через ioremap */
#define RTL_SWTBL_BASE		0xBB000000
#define RTL_TYPE_VLAN_TABLE	6
#define RTL_TBL_ENTRY_LEN	32

#define RTL_ACTION_START	1
#define RTL_ACTION_MASK		1
#define RTL_ACTION_DONE		0
#define RTL_CMD_FORCE		(1 << 3)
#define RTL_EN_STOP_TLU		(1 << 18)
#define RTL_STOP_TLU_READY	(1 << 19)
#define RTL_TABSTS_MASK		1
#define RTL_TABSTS_SUCCESS	0

/*
 * Группа, в которую сводим LAN: порты 1..4 и порт процессора 8.
 *
 * Номер 9 взят у заводской прошивки, а не выдуман: она держит LAN
 * именно в девятой, а первую отдаёт под WAN. Считано с работающего
 * стока 30.08 через /proc/rtl865x/memory:
 *
 *     bb060120: 00023D1E    строка 9, порты 1-4 и 8
 *     bb060020: 00000201    строка 1, только порт 0
 *     bb804a08: 00090001    PVCR0: порт 0 -> 1, порт 1 -> 9
 *
 * Число 0x23D1E совпало с тем, что даёт наша раскладка битов для
 * маски 0x11E. Значит раскладка прочитана верно.
 *
 * RTL_NASH_VID / RTL_WAN_VID объявлены в начале файла: они нужны и приёмнику.
 */
#define RTL_LAN_PORTY		0x11F

/*
 * v6.5: куда отправлять кадр - маска ГНЁЗД RTL8367.
 *
 * С включённым CPU-тегом (P0GMIICR бит 26) поле dp передающего
 * дескриптора 8197F превращается в маску портов ВНЕШНЕГО свитча: железо
 * кладёт её в 4-байтовый тег, и 8367 отдаёт кадр ровно в эти гнёзда, не
 * глядя на свои VLAN. Улики: сток печатал eth1 = 0x01 (WAN, порт 0),
 * eth0 = 0x08 (порт 3), eth2 = 0x10 (порт 4) - это номера портов 8367;
 * донор iptime A2004MU пишет в opts4 маску из DSA-тега 8367.
 *
 * До v6.5 маска была одна на всё - 0x1F, порты 0..4. Кадр из LAN уходил и
 * в синий WAN, а широковещательный кадр из домашней сети, пришедший в LAN1 и
 * переброшенный мостом в eth0.8, возвращался обратно в LAN1. Это и была
 * петля 13.10 15:36 - 14.10 08:39 («received packet with own address» на
 * вышестоящем мосту, DHCP не доходил до клиентов).
 *
 * Теперь маска по метке 802.1Q в самом кадре (у устройства нет
 * NETIF_F_HW_VLAN_CTAG_TX, поэтому метку стек кладёт прямо в данные):
 *   VID 8 (eth0.8, WAN)  -> только порт 0
 *   VID 9 (eth0.9, LAN)  -> порты 3 и 4
 *   без метки и прочее   -> порты 3 и 4: в синий WAN не уходит НИЧЕГО,
 *                           что не отправлено явно через eth0.8.
 */
/*
 * v6.6: гипотеза выше НЕВЕРНА. dp - порты ВНУТРЕННЕГО свитча 8197F (0x18 =
 * внутр. 3,4 - никуда не разведены, LAN был мёртв). Пока тег на передаче
 * выключен (P0GMIICR 02037D57), обе маски = 0x01: внутр. порт 0 (RGMII к
 * 8367), дальше решает VLAN-таблица 8367 по метке 802.1Q кадра.
 */
#define RTL_8367_WAN_GNEZDA	0x01
#define RTL_8367_LAN_GNEZDA	0x01

/*
 * Маски можно сменить на лету, без перепрошивки (проверка гипотезы в ОЗУ
 * и мгновенный откат):
 *   echo "1f 1f" > /proc/rtl819x_gnezda   - как в v6.4 (всё во все порты)
 *   echo "01 18" > /proc/rtl819x_gnezda   - v6.5 (по умолчанию)
 *   cat /proc/rtl819x_gnezda
 */
static u32 rtl819x_gnezda_wan = RTL_8367_WAN_GNEZDA;
static u32 rtl819x_gnezda_lan = RTL_8367_LAN_GNEZDA;

/*
 * Режим «роутер» (router mode), как у стока и у донора DIR-842
 * (ADCDS/openwrt-dlink-dir842-r1, SWITCH-AND-DATAPATH.md §3):
 *   - внутренний свитч 8197F видит гнёзда 8367 своими портами 0..4
 *     (MACCR1.PORT0_ROUTER_MODE + CPU-тег в обе стороны, P0GMIICR биты 25/26);
 *   - метка 802.1Q в ПРОВОД не идёт: VID едет в дескрипторе (opts3),
 *     а выход VLAN без метки (untag), как у стока;
 *   - на приёме VID ставим сами по гнезду из дескриптора (spa):
 *     гнездо 0 = WAN (VID 8), остальные = LAN (VID 9).
 * Для ядра ничего не меняется: eth0.8/eth0.9, мост и flowtable те же,
 * метку стек отдаёт и получает через hwaccel (NETIF_F_HW_VLAN_CTAG_*).
 *
 * Включается ТОЛЬКО на живом интерфейсе через /proc/rtl819x_router
 * (по умолчанию 0 = как было до 28.10.2026), чтобы откат был мгновенным:
 *   echo 1 > /proc/rtl819x_router   - включить
 *   echo 0 > /proc/rtl819x_router   - вернуть старый 802.1Q-транк
 */
/* куда слать в режиме роутера: внутр. порты 8197F = гнёзда 8367 */
#define RTL_ROUTER_WAN_DP	0x01	/* гнездо 0 */
#define RTL_ROUTER_LAN_DP	0x18	/* гнёзда 3 и 4 (LAN1, LAN2) */

/*
 * В каком гнезде LAN живёт MAC: учим по приёму (spa из дескриптора), чтобы
 * кадр клиенту шёл ТОЛЬКО в его гнездо. Без этого маска 0x18 рассылает
 * каждый кадр и в LAN1, и в LAN2, и всё упирается в самое медленное
 * гнездо: замер 28.10.2026 — 85 Мбит/с, порт 4 (приставка, 100 Мбит)
 * получил весь поток. Таблица маленькая, по хешу MAC; коллизия или
 * устаревшая запись дают лишь лишнюю рассылку в оба гнезда либо кадр в
 * старое гнездо до первого же кадра от клиента (тогда запись обновится).
 */
#define RTL_FDB_BITS		8
struct rtl819x_fdb {
	u8	mac[ETH_ALEN];
	u8	port;		/* 0 = пусто, иначе номер гнезда */
};
static struct rtl819x_fdb rtl819x_fdb[1 << RTL_FDB_BITS];

static unsigned int rtl819x_fdb_hash(const u8 *mac)
{
	return (mac[3] ^ mac[4] ^ (mac[5] << 1) ^ (mac[5] >> 7)) &
	       ((1 << RTL_FDB_BITS) - 1);
}

static void rtl819x_fdb_learn(const u8 *mac, unsigned int port)
{
	struct rtl819x_fdb *e = &rtl819x_fdb[rtl819x_fdb_hash(mac)];

	if (is_multicast_ether_addr(mac) || port == RTL_WAN_GNEZDO || port > 4)
		return;
	if (e->port == port && ether_addr_equal(e->mac, mac))
		return;
	ether_addr_copy(e->mac, mac);
	WRITE_ONCE(e->port, port);
}

static u32 rtl819x_fdb_dp(const u8 *mac, u32 dp_vse)
{
	struct rtl819x_fdb *e = &rtl819x_fdb[rtl819x_fdb_hash(mac)];
	unsigned int port = READ_ONCE(e->port);

	if (port && !is_multicast_ether_addr(mac) && ether_addr_equal(e->mac, mac))
		return BIT(port);
	return dp_vse;
}

/* VID кадра: из hwaccel-метки, иначе из метки в данных, иначе 0 */
static u16 rtl819x_tx_vid(const struct sk_buff *skb)
{
	if (skb_vlan_tag_present(skb))
		return skb_vlan_tag_get_id(skb);
	if (skb->len >= VLAN_ETH_HLEN &&
	    skb->data[12] == 0x81 && skb->data[13] == 0x00)
		return ((skb->data[14] & 0x0f) << 8) | skb->data[15];
	return 0;
}

static u32 rtl819x_tx_gnezda(const struct sk_buff *skb)
{
	u16 vid = rtl819x_tx_vid(skb);

	if (vid == RTL_WAN_VID) {
		rtl819x_tx_wan++;
		return READ_ONCE(rtl819x_router) ? RTL_ROUTER_WAN_DP :
		       READ_ONCE(rtl819x_gnezda_wan);
	}
	if (vid)
		rtl819x_tx_lan++;
	else
		rtl819x_tx_bez_metki++;
	if (!READ_ONCE(rtl819x_router))
		return READ_ONCE(rtl819x_gnezda_lan);
	/* адрес получателя — первые 6 байт кадра (метки в данных уже нет) */
	return rtl819x_fdb_dp(skb->data, RTL_ROUTER_LAN_DP);
}

static int rtl819x_gnezda_pokaz(struct seq_file *m, void *v)
{
	seq_printf(m, "wan %02x lan %02x\n", READ_ONCE(rtl819x_gnezda_wan),
		   READ_ONCE(rtl819x_gnezda_lan));
	seq_printf(m, "tx: wan %lu, lan %lu, untagged %lu\n",
		   rtl819x_tx_wan, rtl819x_tx_lan, rtl819x_tx_bez_metki);
	return 0;
}

static int rtl819x_gnezda_open(struct inode *inode, struct file *file)
{
	return single_open(file, rtl819x_gnezda_pokaz, NULL);
}

static ssize_t rtl819x_gnezda_zapis(struct file *file, const char __user *buf,
				    size_t dlina, loff_t *poz)
{
	char tekst[32];
	unsigned int wan, lan;

	if (dlina >= sizeof(tekst))
		return -EINVAL;
	if (copy_from_user(tekst, buf, dlina))
		return -EFAULT;
	tekst[dlina] = 0;
	if (sscanf(tekst, "%x %x", &wan, &lan) != 2 ||
	    !wan || !lan || wan > 0x7f || lan > 0x7f)
		return -EINVAL;

	WRITE_ONCE(rtl819x_gnezda_wan, wan);
	WRITE_ONCE(rtl819x_gnezda_lan, lan);
	pr_info("rtl819x: tx port masks wan %02x lan %02x\n", wan, lan);
	return dlina;
}

static const struct proc_ops rtl819x_gnezda_ops = {
	.proc_open    = rtl819x_gnezda_open,
	.proc_read    = seq_read,
	.proc_lseek   = seq_lseek,
	.proc_release = single_release,
	.proc_write   = rtl819x_gnezda_zapis,
};

static inline u32 rtl_sw_r(u32 adres)
{
	return *(volatile u32 *)adres;
}

static inline void rtl_sw_w(u32 adres, u32 znach)
{
	*(volatile u32 *)adres = znach;
}

static int rtl819x_intsw_zhdat(u32 adres, u32 maska, u32 zhdem)
{
	int popytok = 100000;

	while ((rtl_sw_r(adres) & maska) != zhdem) {
		if (--popytok == 0)
			return -ETIMEDOUT;
		cpu_relax();
	}

	return 0;
}

/*
 * Запись одной строки таблицы коммутатора. Порядок действий вендорский:
 * остановить аппаратный поиск, разложить содержимое по TCR0..TCR7,
 * положить адрес строки в SWTAA, дать команду и дождаться её конца.
 */
static int rtl819x_intsw_zapis_vlan(u16 vid, u32 chleny, u32 untag, u32 fid)
{
	u32 slovo[8];
	unsigned int i;
	int ret;

	memset(slovo, 0, sizeof(slovo));

	/*
	 * Слово 0, раскладка из rtl865xc_tblAsic_vlanTable_t (ветка
	 * _LITTLE_ENDIAN, rtl865x_asicCom.h:211):
	 *     биты 0..5    memberPort      порты 0..5
	 *     биты 6..8    extMemberPort   порты 6..8
	 *     биты 9..14   egressUntag     порты 0..5
	 *     биты 15..17  extEgressUntag  порты 6..8
	 *     биты 18..19  fid
	 */
	slovo[0] = (chleny & 0x3F)
		 | (((chleny >> 6) & 0x7) << 6)
		 | ((untag & 0x3F) << 9)
		 | (((untag >> 6) & 0x7) << 15)
		 | ((fid & 0x3) << 18);

	rtl_sw_w(RTL_SWTCR0, RTL_EN_STOP_TLU | rtl_sw_r(RTL_SWTCR0));
	ret = rtl819x_intsw_zhdat(RTL_SWTCR0, RTL_STOP_TLU_READY,
				  RTL_STOP_TLU_READY);
	if (ret) {
		pr_err("rtl819x-sw: table lookup did not stop\n");
		return ret;
	}

	for (i = 0; i < 8; i++)
		rtl_sw_w(RTL_TCR(i), slovo[i]);

	rtl_sw_w(RTL_SWTAA, RTL_SWTBL_BASE + (RTL_TYPE_VLAN_TABLE << 16) +
			    vid * RTL_TBL_ENTRY_LEN);
	rtl_sw_w(RTL_SWTACR, RTL_ACTION_START | RTL_CMD_FORCE);

	ret = rtl819x_intsw_zhdat(RTL_SWTACR, RTL_ACTION_MASK,
				  RTL_ACTION_DONE);

	rtl_sw_w(RTL_SWTCR0, ~RTL_EN_STOP_TLU & rtl_sw_r(RTL_SWTCR0));

	if (ret) {
		pr_err("rtl819x-sw: VLAN %u table write timed out\n", vid);
		return ret;
	}

	return 0;
}

/*
 * Строка таблицы L2 (MAC -> порты) внутреннего свитча. Формат и хеш строки —
 * донор DIR-842 (rtl865x_asichal.c: gw_l2_row, gw_write_l2_full), значения
 * сверены донором со стоком 3.10 (/proc/rtl865x/l2):
 *   строка = (MAC[0]^..^MAC[5]^fh[fid]) & 0xFF, fh = {00, 0f, f0, ff};
 *   индекс в таблице = строка << 2 (столбец 0 из четырёх);
 *   слово 0 = MAC[1..4], слово 1 = MAC[0] | member<<8 | extmember<<14 |
 *             toCPU<<17 | STA(1<<18) | age 3<<19 | NH(1<<22) | fid<<23 | auth<<25.
 * na_cpu: member 0, extmember 0x4 (порт процессора 8), toCPU 1, auth 1.
 * Пишется тем же окном таблиц, что и VLAN (тип 0 = L2_SWITCH).
 */
#define RTL_TYPE_L2_TABLE	0

static int rtl819x_intsw_zapis_l2(const u8 *mac, u32 fid, bool na_cpu)
{
	static const u8 fh[4] = { 0x00, 0x0f, 0xf0, 0xff };
	u32 slovo[8], stroka;
	unsigned int i;
	int ret;

	if (!na_cpu)
		return -EINVAL;

	stroka = (mac[0] ^ mac[1] ^ mac[2] ^ mac[3] ^ mac[4] ^ mac[5] ^
		  fh[fid & 3]) & 0xFF;

	memset(slovo, 0, sizeof(slovo));
	slovo[0] = (mac[1] << 24) | (mac[2] << 16) | (mac[3] << 8) | mac[4];
	slovo[1] = mac[0] | (0u << 8) | (0x4u << 14) | (1u << 17) |
		   (1u << 18) | (3u << 19) | (1u << 22) | ((fid & 3) << 23) |
		   (1u << 25);

	rtl_sw_w(RTL_SWTCR0, RTL_EN_STOP_TLU | rtl_sw_r(RTL_SWTCR0));
	ret = rtl819x_intsw_zhdat(RTL_SWTCR0, RTL_STOP_TLU_READY,
				  RTL_STOP_TLU_READY);
	if (ret)
		return ret;

	for (i = 0; i < 8; i++)
		rtl_sw_w(RTL_TCR(i), slovo[i]);
	rtl_sw_w(RTL_SWTAA, RTL_SWTBL_BASE + (RTL_TYPE_L2_TABLE << 16) +
			    (stroka << 2) * RTL_TBL_ENTRY_LEN);
	rtl_sw_w(RTL_SWTACR, RTL_ACTION_START | RTL_CMD_FORCE);
	ret = rtl819x_intsw_zhdat(RTL_SWTACR, RTL_ACTION_MASK, RTL_ACTION_DONE);

	rtl_sw_w(RTL_SWTCR0, ~RTL_EN_STOP_TLU & rtl_sw_r(RTL_SWTCR0));

	pr_info("rtl819x-sw: L2 row %u (idx %u) fid %u <- %08x %08x, ret %d\n",
		stroka, stroka << 2, fid, slovo[0], slovo[1], ret);
	return ret;
}

/* PVID порта: по два порта на регистр, чётный в младшей половине */
static void rtl819x_intsw_pvid(unsigned int port, u16 vid)
{
	u32 adres = RTL_PVCR(port >> 1);
	unsigned int sdvig = (port & 1) ? 16 : 0;
	u32 znach = rtl_sw_r(adres);

	znach &= ~(0xFFFu << sdvig);
	znach |= ((u32)vid & 0xFFF) << sdvig;
	rtl_sw_w(adres, znach);
}

/*
 * Включить / выключить режим роутера на живом интерфейсе.
 *
 * Значения сняты со СТОКА под нагрузкой 28.10.2026 (regs-diff-stock-vs-ours):
 *   MACCR1   (BB804058) = 1          PORT0_ROUTER_MODE
 *   P0GMIICR (BB80414C) = 06037D57   + бит 26: CPU-тег и на передачу
 *   PVCR0    (BB804A08) p0 = 8       кадр без метки из гнезда WAN -> группа 8
 *   VLAN 9 = порты 1-4 + 8 (0x11E), без порта 0 (иначе WAN в LAN-группе)
 * Выключение возвращает ровно то, что пишет rtl819x_intsw_nastroit().
 * Таблицу MAC внешнего 8367 после смены сбрасывает пользователь
 * (debugfs 0x0a36 = 0x00ff), как после любой перекоммутации стенда.
 */
static int rtl819x_intsw_zapis_vlan(u16 vid, u32 chleny, u32 untag, u32 fid);
static void rtl819x_intsw_pvid(unsigned int port, u16 vid);
static int rtl819x_intsw_zapis_l2(const u8 *mac, u32 fid, bool na_cpu);

static struct net_device *rtl819x_dev;	/* для MAC роутера в router_set */

static void rtl819x_router_set(bool on)
{
	int ret;

	/*
	 * MAC самого роутера -> только процессор, в обеих группах (FID 0 = LAN,
	 * FID 1 = WAN). Без этой строки в router mode кадр «роутеру» для свитча
	 * — неизвестный адрес, и он разливает его по группе: снимок HSA
	 * 29.10.2026 00:35 — dp 41 (порт 0 + CPU) вместо dp 40, весь поток
	 * LAN->WAN уходил копией обратно в 8367 и в LAN2 (100 Мбит),
	 * отсюда потолок ~94 Мбит/с. У стока такая строка есть
	 * (/proc/rtl865x/l2: «router MAC FID:0 mbr(8)»), у донора DIR-842 —
	 * gw_write_l2_full(CPU-MAC, 0, ext 0x4, toCPU 1, fid, auth 1).
	 */
	if (on && rtl819x_dev) {
		ret = rtl819x_intsw_zapis_l2(rtl819x_dev->dev_addr, 0, true);
		ret |= rtl819x_intsw_zapis_l2(rtl819x_dev->dev_addr, 1, true);
		pr_info("rtl819x: L2 %pM -> CPU (fid 0, 1): %d\n",
			rtl819x_dev->dev_addr, ret);
	}

	if (on) {
		ret = rtl819x_intsw_zapis_vlan(RTL_NASH_VID, RTL_LAN_PORTY & ~1u,
					       RTL_LAN_PORTY & ~1u, 0);
		rtl819x_intsw_pvid(0, RTL_WAN_VID);
		WRITE_ONCE(rtl819x_router, true);
		/* порядок как у вендорского init_8197f_p0(): тег, затем режим */
		rtl_sw_w(0xBB80414C, 0x06037D57);
		rtl_sw_w(0xBB804058, rtl_sw_r(0xBB804058) | 1);
	} else {
		rtl_sw_w(0xBB804058, rtl_sw_r(0xBB804058) & ~1u);
		rtl_sw_w(0xBB80414C, 0x02037D57);
		WRITE_ONCE(rtl819x_router, false);
		rtl819x_intsw_pvid(0, RTL_NASH_VID);
		ret = rtl819x_intsw_zapis_vlan(RTL_NASH_VID, RTL_LAN_PORTY,
					       RTL_LAN_PORTY, 0);
	}
	pr_info("rtl819x: router mode %s (vlan %d) MACCR1=%08x P0GMIICR=%08x PVCR0=%08x\n",
		on ? "ON" : "off", ret, rtl_sw_r(0xBB804058),
		rtl_sw_r(0xBB80414C), rtl_sw_r(RTL_PVCR(0)));
}

static int rtl819x_router_pokaz(struct seq_file *m, void *v)
{
	unsigned int i;

	seq_printf(m, "%d\n", READ_ONCE(rtl819x_router) ? 1 : 0);
	for (i = 0; i < ARRAY_SIZE(rtl819x_fdb); i++)
		if (rtl819x_fdb[i].port)
			seq_printf(m, "fdb %pM -> gnezdo %u\n",
				   rtl819x_fdb[i].mac, rtl819x_fdb[i].port);
	return 0;
}

static int rtl819x_router_open(struct inode *inode, struct file *file)
{
	return single_open(file, rtl819x_router_pokaz, NULL);
}

static ssize_t rtl819x_router_zapis(struct file *file, const char __user *buf,
				    size_t dlina, loff_t *poz)
{
	char c;

	if (!dlina || get_user(c, buf))
		return -EFAULT;
	if (c != '0' && c != '1')
		return -EINVAL;
	rtl819x_router_set(c == '1');
	return dlina;
}

static const struct proc_ops rtl819x_router_ops = {
	.proc_open    = rtl819x_router_open,
	.proc_read    = seq_read,
	.proc_lseek   = seq_lseek,
	.proc_release = single_release,
	.proc_write   = rtl819x_router_zapis,
};

/* ---- «уши» свитча: что он решил по пакету (HSB/HSA) ---------------------
 *
 * Перед каждым пакетом свитч разбирает заголовок в HSB (Header Stamp
 * Before, 0xBB806280) и кладёт своё решение в HSA (Header Stamp After,
 * 0xBB806200): куда отправить, сделал ли сам (hwfwrd) или отдал процессору
 * и почему (why2cpu). Расшифровка — из вендорского SDK
 * (AsicDriver/rtl865xC_hs.h, ветка CONFIG_RTL_8197F, _LITTLE_ENDIAN),
 * порядок чтения и ожидание HSABUSY (TMCR 0xBB806300 бит 2) — как у донора
 * iptime A2004MU (hswatch_store).
 *
 *   echo "c0a80032 c0a80101" > /proc/rtl819x_hs   ждать пакет sip -> dip (до 10 с)
 *   echo "0 0" > /proc/rtl819x_hs                 любой IPv4-пакет
 *   cat /proc/rtl819x_hs                          что поймали
 * Только чтение регистров, свитч не трогаем.
 */
#define RTL_HSA_BASE		0xBB806200
#define RTL_HSB_BASE		0xBB806280
#define RTL_TMCR		0xBB806300
#define RTL_TMCR_HSABUSY	BIT(2)
#define RTL_HS_SLOV		23

static u32 rtl819x_hsb[RTL_HS_SLOV], rtl819x_hsa[RTL_HS_SLOV];
static u32 rtl819x_hs_sip, rtl819x_hs_dip;
static int rtl819x_hs_sost;	/* 0 нет снимка, 1 пойман, 2 не дождались */
static DEFINE_MUTEX(rtl819x_hs_lock);

static u32 rtl819x_hsb_sip(const u32 *b)
{
	return (b[1] >> 2) | ((b[2] & 3) << 30);
}

static u32 rtl819x_hsb_dip(const u32 *b)
{
	return (b[2] >> 18) | ((b[3] & 0x3ffff) << 14);
}

static int rtl819x_hs_pokaz(struct seq_file *m, void *v)
{
	static const char * const tip[8] = { "eth", "pptp", "ipv4", "icmp",
					     "igmp", "tcp", "udp", "ipv6" };
	const u32 *b = rtl819x_hsb, *a = rtl819x_hsa;
	u32 why;
	int i;

	mutex_lock(&rtl819x_hs_lock);
	seq_printf(m, "sost %d (0 net, 1 poiman, 2 ne dozhdalis) filtr %pI4h -> %pI4h\n",
		   rtl819x_hs_sost, &rtl819x_hs_sip, &rtl819x_hs_dip);
	if (rtl819x_hs_sost == 1) {
		u32 sip = rtl819x_hsb_sip(b), dip = rtl819x_hsb_dip(b);

		why = (a[6] >> 18) | ((a[7] & 3) << 14);
		/* W3[31:18] + W4[1:0] = dprt, W4[31:29] = type, W8[31:30] = extspa,
		 * W8[24] = dirtx (кадр от процессора напрямую, без разбора) */
		seq_printf(m, "HSB: spa %u extspa %u dirtx %u vid %u tag %u pppoe %u len %u  %pI4h:%u -> %pI4h:%u  tip %s\n",
			   b[0] & 7, b[8] >> 30, (b[8] >> 24) & 1, b[0] >> 20,
			   b[1] & 1, (b[1] >> 1) & 1, (b[0] >> 5) & 0x3fff,
			   &sip, (b[2] >> 2) & 0xffff, &dip,
			   (b[3] >> 18) | ((b[4] & 3) << 14),
			   tip[b[4] >> 29]);
		seq_printf(m, "HSA: hwfwrd %u  dp %02x dpext %x dpc %u  dvid %u svid %u  l2tr %u l34tr %u egif %u  spao %u  why2cpu %04x\n",
			   (a[6] >> 8) & 1, a[8] & 0x7f, (a[6] >> 12) & 0xf,
			   (a[6] >> 1) & 7, (a[4] >> 9) & 0xfff,
			   (a[5] >> 14) & 0xfff, (a[4] >> 1) & 1, (a[4] >> 2) & 1,
			   a[4] & 1, (a[6] >> 4) & 7, why);
		seq_printf(m, "     why2cpu: [15] %u DDDDD %u SSSSS %u prichina %u acl-zerk %u\n",
			   why >> 15, (why >> 10) & 0x1f, (why >> 5) & 0x1f,
			   (why >> 1) & 0xf, why & 1);
		seq_puts(m, "HSB:");
		for (i = 0; i < RTL_HS_SLOV; i++)
			seq_printf(m, " %08x", b[i]);
		seq_puts(m, "\nHSA:");
		for (i = 0; i < RTL_HS_SLOV; i++)
			seq_printf(m, " %08x", a[i]);
		seq_putc(m, '\n');
	}
	mutex_unlock(&rtl819x_hs_lock);
	return 0;
}

static int rtl819x_hs_open(struct inode *inode, struct file *file)
{
	return single_open(file, rtl819x_hs_pokaz, NULL);
}

static ssize_t rtl819x_hs_zapis(struct file *file, const char __user *buf,
				size_t dlina, loff_t *poz)
{
	unsigned long srok = jiffies + 10 * HZ, flags;
	unsigned long krug = 0;
	char tekst[40];
	u32 b[4], b8;
	int i;

	if (dlina >= sizeof(tekst))
		return -EINVAL;
	if (copy_from_user(tekst, buf, dlina))
		return -EFAULT;
	tekst[dlina] = 0;

	mutex_lock(&rtl819x_hs_lock);
	if (sscanf(tekst, "%x %x", &rtl819x_hs_sip, &rtl819x_hs_dip) != 2) {
		mutex_unlock(&rtl819x_hs_lock);
		return -EINVAL;
	}
	rtl819x_hs_sost = 2;
	memset(rtl819x_hsb, 0, sizeof(rtl819x_hsb));
	memset(rtl819x_hsa, 0, sizeof(rtl819x_hsa));

	do {
		for (i = 0; i < 4; i++)
			b[i] = rtl_sw_r(RTL_HSB_BASE + i * 4);
		b8 = rtl_sw_r(RTL_HSB_BASE + 8 * 4);
		/*
		 * W8 бит 24 = dirtx: кадр от процессора напрямую (свитч его не
		 * разбирает). Нас интересуют кадры, которые свитч решал сам,
		 * поэтому их пропускаем — как донор iptime.
		 */
		if (!(b8 & BIT(24)) &&
		    ((rtl819x_hs_sip || rtl819x_hs_dip) ?
		     (rtl819x_hsb_sip(b) == rtl819x_hs_sip &&
		      rtl819x_hsb_dip(b) == rtl819x_hs_dip) :
		     (b[1] || b[2] || b[3]))) {
			local_irq_save(flags);
			for (i = 0; i < 100 &&
			     (rtl_sw_r(RTL_TMCR) & RTL_TMCR_HSABUSY); i++)
				cpu_relax();
			for (i = 0; i < RTL_HS_SLOV; i++) {
				rtl819x_hsb[i] = i < 4 ? b[i] : i == 8 ? b8 :
					rtl_sw_r(RTL_HSB_BASE + i * 4);
				rtl819x_hsa[i] = rtl_sw_r(RTL_HSA_BASE + i * 4);
			}
			local_irq_restore(flags);
			rtl819x_hs_sost = 1;
			break;
		}
		cpu_relax();
		if (!(++krug & 0xfff))
			cond_resched();
	} while (time_before(jiffies, srok));
	mutex_unlock(&rtl819x_hs_lock);
	return dlina;
}

static const struct proc_ops rtl819x_hs_ops = {
	.proc_open    = rtl819x_hs_open,
	.proc_read    = seq_read,
	.proc_lseek   = seq_lseek,
	.proc_release = single_release,
	.proc_write   = rtl819x_hs_zapis,
};

static void rtl819x_intsw_nastroit(void)
{
	unsigned int port;
	int ret;

	pr_info("rtl819x-sw: before PCRP0=%08x PCRP1=%08x PCRP3=%08x\n",
		rtl_sw_r(RTL_PCRP(0)), rtl_sw_r(RTL_PCRP(1)),
		rtl_sw_r(RTL_PCRP(3)));
	pr_info("rtl819x-sw: before PSRP0=%08x PSRP1=%08x PSRP3=%08x PSRP4=%08x\n",
		rtl_sw_r(RTL_PSRP(0)), rtl_sw_r(RTL_PSRP(1)),
		rtl_sw_r(RTL_PSRP(3)), rtl_sw_r(RTL_PSRP(4)));

	/*
	 * Регистры, которые загрузчик задаёт, а после сброса они пустые.
	 *
	 * Найдены сличением ДВУХ ЖИВЫХ СНИМКОВ всего блока 0xBB804000..0xBB804F00:
	 * один снят из загрузчика, где сеть работает, другой из нашего ядра.
	 * Из 960 значений разошлись 16, из них пять - наши собственные группы
	 * портов и два - след последней операции с таблицей. Остались эти девять.
	 *
	 * В младших разрядах у загрузчика стоят маски портов - FFFF, 7F, 57 -
	 * а у нас нули. Маска портов, равная нулю, означает "пересылать некуда":
	 * ровно то, что мы и наблюдали, кадры не шли ни в одну сторону.
	 *
	 * Пишем до настройки групп, чтобы наша настройка была последней.
	 */
	rtl_sw_w(0xBB804004, 0x8500FFFF);
	rtl_sw_w(0xBB804100, 0x00000001);
	/*
	 * Биты 25/26 - приём и передача CPU-тега внешнего RTL8367: 8197F
	 * снимает тег и кладёт номер гнезда в дескриптор, как у стока и у
	 * донора iptime. Проба в ОЗУ 13.10.2026: связь есть, без снятия - нет.
	 * У загрузчика 0x00037D57 (тег выключен).
	 *
	 * v6.6: бит 26 (ТЕГ НА ПЕРЕДАЧУ) ВЫКЛЮЧЕН, остаётся
	 * только бит 25 (снимать тег на приёме) -> 0x02037D57.
	 * С битом 26 маска dp передающего дескриптора работает дважды: внутр.
	 * свитч 8197F выбирает по ней свой порт (к 8367 ведёт только порт 0),
	 * а 8367 получает ту же маску в теге и шлёт кадр в СВОИ порты, мимо
	 * VLAN-таблицы. Бит 0 = и «провод к 8367», и «синий WAN» - отправить
	 * только в LAN нельзя: 1F = петля утра 14.10, 18 = LAN мёртв (v6.5),
	 * 01 = кадр VLAN 9 в WAN (проба 14.10 17:08). Вендор ставит бит 26
	 * только вместе с MACCR1.PORT0_ROUTER_MODE, у нас он не заводится.
	 * Без тега на передаче кадр приходит в 8367 обычным 802.1Q (метку
	 * кладут eth0.8/eth0.9), и 8367 раскладывает его по VLAN: 9 -> 3,4,
	 * 8 -> 0. Проверить/откатить на живой плате без перепрошивки:
	 *   devmem 0xbb80414c 32 0x06037d57  - как v6.5
	 *   devmem 0xbb80414c 32 0x02037d57  - v6.6
	 */
	rtl_sw_w(0xBB80414C, 0x02037D57);
	rtl_sw_w(0xBB804204, 0x00000001);
	rtl_sw_w(0xBB804234, 0x03007F7F);
	rtl_sw_w(0xBB804300, 0x00200000);
	rtl_sw_w(0xBB804428, 0x00000003);
	rtl_sw_w(0xBB804754, 0x00001249);

	/*
	 * Настройка портов внутреннего коммутатора.
	 *
	 * Значения СНЯТЫ С ЖИВОГО ЗАГРУЗЧИКА, где сеть работает, а не выведены
	 * из описания, которого у нас нет. Читались по адресам 0xBB804104 и
	 * далее сразу после того, как загрузчик поднял сеть сам.
	 *
	 * Порт 0 выбивается из общего ряда, и не случайно: через него идёт
	 * RGMII на внешний RTL8367D, там нет приёмопередатчика, согласовывать
	 * скорость не с кем - её задают принудительно, гигабит MAC-к-MAC.
	 * Порты 1..4 идут ровным шагом 0x04000000 и отличаются от нашего
	 * состояния после сброса только младшим разрядом.
	 *
	 * Признак, по которому видно, что связь между коммутаторами встала:
	 * PSRP0 становится 0x307A вместо 0x30E0, как у портов без пары.
	 */
	rtl_sw_w(RTL_PCRP(0), 0x16972039);
	rtl_sw_w(RTL_PCRP(1), 0x447F0039);
	rtl_sw_w(RTL_PCRP(2), 0x487F0039);
	rtl_sw_w(RTL_PCRP(3), 0x4C7F0039);
	rtl_sw_w(RTL_PCRP(4), 0x507F0039);

	/*
	 * Внутренние порты, включая восьмой - порт процессора. Их я в первый
	 * раз пропустил, и зря: кадры не ходили ни в одну сторону именно на
	 * этом рубеже. Значения оттуда же, с живого загрузчика.
	 */
	rtl_sw_w(RTL_PCRP(6), 0x187F0038);
	rtl_sw_w(RTL_PCRP(7), 0x1C7F0038);
	rtl_sw_w(RTL_PCRP(8), 0x207F0038);

	pr_info("rtl819x-sw: after  PCRP0=%08x PCRP1=%08x PCRP3=%08x\n",
		rtl_sw_r(RTL_PCRP(0)), rtl_sw_r(RTL_PCRP(1)),
		rtl_sw_r(RTL_PCRP(3)));
	pr_info("rtl819x-sw: after  PSRP0=%08x PSRP1=%08x PSRP3=%08x\n",
		rtl_sw_r(RTL_PSRP(0)), rtl_sw_r(RTL_PSRP(1)),
		rtl_sw_r(RTL_PSRP(3)));

	ret = rtl819x_intsw_zapis_vlan(RTL_NASH_VID, RTL_LAN_PORTY,
				       RTL_LAN_PORTY, 0);
	if (ret)
		return;

	/*
	 * Номер группы по умолчанию для КАЖДОГО порта, включая нулевой.
	 * Без этого кадры, приходящие от 8367D в порт 0, получали чужой
	 * номер 8, оставшийся от загрузчика, и до процессора не доходили:
	 * группы 8 в таблице нет вовсе.
	 */
	for (port = 0; port <= 4; port++)
		rtl819x_intsw_pvid(port, RTL_NASH_VID);
	rtl819x_intsw_pvid(8, RTL_NASH_VID);

	pr_info("rtl819x-sw: VLAN %u = ports 1-4 and 8, mask %03x\n",
		RTL_NASH_VID, RTL_LAN_PORTY);

	/*
	 * Группа 8 - WAN (v6.4). Внешний RTL8367D метит кадры
	 * синего гнезда номером 8 (LAN - номером 9) и отдаёт их сюда, в порт 0.
	 * Без этой строки таблицы внутренний коммутатор кадры с меткой 8
	 * выбрасывает: проверено в ОЗУ, eth0.8 молчал, пока группы не было.
	 * Члены - порт 0 и процессор 8, FID 1, чтобы MAC из WAN не путались с LAN.
	 */
	ret = rtl819x_intsw_zapis_vlan(RTL_WAN_VID, 0x101, 0x101, 1);
	pr_info("rtl819x-sw: VLAN %u (WAN) = port 0 and 8, FID 1, result %d\n",
		RTL_WAN_VID, ret);
	pr_info("rtl819x-sw: PVCR0=%08x PVCR1=%08x PVCR2=%08x PVCR4=%08x\n",
		rtl_sw_r(RTL_PVCR(0)), rtl_sw_r(RTL_PVCR(1)),
		rtl_sw_r(RTL_PVCR(2)), rtl_sw_r(RTL_PVCR(4)));

	/*
	 * Этот регистр пишем ПОСЛЕДНИМ, и это выяснено опытом.
	 *
	 * Записанный вместе с остальными, он к концу настройки снова
	 * читался нулём: его сбрасывает что-то из наших дальнейших
	 * действий - соседний регистр или работа с таблицей. У загрузчика
	 * в нём маска портов FFFF, у нас был ноль.
	 */
	rtl_sw_w(0xBB804008, 0x4000FFFF);

	/*
	 * Снимок всего блока регистров коммутатора - для сличения с тем, что
	 * читается из загрузчика, где сеть работает. Разница между двумя
	 * снимками и есть то, чего мы не делаем.
	 */
	{
		unsigned int a;

		for (a = 0xBB804000; a < 0xBB804F00; a += 32)
			pr_info("SWREG %08x: %08x %08x %08x %08x %08x %08x %08x %08x\n",
				a,
				rtl_sw_r(a +  0), rtl_sw_r(a +  4),
				rtl_sw_r(a +  8), rtl_sw_r(a + 12),
				rtl_sw_r(a + 16), rtl_sw_r(a + 20),
				rtl_sw_r(a + 24), rtl_sw_r(a + 28));
	}




}

static netdev_tx_t rtl819x_start_xmit(struct sk_buff *skb,
				      struct net_device *dev)
{
	struct rtl819x_eth *eth = netdev_priv(dev);
	struct rtl819x_ring *ring = &eth->tx[0];
	struct rtl819x_tx_desc *desc;
	unsigned int next, len, hwlen;
	dma_addr_t dma;
	unsigned long flags;
	u16 vid;

	spin_lock_irqsave(&eth->lock, flags);

	/*
	 * Отправленное освобождает NAPI (rtl819x_poll). Здесь — только если
	 * кольцо забито: вдруг железо уже прошло дальше, а NAPI ещё не успел.
	 */
	next = (ring->head + 1) % ring->size;
	if (next == ring->tail)
		rtl819x_tx_reclaim(eth, 0, 0);
	if (next == ring->tail) {
		/* кольцо забито — просим стек придержать поток */
		netif_stop_queue(dev);
		spin_unlock_irqrestore(&eth->lock, flags);
		return NETDEV_TX_BUSY;
	}

	/*
	 * Метку 802.1Q стек отдаёт отдельно (NETIF_F_HW_VLAN_CTAG_TX). В режиме
	 * роутера она едет в дескрипторе, в провод не идёт. В старом режиме
	 * (802.1Q-транк к 8367) кладём её в данные, как было раньше.
	 */
	vid = rtl819x_tx_vid(skb);
	if (skb_vlan_tag_present(skb) && !READ_ONCE(rtl819x_router)) {
		skb = __vlan_hwaccel_push_inside(skb);
		if (!skb) {
			spin_unlock_irqrestore(&eth->lock, flags);
			dev->stats.tx_dropped++;
			return NETDEV_TX_OK;
		}
	}

	/*
	 * Короткие кадры дополняем нулями по-настоящему. Увеличить одну
	 * лишь переменную длины мало: контроллер читает из буфера ровно
	 * столько, сколько ему объявлено, и за концом skb подхватил бы
	 * чужую память.
	 */
	if (skb_padto(skb, ETH_ZLEN)) {
		/* skb_padto освободил skb сам */
		spin_unlock_irqrestore(&eth->lock, flags);
		dev->stats.tx_dropped++;
		return NETDEV_TX_OK;
	}

	len = max_t(unsigned int, skb->len, ETH_ZLEN);

	/*
	 * В дескрипторе длина указывается вместе с контрольной суммой,
	 * которую допишет само железо (rtl819x_swNic.c:764-768).
	 */
	hwlen = len + ETH_FCS_LEN;

	dma = dma_map_single(&eth->pdev->dev, skb->data, len, DMA_TO_DEVICE);
	if (dma_mapping_error(&eth->pdev->dev, dma)) {
		spin_unlock_irqrestore(&eth->lock, flags);
		dev_kfree_skb_any(skb);
		dev->stats.tx_dropped++;
		return NETDEV_TX_OK;
	}

	desc = (struct rtl819x_tx_desc *)ring->desc + ring->head;

	/* дескриптор мог остаться заполненным с прошлой отправки */
	memset(desc, 0, sizeof(*desc));

	desc->addr = cpu_to_le32(dma);

	/*
	 * Длина кадра у передающего дескриптора лежит в поле mlen, то есть
	 * начиная с бита 15. В младших битах opts2 у передачи находятся
	 * vlantag и номера очередей — записанная туда длина превращает
	 * кадр в мусор с нулевым размером, и свитч молча его выбрасывает.
	 */
	desc->opts2 = cpu_to_le32((hwlen << RTL819X_TX_MLEN_SHIFT) &
				  RTL819X_TX_MLEN_MASK);
	/*
	 * Номер группы, в которую коммутатор должен положить кадр.
	 * Вендор задаёт его всегда (tx_dvlanid, rtl819x_swNic.c:781);
	 * без него кадр не принадлежит ни одной группе и пропадает.
	 */
	/*
	 * В режиме роутера — настоящая группа кадра (8 = WAN, 9 = LAN): по ней
	 * свитч снимает метку на выходе (группы без метки, как у стока).
	 */
	desc->opts3 = cpu_to_le32((READ_ONCE(rtl819x_router) && vid ? vid :
				   RTL_NASH_VID) & 0xFFF);

	/*
	 * Порт назначения указываем явно. Полагаться на hwlkup нельзя:
	 * он велит свитчу найти порт по таблице MAC-адресов, а таблица
	 * пуста — заполнять её некому, пока свитч не настроен. Маска всех
	 * физических портов заставляет разослать кадр наружу, как поступает
	 * мост с неизвестным адресом.
	 */
	/*
	 * Аппаратный поиск по таблице MAC (hwlkup) СНЯТ 30.08.
	 *
	 * С ним групповой обмен ходил в обе стороны — соседи по IPv6
	 * доходили до состояния REACHABLE, — а широковещательный не
	 * ходил вовсе: клиент за все опыты не принял ни одного
	 * широковещательного кадра, и ARP не работал ни в одну
	 * сторону, хотя клиент честно спрашивал свободный адрес.
	 *
	 * Похоже, для адреса ff:ff:ff:ff:ff:ff поиск ничего не
	 * находит, а в FFCR загрузчика выставлен бит EnUnkMC2CPU:
	 * неизвестные ГРУППОВЫЕ адреса отдавать процессору. Широко-
	 * вещательный адрес формально групповой, так что кадр вместо
	 * рассылки уходил обратно в процессор.
	 *
	 * Без hwlkup решать свитчу нечего: он обязан отправить кадр
	 * ровно в те порты, которые перечислены ниже.
	 */

	/*
	 * Порты назначения: 0..4 внутреннего коммутатора, маска 0x1F.
	 * Нулевой обязателен — через него идёт RGMII на внешний свитч
	 * RTL8367D, а гнёзда сидят на нём. Порты 1..4 никуда не
	 * разведены, но вреда от них нет.
	 *
	 * Теперь это ЕДИНСТВЕННОЕ указание коммутатору, куда слать:
	 * аппаратный поиск снят, запасного пути больше нет.
	 */
	/*
	 * v6.5: при CPU-теге это маска гнёзд RTL8367, а не внутренних
	 * портов - см. rtl819x_tx_gnezda(). Было 0x1F: в том числе в WAN.
	 */
	desc->opts4 = cpu_to_le32((rtl819x_tx_gnezda(skb) <<
				   RTL819X_TX_DP_SHIFT) & RTL819X_TX_DP_MASK);

	/* свитчу нужно знать, откуда кадр пришёл — из CPU-порта */
	desc->opts5 = cpu_to_le32(RTL819X_TX_EXTSPA_CPU <<
				  RTL819X_TX_EXTSPA_SHIFT);

	ring->skb[ring->head] = skb;

	/* адрес и длина должны быть видны раньше, чем железо увидит own */
	dma_wmb();
	desc->opts1 = cpu_to_le32(RTL819X_DESC_SWCORE_OWNED |
				  (ring->head == ring->size - 1 ?
				   RTL819X_DESC_WRAP : 0) |
				  RTL819X_TX_FS | RTL819X_TX_LS |
				  ((hwlen << RTL819X_TX_PHLEN_SHIFT) &
				   RTL819X_TX_PHLEN_MASK));

	ring->head = next;

	/* сказать контроллеру, что появился новый дескриптор */
	rtl819x_m32(eth, 0, RTL819X_TXFD, RTL819X_CPUICR);

	/* ВРЕМЕННО (29.08): первые десять отправок. Если этих строк нет
	 * вовсе, кадры до драйвера не доходят и копать надо выше. */
	{
		static int _m;

		if (_m < 10) {
			_m++;
			pr_info("rtl819x-tx: frame %d, len=%u head=%u tail=%u icr=%08x\n",
				_m, hwlen, ring->head, ring->tail,
				rtl819x_r32(eth, RTL819X_CPUICR));
		}
	}

	spin_unlock_irqrestore(&eth->lock, flags);

	return NETDEV_TX_OK;
}

/* ---- жизненный цикл ---------------------------------------------------- */

/*
 * Внешний коммутатор RTL8367D настраивает его собственный драйвер
 * (drivers/net/phy/rtl8367b.c, swconfig switch0): сброс, таблицы чипа и
 * платы из DTS, VLAN из /etc/config/network. Здесь его больше не трогаем.
 * The external RTL8367D is set up by its own driver (rtl8367b, swconfig).
 */
static int rtl819x_open(struct net_device *dev)
{
	struct rtl819x_eth *eth = netdev_priv(dev);
	int ret;

	ret = request_irq(eth->irq, rtl819x_irq, 0, dev->name, dev);
	if (ret) {
		netdev_err(dev, "cannot request IRQ %d\n", eth->irq);
		return ret;
	}

	if (eth->soc->hw_init)
		eth->soc->hw_init(eth);

	ret = rtl819x_rings_init(eth);
	if (ret) {
		netdev_err(dev, "cannot allocate descriptor rings\n");
		free_irq(eth->irq, dev);
		return ret;
	}

	napi_enable(&eth->napi);


	rtl819x_intsw_nastroit();

	/* разрешаем приём и передачу */
	rtl819x_m32(eth, 0, RTL819X_TXCMD | RTL819X_RXCMD, RTL819X_CPUICR);
	rtl819x_irq_enable_rx(eth);

	/*
	 * Снимок регистров сетевого контроллера - снят ПОСЛЕ разрешения приёма и передачи, иначе сравнение мнимое
	 * тем же способом, каким сличали коммутатор.
	 */
	{
		unsigned int a;

		for (a = 0xB8010000; a < 0xB8010200; a += 32)
			pr_info("NICREG %08x: %08x %08x %08x %08x %08x %08x %08x %08x\n",
				a,
				rtl_sw_r(a +  0), rtl_sw_r(a +  4),
				rtl_sw_r(a +  8), rtl_sw_r(a + 12),
				rtl_sw_r(a + 16), rtl_sw_r(a + 20),
				rtl_sw_r(a + 24), rtl_sw_r(a + 28));
	}

	netif_start_queue(dev);

	return 0;
}

static int rtl819x_stop(struct net_device *dev)
{
	struct rtl819x_eth *eth = netdev_priv(dev);

	netif_stop_queue(dev);
	rtl819x_irq_disable_all(eth);
	rtl819x_m32(eth, RTL819X_TXCMD | RTL819X_RXCMD, 0, RTL819X_CPUICR);
	napi_disable(&eth->napi);
	free_irq(eth->irq, dev);
	rtl819x_rings_free(eth);

	return 0;
}

static int rtl819x_set_mac_address(struct net_device *dev, void *p)
{
	struct sockaddr *addr = p;

	if (!is_valid_ether_addr(addr->sa_data))
		return -EADDRNOTAVAIL;

	eth_hw_addr_set(dev, addr->sa_data);

	return 0;
}

static void rtl819x_tx_timeout(struct net_device *dev, unsigned int txqueue)
{
	netdev_warn(dev, "transmit timeout (queue %u)\n", txqueue);
}

static const struct net_device_ops rtl819x_netdev_ops = {
	.ndo_open		= rtl819x_open,
	.ndo_stop		= rtl819x_stop,
	.ndo_start_xmit		= rtl819x_start_xmit,
	.ndo_set_mac_address	= rtl819x_set_mac_address,
	.ndo_validate_addr	= eth_validate_addr,
	.ndo_tx_timeout		= rtl819x_tx_timeout,
};

/* ---- описание чипов ---------------------------------------------------- */

static void rtl819x_8197f_hw_init(struct rtl819x_eth *eth)
{
	/*
	 * Порядок из вендорского SDK: сначала полный сброс интерфейса,
	 * затем размер буфера и режим шины, и только потом разрешение
	 * приёма/передачи (его делает rtl819x_open).
	 */
	rtl819x_w32(eth, 0, RTL819X_CPUICR);
	/*
	 * 128 слов. Пробовал 32, как у загрузчика - сеть не пошла, вернул.
	 * сеть работает. Снято сличением живых снимков - у него CPUICR
	 * равен 0xC4000000, у нас было 0xE4000000, и вся разница в этом
	 * поле. Величина не наша и подбирать её незачем.
	 */
	rtl819x_w32(eth, RTL819X_BUSBURST_128W | RTL819X_MBUF_2048B,
		    RTL819X_CPUICR);

	/*
	 * Пороги FIFO выставляются строго после CPUICR: смена размера пачки
	 * сбрасывает HiFifoMark в 0x57 (rtl865x_asicCom.c:1408).
	 */
	rtl819x_m32(eth,
		    RTL819X_LOWFIFOMARK_MASK | RTL819X_HIFIFOMARK_MASK,
		    RTL819X_FIFOMARK_8197F,
		    RTL819X_DMA_CR0);

	/*
	 * Порядок байтов шины сетевого узла. Найдено 01.09.2026.
	 *
	 * Без этого бита кадр приходит развёрнутым внутри каждого
	 * четырёхбайтного слова: MAC 3c:97:0e:c7:0d:f5 читается как
	 * c7:0e:97:3c ... f5:0d. Адрес получателя при этом перестаёт быть
	 * широковещательным, ядро помечает кадр как чужой (pkt_type=3),
	 * мост учит адреса, но наверх не отдаёт ничего: eth0 принимает,
	 * br-lan стоит на нуле. Ошибок при этом НЕ видно - сумма кадра
	 * считается до DMA и сходится.
	 *
	 * Проверено на живой плате: бит поставлен - DHCP выдал адрес и
	 * пинг пошёл 6 из 6; снят - приём снова разваливается.
	 */
	rtl819x_m32(eth, 0, RTL819X_CF_NIC_LITTLE_ENDIAN,
		    RTL819X_CPUICR1);
}

static const struct rtl819x_soc rtl8197f_soc = {
	.cpu_port	= 8,	/* внутренний коммутатор: порты 0..8, процессор восьмой */
	.max_mtu	= RTL819X_MAX_FRAME - RTL819X_FRAME_OVERHEAD,
	.rx_rings	= 1,	/* для начала одно кольцо, потом расширим */
	/* Кольцо 1 мы не используем для отправки, но включаем вместе с
	 * нулевым (так делает вендор), а значит обязаны выделить его и
	 * дать ему адрес: включённое кольцо без базы вешает передачу. */
	.tx_rings	= 2,
	.hw_init	= rtl819x_8197f_hw_init,
};

static const struct of_device_id rtl819x_eth_of_ids[] = {
	{ .compatible = "realtek,rtl8197f-eth", .data = &rtl8197f_soc },
	{ /* конец списка */ }
};
MODULE_DEVICE_TABLE(of, rtl819x_eth_of_ids);

/* ---- регистрация ------------------------------------------------------- */

static int rtl819x_eth_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct net_device *dev;
	struct rtl819x_eth *eth;
	int ret;

	dev = devm_alloc_etherdev(&pdev->dev, sizeof(struct rtl819x_eth));
	if (!dev)
		return -ENOMEM;

	SET_NETDEV_DEV(dev, &pdev->dev);
	platform_set_drvdata(pdev, dev);

	eth = netdev_priv(dev);
	eth->dev = dev;
	eth->pdev = pdev;
	eth->soc = of_device_get_match_data(&pdev->dev);
	if (!eth->soc)
		return -EINVAL;

	spin_lock_init(&eth->lock);

	eth->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(eth->base))
		return PTR_ERR(eth->base);

	eth->irq = platform_get_irq(pdev, 0);
	if (eth->irq < 0)
		return eth->irq;

	/* MAC-адрес: из device tree, иначе случайный */
	ret = of_get_ethdev_address(np, dev);
	if (ret)
		eth_hw_addr_random(dev);

	dev->netdev_ops = &rtl819x_netdev_ops;
	/* проверка сумм на приёме, выключается ethtool -K eth0 rx off */
	dev->hw_features |= NETIF_F_RXCSUM;
	dev->features |= NETIF_F_RXCSUM;
	/*
	 * Метку 802.1Q стек отдаёт и принимает отдельно от данных: в режиме
	 * роутера она едет в дескрипторе, в старом режиме драйвер сам кладёт
	 * её в кадр (rtl819x_start_xmit). Как у донора DIR-842.
	 */
	dev->features |= NETIF_F_HW_VLAN_CTAG_TX | NETIF_F_HW_VLAN_CTAG_RX;
	dev->min_mtu = ETH_ZLEN;
	dev->max_mtu = eth->soc->max_mtu;
	dev->watchdog_timeo = 5 * HZ;

	netif_napi_add(dev, &eth->napi, rtl819x_poll);

	/* до включения интерфейса железо не должно дёргать процессор */
	rtl819x_irq_disable_all(eth);

	ret = devm_register_netdev(&pdev->dev, dev);
	if (ret)
		return ret;
	rtl819x_dev = dev;

	proc_create_single("rtl819x_spa", 0444, NULL, rtl819x_spa_pokaz);
	proc_create("rtl819x_gnezda", 0644, NULL, &rtl819x_gnezda_ops);
	proc_create("rtl819x_router", 0644, NULL, &rtl819x_router_ops);
	proc_create("rtl819x_hs", 0644, NULL, &rtl819x_hs_ops);

	netdev_info(dev, "Realtek RTL819x Ethernet at %pR, irq %d\n",
		    platform_get_resource(pdev, IORESOURCE_MEM, 0), eth->irq);

	return 0;
}

static void rtl819x_eth_remove(struct platform_device *pdev)
{
	struct net_device *dev = platform_get_drvdata(pdev);
	struct rtl819x_eth *eth = netdev_priv(dev);

	rtl819x_irq_disable_all(eth);
	netif_napi_del(&eth->napi);
}

static struct platform_driver rtl819x_eth_driver = {
	.probe	= rtl819x_eth_probe,
	.remove	= rtl819x_eth_remove,
	.driver	= {
		.name		= RTL819X_DRV_NAME,
		.of_match_table	= rtl819x_eth_of_ids,
	},
};
module_platform_driver(rtl819x_eth_driver);

MODULE_DESCRIPTION("Realtek RTL819x Ethernet driver");
MODULE_LICENSE("GPL");
