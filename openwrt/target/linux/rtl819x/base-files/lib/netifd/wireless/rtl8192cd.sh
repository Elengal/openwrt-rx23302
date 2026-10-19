#!/bin/sh
# Обработчик netifd для вендорского драйвера Wi-Fi rtl8192fe 3.8.0 на ELECTRA RX23302
# (RTL8197G 2.4 ГГц встроенный = wlan0, RTL8812FE 5 ГГц на PCIe = wlan1).
#
# ОТКУДА: основа - /lib/netifd/wireless/rtl8192cd.sh из sowarden/hh71vm-openwrt
# (https://github.com/sowarden/hh71vm-openwrt, коммит 8881e78, GPL-2.0), плата Alcatel HH71VM
# (RTL8197FS + RTL8812FE). Взято: схема config_add_*, разбор htmode -> band/use40M/2ndchoffset,
# разбор шифрования (битовая маска wpa2_cipher!), порядок set_mib, fail-closed, отказ для
# не-AP режимов. ИЗМЕНЕНО для RX23302:
#   - iwpriv нет (в ядре нет WEXT) -> своя утилита /usr/sbin/rtlmib (тот же set_mib через
#     SIOCDEVPRIVATE+1);
#   - заводские MAC: hwsetting у нас не отдельный раздел, а внутри "boot" (mtd0) по смещениям
#     структуры HW_SETTING_T: wlan[0] 0x20013, wlan[1] 0x209d6 (сверено с дампом и стоком);
#     VAP-адреса - производные с локальным битом (их rtl_derived_mac);
#   - countrystr/regdomain из option country (RU=12), без него драйвер откатывается в FCC;
#   - MIMO_TR_mode=2 (2T2R) явно; убраны светодиод HH71VM и подстройки EDCA/SWQ (не мерены у нас).
# v4 (показать всё, что умеет железо): каждая опция LuCI, для
# которой у драйвера есть рычаг (MIB собран в ядро - сверено по строкам vmlinux), теперь
# доходит до драйвера: авто-канал, 20/40 авто, «только N/AC», 802.11b, плотность ячейки, маяк,
# RTS/фрагментация, дистанция, LDPC, короткий GI, WMM, изоляция, MAC-фильтр, короткая
# преамбула, DTIM, смена группового ключа, таймаут неактивности, WEP.
# v5 (ядро пересобрано: CONFIG_RTL_11W_SUPPORT, POWER_PERCENT_ADJUSTMENT, 3 VAP, 32 клиента):
# 802.11w и мощность передачи.
# Опция, которую драйвер не умеет, в LuCI не показывается.
#
# Почему не mac80211.sh: драйвер не cfg80211 (свой стек 802.11), настраивается приватным MIB.
#
# ВАЖНО про сброс: MIB драйвера живут, пока загружено ядро, и переживают ifconfig down/up.
# Поэтому КАЖДАЯ опция пишется всегда - и включённая, и выключенная (значением по умолчанию).
# Иначе выключенная в LuCI галочка оставалась бы включённой до перезагрузки роутера.

. /lib/netifd/netifd-wireless.sh
. /lib/functions.sh

init_wireless_driver "$@"

RTLMIB=/usr/sbin/rtlmib

drv_rtl8192cd_init_device_config() {
	# Радио привязано к постоянному netdev драйвера: radio0 -> wlan0, radio1 -> wlan1.
	config_add_string ifname country require_mode distance
	config_add_boolean ampdu legacy_rates noscan ldpc rxldpc short_gi_20 short_gi_40 short_gi_80
	config_add_int beacon_int rts frag cell_density txpower_percent
}

drv_rtl8192cd_init_iface_config() {
	config_add_boolean hidden wmm isolate short_preamble
	config_add_string macfilter ieee80211w
	config_add_array maclist
	config_add_int dtim_period wpa_group_rekey max_inactivity
	config_add_string key1 key2 key3 key4
}

