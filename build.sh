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
. "$HERE/versions.mk.sh"

BUILD=${1:-$HERE/build}
JOBS=${JOBS:-$(nproc 2>/dev/null || echo 2)}

say() { printf '\n=== %s\n' "$*"; }

if [ ! -d "$BUILD/.git" ]; then
	say "OpenWrt $OPENWRT_COMMIT -> $BUILD"
	git clone "$OPENWRT_URL" "$BUILD"
fi

cd "$BUILD"
git checkout -q "$OPENWRT_COMMIT"

if [ ! -f .rx23302-applied ]; then
	say "apply openwrt/ overlay + openwrt-upstream.patch"
	cp -a "$HERE/openwrt/." .
	git apply "$HERE/openwrt-upstream.patch"
	echo "$OPENWRT_COMMIT" > .rx23302-applied
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
