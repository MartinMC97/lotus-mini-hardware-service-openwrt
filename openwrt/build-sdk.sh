#!/bin/sh
set -eu
[ "$#" = 1 ] || { echo 'usage: sh openwrt/build-sdk.sh /path/to/sdk' >&2; exit 2; }
repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
sdk=$(CDPATH= cd -- "$1" && pwd)
[ -f "$sdk/include/package.mk" ] || exit 2
mkdir -p "$repo/openwrt/feed/lotus-hardware-service/src" "$repo/openwrt/artifacts"
tar -czf "$repo/openwrt/feed/lotus-hardware-service/src/source.tar.gz" -C "$repo" Makefile src third_party profiles
ln -sfn "$repo/openwrt/feed/lotus-hardware-service" "$sdk/package/lotus-hardware-service"
cd "$sdk"
./scripts/feeds install alsa-lib
./scripts/kconfig.pl + .config "$repo/openwrt/sdk.config" > tmp/lotus-hardware.config
cp tmp/lotus-hardware.config .config
make defconfig
make -j2 package/lotus-hardware-service/compile CONFIG_AUTOREMOVE= V=s
find bin/packages -name 'lotus-hardware-service-*.apk' -exec cp {} "$repo/openwrt/artifacts/" \;