# netifd-wireless.sh зовёт и эти при `dump`: без них «not found» в stderr
# (VLAN и станции на точке драйвер не умеет - пустые схемы).
drv_rtl8192cd_init_vlan_config() { :; }
drv_rtl8192cd_init_station_config() { :; }

# htmode -> биты 11n/11ac для band, ширина канала и положение второго канала.
# use40M: 0=20, 1=40, 2=80 МГц; 2ndchoffset: 0=неважно, 1=ниже, 2=выше.
rtl_parse_htmode() {
	ht_bit=0; vht_bit=0; use40m=0; choffset=0
	case "$1" in
		VHT*) ht_bit=8; vht_bit=64 ;;
		HT*)  ht_bit=8 ;;
	esac
	case "$1" in
		HT40+|VHT40+) use40m=1; choffset=2 ;;
		HT40-|VHT40-) use40m=1; choffset=1 ;;
		HT40|VHT40)   use40m=1 ;;
		VHT80)        use40m=2 ;;
	esac
}

# band (enum NETWORK_TYPE): 1|2|8 = b|g|n на 2.4 ГГц, 4|8|64 = a|n|ac на 5 ГГц.
rtl_band_mask() {
	case "$1" in
		a) echo $((4 + ht_bit + vht_bit)) ;;
		b) echo 1 ;;
		*) echo $((3 + ht_bit)) ;;
	esac
}

# Регион. Драйвер при open сам сверяет countrystr со своей таблицей стран (8192cd_11h.c,
# 100 стран) и по ней ставит набор каналов (DOT11D); пустой countrystr -> FCC, каналы 1-11.
# regdomain - запасной набор на случай, если страны в таблице нет (12 = 1-13 / 5 ГГц как RU).
rtl_regdomain() {
	case "$1" in
		US|CA|TW|PR|MX|CO|PA|PH|DO|GT|SV|HN|UY|VE|EC|PE|BO|CL|AR|CR|TT) echo 1 ;;
		JP)       echo 6 ;;
		FR)       echo 4 ;;
		IL)       echo 7 ;;
		CN)       echo 11 ;;
		*)        echo 12 ;;
	esac
}

# Набор скоростей (bit0..11 = 1, 2, 5.5, 11, 6, 9, 12, 18, 24, 36, 48, 54 Мбит/с;
# 8192cd_sme.c dot11_rate_table) по «плотности ячейки» и «разрешить 802.11b» - по
# той же таблице, что hostapd.sh у OpenWrt. Печатает "oprates basicrates".
rtl_rates() {
	local hw="$1" density="${2:-0}" legacy="${3:-1}"

	case "$hw" in
		b)
			case "$density" in
				0) echo "15 15" ;;          # 1 2 5.5 11 / 1 2 5.5 11
				1) echo "12 12" ;;          # 5.5 11
				*) echo "8 8" ;;            # 11
			esac
		;;
		a)
			case "$density" in
				0|1) echo "4080 336" ;;     # 6..54 / 6 12 24
				2) echo "4032 320" ;;       # 12..54 / 12 24
				*) echo "3840 256" ;;       # 24..54 / 24
			esac
		;;
		*)
			if [ "$density" -le 1 ] && [ "$legacy" = 0 ]; then
				echo "4080 336"             # без 802.11b: 6..54 / 6 12 24
			elif [ "$density" = 0 ]; then
				echo "4095 15"              # как драйвер по умолчанию: всё / 1 2 5.5 11
			elif [ "$density" = 1 ]; then
				echo "4092 12"              # 5.5 6 9 11 12..54 / 5.5 11
			elif [ "$density" = 2 ] || [ "$legacy" != 0 ]; then
				if [ "$legacy" = 0 ]; then
					echo "4032 320"         # 12..54 / 12 24
				else
					echo "4040 8"           # 11 12..54 / 11
				fi
			else
				echo "3840 256"             # 24..54 / 24
			fi
		;;
	esac
}

