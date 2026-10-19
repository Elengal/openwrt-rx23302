#!/usr/bin/env python3
"""Собирает ОДИН файл прошивки ELECTRA RX23302 под флеш начиная с адреса 0x30000.

    ядро (cvimg KE32)  | 0xFF до 0x2d0000 | rootfs (squashfs + сумма загрузчика)
    флеш 0x030000 ...                       флеш 0x300000 ...

Раздел kernel = 0x30000..0x300000 (0x2d0000 байт), rootfs = 0x300000..0x600000.
Загрузчик ищет ядро на 0x30000 и корневую систему ровно на 0x300000, поэтому
rootfs нельзя класть сразу за ядром.

Пишется целиком: из работающей системы `mtd write full.bin firmware` (раздел
firmware = kernel+rootfs) или загрузчиком через UART: XMOD в ОЗУ, затем
FLW 30000 <адрес в ОЗУ> <длина>.

Сумма rootfs - тем же способом, что scripts/rtl819x-rootfs-chksum.py
(по разбору загрузчика).
"""
import struct
import subprocess
import sys
import os

KERNEL_MAX = 0x2D0000           # раздел kernel
ROOTFS_MAX = 0x300000           # раздел rootfs (до rootfs_data на 0x600000)


def summa(d):
    s = 0
    for i in range(0, len(d) - 1, 2):
        s = (s + ((d[i] << 8) | d[i + 1])) & 0xFFFF
    return s


def main():
    if len(sys.argv) != 4:
        sys.exit("использование: rx23302-full-image.py <kernel.bin> <rootfs.squashfs> <выход>")
    kern_p, root_p, out_p = sys.argv[1:]

    kern = open(kern_p, "rb").read()
    if kern[:4] != b"KE32":
        sys.exit("ядро без подписи KE32: %r" % kern[:4])
    if len(kern) > KERNEL_MAX:
        sys.exit("ядро %d байт больше раздела %d" % (len(kern), KERNEL_MAX))

    # rootfs: копия, к которой применяем ту же сумму, что для rootfs.bin
    tmp = out_p + ".rootfs"
    with open(root_p, "rb") as f, open(tmp, "wb") as g:
        g.write(f.read())
    chk = os.path.join(os.path.dirname(os.path.abspath(__file__)), "rtl819x-rootfs-chksum.py")
    subprocess.run([sys.executable, chk, tmp], check=True)
    root = open(tmp, "rb").read()
    os.unlink(tmp)
    if root[:4] not in (b"hsqs", b"sqsh"):
        sys.exit("rootfs не squashfs")
    if len(root) > ROOTFS_MAX:
        sys.exit("rootfs %d байт больше раздела %d" % (len(root), ROOTFS_MAX))
    if summa(root) != 0:
        sys.exit("сумма rootfs не ноль")

    img = kern + b"\xff" * (KERNEL_MAX - len(kern)) + root
    open(out_p, "wb").write(img)
    print("rx23302-full: ядро %d (запас %d), rootfs %d (запас %d), итого %d байт"
          % (len(kern), KERNEL_MAX - len(kern), len(root), ROOTFS_MAX - len(root), len(img)))


if __name__ == "__main__":
    main()
