/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Ethernet-контроллер Realtek RTL819x (RTL8196E / RTL8197D / RTL8197F).
 *
 * Регистры и битовые поля выписаны из вендорского SDK
 * (AsicDriver/rtl865xc_asicregs.h, ~3900 строк) — здесь только то,
 * что нужно для приёма-передачи через CPU-интерфейс свитч-ядра.
 *
 * Смещения даны относительно CPU_IFACE_BASE: драйвер получает базу
 * через ioremap() из device tree, а не пишет по физическому адресу,
 * как это делал вендорский код (0xB8010000, KSEG1).
 */

#ifndef _RTL819X_ETH_H
#define _RTL819X_ETH_H

/* ---- Регистры CPU-интерфейса (смещения от базы) ---------------------- */

#define RTL819X_CPUICR			0x000	/* управление интерфейсом */
#define RTL819X_CPURPDCR(n)		(0x004 + ((n) << 2))	/* Rx-кольца, 0..5 */
#define RTL819X_CPURMDCR0		0x01c	/* Rx mbuf-дескрипторы */
#define RTL819X_CPUTPDCR(n)		(0x020 + ((n) << 2))	/* Tx-кольца, 0..1 */
#define RTL819X_CPUIIMR			0x028	/* маска прерываний */
#define RTL819X_CPUIISR			0x02c	/* статус прерываний */
#define RTL819X_CPUQDM(n)		(0x030 + ((n) << 1))	/* очередь -> кольцо */
#define RTL819X_DMA_CR0			0x03c
#define RTL819X_DMA_CR1			0x040
#define RTL819X_DMA_CR2			0x044
#define RTL819X_CPUTPDCR2		0x060
#define RTL819X_CPUTPDCR3		0x064
#define RTL819X_DMA_CR3			0x068
#define RTL819X_TXRINGCR		0x078	/* управление Tx-кольцами */
#define RTL819X_DMA_CR4			0x0a0
#define RTL819X_CPUICR1			0x0a4

/* CPUICR1, бит 1: шина сетевого узла работает в младшем порядке байтов.
 * Без него DMA раскладывает байты кадра старшим вперёд, и кадр приходит
 * развёрнутым внутри каждого четырёхбайтного слова. Имя поля вендорское:
 * CF_NIC_LITTLE_ENDIAN, AsicDriver/rtl865xc_asicregs.h:482. */
#define RTL819X_CF_NIC_LITTLE_ENDIAN	(1 << 1)

/* ---- CPUICR: управление приёмом/передачей ---------------------------- */

#define RTL819X_TXCMD			BIT(31)	/* разрешить передачу */
#define RTL819X_RXCMD			BIT(30)	/* разрешить приём */
#define RTL819X_TXFD			BIT(23)	/* дёрнуть выборку Tx-дескриптора */

/* размер пакета за одну транзакцию на шине */
#define RTL819X_BUSBURST_32W		(0 << 28)
#define RTL819X_BUSBURST_64W		(1 << 28)
#define RTL819X_BUSBURST_128W		(2 << 28)
#define RTL819X_BUSBURST_256W		(3 << 28)

/* размер буфера под данные пакета */
#define RTL819X_MBUF_128B		(0 << 24)
#define RTL819X_MBUF_256B		(1 << 24)
#define RTL819X_MBUF_512B		(2 << 24)
#define RTL819X_MBUF_1024B		(3 << 24)
#define RTL819X_MBUF_2048B		(4 << 24)

/* ---- DMA_CR0: пороги заполнения FIFO ---------------------------------- */

/*
 * У 8197F смена поля размера пачки в CPUICR сбрасывает HiFifoMark
 * в значение по умолчанию 0x57, поэтому пороги выставляются ПОСЛЕ
 * записи CPUICR (rtl865x_asicCom.c:1409).
 */
#define RTL819X_LOWFIFOMARK_SHIFT	8
#define RTL819X_LOWFIFOMARK_MASK	(0xff << 8)
#define RTL819X_HIFIFOMARK_MASK		(0xff << 0)
/*
 * Пороги 0xa0/0xa0 из вендорского кода. Пробовал 0x40/0xE6, снятые с
 * живого загрузчика - сеть не пошла, вернул. Урок: пороги заполнения
 * очереди связаны с устройством колец описателей в памяти, а кольца у
 * загрузчика свои. Число из чужого расклада сюда не переносится.
 *
 */
#define RTL819X_FIFOMARK_8197F		((0xa0 << 8) | 0xa0)

/* ---- CPUIIMR / CPUIISR: прерывания ----------------------------------- */

