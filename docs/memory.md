# Память: что сколько ест / Memory: what uses RAM

> **RU** ниже, **EN** — в конце.

64 МБ ОЗУ, после загрузки v0.1.2 `MemAvailable` 6,9–10,6 МБ. Ниже — куда уходит остальное.
Цифры получены из исходников и сборки v0.1.2: `sizeof` и макросы — проба, скомпилированная с
флагами драйвера; размеры в образе ядра — `nm --size-sort vmlinux`. С живого роутера здесь
только строка `Memory:`; остальное — оценка, её надо сверить замером `MemAvailable`.

Пути к драйверу Wi-Fi даны от `openwrt/target/linux/rtl819x/files-6.18/drivers/net/wireless/rtl8192fe/`.
Номера строк — по исходникам тега v0.1.2; правки из раздела 3 их сдвигают.

## Почему блоки больше, чем просят

`kmalloc()` больше 8 КБ (`KMALLOC_MAX_CACHE_SIZE`) идёт мимо slab прямо в buddy, а тот выдаёт
2^n страниц. Структура на 845 КБ занимает 1 МБ, на 260 КБ — 512 КБ. Хвост блока пропадает.
`alloc_pages_exact()` берёт тот же блок, но сразу возвращает лишние страницы; память та же —
непрерывная, в lowmem, выровненная на страницу. `vmalloc()` собирает буфер из отдельных страниц,
он годится только для того, к чему не ходит DMA.

## 1. Строка `Memory:` и «9996K reserved»

```
Memory: 55072K/65536K available (5926K kernel code, 1078K rwdata, 924K rodata, 1216K init, 223K bss, 9996K reserved, 0K cma-reserved)
```

| Часть reserved | КБ | Источник |
|---|---:|---|
| Образ ядра: code 5926 + rwdata 1078 + rodata 924 + init 1216 + bss 223 | 9367 | строка `Memory:` |
| — из них init, возвращается после загрузки («Freeing unused kernel image») | 1216 | поэтому `MemTotal` ≈ 55072 + 1216 = 56288 КБ |
| `mem_map`: 16384 страницы × 32 Б `struct page` | 512 | FLATMEM, `sizeof(struct page)` = 32 |
| Прочие ранние выделения memblock (дерево устройств и т.п.) | ≈117 | остаток |
| CMA | 0 | `CONFIG_CMA` не включён — «0K cma-reserved» |
| `mem=` | — | не задан: `bootargs = "console=ttyS0,38400"` (`dts/rtl8197f_electra_rx23302.dts:38`), память 64 МБ (`:34`) |

DMA-пулов в reserved нет: кольца и буферы Wi-Fi драйвер берёт из обычной памяти уже после
загрузки — они видны как уменьшение `MemFree` (раздел 2), а не как reserved.

### Крупное внутри образа ядра (vmlinux v0.1.2)

| Что | КБ | Источник |
|---|---:|---|
| Драйвер Wi-Fi: код | ≈1121 | символы объектов `rtl8192fe` в vmlinux |
| Драйвер Wi-Fi: данные (`.data`, `.rodata`) + bss | ≈597 + 32 | в том числе таблицы и прошивки — 526 (ниже) |
| `_printk_rb_static_infos`: 4096 записей × 88 Б | 352 | `CONFIG_LOG_BUF_SHIFT=17` (OpenWrt `target/linux/generic/config-6.18:3445`), `.data` |
| `_printk_rb_static_descs`: 4096 × 12 Б | 48 | то же |
| `__log_buf`: 2^17 Б | 128 | то же, `.bss` |

Кольцо printk стоит 528 КБ, а не 128: на каждые 32 байта текста ядро держит ещё 100 байт
описателей. При `LOG_BUF_SHIFT=15` — 132 КБ.

### Таблицы и прошивки драйвера Wi-Fi в vmlinux

