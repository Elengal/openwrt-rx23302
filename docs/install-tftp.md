# Установка без UART: кнопка Reset + TFTP / Installing without UART: Reset + TFTP

> **RU** ниже, **EN** — в конце.

> Проверено на железе 21–22.10.2026: вход кнопкой Reset, приём файла, запись загрузчиком,
> загрузка OpenWrt; содержимое флеша сверено с образом по md5. От отправки файла до
> работающей OpenWrt — около 2 минут.

> ⚠️ Только для **Ротек RX-23302 с RTL8197FH-VG** (заводская прошивка `ELECTRA-RX23302`,
> загрузчик `v3.4T-pre4.6`). Другие ревизии и двойники не проверялись — см. README.

## Как это работает

В заводском загрузчике (`Realtek RTL8197F-VG boot code … v3.4T-pre4.6`) есть аварийный режим.
Если при включении питания **зажата кнопка Reset**, загрузчик не запускает прошивку, а поднимает
TFTP-сервер на адресе **192.168.1.6**. Принятый файл он проверяет (подписи `KE32`/`RE32`, контрольные
суммы), сам пишет во флеш и перезагружается. Разбирать корпус и паять не нужно.

Файл для этого способа — **`openwrt-…-electra_rx23302-squashfs-tftp-factory.bin`**. Это тот же
`full.bin`, только разложенный на два блока с заголовками, которые понимает загрузчик:
ядро ложится с 0x30000, корневая система — с 0x300000. Во флеше получается то же самое, что
после установки через UART.

**Заводская прошивка во втором слоте (0x800000) не затирается** — путь назад: [recovery.md](recovery.md).

## Что понадобится

- Компьютер с проводом Ethernet и TFTP-клиентом (Linux: `tftp-hpa`, `atftp` или `busybox tftp`;
  Windows: встроенный «Клиент TFTP» или tftpd64).
- Скрепка для кнопки Reset.

## Шаги

1. Компьютер — проводом в гнездо **LAN** роутера (не в WAN).
2. На компьютере вручную задайте адрес **192.168.1.10**, маска **255.255.255.0**.
3. Выключите роутер. **Зажмите Reset**, затем, **не отпуская**, включите питание. Держите ~10 секунд.
   - Если до этого уже было включено, а вы зажали кнопку потом, — не сработает: загрузчик смотрит
     на кнопку один раз, в первые секунды.
4. Проверьте, что режим включился: `ping 192.168.1.6` **не отвечает** (так и должно быть), но
   `arp -a` показывает адрес 192.168.1.6. Надёжнее всего — сразу пробовать шаг 5.
5. Отправьте файл (двоичный режим):
   ```sh
   # Linux, busybox:
   busybox tftp -p -l openwrt-…-tftp-factory.bin -r tftp-factory.bin 192.168.1.6
   # Linux, tftp-hpa:
   tftp 192.168.1.6 -m binary -c put openwrt-…-tftp-factory.bin
   # Windows:
   tftp -i 192.168.1.6 PUT openwrt-…-tftp-factory.bin
   ```
   Передача — секунды. **Не выключайте питание** минимум 3 минуты: загрузчик пишет ядро, затем
   корневую систему (`Flash Write Successed!` в логе UART, если он подключён) и сам перезагрузится.
6. После перезагрузки — OpenWrt: 192.168.0.1, LuCI в браузере, Wi-Fi `RX23302` / `RX23302-5G`.
7. **Раздел настроек TFTP не стирает.** Если на роутере уже стояла OpenWrt — сбросьте настройки:
   держите Reset 5+ секунд в работающей OpenWrt, или LuCI «Восстановить заводские», или
   `firstboot -y && reboot`. Если раньше была заводская прошивка — ничего делать не нужно.

## Если что-то пошло не так

- Загрузчик **не трогает** себя и заводские данные (0x0–0x30000), поэтому повторить шаги 3–5 можно
  всегда. Если и это не помогает — [install-uart.md](install-uart.md).
- Если в файле испорчена сумма, загрузчик ничего не пишет («checksum error»).
- Не дописывайте к файлу ничего своего: на хвост `\0mbukanov` загрузчик переписывает сектор заводских
  данных 0x23000.

---

## English

> Verified on hardware (2026-10-21/22): button entry, transfer, bootloader flashing and OpenWrt
> boot; flash contents checked against the image by md5. About 2 minutes from upload to a running
> OpenWrt. Rotek RX-23302 with RTL8197FH-VG (bootloader `v3.4T-pre4.6`) only.

The stock bootloader (`v3.4T-pre4.6`) has a recovery mode: hold **Reset while applying power**
(keep it ~10 s) and it starts a TFTP server at **192.168.1.6** instead of booting. It checks the
received file (`KE32`/`RE32` signatures, checksums), writes it to flash and reboots. No disassembly,
no soldering.

Use **`openwrt-…-electra_rx23302-squashfs-tftp-factory.bin`** — the same content as `full.bin`
split into two bootloader-understood blocks (kernel → 0x30000, rootfs → 0x300000). The stock
firmware in the second bank (0x800000) is not touched — see [recovery.md](recovery.md).

1. PC cable into a router **LAN** port; set PC address **192.168.1.10/24**.
2. Power off, **hold Reset, power on**, keep holding ~10 s (pressing after power-on does not work).
3. `tftp -i 192.168.1.6 PUT openwrt-…-tftp-factory.bin` (binary mode). `ping` to .6 does not answer — expected.
4. Do not cut power for at least 3 minutes; it reboots by itself into OpenWrt (192.168.0.1).
5. TFTP does not erase the settings partition: if OpenWrt was installed before, factory-reset it
   (hold Reset 5+ s, LuCI, or `firstboot -y && reboot`).
