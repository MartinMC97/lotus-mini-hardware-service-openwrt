#!/bin/sh
# Prepare the same pinned user-space SDK used by the original Lotus Mini builds.
set -eu
[ "$#" = 1 ] || { echo 'usage: sh prepare-sdk.sh /absolute/sdk/path' >&2; exit 2; }
sdk=$1
case "$sdk" in /*) ;; *) echo 'SDK path must be absolute' >&2; exit 2 ;; esac
[ ! -e "$sdk" ] || { echo 'SDK destination already exists' >&2; exit 2; }
base=https://downloads.openwrt.org/releases/25.12.0/targets/rockchip/armv8
archive=openwrt-sdk-25.12.0-rockchip-armv8_gcc-14.3.0_musl.Linux-x86_64.tar.zst
archive_sha=8775b11b55bbc139af92abf692ea5248dfa364581fa0e97ee79f66ff24ce2c4d
feeds_sha=724449cc6d7e8cdff082c559b2de63fdb4f90149022d620635083bba78f55d77
download_dir=$(mktemp -d "${TMPDIR:-/tmp}/lotus-sdk.XXXXXX")
trap 'rm -rf "$download_dir"' EXIT HUP INT TERM
curl --fail --location --retry 3 --output "$download_dir/$archive" "$base/$archive"
curl --fail --location --retry 3 --output "$download_dir/feeds.buildinfo" "$base/feeds.buildinfo"
printf '%s  %s\n' "$archive_sha" "$download_dir/$archive" | sha256sum --check --status
printf '%s  %s\n' "$feeds_sha" "$download_dir/feeds.buildinfo" | sha256sum --check --status
mkdir -p "$sdk"
tar --zstd -xf "$download_dir/$archive" -C "$sdk" --strip-components=1
# Keep the SDK's base source revision, and only fetch the two additional feeds
# required by the daemon and LuCI. feeds.buildinfo pins their release revisions.
awk '$1 ~ /^src-/ && ($2 == "base" || $3 == "base") { print }' "$sdk/feeds.conf.default" > "$sdk/feeds.conf"
awk '$2 == "packages" || $2 == "luci" { print }' "$download_dir/feeds.buildinfo" >> "$sdk/feeds.conf"
grep -q ' base .*\^[0-9a-f]' "$sdk/feeds.conf"
grep -q ' packages .*\^[0-9a-f]' "$sdk/feeds.conf"
grep -q ' luci .*\^[0-9a-f]' "$sdk/feeds.conf"
cd "$sdk"
./scripts/feeds update -a
# Install only the relevant recipes and their dependency closure. Installing
# every SDK recipe also enables unrelated bootloader/kernel prerequisites.
./scripts/feeds install alsa-lib luci-base rpcd
