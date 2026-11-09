---
title: OpenWrt для Rotek RX-23302
---

# OpenWrt для Rotek RX-23302 / ELECTRA RX23302

Своя прошивка **OpenWrt (Linux 6.18)** для роутера Ротек RX-23302 (Ростелеком / BeeLine / МТС).
В официальном OpenWrt SoC **Realtek RTL8197F** не поддерживается — порт сделан с нуля:
SoC, Ethernet и коммутатор RTL8367D, Wi-Fi (RTL8197G 2,4 ГГц и RTL8812FE 5 ГГц).

**Текущий релиз — 0.2.1.** Готовые образы: [GitHub Releases](https://github.com/Elengal/openwrt-rx23302/releases).

## Установка

- [Без разборки: кнопка Reset + TFTP](install-tftp.md) — рекомендуемый способ, ~2 минуты
- [Через UART](install-uart.md) — если TFTP не подходит

## Если что-то пошло не так

- [Восстановление заводской прошивки](recovery.md) — заводская прошивка во втором слоте не затирается

## Для разработчиков

- [Исходники, патчи и сборка](https://github.com/Elengal/openwrt-rx23302)
- [Структура порта и патчи](sources.md)

## Поддержка

Вопросы и сообщения о проблемах: [Issues](https://github.com/Elengal/openwrt-rx23302/issues).
