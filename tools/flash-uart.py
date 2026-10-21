#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""
flash-uart.py — прошивка ELECTRA RX23302 через UART и загрузчик Realtek.
flash-uart.py — flash the ELECTRA RX23302 over UART via the Realtek bootloader.

    python3 flash-uart.py full.bin [/dev/ttyUSB0] [--flr]

Что делает / What it does:
  1. ловит приглашение загрузчика <RealTek> (жмёт Esc при загрузке);
     catches the <RealTek> bootloader prompt (sends Esc during boot);
  2. AUTOBURN 0, XMOD 80a00000 <длина> — образ в ОЗУ по XMODEM-1K (~27 мин);
     image into RAM over XMODEM-1K;
  3. FLW 30000 80a00000 <длина> + Y — запись во флеш (~13 мин, печатает точки),
     ждёт тишины и приглашения; / write to flash, wait for silence and prompt;
  4. печатает md5 разделов kernel/rootfs образа — сверка ПОСЛЕ загрузки из Linux
     (см. docs/install-uart.md). / prints md5 of the kernel/rootfs parts of the
     image — verify from Linux after boot.
  --flr: дополнительно сверка в загрузчике (FLR + CMP + 2 последних байта).
     ~15 мин дольше. / optional bootloader read-back, ~15 min longer.
Перезагрузку и firstboot делаете сами (см. docs/install-uart.md).
Reboot and firstboot are up to you.

Основан на отправителе XMODEM rx-flash4.py соседа ZCode (01.09.2026):
свой отправитель, синхронизация по «Load address» — библиотечный xmodem с этим
загрузчиком не работает. / Based on the rx-flash4.py XMODEM sender: a custom
sender synchronised on "Load address"; stock xmodem libraries fail here.

ПРАВИЛА загрузчика (проверено на железе / learned the hard way):
  * команды только с полными аргументами — «FLR» без аргументов вешает загрузчик
    до сброса питания; / never send a command without arguments;
  * пока FLW/FLR печатают точки, ничего не слать — загрузчик глохнет;
    send nothing while FLW/FLR print dots;
  * на вопрос «(Y)es, (N)o» отвечать «Y» и Enter — без Enter загрузчик молчит
    бесконечно (это и выглядело как «FLR завис»); / answer "Y" AND Enter,
    otherwise it waits forever (that looked like a "hang");
  * «<RealTek>» в выводе не значит «готово»: ждать «Successed» или тишину;
    после FLW слово «Successed» не печатается — только тишина и приглашение.
    after FLW there is no "Successed" — just silence and the prompt.