В `built-in.a` драйвера 221 таблица `data_*`/`array_*` на 774 КБ, но ядро собирается с
`CONFIG_LD_DEAD_CODE_DATA_ELIMINATION=y` и `-ffunction-sections -fdata-sections`, и всё, на что нет
ссылок, компоновщик выбрасывает. В vmlinux остаётся 140 объектов на 530,6 КБ (4,4 КБ из них — не
драйвер, а функции ядра с похожими именами):

| Группа | Объектов | КБ | Нужна? |
|---|---:|---:|---|
| Прошивки радио: `data_rtl8812Ffw` 135,6 + `data_rtl8197Gfw` 58,5 | 6 | 194,1 | да |
| Таблицы 8197G (`data_*_8197G*`, `array_mp_8197g_*`) | 37 | 90,0 | да |
| Таблицы 8812F «mp» без номера типа | 53 | 97,7 | да |
| Таблицы 8812F по типам RFE: `PHY_REG_PG_8812Fmp_TypeN` + `TXPWR_LMT_8812Fmp_TypeN`, N = 0, 2, 8, 9, 10, 13, 14 | 14 | 144,5 | нужен один тип |
| Таблицы тестового чипа 8812F (без «mp») | 0 | 0 | в vmlinux уже нет |
| `data_rtl8812FfwMP` (75,5 КБ в `built-in.a`) | 0 | 0 | в vmlinux уже нет |

**«mp» — это не заводской тестовый режим, а серийный чип (mass production).**
`WlanHAL/RTL88XX/RTL8812F/Hal8812FGen.c:22` подключает под комментарием `// TestChip` таблицы
`data_*_8812F.c`, а `:142` под `//MP Chip` — `data_*_8812Fmp.c`. Выбор делается по
`IS_HAL_TEST_CHIP()` (`WlanHAL/RTL88XX/Hal88XXGen.c:1596–1603`): серийный чип берёт таблицы `…mp`.
Если их выкинуть, 5 ГГц останется без таблиц PHY.

**Тип RFE.** `Hal88XXGen.c:2500–2524` выбирает `PHY_REG_PG_8812Fmp_TypeN` по `rfe_type` и печатает
выбор через `panic_printk` строкой `[…][PHY_REG_PG_8812Fmp_TypeN]` (для типов, которых нет в
списке, — `[…][PHY_REG_PG_8812Fmp]`). Наш драйвер ставит `rfe_type = 0` для обоих радио
(`8192cd_osdep.c:13745` — 8812F, `:13754` — 8197G; это настройка вендора из `rx23302.mk:47`, со
стоком не сверена). Если сток подтвердит тип 0, таблицы типов 2, 8, 9, 10, 13 и 14 можно убрать:
−131,7 КБ образа (144,5 − 12,8 КБ типа 0). Если тип другой — убирать надо другие, а наш драйвер
сейчас грузит не ту калибровку.

Как узнать тип в стоке: в UART-логе загрузки заводской прошивки найти строку с
`PHY_REG_PG_8812Fmp`, или в заводской консоли `cat /proc/wlan0/mib_rf /proc/wlan1/mib_rf | grep rfe_type`
(поле печатает `8192cd_proc.c:2067`, файл — `:10335`).

## 2. Драйвер Wi-Fi: что выделяется при загрузке

Два радио: 2,4 ГГц (RTL8197G, в SoC, `wlan0`) и 5 ГГц (RTL8812F, PCIe, `wlan1`). На каждом
радио при загрузке создаются 5 интерфейсов — основной, 3 VAP (`RTL8192CD_NUM_VWLAN=3`,
`rx23302.mk:48`) и `vxd` (`UNIVERSAL_REPEATER`) — независимо от настроек UCI.

Колонки: **нужно** — `sizeof`/запрос; **занято** — блок, который выдаёт buddy; **точно** — то же
число страниц без округления до 2^n; **вернём** — разница, ×2 радио или ×10 интерфейсов.

### 2.1 На каждое радио

