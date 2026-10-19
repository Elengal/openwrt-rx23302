#!/bin/sh
# Первичная настройка Wi-Fi для вендорского драйвера rtl8192fe на ELECTRA RX23302.
# `wifi config` (его зовёт /etc/init.d/boot при первой загрузке) находит радио драйвера
# в /proc/wlanN и дописывает в /etc/config/wireless те, которых там ещё нет.
#
# ОТКУДА: идея и структура - /lib/wifi/rtl8192cd.sh из sowarden/hh71vm-openwrt
# (коммит 8881e78, GPL-2.0). ИЗМЕНЕНО для RX23302: диапазон по phyBandSelect
# (1 = 2.4, 2 = 5 ГГц), заводские SSID/пароль RX23302, страна RU, без лишних полей.
#
# Значения по умолчанию: обе точки WPA2-PSK с паролем
# 1234567890, дальше человек меняет всё в LuCI.

append DRIVERS "rtl8192cd"

rtl8192cd_radios() {
	local dir
	for dir in /proc/wlan[0-9]; do
		[ -d "$dir" ] || continue
		echo "${dir##*/}"
	done
}

rtl8192cd_is_5g() {
	grep -qE 'phyBandSelect: *2' "/proc/$1/mib_rf" 2>/dev/null
}

rtl8192cd_check_device() {
	local ifname
	config_get ifname "$1" ifname
	[ "$ifname" = "$2" ] && known=1
}

detect_rtl8192cd() {
	local dev devidx known channel htmode band ssid

	config_load wireless
	for dev in $(rtl8192cd_radios); do
		known=0
		config_foreach rtl8192cd_check_device wifi-device "$dev"
		[ "$known" = 1 ] && continue

		devidx="${dev#wlan}"
		if rtl8192cd_is_5g "$dev"; then
			band=5g; channel=36; htmode=VHT80; ssid=RX23302-5G
		else
			band=2g; channel=1; htmode=HT20; ssid=RX23302
		fi

		uci -q batch <<-EOF
			set wireless.radio${devidx}=wifi-device
			set wireless.radio${devidx}.type=rtl8192cd
			set wireless.radio${devidx}.ifname=${dev}
			set wireless.radio${devidx}.band=${band}
			set wireless.radio${devidx}.channel=${channel}
			set wireless.radio${devidx}.htmode=${htmode}
			set wireless.radio${devidx}.country=RU
			set wireless.radio${devidx}.disabled=0
			set wireless.default_radio${devidx}=wifi-iface
			set wireless.default_radio${devidx}.device=radio${devidx}
			set wireless.default_radio${devidx}.network=lan
			set wireless.default_radio${devidx}.mode=ap
			set wireless.default_radio${devidx}.ssid=${ssid}
			set wireless.default_radio${devidx}.encryption=psk2+ccmp
			set wireless.default_radio${devidx}.key=1234567890
EOF
		uci -q commit wireless
	done
}