# «Дистанция» в метрах -> таймаут ACK драйвера (регистр ACKTO, единица 1 мкс, 0 = значение
# по умолчанию драйвера 0x40 = 64 мкс). Сигнал идёт туда и обратно (2 пути по 300 м за мкс),
# плюс SIFS 16 мкс и запас 10 мкс. Меньше 64 не ставим (это умолчание), больше 255 регистр не берёт.
rtl_ack_timeout() {
	local d="$1" us

	case "$d" in
		""|auto|0) echo 0; return ;;
	esac
	us=$(( (d * 2 + 299) / 300 + 10 + 16 ))
	[ "$us" -lt 64 ] && us=64
	[ "$us" -gt 255 ] && us=255
	echo "$us"
}

# Заводской MAC радио из блока hwsetting (подпись H601) внутри раздела boot.
rtl_factory_mac() {
	local ifname="$1" mtd off
	case "$ifname" in
		wlan0) off=$((0x20013)) ;;
		wlan1) off=$((0x209d6)) ;;
		*) return 1 ;;
	esac
	mtd=$(grep -m1 '"boot"' /proc/mtd | cut -d: -f1)
	[ -n "$mtd" ] || return 1
	[ "$(dd if="/dev/$mtd" bs=1 skip=$((0x20000)) count=4 2>/dev/null)" = "H601" ] || return 1
	dd if="/dev/$mtd" bs=1 skip="$off" count=6 2>/dev/null |
		hexdump -v -e '5/1 "%02x:" 1/1 "%02x"'
}

# Адрес для VAP: базовый адрес радио + локальный бит, номер VAP - в пятый октет
# (рецепт hh71vm: так адреса двух радио не пересекаются ни при каком числе VAP).
rtl_derived_mac() {
	local base="$1" nth="$2" o1 o2 o3 o4 o5 o6 rest
	case "$base" in
		*:*:*:*:*:*:*) return 1 ;;
		*:*:*:*:*:*)   ;;
		*)             return 1 ;;
	esac
	o1="${base%%:*}"; rest="${base#*:}"
	o2="${rest%%:*}"; rest="${rest#*:}"
	o3="${rest%%:*}"; rest="${rest#*:}"
	o4="${rest%%:*}"; rest="${rest#*:}"
	o5="${rest%%:*}"; o6="${rest#*:}"
	printf '%02x:%s:%s:%s:%02x:%s\n' \
		$(( 0x$o1 | 0x02 )) "$o2" "$o3" "$o4" $(( 0x$o5 ^ ((nth % 16) * 16) )) "$o6"
}

rtl_vif_ifname() {
	[ "$2" = 0 ] && echo "$1" || echo "$1-va$(($2 - 1))"
}

rtl_unsupported_vif() {
	wireless_setup_vif_failed UNSUPPORTED_MODE
}

# Пишет поля MIB по одному и останавливается на первом отказе (значения - отдельными
# аргументами, пробелы в SSID не разваливаются).
rtl_apply_mib() {
	local ifname="$1"
	shift
	while [ $# -gt 0 ]; do
		$RTLMIB "$ifname" set "$1" >/dev/null 2>&1 || {
			echo "rtl8192cd: $ifname: set_mib ${1%%=*} отказ" >&2
			return 1
		}
		shift
	done
}

# MAC-фильтр: aclmode 0 = выкл, 1 = пускать только из списка, 2 = не пускать из списка.
# Список драйвера - до 128 адресов (NUM_ACL при D_ACL); aclnum=0 сбрасывает старый список,
# каждый acladdr= дописывает адрес (12 шестнадцатеричных цифр без двоеточий).
rtl_apply_acl() {
	local ifname="$1" mode="$2" mac n=0 aclmode

	case "$mode" in
		allow) aclmode=1 ;;
		deny)  aclmode=2 ;;
		*)     aclmode=0 ;;
	esac
	rtl_apply_mib "$ifname" "aclnum=0" "aclmode=$aclmode" || return 1
	[ "$aclmode" = 0 ] && return 0

	for mac in $maclist; do
		mac="$(echo "$mac" | tr -d ':-' | tr 'A-F' 'a-f')"
		case "$mac" in
			[0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f]) ;;
			*) continue ;;
		esac
		[ "$n" -lt 128 ] || break
		rtl_apply_mib "$ifname" "acladdr=$mac" || return 1
		n=$((n + 1))
	done
}

