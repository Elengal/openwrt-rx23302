# Откуда что взято / Sources and licenses

Это инженерный обзор происхождения кода, а не юридическое заключение.
This is an engineering provenance review, not legal advice.

Заголовки лицензий и авторские права в файлах остаются как есть и имеют приоритет
над этим обзором. / File-level notices are kept intact and take precedence.

## База / Base

| Что | Откуда | Версия | Лицензия |
|---|---|---|---|
| OpenWrt (система сборки, пакеты) | <https://github.com/openwrt/openwrt> | `82e2e4f4` (main, 09.08.2026) | GPL-2.0-only + лицензии пакетов |
| Фид packages | <https://git.openwrt.org/feed/packages.git> | `712ee13a` | по пакетам |
| Фид LuCI | <https://git.openwrt.org/project/luci.git> | `4c0a4eda` | Apache-2.0 (LuCI) |
| Ядро Linux | kernel.org через OpenWrt | 6.18.41 | GPL-2.0 |

Закреплённые версии — в [`versions.mk.sh`](../versions.mk.sh).

## Компоненты этого репозитория / Components in this repository

Пути — внутри [`openwrt/`](../openwrt/).

### Платформа RTL8197F (`target/linux/rtl819x/`)

| Компонент | Путь | Происхождение | Лицензия |
|---|---|---|---|
| Поддержка SoC (prom, irq, timer, setup, gpio), DTSI | `files-6.18/arch/mips/realtek/`, `dts/RTL819X.dtsi`, `dts/RTL8197F*.dtsi` | форк [choryuidentify/openwrt-rtl8197f](https://github.com/choryuidentify/openwrt-rtl8197f) (ядро 4.14), авторы Weijie Gao, Gaspare Bruno; перенесено на 6.18 нами | GPL-2.0 |
| PCIe (для 5 ГГц) | `files-6.18/arch/mips/pci/pci-realtek.c` | тот же форк (Gaspare Bruno) | GPL-2.0 |
| Контроллер SPI-флеша Sheipa | `files-6.18/drivers/spi/spi-sheipa.*` | Realtek SDK (Copyright 2015 Realtek, «GPL version 2» в шапке) | GPL-2.0 |
| LZMA-загрузчик ядра | `image/lzma-loader/` | OpenWrt (Gabor Juhos) + Weijie Gao | GPL-2.0 |
| Упаковщик образа `cvimg` (KE32) | `tools/cvimg/` | Weijie Gao, по мотивам утилиты Realtek | GPL-2.0 |
| Ethernet-контроллер 8197F | `files-6.18/drivers/net/ethernet/rtl819x_eth.*` | **написан в этом проекте**, значения регистров сверены с Realtek SDK (`rtl819x_swNic.c`, GPL) | GPL-2.0-only |
| Аппаратный NAT: движок таблиц | `files-6.18/drivers/net/ethernet/rtl865x_asichal.*` | **порт** [ADCDS/openwrt-dlink-dir842-r1](https://github.com/ADCDS/openwrt-dlink-dir842-r1) (GPL-2.0): реверс-инжиниринг таблиц ASIC (L2/ARP/L3/NETIF/EXTIP/NAPT/nexthop) по стоку 3.10.90 и вендорскому SDK; адаптирован нами (раскладка платы, без PPPoE) | GPL-2.0 |
| Аппаратный NAT: conntrack → NAPT | `files-6.18/drivers/net/ethernet/rtl819x_hwnat.*` | **порт** ADCDS (GPL-2.0); переработан под штатный flowtable-интерфейс ядра 6.18 (`ndo_setup_tc`/`TC_SETUP_FT`/`FLOW_CLS_*` вместо downstream `ndo_flow_offload`) и таймауты ядра | GPL-2.0 |
| Общие регистры свитча | `files-6.18/drivers/net/ethernet/rtl819x_regs.h` | **этот проект** (значения — вендорский SDK) | GPL-2.0-only |
| Переключатель режимов NAT (init-скрипты, nft-сниппет, uci-defaults) | `base-files/etc/init.d/rtl819x-hwnat*`, `base-files/usr/share/nftables.d/table-post/20-rx23302-flowtable-eth0.nft`, `base-files/etc/uci-defaults/05_enable_flow_offloading` | **этот проект** | GPL-2.0-only |
| Device tree RX23302 | `dts/rtl8197f_electra_rx23302.dts` | **этот проект**, по живому устройству | GPL-2.0 |
| Скрипты образа | `scripts/rx23302-full-image.py`, `scripts/rx23302-tftp-image.py`, `scripts/rtl819x-rootfs-chksum.py` | **этот проект**; формат TFTP-образа — по разбору заводского загрузчика | GPL-2.0-only |
| base-files (netifd/wifi-скрипты, 02_network, uci-defaults); утилита `rtlmib` — пакет из исходника | `base-files/`, `package/utils/rtlmib/` | **этот проект**; `rtl8192cd.sh` по мотивам [sowarden/hh71vm-openwrt](https://github.com/sowarden/hh71vm-openwrt) (`8881e78`) | GPL-2.0 |

### Коммутатор RTL8367D (`files-6.18/drivers/net/phy/`)

| Компонент | Происхождение | Лицензия |
|---|---|---|
| `rtl8367b.c`, `rtl8366_smi.h` — драйвер свитча (swconfig) | OpenWrt `target/linux/generic/files/drivers/net/phy/` (Gabor Juhos и др.), копия с поддержкой RTL8367D rev 0x0030: распознавание чипа, сброс по GPIO, инициализация чипа, VLAN-запись под эту ревизию, атрибут `fid`, исправлен перевёрнутый тайм-аут чтения/записи PHY | GPL-2.0 |
| Шина SMI и общая часть (`rtl8366_smi.c`) | OpenWrt, копия: debugfs-окно (`/sys/kernel/debug/switch/`: `reg`/`val`, `pvid`, `vlan_mc`, `vlan_4k`, `mibs`) переведено на API debugfs Linux 6.x | GPL-2.0 |
| swconfig | OpenWrt, без изменений | GPL-2.0 |
| Значения аналоговой подстройки PHY (5 чисел) | значения регистров из драйвера производителя (как таблицы `jam` в mainline `rtl8365mb`) | факты о железе |
| `realtek,init-regs` в DTS — настройка платы (149 записей) | **этот проект**: сняты с работающего загрузчика и подобраны опытом на живой плате | GPL-2.0 |

Код Realtek SDK (`rtk_api.c`, `rtl8367b_asicdrv*`, `smi.c`, шапка
«proprietary») из репозитория **удалён**. Таблица начальных значений чипа
совпадает с `rtl8367b_initvals` из OpenWrt (150 пар, одна пара отличается
и задана в коде драйвера).

### Wi-Fi rtl8192fe (`files-6.18/drivers/net/wireless/rtl8192fe/`)

| Что | Происхождение | Лицензия |
|---|---|---|
| Драйвер «RTK-8192FE-wlan-driver 3.8.0» (2.4 ГГц RTL8197G + 5 ГГц RTL8812F) | GPL-архив TP-Link **Archer AX12 v1** (`GPL_AX12v1`, `rtl8197/backports-5.2.8-1/drivers/net/wireless/realtek/rtl8192fe`), получен через [tobyw121/Openwrt_RTL](https://github.com/tobyw121/Openwrt_RTL) (`e6d68e90`) | 865 файлов с шапкой GPL, 174 — mbed TLS (Apache-2.0, ARM), 407 без шапки (сгенерированные таблицы, HAL) |
| Перенос на ядро 6.18 (таймеры, DMA, proc_ops, WEXT…) | **этот проект** | GPL-2.0 |
| Части halmac/phydm 8812F, sha256 | [sowarden/hh71vm-openwrt](https://github.com/sowarden/hh71vm-openwrt), [Putpocket/iptime-a2004mu-openwrt](https://github.com/Putpocket/iptime-a2004mu-openwrt) | GPL-2.0 |
| Бинарные прошивки радио (`data_rtl8197Gfw.c`, `data_rtl8812Ffw*.c`, `*.bin`) | поставляются в GPL-архивах производителя, не изменялись | условия производителя |

Сверка с копией у tobyw121 (`e6d68e90`, 19.10.2026): из 1880 наших файлов 1644 совпадают байт в
байт, 32 изменены (перенос на 6.18), 204 добавлены: 71 — байт в байт из hh71vm (таблицы и halmac 8812F),
остальные — сгенерированные из этих таблиц `data_*.c`, переходники `compat_*`, `rx23302.mk`,
`Kconfig.rx23302`, `config_8192cd.h`, `git_info.h`.

Официально исходники драйвера TP-Link раздаёт через GPL Code Center:
<https://www.tp-link.com/us/support/gpl-code/> (модель Archer AX12 v1).

Из драйвера убраны таблицы и прошивки чужих чипов, которые наша конфигурация не собирает
(`WlanHAL/Data/` 8192E, 8192F, 8198F, 8814A, 8814B, 8821C, 8822B, 8822C, 8881A; `data*/` 92C/92D/88E/8812/8723;
`pe/`) — ~3 МБ. Остались только 8197G и 8812F.

Из драйвера убраны Windows-утилиты Realtek из GPL-архива (`clone88xx.exe`, `MP_Precomp.exe`,
`insert_halmac_in_reg_def.exe`, `Run4WindowsOnly.bat`) — для сборки не нужны.

Удалено из дерева порта как неиспользуемое: старый драйвер `rtl8192cd` (40 МБ),
отладочные патчи DSA `rtl8365mb` (730–735) и сам DSA — свитч управляется своим кодом.

### Пакеты OpenWrt

| Компонент | Путь | Лицензия |
|---|---|---|
| iwinfo: Wireless Extensions для rtl8192cd (патчи 200–206) | `package/network/utils/iwinfo/patches/` | GPL-2.0 (как iwinfo) |
| LuCI: настройки Wi-Fi для rtl8192cd | `feeds-patches/luci-mod-network/` | Apache-2.0 (как LuCI) |
| netifd: исправление цикла чтения обработчиков | `openwrt-upstream.patch` | GPL-2.0 (как netifd); отправлено в OpenWrt: PR #25851 |
| `feeds-patches/` для пакетов из фидов | `openwrt-upstream.patch` (include/package.mk) | GPL-2.0-only |

## Доноры, которые изучались / Reference projects

- [choryuidentify/openwrt-rtl8197f](https://github.com/choryuidentify/openwrt-rtl8197f) — исходный порт RTL8197F на OpenWrt (ядро 4.14).
- [Putpocket/iptime-a2004mu-openwrt](https://github.com/Putpocket/iptime-a2004mu-openwrt) — RTL8197F + RTL8367RB на ядре 6.18; CPU-тег и аппаратный NAT — **изучались** (идеи CPU-тега для router mode), код не копировался: наш hw NAT — порт ADCDS.
- [sowarden/hh71vm-openwrt](https://github.com/sowarden/hh71vm-openwrt) — RTL8197FS + RTL8812FE (наша пара радио), halmac 1.6.6.6.
- [tobyw121/Openwrt_RTL](https://github.com/tobyw121/Openwrt_RTL) — Xiaomi RD05 (RTL8197FH + RTL8367D, плата-близнец), исходники из GPL-архивов.
- [ADCDS/openwrt-dlink-dir842-r1](https://github.com/ADCDS/openwrt-dlink-dir842-r1) — D-Link DIR-842, 802.11w в старом драйвере.
  **Источник аппаратного NAT (0.2.0):** реверс-инжиниринг ASIC-таблиц RTL865xC/8197F и протокола NAPT
  (`rtl865x_asichal.c`, `rtl819x_hwnat.c`, SPDX GPL-2.0). Взято ~2700 строк; наш перенос — не копия
  байт в байт, изменения: API ядра (`ndo_setup_tc`/`TC_SETUP_FT`/`FLOW_CLS_REPLACE|DESTROY|STATS` вместо
  downstream `ndo_flow_offload`), VID 8/9 и раскладка гнёзд этой платы, ARP-окно подключённой WAN-сети,
  `dstVid` в nexthop = индекс netif (в доноре совпадал с VID случайно), без PPPoE. Подробности — в истории
  коммитов ветки (bf613f0…386bd37).
  Наш проект — GPL-2.0-only, донор — GPL-2.0: совместимо; заголовки SPDX в наших файлах сохранены.