/*
 * ВАЖНО: регистр разрешения (CPUIIMR) и регистр статуса (CPUIISR)
 * разложены ПО-РАЗНОМУ. Совпадающие по смыслу источники стоят на разных
 * битах: например «кончились mbuf» — бит 11 в маске и бит 16 в статусе.
 * Поэтому константы разделены на две группы: _IE_ для маски, _IP_ для
 * статуса. Смешивать их нельзя — именно так был получен шторм прерываний
 * на 178 млн срабатываний.
 */

/* маска разрешения — CPUIIMR */
#define RTL819X_LINK_CHANGE_IE		BIT(31)		/* смена состояния линка */
#define RTL819X_PKTHDR_RUNOUT_IE_ALL	(0x3f << 17)	/* кончились Rx-дескрипторы */
#define RTL819X_MBUF_RUNOUT_IE0		BIT(11)		/* кончились mbuf (в статусе это бит 16!) */
#define RTL819X_TX_DONE_IE_ALL		(0x03 << 9)	/* передан пакет, кольца 0..1 */
#define RTL819X_RX_DONE_IE_ALL		(0x3f << 3)	/* принят пакет, кольца 0..5 */

/*
 * У 8197F колец передачи четыре, и «всё передано» разнесено:
 * кольца 0,1 — биты 1,2; кольца 2,3 — биты 12,13 (rtl865xc_asicregs.h:369-373).
 */
#define RTL819X_TX_ALL_DONE_IE_ALL	(BIT(1) | BIT(2) | BIT(12) | BIT(13))

/*
 * Ровно то, что пишет вендор в rtl865x_start() (rtl865x_asicCom.c:1417):
 * приём, «всё передано», смена линка и нехватка дескрипторов. Значение 0x807e31fe.
 * Ошибки приёма/передачи вендор не разрешает нигде — в маске их биты заняты
 * под другое.
 */
/* что вендор маскирует на время работы NAPI (rtl_nic.c:7443) */
#define RTL819X_IIMR_RX_BITS		(RTL819X_RX_DONE_IE_ALL | \
					 RTL819X_PKTHDR_RUNOUT_IE_ALL)

#define RTL819X_IIMR_NORMAL		(RTL819X_RX_DONE_IE_ALL | \
					 RTL819X_TX_ALL_DONE_IE_ALL | \
					 RTL819X_LINK_CHANGE_IE | \
					 RTL819X_PKTHDR_RUNOUT_IE_ALL)

/* статус — CPUIISR */
#define RTL819X_LINK_CHANGE_IP		BIT(31)
#define RTL819X_RX_ERR_IP_ALL		(0x3f << 25)
#define RTL819X_TX_ERR_IP_ALL		(0x03 << 23)
#define RTL819X_PKTHDR_RUNOUT_IP_ALL	(0x3f << 17)
#define RTL819X_MBUF_RUNOUT_IP0		BIT(16)
#define RTL819X_TX_DONE_IP_ALL		(0x03 << 9)
#define RTL819X_RX_DONE_IP_ALL		(0x3f << 3)
#define RTL819X_TX_ALL_DONE_IP_ALL	(BIT(1) | BIT(2) | BIT(12) | BIT(13))

/* ---- Биты дескриптора (opts1) ---------------------------------------- */

#define RTL819X_DESC_OWNED_BIT		BIT(0)	/* чей дескриптор */
#define RTL819X_DESC_RISC_OWNED		0	/* ...процессора: можно трогать */
#define RTL819X_DESC_SWCORE_OWNED	BIT(0)	/* ...железа: не трогать */
#define RTL819X_DESC_WRAP		BIT(1)	/* последний в кольце */

/*
 * Старшие 16 бит opts1 приёмного дескриптора — размер буфера, на который
 * этот дескриптор указывает (поле m_extsize, rtl819x_swNic.h:145).
 * Вендор кладёт туда clusterSize и без него железо считает буфер нулевым.
 */
#define RTL819X_RX_EXTSIZE_SHIFT	16

/* ---- Дескрипторы DMA ------------------------------------------------- */

/*
 * Формат из вендорского rtl819x_swNic.h. Кольца требуют выравнивания
 * на 256 байт — это требование железа, не оптимизация.
 */
#define RTL819X_DESC_ALIGN		256

struct rtl819x_rx_desc {
	u32 opts1;	/* own(1) eor(1) ls(1) fs(1) rcdf(1) fae(1) rsvd(10) extsize(16) */
	u32 addr;	/* физический адрес буфера */
	u32 opts2;	/* len(14) rsvd(3) qid(3) dp_ext(4) extspa(2) */
	u32 opts3;
	u32 opts4;
	u32 opts5;
} __packed __aligned(4);