rtl_vif_fail() {
	wireless_setup_vif_failed "$1"
	vif_idx=$((vif_idx + 1))
}

rtl_setup_vif() {
	local name="$1"
	local ifname ssid key hidden macaddr wmm isolate short_preamble macfilter ieee80211w pmf
	local dtim_period wpa_group_rekey max_inactivity key1 key2 key3 key4 maclist
	local wpa auth_type wpa_cipher auth_mode_open auth_mode_shared
	local psk_enable=0 encmode=0 cipher_mask=0 ieee8021x=0 default_port=1 authtype=0
	local wepkeyid=0 wepk i

	ifname="$(rtl_vif_ifname "$dev_ifname" "$vif_idx")"

	json_select config
	json_get_vars ssid key hidden macaddr wmm short_preamble macfilter ieee80211w \
		dtim_period wpa_group_rekey max_inactivity key1 key2 key3 key4
	json_get_values maclist maclist
	wireless_vif_parse_encryption
	json_select ..

	# «Изолировать клиентов» - из СВОЕЙ секции uci, а не из json, который for_each_interface
	# даёт обработчику: туда netifd-wireless.sh сам дописывает isolate=1
	# (_wireless_set_brsnoop_isolation: мост + multicast_to_unicast по умолчанию; это его
	# изоляция в мосте Linux через hairpin). Читать его отсюда = включить изоляцию в драйвере
	# всем без галочки - поймано на стенде 06.10.2026 (block_relay=1 при пустом option isolate).
	# $1 здесь - номер интерфейса ("01"), имя секции uci лежит в json рядом с config.
	local section
	json_get_var section name
	isolate="$(uci -q get "wireless.${section:-$name}.isolate")"

	[ -d "/proc/$ifname" ] || { rtl_vif_fail NO_SUCH_INTERFACE; return 1; }

	case "$wpa_cipher" in
		*CCMP*TKIP*|*TKIP*CCMP*) cipher_mask=10; encmode=4 ;;
		*CCMP*)                  cipher_mask=8;  encmode=4 ;;
		*TKIP*)                  cipher_mask=2;  encmode=2 ;;
	esac
	case "$auth_type" in
		psk)
			# 1=WPA, 2=WPA2, 3=смешанный. Станция заблокирована до 4-way handshake драйвера.
			psk_enable="$wpa"; ieee8021x=1; default_port=0
			[ ${#key} -ge 8 ] || { rtl_vif_fail INVALID_WPA_PSK; return 1; }
		;;
		none)
			psk_enable=0; encmode=0; cipher_mask=0; ieee8021x=0; default_port=1
		;;
		wep)
			# WEP драйвер делает сам: encmode 1 = WEP-40 (5 байт), 5 = WEP-104 (13 байт);
			# authtype 0 = открытая, 1 = общий ключ, 2 = обе. Ключи - wepkey1..4 (hex),
			# активный - wepdkeyid (0..3). option key как в OpenWrt: номер 1-4 (ключи в
			# key1..key4) или сам ключ; «s:» - явная строка (prepare_key_wep).
			psk_enable=0; cipher_mask=0; ieee8021x=0; default_port=1
			if [ "$auth_mode_shared" = 1 ] && [ "$auth_mode_open" = 1 ]; then
				authtype=2
			elif [ "$auth_mode_shared" = 1 ]; then
				authtype=1
			fi
			case "$key" in
				[1234]) wepkeyid=$((key - 1)) ;;
				*)      key1="$key"; wepkeyid=0 ;;
			esac
			encmode=0
			for wepk in "$key1" "$key2" "$key3" "$key4"; do
				[ -n "$wepk" ] || continue
				wepk="$(prepare_key_wep "$wepk")"
				case "${#wepk}" in
					10) [ "$encmode" = 5 ] && { rtl_vif_fail INVALID_WEP_KEY; return 1; }; encmode=1 ;;
					26) [ "$encmode" = 1 ] && { rtl_vif_fail INVALID_WEP_KEY; return 1; }; encmode=5 ;;
					*)  rtl_vif_fail INVALID_WEP_KEY; return 1 ;;
				esac
			done
			[ "$encmode" != 0 ] || { rtl_vif_fail INVALID_WEP_KEY; return 1; }
		;;
		*)
			# EAP (нужен внешний аутентификатор - hostapd в образе нет), SAE/WPA3 и OWE
			# (в ядро не собраны): встроенный аутентификатор драйвера их не умеет.
			rtl_vif_fail UNSUPPORTED_ENCRYPTION
			return 1
		;;
	esac

	# 802.11w (защищённые служебные кадры, PMF): 0 = выкл, 1 = по желанию клиента,
	# 2 = обязательно. Драйвер (8192cd_psk.c) умеет только с WPA2-PSK: при 1 объявляет
	# AKM PSK + PSK-SHA256 (нужен enableSHA256=1), при 2 - только PSK-SHA256.
	case "$ieee80211w" in
		1|2) if [ "$auth_type" = psk ] && [ "$wpa" = 2 ]; then pmf="$ieee80211w"; else pmf=0; fi ;;
		*)   pmf=0 ;;
	esac

	# Опускаем ДО настройки и оставляем опущенным при любой ошибке: полунастроенная точка
	# не должна выходить в эфир со старым ключом или без шифрования.
	ifconfig "$ifname" down 2>/dev/null

	[ -n "$macaddr" ] || {
		if [ "$vif_idx" = 0 ]; then
			macaddr="$(rtl_factory_mac "$dev_ifname")"
		else
			macaddr="$(rtl_derived_mac "$(rtl_factory_mac "$dev_ifname")" "$vif_idx")"
		fi
	}
	# Без своего MAC у обоих радио один BSSID 00:e0:4c:81:86:86, и Android показывает
	# только одну из двух сетей (проверено на RX23302 02.10.2026).
	[ -n "$macaddr" ] && ip link set dev "$ifname" address "$macaddr"

	# Настройки радио (канал, ширина, диапазон, скорости, страна...) пишутся только в
	# корневой интерфейс wlanX: вторые сети (wlanX-va0..) работают на том же радио и берут их
	# у корня. И это не только лишнее, а опасно: set_mib "band" для НЕ-корневого интерфейса
	# в режиме точки зовёт init_beacon() (8192cd_ioctl.c:4175), а у опущенного VAP маяк ещё не
	# собран - Oops в init_beacon/update_tim и перезагрузка роутера (поймано на стенде
	# 06.10.2026, UART-лог scratch-06-10/uart-v4-06-10.log).
	if [ "$vif_idx" = 0 ]; then
		rtl_apply_mib "$ifname" \
			"countrystr=$country_str" \
			"regdomain=$regdomain" \
			"band=$band" \
			"deny_legacy=$deny_legacy" \
			"oprates=$oprates" \
			"basicrates=$basicrates" \
			"channel=$channel" \
			"use40M=$use40m" \
			"2ndchoffset=$choffset" \
			"coexist=$coexist" \
			"shortGI20M=$sgi20" \
			"shortGI40M=$sgi40" \
			"shortGI80M=$sgi80" \
			"ldpc=$ldpc_mib" \
			"bcnint=$bcnint" \
			"rtsthres=$rtsthres" \
			"fragthres=$fragthres" \
			"ack_timeout=$acktimeout" \
			"MIMO_TR_mode=2" \
			"powerpercent=$powerpct" \
			"ampdu=$ampdu" || { rtl_vif_fail MIB_WRITE_FAILED; return 1; }
	fi

	rtl_apply_mib "$ifname" \
		"ssid=$ssid" \
		"opmode=16" \
		"hiddenAP=${hidden:-0}" \
		"qos_enable=${wmm:-1}" \
		"block_relay=${isolate:-0}" \
		"preamble=${short_preamble:-0}" \
		"dtimperiod=${dtim_period:-1}" \
		"gk_rekey=${wpa_group_rekey:-0}" \
		"expired_time=$(( ${max_inactivity:-300} * 100 ))" \
		"authtype=$authtype" \
		"psk_enable=$psk_enable" \
		"encmode=$encmode" \
		"802_1x=$ieee8021x" \
		"default_port=$default_port" \
		"wpa_cipher=$cipher_mask" \
		"wpa2_cipher=$cipher_mask" \
		"dot11IEEE80211W=$pmf" \
		"enableSHA256=$([ "$pmf" != 0 ] && echo 1 || echo 0)" || { rtl_vif_fail MIB_WRITE_FAILED; return 1; }

	if [ "$psk_enable" != 0 ]; then
		rtl_apply_mib "$ifname" "passphrase=$key" || { rtl_vif_fail KEY_WRITE_FAILED; return 1; }
	fi
	if [ "$auth_type" = wep ]; then
		i=1
		for wepk in "$key1" "$key2" "$key3" "$key4"; do
			if [ -n "$wepk" ]; then
				rtl_apply_mib "$ifname" "wepkey$i=$(prepare_key_wep "$wepk")" || {
					rtl_vif_fail KEY_WRITE_FAILED; return 1; }
			fi
			i=$((i + 1))
		done
		rtl_apply_mib "$ifname" "wepdkeyid=$wepkeyid" || { rtl_vif_fail KEY_WRITE_FAILED; return 1; }
	fi
	rtl_apply_acl "$ifname" "$macfilter" || { rtl_vif_fail MACFILTER_FAILED; return 1; }

	ifconfig "$ifname" up || { rtl_vif_fail IFUP_FAILED; return 1; }

	# netifd сам кладёт интерфейс в мост по option network.
	wireless_add_vif "$name" "$ifname"
	vif_idx=$((vif_idx + 1))
}