| Что | Нужно, КБ | Занято, КБ | Точно, КБ | Вернём ×2, КБ | Источник |
|---|---:|---:|---:|---:|---|
| `struct rtl8192cd_hw` | 844,1 | 1024 | 848 | 352 | `8192cd_osdep.c:15336` |
| `struct priv_shared_info` | 48,9 | 64 | 52 | 24 | `:15358` |
| `wlan_hdr_poll` (`PRE_ALLOCATED_HDR` = 5 × `NUM_TX_DESC` = 5120) | 240,0 | 256 | 244 | 24 | `:15375`, `8192cd_cfg.h:4511` |
| `wlanllc_hdr_poll` | 360,0 | 512 | 364 | 296 | `:15391` |
| `wlanbuf_poll` | 92,0 | 128 | 96 | 64 | `:15407` |
| `wlanicv_poll` | 80,0 | 128 | 84 | 88 | `:15423` |
| `wlanmic_poll` | 80,0 | 128 | 84 | 88 | `:15440` |
| `rsv_buf` (`MAX_RSV_BUF_SIZE`) | 64 | 64 | 64 | 0 | `:18370` |
| **Итого** | **1809** | **2304 × 2 = 4608** | | **936** | |

Кольца DMA (`InitWlanBuffer88XX_V1`, `8192cd_osdep.c:14423`; для 8197G и 8812F выбирается
именно она — `:17880–17886`; размеры — `WlanHAL/RTL88XX/RTL8197G/Hal8197GCfg.h:29–49`,
`RTL8812F/Hal8812FCfg.h:29–49`):

| Что | Нужно 2,4 / 5, КБ | Занято, КБ | Точно 2,4 / 5, КБ | Вернём 2,4 + 5, КБ | Источник |
|---|---:|---:|---:|---:|---|
| TXBD: 7200 / 6176 × 32 Б, +4 КБ на выравнивание | 229 / 197 | 256 + 256 | 232 / 200 | 24 + 56 | `:14758` |
| TXDESC: 7200 / 6176 × 64 Б, +4 КБ | 454 / 390 | 512 + 512 | 456 / 392 | 56 + 120 | `:14767` |
| BCN | 4,75 | 8 + 8 | 8 | 0 | `:14776` |
| AMSDU (`DESC_DMA_PAGE_SIZE_HAL_FOR_AMSDU`) | 260 / 260 | 512 + 512 | 260 / 260 | 252 + 252 | `:14822` |
| RXBD (`dma_alloc_coherent`) | 6 / 6 | 8 + 8 | — | 0 | `:14745` |
| **Итого** | | **2592** | | **760** | |

`NUM_TX_DESC` = 1024 на очередь (`8192cd_cfg.h:3390`); 7200 и 6176 — сумма по всем очередям
(`TOTAL_NUM_TXBD_NO_BCN_8197G/_8812F`).

Буферы приёма — 256 skb на радио (`NUM_RX_DESC`, `8192cd_cfg.h:3485`), обычный `dev_alloc_skb()`
(`8192cd_osdep.c:4678` → `WlanHAL/RTL88XX/Hal88XXRxDesc.c:334` → `8192cd_util.c:7228`):

| Радио | Длина буфера | Чем выделяется | Занято, КБ | Источник |
|---|---:|---|---:|---|
| 5 ГГц (`wlan1`) | 4000 Б | 4000 + 64 (`NET_SKB_PAD`) > 3840 = `SKB_WITH_OVERHEAD(PAGE_SIZE)` → `kmalloc-8k` | 256 × 8 = 2048 | `MAX_RX_BUF_LEN`, `8192cd_cfg.h:3974` |
| 2,4 ГГц (`wlan0`) | 2600 Б | фрагмент страницы 2944 Б | ≈736 | `MAX_RX_BUF_LEN_1`, `8192cd_cfg.h:3961` |
| Заголовки `sk_buff` | 176 Б | `skbuff_head_cache` | ≈90 | |

Какое радио берёт 4000: `core/core.h:143` сравнивает `wlandev_idx` с `0^WLANIDX`, а `WLANIDX` = 1
при `CONFIG_BAND_2G_ON_WLAN0` (`8192cd_cfg.h:4900`, `rx23302.mk:12`).

### 2.2 На каждый интерфейс (5 на радио, 10 всего)

