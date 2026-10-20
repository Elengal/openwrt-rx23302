# OpenWrt для Ротек (Rotek) RX-23302 / ELECTRA RX23302

[Русский](#русский) · [English](#english)

> Первый релиз — **0.1.0**: готовый образ в [Releases](https://github.com/Elengal/openwrt-rx23302/releases).

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
| ✅ LuCI на русском | |
| ⚠️ Режим точки доступа | работает, настраивается вручную |

### Что не работает или не проверено

- **sysupgrade** (обновление из LuCI) — не проверен. Пока — только UART.
- **Аппаратный NAT** RTL8197F — не перенесён.
- **Возврат на заводскую прошивку** — процедура не проверена на железе (сама заводская прошивка
  во флеше не затирается).
- Драйвер Wi-Fi много пишет в журнал ядра.
- Консольный `iwinfo wlan0 info` печатает справку (его ucode-модуль знает только nl80211); LuCI и `ubus call network.wireless status` работают.
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

Только через UART: [docs/install-uart.md](docs/install-uart.md).
Если что-то пошло не так: [docs/recovery.md](docs/recovery.md).

### Сборка

Linux, ~15 ГБ места, зависимости сборки OpenWrt
([список](https://openwrt.org/docs/guide-developer/toolchain/install-buildsystem)).

```sh
git clone https://github.com/Elengal/openwrt-rx23302
cd openwrt-rx23302
./build.sh            # ~1 час на 8 потоках
```

Готовый образ: `build/bin/targets/rtl819x/rtl8197f/openwrt-rtl819x-rtl8197f-electra_rx23302-squashfs-full.bin`.

`build.sh` клонирует OpenWrt на закреплённом коммите ([versions.mk.sh](versions.mk.sh)),
накладывает каталог [`openwrt/`](openwrt/) и [`openwrt-upstream.patch`](openwrt-upstream.patch),
закрепляет фиды и собирает.

### Откуда код

Порт собран из работы многих людей — подробно, со ссылками и лицензиями:
[docs/sources.md](docs/sources.md).

### Лицензия

GPL-2.0 для кода этого проекта; у компонентов — свои лицензии, см. [docs/sources.md](docs/sources.md).

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
verified with tcpdump), software flow offloading, LuCI. The switch runs on OpenWrt's GPL
`rtl8367b` swconfig driver (not the closed Realtek SDK): LuCI "Network → Switch" tab,
`swconfig dev switch0 show`, register window in `/sys/kernel/debug/switch/`. Ports: 0 = WAN,
3 = LAN1, 4 = LAN2, 7 = CPU.

**Not yet:** sysupgrade (untested — UART only), RTL8197F hardware NAT, verified return to stock
(the stock image in flash is not overwritten), noisy Wi-Fi driver log, console `iwinfo <dev> info` prints usage (nl80211-only ucode module; LuCI is fine).

**Look-alikes** — Transservice **TS-7322** (Rostelecom), Transservice **TS-7022** (MTS), **Beeline RX-22302** —
reportedly the same hardware, but **untested**. Compare the first UART boot line first (ours:
`Realtek RTL8197F-VG boot code`); board setup, switch ports and Wi-Fi may differ. The bootloader and the
stock image in the second slot are not overwritten. Tried it? Please report in
[Issues](https://github.com/Elengal/openwrt-rx23302/issues).

**Install:** UART only — [docs/install-uart.md](docs/install-uart.md).
**Recovery:** [docs/recovery.md](docs/recovery.md).
**Build:** `./build.sh` (Linux, ~15 GB, OpenWrt build prerequisites).
**Sources and licenses:** [docs/sources.md](docs/sources.md).
