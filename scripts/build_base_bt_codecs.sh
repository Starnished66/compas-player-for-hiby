#!/usr/bin/env bash
# Rebuild AAC/aptX and the LDAC decoder; retain the device's working LDAC
# encoder/ABR pair.
set -euo pipefail
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
base="$repo/scratch/base-upgrade"
work=${BASE_CODECS_BUILD_DIR:-"$base/codecs"}
stock=${BASE_STOCK_ROOT:-/home/josegarita/Desktop/Test2/squashfs-root}
cross=${BASE_CROSS_PREFIX:-"$repo/scratch/ingenic-toolchain-v5.2/toolchain/bin/mips-linux-gnu-"}
check_source() {
    [[ $(git -C "$1" rev-parse HEAD) == "$2" ]] || { echo "Source revision mismatch: $1" >&2; exit 1; }
    git -C "$1" diff --quiet
    git -C "$1" diff --cached --quiet
}
check_source "$base/fdk-aac" 716f4394641d53f0d79c9ddac3fa93b03a49f278
check_source "$base/libopenaptx" 2459ed4686eaef0a19dfa3f330a960813c5f60de
check_source "$base/ldacBT" 6579bd585a618f2e1612b3c1650d2b7fcfb1d43f
check_source "$base/ldacBT/libldac" 82b6a1abee84787b8fa167efe20290073f60db2d
check_source "$base/libldacdec-anonymix007" c90094b15e25aef0e47c6d775fa94aceb36cabbc
mkdir -p "$work"
work=$(cd -- "$work" && pwd)
run=$(mktemp -d "$work/run.XXXXXX")
stage="$run/stage"
mkdir -p "$stage/usr/lib/pkgconfig" "$stage/usr/include/ldac" "$run/aptx"
flags='-Os -EL -mips32r2 -mabi=32 -mhard-float -mfp32'
cmake -S "$base/fdk-aac" -B "$run/aac" -DCMAKE_SYSTEM_NAME=Linux \
    -DCMAKE_C_COMPILER="${cross}gcc" -DCMAKE_CXX_COMPILER="${cross}g++" \
    -DCMAKE_C_FLAGS="$flags" -DCMAKE_CXX_FLAGS="$flags" \
    -DCMAKE_SHARED_LINKER_FLAGS="$flags -Wl,--no-undefined" \
    -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_BUILD_TYPE=MinSizeRel \
    -DBUILD_SHARED_LIBS=ON -DBUILD_PROGRAMS=OFF
cmake --build "$run/aac" --parallel "${JOBS:-4}"
DESTDIR="$stage" cmake --install "$run/aac"
# Build from an isolated copy; do not dirty the pinned source checkout.
git -C "$base/libopenaptx" archive HEAD | tar -x -C "$run/aptx"
make -C "$run/aptx" -j"${JOBS:-4}" CC="${cross}gcc" AR="${cross}ar" \
    CFLAGS="$flags" LDFLAGS="$flags" PREFIX=/usr
make -C "$run/aptx" CC="${cross}gcc" AR="${cross}ar" \
    CFLAGS="$flags" LDFLAGS="$flags" PREFIX=/usr DESTDIR="$stage" install
# pkg-config prefixes this upstream rpath with the build sysroot, leaking
# a workstation path into consumers. The normal target loader finds /usr/lib.
sed -i 's/-Wl,-rpath=${libdir} //g' "$stage/usr/lib/pkgconfig/libopenaptx.pc"
for library in libldacBT_enc.so.2 libldacBT_abr.so.2; do
    cp -L "$stock/usr/lib/$library" "$stage/usr/lib/$library"
done
# LDAC decoder from source: the stock libldacdec.so.1 crashes about a second
# into real LDAC reception (DAC mode). Same soname, exports and ABI as the
# stock build (MIPS32r2 hard-float, double precision); the patch bounds every
# read to the packet, validates stream fields, rejects frames that disagree
# with the negotiated format, and fixes S32 gain and scale-factor decoding.
mkdir -p "$run/ldacdec"
git -C "$base/libldacdec-anonymix007" archive HEAD | tar -x -C "$run/ldacdec"
# Not `git apply`: run inside this repository it silently skips every path.
(cd "$run/ldacdec" && patch -p1 --forward --fuzz=0 --no-backup-if-mismatch \
    < "$repo/scripts/base_image/libldacdec-hiby.patch")
grep -q 'ldacDecode_type_len' "$run/ldacdec/libldacdec.c" ||
    { echo 'LDAC decoder patch not applied' >&2; exit 1; }
(
    cd "$run/ldacdec"
    "${cross}gcc" ${flags/-Os/-O2} -fPIC -shared -DDOUBLE64 -std=gnu11 -Wall \
        -I"$base/ldacBT/libldac/inc" -I"$base/ldacBT/libldac/src" \
        -Wl,-soname,libldacdec.so.1 -Wl,--no-undefined \
        libldacdec.c bit_allocation.c huffCodes.c bit_reader.c utility.c imdct.c spectrum.c \
        -lm -lpthread -o "$stage/usr/lib/libldacdec.so.1"
    "${cross}strip" --strip-unneeded "$stage/usr/lib/libldacdec.so.1"
)
ln -s libldacBT_enc.so.2 "$stage/usr/lib/libldacBT_enc.so"
ln -s libldacBT_abr.so.2 "$stage/usr/lib/libldacBT_abr.so"
ln -s libldacdec.so.1 "$stage/usr/lib/libldacdec.so"
cp "$base/ldacBT/libldac/inc/ldacBT.h" "$stage/usr/include/ldac/"
cp "$base/ldacBT/libldac/abr/inc/ldacBT_abr.h" "$stage/usr/include/ldac/"
# Local developer metadata for the 2.x LDAC ABI (retained encoder/ABR, rebuilt
# decoder), not a software release/version claim. No vendor binary is
# redistributed in git.
for part in enc abr dec; do
    library="ldacBT_$part"
    [[ $part != dec ]] || library=ldacdec
    printf 'prefix=/usr\nlibdir=${prefix}/lib\nincludedir=${prefix}/include/ldac\nName: ldacBT-%s\nDescription: retained HiBy LDAC ABI\nVersion: 2.0.0\nLibs: -L${libdir} -l%s\nCflags: -I${includedir}\n' \
        "$part" "$library" > "$stage/usr/lib/pkgconfig/ldacBT-$part.pc"
done
[[ ! -e "$work/stage" || -L "$work/stage" ]] || { echo 'stage must be a symlink' >&2; exit 1; }
ln -sfnT "$stage" "$work/stage"
printf 'Codec developer stage: %s\nLDAC encoder/ABR retained; decoder rebuilt from source.\n' "$stage"