| Что | Нужно, КБ | Занято, КБ | ×10, КБ | Вернём, КБ | Источник |
|---|---:|---:|---:|---:|---|
| `net_device` + `struct rtl8192cd_priv` | 1,3 + 27,0 | 32 | 320 | — | `8192cd_osdep.c:15226` |
| `struct wifi_mib` | 31,0 | 32 | 320 | — | `:15250` |
| `DOT11_QUEUE` × 2 (события, `GENERAL_EVENT`) | 12,2 | 16 | 320 | 80 (кандидат) | `:15259`, `:15304` |
| `struct ss_res` (результаты сканирования, 3 × 64 `bss_desc`) | 162,5 | 256 | 2560 | **920** (vmalloc: 164) | `:15502` → `:14090` |
| `wlan_acl_poll`, `Eap_packet`, `wpa_global_info` | 2,5 + 1,5 + 0,6 | 4 + 2 + 1 | 70 | — | `:15460`, `:15478`, `:15488` |

`ss_res` выделяется через `rtw_vmalloc_flag()`, но `CONFIG_USE_VMALLOC` определён только для
USB/SDIO (`8192cd_cfg.h:4661`, `:4713`), поэтому у нас это `kmalloc()` (`8192cd_util.h:1403–1413`).
Оценка «+2 VAP ≈ 0,3 МБ» в `rx23302.mk:42` не учитывала `ss_res`: один интерфейс сейчас стоит
≈360 КБ.

### 2.3 На каждого клиента (во время работы)

| Что | Нужно, КБ | Занято, КБ | Точно, КБ | Источник |
|---|---:|---:|---:|---|
| `struct aid_obj` (`stat_info` 17,3 КБ) | 17,3 | 32 | 20 | `8192cd_util.c:613` |

### Итого по Wi-Fi при загрузке

≈13,3 МБ, из них ≈3,7 МБ — округление до 2^n: `ss_res` 0,9; `rtl8192cd_hw`, `pshare` и пулы 1,0;
кольца TX и AMSDU 0,8; буферы приёма 5 ГГц 1,0 (4320 из 8192 байт на буфер).

## 3. Правки в этой ветке

Каждая — отдельным коммитом, в этом порядке; они накопительные (коммит 4 использует помощник из
коммита 3).

| # | Коммит | `MemAvailable`, ожидаем | Риск |
|---|---|---:|---|
| 1 | printk AMSDU: аргумент `__func__` без `%s` в формате | 0 | нет; в логе станет `InitWlanBuffer88XX_V1: AMSDU: cpu_addr …, size 266240`, по строке на радио |
| 2 | `ss_res` через `vzalloc()`/`vfree()` | +920 КБ | низкий: таблица только для процессора, без DMA; выделяется при загрузке в контексте процесса (`GFP_KERNEL`) |
| 3 | `rtl8192cd_hw`, `pshare` и пять пулов — `alloc_pages_exact()` | +936 КБ | низкий: та же непрерывная lowmem, `virt_to_phys()` работает как раньше |
| 4 | TXBD, TXDESC и AMSDU — `alloc_pages_exact()` | +760 КБ | низкий: адрес по-прежнему выровнен на страницу, размер с тем же запасом в 4 КБ |
| 5 | `CONFIG_LOG_BUF_SHIFT=15` | +396 КБ (и `MemTotal` +396; в vmlinux `.data` 1075 → 775 КБ, `.bss` 224 → 128 КБ) | `dmesg` хранит 32 КБ текста вместо 128 — начало загрузки может вытесниться; UART показывает всё, `logread` тоже получает сообщения ядра |
| | **Всего** | **≈ +3,0 МБ** | |

Как мерить: `MemAvailable` и `MemFree` после одинакового времени работы (например, медиана трёх
замеров через 5 минут после загрузки), оба радио подняты. `Slab`/`SUnreclaim` после коммитов 2–4
упадут гораздо сильнее, чем вырастет `MemAvailable`: большие `kmalloc()` считаются в `SUnreclaim`,
а `alloc_pages_exact()` и `vmalloc()` — нет (`vmalloc` виден в `VmallocUsed`). Это нормально.

