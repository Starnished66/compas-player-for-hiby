#!/usr/bin/env bash
# Cross-build BlueZ daemon and device-facing tools into scratch only.
# The public libbluetooth library is built by build_base_bluez_library.sh;
# this recipe deliberately does not install a second copy of that library.
set -euo pipefail

readonly SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
readonly REPO_ROOT=$(cd -- "$SCRIPT_DIR/.." && pwd)
readonly BLUEZ_VERSION=5.87
readonly BLUEZ_SHA256=26bdcf2cebd7310c6f598850606b037ef0c515fe6608ebc54d22c50c4c32b35f
readonly BLUEZ_URL=https://www.kernel.org/pub/linux/bluetooth/bluez-5.87.tar.xz
readonly BLUEZ_STORAGE_DIR=/usr/data/bluetooth
readonly READLINE_VERSION=8.0
readonly READLINE_SHA256=e339f51971478d369f8a053a330a190781acb9864cf4c541060f12078948e461
readonly READLINE_URL=https://ftp.gnu.org/gnu/readline/readline-8.0.tar.gz
readonly TARGET_FLAGS='-EL -mips32r2 -mabi=32 -mhard-float -mfp32'
readonly STOCK_ROOT=${BASE_STOCK_ROOT:-/home/josegarita/Desktop/Test2/squashfs-root}
readonly GLIB_STAGE=${BASE_GLIB_STAGE:-$REPO_ROOT/scratch/base-upgrade/glib/stage}
readonly DBUS_STAGE=${BASE_DBUS_STAGE:-$REPO_ROOT/scratch/base-upgrade/dbus/stage}
readonly CROSS_PREFIX=${BASE_CROSS_PREFIX:-$REPO_ROOT/scratch/ingenic-toolchain-v5.2/toolchain/bin/mips-linux-gnu-}
work=${BASE_BLUEZ_BUILD_DIR:-$REPO_ROOT/scratch/base-upgrade/bluez}
source_archive=${BASE_BLUEZ_SOURCE_ARCHIVE:-$REPO_ROOT/scratch/base-upgrade/bluez-library/downloads/bluez-5.87.tar.xz}
jobs=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)}

die() { echo "build_base_bluez.sh: $*" >&2; exit 1; }
usage() { echo "Usage: $0 [--build-dir DIR] [--source-archive FILE] [--jobs N]" >&2; }

