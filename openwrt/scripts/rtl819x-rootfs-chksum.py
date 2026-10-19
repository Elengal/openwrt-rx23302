#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Готовит образ корневой системы так, как его ждёт загрузчик RTL819x.

Загрузчик читает 16 байт с начала раздела, сверяет подпись squashfs (годится и
"hsqs", и "sqsh"), затем берёт длину из поля по смещению 8 — старшим байтом
вперёд — и прибавляет 642. По этой длине он считает сумму 16-битных слов и ждёт
ноль. Не сошлось — печатает "rootfs checksum error at ...", объявляет набор
негодным и копирует поверх заводскую копию из второго банка. Своя прошивка при
этом молча исчезает: записалась, а после перезагрузки её уже нет.

Разобрано по коду самого загрузчика (0x8000CA00..0x8000CBA0) и сверено с
заводским образом RX23302: поле 0x27AD80, длина 0x27B002, сумма ноль.

По смещению 8 у squashfs лежит время сборки — вендор пишет туда длину. Ядро это
поле не проверяет, монтированию не мешает.

Соглашение вендора: поле = длина добитых данных минус 640, а полная длина, по
которой считается сумма, включает и два байта самой суммы. Данные добиваются
нулями до границы 4096.
"""
import struct
import sys

VYRAVNIVANIE = 4096
POPRAVKA = 640          # поле = добитая длина - 640
HVOST = 2               # два байта суммы, входят в проверяемую длину


def summa(dannye):
    itog = 0
    for i in range(0, len(dannye) - 1, 2):
        itog = (itog + ((dannye[i] << 8) | dannye[i + 1])) & 0xFFFF
    if len(dannye) % 2:
        itog = (itog + (dannye[-1] << 8)) & 0xFFFF
    return itog


def main():
    if len(sys.argv) != 2:
        sys.exit("использование: rtl819x-rootfs-chksum.py <файл>")
    put = sys.argv[1]
    syroy = open(put, "rb").read()

    if syroy[:4] not in (b"hsqs", b"sqsh"):
        sys.exit("не squashfs: подпись %r" % syroy[:4])

    zanyato = struct.unpack("<Q", syroy[40:48])[0]
    if zanyato == 0 or zanyato > len(syroy):
        sys.exit("испорченный суперблок: bytes_used=%d при размере %d" % (zanyato, len(syroy)))

    dlina = ((zanyato + VYRAVNIVANIE - 1) // VYRAVNIVANIE) * VYRAVNIVANIE
    telo = bytearray(syroy[:zanyato] + b"\x00" * (dlina - zanyato))
    struct.pack_into(">I", telo, 8, dlina - POPRAVKA)

    hvost = (-summa(bytes(telo))) & 0xFFFF
    itog = bytes(telo) + struct.pack(">H", hvost)

    # самопроверка ровно тем же путём, каким идёт загрузчик
    pole = struct.unpack(">I", itog[8:12])[0]
    proveryaemo = pole + POPRAVKA + HVOST
    if proveryaemo != len(itog):
        sys.exit("длина не сходится: поле даёт %d, образ %d" % (proveryaemo, len(itog)))
    if summa(itog[:proveryaemo]) != 0:
        sys.exit("самопроверка не сошлась: 0x%04X" % summa(itog[:proveryaemo]))

    open(put, "wb").write(itog)
    print("rootfs-chksum: данные 0x%X, добито до 0x%X, поле 0x%X, сумма 0x%04X"
          % (zanyato, dlina, dlina - POPRAVKA, hvost))


if __name__ == "__main__":
    main()
