# Восстановление и возврат на заводскую прошивку / Recovery and return to stock

> **RU** ниже, **EN** — в конце.

## Разметка флеша (16 МБ, XM25QH128C)

| Смещение | Размер | Раздел | Что там |
|---|---|---|---|
| 0x000000 | 0x030000 | boot | загрузчик Realtek и заводские настройки платы (MAC, калибровка) — **не трогать никогда** |
| 0x030000 | 0x2D0000 | kernel | ядро OpenWrt |
| 0x300000 | 0x300000 | rootfs | корневая система OpenWrt |
| 0x600000 | 0x1A0000 | rootfs_data | настройки OpenWrt (overlay) |
| 0x7A0000 | 0x060000 | cfg | раздел настроек заводской прошивки, только чтение |
| 0x800000 | 0x800000 | stock | **банк 2: заводская прошивка, OpenWrt его не трогает** |

Заводская прошивка — dual-image: два банка по 8 МБ. OpenWrt пишется только в банк 1,
заводская копия в банке 2 (ядро с 0x830000) остаётся нетронутой.

## Как загрузчик защищает от кирпича

При каждом включении загрузчик проверяет банк 1 (заголовок ядра `KE32` и сумму rootfs).
Если банк 1 негоден, он **сам копирует банк 2 в банк 1** и загружает заводскую прошивку:

```
Checking bank2...
Flash read from 00830000 to 80800000 with 007D0000 bytes ?
Flash Program from 80800000 to 00030000 with 007D0000 bytes ?
Copy bank2 to bank1 successfully!
Jump to image start=0x80a00000...
Linux version 3.10.90-pre5 (abudeev@honor) ...
```

*(реальный лог с RX23302, 20.10.2026)*

Поэтому:
- оборванная прошивка (выключили свет во время sysupgrade) — роутер поднимется на **заводской**
  прошивке, а не превратится в кирпич;
- возврат на сток делается одной командой (ниже).

## Вернуть заводскую прошивку (проверено на железе 20.10.2026)

Из OpenWrt (SSH или UART):

```sh
mtd erase kernel && reboot
```

Загрузчик не найдёт ядра в банке 1, скопирует заводскую прошивку из банка 2 и загрузит её
(копирование ~1–2 мин, потом обычная загрузка стока). UART для этого **не нужен**.

Проверено: после возврата банк 1 (0x30000–0x7A0000) **побайтно совпадает** с заводским дампом
флеша, загрузчик и заводские настройки платы не тронуты.

> Заводская прошивка поднимет свои настройки Ростелекома (раздел `cfg` OpenWrt не трогает).
> Хотите вернуть ещё и заводские настройки — сделайте сброс кнопкой Reset на стоке.

## Если OpenWrt не загружается

1. Подключите UART (38400 8N1), включите роутер, жмите Esc до приглашения `<RealTek>`.
2. Прошейте образ заново: `tools/flash-uart.py` (он сам ловит загрузчик).

Если загрузчик сам поднял заводскую прошивку (см. лог выше) — OpenWrt ставится заново
так же, через UART: [install-uart.md](install-uart.md). Это проверено: сток → OpenWrt.

Загрузчик живёт в отдельной области и прошивкой не затирается: пока он печатает
`<RealTek>`, устройство восстанавливается.

## Правила загрузчика

- Команды только **с полными аргументами**. `FLR` без аргументов вешает загрузчик до сброса питания.
- На вопрос `(Y)es, (N)o` — **`Y` и Enter**. Без Enter загрузчик молча ждёт бесконечно (выглядит как зависание).
- Пока `FLW`/`FLR` печатают точки — ничего не отправлять.
- Слово `<RealTek>` в выводе ещё не значит «готово» — ждите `Successed` или тишину.

---

## English

**Flash layout:** see the table above. OpenWrt lives in bank 1; the **stock firmware in bank 2
(0x800000, kernel at 0x830000) is never written**. The bootloader (0x0–0x30000) is never written.

**Brick protection:** on every boot the bootloader validates bank 1 (`KE32` kernel header and
rootfs checksum). If bank 1 is invalid it **copies bank 2 over bank 1 by itself** and boots the
stock firmware (`Copy bank2 to bank1 successfully!`, real log above). An interrupted sysupgrade
therefore ends in stock firmware, not a brick.

**Return to stock (verified on hardware 2026-10-20):** from OpenWrt run

```sh
mtd erase kernel && reboot
```

The bootloader restores the stock firmware from bank 2 (~1–2 min) and boots it. No UART needed.
After the restore bank 1 matched the original stock flash dump byte for byte.

**OpenWrt does not boot:** connect UART, press Esc during power-on until `<RealTek>`, re-flash
with `tools/flash-uart.py`. Going stock → OpenWrt again is the normal UART install
([install-uart.md](install-uart.md)), also verified.

Never send bootloader commands without arguments; answer `(Y)es, (N)o` with `Y` **and** Enter.