"""
import hashlib
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("нужен pyserial / pyserial required:  pip install pyserial")

BAUD = 38400
RAM_IMG = 0x80A00000      # куда XMODEM кладёт образ / where XMODEM puts the image
RAM_COPY = 0x81400000     # куда читаем флеш для сверки / flash read-back buffer
FLASH_OFF = 0x30000       # начало ядра во флеше / kernel start in flash
KERNEL_LEN = 0x2D0000     # раздел kernel (mtd1); дальше — rootfs (mtd2)
ROOTFS_LEN = 0x300000     # раздел rootfs (mtd2), до rootfs_data на 0x600000


def log(msg):
    print(time.strftime("%H:%M:%S"), msg, flush=True)


def crc16(data, _t=[]):
    if not _t:
        for b in range(256):
            c = b << 8
            for _ in range(8):
                c = ((c << 1) ^ 0x1021) if c & 0x8000 else c << 1
            _t.append(c & 0xFFFF)
    c = 0
    for b in data:
        c = ((c << 8) & 0xFFFF) ^ _t[((c >> 8) ^ b) & 0xFF]
    return c


class Bootloader:
    def __init__(self, dev):
        self.p = serial.Serial(dev, BAUD, timeout=0.5)

    def drain(self, quiet, limit):
        """Читать, пока порт не помолчит quiet секунд (не дольше limit)."""
        buf, last, start = b"", time.time(), time.time()
        while time.time() - last < quiet and time.time() - start < limit:
            c = self.p.read(4096)
            if c:
                buf += c
                last = time.time()
        return buf

    def prompt(self):
        self.p.reset_input_buffer()
        self.p.write(b"\r\n")
        time.sleep(1.5)
        return b"<RealTek>" in self.p.read(512)

    def catch(self, timeout=90):
        """Esc во время загрузки до приглашения <RealTek>."""
        log("жду загрузчик: включите/перезагрузите роутер / power-cycle the router now")
        self.p.reset_input_buffer()
        self.p.write(b"reboot\n")
        buf, end = b"", time.time() + timeout
        while time.time() < end:
            self.p.write(b"\x1b")
            time.sleep(0.15)
            buf = (buf + self.p.read(256))[-3000:]
            if b"<RealTek>" in buf:
                time.sleep(0.5)
                self.drain(1, 5)
                return True
        return False

    def cmd(self, line, timeout, yes=False, until=()):
        self.p.reset_input_buffer()
        self.p.write((line + "\n").encode())
        buf, said, end = b"", False, time.time() + timeout
        while time.time() < end:
            c = self.p.read(4096)
            if not c:
                continue
            buf += c
            if yes and not said and b"(Y)es" in buf:
                # Загрузчик ждёт Y И Enter: без Enter молчит вечно (проверено 20.10.2026).
                # The bootloader needs Y AND Enter; without Enter it waits forever.
                self.p.write(b"Y\r")
                said = True
            if any(u in buf for u in until):
                time.sleep(1)
                buf += self.p.read(4096)
                break
        return buf

    def xmodem(self, data):
        """XMODEM-1K. data уже дополнен до кратного 1024."""
        self.p.reset_input_buffer()
        self.p.write(f"XMOD {RAM_IMG:x} {self.length:X}\n".encode())
        buf, end = b"", time.time() + 15
        while time.time() < end and b"Load address" not in buf:
            buf += self.p.read(64)
        if b"Load address" not in buf:
            log("!! загрузчик не ответил на XMOD")
            return False
        time.sleep(0.6)
        self.p.reset_input_buffer()
        mode, end = None, time.time() + 40
        while time.time() < end and not mode:
            b = self.p.read(1)
            mode = {b"C": "crc", b"\x15": "sum"}.get(b)
        if not mode:
            log("!! приёмник не запросил передачу")
            return False

        seq, i, junk, t0 = 1, 0, 0, time.time()
        total = len(data) // 1024
        while i < len(data):
            chunk = data[i:i + 1024]
            tail = (bytes([sum(chunk) & 0xFF]) if mode == "sum"
                    else crc16(chunk).to_bytes(2, "big"))
            self.p.write(bytes([2, seq & 0xFF, 255 - (seq & 0xFF)]) + chunk + tail)
            self.p.flush()
            ans, end = b"", time.time() + 5
            while time.time() < end and not ans:
                ans = self.p.read(1)
            if ans == b"\x06":
                seq, i, junk = seq + 1, i + 1024, 0
                if (i // 1024) % 256 == 0:
                    log(f"  {i // 1024}/{total} КиБ, {time.time() - t0:.0f} с")
            elif ans in (b"\x15", b""):
                continue
            elif ans == b"C":
                mode = "crc"
            else:
                junk += 1
                if junk > 8:
                    log(f"!! рассинхрон на пакете {seq}")
                    return False
                time.sleep(0.3)
                self.p.reset_input_buffer()
        for _ in range(6):
            self.p.write(b"\x04")
            end = time.time() + 5
            while time.time() < end:
                if self.p.read(1) == b"\x06":
                    log(f"XMODEM OK за {time.time() - t0:.0f} с")
                    return True
        log("!! EOT не подтверждён")
        return False


def linux_check(img):
    """Команды сверки из Linux: mtd1 = kernel целиком, mtd2 = rootfs (начало)."""
    k = hashlib.md5(img[:KERNEL_LEN]).hexdigest()
    rest = img[KERNEL_LEN:]
    r = hashlib.md5(rest).hexdigest()
    log("сверка после загрузки / verify after boot (UART или SSH):")
    print(f"    md5sum /dev/mtd1                      -> {k}")
    print(f"    head -c {len(rest)} /dev/mtd2 | md5sum   -> {r}")


def flr_check(bl, img, n):
    log("FLR — чтение флеша для сверки, ~15 мин (--flr)")
    bl.cmd(f"FLR {RAM_COPY:x} {FLASH_OFF:x} {n:X}", 1800, yes=True,
           until=(b"Successed", b"Failed"))
    bl.drain(5, 1800)
    bl.drain(3, 10)
    if not bl.prompt():
        log("после FLR нет приглашения: подождите; если тишина >20 мин — сбросьте"
            " питание, флеш уже записан, сверьте из Linux")
        return False
    r = bl.cmd(f"CMP {RAM_COPY:x} {RAM_IMG:x} {n - 2:X}", 300,
               until=(b"No error found", b"rror"))
    ok_cmp = b"No error found" in r
    log("CMP: " + ("No error found" if ok_cmp else "ОШИБКА / MISMATCH"))
    bl.drain(2, 5)
    addr = RAM_COPY + n - 2
    r = bl.cmd(f"DB {addr - 16:x} 20", 20, until=(b"<RealTek>",)).decode("latin-1")
    want = f"{addr:08X}: {img[-2]:02x} {img[-1]:02x}"
    ok_tail = want.lower() in r.lower()
    log(f"последние 2 байта: {'совпали' if ok_tail else 'НЕ совпали'}")
    return ok_cmp and ok_tail


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    use_flr = "--flr" in sys.argv[1:]
    if not args:
        sys.exit(__doc__)
    img = open(args[0], "rb").read()
    dev = args[1] if len(args) > 1 else "/dev/ttyUSB0"
    n = len(img)
    # Те же проверки, что в sysupgrade (base-files/lib/upgrade/platform.sh):
    # чужой файл зальётся за 40 минут и затрёт ядро и настройки.
    # Same checks as sysupgrade: a wrong file would take 40 min to flash.
    if n <= KERNEL_LEN or n > KERNEL_LEN + ROOTFS_LEN:
        sys.exit(f"странный размер образа {n} Б — это точно full.bin? / unexpected image size")
    if img[:4] != b"KE32":
        sys.exit(f"нет заголовка ядра KE32 ({img[:4]!r}) — нужен full.bin / not a full.bin")
    if img[8:12] != FLASH_OFF.to_bytes(4, "big"):
        sys.exit(f"адрес записи ядра {img[8:12].hex()} не 00030000 — не тот образ / wrong image")
    if img[KERNEL_LEN:KERNEL_LEN + 4] not in (b"hsqs", b"sqsh"):
        sys.exit("нет squashfs на 0x2d0000 — это не full.bin / no squashfs at 0x2d0000")
    log(f"образ {args[0]}: {n} Б (0x{n:X}), последние байты {img[-2:].hex(' ')}")

    bl = Bootloader(dev)
    bl.length = n
    if not bl.prompt() and not bl.catch():
        sys.exit("нет приглашения <RealTek> / no bootloader prompt")
    log("загрузчик на связи")
    r = bl.cmd("AUTOBURN 0", 5, until=(b"AutoBurning=0",))
    if b"AutoBurning=0" not in r:
        log("(AUTOBURN 0 не подтвердился — продолжаю)")

    if not bl.xmodem(img + b"\x1a" * (-n % 1024)):
        sys.exit("передача не удалась; флеш НЕ тронут / transfer failed, flash untouched")

    time.sleep(1.5)
    bl.drain(1, 5)
    log(f"FLW {FLASH_OFF:x} {RAM_IMG:x} {n:X} — запись, ~13 мин, питание НЕ трогать")
    r = bl.cmd(f"FLW {FLASH_OFF:x} {RAM_IMG:x} {n:X}", 15, yes=True, until=(b"(Y)es",))
    if b"(Y)es" not in r:
        sys.exit("FLW не спросил подтверждения; флеш НЕ тронут")
    t0 = time.time()
    # После Y+Enter загрузчик стирает и пишет, печатая точки; конец — тишина
    # и приглашение. / After Y+Enter it prints dots; the end is silence + prompt.
    seen, end = r, time.time() + 120
    while time.time() < end and b"." not in seen:
        seen += bl.p.read(256)
    if b"." not in seen:
        sys.exit("FLW: запись не началась за 2 мин — флеш, вероятно, НЕ тронут; запустите заново")
    log(f"FLW: пошли точки через {time.time() - t0:.0f} с")
    bl.drain(30, 1800)          # точки до тишины / dots until silence
    log(f"FLW: точки кончились, всего {time.time() - t0:.0f} с")
    if not bl.prompt():
        sys.exit("после FLW нет приглашения — подождите пару минут и нажмите Enter;"
                 " НЕ сбрасывайте питание, пока идут точки")
    log("запись завершена, загрузчик на связи / write finished")

    if use_flr and not flr_check(bl, img, n):
        log("СВЕРКА В ЗАГРУЗЧИКЕ НЕ ПРОШЛА — повторите прошивку")
        return 1
    linux_check(img)
    log("ГОТОВО. Дальше: reboot (или сброс питания), сверка md5, firstboot -y, sync; reboot")
    return 0


if __name__ == "__main__":
    sys.exit(main())