while (($#)); do
    case $1 in
        --build-dir) (($# >= 2)) || { usage; exit 2; }; work=$2; shift 2 ;;
        --source-archive) (($# >= 2)) || { usage; exit 2; }; source_archive=$2; shift 2 ;;
        --jobs) (($# >= 2)) || { usage; exit 2; }; jobs=$2; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) usage; die "unknown option: $1" ;;
    esac
done

[[ $jobs =~ ^[1-9][0-9]*$ ]] || die "jobs must be a positive integer: $jobs"
[[ -d $STOCK_ROOT/usr/lib ]] || die "stock root is missing usr/lib: $STOCK_ROOT"
[[ -d $GLIB_STAGE/usr/include && -d $GLIB_STAGE/usr/lib/pkgconfig ]] || die "GLib stage is incomplete: $GLIB_STAGE"
[[ -d $DBUS_STAGE/usr/include && -d $DBUS_STAGE/usr/lib/pkgconfig ]] || die "D-Bus stage is incomplete: $DBUS_STAGE"
for tool in curl sha256sum tar make pkg-config readelf file rg mktemp; do
    command -v "$tool" >/dev/null || die "$tool is required"
done

cc="${CROSS_PREFIX}gcc"
ar="${CROSS_PREFIX}ar"
ranlib="${CROSS_PREFIX}ranlib"
strip="${CROSS_PREFIX}strip"
for tool in "$cc" "$ar" "$ranlib" "$strip"; do
    [[ -x $tool ]] || die "missing cross tool: $tool"
done

[[ ! -L $work ]] || die "build directory must not be a symlink: $work"
mkdir -p -- "$work/downloads"
work=$(cd -- "$work" && pwd)
source_archive=$(cd -- "$(dirname -- "$source_archive")" && pwd)/$(basename -- "$source_archive")
if [[ ! -f $source_archive ]]; then
    source_archive="$work/downloads/bluez-$BLUEZ_VERSION.tar.xz"
    if [[ ! -f $source_archive ]]; then
        curl --fail --location --proto '=https' --proto-redir '=https' --silent --show-error \
            --retry 3 "$BLUEZ_URL" --output "$source_archive.part"
        mv -- "$source_archive.part" "$source_archive"
    fi
fi
[[ ! -L $source_archive ]] || die "source archive must not be a symlink: $source_archive"
printf '%s  %s\n' "$BLUEZ_SHA256" "$source_archive" | sha256sum --check --status \
    || die "SHA256 mismatch: $source_archive"

readonly RUN=$(mktemp -d "$work/run.XXXXXX")
readonly SOURCE_ROOT=$RUN/src
readonly BUILD=$RUN/build
readonly STAGE=$RUN/stage
readonly SYSROOT=$RUN/sysroot
readonly CROSS_FILE=$RUN/cross-file.ini
readonly STABLE_STAGE=$work/stage
mkdir -p -- "$SOURCE_ROOT" "$BUILD" "$STAGE" "$SYSROOT/usr/include" "$SYSROOT/usr/lib/pkgconfig"
tar -xf "$source_archive" -C "$SOURCE_ROOT"
readonly SOURCE="$SOURCE_ROOT/bluez-$BLUEZ_VERSION"
[[ -d $SOURCE && -f $SOURCE/configure ]] || die "archive did not contain bluez-$BLUEZ_VERSION"
# Local fixes, in byte order. Not `git apply`: run inside this repository it
# silently skips every path.
bluez_patches=$(LC_ALL=C ls -1 "$REPO_ROOT"/scripts/base_image/bluez-*.patch | LC_ALL=C sort)
[[ -n $bluez_patches ]] || die 'no BlueZ patches found'
while IFS= read -r fix; do
    (cd "$SOURCE" && patch -p1 --forward --fuzz=0 --no-backup-if-mismatch < "$fix")
done <<< "$bluez_patches"
for patched in a2dp.c avdtp.c; do
    grep -q 'BT_IO_OPT_IMTU, 1024' "$SOURCE/profiles/audio/$patched" ||
        die "BlueZ AVDTP MTU patch not applied to $patched"
done
grep -q 'handle_transport_connect(session, chan, imtu, omtu);' "$SOURCE/profiles/audio/avdtp.c" ||
    die 'BlueZ AVDTP media MTU patch not applied'

# bluetoothctl requires readline headers, but the vendor rootfs contains only
# its ABI-compatible runtime library. Prefer host headers when available;
# otherwise fetch the matching GNU Readline 8.0 source headers. In either case,
# copy only the header files into this disposable target sysroot, never add a
# host include directory to the target compiler search path.
readline_include=${BASE_BLUEZ_READLINE_INCLUDE:-}
if [[ ! -f $readline_include/readline/readline.h || ! -f $readline_include/readline/history.h ]]; then
    readline_archive="$work/downloads/readline-$READLINE_VERSION.tar.gz"
    if [[ ! -f $readline_archive ]]; then
        curl --fail --location --proto '=https' --proto-redir '=https' --silent --show-error \
            --retry 3 "$READLINE_URL" --output "$readline_archive.part"
        mv -- "$readline_archive.part" "$readline_archive"
    fi
    [[ ! -L $readline_archive ]] || die "Readline archive must not be a symlink: $readline_archive"
    printf '%s  %s\n' "$READLINE_SHA256" "$readline_archive" | sha256sum --check --status \
        || die "SHA256 mismatch: $readline_archive"
    tar -xzf "$readline_archive" -C "$RUN"
    readline_headers=("$RUN/readline-$READLINE_VERSION/"*.h)
else
    readline_headers=("$readline_include"/readline/*.h)
fi
[[ -f ${readline_headers[0]} ]] || die 'readline development headers are missing'
mkdir -p -- "$SYSROOT/usr/include/readline"
cp -a "${readline_headers[@]}" "$SYSROOT/usr/include/readline/"
[[ -f $STOCK_ROOT/usr/lib/libreadline.so ]] || die "vendor libreadline.so is missing: $STOCK_ROOT/usr/lib"
[[ -f $STOCK_ROOT/usr/lib/libncurses.so ]] || die "vendor libncurses.so is missing: $STOCK_ROOT/usr/lib"

# Make one disposable pkg-config sysroot. This prevents the /usr prefixes in
# the staged GLib/D-Bus .pc files from resolving to host libraries.
ln -s "$GLIB_STAGE/usr/include/glib-2.0" "$SYSROOT/usr/include/glib-2.0"
ln -s "$GLIB_STAGE/usr/lib/glib-2.0" "$SYSROOT/usr/lib/glib-2.0"
ln -s "$DBUS_STAGE/usr/include/dbus-1.0" "$SYSROOT/usr/include/dbus-1.0"
ln -s "$DBUS_STAGE/usr/lib/dbus-1.0" "$SYSROOT/usr/lib/dbus-1.0"
for header in ffi.h ffitarget.h; do
    [[ -e "$GLIB_STAGE/usr/include/$header" ]] && ln -s "$GLIB_STAGE/usr/include/$header" "$SYSROOT/usr/include/$header"
done
for lib in "$GLIB_STAGE"/usr/lib/lib*.so* "$DBUS_STAGE"/usr/lib/lib*.so*; do
    [[ -e $lib ]] || continue
    ln -s "$lib" "$SYSROOT/usr/lib/$(basename -- "$lib")"
done
for pc in "$GLIB_STAGE"/usr/lib/pkgconfig/*.pc "$DBUS_STAGE"/usr/lib/pkgconfig/*.pc; do
    [[ -f $pc ]] || continue
    ln -s "$pc" "$SYSROOT/usr/lib/pkgconfig/$(basename -- "$pc")"
done

readonly CFLAGS="$TARGET_FLAGS -Os -fPIC -fno-PIE -I$SYSROOT/usr/include"
readonly LDFLAGS="$TARGET_FLAGS -fno-PIE -L$SYSROOT/usr/lib -L$STOCK_ROOT/usr/lib -Wl,-rpath-link,$SYSROOT/usr/lib -Wl,-rpath-link,$STOCK_ROOT/usr/lib -Wl,-z,noexecstack"
export CC="$cc" AR="$ar" RANLIB="$ranlib" STRIP="$strip"
export CFLAGS CPPFLAGS="$TARGET_FLAGS -fno-PIE -I$SYSROOT/usr/include"
export CXXFLAGS="$CFLAGS" LDFLAGS
export PKG_CONFIG_LIBDIR="$SYSROOT/usr/lib/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="$SYSROOT"
unset PKG_CONFIG_PATH

vendor_plugins_dir="$STOCK_ROOT/usr/lib/bluetooth/plugins"
vendor_plugin=$(find "$vendor_plugins_dir" -type f -name '*.so' -print -quit 2>/dev/null || true)
external_plugins_args=(--disable-external-plugins)
if [[ -n $vendor_plugin ]]; then
    echo "Preserving external BlueZ plugin loading for vendor plugin: $vendor_plugin" >&2
    external_plugins_args=(--enable-external-plugins)
else
    echo "No vendor external BlueZ plugins found under $vendor_plugins_dir; external loading disabled." >&2
fi

printf '%s\n' \
    '[binaries]' \
    "c = '$cc'" "ar = '$ar'" "ranlib = '$ranlib'" "strip = '$strip'" \
    "pkg-config = 'pkg-config'" '' \
    '[properties]' "sys_root = '$SYSROOT'" 'needs_exe_wrapper = true' '' \
    '[host_machine]' "system = 'linux'" "subsystem = 'linux'" "kernel = 'linux'" \
    "cpu_family = 'mips'" "cpu = 'mips32r2'" "endian = 'little'" > "$CROSS_FILE"

pushd "$BUILD" >/dev/null
env CFLAGS="$CFLAGS" CPPFLAGS="$CPPFLAGS" CXXFLAGS="$CXXFLAGS" LDFLAGS="$LDFLAGS" \
    "$SOURCE/configure" --host=mips-linux-gnu --build="$(gcc -dumpmachine)" \
    --prefix=/usr --exec-prefix=/usr --libdir=/usr/lib --libexecdir=/usr/libexec \
    --sysconfdir=/etc --localstatedir=/var --with-dbusconfdir=/etc/dbus-1 \
    --with-dbussystembusdir=/usr/share/dbus-1/system-services \
    --with-dbussessionbusdir=/usr/share/dbus-1/services \
    --disable-library --enable-tools --enable-client --enable-monitor \
    --enable-deprecated --disable-udev --disable-cups --disable-obex \
    --disable-mesh --disable-btpclient --disable-midi --disable-manpages \
    --disable-bap --disable-bass --disable-mcp --disable-ccp --disable-vcp \
    --disable-micp --disable-csip --disable-tmap --disable-gmap --disable-asha \
    --disable-testing --enable-experimental "${external_plugins_args[@]}" \
    --disable-systemd
[[ -f $BUILD/config.h ]] || die 'BlueZ configure did not produce config.h'
readonly BLUEZ_CONFIGURED_STORAGE_DIR=/var/lib/bluetooth
[[ $(grep -F -c "#define STORAGEDIR \"$BLUEZ_CONFIGURED_STORAGE_DIR\"" "$BUILD/config.h") -eq 1 ]] ||
    die "unexpected BlueZ STORAGEDIR in $BUILD/config.h"
# Keep --localstatedir=/var for unrelated state paths; redirect only BlueZ's
# pairing database to the device's persistent stock directory.
sed -i "s|#define STORAGEDIR \"$BLUEZ_CONFIGURED_STORAGE_DIR\"|#define STORAGEDIR \"$BLUEZ_STORAGE_DIR\"|" "$BUILD/config.h"
grep -F "#define STORAGEDIR \"$BLUEZ_STORAGE_DIR\"" "$BUILD/config.h" >/dev/null ||
    die "BlueZ STORAGEDIR is not $BLUEZ_STORAGE_DIR"
if grep -F "#define STORAGEDIR \"$BLUEZ_CONFIGURED_STORAGE_DIR\"" "$BUILD/config.h" >/dev/null; then
    die 'BlueZ config still contains /var/lib/bluetooth'
fi
make -j"$jobs"
make install DESTDIR="$STAGE"
popd >/dev/null

# gatttool is deliberately deprecated and therefore remains a noinst target;
# the vendor image ships it, so install only that binary into the disposable
# BlueZ stage when the target was built.
if [[ -x $BUILD/attrib/gatttool ]]; then
    install -D -m 0755 "$BUILD/attrib/gatttool" "$STAGE/usr/bin/gatttool"
fi

readonly DAEMON="$STAGE/usr/libexec/bluetooth/bluetoothd"
readonly REQUIRED_TOOLS=(bluetoothctl btmon btattach rctest l2test l2ping bluemoon hex2hcd isotest \
    hciattach hciconfig hcitool rfcomm sdptool ciptool gatttool)
[[ -x $DAEMON ]] || die "missing staged daemon: $DAEMON"
daemon_strings=$(strings "$DAEMON") || die 'unable to inspect staged bluetoothd strings'
printf '%s\n' "$daemon_strings" | grep -Fx "$BLUEZ_STORAGE_DIR" >/dev/null ||
    die "staged bluetoothd does not contain $BLUEZ_STORAGE_DIR"
if printf '%s\n' "$daemon_strings" | grep -Fx "$BLUEZ_CONFIGURED_STORAGE_DIR" >/dev/null; then
    die 'staged bluetoothd still contains /var/lib/bluetooth'
fi
if printf '%s\n' "$daemon_strings" | grep -Fx "$BLUEZ_STORAGE_DIR/lib/bluetooth" >/dev/null; then
    die 'staged bluetoothd contains an unintended extra /lib/bluetooth suffix'
fi
for binary in "${REQUIRED_TOOLS[@]}"; do
    [[ -x "$STAGE/usr/bin/$binary" ]] || die "missing staged tool: $binary"
done

mkdir -p -- "$STAGE/usr/share/licenses/bluez-daemon-tools"
cp "$SOURCE/COPYING" "$SOURCE/COPYING.LIB" "$STAGE/usr/share/licenses/bluez-daemon-tools/"
printf '%s\n' \
    "BlueZ $BLUEZ_VERSION daemon/tools source: $BLUEZ_URL" \
    "SHA256: $BLUEZ_SHA256" \
    "Target: mips-linux-gnu, $TARGET_FLAGS" \
    "Readline headers: GNU Readline $READLINE_VERSION (runtime ABI retained from stock root)" \
    "Configured runtime daemon path: /usr/libexec/bluetooth/bluetoothd" \
    "Persistent BlueZ storage path: $BLUEZ_STORAGE_DIR" \
    "Expected invocation: bluetoothd -n -C -f /etc/bluetooth/main.conf" \
    > "$STAGE/usr/share/licenses/bluez-daemon-tools/BUILD-NOTICE.txt"

for elf in "$DAEMON" "${REQUIRED_TOOLS[@]/#/$STAGE/usr/bin/}"; do
    file "$elf" | rg 'ELF 32-bit.*LSB.*MIPS' >/dev/null || die "wrong ELF format: $elf"
    readelf -h "$elf" | rg 'Flags:.*mips32r2' >/dev/null || die "wrong MIPS ISA flags: $elf"
    readelf -A "$elf" | rg 'Tag_GNU_MIPS_ABI_FP:.*Hard float' >/dev/null || die "wrong FP ABI: $elf"
    dynamic=$(readelf -d "$elf")
    if printf '%s\n' "$dynamic" | rg -q "($REPO_ROOT|$work|/usr/local|/lib/x86_64-linux-gnu|/lib64)"; then
        die "host/build RPATH leaked into $elf"
    fi
done
readelf -d "$DAEMON" | rg 'NEEDED|RPATH|RUNPATH' || true
readelf -d "$STAGE/usr/bin/bluetoothctl" | rg 'NEEDED|RPATH|RUNPATH' || true

# This is a real target execution under qemu-user, using the read-only stock
# loader/libc and vendor readline/ncurses ABI plus the newly staged GLib/D-Bus.
command -v qemu-mipsel >/dev/null || die "qemu-mipsel is required for target smoke tests"
runtime_ld="$STAGE/usr/lib:$GLIB_STAGE/usr/lib:$DBUS_STAGE/usr/lib:$STOCK_ROOT/usr/lib"
qemu=(qemu-mipsel -L "$STOCK_ROOT" -E LD_BIND_NOW=1 -E "LD_LIBRARY_PATH=$runtime_ld")
"${qemu[@]}" "$STAGE/usr/bin/bluetoothctl" --help > "$RUN/bluetoothctl-help.txt"
rg -q '^bluetoothctl' "$RUN/bluetoothctl-help.txt" || die "bluetoothctl --help did not produce expected output"
"${qemu[@]}" "$DAEMON" --version > "$RUN/bluetoothd-version.txt"
rg -q "^5\.87" "$RUN/bluetoothd-version.txt" || die "bluetoothd --version was not 5.87"

[[ ! -e "$STABLE_STAGE" || -L "$STABLE_STAGE" ]] || die 'stage must be a symlink'
ln -sfnT -- "$STAGE" "$STABLE_STAGE"
printf 'Staged BlueZ %s daemon/tools: %s\n' "$BLUEZ_VERSION" "$STAGE"
printf 'Stable stage: %s\n' "$STABLE_STAGE"
printf 'bluetoothctl --help capture: %s\n' "$RUN/bluetoothctl-help.txt"
printf 'Test2 has not been modified.\n'
