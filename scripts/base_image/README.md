# R1 base-image upgrade

This is a staged **userspace** upgrade, not a replacement distribution or
kernel. The player uses a static musl build; the extracted firmware uses
glibc 2.22. Build its shared libraries with the Ingenic glibc toolchain,
not `mipsel-linux-musl-gcc`.

## Component set

| Component | Candidate version / scope |
| --- | --- |
| BlueALSA | 5.0.0: daemon, control tools, ALSA plugins |
| ALSA library | 1.2.16; retain device ALSA configuration |
| SBC | 2.2 |
| AAC | fdk-aac 2.0.3 |
| aptX | rebuild libopenaptx 0.2.0; retain LGPL version |
| BlueZ | 5.87 public `libbluetooth` only; retain vendor 5.54 daemon/tools |
| D-Bus | 1.16.2 library, daemon, tools and activation helper together |
| GLib | 2.84.4, with libffi 3.4.8 and PCRE2 10.46 |
| zlib | 1.3.2 |
| Expat | 2.8.4 |

Retain the matched vendor LDAC encoder/ABR pair. The LDAC decoder is rebuilt
from source: the vendor libldacdec.so.1 segfaults (decodeSpectrumFine,
decodeScaleFactors) about a second into real LDAC reception in DAC mode.
`libldacdec-hiby.patch` bounds every read to the packet and frame, validates
stream fields and the negotiated rate/channel mode, commits IMDCT history only
for fully valid frames, and fixes upstream decoding bugs (fine-precision cap
and dequantization, descending gradient, dual-channel blocks, mode-2 scale
factor wrap, scale factor 0, S32 gain). Soname, exports and ABI match the
vendor build. `ldac_decoder_harness.c` is the host check: it encodes with the
pinned AOSP encoder and decodes through dlopen, reporting rejects and SNR.
The small decoder compatibility header supplies the declarations BlueALSA
needs.
Keep libc/loader, OpenSSL, curl, Wi-Fi drivers/firmware and the working player
mbedTLS version unchanged in this batch. Shared-library SONAME filenames do
not establish upstream source versions.

## Sources and build order

Host tools: GCC/C++, make, autoconf, automake, libtool, gettext development
tools, CMake, Meson, Ninja, pkg-config, Git, curl, archive utilities, readelf,
and qemu-user (`qemu-mipsel`). GLib development tools may also be required
for host-side generators.

Archive-based recipes download pinned archives and verify SHA-256. Git-based
recipes require the following checkouts; they reject unexpected revisions.
Clone missing checkouts and select the pinned commit, without resetting an
existing dirty checkout:

| Directory relative to repository | Upstream | Commit |
| --- | --- | --- |
| `scratch/ingenic-toolchain-v5.2` | https://github.com/tobunto/ingenic-toolchain-v5.2 | `4de21963b0f10c19118045f78c3e853a5e2c0be6` |
| `scratch/bluez-alsa-5.0.0` | https://github.com/arkq/bluez-alsa | `1935d6dcb8975f2d7a51aaafe61538d157224623` |
| `scratch/base-upgrade/fdk-aac` | https://github.com/mstorsjo/fdk-aac | `716f4394641d53f0d79c9ddac3fa93b03a49f278` |
| `scratch/base-upgrade/libopenaptx` | https://github.com/pali/libopenaptx | `2459ed4686eaef0a19dfa3f330a960813c5f60de` |
| `scratch/base-upgrade/ldacBT` | https://github.com/EHfive/ldacBT | `6579bd585a618f2e1612b3c1650d2b7fcfb1d43f` |
| `scratch/base-upgrade/libldacdec-anonymix007` | https://github.com/anonymix007/libldacdec | `c90094b15e25aef0e47c6d775fa94aceb36cabbc` |
| `dbus` (existing checkout) | https://gitlab.freedesktop.org/dbus/dbus | `958bf9db2100553bcd2fe2a854e1ebb42e886054` |

Initialize LDAC's submodule: its `libldac` revision must be
`82b6a1abee84787b8fa167efe20290073f60db2d`. The LDAC decoder is built from
anonymix007/libldacdec `c90094b15e25aef0e47c6d775fa94aceb36cabbc` plus
`libldacdec-hiby.patch` (its own `libldac` submodule is not needed).

From the repository root:

```sh
bash scripts/build_base_audio_deps.sh
bash scripts/build_base_zlib.sh
bash scripts/build_base_expat.sh
bash scripts/build_base_bluez_library.sh
bash scripts/build_base_glib.sh
bash scripts/build_base_dbus.sh
bash scripts/build_base_bt_codecs.sh
bash scripts/build_base_bluealsa.sh
make bluetooth-codec-selftest
make -j4 target
bash scripts/prepare_base_image_overlay.sh
```

Default stock tree: `/home/josegarita/Desktop/Test2/squashfs-root`.
Override `BASE_STOCK_ROOT` and `BASE_CROSS_PREFIX` where needed. Outputs stay
under `scratch/base-upgrade`; the overlay script does not install or flash.
Keep the downloaded sources, notices and these recipes with release source
materials. Runtime notices are included in the overlay.

## Validation and installation

Copy the stock root into a fresh scratch directory, merge
`scratch/base-upgrade/overlay/` into that copy, then run:

```sh
bash scripts/test_base_image.sh /absolute/path/to/candidate/root
```

This checks eager dynamic linking of new and retained tools, both BlueALSA
plugins, compression roundtrip, XML parsing, ALSA configuration allocation,
and SBC encoding/decoding. It is **not** a kernel boot, audio quality,
Bluetooth pairing, suspend, or radio firmware test. QEMU user emulation uses
the host kernel, not the device's Linux 4.4 kernel.

