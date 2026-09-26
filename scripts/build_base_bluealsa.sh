#!/usr/bin/env bash
# Build BlueALSA 5 against isolated glibc-compatible dependency stages.
set -euo pipefail
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
work=${BASE_BLUEALSA_BUILD_DIR:-"$repo/scratch/base-upgrade/bluealsa"}
source_dir=${BASE_BLUEALSA_SOURCE:-"$repo/scratch/bluez-alsa-5.0.0"}
cross=${BASE_CROSS_PREFIX:-"$repo/scratch/ingenic-toolchain-v5.2/toolchain/bin/mips-linux-gnu-"}
commit=1935d6dcb8975f2d7a51aaafe61538d157224623
[[ $(git -C "$source_dir" rev-parse HEAD) == "$commit" ]] || { echo 'BlueALSA source revision mismatch' >&2; exit 1; }
git -C "$source_dir" diff --quiet
git -C "$source_dir" diff --cached --quiet
stages=(
    "${BASE_AUDIO_STAGE:-$repo/scratch/base-upgrade/audio/stage}"
    "${BASE_BLUEZ_STAGE:-$repo/scratch/base-upgrade/bluez-library/stage}"
    "${BASE_DBUS_STAGE:-$repo/scratch/base-upgrade/dbus/stage}"
    "${BASE_GLIB_STAGE:-$repo/scratch/base-upgrade/glib/stage}"
    "${BASE_CODECS_STAGE:-$repo/scratch/base-upgrade/codecs/stage}"
    "${BASE_ZLIB_STAGE:-$repo/scratch/base-upgrade/zlib/current}"
    "${BASE_EXPAT_STAGE:-$repo/scratch/base-upgrade/expat/current}"
)
for stage in "${stages[@]}"; do
    [[ -d "$stage/usr/lib" ]] || { echo "Missing dependency stage: $stage" >&2; exit 1; }
done
mkdir -p "$work"
work=$(cd -- "$work" && pwd)
run=$(mktemp -d "$work/run.XXXXXX")
sysroot="$run/sysroot"
mkdir -p "$sysroot/usr" "$run/build" "$run/stage" "$run/src"
# Build a patched copy; the pinned checkout stays clean. Not `git apply`: run
# inside this repository it silently skips every path.
git -C "$source_dir" archive HEAD | tar -x -C "$run/src"
# Byte order, not the locale, decides the order of multiple patches. The list
# is built in a checked step so a failed listing cannot skip patches.
patches=$(LC_ALL=C ls -1 "$repo"/scripts/base_image/bluealsa-*.patch | LC_ALL=C sort)
[[ -n "$patches" ]] || { echo 'No BlueALSA patches found' >&2; exit 1; }
while IFS= read -r fix; do
    (cd "$run/src" && patch -p1 --forward --fuzz=0 --no-backup-if-mismatch < "$fix")
done <<< "$patches"
grep -q 'rtp_a2dp_get_payload(const rtp_header_t \*hdr, size_t len, size_t min_payload)' "$run/src/src/rtp.h" ||
    { echo 'BlueALSA RTP length patch not applied' >&2; exit 1; }
grep -q 'keep_remote_volume' "$run/src/src/bluealsa-dbus.c" ||
    { echo 'BlueALSA sink soft-volume patch not applied' >&2; exit 1; }
grep -q 'Preferring SBC Dual Channel HD' "$run/src/src/a2dp-sbc.c" ||
    { echo 'BlueALSA SBC XQ capability patch not applied' >&2; exit 1; }
for stage in "${stages[@]}"; do cp -a "$stage/usr/." "$sysroot/usr/"; done
# Libtool .la metadata from staged libraries can cause it to embed the
# workstation sysroot as an RPATH. pkg-config supplies the required linkage;
# keep these generated files outside the private link search directory.
mkdir -p "$run/libtool-metadata"
for archive in "$sysroot"/usr/lib/*.la; do
    [[ ! -f "$archive" ]] || mv "$archive" "$run/libtool-metadata/"
done
export CC="${cross}gcc" AR="${cross}ar" NM="${cross}nm" RANLIB="${cross}ranlib"
export STRIP="${cross}strip" LD="${cross}ld" OBJDUMP="${cross}objdump"
export CFLAGS='-Os -EL -mips32r2 -mabi=32 -mhard-float -mfp32'
export CPPFLAGS="-I$sysroot/usr/include -I$sysroot/usr/include/ldac -include $repo/scripts/base_image/ldac_decode_compat.h"
export LDFLAGS="-EL -mips32r2 -mabi=32 -mhard-float -mfp32 -L$sysroot/usr/lib -Wl,-rpath-link,$sysroot/usr/lib -Wl,--no-undefined"
export PKG_CONFIG_LIBDIR="$sysroot/usr/lib/pkgconfig:$sysroot/usr/share/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="$sysroot"
unset PKG_CONFIG_PATH
for package in alsa bluez dbus-1 gio-unix-2.0 glib-2.0 sbc fdk-aac ldacBT-enc ldacBT-abr ldacBT-dec; do
    pkg-config --modversion "$package"
done
aptx_option=--with-libopenaptx
if pkg-config --exists libfreeaptx; then aptx_option=--with-libfreeaptx; fi
(cd "$run/src" && autoreconf --install)
(
    cd "$run/build"
    "$run/src/configure" --host=mips-linux-gnu --prefix=/usr --libdir=/usr/lib \
        --sysconfdir=/etc --localstatedir=/var --disable-static --disable-manpages \
        --with-alsaplugindir=/usr/lib/alsa-lib --with-alsaconfdir=/etc/alsa/conf.d \
        --with-dbusconfdir=/etc/dbus-1/system.d --with-dbus-iface-xml=/usr/share/dbus-1/interfaces \
        --enable-aac --enable-aptx --enable-aptx-hd "$aptx_option" --enable-ldac
    # A successful encoder-only configuration is not sufficient for R1 DAC mode.
    grep -q '^#define HAVE_LDAC_DECODE 1' config.h
    make -j"${JOBS:-4}"
    make DESTDIR="$run/stage" install
)
for elf in "$run/stage"/usr/bin/* "$run/stage"/usr/lib/alsa-lib/*.so; do
    if readelf -d "$elf" | grep -E '(RPATH|RUNPATH).*\[.*/(home|scratch)/' >/dev/null; then
        echo "Host runtime path in $elf" >&2
        exit 1
    fi
done
[[ ! -e "$work/stage" || -L "$work/stage" ]] || { echo 'stage must be a symlink' >&2; exit 1; }
ln -sfnT "$run/stage" "$work/stage"
printf 'BlueALSA 5.0.0 stage: %s\nDependency sysroot: %s\n' "$run/stage" "$sysroot"