drv_rtl8192cd_setup() {
	local dev="$1"
	local dev_ifname raw_htmode band ampdu country country_str regdomain vif_idx=0
	local ht_bit vht_bit use40m choffset
	local require_mode distance legacy_rates noscan ldpc rxldpc short_gi_20 short_gi_40 short_gi_80
	local beacon_int rts frag cell_density txpower_percent powerpct
	local deny_legacy coexist sgi20 sgi40 sgi80 ldpc_mib bcnint rtsthres fragthres acktimeout
	local rates oprates basicrates

	json_select config
	json_get_vars ifname ampdu country require_mode distance legacy_rates noscan ldpc rxldpc txpower_percent \
		short_gi_20 short_gi_40 short_gi_80 beacon_int rts frag cell_density
	json_get_var raw_htmode htmode
	json_select ..

	dev_ifname="${ifname:-wlan${dev#radio}}"
	[ -d "/proc/$dev_ifname" ] || {
		wireless_setup_failed NO_SUCH_INTERFACE
		return 1
	}

	country_str="${country:-RU}"
	regdomain="$(rtl_regdomain "$country_str")"
	rtl_parse_htmode "$raw_htmode"
	case "$raw_htmode" in
		HT*|VHT*) ampdu="${ampdu:-1}" ;;
		*)        ampdu=0 ;;
	esac
	# $hwmode и $channel заполняет _wdev_prepare_channel до вызова; channel «auto» он
	# превращает в 0, а 0 для драйвера и есть «выбрать канал самому» (auto_channel: при
	# включении точка слушает эфир и берёт самый чистый канал из разрешённых для страны).
	band="$(rtl_band_mask "$hwmode")"

	# «Только N» / «только AC»: не пускать клиентов без HT / без VHT (legacySTADeny,
	# биты как в band: 1|2|4 = b|g|a, 8 = n). 802.11b выключен - не пускать b-клиентов.
	case "$require_mode" in
		n)  deny_legacy=$((1 + 2 + 4)) ;;
		ac) deny_legacy=$((1 + 2 + 4 + 8)) ;;
		*)  deny_legacy=0 ;;
	esac
	[ "$hwmode" = g ] && [ "${legacy_rates:-1}" = 0 ] && deny_legacy=$((deny_legacy | 1))

	# 20/40 на 2.4 ГГц: при 40 МГц драйвер (coexist=1, как OpenWrt по умолчанию) слушает
	# соседей и сам сужается до 20, если второй канал занят (802.11n 20/40 coexistence).
	# «Принудительно 40 МГц» (noscan) - не сужаться. На 5 ГГц драйвер coexist гасит сам.
	if [ "$use40m" = 1 ] && [ "$hwmode" = g ] && [ "${noscan:-0}" != 1 ]; then
		coexist=1
	else
		coexist=0
	fi

	# Короткий защитный интервал (GI 400 нс, +11% скорости) - на всех ширинах, как mac80211
	# включает по умолчанию. У драйвера он выключен (0) - отсюда 780 вместо 867 Мбит/с на VHT80.
	sgi20="${short_gi_20:-1}"; sgi40="${short_gi_40:-1}"; sgi80="${short_gi_80:-1}"
	# LDPC: бит0 = приём, бит1 = передача (драйвер по умолчанию 3, LuCI - обе галочки вкл).
	ldpc_mib=$(( ${rxldpc:-1} + 2 * ${ldpc:-1} ))

	bcnint="${beacon_int:-100}"
	# Пороги: «выключено» у 802.11 - 2347 (RTS) и 2346 (фрагментация).
	rtsthres="${rts:-2347}"; [ "$rtsthres" -gt 2347 ] && rtsthres=2347
	fragthres="${frag:-2346}"; [ "$fragthres" -gt 2346 ] && fragthres=2346
	[ "$fragthres" -lt 256 ] && fragthres=256
	acktimeout="$(rtl_ack_timeout "$distance")"
	# Мощность в процентах от калиброванной (своя опция txpower_percent: option txpower
	# в OpenWrt - абсолютные дБм, а их драйвер не знает - калибровки в hwsetting нет).
	powerpct="${txpower_percent:-100}"
	[ "$powerpct" -ge 1 ] 2>/dev/null || powerpct=100
	[ "$powerpct" -gt 100 ] && powerpct=100

	rates="$(rtl_rates "$hwmode" "${cell_density:-0}" "${legacy_rates:-1}")"
	oprates="${rates% *}"; basicrates="${rates#* }"

	# Несколько сетей на одном радио (wlanX-va0...): маяки вторых сетей драйвер заводит при
	# открытии корня только если у корня vap_enable=1 (8192cd_osdep.c, MBSSID). Сам он ставит
	# этот флаг лишь в ветке cfg80211 (prepare_iface_nl80211), которой у нас нет, поэтому -
	# здесь, по числу точек на радио, до того как корень поднимется.
	rtl_count_ap() { vap_count=$((vap_count + 1)); }
	vap_count=0
	for_each_interface "ap" rtl_count_ap
	[ -d "/proc/$dev_ifname" ] && ifconfig "$dev_ifname" down 2>/dev/null
	rtl_apply_mib "$dev_ifname" "vap_enable=$([ "$vap_count" -gt 1 ] && echo 1 || echo 0)" || {
		wireless_setup_failed MIB_WRITE_FAILED
		return 1
	}

	for_each_interface "sta adhoc mesh monitor wds" rtl_unsupported_vif
	for_each_interface "ap" rtl_setup_vif

	wireless_set_up
}

drv_rtl8192cd_teardown() {
	local dev="$1" dev_ifname vif

	json_select config
	json_get_vars ifname
	json_select ..
	dev_ifname="${ifname:-wlan${dev#radio}}"

	for vif in "$dev_ifname" "$dev_ifname"-va0 "$dev_ifname"-va1 "$dev_ifname"-va2 "$dev_ifname"-va3; do
		[ -d "/proc/$vif" ] && ifconfig "$vif" down 2>/dev/null
	done
}

drv_rtl8192cd_cleanup() {
	return 0
}

add_driver rtl8192cd