struct rtl819x_tx_desc {
	u32 opts1;
	u32 addr;
	u32 opts2;
	u32 opts3;
	u32 opts4;
	u32 opts5;
} __packed __aligned(4);

/* поля opts1 приёмного дескриптора */
#define RTL819X_RX_OWN			BIT(0)
#define RTL819X_RX_EOR			BIT(1)
#define RTL819X_RX_LS			BIT(2)	/* последний сегмент пакета */
#define RTL819X_RX_FS			BIT(3)	/* первый сегмент пакета */
#define RTL819X_RX_CRC_ERR		BIT(4)
#define RTL819X_RX_FAE			BIT(5)	/* ошибка выравнивания кадра */

/* ---- передающий дескриптор ------------------------------------------
 *
 * ВНИМАНИЕ: раскладка НЕ совпадает с приёмным. Общие только own (бит 0)
 * и eor (бит 1); дальше всё своё, и длина живёт в другом месте.
 * Поля выписаны из struct tx_desc (rtl819x_swNic.h, ветка _LITTLE_ENDIAN).
 */
#define RTL819X_TX_LS			BIT(2)	/* последний сегмент кадра */
#define RTL819X_TX_FS			BIT(3)	/* первый сегмент кадра */
#define RTL819X_TX_HWLKUP		BIT(4)	/* свитч сам ищет порт по MAC-таблице */
#define RTL819X_TX_BRIDGE		BIT(5)	/* кадр идёт через мост свитча */
#define RTL819X_TX_PHLEN_SHIFT		6	/* opts1, биты 6..22 */
#define RTL819X_TX_PHLEN_MASK		GENMASK(22, 6)

#define RTL819X_TX_MLEN_SHIFT		15	/* opts2, биты 15..31 */
#define RTL819X_TX_MLEN_MASK		GENMASK(31, 15)

#define RTL819X_TX_DP_SHIFT		24	/* opts4, биты 24..30: маска портов */
#define RTL819X_TX_DP_MASK		GENMASK(30, 24)

/*
 * Маска физических портов. CPU-порт (седьмой) сюда не входит: кадр
 * пришёл оттуда, отправлять его обратно незачем.
 */
#define RTL819X_TX_DP_ALL		0x3f

#define RTL819X_TX_EXTSPA_SHIFT		30	/* opts5, биты 30..31 */
/*
 * Источник кадра. Железо кодирует это поле так (rtl819x_swNic.h:129):
 *
 *     [31:30] extspa: Source extension port number: 2'd0: from CPU,
 *                     2'd1~2'd3: from extension port numbers
 *
 * То есть "кадр от процессора" это НОЛЬ. Раньше здесь стояла тройка,
 * взятая из вендорского PKTHDR_EXTPORT_LIST_CPU (common/mbuf.h:121).
 * Но это ПРОГРАММНЫЙ номер в списке портов, он служит для масок dp_ext,
 * а не для этого поля. Тройка означает "кадр пришёл с расширенного
 * порта 3", и свитч, не считая процессор источником, возвращал нам наш
 * же кадр обратно.
 *
 * Замерено 30.08: отправлено 30 кадров, принято ровно те же 30, при
 * нуле у получателя.
 */
#define RTL819X_TX_EXTSPA_CPU		0	/* источник кадра — процессор */

/* поля opts2 — длина принятого пакета */
#define RTL819X_RX_LEN_MASK		GENMASK(13, 0)

/* ---- Управление кольцами передачи ------------------------------------
 *
 * Мало записать адрес кольца в CPUTPDCR: контроллеру нужна ещё его
 * граница. В DMA_CR1 кладётся смещение последнего дескриптора в байтах,
 * иначе железо забирает дескрипторы и не возвращает.
 */
#define RTL819X_TX_RING0_TAIL_AWARE	BIT(0)	/* DMA_CR4 */

#define RTL819X_TX_RING0_EN		BIT(0)	/* TXRINGCR, обязан быть включён */
#define RTL819X_TX_RING1_EN		BIT(1)
#define RTL819X_TX_RING2_EN		BIT(2)
#define RTL819X_TX_RING3_EN		BIT(3)

/* ---- Параметры колец -------------------------------------------------- */

#define RTL819X_RX_RING_COUNT		6	/* колец приёма в железе */
#define RTL819X_TX_RING_COUNT		2	/* колец передачи */

#define RTL819X_RX_RING_SIZE		128
#define RTL819X_TX_RING_SIZE		128

#endif /* _RTL819X_ETH_H */
