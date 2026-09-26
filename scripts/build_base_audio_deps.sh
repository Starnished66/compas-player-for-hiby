#!/usr/bin/env bash
# Build ABI-compatible ALSA/SBC libraries; never install into the firmware tree.
set -euo pipefail
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
work=${BASE_AUDIO_BUILD_DIR:-"$repo/scratch/base-upgrade/audio"}
cross=${BASE_CROSS_PREFIX:-"$repo/scratch/ingenic-toolchain-v5.2/toolchain/bin/mips-linux-gnu-"}
mkdir -p "$work/downloads" "$work/src" "$work/build" "$work/stage"
work=$(cd -- "$work" && pwd)
export CC="${cross}gcc" AR="${cross}ar" RANLIB="${cross}ranlib" STRIP="${cross}strip"
export NM="${cross}nm" LD="${cross}ld" OBJDUMP="${cross}objdump"
export CFLAGS='-Os -EL -mips32r2 -mabi=32 -mhard-float -mfp32'
# The staged lib dir is on the link path so packages built later here can
# link the ones built earlier (alsa-plugins needs -lasound and -lspeexdsp).
export LDFLAGS="-EL -mips32r2 -mabi=32 -mhard-float -mfp32 -L$work/stage/usr/lib"
# Do not discover host libraries when configuring target packages.
export PKG_CONFIG_LIBDIR="$work/stage/usr/lib/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="$work/stage"
unset PKG_CONFIG_PATH
"$CC" --version
fetch() {
    local package=$1 url=$2 checksum=$3 archive="$work/downloads/${2##*/}"
    if [[ ! -f "$archive" ]]; then
        curl --fail --location --retry 3 "$url" -o "$archive.part"
        mv -- "$archive.part" "$archive"
    fi
    printf '%s  %s\n' "$checksum" "$archive" | sha256sum --check --status
    if [[ ! -d "$work/src/$package" ]]; then
        tar -xf "$archive" -C "$work/src"
    fi
}
build_autoconf() {
    local package=$1
    shift
    mkdir -p "$work/build/$package"
    (
        cd "$work/build/$package"
        "$work/src/$package/configure" --host=mips-linux-gnu --prefix=/usr \
            --libdir=/usr/lib --sysconfdir=/etc --disable-static "$@"
        make -j"${JOBS:-4}"
        make DESTDIR="$work/stage" install
    )
}
fetch sbc-2.2 https://www.kernel.org/pub/linux/bluetooth/sbc-2.2.tar.xz \
    a1ada76ef35e5af9c2fbd063754dc9e37a8d989417c6eb1ecebb089b1383ae9e
build_autoconf sbc-2.2 --disable-tools --disable-tester
fetch alsa-lib-1.2.16 https://www.alsa-project.org/files/pub/lib/alsa-lib-1.2.16.tar.bz2 \
    122b1e3166d55fe19bcde656535d7a36f2ab10e66c72c6ad2f43f20ffded0a96
# The vendor libasound exports unversioned symbols, including plugin helpers.
build_autoconf alsa-lib-1.2.16 --disable-python --without-versioned
# Resampler for Bluetooth output. An A2DP transport's rate is fixed at
# connect time, so a track at any other rate has to be converted; without
# this plugin alsa-lib falls back to its built-in linear converter. The
# player pins its BlueALSA PCM's rate rather than letting BlueALSA
# renegotiate, which would recreate the transport and drop the accessory.
fetch speexdsp-1.2.1 https://downloads.xiph.org/releases/speex/speexdsp-1.2.1.tar.gz \
    8c777343e4a6399569c72abc38a95b24db56882c83dbdb6c6424a5f4aeb54d3d
build_autoconf speexdsp-1.2.1 --disable-examples
fetch alsa-plugins-1.2.12 https://www.alsa-project.org/files/pub/plugins/alsa-plugins-1.2.12.tar.bz2 \
    7bd8a83d304e8e2d86a25895d8dcb0ef0245a8df32e271959cdbdc6af39b66f2
# Only the speexdsp rate converter is wanted; every other plugin here pulls
# in libraries this firmware does not carry. The plugin directory is created
# up front because the package's install hook symlinks the quality variants
# into it before anything else has made it.
mkdir -p "$work/stage/usr/lib/alsa-lib"
# Libtool .la metadata on the link path makes it embed this workstation's
# stage directory as the plugin's RPATH (see build_base_bluealsa.sh). The
# pkg-config and -L flags already supply the linkage.
mkdir -p "$work/libtool-metadata"
for archive in "$work"/stage/usr/lib/*.la; do
    [[ ! -f "$archive" ]] || mv "$archive" "$work/libtool-metadata/"
done
build_autoconf alsa-plugins-1.2.12 --disable-oss --disable-mix --disable-usbstream \
    --disable-arcamav --disable-jack --disable-pulseaudio --disable-samplerate \
    --disable-libav --disable-a52 --disable-lavrate --disable-aaf
for elf in "$work"/stage/usr/lib/*.so* "$work"/stage/usr/lib/alsa-lib/*.so; do
    [[ -f "$elf" && ! -L "$elf" ]] || continue
    if readelf -d "$elf" | grep -E '(RPATH|RUNPATH).*\[.*/(home|scratch)/' >/dev/null; then
        echo "Host runtime path in $elf" >&2
        exit 1
    fi
done
printf 'Staged audio libraries: %s\n' "$work/stage"
printf 'Not installed into Test2; retain device ALSA configuration during integration.\n'
