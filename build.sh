#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# openwrt-rx23302: собрать прошивку для ELECTRA RX23302.
# Build OpenWrt firmware for the ELECTRA RX23302.
#
#   ./build.sh [каталог_сборки | build_dir]   (по умолчанию ./build)
#
# Клонирует OpenWrt на закреплённом коммите, накладывает изменения этого
# репозитория, закрепляет фиды, ставит конфигурацию и собирает образ.
# Clones OpenWrt at a pinned commit, applies this repository on top, pins
# the feeds, writes the config and builds the image.

set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/versions.sh"

BUILD=${1:-$HERE/build}
JOBS=${JOBS:-$(nproc 2>/dev/null || echo 2)}

say() { printf '\n=== %s\n' "$*"; }

if [ ! -d "$BUILD/.git" ]; then
	say "OpenWrt $OPENWRT_COMMIT -> $BUILD"
	git clone "$OPENWRT_URL" "$BUILD"
fi

cd "$BUILD"
git checkout -q "$OPENWRT_COMMIT"

# Отпечаток порта: содержимое openwrt/ + патч. Меняется хоть байт -
# наложение повторяется, иначе сборка молча взяла бы старьё после
# обновления этого репозитория.
STAMP=$( (cd "$HERE" && find openwrt -type f -print0 | sort -z | xargs -0 sha256sum 2>/dev/null; \
	sha256sum "$HERE/openwrt-upstream.patch") | sha256sum | cut -d' ' -f1)

if [ ! -f .rx23302-applied ] || [ "$(cat .rx23302-applied)" != "$OPENWRT_COMMIT:$STAMP" ]; then
	say "apply openwrt/ overlay + openwrt-upstream.patch"
	# Снести наложенное прошлой сборки. Кэш в build_dir/staging_dir -
	# в gitignore, git clean его не трогает: пересборка остаётся быстрой.
	git reset --hard -q HEAD
	git clean -fdq
	cp -a "$HERE/openwrt/." .
	git apply "$HERE/openwrt-upstream.patch"
	echo "$OPENWRT_COMMIT:$STAMP" > .rx23302-applied
else
	say "порт не менялся - наложение пропущено"
fi

say "feeds: luci $FEED_LUCI_COMMIT, packages $FEED_PACKAGES_COMMIT"
cat > feeds.conf <<EOF
src-git packages $FEED_PACKAGES_URL^$FEED_PACKAGES_COMMIT
src-git luci $FEED_LUCI_URL^$FEED_LUCI_COMMIT
EOF
./scripts/feeds update -a
./scripts/feeds install -a

say "config"
cp target/linux/rtl819x/rx23302.diffconfig .config
make defconfig

say "make -j$JOBS"
make -j"$JOBS" download
make -j"$JOBS"

IMG=bin/targets/rtl819x/rtl8197f/openwrt-rtl819x-rtl8197f-electra_rx23302-squashfs-full.bin
say "done"
ls -l "$IMG"
sha256sum "$IMG"