## 4. Следующие кандидаты (не в этой ветке)

| Что | КБ | Что нужно решить или проверить | Источник |
|---|---:|---|---|
| Буфер приёма 5 ГГц 4000 → ≤ 3776 Б: skb помещается в страницу вместо `kmalloc-8k` | ≈ −1000 | приём больших A-MSDU на 5 ГГц (`RX_BUFFER_GATHER` включён); iperf на стенде | `8192cd_cfg.h:3974` |
| `NUM_TX_DESC` 1024 → 512: кольца TX и пулы заголовков вдвое | ≈ −1400 | глубина очереди TX, скорость при многих клиентах | `8192cd_cfg.h:3390`, `:4511` |
| VAP 3 → 1 на радио | ≈ −1000 (≈270 КБ на интерфейс после коммита 2) | в README обещано «до 3 гостевых сетей на радио» | `rx23302.mk:48` |
| Без `vxd` | ≈ −540 | пропадёт режим клиента/репитера | `UNIVERSAL_REPEATER` |
| Таблицы RFE кроме нашего типа | ≈ −132 | сначала тип из стока (раздел 1) | `Hal88XXGen.c:2500–2524` |
| `DOT11_QUEUE` — `alloc_pages_exact()` | −80 | — | `8192cd_osdep.c:15259`, `:15304` |
| `aid_obj` — `alloc_pages_exact()` | −12 на клиента | выделяется в softirq (`GFP_ATOMIC`) | `8192cd_util.c:613` |

---

## English

64 MB RAM; after boot v0.1.2 has 6.9–10.6 MB `MemAvailable`. Numbers come from the v0.1.2 sources and
build (struct sizes and macros from probes compiled with the driver's flags, image sizes from `nm`);
only the `Memory:` line is from the device. Line numbers refer to the v0.1.2 tag.

- **9996K reserved** = kernel image 9367K (1216K init is freed after boot) + `mem_map` 512K
  (16384 × 32-byte `struct page`) + ≈117K other memblock. No CMA (`CONFIG_CMA` off), no `mem=`
  (bootargs is just the console). The Wi-Fi DMA rings are not in reserved; they come from the buddy
  allocator after boot.
- **printk ring** with `LOG_BUF_SHIFT=17` costs 528K, not 128K: `_printk_rb_static_infos` 352K and
  `_descs` 48K in `.data` plus 128K `__log_buf`. `LOG_BUF_SHIFT=15` → 132K.
- **Driver tables in vmlinux** are 530.6K (140 objects), not the 774K seen in `built-in.a`: the
  kernel links with `LD_DEAD_CODE_DATA_ELIMINATION`, so test-chip tables and `data_rtl8812FfwMP` are
  already gone. "mp" means the mass-production chip (`Hal8812FGen.c:22` vs `:142`, chosen by
  `IS_HAL_TEST_CHIP()`); those tables are required. The 8812F RFE-type tables (144.5K) are needed for one
  type only; ≈132K can go once the stock RFE type is known (stock UART log line `PHY_REG_PG_8812Fmp…`
  or `rfe_type` in stock `/proc/wlanX/mib_rf`).
- **Wi-Fi at boot** ≈13.3 MB, ≈3.7 MB of it is power-of-two rounding of `kmalloc()` > 8K.

Changes in this branch, one commit each, cumulative: (1) AMSDU printk fix; (2) `ss_res` via vmalloc,
+920K; (3) `alloc_pages_exact()` for `rtl8192cd_hw`, `priv_shared_info` and the five pools, +936K;
(4) the same for TXBD/TXDESC/AMSDU, +760K; (5) `LOG_BUF_SHIFT=15`, +396K. Expected total ≈ +3.0 MB
`MemAvailable`. `SUnreclaim` drops much more than `MemAvailable` rises — large `kmalloc()` is counted
there, `alloc_pages_exact()`/`vmalloc()` are not.
