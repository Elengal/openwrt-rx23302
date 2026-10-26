# Аппаратный NAT RTL8197F: план переноса / RTL8197F hardware NAT: porting plan

> **Черновик к 0.2.0. Не сливать.** На нашей плате ничего из этого ещё не проверялось.
> Всё, что помечено «у донора», проверено на его железе (iptime A2004MU) и взято из его кода;
> всё, что помечено «гипотеза» или «проверить», — нет.
> **RU** ниже, **EN** — в конце.

## Источник

Донор — [Putpocket/iptime-a2004mu-openwrt](https://github.com/Putpocket/iptime-a2004mu-openwrt),
коммит `cd3d0823ba21e8122b12f58c505b99576e75ef35` (26.08.2026), GPL-2.0-only (совместимо с нашей лицензией).
Весь код — в одном патче `patches/openwrt/0001-iptime-a2004mu-rtl8197f-support.patch`
(sha256 `9b3f14e17924b152d1a06c99d5648cae02eb2b5c21d51bbafa2ab1139d7c54ef`),
драйвер внутри него — `target/linux/rtl819x/files/drivers/net/ethernet/realtek/rtl8197f_eth.c` (3450 строк).
Ниже функции донора названы по имени, номера строк не даются: в патче они плавают.

Если у вас в `donors/` лежит другая ревизия — сверьте sha256 патча; при расхождении этот план
надо перечитать против вашей копии.

Ссылки на наш код — по ветке `main` (`394f2c9`), файл
`openwrt/target/linux/rtl819x/files-6.18/drivers/net/ethernet/rtl819x_eth.c`.

## Что умеет донор

- Подключение к ядру штатное: nftables flowtable с `flags offload`, `ndo_setup_tc(TC_SETUP_FT)`
  (`rtl_setup_tc`, `rtl_setup_ft_cb`). Галочка «Аппаратное ускорение» в LuCI (`flow_offloading_hw`)
  включает его без вендорских хуков в conntrack.
- Только IPv4 TCP/UDP и только SNAT (LAN → WAN и ответ). PPPoE, DNAT (проброс портов), IPv6,
  коллизии хеша — остаются программными.
- Не больше 64 потоков (`RTL_HWNAT_MAX_FLOWS`), LAN только /24, один внешний адрес, один шлюз.
- Транзакционно: перед записью каждой строки таблицы сохраняет старую, после записи читает обратно и
  сравнивает, при ошибке откатывает всё (`rtl_hwnat_base_write`, `rtl_hwnat_base_restore`).
- Самопроверка хеша и упаковки записей при загрузке (`rtl_hwnat_selftest`).
- Отладка: снимок разбора заголовка и решения коммутатора по конкретному пакету
  (HSB/HSA, `hswatch_store`), состояние в sysfs (`hwnat_show`, `probe_show`).
- Скорость автор не мерил: «no gigabit routing, hardware-NAT throughput … is claimed».

## Чем наша плата отличается

| | донор (A2004MU) | мы (RX23302) |
|---|---|---|
| внешний свитч | RTL8367RB, DSA `rtl8365mb` | RTL8367D, swconfig `rtl8367b` |
| CPU-тег 8367 | 4 байта, приём **и передача** | 4 байта, только приём (`0x121a = 0x0239` в DTS) |
| P0GMIICR (`0xBB80414C`) | биты 25 и 26 (тег на приём и передачу) | `0x02037D57`: только бит 25 (`rtl819x_eth.c:977`) |
| PORT0_ROUTER_MODE | включён (`rtl_apply_a`) | выключен; в комментарии у нас «не заводится» (`rtl819x_eth.c:969`) |
| порты, которые видит 8197F | логические = порты 8367: LAN 0–3, WAN 4 | один внутренний порт 0, WAN/LAN различаются только меткой 802.1Q |
| куда слать кадр (dp в TX-дескрипторе) | маска порта 8367 из DSA-тега | всегда `0x01` = внутренний порт 0 (`rtl819x_tx_gnezda`, `rtl819x_eth.c:775`) |
| VLAN | обычно VLAN 1; 8 и 9 — только на время аппаратного NAT | 8 (WAN) и 9 (LAN) всегда; CPU-порт 8367 в обоих с меткой |
| egress mode портов 8367 | донор сбрасывает в ORIGINAL на время NAT | `rtl8367b` уже ставит ORIGINAL всем портам |
| Wi-Fi в flowtable | радио по умолчанию выключено | радио включено, интерфейсы rtl8192cd в `br-lan` |
| LAN | 192.168.1.0/24 | 192.168.0.0/24 |

Главное различие — **router mode**. У донора поток LAN→WAN приходит в коммутатор 8197F с одного
логического порта (LAN) и уходит в другой (WAN), хотя физически оба — один RGMII к 8367.
У нас без router mode коммутатору пришлось бы отправлять кадр обратно в тот же порт 0, в
другой VLAN. Умеет ли так L3/L4-движок 8197F, неизвестно: донор этого не проверял, ему не нужно.

Хорошая новость: сторона 8367 у нас уже в том состоянии, в которое донор её переводит для NAT
(`rtl_hwnat_ext_prepare`): VLAN 8 = WAN + CPU с меткой, VLAN 9 = LAN + CPU с меткой, egress mode ORIGINAL.

Заводская прошивка на нашей плате работает именно в router mode с логическими портами = портами 8367:
`/proc/rtl865x/vlan` давал `VID[9] member ports: 1 2 3 4 8`, `VID[1] member ports: 0`,
а `/proc/rtl865x/l2` — клиента LAN1 на порту 3 (см. комментарий у `RTL_SWCORE_BASE` в нашем драйвере).

## Таблицы коммутатора

Все таблицы — в одном окне: строка `index` таблицы `type` лежит по адресу
`0xBB000000 | type << 16 | index << 5`, восемь 32-битных слов. Читать можно прямо из окна.
Писать — через TACI, при остановленном аппаратном поиске (TLU), ровно как наша
`rtl819x_intsw_zapis_vlan` (`rtl819x_eth.c:865`) и `rtl_tbl_wr` донора:

1. `SWTCR0 (0xBB804418) |= bit 18` (STOP_TLU), ждать `bit 19` (готово);
2. слова в `TCR0..7 (0xBB804D20…)`, адрес строки в `SWTAA (0xBB804D08)`;
3. `SWTACR (0xBB804D00) = 0x09` (force write + start), ждать сброса `bit 0`;
4. снять STOP_TLU.

Какие таблицы и как их заполняет донор (раскладку битов брать из его кода, а не отсюда;
здесь — для ориентира, названия полей, где они очевидны из значений):

| type | таблица | строк | что донор пишет | формат у донора |
|---|---|---|---|---|
| 0 | L2 | 1024: 256 рядов × 4 пути; ряд = XOR шести байт MAC ^ `{00,0f,f0,ff}[fid]` | MAC шлюза (FID 1, порт WAN); MAC клиента LAN (FID 0, его порт) | `rtl_hwnat_pack_l2`, поиск места — `rtl_hwnat_l2_add` |
| 1 | ARP | индекс = последний байт IP хоста LAN (отсюда /24) | хост LAN → строка L2 клиента | слово 0: `BIT0 \| l2_index << 1 \| 31 << 11` |
| 2 | маршруты | 8, приоритет по индексу | 0: свой LAN-адрес /32 → CPU; 1: LAN /24 → через ARP; 7: по умолчанию → nexthop | `rtl_hwnat_pack_cpu_route`, `rtl_hwnat_base_start`; похоже, `w1[4:0]` = длина префикса − 1, бит 5 — валидность, `[8:6]` — действие (4 CPU, 2 ARP, 5 nexthop) |
| 4 | netif | 8 | 0: LAN, VID 9, MAC роутера в LAN, маршрутизация вкл; 1: WAN, VID 8, MAC WAN | `rtl_pack_netif` (MTU 1536) |
| 5 | внешние IP | — | 0: адрес WAN | `w1 = IP`, `w2 = BIT0` |
| 6 | VLAN | 4096 | у нас уже используется: 8 и 9 | наша раскладка в `rtl819x_intsw_zapis_vlan` совпадает с донорской |
| 9 | NAPT TCP/UDP | 1024, по хешу | на поток две строки: исходящая и входящая | `rtl_hwnat_pack_napt`, хеш `rtl_hwnat_hash` |
| 13 | nexthop | — | 0 и 1 → строка L2 шлюза | `w0 = 1 << 5 \| gw_l2_index << 11` |

NAPT — самая тонкая часть, переносить **дословно** вместе с самотестом:
- индекс исходящей строки = `hash(tcp, int_ip, int_port, rem_ip, rem_port)`,
  входящей = `hash(tcp, rem_ip, rem_port, ext_ip, ext_port)`; совпали — поток не берём;
- внешний порт в строках не хранится целиком, а раскладывается по полям (`offset`, `selIP`, `selEIdx`)
  по схеме «enhanced hash1» (`SWTCR1` бит 13); входящая строка ещё несёт контрольный хеш
  `hash(rem_ip, rem_port, 0, 0, proto | 2)`;
- занятая строка (не нули) = коллизия → поток остаётся программным.

## Регистры

Адреса — в KSEG1, как в нашем драйвере (`0xBB80xxxx` = база коммутатора `0x1B800000`).

| адрес | имя | что | донор | у нас сейчас |
|---|---|---|---|---|
| `0xBB804410` | MSCR | биты 0/1/2 — движки L2/L3/L4 | L3+L4 включает на первом потоке, выключает на последнем | не трогаем |
| `0xBB804418` | SWTCR0 | 18 STOP_TLU, 19 TLU остановлен, `[17:16]` выбор LAN (0 = по VLAN), 15 неизвестный VLAN → CPU, 14 неизвестный NAPT → CPU | выбор LAN = по VLAN при старте NAT | только биты 18/19 при записи таблиц |
| `0xBB80441C` | SWTCR1 | 13 enhanced hash1, 9 L4 4-way | hash1 вкл, 4-way выкл | не трогаем |
| `0xBB804428` | FFCR | 1 неизвестный unicast → CPU, 0 multicast → CPU | multicast → CPU | `0x3` (`rtl819x_eth.c:981`) |
| `0xBB804048` | CSCR | 4/5 — пересчёт сумм L3/L4 | оба вкл на время NAT | не трогаем |
| `0xBB804A00` | VLAN control 0 | фильтр VLAN на входе | 0 | не трогаем |
| `0xBB804A08…` | PVCR0…4 | PVID, по два порта на регистр | на время NAT: порты 0–3 → 9, 4 (WAN) → 8, CPU (8) → 9 | все → 9 (`rtl819x_intsw_pvid`) |
| `0xBB80414C` | P0GMIICR | 25 снимать тег на приёме, 26 ставить тег на передаче, 6 Conf_done; остальное — тайминги RGMII | `\| 0x06000000`, Conf_done последним | `0x02037D57` |
| `0xBB804058` | MACCR1 | бит 0 — PORT0_ROUTER_MODE | `\|= 1` | не трогаем |
| `0xBB804100` | PITCR | бит 0 — внешний интерфейс порта 0 | `\|= 1` | `1` (`rtl819x_eth.c:955`) |
| `0xBB804000` | MACCR | бит 12 | `\|= 0x1000` | не трогаем |
| `0xB8000850` | SYS | пинмукс RGMII | `(v & 0x01bfffff) \| 0xd8000000` | не трогаем, RGMII и так работает |
| `0xBB806280…` | HSB | разобранный заголовок последнего пакета | `hswatch_store` | — |
| `0xBB806200…` | HSA | решение: `0x6218` бит 8 — переслано аппаратно, `0x6220 & 0x7f` — dp, причина ухода на CPU — `0x6218 >> 18` и `0x621C & 3` | там же | — |

HSB/HSA — главный инструмент отладки: по ним видно, что коммутатор понял в пакете и почему
отдал его процессору, а не переслал сам. Перенести первым делом.

## Порядок у донора

При `ndo_open`: часы коммутатора (`CLK_MANAGE` бит 11) → `rtl_apply_a` (RGMII порта 0 + router mode,
Conf_done последним) → `rtl_apply_fwd` (VLAN 1, netif 0 без маршрутизации) → `rtl_dma_enable`.

Аппаратный NAT живёт по первому и последнему потоку:

1. **prepare** (`rtl_hwnat_prepare`, на первом потоке): сохранить и переписать VLAN 8/9 и netif 1 в 8197F,
   VLAN 8/9 и egress mode в 8367 по SMI; прочитать обратно; при расхождении всё вернуть.
2. **base_start** (`rtl_hwnat_base_start`, на первой паре правил): остановить TLU; сохранить SWTCR1, CSCR, PVCR;
   L2 шлюза → netif 0/1 → маршруты 0/1/7 → nexthop 0/1 → внешний IP → SWTCR1 (hash1) → CSCR (суммы) →
   PVCR → запустить TLU с выбором LAN «по VLAN».
3. **flow_install** (`rtl_hwnat_flow_install`): два хеша, проверка коллизии, L2 клиента, ARP,
   две строки NAPT, чтение обратно, MSCR |= L2|L3|L4.
4. **destroy**: вернуть сохранённые строки NAPT; ушёл последний поток — `rtl_hwnat_base_stop`
   (L3/L4 выкл, всё вернуть).
5. **off** (`rtl_hwnat_off`, из `ndo_stop` и sysfs): L3/L4 выкл, вернуть всё, включая 8367.

Ядро присылает два правила на соединение, по одному на направление, с разными cookie.
Донор держит исходящее в «ожидании» до 5 с (`rtl_hwnat_replace`) и ставит пару, когда приходит ответное.
Статистика (`rtl_hwnat_stats`): если у исходящей строки NAPT не обнулился возраст — поток жив,
`lastused = jiffies`; иначе flowtable через 30 с сама снимет поток.

## Какие правила пришлёт ядро нам

Проверено по исходникам 6.18 (`net/netfilter/nf_flow_table_offload.c`), не на плате.
fw4 спускает `eth0.8` и `br-lan` до нижних устройств (`resolve_lower_devices`), так что в flowtable
попадут `eth0` и `wlan*`. Правило для `eth0` будет таким:

- ключи: `META` (ingress = `eth0`), `VLAN` (vid 9 на исходящем, 8 на ответном), `BASIC`, `CONTROL`,
  `IPV4_ADDRS`, `PORTS`, для TCP ещё `TCP` с маской флагов FIN|RST;
- действия: `MANGLE eth` (MAC), `VLAN_POP`, `VLAN_PUSH` (8 или 9), `MANGLE ip4` + `MANGLE tcp/udp`
  (SNAT на исходящем, обратная подмена на ответном), `CSUM`, `REDIRECT` → `eth0`.

Разборщик донора (`rtl_ft_parse`) опирается на DSA-порты и отвергает `VLAN_*` — его надо
переписать: сторону (WAN/LAN) брать из vid, LAN-адрес роутера — через устройство VLAN 9 и его мост
(`__vlan_find_dev_deep_rcu` → `netdev_master_upper_dev_get_rcu` → адрес), порт клиента LAN —
из L2-таблицы 8197F (в router mode она учится сама, как у стока) или из своего кэша MAC → spa на приёме.

**Wi-Fi.** fw4 проверяет `flags offload` сразу на всех устройствах (`nft_try_hw_offload`, `nft -c`),
а ядро для устройства без `ndo_setup_tc` и без indirect-обработчика возвращает EOPNOTSUPP
(`flow_indr_dev_setup_offload`). У rtl8192cd `ndo_setup_tc` нет, поэтому без доработки fw4 молча
останется на программном ускорении. Решение — из нашего драйвера зарегистрировать indirect-обработчик
(`flow_indr_dev_register`), который привязывает flowtable к любому такому устройству, а правила
для чужих устройств отвергает. Так flowtable создаётся, потоки Wi-Fi идут программно, проводные — аппаратно.

## План по этапам

### Этап 0. Перед аппаратным NAT

- PR #2 (проверка контрольных сумм на приёме) — проверить на плате.
- Замерить исходную скорость: iperf3 WAN↔LAN через роутер, `top` (sirq), ускорение выключено / программное.
  Без этих цифр нельзя будет сказать, дал ли аппаратный NAT что-нибудь.

### Этап 1. Router mode — опыт без перепрошивки

Цель — понять, заводится ли router mode у нас вообще. Нужны UART (или готовность выключить питание)
и **WAN, подключённый к тестовому ПК с tcpdump**, а не к провайдеру: VLAN 9 внутреннего коммутатора
сейчас содержит порт 0, и в router mode широковещание LAN может уйти в WAN. Откат — перезагрузка,
во флеш ничего не пишется.

```sh
# что сейчас
for a in 0xbb80414c 0xbb804058 0xbb804100 0xbb804000; do echo "$a $(devmem $a 32)"; done
cat /proc/rtl819x_gnezda /proc/rtl819x_spa

# в router mode dp = маска гнёзд 8367: WAN = порт 0, LAN = порты 3 и 4
echo "01 18" > /proc/rtl819x_gnezda

# порядок донора (rtl_apply_a), наши тайминги RGMII не трогаем
devmem 0xbb80414c 32 $(( $(devmem 0xbb80414c 32) | 0x06000000 ))   # теги на приём и передачу
devmem 0xbb804058 32 $(( $(devmem 0xbb804058 32) | 0x1 ))          # PORT0_ROUTER_MODE
devmem 0xbb804100 32 $(( $(devmem 0xbb804100 32) | 0x1 ))          # PITCR
devmem 0xbb804000 32 $(( $(devmem 0xbb804000 32) | 0x1000 ))       # MACCR бит 12
devmem 0xbb80414c 32 $(( $(devmem 0xbb80414c 32) | 0x40 ))         # Conf_done — последним
```

Что смотреть:
- пинг и DHCP с LAN1 и LAN2;
- `cat /proc/rtl819x_spa`: приходят ли кадры с `spa` 0, 3, 4;
- `tcpdump -e` на клиенте LAN: приходят ли кадры с меткой 802.1Q (если да — драйверу придётся снимать
  метку перед отправкой: порт теперь задаёт dp, метка лишняя);
- tcpdump на тестовом WAN: нет ли там кадров из LAN.

Если не заводится — попробовать так же с донорскими таймингами P0GMIICR (`(v & 0xfff3ffe8) | 0x000c0005`)
и пинмуксом `0xB8000850`, и только потом переходить к варианту без router mode (ниже).

### Этап 2. Router mode в драйвере

- `rtl819x_intsw_nastroit` (`rtl819x_eth.c:928`): порядок как в этапе 1; маски `01 18` по умолчанию.
- VLAN внутреннего коммутатора — на логических портах: 9 = {3, 4, CPU}, 8 = {0, CPU}
  (сейчас 9 = `0x11F`, порты 0–4, `RTL_LAN_PORTY`). PVID: 0 → 8, 3 и 4 → 9.
- Снимать 802.1Q перед отправкой, если этап 1 покажет, что 8367 её не снимает.
- Повторить проверку «LAN не утекает в WAN» tcpdump'ом — README это обещает.
- Это изменение само по себе полезно и без NAT: кадр уходит ровно в нужное гнездо, как у стока.

### Этап 3. Одна запись руками

Отладочный вход (`/proc/rtl819x_hwnat` на запись): поставить базовые таблицы и одну пару NAPT для
заранее известного потока (например, iperf3 UDP с фиксированными портами) и смотреть HSB/HSA:
переслал ли коммутатор пакет сам и с какими полями. Здесь же проверить:
- работает ли перенос `rtl_hwnat_hash`/`rtl_hwnat_pack_napt` без изменений (самотест + живой поток);
- что делает железо с FIN/RST: ядро просит не трогать такие пакеты (маска флагов в правиле), донор это
  не проверяет. Если железо пересылает их само, conntrack не увидит закрытия, поток доживёт до тайм-аута.

### Этап 4. flowtable

- `ndo_setup_tc(TC_SETUP_FT)` + `NETIF_F_HW_TC` на `eth0`; indirect-обработчик для Wi-Fi и прочих.
- Разбор VLAN-правил (см. выше), пары правил, установка/снятие/статистика — по образцу донора.
- Всё, что не подходит (PPPoE, IPv6, DNAT, LAN не /24, второй WAN-адрес, коллизия, > 64 потоков), — тихо
  в программный путь (`-EOPNOTSUPP`).
- `ndo_stop` и смена адреса WAN → полный откат (`rtl_hwnat_off`).
- `flow_offloading_hw` по умолчанию **не** включать.

### Этап 5. Проверка перед релизом

- `grep HW_OFFLOAD /proc/net/nf_conntrack` — потоки действительно в железе;
- iperf3 TCP и UDP WAN↔LAN: скорость и sirq против этапа 0;
- 64+ одновременных потоков, коллизии, истечение потоков, `wifi` / `ifup wan` / смена DHCP-адреса,
  `/etc/init.d/firewall restart`, выключение галочки — всё возвращается в исходное состояние
  (донор сравнивает с сохранёнными значениями — так же);
- Wi-Fi-клиенты работают (программно), IPTV/мультикаст не сломан;
- суточный прогон.

### Запасной вариант: без router mode

Если router mode у нас не заведётся: оставить всё на внутреннем порту 0 и VLAN 8/9 (FID 1 и 0 у нас уже
разведены), netif по VID, и проверить на этапе 3, отправляет ли L3/L4-движок кадр обратно в порт 0 в
другом VLAN. Клиенты LAN тогда все «живут» на порту 0, порт в L2 = 0. Шансы неизвестны.

## Ограничения первой версии

IPv4 TCP/UDP SNAT, один WAN-адрес, LAN /24, до 64 потоков, только провод WAN↔LAN.
Не ускоряются: PPPoE (в коммутаторе есть таблица PPPoE, донор её не трогал — следующий шаг), IPv6,
проброс портов, Wi-Fi (в SDK для этого есть «расширенные порты», поля `extspa`/`dp_ext` в дескрипторах,
но это переделка Wi-Fi-драйвера). Ускоренные потоки не видят SQM/QoS и учёт трафика.

## Открытые вопросы (проверить на плате)

1. Заводится ли router mode с нашими таймингами RGMII (этап 1)?
2. Какие `spa` приходят в router mode — номера портов 8367 (0, 3, 4)?
3. Снимает ли 8367D метку 802.1Q с кадров, направленных CPU-тегом?
4. Учит ли 8197F MAC клиентов на логических портах (как сток: `mbr(3)`)?
5. Пересылает ли железо FIN/RST само?
6. Без router mode: разворачивает ли L3/L4 кадр в тот же порт 0?
7. Не мешает ли включённый L3/L4 IPTV (мультикаст, IGMP)?

## Скелет кода в этом PR

Включается только опцией ядра `CONFIG_NET_RTL819X_HWNAT` (по умолчанию выключена, в `config-6.18`
явно `is not set`), так что обычная сборка по поведению не меняется: вместо функций — пустые
заглушки, размер кода `rtl819x_eth.o` тот же.

- `rtl819x_hwnat.h` / `rtl819x_hwnat.c` — типы таблиц, адреса, чтение/запись таблиц;
  `ndo_setup_tc(TC_SETUP_FT)`, indirect-обработчик для прочих устройств, разбор VLAN-правил.
  **В железо ничего не пишется**: каждое правило разбирается, считается и отвергается (`-EOPNOTSUPP`),
  так что все потоки остаются программными.
- `/proc/rtl819x_hwnat` — сколько правил пришло, сколько разобрано, последнее разобранное правило.
  Это уже полезно: на плате видно, какие правила реально присылает ядро (вопросы раздела выше),
  до того как писать хоть одну таблицу.

---

## English

**Draft for 0.2.0, do not merge; nothing here has been tested on the RX23302.**

Source: [Putpocket/iptime-a2004mu-openwrt](https://github.com/Putpocket/iptime-a2004mu-openwrt) at `cd3d0823`
(GPL-2.0-only), driver `rtl8197f_eth.c` inside `patches/openwrt/0001-iptime-a2004mu-rtl8197f-support.patch`
(sha256 `9b3f14e1…7c54ef`). It offloads IPv4 TCP/UDP SNAT (max 64 flows, /24 LAN, one WAN address) through the
standard nftables flowtable (`TC_SETUP_FT`), transactionally, with a table self-test.

The main difference is **router mode**: the donor runs the 8197F switch with hardware CPU tags in both directions
and PORT0_ROUTER_MODE, so the switch sees the RTL8367 ports as logical ports and a flow enters and leaves on
different ports. We tag only on RX and send everything through internal port 0, telling WAN from LAN by 802.1Q
tag. The stock firmware on our board does run in router mode, and our RTL8367D side (VLAN 8/9 with tagged CPU,
egress mode ORIGINAL) already matches what the donor sets up for NAT.

Tables used (window `0xBB000000 | type << 16 | index << 5`, written via TACI with the TLU stopped): L2 (0), ARP (1),
routes (2), netif (4), external IP (5), VLAN (6), NAPT (9), next hop (13). Key registers: MSCR `0xBB804410`
(L2/L3/L4 engines), SWTCR0/1 `0xBB804418/441C`, CSCR `0xBB804048`, PVCR `0xBB804A08…`, router mode via
P0GMIICR `0xBB80414C` bits 25/26/6, MACCR1 `0xBB804058` bit 0, PITCR `0xBB804100`, MACCR `0xBB804000` bit 12;
debugging via HSB/HSA `0xBB806280/0xBB806200`.

Kernel 6.18 will hand us VLAN-keyed rules on `eth0` with `VLAN_POP`/`VLAN_PUSH`, so the donor's DSA-based parser
must be rewritten. fw4 tests hardware offload on all lower devices at once; the rtl8192cd Wi-Fi netdevs have no
`ndo_setup_tc`, so we need an indirect block callback or fw4 silently falls back to software offload.

Stages: 0) measure, RX checksum (PR #2); 1) router mode by `devmem`, no flashing; 2) router mode in the driver
(also fixes per-port TX like stock); 3) one hand-made NAPT entry, checked with HSB/HSA; 4) flowtable integration,
off by default; 5) tests. Fallback without router mode: hairpin on port 0 across VLANs, untested.

The skeleton in this PR is behind `CONFIG_NET_RTL819X_HWNAT` (off by default): it binds the flowtable, parses
and counts rules in `/proc/rtl819x_hwnat`, and rejects all of them, so nothing is offloaded yet.
