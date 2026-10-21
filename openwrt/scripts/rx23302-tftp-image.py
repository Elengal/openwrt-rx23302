#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""full.bin -> tftp-factory.bin для аварийного TFTP заводского загрузчика RX23302.
full.bin -> tftp-factory.bin for the RX23302 stock bootloader TFTP recovery.

Загрузчик v3.4T-pre4.6 (AUTOBURN=1 по умолчанию) после приёма файла по TFTP
идёт по нему блоками «16 байт заголовка + данные»: заголовок {подпись,
адрес загрузки, адрес во флеше, длина} (big-endian). Сумма 16-битных слов
данных должна быть 0. KE32 пишется во флеш вместе с заголовком, RE32 - без.
The bootloader walks the received file as "16-byte header + data" blocks
{signature, load address, flash address, length} (big-endian); data must
sum to 0 in 16-bit words; KE32 is written with its header, RE32 without.

    [KE32 hdr | ядро/kernel] [RE32 {0, 0x300000, len} | rootfs]

Во флеше получается ровно то же, что даёт full.bin: ядро с 0x30000,
rootfs с 0x300000. Хвост "\\0mbukanov" НЕ добавляется: по нему загрузчик
переписал бы сектор заводских данных 0x23000.
On flash this is byte-identical to full.bin. No "mbukanov" tail: the
bootloader would rewrite the factory data sector 0x23000 for it.
"""
import struct
import sys

KERNEL_PART = 0x2D0000      # 0x30000..0x300000
ROOTFS_FLASH = 0x300000


def sum16(d):
    return sum(struct.unpack(">%dH" % (len(d) // 2), d[: len(d) // 2 * 2])) & 0xFFFF


def main():
    if len(sys.argv) != 3:
        sys.exit("использование / usage: rx23302-tftp-image.py <full.bin> <out>")
    full = open(sys.argv[1], "rb").read()
    sig, _load, burn, klen = struct.unpack(">4sIII", full[:16])
    if sig != b"KE32" or burn != 0x30000:
        sys.exit("не full.bin RX23302: %r %#x" % (sig, burn))
    if 16 + klen > KERNEL_PART or sum16(full[16:16 + klen]) != 0:
        sys.exit("ядро: размер или сумма / kernel size or checksum")
    if full[16 + klen:KERNEL_PART].strip(b"\xff"):
        sys.exit("между ядром и rootfs не 0xFF / gap is not 0xFF")
    root = full[KERNEL_PART:]
    if root[:4] != b"hsqs" or len(root) % 2 or sum16(root) != 0:
        sys.exit("rootfs: не squashfs или сумма не 0 / bad rootfs")
    out = full[:16 + klen]
    out += struct.pack(">4sIII", b"RE32", 0, ROOTFS_FLASH, len(root)) + root
    open(sys.argv[2], "wb").write(out)
    print("rx23302-tftp: ядро %d, rootfs %d, итого %d байт" % (klen, len(root), len(out)))


if __name__ == "__main__":
    main()
