# Восстановление / Recovery

## Разметка флеша (16 МБ, XM25QH128C)

| Смещение | Размер | Раздел | Что там |
|---|---|---|---|
| 0x000000 | 0x030000 | boot | загрузчик Realtek — **не трогать никогда** |
| 0x030000 | 0x2D0000 | kernel | наш образ (ядро + rootfs целиком пишутся отсюда) |
| 0x300000 | 0x300000 | rootfs | |
| 0x600000 | 0x1A0000 | rootfs_data | настройки OpenWrt (overlay) |
| 0x7A0000 | 0x060000 | cfg | настройки устройства (MAC и т.п.), только чтение |
| 0x800000 | 0x800000 | stock | **второй слот: заводская прошивка остаётся нетронутой** |

Заводская прошивка — dual-image: два одинаковых слота по 8 МБ. Мы пишем только в первый.

## Если OpenWrt не загружается

1. Подключите UART (38400 8N1), включите роутер, жмите Esc до приглашения `<RealTek>`.
2. Прошейте образ заново: `tools/flash-uart.py` (он сам ловит загрузчик).

Загрузчик живёт в отдельной области и прошивкой не затирается: пока он печатает
`<RealTek>`, устройство восстанавливается.

## Вернуть заводскую прошивку

> ⚠️ Процедура возврата на сток ещё **не проверена на железе**.
> Идея: скопировать второй слот (0x830000…) в первый командами загрузчика
> `FLR` (флеш → ОЗУ) и `FLW` (ОЗУ → флеш). Точные команды и длины будут добавлены
> после проверки.

## Правила загрузчика

- Команды только **с полными аргументами**. `FLR` без аргументов вешает загрузчик до сброса питания.
- На вопрос `(Y)es, (N)o` — **`Y` и Enter**. Без Enter загрузчик молча ждёт бесконечно (выглядит как зависание).
- Пока `FLW`/`FLR` печатают точки — ничего не отправлять.
- Слово `<RealTek>` в выводе ещё не значит «готово» — ждите `Successed` или тишину.

---

## English

Flash layout: see the table above. Our image lives in the first 8 MB slot; the **stock firmware
in the second slot (0x800000) is left intact**. The bootloader (0x0–0x30000) is never written.

If OpenWrt does not boot: connect UART, press Esc during power-on until `<RealTek>`, re-flash
with `tools/flash-uart.py`.

Returning to stock: **not yet verified on hardware** (planned: copy slot 2 to slot 1 with the
bootloader's `FLR`/`FLW`). Never send bootloader commands without arguments.
