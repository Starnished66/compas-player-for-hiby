#!/usr/bin/env bash
# Prepare a minimal runtime overlay. Does not modify the input firmware tree.
set -euo pipefail
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
base="$repo/scratch/base-upgrade"
stock=${BASE_STOCK_ROOT:-/home/josegarita/Desktop/Test2/squashfs-root}
mkdir -p "$base/overlays"
run=$(mktemp -d "$base/overlays/run.XXXXXX")
overlay="$run/root"
mkdir -p "$overlay/usr/lib/alsa-lib" "$overlay/usr/bin" "$overlay/usr/libexec" "$overlay/etc/alsa/conf.d" \
    "$overlay/etc/dbus-1/system.d" "$overlay/usr/share/alsa/alsa.conf.d" \
    "$overlay/usr/share/dbus-1/interfaces"
copy_libraries() {
    local stage=$1
    shift
    for stem in "$@"; do
        [[ -e "$stage/usr/lib/$stem.so" ]] || { echo "Missing library: $stage/$stem" >&2; exit 1; }
        cp -a "$stage/usr/lib/$stem".so* "$overlay/usr/lib/"
    done
}
copy_libraries "$base/audio/stage" libasound libatopology libsbc libspeexdsp
copy_libraries "$base/zlib/current" libz
copy_libraries "$base/expat/current" libexpat
copy_libraries "$base/bluez-library/stage" libbluetooth
copy_libraries "$base/dbus/stage" libdbus-1
copy_libraries "$base/glib/stage" libglib-2.0 libgobject-2.0 libgio-2.0 libgmodule-2.0 libgthread-2.0 libffi libpcre2-8
copy_libraries "$base/codecs/stage" libfdk-aac libopenaptx libldacdec
for binary in bluealsad bluealsactl bluealsa-aplay; do
    cp "$base/bluealsa/stage/usr/bin/$binary" "$overlay/usr/bin/"
done
# libdbus's private ABI changes between releases: update its own tools and
# daemon together, while retaining all device bus policies and init scripts.
for binary in "$base/dbus/stage/usr/bin/"*; do
    cp "$binary" "$overlay/usr/bin/"
done
cp "$base/dbus/stage/usr/libexec/dbus-daemon-launch-helper" "$overlay/usr/libexec/"
chmod --reference="$stock/usr/libexec/dbus-daemon-launch-helper" "$overlay/usr/libexec/dbus-daemon-launch-helper"
cp "$base/bluealsa/stage/usr/lib/alsa-lib/"*.so "$overlay/usr/lib/alsa-lib/"
# Speex rate converter for Bluetooth output. An A2DP transport's rate is
# fixed at connect time, so tracks at any other rate are converted rather
# than renegotiated; without this alsa-lib uses its built-in linear
# converter. cp -a keeps the quality-variant symlinks.
cp -a "$base/audio/stage/usr/lib/alsa-lib/libasound_module_rate_speexrate"*.so \
    "$overlay/usr/lib/alsa-lib/"
cp "$base/bluealsa/stage/usr/share/alsa/alsa.conf.d/20-bluealsa.conf" "$overlay/usr/share/alsa/alsa.conf.d/"
ln -s /usr/share/alsa/alsa.conf.d/20-bluealsa.conf "$overlay/etc/alsa/conf.d/20-bluealsa.conf"
cp "$base/bluealsa/stage/etc/dbus-1/system.d/org.bluealsa.conf" "$overlay/etc/dbus-1/system.d/"
cp "$base/bluealsa/stage/usr/share/dbus-1/interfaces/org.bluealsa.xml" "$overlay/usr/share/dbus-1/interfaces/"
for script in bt_init bt_resume bluealsa_profile; do
    [[ -f "$stock/usr/bin/$script" ]] || { echo "Missing stock script $script" >&2; exit 1; }
    # Do not rename PCM type bluealsa or the bluealsa_profile script itself.
    sed -e 's|/usr/bin/bluealsa |/usr/bin/bluealsad |g' \
        -e 's/ --a2dp-volume//g' -e 's/pgrep "bluealsa"/pgrep "bluealsad"/g' \
        -e '/^killall/s/\bbluealsa\b/bluealsad/g' \
        "$stock/usr/bin/$script" > "$overlay/usr/bin/$script"
    chmod --reference="$stock/usr/bin/$script" "$overlay/usr/bin/$script"
    sh -n "$overlay/usr/bin/$script"