Back up the extracted stock tree on the workstation before applying the
overlay to Test2. Do not put backups in `/usr/data`. Preserve existing D-Bus
policies/init configuration and helper permissions. The existing Test2 packer
uses `mksquashfs -all-root`; this matters because staging files are owned by
the build user. The stock activation helper is 0755; this upgrade does not
silently add setuid privileges. Service activation requiring elevated
privileges is not enabled or newly validated here.

BlueALSA 5 renames `bluealsa` to `bluealsad` and `bluealsa-cli` to
`bluealsactl`. The overlay updates device startup scripts and includes the
matching player. An older SD-card override can supersede that player at boot:
make sure the running binary is the upgraded build. Old executables are
retained for rollback/loader testing, not intended to control the new daemon.
The old and new BlueALSA policy filenames are deliberately both retained.

Before a release, test SBC/SBC-XQ/AAC/aptX/aptX-HD/LDAC playback, Bluetooth
DAC mode, volume sync on/off, reconnect, local audio, Wi-Fi, and suspend/wake
on the actual R1. Repacking/flashing is a separate step.

## Kernel boundary

The stock kernel is Linux 4.4.94+ and the 29 vendor modules require its exact
MIPS32_R2 ABI. The available mainline/Letux tree lacks an R1 board definition
and validated audio, display/touch and other device support. Replacing the
kernel safely requires the vendor source/configuration or a full board port;
it is not a drop-in package upgrade. Keep `xImage` and modules unchanged.

## Player-facing follow-up batches

These overlays are deliberately separate from the initial shared-library
refresh. Build and validate them individually before combining a release:

```sh
bash scripts/build_base_alsa_utils.sh
bash scripts/prepare_player_base_overlay.sh alsa

bash scripts/build_base_bluez.sh
make bluetooth-codec-selftest
make -j4 target
bash scripts/prepare_player_base_overlay.sh bluetooth

bash scripts/build_base_wifi.sh
bash scripts/prepare_player_base_overlay.sh wifi
```

Outputs: `scratch/base-upgrade/player-overlays/{alsa,bluetooth,wifi}/current`.
Each has a sibling `files.txt` manifest. Nothing is installed or flashed by
these scripts. The Bluetooth overlay assumes the first batch's GLib/D-Bus
libraries are already present and includes the matching player binary.

- **ALSA utilities 1.2.16:** only `aplay`, `arecord`, and `amixer` are installed.
  Keep the vendor mixer/state configuration and existing `alsamixer`.
  `test_base_alsa_utils.sh ROOTFS` exercises the player's raw-PCM pipe using
  the null device at 44.1/48/192 kHz, in S16_LE/S24_LE, plus null capture.
- **BlueZ 5.87:** upgrade `bluetoothd`, `bluetoothctl`, `hciconfig`, `hcitool`
  and `btmon`. Preserve UART/firmware loaders (`hciattach`,
  `brcm_patchram_plus`, etc.), init scripts, configuration and pairing data.
  The daemon's generated `STORAGEDIR` is redirected to `/usr/data/bluetooth`,
  matching the device's persistent stock pairing directory; `/var/lib/bluetooth`
  remains the unrelated configure-time state default and is not used for
  pairing data. The build recipe verifies both `config.h` and staged daemon
  strings before publishing its stage.
  Mesh and LE Audio/related new audio profiles are disabled for Linux 4.4;
  classic A2DP/AVRCP remain. The player detects the supported paired-device
  command from CLI help, retaining compatibility with stock BlueZ 5.54.
- **wpa_supplicant 2.12 / libnl 3.12.0:** install supplicant, CLI, client
  library, and only libnl core/generic-netlink modules. Preserve saved
  networks, DHCP and vendor drivers. The checked-in `wpa.config` keeps
  nl80211/WEXT/wired, enterprise EAP, WPS, HS20/interworking and CLI control;
  it does not inherit upstream's AP/P2P/MACsec/D-Bus defaults. The `none`
  backend supports isolated control-interface testing without touching a
  physical interface. WPA3/other capabilities still depend on device support.

The radio recipes obtain pinned development headers rather than installing
host development packages. Readline 8.0 and OpenSSL 1.1.1f **runtime libraries
are retained**, not upgraded. In particular, this does not fix vulnerabilities
in the retained old OpenSSL itself. The OpenSSL sources are used only to
generate matching public headers. The player's separate TLS implementation
is unchanged.

Validation so far: ALSA null playback/capture passed on both QEMU and the
actual R1 kernel/libc. BlueZ 5.87 version/help and read-only CLI queries ran
on the R1 from `/tmp`, without replacing the installed daemon. These do not
establish headphone pairing, audible playback, Wi-Fi association, or suspend
correctness; those remain release gates. Keep radio overlays out of Test2
until the corresponding device tests are completed.

Wi-Fi validation also passed on the R1: `netlink_smoke.c` resolved `nlctrl`
and `nl80211` through the new libnl; `wpa-control-smoke.sh` started the new
supplicant with the `none` backend on loopback, received `PONG`, checked the
TLS/PEAP/TTLS/FAST method list, and terminated that isolated instance. The
test uses `wpa-test.conf`, not saved networks, and never takes over `wlan0`.

RAM caution: `/tmp` is RAM-backed and the R1 exposes about 56 MiB total RAM.
Keeping several batches of test binaries/libraries there exhausted enough
headroom to prevent ADB shell creation during testing. Removing unused test
copies restored access without a reboot. Stage one batch at a time, keep at
least 8 MiB available before the isolated supplicant test, and remove test
copies afterward. This is not a substitute for memory testing the final
flash-backed image during real playback and radio activity.
