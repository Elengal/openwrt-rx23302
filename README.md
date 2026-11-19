# OpenWrt для Ротек (Rotek) RX-23302 / ELECTRA RX23302

[![Release](https://img.shields.io/github/v/release/Elengal/openwrt-rx23302?display_name=release&label=release)](https://github.com/Elengal/openwrt-rx23302/releases)
[![Downloads](https://img.shields.io/github/downloads/Elengal/openwrt-rx23302/total?label=downloads)](https://github.com/Elengal/openwrt-rx23302/releases)
[![License](https://img.shields.io/github/license/Elengal/openwrt-rx23302?label=license)](LICENSE)

[Русский](#русский) · [English](#english)

> Текущий релиз — **0.2.3**: готовые образы в [Releases](https://github.com/Elengal/openwrt-rx23302/releases).
> Новое в 0.2.3: **LuCI больше не «теряет» 5 ГГц и 11ac** — драйвер выдавал план каналов текущего режима
> за возможности чипа, и любое сохранение в LuCI (даже переименование SSID) могло навсегда запереть
> 5 ГГц-радио в 2.4 ГГц (самозамок, воспроизведён дважды). Теперь SIOCGIWRANGE отдаёт регуляторные планы
> всех диапазонов чипа (8812F — 2.4+5, 8197G — 2.4): выбор 5g/AC/VHT80 в LuCI доступен всегда.
> Новое в 0.2.2: **скорость Wi-Fi исправлена** — драйвер терял кадры A-MSDU на приёме (TCP-паузы, аплинк 53–106 Мбит/с);
> починено (RX-буферы 4600 + A-MSDU выключен как оптимизация на этом CPU): **DOWN 436–480, UP 423–485 Мбит/с**
> (AP-мост, канон 300 МиБ, клиент у точки; заводская ~492/372). **L2-таблицы**: Wi-Fi-станции → статические
> CPU-ловушки (стабильность). **PS-фикс** Wi-Fi (MACID не усыпляется + детектор TX-DMA-hang).
> **Светодиод 2.4G починен** (регрессия с 0.2.0) + **настройка ламп в LuCI** (Система → LED Configuration):
> Status и WiFi 2.4G, работают из коробки после прошивки, переживают перезагрузку.
> Новое в 0.2.1: **свой MAC у каждой коробки** — eth0 читает заводской адрес из таблицы hwconfig во флеше
> (в 0.2.0 все прошитые роутеры делили один `02:E0:4C:81:96:C1` — коллизии у соседей по сегменту);
> VLAN и Wi-Fi наследуют его же. Аппаратному NAT корректный MAC обязателен.
> Новое в 0.2.0: **аппаратный NAT RTL8197F** — включается галкой LuCI «Flow offloading: Hardware»
> (тестовый режим, по умолчанию — программный офлоад); честные замеры скорости — [ниже](#скорость-честные-замеры).
> Новое в 0.1.2: **установка без разборки и UART** — кнопкой Reset и TFTP ([docs/install-tftp.md](docs/install-tftp.md)),
> кнопка Reset в OpenWrt, светодиоды портов и Wi-Fi 2,4 ГГц.

---

## Русский

Своя прошивка OpenWrt (ядро Linux 6.18) для роутера **Ротек RX-23302** (Rotek RX-23302),
который раздаёт Ростелеком. Заводская прошивка называет его **ELECTRA-RX23302** — это одно и то же
устройство. Своя — потому что такого порта нигде нет: SoC Realtek RTL8197F в официальном
OpenWrt не поддерживается.

> ⚠️ **Проверено только на RX-23302 с SoC RTL8197FH-VG** (заводская прошивка
> `ELECTRA-RX23302 V1.24.137`). Ревизия **RX-23302R** (по сообщениям — RTL8197FN) и другие
> модели Ротек **не проверялись**: прошивка может не загрузиться. Проверка до прошивки: лог загрузки
> по UART начинается с `Realtek RTL8197F-VG boot code`. Другая ревизия — на свой риск.

### Железо

| | |
|---|---|
| SoC | Realtek RTL8197FH-VG, MIPS 24Kc, 1 ГГц |
| ОЗУ / флеш | 64 МБ DDR2 / 16 МБ SPI NOR (XM25QH128C) |
| Wi-Fi 2,4 ГГц | встроенный RTL8197G, 2×2 802.11n |
| Wi-Fi 5 ГГц | RTL8812FE (PCIe), 2×2 802.11ac, до 866 Мбит/с |
| Коммутатор | RTL8367D (chip id 0x6642, ver 0x0030): 1 WAN + 2 LAN, гигабит |
| Консоль | UART 38400 8N1 |

### Что работает

| | |
|---|---|
| ✅ Wi-Fi 2,4 и 5 ГГц | WPA2, 802.11w, до 3 гостевых сетей на радио, мощность передачи, всё — из LuCI |
| ✅ Режим роутера | WAN по DHCP, NAT, межсетевой экран, LAN 192.168.0.1 |
| ✅ Гнёзда | WAN, LAN1, LAN2 — проверены проводом на 0.1.0 |
| ✅ Разделение WAN/LAN | VLAN 8/9 во внешнем коммутаторе, утечек LAN в WAN нет (проверено tcpdump) |
| ✅ Коммутатор в LuCI | драйвер OpenWrt `rtl8367b` (swconfig) вместо закрытого SDK Realtek: вкладка «Сеть → Коммутатор», VLAN, состояние портов |
| ✅ Программное ускорение NAT | flow offloading включён по умолчанию |
| ✅ Аппаратный NAT (тест) | NAPT-движок RTL8197F: галка LuCI «Flow offloading: Hardware» (или `uci set firewall.@defaults[0].flow_offloading_hw=1`); 857–929 Мбит/с, CPU 90–100% idle. Только IPv4 TCP/UDP; IPv6 и ICMP — программно; таблица 1024 строки |
| ✅ LuCI на русском | |
| ✅ Обновление из LuCI / sysupgrade | `…-squashfs-sysupgrade.bin`, с сохранением настроек и без; оборванное обновление — не кирпич (см. ниже) |
| ✅ Возврат на заводскую прошивку | одной командой, без UART — [docs/recovery.md](docs/recovery.md) |
| ✅ Установка без UART | кнопка Reset при включении + TFTP — [docs/install-tftp.md](docs/install-tftp.md) |
| ✅ Кнопка Reset | коротко — перезагрузка, держать 5+ с — сброс настроек |
| ✅ MAC устройства | eth0 читает заводской адрес из таблицы hwconfig во флеше (у каждой коробки свой); VLAN и Wi-Fi наследуют; стабильный между перезагрузками |
| ✅ Светодиоды | POWER (аппаратно), WAN/LAN1/LAN2 (линк и активность, коммутатор); 5G — горит и мигает от трафика (HW-движок 8812F); **2.4G — горит и мигает от трафика (0.2.2)**; Status — ровно горит. **Настройка Status и 2.4G в LuCI** (Система → LED Configuration) |
| ⚠️ Режим точки доступа | работает, настраивается вручную |

### Скорость: честные замеры

Один и тот же стенд (ПК ↔ RX23302), один инструмент (`iperf3`, TCP, 3–8 прогонов по 10 с),
один и тот же провод — цифры сравнимы. Замеры сделаны на работающем роутере, во время замеров через него
шёл обычный домашний трафик (включая ТВ-приставку), поэтому вилки шире, чем на пустом стенде.

| Прошивка | NAT | LAN→WAN | WAN→LAN | CPU | Когда замерено |
|---|---|---|---|---|---|
| Заводская `V1.24.137` | программный (аппаратный не включается: **0 строк NAPT**) | 724–748 | 791–809 | ~40% idle | 27–28.10.2026 |
| Наша, программный офлоад | программный | 850–876 | 861–876 | 100% sirq | 29.10.2026 |
| Наша, **аппаратный NAT** (тест) | **железо** | **857–900**, изредка ~430–440 | **846–863** (на чистом стенде до 929) | **90–100% idle** | 29.10.2026 |
| Наша **0.2.1**, программный офлоад | программный | 759–890 | — (первичные прогоны) | — | 30.10.2026 |
| Наша **0.2.1**, **аппаратный NAT** + стресс | **железо** | 845–892; 30 мин × 8 потоков = **145 ГБ без сбоев** | — | 90–100% idle | 30.10.2026 |

**Что это значит.** Заводская прошивка на этом железе не включает аппаратный NAT: 0 строк в NAPT-таблице,
и весь гигабит идёт через один процессор (724–809 при ~40% простоя). Наш порт ставит эти строки и отдаёт
трафик коммутатору: **857–929 Мбит/с при свободном процессоре** — на том же стенде, с тем же `iperf3`
и той же методикой. Гигабит, за который платят, проходит почти целиком.

**Честно о просадках.** Аппаратный NAT — тестовый режим: таблица NAPT на 1024 строки, и при коллизии
отображения поток падает в программный путь (в замерах это редкие прогоны ~430–440 Мбит/с). Со временем
эти строки освобождаются, и скорость возвращается. Смена режима NAT применяется перезагрузкой — включение
на ходу нестабильно. Wi-Fi и реальный провайдер дадут меньше, чем проводной стенд.

### Скорость Wi-Fi (0.2.2, честные замеры)

Режим точки доступа (мост), клиент Redmi Note 11 Pro+ рядом с точкой (−12…−33 dBm), 5 ГГц канал 36 VHT80 2×2,
канон 300 МиБ (curl/iperf3, TCP). Это **Wi-Fi throughput**, не проводной NAT (таблица выше — про NAT-роутинг).

| Прошивка | DOWN | UP |
|---|---|---|
| Заводская `V1.24.137` | ~492 | ~372 |
| Наша **0.2.2** | **436–480** | **423–485** |

До 0.2.2 аплинк падал до 53–106 Мбит/с с TCP-паузами 5–8 с: драйвер терял кадры A-MSDU на приёме (сборка
субфреймов по кускам голодала под нагрузкой). Починено: RX-буферы 4600 (кадр A-MSDU помещается целиком,
сборка по кускам не нужна) + A-MSDU выключен как оптимизация (на этом MIPS накладные расходы агрегации на CPU
больше выигрыша от неё). Ошибок приёма нет. 2.4G: путь чистый, 0 ошибок (клиент со старым 11n Wi-Fi даёт
~27–33 Мбит/с — потолок клиента, не точки). 0.2.3 путь данных не меняет — замеры 0.2.2 остаются в силе.

### Что не работает или не проверено

- **Аппаратный NAT — тестовый режим (0.2.1-test)**: не включён по умолчанию; IPv6 и ICMP через него не идут;
  при коллизии строк NAPT возможны просадки (см. «Скорость: честные замеры»); смена режима — через перезагрузку.
- **Режим точки доступа (мост)**: аппаратный NAT там не применим — сервис сам отказывается его включать,
  сеть не ломается.
- Драйвер Wi-Fi много пишет в журнал ядра.
- Консольный `iwinfo wlan0 info` печатает справку (его ucode-модуль знает только nl80211); LuCI и `ubus call network.wireless status` работают.
- **Обновление через LuCI/sysupgrade на этой плате может сорваться по OOM (0.2.3, known issue):** procd
  копирует весь rootfs в RAM (ramroot для pivot_root), вендорский образ не влезает в свободную память
  (~8.5 MB после размещения образа в /tmp). Симптом: после «прошить» роутер полулетаргичен (сеть мертва,
  флеш не тронут), лечение — передёрнуть питание. Надёжные пути: TFTP через загрузчик
  ([docs/install-tftp.md](docs/install-tftp.md)) или `mtd write` с бэкапом конфига. Планируем починить.
- **DHCP-сервер на LAN включён по умолчанию:** при установке точкой доступа (мост) в сеть со своим DHCP
  роутер конкурирует за выдачу адресов — клиент может получить шлюз на RX и потерять интернет.
  Для AP-режима: `dhcp.lan.ignore=1` (+ `ra`/`dhcpv6` = disabled), в LuCI: Сеть → Интерфейсы → LAN →
  DHCP-сервер → «Игнорировать интерфейс». В домовых сборках проекта уже выключено.
- **Двойники** — Transservice **TS-7322** (Ростелеком), Transservice **TS-7022** (МТС), **Билайн RX-22302** —
  по описаниям то же железо, но **не проверялись**. Сначала сравните первую строку лога загрузки по UART
  (у нас `Realtek RTL8197F-VG boot code`). Могут отличаться настройка платы, гнёзда свитча и Wi-Fi.
  Загрузчик и заводская прошивка во втором слоте при этом не затираются. Пробовали — расскажите в
  [Issues](https://github.com/Elengal/openwrt-rx23302/issues).

### Коммутатор: для любопытных

```sh
swconfig dev switch0 show              # порты, VLAN, счётчики
ls /sys/kernel/debug/switch/           # окно регистров: reg/val, pvid, vlan_mc, vlan_4k, mibs
```

Порты: **0 = WAN** (синее), **3 = LAN1**, **4 = LAN2**, **7 = процессор**; 1, 2, 5, 6 на плате не разведены.
По умолчанию VLAN 8 = WAN (порт 0 + CPU с тегом), VLAN 9 = LAN (порты 3, 4 + CPU с тегом).

### Установка

**Первая установка — без разборки:** зажать Reset, включить питание, через ~10 с передать по TFTP файл
**`…-squashfs-tftp-factory.bin`** на 192.168.1.6 — [docs/install-tftp.md](docs/install-tftp.md).
Запасной путь — через UART (файл `…-squashfs-full.bin`): [docs/install-uart.md](docs/install-uart.md).
Установка через заводской веб-интерфейс Ростелекома не проверялась.

**Обновление** уже установленной OpenWrt — без UART: LuCI → «Система» → «Резервная копия / Прошивка»
→ «Установить новый образ прошивки», файл **`…-squashfs-sysupgrade.bin`**. Или из консоли:

```sh
sysupgrade /tmp/openwrt-…-squashfs-sysupgrade.bin      # с сохранением настроек
sysupgrade -n /tmp/openwrt-…-squashfs-sysupgrade.bin   # начисто
```

Обновление занимает ~3 мин. Порядок записи такой, что при обрыве (выключили свет) загрузчик
поднимет заводскую прошивку из второго банка — не кирпич; OpenWrt потом ставится заново тем же TFTP.

**Вернуть заводскую прошивку:** `mtd erase kernel && reboot` — подробно в [docs/recovery.md](docs/recovery.md).

### Сборка

Linux, ~15 ГБ места, зависимости сборки OpenWrt
([список](https://openwrt.org/docs/guide-developer/toolchain/install-buildsystem)).

```sh
git clone https://github.com/Elengal/openwrt-rx23302
cd openwrt-rx23302
./build.sh            # ~1 час на 8 потоках
```

Готовые образы: `build/bin/targets/rtl819x/rtl8197f/openwrt-rtl819x-rtl8197f-electra_rx23302-squashfs-{tftp-factory,full,sysupgrade}.bin`.

`build.sh` клонирует OpenWrt на закреплённом коммите ([versions.sh](versions.sh)),
накладывает каталог [`openwrt/`](openwrt/) и [`openwrt-upstream.patch`](openwrt-upstream.patch),
закрепляет фиды и собирает.

### Откуда код

Порт собран из работы многих людей — подробно, со ссылками и лицензиями:
[docs/sources.md](docs/sources.md).

### Лицензия

GPL-2.0 для кода этого проекта — текст в [LICENSE](LICENSE); у компонентов — свои лицензии, см. [docs/sources.md](docs/sources.md).

---

## English

OpenWrt (Linux 6.18) for the **Rotek RX-23302** router (a.k.a. **ELECTRA RX23302** in the stock
firmware; distributed by Rostelecom, Russia). The Realtek RTL8197F SoC is not supported by
upstream OpenWrt; this is a standalone port.

> ⚠️ **Tested only on RX-23302 with the RTL8197FH-VG SoC** (stock firmware
> `ELECTRA-RX23302 V1.24.137`). The **RX-23302R** revision (reportedly RTL8197FN) and other Rotek
> models are **untested** and may not boot. The UART boot log must start with
> `Realtek RTL8197F-VG boot code`.

**Hardware:** RTL8197FH-VG (MIPS 24Kc, 1 GHz), 64 MB RAM, 16 MB SPI NOR, 2.4 GHz RTL8197G
2×2 n, 5 GHz RTL8812FE 2×2 ac, RTL8367D switch (1 WAN + 2 LAN, gigabit), UART 38400 8N1.

**Works:** 2.4 + 5 GHz Wi-Fi (WPA2, 802.11w, guest networks, TX power — all in LuCI), router
mode (DHCP WAN, NAT, firewall), all ports, WAN/LAN separation by VLAN (no LAN→WAN leaks,
verified with tcpdump), software flow offloading, LuCI. **Hardware NAT (test):** the RTL8197F
NAPT engine via the LuCI toggle "Flow offloading: Hardware" (or
`uci set firewall.@defaults[0].flow_offloading_hw=1`): IPv4 TCP/UDP only, IPv6 and ICMP stay in
software, 1024-row NAPT table. The switch runs on OpenWrt's GPL
`rtl8367b` swconfig driver (not the closed Realtek SDK): LuCI "Network → Switch" tab,
`swconfig dev switch0 show`, register window in `/sys/kernel/debug/switch/`. Ports: 0 = WAN,
3 = LAN1, 4 = LAN2, 7 = CPU.

**New in 0.1.2:** install **without opening the case or UART** — hold Reset at power-on and send
`…-squashfs-tftp-factory.bin` over TFTP to 192.168.1.6 ([docs/install-tftp.md](docs/install-tftp.md));
the Reset button works in OpenWrt (short press = reboot, 5+ s = factory reset); POWER/WAN/LAN1/LAN2/2.4G LEDs.

**New in 0.2.3: LuCI no longer "loses" 5 GHz / 11ac.** The driver published the current mode's channel plan
as the chip's capabilities in SIOCGIWRANGE, so saving any LuCI form (even an SSID rename) could permanently
lock a 5 GHz radio into 2.4 GHz (a self-lock, reproduced twice on the board). Now the regulatory channel
plans of every band the chip supports are reported (RTL8812F dual-band, RTL8197G 2.4-only), so the
5g / AC / VHT80 choices stay available in LuCI regardless of the current mode.

**New in 0.2.2: Wi-Fi speed fixed (A-MSDU).** The vendor driver dropped A-MSDU frames on RX (the subframe
gather assembly starved under load) → TCP stalls of 5–8 s, uplink 53–106 Mbit/s. Fixed: RX buffers raised
to 4600 (a whole A-MSDU fits, no gather needed) and A-MSDU disabled as an optimisation (on this MIPS the
aggregation CPU overhead outweighs the gain). **DOWN 436–480, UP 423–485 Mbit/s** (AP bridge, 300 MiB canon,
client next to the AP; stock ~492/372). **L2 tables:** Wi-Fi stations get static CPU traps (stability).
**Wi-Fi PS fix** (MACID not put to sleep + TX-DMA-hang detector for 8812F). **2.4G LED fixed** (regression
since 0.2.0) + **LED configuration in LuCI** (System → LED Configuration): Status and WiFi 2.4G, work out of
the box after flashing, survive reboots.

**0.2.1: per-device MAC** — eth0 reads the factory address from the on-flash hwconfig table
(all flashed units used to share one `02:E0:4C:81:96:C1` — collisions between neighbours on the
same ISP segment); VLANs and Wi-Fi inherit it. A correct MAC is required by hardware NAT.

**0.2.0 (test branch): RTL8197F hardware NAT** — experimental, enabled by a LuCI toggle; honest speed numbers below.

**0.1.1:** sysupgrade / LuCI firmware upgrade (`…-squashfs-sysupgrade.bin`, with or without
keeping settings; an interrupted upgrade falls back to stock firmware, not a brick) and a verified
return to stock: `mtd erase kernel && reboot` ([docs/recovery.md](docs/recovery.md)).

**Speed: honest numbers.** Same bench (host PC ↔ RX23302), same tool (`iperf3`, TCP, 3–8 runs of
10 s), same cable — the numbers are comparable. Measured on the live router while normal home traffic
(including a TV set-top box) was flowing, so the spread is wider than on an idle bench.

| Firmware | NAT | LAN→WAN | WAN→LAN | CPU | Measured |
|---|---|---|---|---|---|
| Stock `V1.24.137` | software (hardware NAT does **not** engage: **0 NAPT rows**) | 724–748 | 791–809 | ~40% idle | 27–28.10.2026 |
| Ours, software offload | software | 850–876 | 861–876 | 100% sirq | 29.10.2026 |
| Ours, **hardware NAT** (test) | **silicon** | **857–900**, occasionally ~430–440 | **846–863** (up to 929 on an idle bench) | **90–100% idle** | 29.10.2026 |
| Ours **0.2.1**, software offload | software | 759–890 | — (primary runs) | — | 30.10.2026 |
| Ours **0.2.1**, **hardware NAT** + stress | **silicon** | 845–892; 30 min × 8 streams = **145 GB, no failures** | — | 90–100% idle | 30.10.2026 |

**What it means.** The stock firmware does not engage hardware NAT on this hardware: zero NAPT rows —
the whole gigabit rides on one CPU (724–809 at ~40% idle). This port writes the rows and hands the
traffic to the switch: **857–929 Mbit/s with an idle CPU**, same bench, same `iperf3`, same method.
The gigabit you pay for now passes almost entirely.

**Honest caveats.** Hardware NAT is a test mode: the NAPT table has 1024 rows, and a hash collision
drops that flow to the software path — hence the rare ~430–440 Mbit/s runs; rows age out and the speed
returns. Switching the NAT mode applies via a reboot (live switching is unstable). Wi-Fi and a real ISP
link will be slower than this wired bench.

**Wi-Fi speed (0.2.2, honest numbers).** Access-point (bridge) mode, Redmi Note 11 Pro+ client next to the
AP (−12…−33 dBm), 5 GHz channel 36 VHT80 2×2, 300 MiB canon (curl/iperf3, TCP). This is **Wi-Fi throughput**,
not wired NAT (the table above is NAT routing).

| Firmware | DOWN | UP |
|---|---|---|
| Stock `V1.24.137` | ~492 | ~372 |
| Ours **0.2.2** | **436–480** | **423–485** |

Before 0.2.2 the uplink fell to 53–106 Mbit/s with 5–8 s TCP stalls (the driver dropped A-MSDU frames on RX).
Fixed: RX buffers 4600 (a whole A-MSDU fits) + A-MSDU disabled as an optimisation (aggregation CPU overhead
outweighs the gain on this MIPS). No RX errors. 2.4G: clean path, 0 errors (an old 11n client gives ~27–33
Mbit/s — the client's ceiling, not the AP's). 0.2.3 does not touch the data path — the 0.2.2 numbers stand.

**Not yet:** noisy Wi-Fi driver log, console `iwinfo <dev> info` prints usage (nl80211-only ucode module;
LuCI is fine). LEDs: POWER is hardwired (always on); WAN/LAN1/LAN2 are driven by the switch and 5G by the
8812F hardware LED engine — these are not LuCI-configurable; Status and 2.4G are (System → LED Configuration).
**Hardware NAT is a test mode (0.2.1-test):** off by default; IPv6/ICMP stay in software; NAPT row collisions
can cause dips (see "Speed: honest numbers"); switching the mode needs a reboot. In access-point (bridge)
mode hardware NAT does not apply — the service refuses it and leaves the bridge intact.

**Known issues (0.2.3):** flashing via LuCI/sysupgrade can fail with OOM on this board — procd copies the
whole rootfs into RAM (ramroot for pivot_root) and the vendor image does not fit into free memory
(~8.5 MB once the image sits in /tmp). Symptom: after "flash" the router is half-dead (network down, flash
untouched); recover by power-cycling. Reliable paths: TFTP via the bootloader
([docs/install-tftp.md](docs/install-tftp.md)) or `mtd write` with a config backup. Fix planned.
Also: the LAN DHCP server is on by default; in access-point (bridge) deployments into a network with its
own DHCP the router races for lease assignment (clients may get a gateway on the RX and lose internet).
For AP mode set `dhcp.lan.ignore=1` (and `ra`/`dhcpv6` = disabled) — LuCI: Network → Interfaces → LAN →
DHCP Server → "Ignore interface".

**Look-alikes** — Transservice **TS-7322** (Rostelecom), Transservice **TS-7022** (MTS), **Beeline RX-22302** —
reportedly the same hardware, but **untested**. Compare the first UART boot line first (ours:
`Realtek RTL8197F-VG boot code`); board setup, switch ports and Wi-Fi may differ. The bootloader and the
stock image in the second slot are not overwritten. Tried it? Please report in
[Issues](https://github.com/Elengal/openwrt-rx23302/issues).

**Install:** Reset + TFTP (`…-squashfs-tftp-factory.bin`) — [docs/install-tftp.md](docs/install-tftp.md);
fallback over UART (`…-squashfs-full.bin`) — [docs/install-uart.md](docs/install-uart.md).
**Upgrade:** LuCI → System → Backup / Flash firmware, or `sysupgrade`, with `…-squashfs-sysupgrade.bin`.
**Recovery:** [docs/recovery.md](docs/recovery.md).
**Build:** `./build.sh` (Linux, ~15 GB, OpenWrt build prerequisites).
**Sources and licenses:** [docs/sources.md](docs/sources.md); project code is GPL-2.0, see [LICENSE](LICENSE).