done
[[ -f "$repo/compas_player_target" ]] || { echo 'Run make target first' >&2; exit 1; }
cp "$repo/compas_player_target" "$overlay/usr/bin/compas_player"
# Keep upstream notices with the runtime. Source archives/checkouts stay in
# scratch for release source bundles; no vendor LDAC binaries enter git.
licenses="$overlay/usr/share/licenses/base-upgrade"
copy_notice() {
    local package=$1
    shift
    mkdir -p "$licenses/$package"
    cp -a "$@" "$licenses/$package/"
}
copy_notice bluealsa "$repo/scratch/bluez-alsa-5.0.0/LICENSE" "$repo/scratch/bluez-alsa-5.0.0/LICENSES"
copy_notice alsa "$base/audio/src/alsa-lib-1.2.16/COPYING"
copy_notice sbc "$base/audio/src/sbc-2.2/COPYING" "$base/audio/src/sbc-2.2/COPYING.LIB"
copy_notice zlib "$(dirname "$(readlink -f "$base/zlib/current")")/src/zlib-1.3.2/LICENSE"
copy_notice expat "$base/expat/src/expat-2.8.4/COPYING"
glib_src="$(dirname "$(readlink -f "$base/glib/stage")")/src"
copy_notice glib "$glib_src/glib-2.84.4/COPYING" "$glib_src/glib-2.84.4/LICENSES"
copy_notice pcre2 "$glib_src/pcre2-10.46/COPYING"
copy_notice libffi "$glib_src/libffi-3.4.8/LICENSE"
copy_notice dbus "$repo/dbus/COPYING"
bluez_src="$(dirname "$(readlink -f "$base/bluez-library/stage")")/bluez-5.87"
copy_notice bluez "$bluez_src/COPYING" "$bluez_src/COPYING.LIB"
copy_notice fdk-aac "$base/fdk-aac/NOTICE"
copy_notice libopenaptx "$base/libopenaptx/COPYING"
# Upstream publishes no license file; ship its README and our patch with it.
copy_notice libldacdec "$base/libldacdec-anonymix007/README.md" \
    "$repo/scripts/base_image/libldacdec-hiby.patch"
cross=${BASE_CROSS_PREFIX:-"$repo/scratch/ingenic-toolchain-v5.2/toolchain/bin/mips-linux-gnu-"}
while IFS= read -r -d '' elf; do
    [[ $(head -c 4 "$elf") == $'\177ELF' ]] || continue
    "${cross}strip" --strip-unneeded "$elf"
    if readelf -d "$elf" | grep -E '(RPATH|RUNPATH).*\[.*/(home|scratch)/' >/dev/null; then
        echo "Host runtime path in $elf" >&2
        exit 1
    fi
done < <(find "$overlay/usr" -type f -print0)
# The rebuilt LDAC decoder must be what ships: the vendor one crashes in DAC mode.
cmp -s "$overlay/usr/lib/libldacdec.so.1" "$base/codecs/stage/usr/lib/libldacdec.so.1" ||
    { echo 'Overlay LDAC decoder differs from the codec stage' >&2; exit 1; }
(cd "$overlay" && find . \( -type f -o -type l \) -print | LC_ALL=C sort) > "$run/files.txt"
[[ ! -e "$base/overlay" || -L "$base/overlay" ]] || { echo 'overlay must be a symlink' >&2; exit 1; }
ln -sfnT "$overlay" "$base/overlay"
printf 'Runtime overlay: %s\nManifest: %s/files.txt\nTest2 has not been modified.\n' "$overlay" "$run"
