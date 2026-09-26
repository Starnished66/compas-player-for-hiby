#include "bluetooth_control.h"
#include "debug_log.h"
#include "hiby_sys_server.h"
#include "subprocess.h"
#include "audio.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

/* Guards bt_control_init_chip(), bt_control_enable(), and bt_control_disable()
 * so chip firmware operations and power toggles are strictly serialized across threads. */
static pthread_mutex_t bt_chip_mutex = PTHREAD_MUTEX_INITIALIZER;
/* Shared by fallback bring-up and explicit profile changes. */
static pthread_mutex_t bt_daemon_respawn_mutex = PTHREAD_MUTEX_INITIALIZER;
typedef enum {
    BT_CODEC_AUTO = 0,
    BT_CODEC_LDAC_HQ,
    BT_CODEC_LDAC_SQ,
    BT_CODEC_APTX,
    BT_CODEC_AAC,
    BT_CODEC_SBC,
    BT_CODEC_SBC_XQ,
} bt_codec_preference_t;

static atomic_int bt_codec_preference = BT_CODEC_AUTO;
static atomic_bool modern_soft_volume_requested = false;
static pthread_mutex_t bt_soft_volume_mutex = PTHREAD_MUTEX_INITIALIZER;
static char bt_soft_volume_applied_path[256];
#define BT_SOURCE_VOLUME_MAX 127

/* The firmware overlay contains BlueALSA 5 only. Keep the daemon and control
 * client names centralized so every path uses the same current API. */
typedef struct {
    const char * daemon;
    const char * daemon_name;
    const char * ctl;
} bluealsa_backend_t;

static bluealsa_backend_t bluealsa_backend(void) {
    return (bluealsa_backend_t) {
        "/usr/bin/bluealsad", "bluealsad", "/usr/bin/bluealsactl"
    };
}

static const char * bluealsa_ctl_name(void) {
    return bluealsa_backend().ctl;
}

/* Both are pushed in from the UI; see their setters below. */
static atomic_bool bt_speexrate_enabled = false;
static atomic_bool bt_dac_all_codecs = false;
/* Requested A2DP transport rate in Hz, 0 for BlueALSA's own choice. */
static atomic_uint bt_sample_rate = 44100;

/* Remembers a rate the accessory REFUSED, not one that was applied: a refusal
 * can tear the transport down, so retrying it every reconnect would become a
 * connect/refuse/drop cycle. A successful apply needs no memory, because the
 * transport then reports the requested rate and the next attempt is a no-op. */
static pthread_mutex_t bt_rate_applied_mutex = PTHREAD_MUTEX_INITIALIZER;
static char bt_rate_refused_path[256];
static char bt_rate_refused_codec[32]; /* a refusal holds for this codec only */
static unsigned int bt_rate_refused_rate;

/* Routine (startup, the connected device changing): the refusal memory is
 * kept, so a refused rate is not retried on every reconnect. */
void bt_control_set_sample_rate(unsigned int rate) {
    atomic_store(&bt_sample_rate, rate);
}

/* The user picked a rate: a different choice deserves a fresh attempt even
 * on a device that refused the previous one. */
void bt_control_choose_sample_rate(unsigned int rate) {
    atomic_store(&bt_sample_rate, rate);
    pthread_mutex_lock(&bt_rate_applied_mutex);
    bt_rate_refused_path[0] = '\0';
    bt_rate_refused_codec[0] = '\0';
    bt_rate_refused_rate = 0;
    pthread_mutex_unlock(&bt_rate_applied_mutex);
}

/* 44.1 kHz is the one rate the daemon can be told to negotiate directly, so
 * it is the only one reached without re-establishing the link. Automatic
 * resolves to it: left to itself BlueALSA picks the highest rate up to
 * 48 kHz, which would resample the 44.1 kHz majority of a music library.
 * The rate list is read from BlueZ, so clamping what the daemon advertises
 * no longer hides the accessory's other rates from the user. */
/* Automatic is not "let BlueALSA choose": left alone it picks the highest
 * rate up to 48 kHz, which resamples the 44.1 kHz majority of a music
 * library. Every reader resolves it through here so the daemon argument, the
 * per-connection apply and the UI cannot disagree about what it means. */
static unsigned int bt_effective_sample_rate(void) {
    unsigned int rate = atomic_load(&bt_sample_rate);
    return rate == 0 ? 44100 : rate;
}

static bool bt_rate_wants_audio_cd(void) {
    return bt_effective_sample_rate() == 44100;
}

/* Selects the speex resampler for Bluetooth output. On by default: a track
 * whose rate differs from the A2DP transport's is converted either way, and
 * speex measured about a point of CPU more than alsa-lib's built-in linear
 * converter while sounding better. Converting at all is the expensive part,
 * which is why the transport rate matters more than the converter. */
void bt_control_set_speexrate_enabled(bool enabled) {
    atomic_store(&bt_speexrate_enabled, enabled);
}

/* Experimental (Developer Options): DAC mode's sink also registers LDAC
 * (aptX and aptX-HD are always registered). The source encoder preference
 * does not apply to a sink, which decodes whatever the phone picked. Takes
 * effect the next time the sink daemon starts. */
void bt_control_set_dac_all_codecs(bool enabled) {
    atomic_store(&bt_dac_all_codecs, enabled);
}

#ifndef BT_LDAC_DECODER_PATH
#define BT_LDAC_DECODER_PATH "/usr/lib/libldacdec.so.1"
#endif

/* The stock firmware's libldacdec.so.1 segfaults on phone LDAC streams,
 * killing bluealsad and dropping the connection. The rebuilt decoder in the
 * updated base image exports the bounded ldacDecode_type_len entry point, so
 * the sink offers LDAC only when the installed library contains that name. */
static bool bt_ldac_decoder_is_rebuilt(void) {
    static const char marker[] = "ldacDecode_type_len";
    const size_t marker_len = sizeof(marker) - 1;
    FILE * f = fopen(BT_LDAC_DECODER_PATH, "rb");
    if (!f) return false;
    char buf[4096];
    size_t keep = 0, n;
    bool found = false;
    while (!found && (n = fread(buf + keep, 1, sizeof(buf) - keep, f)) > 0) {
        size_t len = keep + n;
        for (size_t i = 0; i + marker_len <= len; i++) {
            if (buf[i] == marker[0] && memcmp(buf + i, marker, marker_len) == 0) {
                found = true;
                break;
            }
        }
        /* Carry the tail over so a name split across reads still matches. */
        keep = len < marker_len - 1 ? len : marker_len - 1;
        memmove(buf, buf + len - keep, keep);
    }
    fclose(f);
    return found;
}

void bt_control_restore_codec_preference(const char * codec) {
    bt_codec_preference_t preference = BT_CODEC_AUTO;
    if (codec) {
        if (strcmp(codec, "ldac_hq") == 0) preference = BT_CODEC_LDAC_HQ;
        else if (strcmp(codec, "ldac_sq") == 0 || strcmp(codec, "ldac") == 0) preference = BT_CODEC_LDAC_SQ;
        else if (strcmp(codec, "aptx") == 0) preference = BT_CODEC_APTX;
        else if (strcmp(codec, "aac") == 0) preference = BT_CODEC_AAC;
        else if (strcmp(codec, "sbc") == 0) preference = BT_CODEC_SBC;
        else if (strcmp(codec, "sbc_xq") == 0) preference = BT_CODEC_SBC_XQ;
    }
    atomic_store(&bt_codec_preference, preference);
}

/* ALSA reads $HOME/.asoundrc; main() points HOME at this writable
 * directory because the rootfs is a read-only squashfs. */
/* Negotiate the A2DP transport at 44.1 kHz. Resampling costs ~28% of a core
 * on an R1 whatever converter does it, and it only happens when a track's
 * rate differs from the transport's, so the cheapest transport rate is the
 * one most tracks already use. BlueALSA otherwise picks the highest rate up
 * to 48 kHz, which resamples the 44.1 kHz majority of a typical library.
 * Source only: see bt_control_apply_output_settings(). */
#define BT_A2DP_FORCE_44K1_ARG "--a2dp-force-audio-cd"

#define BT_ALSA_CONFIG_DIR "/usr/data/alsa"
#define BT_ALSA_PCM_NAME "bt_out"
/* Quality-3 speex converter, shipped in the base image. Measured on an R1
 * resampling 44.1k to a 48k transport, it costs the same as alsa-lib's
 * built-in linear converter while being a real sinc resampler; quality 5
 * costs ~14 points more CPU and quality 10 cannot keep up at all. alsa-lib
 * fails the open outright when a named converter is missing, so its
 * presence is checked rather than assumed: a player binary must keep
 * working on a base image that predates it. */
#define BT_ALSA_RATE_CONVERTER "speexrate"
#define BT_ALSA_RATE_CONVERTER_PLUGIN \
    "/usr/lib/alsa-lib/libasound_module_rate_" BT_ALSA_RATE_CONVERTER ".so"

const char * bt_control_get_playback_pcm(void) {
    switch ((bt_codec_preference_t) atomic_load(&bt_codec_preference)) {
    case BT_CODEC_LDAC_HQ:
    case BT_CODEC_LDAC_SQ:
        return "bluealsa:CODEC=LDAC";
    case BT_CODEC_APTX:
        return "bluealsa:CODEC=aptX";
    case BT_CODEC_AAC:
        return "bluealsa:CODEC=AAC";
    case BT_CODEC_SBC:
    case BT_CODEC_SBC_XQ:
        return "bluealsa:CODEC=SBC";
    case BT_CODEC_AUTO:
    default:
        return "bluealsa";
    }
}

static bool bt_codec_is_sbc_xq(void) {
    return (bt_codec_preference_t) atomic_load(&bt_codec_preference) == BT_CODEC_SBC_XQ;
}

/* Optional codecs are not all enabled by BlueALSA's default configuration.
 * Enabling the selected codec here keeps the setting effective. SBC is
 * mandatory and needs no -c option. */
static const char * bt_codec_daemon_name(void) {
    switch ((bt_codec_preference_t) atomic_load(&bt_codec_preference)) {
    case BT_CODEC_LDAC_HQ:
    case BT_CODEC_LDAC_SQ: return "LDAC";
    case BT_CODEC_APTX: return "aptX";
    case BT_CODEC_AAC: return "AAC";
    default: return NULL;
    }
}

/* The codec name `bluealsactl codec PCM_PATH CODEC` expects. Deliberately
 * NOT bt_codec_daemon_name() above: that one feeds the daemon's own -c
 * enable flag, where SBC is omitted because it is mandatory and always
 * built in, but selecting a codec on a live PCM has to name SBC explicitly.
 * AUTO returns NULL -- there is nothing to force, BlueALSA negotiating on
 * its own IS the preference. */
static const char * bt_codec_select_name(void) {
    switch ((bt_codec_preference_t) atomic_load(&bt_codec_preference)) {
    case BT_CODEC_LDAC_HQ:
    case BT_CODEC_LDAC_SQ: return "LDAC";
    case BT_CODEC_APTX: return "aptX";
    case BT_CODEC_AAC: return "AAC";
    case BT_CODEC_SBC:
    case BT_CODEC_SBC_XQ: return "SBC";
    default: return NULL;
    }
}

/* Automatic runs LDAC like the stock player (LDAC_ABR): starting at high
 * quality with adaptive bitrate, so a weak link steps down instead of
 * dropping out. */
static const char * bt_codec_daemon_quality(void) {
    switch ((bt_codec_preference_t) atomic_load(&bt_codec_preference)) {
    case BT_CODEC_LDAC_HQ: return "high";
    case BT_CODEC_LDAC_SQ: return "standard";
    case BT_CODEC_AUTO: return "high";
    default: return NULL;
    }
}

static bool bt_codec_auto(void) {
    return (bt_codec_preference_t) atomic_load(&bt_codec_preference) == BT_CODEC_AUTO;
}

/* Build source-daemon codec arguments for BlueALSA 5. */
static int bt_codec_append_daemon_args(char ** argv, int argc) {
    if (bt_codec_auto()) {
        argv[argc++] = (char *) "--all-codecs";
        argv[argc++] = (char *) "--ldac-abr";
    } else {
        const char * daemon_codec = bt_codec_daemon_name();
        if (daemon_codec) {
            argv[argc++] = (char *) "-c";
            argv[argc++] = (char *) daemon_codec;
        }
    }
    const char * daemon_quality = bt_codec_daemon_quality();
    if (daemon_quality) {
        argv[argc++] = (char *) "--ldac-quality";
        argv[argc++] = (char *) daemon_quality;
    }
    return argc;
}

static bool bluealsa_is_audio_pcm_path(const char * path) {
    return path && (strstr(path, "/a2dpsnk/source") != NULL ||
                    strstr(path, "/a2dpsrc/sink") != NULL);
}

/* BlueALSA 5 has no daemon-level --a2dp-volume option; apply the equivalent
 * control command when each PCM appears. */
/* bt_soft_volume_mutex guards only the applied-path latch and is never held
 * across a subprocess, so event handlers clearing it stay fast. Appliers
 * take bt_soft_volume_serial for the whole command (one at a time); the
 * latch is recorded only after the command succeeded, and not at all if a
 * clear happened meanwhile (bt_soft_volume_clears). */
static pthread_mutex_t bt_soft_volume_serial = PTHREAD_MUTEX_INITIALIZER;
static unsigned int bt_soft_volume_clears;

static void bluealsa_apply_soft_volume_cancellable(const char * path, const atomic_bool * cancel) {
    bluealsa_backend_t backend = bluealsa_backend();
    if (!bluealsa_is_audio_pcm_path(path)) return;
    pthread_mutex_lock(&bt_soft_volume_serial);
    pthread_mutex_lock(&bt_soft_volume_mutex);
    bool done = strcmp(bt_soft_volume_applied_path, path) == 0 || (cancel && atomic_load(cancel));
    unsigned int clears = bt_soft_volume_clears;
    pthread_mutex_unlock(&bt_soft_volume_mutex);
    if (!done) {
        char * argv[] = { (char *) backend.ctl, (char *) "soft-volume",
                          (char *) path,
                          (char *) (atomic_load(&modern_soft_volume_requested) ? "on" : "off"), NULL };
        int exit_code = -1;
        bool ok = subprocess_run_checked(argv, NULL, 0, 5000, &exit_code) && exit_code == 0;
        pthread_mutex_lock(&bt_soft_volume_mutex);
        if (ok && bt_soft_volume_clears == clears)
            snprintf(bt_soft_volume_applied_path, sizeof(bt_soft_volume_applied_path), "%s", path);
        pthread_mutex_unlock(&bt_soft_volume_mutex);
    }
    pthread_mutex_unlock(&bt_soft_volume_serial);
}

static void bluealsa_apply_soft_volume(const char * path) {
    bluealsa_apply_soft_volume_cancellable(path, NULL);
}

/* Every clear also invalidates an application still in flight, whatever
 * its path, since that one has no latch yet to compare against; the cost of
 * a spurious invalidation is one idempotent soft-volume command later. */
static void bluealsa_clear_soft_volume_path(const char * path) {
    pthread_mutex_lock(&bt_soft_volume_mutex);
    if (!path || strcmp(bt_soft_volume_applied_path, path) == 0)
        bt_soft_volume_applied_path[0] = '\0';
    bt_soft_volume_clears++;
    pthread_mutex_unlock(&bt_soft_volume_mutex);
}

/* Extracts the "Selected codec: X" line from `bluealsactl info` output.
 * Shared by the codec readback below and bt_control_get_connected_device_
 * codec() so the two cannot drift apart on the format. */
static bool bluealsa_parse_selected_codec(const char * info_out, char * out, size_t out_size) {
    const char * line = strstr(info_out, "Selected codec:");
    if (!line) return false;
    line += strlen("Selected codec:");
    while (*line == ' ') line++;
    size_t i = 0;
    while (line[i] != '\0' && line[i] != '\n' && line[i] != '\r' && i < out_size - 1) {
        out[i] = line[i];
        i++;
    }
    out[i] = '\0';
    return i != 0;
}

/* Applies the user's codec preference to a live PCM.
 *
 * Without this, the preference only ever reached BlueALSA through the
 * bluealsa:CODEC=... PCM name that audio_output.c opens (see
 * bt_control_get_playback_pcm()), so a freshly connected accessory kept
 * whatever BlueALSA negotiated on its own -- usually AAC -- until the next
 * time playback happened to reopen that PCM, i.e. a track change a couple
 * of songs later. That is GitHub issue #94's remaining half: manual
 * selection appeared to work only because changing it restarts the daemon
 * and the user then presses play.
 *
 * `bluealsactl codec` terminates the PCM if it is currently running (its
 * own documented behavior), which is exactly why this runs at PCMAdded --
 * the PCM exists but nothing has opened it for playback yet. A codec change
 * can itself cycle the PCM; that is already tolerated, since PCMRemoved
 * starts BT_OUTPUT_RECONFIGURE_GRACE_MS rather than reporting a disconnect,
 * and the re-add carries the same path (BlueALSA paths key on device and
 * profile, not codec), so the latch below suppresses a re-apply loop.
 *
 * Purely additive: if this fails, the PCM-name path still applies the codec
 * on the next open exactly as before, so the worst case is today's
 * behavior. */
static char bt_codec_applied_path[256];
static pthread_mutex_t bt_codec_applied_mutex = PTHREAD_MUTEX_INITIALIZER;

/* bt_codec_applied_mutex guards only the latch; see bt_codec_apply_serial. */
static unsigned int bt_codec_clears;

/* Called only on a confirmed disconnect or when the watch ends; like the
 * soft-volume clear it also invalidates a selection still in flight. */
static void bluealsa_clear_codec_path(const char * path) {
    pthread_mutex_lock(&bt_codec_applied_mutex);
    if (!path || strcmp(bt_codec_applied_path, path) == 0)
        bt_codec_applied_path[0] = '\0';
    bt_codec_clears++;
    pthread_mutex_unlock(&bt_codec_applied_mutex);
}

/* Defined further down alongside the volume-sync helpers; needed earlier by
 * bt_control_reconcile_source_settings()'s already-connected case. */
static bool find_source_pcm_path(char * out, size_t out_size);
static uint32_t bt_control_monotonic_ms(void);
/* Posts to the output watch's preference worker; false when none runs. */
static bool bt_pref_request(const char * path);

/* Applies an explicit transport rate other than 44.1 kHz, which the daemon
 * has no argument for. This goes through SelectCodec, and in BlueALSA 5 that
 * recreates the A2DP transport rather than reconfiguring it, so it costs a
 * brief re-handshake and the accessory is free to refuse it outright.
 *
 * Attempted at most once per PCM path per run, and deliberately NOT reset on
 * disconnect the way the codec preference is: a refusal can itself tear the
 * transport down, and retrying on every reconnect would turn that into an
 * endless connect/refuse/drop cycle. */
static void copy_info_field(char * dst, size_t size, const char * text, const char * key);

static bool bt_cancelled(const atomic_bool * cancel);

/* cancel (optional) is checked between its two subprocesses. Returns true
 * when it ran a rate reconfiguration (`codec -r`), successful or not: that
 * can replace the PCM and fall back to another codec, so the caller has to
 * verify the codec again. */
static void bt_auto_forget_switch(const char * path);

static void bt_rate_remember_refusal(const char * path, const char * codec, unsigned int rate) {
    pthread_mutex_lock(&bt_rate_applied_mutex);
    snprintf(bt_rate_refused_path, sizeof(bt_rate_refused_path), "%s", path);
    snprintf(bt_rate_refused_codec, sizeof(bt_rate_refused_codec), "%s", codec);
    bt_rate_refused_rate = rate;
    pthread_mutex_unlock(&bt_rate_applied_mutex);
}

static bool bluealsa_apply_rate_preference_cancellable(const char * path, const atomic_bool * cancel) {
    /* 44.1 kHz, including the Automatic that resolves to it, is already
     * carried by the daemon argument and costs no re-handshake. */
    unsigned int want = bt_effective_sample_rate();
    if (want == 44100) return false;
    if (!bluealsa_is_audio_pcm_path(path)) return false;

    bluealsa_backend_t backend = bluealsa_backend();
    char info_out[2048];
    char * info_argv[] = { (char *) backend.ctl, (char *) "info", (char *) path, NULL };
    if (!subprocess_run_low_priority(info_argv, info_out, sizeof(info_out))) return false;
    if (bt_cancelled(cancel)) return false;

    unsigned int current = 0;
    const char * rate_field = strstr(info_out, "Rate:");
    if (rate_field) (void) sscanf(rate_field + strlen("Rate:"), "%u", &current);
    if (current == want) return false; /* already negotiated there */

    char codec[32] = {0};
    copy_info_field(codec, sizeof(codec), info_out, "Selected codec:");
    char * codec_end = strchr(codec, ':'); /* v5 appends the configuration blob */
    if (codec_end) *codec_end = '\0';
    if (!codec[0]) return false;

    pthread_mutex_lock(&bt_rate_applied_mutex);
    bool refused = bt_rate_refused_rate == want && strcmp(bt_rate_refused_path, path) == 0 &&
                   strcasecmp(bt_rate_refused_codec, codec) == 0;
    pthread_mutex_unlock(&bt_rate_applied_mutex);
    if (refused) return false;

    /* The link is about to be re-established for the rate: a drop from here
     * on is not the codec's fault, so Automatic must not blame it. */
    bt_auto_forget_switch(path);
    char rate_str[16];
    snprintf(rate_str, sizeof(rate_str), "%u", want);
    char * argv[] = { (char *) backend.ctl, (char *) "codec", (char *) "-r", rate_str,
                      (char *) path, codec, NULL };
    int exit_code = -1;
    if (!subprocess_run_checked(argv, NULL, 0, 5000, &exit_code) || exit_code != 0) {
        DBG_LOG("bt_control: rate %u refused for %s on %s\n", want, path, codec);
        bt_rate_remember_refusal(path, codec, want);
        return true;
    }
    /* Accepted is not the same as in effect: an accessory can settle on
     * another rate. Remember that as a refusal too, so the rate is not
     * requested (and the codec re-verified) again and again. */
    if (!bt_cancelled(cancel) && subprocess_run_low_priority(info_argv, info_out, sizeof(info_out))) {
        unsigned int after = 0;
        rate_field = strstr(info_out, "Rate:");
        if (rate_field) (void) sscanf(rate_field + strlen("Rate:"), "%u", &after);
        if (after != want) {
            DBG_LOG("bt_control: rate %u not kept for %s on %s (now %u)\n", want, path, codec, after);
            bt_rate_remember_refusal(path, codec, want);
        }
    }
    return true;
}

static void bluealsa_apply_rate_preference(const char * path) {
    (void) bluealsa_apply_rate_preference_cancellable(path, NULL);
}

/* Automatic picks the best codec both sides support, in the order the stock
 * HiBy player documents: LDAC > aptX HD > aptX > AAC > SBC. Left to itself,
 * BlueZ takes the first endpoint in an order that does not follow quality
 * (and then keeps its LastUsed one), so LDAC headphones could stay on AAC. */
static const char * const BT_AUTO_CODEC_RANKING[] = { "LDAC", "aptX-HD", "aptX", "AAC", "SBC" };

/* From `bluealsactl info` output: the highest-ranked entry of "Available
 * codecs:" (codecs the accessory advertises that this daemon has enabled).
 * False while the list is unknown, e.g. before the accessory's endpoints
 * have been published. */
static bool bluealsa_best_available_codec(const char * info_out, unsigned int rejected,
                                          char * out, size_t out_size) {
    const char * line = strstr(info_out, "Available codecs:");
    if (!line) return false;
    line += strlen("Available codecs:");
    const char * eol = strchr(line, '\n');
    size_t len = eol ? (size_t) (eol - line) : strlen(line);
    char list[512];
    if (len >= sizeof(list)) len = sizeof(list) - 1;
    memcpy(list, line, len);
    list[len] = '\0';
    for (size_t r = 0; r < sizeof(BT_AUTO_CODEC_RANKING) / sizeof(BT_AUTO_CODEC_RANKING[0]); r++) {
        if (rejected & (1u << r)) continue;
        char copy[512];
        snprintf(copy, sizeof(copy), "%s", list);
        char * save = NULL;
        for (char * tok = strtok_r(copy, " \t\r", &save); tok; tok = strtok_r(NULL, " \t\r", &save)) {
            if (strcasecmp(tok, BT_AUTO_CODEC_RANKING[r]) == 0) {
                snprintf(out, out_size, "%s", BT_AUTO_CODEC_RANKING[r]);
                return true;
            }
        }
    }
    return false;
}

/* Automatic remembers, per accessory and for this session, codecs it could
 * not use: a selection that failed or was not in effect afterwards, or one
 * followed by a disconnect within BT_AUTO_REJECT_WINDOW_MS (a device that
 * advertises a codec but drops the link on it). Automatic then settles on
 * the next one down instead of retrying it on every reconnect. SBC, the
 * mandatory floor, is never rejected. */
#define BT_AUTO_REJECT_DEVICES 8
#define BT_AUTO_REJECT_WINDOW_MS 10000u
static struct {
    char device[32];     /* "dev_XX_XX_XX_XX_XX_XX" from the PCM path */
    unsigned int rejected;
    int switched;        /* ranking index of the last switch, or -1 */
    uint32_t switched_at;
} bt_auto_devices[BT_AUTO_REJECT_DEVICES];
static int bt_auto_devices_next;
static pthread_mutex_t bt_auto_devices_mutex = PTHREAD_MUTEX_INITIALIZER;

static int bt_auto_rank_index(const char * codec) {
    for (size_t r = 0; r < sizeof(BT_AUTO_CODEC_RANKING) / sizeof(BT_AUTO_CODEC_RANKING[0]); r++)
        if (strcasecmp(codec, BT_AUTO_CODEC_RANKING[r]) == 0) return (int) r;
    return -1;
}

static bool bt_auto_device_key(const char * path, char * out, size_t out_size) {
    const char * dev = path ? strstr(path, "/dev_") : NULL;
    if (!dev) return false;
    dev++;
    size_t len = strcspn(dev, "/");
    if (len == 0 || len >= out_size) return false;
    memcpy(out, dev, len);
    out[len] = '\0';
    return true;
}

/* Caller holds bt_auto_devices_mutex. */
static int bt_auto_device_slot_locked(const char * path, bool create) {
    char key[32];
    if (!bt_auto_device_key(path, key, sizeof(key))) return -1;
    for (int i = 0; i < BT_AUTO_REJECT_DEVICES; i++)
        if (strcmp(bt_auto_devices[i].device, key) == 0) return i;
    if (!create) return -1;
    int i = bt_auto_devices_next;
    bt_auto_devices_next = (bt_auto_devices_next + 1) % BT_AUTO_REJECT_DEVICES;
    snprintf(bt_auto_devices[i].device, sizeof(bt_auto_devices[i].device), "%s", key);
    bt_auto_devices[i].rejected = 0;
    bt_auto_devices[i].switched = -1;
    return i;
}

static unsigned int bt_auto_rejected(const char * path) {
    pthread_mutex_lock(&bt_auto_devices_mutex);
    int i = bt_auto_device_slot_locked(path, false);
    unsigned int rejected = i >= 0 ? bt_auto_devices[i].rejected : 0;
    pthread_mutex_unlock(&bt_auto_devices_mutex);
    return rejected;
}

static void bt_auto_reject(const char * path, const char * codec) {
    int r = bt_auto_rank_index(codec);
    if (r < 0 || strcasecmp(codec, "SBC") == 0) return;
    pthread_mutex_lock(&bt_auto_devices_mutex);
    int i = bt_auto_device_slot_locked(path, true);
    if (i >= 0) bt_auto_devices[i].rejected |= 1u << r;
    pthread_mutex_unlock(&bt_auto_devices_mutex);
    DBG_LOG("bt_control: auto codec: %s rejected for %s\n", codec, path);
}

static void bt_auto_note_switch(const char * path, const char * codec) {
    pthread_mutex_lock(&bt_auto_devices_mutex);
    int i = bt_auto_device_slot_locked(path, true);
    if (i >= 0) {
        bt_auto_devices[i].switched = bt_auto_rank_index(codec);
        bt_auto_devices[i].switched_at = bt_control_monotonic_ms();
    }
    pthread_mutex_unlock(&bt_auto_devices_mutex);
}

/* Link changes that are not the codec's fault: a rate reconfiguration after
 * the switch, or a disconnect the player itself asked for. */
static void bt_auto_forget_switch(const char * path) {
    pthread_mutex_lock(&bt_auto_devices_mutex);
    int i = bt_auto_device_slot_locked(path, false);
    if (i >= 0) bt_auto_devices[i].switched = -1;
    pthread_mutex_unlock(&bt_auto_devices_mutex);
}

/* A confirmed disconnect: one right after an Automatic switch rejects it. */
static void bt_auto_note_disconnect(const char * path) {
    char codec[16] = "";
    pthread_mutex_lock(&bt_auto_devices_mutex);
    int i = bt_auto_device_slot_locked(path, false);
    if (i >= 0 && bt_auto_devices[i].switched >= 0 &&
        bt_control_monotonic_ms() - bt_auto_devices[i].switched_at < BT_AUTO_REJECT_WINDOW_MS)
        snprintf(codec, sizeof(codec), "%s", BT_AUTO_CODEC_RANKING[bt_auto_devices[i].switched]);
    if (i >= 0) bt_auto_devices[i].switched = -1;
    pthread_mutex_unlock(&bt_auto_devices_mutex);
    if (codec[0]) bt_auto_reject(path, codec);
}

/* One codec selection at a time, across every caller (the output watch's
 * preference worker, reconciliation): taken for the whole sequence, never
 * by event handling. The latch is recorded only once a selection has been
 * verified, so no caller can mistake one still in flight for a result, and
 * not at all if a disconnect cleared it meanwhile (bt_codec_clears). */
static pthread_mutex_t bt_codec_apply_serial = PTHREAD_MUTEX_INITIALIZER;

static bool bluealsa_codec_latch_commit(const char * path, unsigned int clears) {
    pthread_mutex_lock(&bt_codec_applied_mutex);
    if (bt_codec_clears == clears)
        snprintf(bt_codec_applied_path, sizeof(bt_codec_applied_path), "%s", path);
    pthread_mutex_unlock(&bt_codec_applied_mutex);
    return true;
}

/* Returns false only when a selection was attempted and did not take
 * effect (or, for Automatic, the accessory's codecs are not known yet), so
 * the caller can retry; nothing to do counts as success. */
static bool bt_cancelled(const atomic_bool * cancel) {
    return cancel && atomic_load(cancel);
}

static bool bluealsa_apply_codec_preference_serial(const char * path, const atomic_bool * cancel) {
    bluealsa_backend_t backend = bluealsa_backend();
    pthread_mutex_lock(&bt_codec_applied_mutex);
    bool done = strcmp(bt_codec_applied_path, path) == 0;
    unsigned int clears = bt_codec_clears;
    pthread_mutex_unlock(&bt_codec_applied_mutex);
    if (done) return true;
    if (bt_cancelled(cancel)) return true; /* stopping: not a failure to retry */

    /* Current state first: an accessory already on the wanted codec is
     * recorded without re-selecting it (which would reconfigure the link). */
    char info_out[2048];
    char * info_argv[] = { (char *) backend.ctl, (char *) "info", (char *) path, NULL };
    if (!subprocess_run(info_argv, info_out, sizeof(info_out))) return false;
    const char * codec = bt_codec_select_name();
    bool automatic = codec == NULL;
    char best[32];
    if (automatic) {
        if (!bluealsa_best_available_codec(info_out, bt_auto_rejected(path), best, sizeof(best))) {
            DBG_LOG("bt_control: auto codec for %s: available codecs not known yet\n", path);
            return false;
        }
        codec = best;
    }
    char active[32];
    if (bluealsa_parse_selected_codec(info_out, active, sizeof(active)) &&
        strcasecmp(active, codec) == 0) {
        DBG_LOG("bt_control: codec for %s already %s\n", path, active);
        return bluealsa_codec_latch_commit(path, clears);
    }
    if (bt_cancelled(cancel)) return true;

    char * argv[] = { (char *) backend.ctl, (char *) "codec",
                      (char *) path, (char *) codec, NULL };
    int exit_code = -1;
    if (!subprocess_run_checked(argv, NULL, 0, 5000, &exit_code) || exit_code != 0) {
        DBG_LOG("bt_control: codec select %s -> %s FAILED (exit %d)\n", path, codec, exit_code);
        if (automatic) bt_auto_reject(path, codec);
        return false;
    }
    if (automatic) bt_auto_note_switch(path, codec);
    if (bt_cancelled(cancel)) return true;
    /* Read back rather than trusting the exit status: the accessory can
     * refuse a codec it advertised, and the top bar reports the real
     * transport, so a silent mismatch would look like the original bug.
     * Only an observed match counts; an unavailable readback is retried,
     * and the next attempt's state check records it if it did take. */
    if (!subprocess_run(info_argv, info_out, sizeof(info_out)) ||
        !bluealsa_parse_selected_codec(info_out, active, sizeof(active))) {
        DBG_LOG("bt_control: codec select %s -> %s: readback unavailable\n", path, codec);
        return false;
    }
    DBG_LOG("bt_control: codec select %s -> %s, now reports %s\n", path, codec, active);
    if (strcasecmp(active, codec) != 0) {
        if (automatic) bt_auto_reject(path, codec);
        return false;
    }
    return bluealsa_codec_latch_commit(path, clears);
}

/* cancel (optional) is checked between subprocesses; a cancelled pass
 * reports success so it is not scheduled for a retry. */
static bool bluealsa_apply_codec_preference_cancellable(const char * path, const atomic_bool * cancel) {
    if (!bluealsa_is_audio_pcm_path(path)) return true;
    pthread_mutex_lock(&bt_codec_apply_serial);
    bool ok = bluealsa_apply_codec_preference_serial(path, cancel);
    pthread_mutex_unlock(&bt_codec_apply_serial);
    return ok;
}

static bool bluealsa_apply_codec_preference(const char * path) {
    return bluealsa_apply_codec_preference_cancellable(path, NULL);
}

/* Whether this PCM's codec has been verified since it last connected: the
 * latch is recorded only after an observed match. */
static bool bluealsa_codec_verified(const char * path) {
    pthread_mutex_lock(&bt_codec_applied_mutex);
    bool verified = strcmp(bt_codec_applied_path, path) == 0;
    pthread_mutex_unlock(&bt_codec_applied_mutex);
    return verified;
}

static bool parse_monitor_volume(const char * text, int * out) {
    if (!text || !out) return false;
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        char * end;
        unsigned long packed = strtoul(text, &end, 16);
        if (end == text || *end != '\0' || packed > UINT32_MAX) return false;
        *out = (int) ((packed >> 8) & 0xFF); /* legacy left channel byte */
        return true;
    }
    char * end;
    long value = strtol(text, &end, 10);
    if (end == text || (strcmp(end, "[M]") != 0 && *end != '\0') ||
            value < 0 || value > BT_SOURCE_VOLUME_MAX) return false;
    *out = (int) value; /* v5 prints one value per channel */
    return true;
}

static pthread_mutex_t bt_dac_info_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t bt_dac_monitor_mutex = PTHREAD_MUTEX_INITIALIZER;
static bt_dac_stream_info_t bt_dac_info;
static pthread_t bt_dac_info_thread;
static bool bt_dac_info_active;
static pid_t bt_dac_info_monitor_pid = -1;

void bt_control_get_dac_stream_info(bt_dac_stream_info_t * out) {
    if (!out) return;
    pthread_mutex_lock(&bt_dac_info_mutex);
    *out = bt_dac_info;
    pthread_mutex_unlock(&bt_dac_info_mutex);
}

static void bt_dac_info_store(const bt_dac_stream_info_t * info) {
    pthread_mutex_lock(&bt_dac_info_mutex);
    bt_dac_info = *info;
    pthread_mutex_unlock(&bt_dac_info_mutex);
}

static unsigned int pcm_format_bit_depth(const char * format) {
    if (!format) return 0;
    if (strstr(format, "S16") || strstr(format, "U16")) return 16;
    if (strstr(format, "S24") || strstr(format, "U24")) return 24;
    if (strstr(format, "S32") || strstr(format, "U32") || strstr(format, "FLOAT")) return 32;
    if (strstr(format, "S8") || strstr(format, "U8")) return 8;
    return 0;
}

static void copy_info_field(char * dst, size_t size, const char * text, const char * key) {
    const char * p = strstr(text, key);
    if (!p) return;
    p += strlen(key);
    while (*p == ' ' || *p == '\t') p++;
    size_t n = strcspn(p, "\r\n");
    if (n >= size) n = size - 1;
    memcpy(dst, p, n);
    dst[n] = '\0';
}

static void bt_dac_info_refresh(void) {
    bt_dac_stream_info_t info = {0};
    const char * ctl = bluealsa_ctl_name();
    char list_out[4096];
    char * list_argv[] = { (char *) ctl, (char *) "list-pcms", NULL };
    if (!subprocess_run(list_argv, list_out, sizeof(list_out))) {
        bt_dac_info_store(&info);
        return;
    }
    char path[256] = {0};
    char * save = NULL;
    for (char * line = strtok_r(list_out, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save)) {
        if (strstr(line, "/a2dpsnk/source")) {
            snprintf(path, sizeof(path), "%s", line);
            break;
        }
    }
    if (!path[0]) {
        bluealsa_clear_soft_volume_path(NULL);
        bt_dac_info_store(&info); /* PCM disappeared: clear stale data now. */
        return;
    }
    bluealsa_apply_soft_volume(path);
    info.available = true;
    char info_out[2048];
    char * info_argv[] = { (char *) ctl, (char *) "info", path, NULL };
    if (subprocess_run(info_argv, info_out, sizeof(info_out))) {
        copy_info_field(info.codec, sizeof(info.codec), info_out, "Selected codec:");
        copy_info_field(info.pcm_format, sizeof(info.pcm_format), info_out, "Format:");
        const char * sampling = strstr(info_out, "Sampling:");
        if (!sampling) sampling = strstr(info_out, "Rate:");
        const char * channels = strstr(info_out, "Channels:");
        const char * running = strstr(info_out, "Running:");
        if (sampling) {
            const char * value = strchr(sampling, ':');
            if (value) (void) sscanf(value + 1, "%u", &info.sample_rate);
        }
        if (channels) (void) sscanf(channels + strlen("Channels:"), "%u", &info.channels);
        if (running) {
            running += strlen("Running:");
            while (*running == ' ' || *running == '\t') running++;
            info.running = !strncmp(running, "true", 4) || !strncmp(running, "yes", 3) || *running == '1';
        }
        info.bit_depth = pcm_format_bit_depth(info.pcm_format);
    }
    bt_dac_info_store(&info);
}

/* One `bluealsactl monitor` session: returns once that child has exited and
 * been reaped, or right away if it could not be started or a stop landed. */
static void bt_dac_info_monitor_run_once(void) {
    /* Codec and Running are property changes, which the monitor omits unless
     * asked for: a PCM appears before playback starts, so without these the
     * idle-to-running transition never arrives. The set is narrowed because
     * Delay/ClientDelay change continuously during playback and each event
     * costs two subprocesses in bt_dac_info_refresh(). */
    char * argv[] = { (char *) bluealsa_ctl_name(), (char *) "monitor",
                      (char *) "--properties=Codec,Running", NULL };
    pid_t pid;
    int fd;
    if (!subprocess_popen(argv, &pid, &fd)) return;

    /* Publish the child only while holding the same lock stop() uses. If a
     * stop landed in the small fork-to-publish window, terminate our own
     * child instead of entering fgets() after stop() already looked for it. */
    pthread_mutex_lock(&bt_dac_monitor_mutex);
    if (!bt_dac_info_active) {
        pthread_mutex_unlock(&bt_dac_monitor_mutex);
        subprocess_terminate(pid);
        close(fd);
        return;
    }
    bt_dac_info_monitor_pid = pid;
    pthread_mutex_unlock(&bt_dac_monitor_mutex);

    FILE * f = fdopen(fd, "r");
    if (!f) {
        pthread_mutex_lock(&bt_dac_monitor_mutex);
        if (bt_dac_info_monitor_pid == pid) bt_dac_info_monitor_pid = -1;
        pthread_mutex_unlock(&bt_dac_monitor_mutex);
        subprocess_terminate(pid);
        close(fd);
        return;
    }

    /* Subscribe first so changes occurring during this initial blocking
     * query remain queued in the monitor pipe and cannot be missed. */
    bt_dac_info_refresh();
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if ((strstr(line, "PCMRemoved") && strstr(line, "/a2dpsnk/source")) ||
                strstr(line, "ServiceStopped")) {
            /* A PCM that comes back under the same path (reconnect, daemon
             * restart) starts with its own SoftVolume, so apply it again. */
            bluealsa_clear_soft_volume_path(NULL);
            bt_dac_stream_info_t empty = {0};
            bt_dac_info_store(&empty);
        } else if (strstr(line, "PCM") || strstr(line, "/a2dpsnk/source") || strstr(line, "Codec") || strstr(line, "Running")) {
            bt_dac_info_refresh();
        }
    }
    fclose(f);
    pthread_mutex_lock(&bt_dac_monitor_mutex);
    if (bt_dac_info_monitor_pid == pid) bt_dac_info_monitor_pid = -1;
    pthread_mutex_unlock(&bt_dac_monitor_mutex);
    /* This thread owns reaping its monitor child. stop() only signals and
     * joins, avoiding two threads racing waitpid() on the same PID. */
    (void) waitpid(pid, NULL, 0);
    bt_dac_stream_info_t empty = {0};
    bt_dac_info_store(&empty);
}

/* Supervises the monitor for as long as DAC mode is on: if the child exits on
 * its own, a PCM seen by the next session would otherwise keep a stale
 * "already applied" soft-volume entry, so the cache is cleared and a new
 * monitor started (after a short pause, so a child that cannot start does
 * not spin). stop() clears bt_dac_info_active and ends the loop. */
static void * bt_dac_info_thread_func(void * arg) {
    (void) arg;
    for (;;) {
        bt_dac_info_monitor_run_once();
        bluealsa_clear_soft_volume_path(NULL);
        bool active = true;
        for (int waited_ms = 0; active && waited_ms < 1000; waited_ms += 50) {
            pthread_mutex_lock(&bt_dac_monitor_mutex);
            active = bt_dac_info_active;
            pthread_mutex_unlock(&bt_dac_monitor_mutex);
            if (active) usleep(50000);
        }
        if (!active) break;
    }
    return NULL;
}


static void bt_dac_info_monitor_stop(void) {
    pthread_mutex_lock(&bt_dac_monitor_mutex);
    if (!bt_dac_info_active) {
        pthread_mutex_unlock(&bt_dac_monitor_mutex);
        bt_dac_stream_info_t empty = {0}; bt_dac_info_store(&empty); return;
    }
    bt_dac_info_active = false;
    pid_t pid = bt_dac_info_monitor_pid;
    pthread_mutex_unlock(&bt_dac_monitor_mutex);
    if (pid > 0) {
        kill(pid, SIGTERM);
        /* fgets() normally unblocks as soon as SIGTERM closes the pipe.
         * Keep the same one-second grace period as subprocess_terminate(),
         * but let the monitor thread itself reap the child. */
        for (int waited_ms = 0; waited_ms < 1000; waited_ms += 50) {
            pthread_mutex_lock(&bt_dac_monitor_mutex);
            bool still_same_child = bt_dac_info_monitor_pid == pid;
            pthread_mutex_unlock(&bt_dac_monitor_mutex);
            if (!still_same_child) break;
            usleep(50000);
        }
        pthread_mutex_lock(&bt_dac_monitor_mutex);
        bool still_same_child = bt_dac_info_monitor_pid == pid;
        pthread_mutex_unlock(&bt_dac_monitor_mutex);
        if (still_same_child) kill(pid, SIGKILL);
    }
    pthread_join(bt_dac_info_thread, NULL);
    pthread_mutex_lock(&bt_dac_monitor_mutex);
    bt_dac_info_monitor_pid = -1;
    pthread_mutex_unlock(&bt_dac_monitor_mutex);
    bt_dac_stream_info_t empty = {0};
    bt_dac_info_store(&empty);
}

static void bt_dac_info_monitor_start(void) {
    bt_dac_info_monitor_stop();
    pthread_mutex_lock(&bt_dac_monitor_mutex);
    bt_dac_info_active = true;
    if (pthread_create(&bt_dac_info_thread, NULL, bt_dac_info_thread_func, NULL) != 0) {
        bt_dac_info_active = false;
    }
    pthread_mutex_unlock(&bt_dac_monitor_mutex);
}

/* /etc/init.d/S80_bt_init always starts a second, independent dbus-daemon
 * (`--config-file=/usr/share/dbus-1/system.conf`) alongside the one
 * S30dbus already started at boot (`--system`), regardless of anything
 * this app does. Depending on which of the two buses a given client
 * (bluetoothd, bluealsa, bt-agent, or this file's own bluetoothctl calls)
 * lands on, they can lose visibility of each other -- e.g. an incoming
 * AVDTP connect gets rejected ("Authentication attempt without agent")
 * because bt-agent registered its pairing agent on the bus bluetoothd
 * wasn't listening on. A bus-reachability check alone doesn't catch this,
 * since either daemon individually still answers fine even with the
 * split-brain intact; counting dbus-daemon processes directly is what
 * actually detects it, and is cheap enough (a single `ps`) to call
 * unconditionally. */
static int count_matching(const char * needle) {
    char out[4096];
    char * argv[] = { (char *) "ps", NULL };
    if (!subprocess_run(argv, out, sizeof(out))) return 0;

    int count = 0;
    char * line_save = NULL;
    char * line = strtok_r(out, "\n", &line_save);
    while (line) {
        if (strstr(line, needle)) count++;
        line = strtok_r(NULL, "\n", &line_save);
    }
    return count;
}

static int count_process_exact(const char * name) {
    char out[4096];
    char * argv[] = { (char *) "ps", NULL };
    if (!subprocess_run(argv, out, sizeof(out))) return 0;

    int count = 0;
    char * line_save = NULL;
    char * line = strtok_r(out, "\n", &line_save);
    while (line) {
        char * field_save = NULL;
        char * field = strtok_r(line, " \t", &field_save);
        while (field) {
            const char * base = strrchr(field, '/');
            if (base) base++;
            else base = field;
            if (strcmp(base, name) == 0) {
                count++;
                break;
            }
            field = strtok_r(NULL, " \t", &field_save);
        }
        line = strtok_r(NULL, "\n", &line_save);
    }
    return count;
}

/* Reconcile only the backend daemon itself.  The stock startup scripts can
 * leave a source daemon running without the requested encoder quality; a
 * sink daemon is deliberately left alone because SBC-XQ is source-only. */
static bool bluealsa_needs_source_restart(const bluealsa_backend_t * backend,
                                          bt_codec_preference_t preference) {
    DIR * proc = opendir("/proc");
    if (!proc) return false;
    bool restart = false;
    struct dirent * entry;
    while ((entry = readdir(proc))) {
        if (entry->d_name[0] < '0' || entry->d_name[0] > '9') continue;
        char path[320];
        snprintf(path, sizeof(path), "/proc/%s/cmdline", entry->d_name);
        FILE * f = fopen(path, "r");
        if (!f) continue;
        char args[2048] = {0};
        size_t len = fread(args, 1, sizeof(args) - 1, f);
        fclose(f);
        if (!len) continue;
        const char * name = strrchr(args, '/');
        name = name ? name + 1 : args;
        if (strcmp(name, backend->daemon_name) != 0) continue;
        bool sink = false, active_xq = false, active_all_codecs = false, active_force_cd = false;
        bool active_ldac_abr = false;
        const char * active_codec = NULL;
        const char * active_ldac_quality = NULL;
        for (size_t pos = strlen(args) + 1; pos < len; ) {
            size_t arg_len = strlen(args + pos);
            const char * arg = args + pos;
            if (strcmp(arg, "a2dp-sink") == 0 ||
                    strcmp(arg, "--profile=a2dp-sink") == 0) sink = true;
            if (strcmp(arg, "--profile") == 0 && pos + arg_len + 1 < len &&
                    strcmp(args + pos + arg_len + 1, "a2dp-sink") == 0) sink = true;
            if (strcmp(arg, "--sbc-quality=xq") == 0 ||
                    (strcmp(arg, "--sbc-quality") == 0 &&
                     pos + arg_len + 1 < len &&
                     strcmp(args + pos + arg_len + 1, "xq") == 0)) active_xq = true;
            if (strcmp(arg, "--all-codecs") == 0) active_all_codecs = true;
            if (strcmp(arg, "--ldac-abr") == 0) active_ldac_abr = true;
            if (strcmp(arg, BT_A2DP_FORCE_44K1_ARG) == 0) active_force_cd = true;
            if (strcmp(arg, "-c") == 0 || strcmp(arg, "--codec") == 0) {
                if (pos + arg_len + 1 < len) active_codec = args + pos + arg_len + 1;
            } else if (strncmp(arg, "--codec=", 8) == 0) {
                active_codec = arg + 8;
            }
            if (strncmp(arg, "--ldac-quality=", 15) == 0) active_ldac_quality = arg + 15;
            else if (strcmp(arg, "--ldac-quality") == 0 && pos + arg_len + 1 < len)
                active_ldac_quality = args + pos + arg_len + 1;
            if (!arg_len) break;
            pos += arg_len + 1;
        }
        if (!sink) {
            const char * expected_codec = NULL;
            if (preference == BT_CODEC_LDAC_HQ || preference == BT_CODEC_LDAC_SQ) expected_codec = "LDAC";
            else if (preference == BT_CODEC_APTX) expected_codec = "aptX";
            else if (preference == BT_CODEC_AAC) expected_codec = "AAC";
            bool expected_xq = preference == BT_CODEC_SBC_XQ;
            const char * expected_ldac_quality = preference == BT_CODEC_LDAC_HQ ? "high" :
                                                  preference == BT_CODEC_LDAC_SQ ? "standard" :
                                                  preference == BT_CODEC_AUTO ? "high" : NULL;
            bool expected_all_codecs = preference == BT_CODEC_AUTO;
            bool expected_ldac_abr = preference == BT_CODEC_AUTO;
            bool expected_force_cd = bt_rate_wants_audio_cd();
            if (active_xq != expected_xq || active_all_codecs != expected_all_codecs ||
                    active_ldac_abr != expected_ldac_abr ||
                    active_force_cd != expected_force_cd ||
                    (expected_codec && (!active_codec || strcasecmp(active_codec, expected_codec) != 0)) ||
                    (!expected_codec && active_codec) ||
                    (expected_ldac_quality && (!active_ldac_quality ||
                                               strcasecmp(active_ldac_quality, expected_ldac_quality) != 0)) ||
                    (!expected_ldac_quality && active_ldac_quality))
                restart = true;
        }
    }
    closedir(proc);
    return restart;
}

static bool dbus_system_bus_reachable(void) {
    char out[256];
    char * argv[] = { (char *) "dbus-send", (char *) "--system", (char *) "--print-reply",
                       (char *) "--dest=org.freedesktop.DBus", (char *) "/org/freedesktop/DBus",
                       (char *) "org.freedesktop.DBus.ListNames", NULL };
    return subprocess_run(argv, out, sizeof(out)) && strstr(out, "method return") != NULL;
}

static void ensure_single_dbus_daemon(void) {
    if (count_matching("dbus-daemon") <= 1 && dbus_system_bus_reachable()) return;

    subprocess_kill_all_matching("dbus-daemon");
    remove("/var/run/messagebus.pid"); /* stale pidfile blocks a fresh start otherwise */
    usleep(300000);

    /* Launch dbus-daemon in the background with --fork. */
    char * argv[] = { (char *) "dbus-daemon", (char *) "--system", (char *) "--fork", NULL };
    subprocess_spawn_daemon(argv);
    usleep(300000);
}

/* Defined with the rest of the output-settings section, below -- restores
 * whatever bt_control_apply_output_settings() was last actually asked for
 * (DAC/a2dp-sink mode in particular). Needed by the wedge recovery further
 * down, which is defined earlier in the file than that section. */
static void bt_control_reapply_last_output_settings(void);

/* Cheap pre-check for whether a Bluetooth adapter exists before asking
 * bluetoothctl about it. With no hci0, `bluetoothctl show` doesn't fail
 * fast -- it can take close to subprocess_run()'s full 15s timeout to give
 * up, which would stall the UI thread since this is called every
 * update_timer_cb tick. `hciconfig` returns almost instantly (empty output
 * when no adapter exists), so the bluetoothctl-over-D-Bus round trip is
 * only attempted when there's actually an adapter to ask about. */
static bool bt_control_adapter_present(void) {
    char out[64];
    char * argv[] = { (char *) "hciconfig", NULL };
    if (!subprocess_run(argv, out, sizeof(out))) return false;
    return out[0] != '\0';
}

/* Once real A2DP audio starts flowing during Bluetooth DAC mode,
 * bluetoothd can end up in a state where `bluetoothctl show` reports "No
 * default controller available" even though hci0 is still genuinely up
 * (bt_control_adapter_present() already true) and bluealsa/bluealsa-aplay
 * may still be actively streaming through it -- bluetoothd itself doesn't
 * crash (same PID, still sleeping), its D-Bus interface just stops
 * answering correctly. The only recovery that works is restarting
 * bluetoothd, mirroring /usr/bin/bt_resume's own remediation: stop
 * bluetoothd, `hciconfig hci0 reset`, start bluetoothd again.
 * Rate-limited via last_recovery_attempt so a wedge that recovery doesn't
 * fix can't trigger a restart on every ~5s status poll indefinitely. */
#define WEDGED_RECOVERY_COOLDOWN_SECONDS 20

static void bt_control_recover_wedged_daemon(void) {
    static time_t last_recovery_attempt = 0;
    time_t now = time(NULL);
    if (now - last_recovery_attempt < WEDGED_RECOVERY_COOLDOWN_SECONDS) {
        DBG_LOG("bt_control: wedged daemon detected, but recovery on cooldown (%lds left)\n",
                (long) (WEDGED_RECOVERY_COOLDOWN_SECONDS - (now - last_recovery_attempt)));
        return;
    }
    last_recovery_attempt = now;

    DBG_LOG("bt_control: wedged daemon detected, attempting recovery\n");

    /* Ensure a single system D-Bus daemon is running before restarting bluetoothd. */
    ensure_single_dbus_daemon();

    subprocess_kill_all_matching("bluetoothd");
    usleep(500000);

    char * reset_argv[] = { (char *) "hciconfig", (char *) "hci0", (char *) "reset", NULL };
    bool reset_ok = subprocess_run(reset_argv, NULL, 0);
    usleep(500000);

    char * bluetoothd_argv[] = { (char *) "/usr/libexec/bluetooth/bluetoothd", (char *) "-E", (char *) "-C", NULL };
    bool spawn_ok = subprocess_spawn_daemon(bluetoothd_argv);
    usleep(500000); /* give it a moment to register the adapter before the next step touches it */

    /* A freshly-restarted bluetoothd comes up with the adapter powered off
     * by default; this recovery only ever runs because Bluetooth was
     * expected to be on, so leaving it off after "fixing" it would just be
     * a different flavor of the same failure. */
    bt_control_enable();

    /* Even with power restored, bluealsa is left at whatever bluetoothd's
     * restart left it as -- the stock a2dp-source default, not the
     * a2dp-sink profile Bluetooth DAC mode needs to receive audio. Without
     * this, the phone would reconnect fine but drop the instant it tried
     * to stream, since no sink profile was registered to receive it. */
    bt_control_reapply_last_output_settings();

    DBG_LOG("bt_control: recovery attempt done (hci0 reset=%d, bluetoothd spawn=%d)\n", reset_ok, spawn_ok);
}

/* A single subprocess_run() timeout on `bluetoothctl show` isn't proof of
 * a genuine bluetoothd wedge: bluealsa can legitimately spend ~15s in a
 * normal futex wait for the remote device to send Start after Open (some
 * phones open the transport, never start, then close it), during which
 * bluetoothd can be slow to answer unrelated D-Bus queries. Requiring
 * several CONSECUTIVE timeouts (polled every ~5s, so spanning well over
 * 15s) before triggering recovery filters out that normal window while
 * still catching a real wedge, which by definition doesn't self-resolve.
 * "No default controller available" (a fast, clean response, not a
 * timeout) is a different, unambiguous signal that there's no adapter at
 * all, so that one still recovers immediately without a threshold. */
#define TIMEOUT_RECOVERY_THRESHOLD 4

/* Grace period after startup to avoid falsely triggering daemon recovery while
 * Bluetooth chip initialization or bring-up is still in progress. */
#define BT_BOOT_RACE_GRACE_MS 15000

static uint32_t bt_control_monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t) (ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* Guards bt_control_is_powered() state and timeout counters against concurrent
 * polling from the GUI timer and toggle background threads. */
static pthread_mutex_t bt_status_mutex = PTHREAD_MUTEX_INITIALIZER;

static bool bt_control_is_powered_impl(void) {
    static int consecutive_timeouts = 0;
    static bool first_poll_seen = false;
    static uint32_t first_poll_tick = 0;

    if (!first_poll_seen) {
        first_poll_seen = true;
        first_poll_tick = bt_control_monotonic_ms();
    }

    if (!bt_control_adapter_present()) {
        DBG_LOG("bt_control: bt_control_is_powered: no adapter present\n");
        return false;
    }

    char out[2048];
    char * argv[] = { (char *) "bluetoothctl", (char *) "show", NULL };
    if (!subprocess_run(argv, out, sizeof(out))) {
        consecutive_timeouts++;
        DBG_LOG("bt_control: bt_control_is_powered: bluetoothctl show subprocess failed/timed out (%d/%d)\n",
                consecutive_timeouts, TIMEOUT_RECOVERY_THRESHOLD);
        if (consecutive_timeouts >= TIMEOUT_RECOVERY_THRESHOLD) {
            bt_control_recover_wedged_daemon();
            consecutive_timeouts = 0;
        }
        return false;
    }
    consecutive_timeouts = 0; /* any actual reply, even "not powered", means it isn't stuck */

    if (strstr(out, "Powered: yes") != NULL) return true;

    if (strstr(out, "No default controller available") != NULL) {
        uint32_t since_first_poll = bt_control_monotonic_ms() - first_poll_tick;
        if (since_first_poll < BT_BOOT_RACE_GRACE_MS) {
            DBG_LOG("bt_control: bt_control_is_powered: 'No default controller' %ums after first poll -- "
                    "within boot grace window, treating as still starting up, not a wedge\n",
                    (unsigned) since_first_poll);
        } else {
            bt_control_recover_wedged_daemon();
        }
    } else {
        DBG_LOG("bt_control: bt_control_is_powered: not powered (output: %.200s)\n", out);
    }
    return false;
}

bool bt_control_is_powered(void) {
    pthread_mutex_lock(&bt_status_mutex);
    bool result = bt_control_is_powered_impl();
    pthread_mutex_unlock(&bt_status_mutex);
    return result;
}

#define BT_INIT_TIMEOUT_MS 30000

/* Uses /usr/bin/bt_resume, not /usr/bin/bt_init -- the stock hiby_player
 * binary references bt_resume, bt_enable, bt_suspend, and `bt-device -l`
 * by exact path, never bt_init/bt_done/bluealsa_profile. bt_init reliably
 * creates a duplicate dbus-daemon; bt_resume avoids that since its own
 * dbus-daemon-startup lines are commented out in the script, so it just
 * uses whatever system dbus-daemon is already running. Otherwise
 * near-identical to bt_init: same chip detection/firmware flash, plus
 * pgrep guards around starting bluetoothd/bt-agent/bluealsa so it won't
 * double-start any of those if called again while they're still up. */
/* hci0 can come up already-present at the kernel level (module auto-load,
 * bluetoothd restoring a persisted "Powered" state) without bt_resume ever
 * having run this boot; in that case bluealsa never gets started, so
 * bluetoothd has no local A2DP source SDP record to offer and every
 * connect attempt fails with `a2dp-sink profile connect failed: Protocol
 * not available` regardless of the remote device. Since
 * bt_control_init_chip()'s early return is keyed purely on hci0 presence,
 * it was silently skipping bt_resume's own bluealsa-start step. This
 * ensures the BlueALSA 5 source daemon specifically, every time, decoupled
 * from whether hci0 needed a fresh bring-up; DAC mode and the app's volume
 * synchronization are handled by their own call sites. */
/* Side-effect-free accessory probe, answering "maybe" rather than "no" when
 * it cannot tell: a failed or timed-out list-pcms must never authorize
 * killing a daemon that may own a live link, and a timeout is likeliest
 * exactly when the system is already struggling. A daemon that is genuinely
 * gone is still respawned, because the caller finds no process to keep.
 * Deliberately not find_source_pcm_path(), which also applies the
 * SoftVolume preference to whatever it finds. */
static bool bluealsa_source_pcm_may_be_connected(void) {
    char list_out[4096];
    char * argv[] = { (char *) bluealsa_ctl_name(), (char *) "list-pcms", NULL };
    if (!subprocess_run_low_priority(argv, list_out, sizeof(list_out))) return true;
    return strstr(list_out, "/a2dpsrc/sink") != NULL;
}

static bool ensure_bluealsa_running_ex(bool * out_deferred) {
    pthread_mutex_lock(&bt_daemon_respawn_mutex);
    bluealsa_backend_t backend = bluealsa_backend();
    bt_codec_preference_t preference = (bt_codec_preference_t) atomic_load(&bt_codec_preference);
    bool must_restart = bluealsa_needs_source_restart(&backend, preference);
    /* Killing the daemon tears down every A2DP link it owns, and encoder
     * arguments only affect codec negotiation for new connections, so a
     * connected accessory is never dropped to correct them; the mismatch is
     * reconciled on a later call once nothing is connected. That session
     * keeps whatever it already negotiated: bluealsa_apply_codec_preference()
     * can retarget a live PCM only for an explicit codec choice, never for
     * "auto", and LDAC quality is a daemon-level argument either way.
     * bt_resume/bt_init start the daemon without the arguments an "auto"
     * preference expects, so this mismatch is present on every boot. */
    if (must_restart && bluealsa_source_pcm_may_be_connected()) {
        must_restart = false;
        /* Reported so the caller can retry once nothing is connected, rather
         * than recording a reconciliation that never actually happened. */
        if (out_deferred) *out_deferred = true;
    }
    if (must_restart)
        subprocess_kill_all_matching(backend.daemon_name);
    /* Killing detached daemons is asynchronous; the old process may still be
     * visible to an immediate ps snapshot. Once a mismatch was established,
     * respawn unconditionally instead of allowing that stale snapshot to
     * suppress the replacement. */
    if (!must_restart && count_process_exact(backend.daemon_name) > 0) {
        pthread_mutex_unlock(&bt_daemon_respawn_mutex);
        return true;
    }
    char * argv[11];
    int argc = 0;
    argv[argc++] = (char *) backend.daemon;
    argv[argc++] = (char *) "-p";
    argv[argc++] = (char *) "a2dp-source";
    if (bt_rate_wants_audio_cd()) argv[argc++] = (char *) BT_A2DP_FORCE_44K1_ARG;
    if (bt_codec_is_sbc_xq()) argv[argc++] = (char *) "--sbc-quality=xq";
    argc = bt_codec_append_daemon_args(argv, argc);
    argv[argc] = NULL;
    bool ok = subprocess_spawn_daemon(argv);
    pthread_mutex_unlock(&bt_daemon_respawn_mutex);
    return ok;
}

static bool ensure_bluealsa_running(void) {
    return ensure_bluealsa_running_ex(NULL);
}

bool bt_control_init_chip(void) {
    pthread_mutex_lock(&bt_chip_mutex);

    if (bt_control_adapter_present()) {
        ensure_bluealsa_running();
        pthread_mutex_unlock(&bt_chip_mutex);
        return true;
    }

    char out[512];
    char * bt_resume_argv[] = { (char *) "/usr/bin/bt_resume", NULL };
    subprocess_run_timeout(bt_resume_argv, out, sizeof(out), BT_INIT_TIMEOUT_MS);

    ensure_bluealsa_running(); /* belt-and-suspenders: bt_resume already starts it, but confirm rather than assume */
    bool result = bt_control_adapter_present();
    pthread_mutex_unlock(&bt_chip_mutex);
    return result;
}

bool bt_control_reconcile_source_settings(void) {
    bool deferred = false;
    if (!ensure_bluealsa_running_ex(&deferred)) return false;
    /* An accessory already connected before the PCMAdded watch started
     * never produces that event, so the codec preference would sit
     * unapplied until the next reconnect. Same reasoning (and same pairing
     * with discovery) find_source_pcm_path() already documents for the v5
     * SoftVolume preference, which it applies on discovery itself. */
    char path[256];
    if (find_source_pcm_path(path, sizeof(path)) && !bt_pref_request(path)) {
        bluealsa_apply_codec_preference(path);
        bluealsa_apply_rate_preference(path);
    }
    /* Not reconciled while an accessory held the daemon's old arguments. */
    return !deferred;
}

/* /usr/bin/bt_enable -- matches hiby_player's own confirmed strings rather
 * than the bluetoothctl power on this function used before. It's
 * `bt-adapter --set Powered On` + `--set Discoverable On` together --
 * turning Bluetooth on for real also makes this device discoverable/
 * pairable by default, it isn't something that only happens during
 * Bluetooth DAC mode (see bt_control_apply_output_settings()'s own separate
 * discoverable toggle for that -- this and that are just two different call
 * sites setting the same underlying state). Confirmed live: on a device
 * where the chip is already flashed but the radio is administratively down
 * (the state the stock boot sequence itself leaves it in -- see
 * bt_control_init_chip()'s comment), bt_enable brings hci0 fully up in
 * about a second, no re-flash needed. */
void bt_control_enable(void) {
    pthread_mutex_lock(&bt_chip_mutex);
    char out[256];
    char * argv[] = { (char *) "/usr/bin/bt_enable", NULL };
    subprocess_run(argv, out, sizeof(out));
    pthread_mutex_unlock(&bt_chip_mutex);
}

/* /usr/bin/bt_disable, NOT /usr/bin/bt_suspend, despite bt_suspend being
 * the one actually referenced in hiby_player's strings (bt_disable isn't
 * referenced by the binary or anything else in the squashfs). Tried
 * bt_suspend first to match that evidence exactly, and confirmed live that
 * it's unsafe for a simple in-app toggle: it fully tears down the chip's
 * UART firmware link (kills brcm_patchram_plus/hciattach, rfkill block),
 * and re-flashing it back with bt_resume afterward reliably failed with
 * "Can't get device info: No such device" -- the same unrecoverable-
 * without-a-reboot failure this project already knew about from re-running
 * bt_init, this time confirmed even through the "correct" suspend-then-
 * resume pair. bt_suspend is presumably tied to the whole device's own
 * sleep/wake cycle, not a user-facing Bluetooth on/off switch. bt_disable
 * (`bt-adapter --set Discoverable Off` + `--set Powered Off`) only touches
 * the D-Bus adapter state, the same layer bt_enable operates at, so a
 * later bt_enable can bring it back in ~1s with no re-flash -- this is the
 * safe pairing. */
void bt_control_disable(void) {
    pthread_mutex_lock(&bt_chip_mutex);
    char out[256];
    char * argv[] = { (char *) "/usr/bin/bt_disable", NULL };
    subprocess_run(argv, out, sizeof(out));
    pthread_mutex_unlock(&bt_chip_mutex);
}

/* Parses "Paired: yes"/"Connected: yes" out of `bluetoothctl info <mac>`'s
 * output (see bluetooth_control.h -- confirmed exact field names/format
 * against a real device). */
static void query_device_state(const char * mac, bool * out_paired, bool * out_connected) {
    *out_paired = false;
    *out_connected = false;
    char out[2048];
    char * argv[] = { (char *) "bluetoothctl", (char *) "info", (char *) mac, NULL };
    if (!subprocess_run_low_priority(argv, out, sizeof(out))) return;
    *out_paired = strstr(out, "Paired: yes") != NULL;
    *out_connected = strstr(out, "Connected: yes") != NULL;
}

/* BlueZ 5.87 removed the legacy `paired-devices` command and added the
 * `Paired` property filter to `devices`.  Probe the CLI's own command help,
 * rather than coupling this to the BlueZ daemon version or filesystem paths.
 * Keep the mutex held while probing so concurrent callers cannot spawn two
 * capability probes.  A failed, empty, truncated, or unrecognized probe
 * deliberately leaves the cache empty, allowing a later poll to retry. */
typedef enum {
    BT_BLUETOOTHCTL_DEVICES_UNKNOWN,
    BT_BLUETOOTHCTL_DEVICES_MODERN,
    BT_BLUETOOTHCTL_DEVICES_LEGACY
} bt_bluetoothctl_devices_capability_t;

static pthread_mutex_t bt_bluetoothctl_devices_mutex = PTHREAD_MUTEX_INITIALIZER;
static bt_bluetoothctl_devices_capability_t bt_bluetoothctl_devices_capability =
    BT_BLUETOOTHCTL_DEVICES_UNKNOWN;

/* `bluetoothctl --help` prints first-level commands as tab-indented lines,
 * followed by a tab-separated description.  It intentionally does not print
 * the command argument specification, so detect command names only. */
static bool bt_bluetoothctl_help_has_command(const char * help, const char * command) {
    size_t command_len = strlen(command);
    const char * line = help;
    while (line && *line) {
        const char * end = strchr(line, '\n');
        const char * token = line;
        if (*token == '\t' && token[1] != '\t') {
            const char * token_end = token + 1;
            token++;
            while (*token_end && *token_end != '\t' && *token_end != ' ' &&
                    *token_end != '\r' && *token_end != '\n') token_end++;
            if ((size_t) (token_end - token) == command_len &&
                    strncmp(token, command, command_len) == 0) return true;
        }
        if (!end) break;
        line = end + 1;
    }
    return false;
}

static bool bt_control_paired_devices_argv_checked(char * argv[4], int timeout_ms,
                                                   bt_control_cancel_callback_t cancel_cb,
                                                   void * cancel_ctx) {
    pthread_mutex_lock(&bt_bluetoothctl_devices_mutex);

    if (cancel_cb && cancel_cb(cancel_ctx)) {
        pthread_mutex_unlock(&bt_bluetoothctl_devices_mutex);
        return false;
    }

    if (bt_bluetoothctl_devices_capability == BT_BLUETOOTHCTL_DEVICES_UNKNOWN) {
        /* 5.87 prints over 9 KiB including submenu commands. A 4 KiB
         * capture truncates it and would leave this probe retrying forever. */
        char help[16384] = {0};
        char * help_argv[] = { (char *) "bluetoothctl", (char *) "--help", NULL };
        int exit_code = -1;
        bool ran = timeout_ms > 0 ? subprocess_run_checked(help_argv, help, sizeof(help),
                                                            timeout_ms, &exit_code) && exit_code == 0 :
                                   subprocess_run(help_argv, help, sizeof(help));
        if (ran) {
            size_t help_len = strnlen(help, sizeof(help));
            bool complete = help_len > 0 && help_len < sizeof(help) - 1 &&
                            help[help_len - 1] == '\n';
            /* Prefer the legacy command if both names ever appear in a
             * vendor-customized help listing. */
            if (complete && bt_bluetoothctl_help_has_command(help, "paired-devices"))
                bt_bluetoothctl_devices_capability = BT_BLUETOOTHCTL_DEVICES_LEGACY;
            else if (complete && bt_bluetoothctl_help_has_command(help, "devices"))
                bt_bluetoothctl_devices_capability = BT_BLUETOOTHCTL_DEVICES_MODERN;
        }
    }

    switch (bt_bluetoothctl_devices_capability) {
    case BT_BLUETOOTHCTL_DEVICES_MODERN:
        argv[0] = (char *) "bluetoothctl";
        argv[1] = (char *) "devices";
        argv[2] = (char *) "Paired";
        argv[3] = NULL;
        break;
    case BT_BLUETOOTHCTL_DEVICES_LEGACY:
        argv[0] = (char *) "bluetoothctl";
        argv[1] = (char *) "paired-devices";
        argv[2] = NULL;
        break;
    default:
        pthread_mutex_unlock(&bt_bluetoothctl_devices_mutex);
        return false;
    }

    pthread_mutex_unlock(&bt_bluetoothctl_devices_mutex);
    return !cancel_cb || !cancel_cb(cancel_ctx);
}

static bool bt_control_paired_devices_argv(char * argv[4]) {
    return bt_control_paired_devices_argv_checked(argv, 0, NULL, NULL);
}

bool bt_control_is_connected(void) {
    char devices_buf[4096];
    char * devices_argv[4];
    if (!bt_control_paired_devices_argv(devices_argv) ||
            !subprocess_run(devices_argv, devices_buf, sizeof(devices_buf))) return false;

    char * line_save = NULL;
    char * line = strtok_r(devices_buf, "\n", &line_save);
    while (line) {
        if (strncmp(line, "Device ", 7) == 0) {
            const char * mac_start = line + 7;
            const char * name_start = strchr(mac_start, ' ');
            if (name_start && (size_t) (name_start - mac_start) == 17) {
                char mac[18];
                memcpy(mac, mac_start, 17);
                mac[17] = '\0';
                bool paired, connected;
                query_device_state(mac, &paired, &connected);
                if (connected) return true;
            }
        }
        line = strtok_r(NULL, "\n", &line_save);
    }
    return false;
}

/* See the header comment. bt_bluetoothctl_devices_capability is written
 * under bt_bluetoothctl_devices_mutex by bt_control_paired_devices_argv_checked()'s
 * one-time probe, so it must be read under that same mutex too -- copy it out
 * and unlock before doing anything else, both because the mutex is plain
 * (non-recursive) and the LEGACY/UNKNOWN fallback below (bt_control_is_connected())
 * re-enters that exact probe and would self-deadlock if this still held it.
 *
 * Returns 1 (connected), 0 (not connected), or -1 if this cycle couldn't
 * determine either way. -1 covers two cases: capability still UNKNOWN and
 * bt_control_is_connected()'s own query failed, or capability is MODERN but
 * this specific `devices Connected` call failed. The latter deliberately
 * does NOT fall back to the O(N) per-paired-device path -- that fallback is
 * exactly the fork storm this function exists to avoid, and a transient
 * bluetoothctl hiccup is most likely during the same radio power-on window
 * that storm is worst in. Callers should keep the last known state on -1,
 * not treat it as "nothing connected". */
int bt_control_any_paired_connected(void) {
    pthread_mutex_lock(&bt_bluetoothctl_devices_mutex);
    bt_bluetoothctl_devices_capability_t capability = bt_bluetoothctl_devices_capability;
    pthread_mutex_unlock(&bt_bluetoothctl_devices_mutex);

    if (capability == BT_BLUETOOTHCTL_DEVICES_MODERN) {
        char out[512];
        char * argv[] = { (char *) "bluetoothctl", (char *) "devices", (char *) "Connected", NULL };
        if (subprocess_run_low_priority(argv, out, sizeof(out))) return strstr(out, "Device ") != NULL ? 1 : 0;
        return -1;
    }
    return bt_control_is_connected() ? 1 : 0;
}

/* Returns paired and connected state for all paired devices.
 * Returns device count, or -1 if the underlying bluetoothctl call fails. */
int bt_control_list_paired_states(bt_device_t * out, int max_count) {
    char devices_buf[4096];
    char * devices_argv[4];
    if (!bt_control_paired_devices_argv(devices_argv) ||
            !subprocess_run_low_priority(devices_argv, devices_buf, sizeof(devices_buf))) return -1;

    int count = 0;
    char * line_save = NULL;
    char * line = strtok_r(devices_buf, "\n", &line_save);
    while (line && count < max_count) {
        if (strncmp(line, "Device ", 7) == 0) {
            const char * mac_start = line + 7;
            const char * name_start = strchr(mac_start, ' ');
            if (name_start && (size_t) (name_start - mac_start) == 17) {
                memcpy(out[count].mac, mac_start, 17);
                out[count].mac[17] = '\0';
                snprintf(out[count].name, sizeof(out[count].name), "%s", name_start + 1);
                query_device_state(out[count].mac, &out[count].paired, &out[count].connected);
                count++;
            }
        }
        line = strtok_r(NULL, "\n", &line_save);
    }
    return count;
}

/* Runs discovery restricted to the classic BR/EDR bearer using `scan.transport bredr`.
 * Dual-mode devices paired via classic BR/EDR reliably support A2DP/AVRCP audio. */
static void run_bredr_scan(int seconds) {
    pid_t pid;
    int write_fd;
    char * argv[] = { (char *) "bluetoothctl", NULL };
    if (!subprocess_popen_stdin(argv, &pid, &write_fd)) return;

    const char * start_cmds = "scan.transport bredr\nscan on\n";
    ssize_t ignored = write(write_fd, start_cmds, strlen(start_cmds));
    (void) ignored;

    sleep(seconds > 0 ? (unsigned int) seconds : 1);

    const char * stop_cmds = "scan off\nquit\n";
    ignored = write(write_fd, stop_cmds, strlen(stop_cmds));
    (void) ignored;

    close(write_fd);
    subprocess_terminate(pid); /* reaps it either way -- `quit` alone isn't guaranteed to have taken effect yet */
}

/* Lists devices BlueZ already knows (paired, and anything seen since boot)
 * without running an inquiry. An inquiry makes the controller interleave
 * discovery with an active A2DP link, which is audible as the stream cutting
 * in and out, so the caller decides when that cost is worth paying. */
int bt_control_list_devices(bt_device_t * out, int max_count) {
    return bt_control_scan(0, out, max_count);
}

int bt_control_scan(int seconds, bt_device_t * out, int max_count) {
    if (seconds > 0) run_bredr_scan(seconds); /* blocks for `seconds` -- that's the point */

    char devices_buf[8192];
    char * devices_argv[] = { (char *) "bluetoothctl", (char *) "devices", NULL };
    if (!subprocess_run(devices_argv, devices_buf, sizeof(devices_buf))) return 0;

    int count = 0;
    char * line_save = NULL;
    char * line = strtok_r(devices_buf, "\n", &line_save);
    while (line && count < max_count) {
        /* "Device XX:XX:XX:XX:XX:XX Some Name Here" */
        if (strncmp(line, "Device ", 7) == 0) {
            const char * mac_start = line + 7;
            const char * name_start = strchr(mac_start, ' ');
            if (name_start && (size_t) (name_start - mac_start) == 17) {
                memcpy(out[count].mac, mac_start, 17);
                out[count].mac[17] = '\0';
                snprintf(out[count].name, sizeof(out[count].name), "%s", name_start + 1);
                query_device_state(out[count].mac, &out[count].paired, &out[count].connected);
                count++;
            }
        }
        line = strtok_r(NULL, "\n", &line_save);
    }
    return count;
}

bool bt_control_connect(const char * mac) {
    char out[512];
    pthread_mutex_lock(&bt_chip_mutex);

    char * pair_argv[] = { (char *) "bluetoothctl", (char *) "pair", (char *) mac, NULL };
    subprocess_run(pair_argv, out, sizeof(out)); /* no-op if already paired -- not fatal either way */

    char * trust_argv[] = { (char *) "bluetoothctl", (char *) "trust", (char *) mac, NULL };
    subprocess_run(trust_argv, out, sizeof(out));

    char * connect_argv[] = { (char *) "bluetoothctl", (char *) "connect", (char *) mac, NULL };
    bool ok = subprocess_run(connect_argv, out, sizeof(out)) && strstr(out, "Failed") == NULL;
    pthread_mutex_unlock(&bt_chip_mutex);
    return ok;
}

bool bt_control_disconnect(const char * mac) {
    char out[512];
    if (mac) {
        /* Asked for here, so not the codec's fault (see bt_auto_note_disconnect). */
        char path[64];
        snprintf(path, sizeof(path), "/dev_%s/", mac);
        for (char * c = path; *c; c++) if (*c == ':') *c = '_';
        bt_auto_forget_switch(path);
    }
    pthread_mutex_lock(&bt_chip_mutex);
    char * argv[] = { (char *) "bluetoothctl", (char *) "disconnect", (char *) mac, NULL };
    bool ok = subprocess_run(argv, out, sizeof(out)) && strstr(out, "Failed") == NULL;
    pthread_mutex_unlock(&bt_chip_mutex);
    return ok;
}

bool bt_control_forget(const char * mac) {
    char out[512];
    pthread_mutex_lock(&bt_chip_mutex);
    char * argv[] = { (char *) "bluetoothctl", (char *) "remove", (char *) mac, NULL };
    bool ok = subprocess_run(argv, out, sizeof(out)) && strstr(out, "Failed") == NULL;
    pthread_mutex_unlock(&bt_chip_mutex);
    return ok;
}

#define BT_RECONNECT_QUERY_TIMEOUT_MS 1500
#define BT_RECONNECT_CONNECT_TIMEOUT_MS 6500

static bool bt_reconnect_cancelled(bt_control_cancel_callback_t cancel_cb, void * cancel_ctx) {
    return cancel_cb && cancel_cb(cancel_ctx);
}

static bool bt_reconnect_valid_mac(const char * mac) {
    if (!mac || strlen(mac) != 17) return false;
    for (int i = 0; i < 17; i++) {
        if (i % 3 == 2) {
            if (mac[i] != ':') return false;
        } else if (!((mac[i] >= '0' && mac[i] <= '9') ||
                     (mac[i] >= 'A' && mac[i] <= 'F') ||
                     (mac[i] >= 'a' && mac[i] <= 'f'))) return false;
    }
    return true;
}

static bool bt_reconnect_run(char * const argv[], char * output, size_t output_size,
                             int timeout_ms, bt_control_cancel_callback_t cancel_cb,
                             void * cancel_ctx, int * exit_code) {
    if (bt_reconnect_cancelled(cancel_cb, cancel_ctx)) return false;
    bool ran = subprocess_run_checked(argv, output, output_size, timeout_ms, exit_code);
    if (!ran || (exit_code && *exit_code != 0)) return false;
    /* An incomplete snapshot cannot establish that another output is absent. */
    if (output && output_size && strnlen(output, output_size) >= output_size - 1) return false;
    return !bt_reconnect_cancelled(cancel_cb, cancel_ctx);
}

static bool bt_reconnect_info(const char * mac, char * output, size_t output_size,
                              bt_control_cancel_callback_t cancel_cb, void * cancel_ctx) {
    char * argv[] = { (char *) "bluetoothctl", (char *) "info", (char *) mac, NULL };
    int exit_code = -1;
    return bt_reconnect_run(argv, output, output_size, BT_RECONNECT_QUERY_TIMEOUT_MS,
                            cancel_cb, cancel_ctx, &exit_code);
}

static bool bt_reconnect_audio_output_connected(bt_control_cancel_callback_t cancel_cb,
                                                void * cancel_ctx, bool * query_ok) {
    char output[4096] = {0};
    char * argv[] = { (char *) bluealsa_ctl_name(), (char *) "list-pcms", NULL };
    int exit_code = -1;
    bool ok = bt_reconnect_run(argv, output, sizeof(output), BT_RECONNECT_QUERY_TIMEOUT_MS,
                               cancel_cb, cancel_ctx, &exit_code);
    if (query_ok) *query_ok = ok;
    if (!ok) return false;
    return strstr(output, "/a2dpsrc/sink") != NULL;
}

/* Do not call bt_control_is_powered() from the reconnect critical section:
 * that public status path has its own recovery behavior and can re-enter
 * chip/daemon lifecycle code. This is the private, bounded read-only query
 * needed by the worker and deliberately never powers the adapter on. */
static bool bt_reconnect_adapter_powered(bt_control_cancel_callback_t cancel_cb,
                                         void * cancel_ctx) {
    char output[2048] = {0};
    char * argv[] = { (char *) "bluetoothctl", (char *) "show", NULL };
    int exit_code = -1;
    return bt_reconnect_run(argv, output, sizeof(output), BT_RECONNECT_QUERY_TIMEOUT_MS,
                            cancel_cb, cancel_ctx, &exit_code) &&
           strstr(output, "Powered: yes") != NULL;
}

static bool bt_reconnect_info_line_is(const char * info, const char * key, const char * value) {
    size_t key_len = strlen(key);
    const char * line = info;
    while (line && *line) {
        while (*line == ' ' || *line == '\t') line++;
        if (strncmp(line, key, key_len) == 0) {
            const char * p = line + key_len;
            while (*p == ' ' || *p == '\t') p++;
            size_t value_len = strcspn(p, "\r\n");
            while (value_len && (p[value_len - 1] == ' ' || p[value_len - 1] == '\t')) value_len--;
            if (strlen(value) == value_len && strncmp(p, value, value_len) == 0) return true;
        }
        line = strchr(line, '\n');
        if (line) line++;
    }
    return false;
}

static bool bt_reconnect_info_has_uuid(const char * info, const char * uuid) {
    size_t key_len = strlen("UUID:");
    const char * line = info;
    while (line && *line) {
        while (*line == ' ' || *line == '\t') line++;
        if (strncmp(line, "UUID:", key_len) == 0) {
            const char * end = strchr(line, '\n');
            size_t line_len = end ? (size_t)(end - line) : strlen(line);
            const char * match = strstr(line + key_len, uuid);
            if (match && (size_t)(match - line) + strlen(uuid) <= line_len) return true;
        }
        line = strchr(line, '\n');
        if (line) line++;
    }
    return false;
}

static bool bt_reconnect_info_eligible(const char * info, bool * connected) {
    if (connected) *connected = false;
    if (!info || !bt_reconnect_info_line_is(info, "Paired:", "yes") ||
            !bt_reconnect_info_line_is(info, "Trusted:", "yes") ||
            bt_reconnect_info_line_is(info, "Blocked:", "yes") ||
            !bt_reconnect_info_has_uuid(info, "0000110b-0000-1000-8000-00805f9b34fb")) return false;
    if (connected) *connected = bt_reconnect_info_line_is(info, "Connected:", "yes");
    return true;
}

static bool bt_reconnect_disconnect_locked(const char * mac) {
    char output[512] = {0};
    char * argv[] = { (char *) "bluetoothctl", (char *) "disconnect", (char *) mac, NULL };
    int exit_code = -1;
    return subprocess_run_checked(argv, output, sizeof(output),
                                  BT_RECONNECT_QUERY_TIMEOUT_MS, &exit_code) &&
           exit_code == 0 && strstr(output, "Failed") == NULL;
}

bool bt_control_reconnect_paired(const char * preferred_mac,
                                 bt_control_cancel_callback_t cancel_cb,
                                 void * cancel_ctx) {
    if (preferred_mac && (!bt_reconnect_valid_mac(preferred_mac) ||
                          bt_reconnect_cancelled(cancel_cb, cancel_ctx))) return false;
    if (!bt_reconnect_adapter_powered(cancel_cb, cancel_ctx)) return false;
    bool pcm_query_ok = false;
    if (bt_reconnect_audio_output_connected(cancel_cb, cancel_ctx, &pcm_query_ok)) return true;
    if (!pcm_query_ok) return false;
    if (bt_reconnect_cancelled(cancel_cb, cancel_ctx)) return false;

    char target[18] = {0};
    char info[4096] = {0};
    bool was_connected = false;
    if (preferred_mac) {
        snprintf(target, sizeof(target), "%s", preferred_mac);
        if (!bt_reconnect_info(target, info, sizeof(info), cancel_cb, cancel_ctx) ||
                !bt_reconnect_info_eligible(info, &was_connected)) return false;
    } else {
        char devices[4096] = {0};
        char * devices_argv[4];
        if (!bt_control_paired_devices_argv_checked(devices_argv, BT_RECONNECT_QUERY_TIMEOUT_MS,
                                                     cancel_cb, cancel_ctx)) return false;
        int exit_code = -1;
        if (!bt_reconnect_run(devices_argv, devices, sizeof(devices),
                              BT_RECONNECT_QUERY_TIMEOUT_MS, cancel_cb, cancel_ctx, &exit_code)) return false;
        size_t devices_len = strnlen(devices, sizeof(devices));
        if (devices_len == 0 || devices_len >= sizeof(devices) - 1 || devices[devices_len - 1] != '\n') return false;
        unsigned int eligible = 0;
        char * save = NULL;
        for (char * line = strtok_r(devices, "\r\n", &save); line;
             line = strtok_r(NULL, "\r\n", &save)) {
            if (bt_reconnect_cancelled(cancel_cb, cancel_ctx)) return false;
            if (strncmp(line, "Device ", 7) != 0) continue;
            const char * mac = line + 7;
            const char * name = strchr(mac, ' ');
            char candidate[18];
            if (!name || (size_t)(name - mac) != 17) continue;
            memcpy(candidate, mac, 17);
            candidate[17] = '\0';
            if (!bt_reconnect_valid_mac(candidate)) continue;
            char candidate_info[4096] = {0};
            bool candidate_connected = false;
            if (!bt_reconnect_info(candidate, candidate_info, sizeof(candidate_info),
                                   cancel_cb, cancel_ctx)) return false;
            if (bt_reconnect_info_eligible(candidate_info, &candidate_connected)) {
                eligible++;
                snprintf(target, sizeof(target), "%s", candidate);
                snprintf(info, sizeof(info), "%s", candidate_info);
                was_connected = candidate_connected;
                if (eligible > 1) return false;
            }
        }
        if (eligible != 1) return false;
    }
    if (was_connected) return true;
    if (bt_reconnect_cancelled(cancel_cb, cancel_ctx)) return false;

    pthread_mutex_lock(&bt_chip_mutex);
    bool result = false;
    bool connected_after = false;
    if (!bt_reconnect_cancelled(cancel_cb, cancel_ctx) &&
            bt_reconnect_adapter_powered(cancel_cb, cancel_ctx) &&
            bt_reconnect_audio_output_connected(cancel_cb, cancel_ctx, &pcm_query_ok) == false &&
            pcm_query_ok) {
        /* Refresh under the chip lock so a connection made by another path
         * before lock acquisition is treated as preexisting. */
        char locked_info[4096] = {0};
        bool locked_connected = false;
        if (!bt_reconnect_info(target, locked_info, sizeof(locked_info), NULL, NULL) ||
                !bt_reconnect_info_eligible(locked_info, &locked_connected)) {
            pthread_mutex_unlock(&bt_chip_mutex);
            return false;
        }
        if (locked_connected) {
            pthread_mutex_unlock(&bt_chip_mutex);
            return true;
        }
        if (bt_reconnect_cancelled(cancel_cb, cancel_ctx)) {
            pthread_mutex_unlock(&bt_chip_mutex);
            return false;
        }
        char * connect_argv[] = { (char *) "bluetoothctl", (char *) "--timeout", (char *) "5",
                                  (char *) "connect", target, NULL };
        char connect_output[1024] = {0};
        int exit_code = -1;
        bool connect_ran = subprocess_run_checked(connect_argv, connect_output, sizeof(connect_output),
                                                  BT_RECONNECT_CONNECT_TIMEOUT_MS, &exit_code);
        bool connect_ok = connect_ran && exit_code == 0 && strstr(connect_output, "Failed") == NULL;
        /* Verify and, when cancellation raced the command, clean up using an
         * uncancelled bounded read. The callback is intentionally not used
         * for this cleanup observation. */
        if (bt_reconnect_info(target, info, sizeof(info), NULL, NULL))
            connected_after = bt_reconnect_info_eligible(info, &was_connected) && was_connected;
        if (bt_reconnect_cancelled(cancel_cb, cancel_ctx)) {
            (void) bt_reconnect_disconnect_locked(target);
        } else {
            result = connect_ok && connected_after;
        }
    }
    pthread_mutex_unlock(&bt_chip_mutex);
    return result;
}

/* Synchronizes volume between this player and connected a2dp-source accessories.
 * Maps AVRCP 0-127 linearly to the player's 0-100% volume. */
/* apply_soft_volume: discovery also applies the SoftVolume preference, as a
 * source PCM can predate a monitor subscription. The output watch's worker
 * passes false and applies it in its own cancellable pass instead. */
static bool find_source_pcm_path_ex(char * out, size_t out_size, bool apply_soft_volume) {
    char list_out[4096];
    char * argv[] = { (char *) bluealsa_ctl_name(), (char *) "list-pcms", NULL };
    if (!subprocess_run_low_priority(argv, list_out, sizeof(list_out))) {
        /* Log failure if bluealsactl list-pcms fails or times out. */
        DBG_LOG("bt_control: find_source_pcm_path: bluealsactl list-pcms failed/timed out\n");
        return false;
    }

    char * line_save = NULL;
    char * line = strtok_r(list_out, "\n", &line_save);
    while (line) {
        /* "a2dpsrc" (not "a2dpsnk") distinguishes this from a DAC-mode PCM
         * if one ever coexisted; "/sink" is bluealsa's own role name for
         * the PCM WE write into (confirmed live: `bluealsactl list-pcms`
         * on a real connected device returned exactly
         * "/org/bluealsa/hci0/dev_XX_XX_XX_XX_XX_XX/a2dpsrc/sink"). */
        if (strstr(line, "/a2dpsrc/sink") != NULL) {
            snprintf(out, out_size, "%s", line);
            /* A source PCM can predate this monitor subscription, so apply
             * the v5 SoftVolume preference during discovery as well as on
             * PCMAdded. */
            if (apply_soft_volume) bluealsa_apply_soft_volume(out);
            return true;
        }
        line = strtok_r(NULL, "\n", &line_save);
    }
    return false;
}

static bool find_source_pcm_path(char * out, size_t out_size) {
    return find_source_pcm_path_ex(out, out_size, true);
}

/* Public wrapper around the same check find_source_pcm_path() above already
 * does for the AVRCP volume-sync feature -- a "/a2dpsrc/sink" PCM only
 * exists in bluealsa's own list once a real audio-capable accessory has
 * actually negotiated the A2DP sink role with this device acting as
 * a2dp-source, which is a stronger signal than bt_control_is_connected()/
 * bt_control_list_paired_states() (those report ANY paired device with an
 * active connection, which could be a non-audio BLE peripheral with no A2DP
 * profile at all). Used for the topbar's "BT headphone connected" icon
 * (gui.c) -- same subprocess cost as everything else here, call off the UI
 * thread. */
bool bt_control_is_a2dp_source_connected(void) {
    char path[256];
    return find_source_pcm_path(path, sizeof(path));
}

bool bt_control_get_connected_device_mac(char * out, size_t out_size) {
    char path[256];
    if (!find_source_pcm_path(path, sizeof(path))) return false;

    const char * dev = strstr(path, "dev_");
    if (!dev) return false;
    dev += 4; /* skip "dev_" */

    char mac[18];
    if (strlen(dev) < 17) return false; /* "XX_XX_XX_XX_XX_XX" */
    for (int i = 0; i < 17; i++) mac[i] = (dev[i] == '_') ? ':' : dev[i];
    mac[17] = '\0';

    snprintf(out, out_size, "%s", mac);
    return true;
}

bool bt_control_get_connected_device_codec(char * out, size_t out_size) {
    return bt_control_get_connected_device_stream(out, out_size, NULL);
}

/* One `bluealsactl info` for both the codec and the rate: the same call
 * already reported the sampling frequency, it was just being discarded.
 * out_sample_rate may be NULL when only the codec is wanted, and is left
 * untouched when the output reports no frequency. */
bool bt_control_get_connected_device_stream(char * out, size_t out_size, unsigned int * out_sample_rate) {
    char path[256];
    if (!find_source_pcm_path(path, sizeof(path))) return false;

    char info_out[2048];
    char * argv[] = { (char *) bluealsa_ctl_name(), (char *) "info", path, NULL };
    if (!subprocess_run_low_priority(argv, info_out, sizeof(info_out))) return false;

    /* "Selected codec: AAC" -- confirmed live via `bluealsactl info
     * <pcm-path>` (also reports "Available codecs: SBC AAC", but that's
     * every codec the accessory advertised support for, not what's
     * actually in use right now). */
    char codec[32];
    if (!bluealsa_parse_selected_codec(info_out, codec, sizeof(codec))) return false;

    if (out_sample_rate) {
        /* Same two spellings the sink-side parse accepts. */
        const char * sampling = strstr(info_out, "Sampling:");
        if (!sampling) sampling = strstr(info_out, "Rate:");
        const char * value = sampling ? strchr(sampling, ':') : NULL;
        if (value) (void) sscanf(value + 1, "%u", out_sample_rate);
    }

    snprintf(out, out_size, "%s", codec);
    return true;
}

/* Writes an ALSA PCM definition whose plug slave is pinned to the rate the
 * A2DP transport already negotiated, and reports the PCM name to open.
 *
 * Opening the BlueALSA PCM at the track's own rate instead is destructive:
 * on a mismatch the plugin calls SelectCodec, and in BlueALSA 5 that is a
 * transport recreation, not a reconfiguration ("A2DP codec selection is in
 * fact a transport recreation", ba-transport.c), so the accessory drops --
 * and the call fails outright on devices that refuse the new configuration.
 * Pinning the slave rate makes ALSA convert the track instead, which is what
 * PipeWire, PulseAudio and Android all do with an A2DP transport.
 *
 * The definition names the accessory explicitly rather than relying on
 * BlueALSA's "most recently connected" default, so it cannot follow a
 * different device than the one just queried. Written atomically: a
 * half-written file in HOME would break every ALSA open, not just this one.
 *
 * Returns false when the transport cannot be read, leaving the caller to
 * open bt_control_get_playback_pcm() directly as before. */
/* Rates the accessory itself advertises, read from BlueZ rather than from
 * BlueALSA. BlueALSA reports the negotiated intersection, and
 * --a2dp-force-audio-cd rewrites its own advertised capability to 44.1 kHz
 * alone (a2dp-aac.c and friends), so asking it while the clamp is on would
 * only ever answer "44.1". BlueZ still exposes each remote endpoint under
 * org.bluez.MediaEndpoint1 with the capability bytes the accessory actually
 * sent, which is what this decodes. */

/* Value of one dbus-send property, as the tool prints it. */
static const char * dbus_prop_value(const char * out, const char * prop) {
    char needle[64];
    snprintf(needle, sizeof(needle), "string \"%s\"", prop);
    const size_t needle_len = strlen(needle);
    const char * p = out;
    while ((p = strstr(p, needle)) != NULL) {
        const char * value = p + needle_len;
        while (isspace((unsigned char) *value)) value++;
        /* A property key is followed immediately by its variant value. This
         * avoids treating the same text inside an unrelated string value as
         * a property. */
        if (strncmp(value, "variant", 7) == 0 &&
                (isspace((unsigned char) value[7]) || value[7] == '\0')) {
            value += 7;
            while (isspace((unsigned char) *value)) value++;
            return value;
        }
        p += needle_len;
    }
    return NULL;
}

static bool dbus_prop_string_equals(const char * out, const char * prop,
                                    const char * expected) {
    const char * value = dbus_prop_value(out, prop);
    if (!value) return false;
    while (isspace((unsigned char) *value)) value++;
    if (strncmp(value, "string", 6) != 0 ||
            !isspace((unsigned char) value[6])) return false;
    value += 6;
    while (isspace((unsigned char) *value)) value++;
    if (*value++ != '"') return false;
    const char * end = strchr(value, '"');
    if (!end) return false;
    size_t len = (size_t) (end - value);
    return len == strlen(expected) && strncasecmp(value, expected, len) == 0;
}

/* dbus-send prints byte arrays differently across versions: this device
 * emits bare "80 01 8c", others "byte 0x80, byte 0x01". Both the "byte"
 * token and the "0x" prefix are therefore optional, because requiring
 * either one silently yields an empty capability list on the other format. */
static bool dbus_parse_hex_byte(const char ** cursor, uint8_t * out) {
    const char * p = *cursor;
    while (isspace((unsigned char) *p)) p++;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;

    unsigned int value = 0;
    int digits = 0;
    while (isxdigit((unsigned char) *p) && digits < 2) {
        value <<= 4;
        if (*p >= '0' && *p <= '9') value += (unsigned int) (*p - '0');
        else if (*p >= 'a' && *p <= 'f') value += (unsigned int) (*p - 'a' + 10);
        else value += (unsigned int) (*p - 'A' + 10);
        p++;
        digits++;
    }
    if (digits == 0 || isxdigit((unsigned char) *p)) return false;
    *out = (uint8_t) value;
    *cursor = p;
    return true;
}

static bool dbus_prop_byte(const char * out, const char * prop, uint8_t * value) {
    const char * p = dbus_prop_value(out, prop);
    if (!p) return false;
    while (isspace((unsigned char) *p)) p++;
    if (strncmp(p, "byte", 4) != 0 || !isspace((unsigned char) p[4])) return false;
    p += 4;
    return dbus_parse_hex_byte(&p, value);
}

/* dbus-send prints a byte array as e.g. "array [ byte 0x80 byte 0x01 ]";
 * line wrapping and optional commas are presentation details, not separators
 * that can be ignored while starting at the first character after '['. */
static bool dbus_prop_byte_array(const char * out, const char * prop,
                                 uint8_t * bytes, size_t bytes_size,
                                 size_t * out_len) {
    const char * p = dbus_prop_value(out, prop);
    if (!p) return false;
    p = strchr(p, '[');
    if (!p) return false;
    p++;

    size_t len = 0;
    for (;;) {
        while (isspace((unsigned char) *p) || *p == ',') p++;
        if (*p == ']') {
            *out_len = len;
            return len != 0;
        }
        if (*p == '\0' || len >= bytes_size) return false;
        if (strncmp(p, "byte", 4) == 0 && isspace((unsigned char) p[4])) p += 4;
        if (!dbus_parse_hex_byte(&p, &bytes[len])) return false;
        len++;
    }
}

/* BlueALSA exposes the standard codec byte separately and vendor codec IDs
 * in the first six bytes of a vendor capability blob. Match both, because
 * the selected codec name alone cannot distinguish two remote vendor SEPs. */
static bool a2dp_caps_match_codec(const uint8_t * caps, size_t len,
                                  uint8_t codec_id, const char * codec) {
    if (strcasecmp(codec, "SBC") == 0) return codec_id == 0x00;
    if (strcasecmp(codec, "AAC") == 0) return codec_id == 0x02;
    if (len < 6 || codec_id != 0xff) return false;

    uint32_t vendor_id = (uint32_t) caps[0] |
                         ((uint32_t) caps[1] << 8) |
                         ((uint32_t) caps[2] << 16) |
                         ((uint32_t) caps[3] << 24);
    uint16_t vendor_codec_id = (uint16_t) caps[4] | ((uint16_t) caps[5] << 8);
    if (strcasecmp(codec, "aptX") == 0)
        return vendor_id == 0x004f && vendor_codec_id == 0x0001;
    if (strcasecmp(codec, "aptX-HD") == 0)
        return vendor_id == 0x00d7 && vendor_codec_id == 0x0024;
    if (strcasecmp(codec, "LDAC") == 0)
        return vendor_id == 0x012d && vendor_codec_id == 0x00aa;
    return false;
}

/* A2DP capability bytes are transmitted most-significant-bit first, so each
 * codec's rate bitfield is read straight off the wire layout in
 * bluetooth-a2dp.h rather than through its packed struct. */
static int a2dp_caps_rates(const uint8_t * caps, size_t len, const char * codec,
                           unsigned int * out, int max_count) {
    typedef struct { unsigned int mask, rate; } rate_bit_t;
    static const rate_bit_t sbc_aptx[] = {
        { 1u << 3, 16000 }, { 1u << 2, 32000 }, { 1u << 1, 44100 }, { 1u << 0, 48000 },
    };
    static const rate_bit_t aac[] = {
        { 1u << 11, 8000 }, { 1u << 10, 11025 }, { 1u << 9, 12000 }, { 1u << 8, 16000 },
        { 1u << 7, 22050 }, { 1u << 6, 24000 }, { 1u << 5, 32000 }, { 1u << 4, 44100 },
        { 1u << 3, 48000 }, { 1u << 2, 64000 }, { 1u << 1, 88200 }, { 1u << 0, 96000 },
    };
    static const rate_bit_t ldac[] = {
        { 1u << 5, 44100 }, { 1u << 4, 48000 }, { 1u << 3, 88200 },
        { 1u << 2, 96000 }, { 1u << 1, 176400 }, { 1u << 0, 192000 },
    };

    const rate_bit_t * table = NULL;
    size_t table_n = 0;
    unsigned int field = 0;

    if (strcasecmp(codec, "SBC") == 0) {
        if (len < 1) return 0;
        field = caps[0] >> 4;
        table = sbc_aptx; table_n = sizeof(sbc_aptx) / sizeof(sbc_aptx[0]);
    } else if (strcasecmp(codec, "AAC") == 0) {
        if (len < 3) return 0;
        field = ((unsigned int) caps[1] << 4) | (caps[2] >> 4);
        table = aac; table_n = sizeof(aac) / sizeof(aac[0]);
    } else if (len >= 7) { /* vendor codecs carry 4-byte vendor + 2-byte codec first */
        if (strcasecmp(codec, "LDAC") == 0) {
            field = caps[6] & 0x3f;
            table = ldac; table_n = sizeof(ldac) / sizeof(ldac[0]);
        } else if (strcasecmp(codec, "aptX") == 0 ||
                   strcasecmp(codec, "aptX-HD") == 0) {
            /* a2dp_aptx_hd_t embeds a2dp_aptx_t, so both rate fields are at
             * byte 6 despite aptX-HD's additional trailing reserved bytes. */
            field = caps[6] >> 4;
            table = sbc_aptx; table_n = sizeof(sbc_aptx) / sizeof(sbc_aptx[0]);
        }
    }
    if (!table) return 0;

    /* Tables run low to high, so a forward walk lists rates in order. */
    int count = 0;
    for (size_t i = 0; i < table_n && count < max_count; i++)
        if (field & table[i].mask) out[count++] = table[i].rate;
    return count;
}

/* Re-establishes the accessory's link so a new transport rate takes effect.
 *
 * The rate is fixed when the A2DP configuration is negotiated, and these
 * accessories refuse to renegotiate a live transport: SelectCodec on a
 * connected PCM returns EIO and takes the link down with it (measured on a
 * real headset, in both clamped and unclamped daemons). Cycling the device
 * link is therefore the only thing that actually applies a rate change --
 * but only the link needs cycling, not the radio, so this does it instead
 * of asking the user to turn Bluetooth off and on.
 *
 * Blocks for several seconds. Callers must run it off the UI thread. */
bool bt_control_reconnect_for_rate_change(unsigned int want) {
    char mac[18];
    if (!bt_control_get_connected_device_mac(mac, sizeof(mac))) return false;
    if (want == 0) want = 44100; /* Automatic resolves the same way everywhere */
    /* Re-asserted rather than read from the atomic: disconnecting below makes
     * the status refresh publish "nothing connected", which would otherwise
     * overwrite the requested rate with the global default before the daemon
     * is respawned with it. */
    atomic_store(&bt_sample_rate, want);

    /* Try reconfiguring the live transport first. A Galaxy Buds2 Pro refuses
     * this with EIO and drops the link whether or not it is streaming
     * (measured), but accessories that accept it change rate with nothing
     * dropping at all, so it is worth one attempt before cycling the link. */
    char path[256];
    if (find_source_pcm_path(path, sizeof(path))) {
        char info_out[2048];
        char * info_argv[] = { (char *) bluealsa_ctl_name(), (char *) "info", path, NULL };
        if (subprocess_run_low_priority(info_argv, info_out, sizeof(info_out))) {
            char codec[32] = {0};
            copy_info_field(codec, sizeof(codec), info_out, "Selected codec:");
            char * codec_end = strchr(codec, ':');
            if (codec_end) *codec_end = '\0';

            if (codec[0]) {
                char rate_str[16];
                snprintf(rate_str, sizeof(rate_str), "%u", want);
                char * argv[] = { (char *) bluealsa_ctl_name(), (char *) "codec",
                                  (char *) "-r", rate_str, path, codec, NULL };
                (void) subprocess_run_checked(argv, NULL, 0, 5000, NULL);

                /* Believe the transport, not the exit status: a refusal can
                 * still leave the link up at the old rate. */
                unsigned int now = 0;
                if (find_source_pcm_path(path, sizeof(path)) &&
                        subprocess_run_low_priority(info_argv, info_out, sizeof(info_out))) {
                    const char * field = strstr(info_out, "Rate:");
                    if (field) (void) sscanf(field + strlen("Rate:"), "%u", &now);
                }
                if (now == want) return true; /* accepted in place, nothing dropped */
            }
        }
    }

    bt_control_disconnect(mac);
    /* The PCM outlives the disconnect briefly, and ensure_bluealsa_running()
     * refuses to respawn while it still sees one, so the respawn would be
     * skipped and the old arguments kept. Wait for it to go. */
    for (int waited_ms = 0; waited_ms < 5000; waited_ms += 250) {
        char gone[256];
        if (!find_source_pcm_path(gone, sizeof(gone))) break;
        usleep(250000);
    }
    ensure_bluealsa_running();
    bt_control_connect(mac);

    /* Believe the transport, not bluetoothctl's exit: it reports failure on a
     * slow connect that then completes on its own a few seconds later, which
     * otherwise told the user it had failed right before it worked. */
    /* Bounded on the clock, not on iterations: each probe forks a subprocess
     * that can itself take up to the subprocess timeout, so counting rounds
     * would let this run for minutes and stall a teardown that joins it. */
    uint32_t deadline = bt_control_monotonic_ms() + 20000;
    while ((int32_t) (deadline - bt_control_monotonic_ms()) > 0) {
        usleep(500000);
        char path[256];
        if (!find_source_pcm_path(path, sizeof(path))) continue;
        char info_out[2048];
        char * argv[] = { (char *) bluealsa_ctl_name(), (char *) "info", path, NULL };
        if (!subprocess_run_low_priority(argv, info_out, sizeof(info_out))) continue;
        unsigned int now = 0;
        const char * field = strstr(info_out, "Rate:");
        if (field) (void) sscanf(field + strlen("Rate:"), "%u", &now);
        if (now == want) return true;
    }
    return false;
}

int bt_control_get_available_rates(unsigned int * out, int max_count) {
    if (!out || max_count <= 0) return 0;

    char path[256];
    if (!find_source_pcm_path(path, sizeof(path))) return 0;

    char info_out[2048];
    char * info_argv[] = { (char *) bluealsa_ctl_name(), (char *) "info", path, NULL };
    if (!subprocess_run_low_priority(info_argv, info_out, sizeof(info_out))) return 0;

    char device_path[160] = {0};
    copy_info_field(device_path, sizeof(device_path), info_out, "Device:");
    char codec[32] = {0};
    copy_info_field(codec, sizeof(codec), info_out, "Selected codec:");
    char * codec_end = strchr(codec, ':');
    if (codec_end) *codec_end = '\0';
    if (!device_path[0] || !codec[0]) return 0;

    /* The accessory's endpoints hang off its BlueZ object as sepN children,
     * listed at the very END of the introspection XML after every interface
     * and method. That document runs past 5 KiB on a real headset, so this
     * buffer is generous and heap-allocated rather than sitting on the
     * worker's stack -- undersizing it silently drops the sep nodes and the
     * rate list comes back empty. */
    enum { BT_INTROSPECT_MAX = 16384 };
    char * introspect = malloc(BT_INTROSPECT_MAX);
    if (!introspect) return 0;
    char * intro_argv[] = { (char *) "dbus-send", (char *) "--system", (char *) "--print-reply",
                            (char *) "--dest=org.bluez", device_path,
                            (char *) "org.freedesktop.DBus.Introspectable.Introspect", NULL };
    if (!subprocess_run_low_priority(intro_argv, introspect, BT_INTROSPECT_MAX)) {
        free(introspect);
        return 0;
    }

    const char * scan = introspect;
    const char * node;
    while ((node = strstr(scan, "<node name=\"sep")) != NULL) {
        scan = node + 1;
        const char * name = node + strlen("<node name=\"");
        size_t name_len = strcspn(name, "\"");
        if (name_len == 0 || name_len >= 16) continue;

        char sep_path[192];
        snprintf(sep_path, sizeof(sep_path), "%s/%.*s", device_path, (int) name_len, name);

        char props[2048];
        char * prop_argv[] = { (char *) "dbus-send", (char *) "--system", (char *) "--print-reply",
                               (char *) "--dest=org.bluez", sep_path,
                               (char *) "org.freedesktop.DBus.Properties.GetAll",
                               (char *) "string:org.bluez.MediaEndpoint1", NULL };
        if (!subprocess_run_low_priority(prop_argv, props, sizeof(props))) continue;

        /* Only the accessory's SINK endpoint matters: that is what this
         * device streams into. */
        if (!dbus_prop_string_equals(props, "UUID",
                                     "0000110b-0000-1000-8000-00805f9b34fb")) continue;

        uint8_t codec_id;
        if (!dbus_prop_byte(props, "Codec", &codec_id)) continue;

        uint8_t caps[32];
        size_t caps_len = 0;
        if (!dbus_prop_byte_array(props, "Capabilities", caps, sizeof(caps), &caps_len)) continue;
        if (!a2dp_caps_match_codec(caps, caps_len, codec_id, codec)) continue;

        int n = a2dp_caps_rates(caps, caps_len, codec, out, max_count);
        if (n > 0) {
            free(introspect);
            return n;
        }
    }
    free(introspect);
    return 0;
}

bool bt_control_prepare_playback_pcm(char * out, size_t out_size) {
    if (!out || out_size == 0) return false;

    char path[256];
    if (!find_source_pcm_path(path, sizeof(path))) return false;

    /* Derived from the path already in hand: bt_control_get_connected_device_mac()
     * would repeat the list-pcms this just did. */
    const char * dev = strstr(path, "dev_");
    if (!dev || strlen(dev + 4) < 17) return false;
    dev += 4;
    char mac[18];
    for (int i = 0; i < 17; i++) mac[i] = (dev[i] == '_') ? ':' : dev[i];
    mac[17] = '\0';

    char info_out[2048];
    char * info_argv[] = { (char *) bluealsa_ctl_name(), (char *) "info", path, NULL };
    if (!subprocess_run_low_priority(info_argv, info_out, sizeof(info_out))) return false;

    unsigned int rate = 0;
    unsigned int channels = 0;
    const char * rate_field = strstr(info_out, "Rate:");
    if (rate_field) (void) sscanf(rate_field + strlen("Rate:"), "%u", &rate);
    const char * channels_field = strstr(info_out, "Channels:");
    if (channels_field) (void) sscanf(channels_field + strlen("Channels:"), "%u", &channels);
    if (rate == 0) return false;
    if (channels == 0) channels = 2;

    if (mkdir(BT_ALSA_CONFIG_DIR, 0755) != 0 && errno != EEXIST) return false;

    char tmp_path[128];
    char final_path[128];
    snprintf(tmp_path, sizeof(tmp_path), "%s/.asoundrc.tmp", BT_ALSA_CONFIG_DIR);
    snprintf(final_path, sizeof(final_path), "%s/.asoundrc", BT_ALSA_CONFIG_DIR);

    FILE * f = fopen(tmp_path, "w");
    if (!f) return false;
    fprintf(f, "pcm.%s {\n", BT_ALSA_PCM_NAME);
    fprintf(f, "    type plug\n");
    fprintf(f, "    slave {\n");
    fprintf(f, "        pcm {\n");
    fprintf(f, "            type bluealsa\n");
    fprintf(f, "            device \"%s\"\n", mac);
    fprintf(f, "            profile \"a2dp\"\n");
    const char * codec = bt_codec_select_name();
    if (codec) fprintf(f, "            codec \"%s\"\n", codec);
    fprintf(f, "        }\n");
    fprintf(f, "        rate %u\n", rate);
    fprintf(f, "        channels %u\n", channels);
    fprintf(f, "    }\n");
    if (atomic_load(&bt_speexrate_enabled) && access(BT_ALSA_RATE_CONVERTER_PLUGIN, F_OK) == 0)
        fprintf(f, "    rate_converter \"%s\"\n", BT_ALSA_RATE_CONVERTER);
    fprintf(f, "}\n");
    bool ok = fflush(f) == 0 && fsync(fileno(f)) == 0;
    ok = (fclose(f) == 0) && ok;
    if (!ok) { remove(tmp_path); return false; }
    if (rename(tmp_path, final_path) != 0) { remove(tmp_path); return false; }

    snprintf(out, out_size, "%s", BT_ALSA_PCM_NAME);
    DBG_LOG("bt_control: BT output pinned to %u Hz %u ch for %s\n", rate, channels, mac);
    return true;
}

static pthread_t bt_source_vol_sync_thread;
static atomic_bool bt_source_vol_sync_active = false;
static bool bt_source_vol_sync_joinable;
static atomic_int bt_source_vol_pending_percent = -1;

/* Set by whichever direction writes/observes a value most recently, so the
 * other direction recognizes it as already in sync instead of re-writing
 * it right back, just shared between two directions here instead of one
 * direction echoing itself. */
static int bt_source_vol_last_synced_raw = -1;

static void bt_source_push_app_volume_if_changed(float * last_synced_app_percent) {
    float current_percent = audio_get_volume();
    if (current_percent == *last_synced_app_percent) return;

    int raw = (int) (current_percent * (float) BT_SOURCE_VOLUME_MAX + 0.5f);
    if (raw < 0) raw = 0;
    if (raw > BT_SOURCE_VOLUME_MAX) raw = BT_SOURCE_VOLUME_MAX;

    if (raw == bt_source_vol_last_synced_raw) {
        *last_synced_app_percent = current_percent;
        return;
    }

    char path[256];
    if (!find_source_pcm_path(path, sizeof(path))) return;

    char raw_str[8];
    snprintf(raw_str, sizeof(raw_str), "%d", raw);
    char * vol_argv[] = { (char *) bluealsa_ctl_name(), (char *) "volume", path, raw_str, raw_str, NULL };
    if (!subprocess_run(vol_argv, NULL, 0)) return;

    bt_source_vol_last_synced_raw = raw;
    *last_synced_app_percent = current_percent;
    hiby_sys_server_report_volume((int) (current_percent * 100.0f + 0.5f));
}

static void * bt_source_vol_sync_thread_func(void * arg) {
    (void) arg;

    /* Establish the already-visible player value before subscribing to
     * bluealsa property changes. Starting the monitor first can queue the
     * accessory's remembered pre-sync value, which would then overwrite
     * the app even if we push immediately afterward. */
    float last_synced_app_percent = -1.0f;
    bt_source_push_app_volume_if_changed(&last_synced_app_percent);
    if (!atomic_load(&bt_source_vol_sync_active)) return NULL;

    char * argv[] = { (char *) bluealsa_ctl_name(), (char *) "monitor", (char *) "-p", NULL };
    pid_t pid;
    int read_fd;
    if (!subprocess_popen(argv, &pid, &read_fd)) {
        atomic_store(&bt_source_vol_sync_active, false);
        return NULL;
    }

    char buf[512];
    size_t buf_len = 0;

    while (bt_source_vol_sync_active) {
        /* Push app-driven state BEFORE reading monitor events. In
         * particular, the first iteration must make the already-visible
         * player percentage authoritative before bluealsa's monitor can
         * report the accessory's remembered value; doing this afterward
         * let that startup event silently change audio_get_volume() while
         * the slider/topbar still showed the old app value. */
        bt_source_push_app_volume_if_changed(&last_synced_app_percent);

        /* Bounded, not a blocking fgets() -- this loop also needs to notice
         * this app's OWN volume changing (UI slider, hardware buttons), which
         * has no fd to select() on, so it polls this monitor's pipe with a
         * short timeout instead of blocking on it indefinitely. 500ms is
         * imprecise for a "keep two volume controls in sync" feature (not
         * a latency-sensitive control path), matched on both sides of this
         * loop -- see the push check below. */
        struct pollfd pfd = { .fd = read_fd, .events = POLLIN };
        int pr = poll(&pfd, 1, 500);
        if (!atomic_load(&bt_source_vol_sync_active)) break;
        if (pr < 0 && errno != EINTR) break;
        if (pr > 0 && (pfd.revents & (POLLERR | POLLNVAL))) break;
        if (pr > 0 && (pfd.revents & POLLHUP) && !(pfd.revents & POLLIN)) break;

        if (pr > 0 && (pfd.revents & POLLIN)) {
            if (buf_len >= sizeof(buf) - 1) buf_len = 0; /* defensive -- a line this long can't be a real PropertyChanged message, drop and resync */
            ssize_t n = read(read_fd, buf + buf_len, sizeof(buf) - 1 - buf_len);
            if (n <= 0) break; /* monitor subprocess died or its pipe closed */
            buf_len += (size_t) n;
            buf[buf_len] = '\0';

            char * line_start = buf;
            char * newline;
            while ((newline = memchr(line_start, '\n', buf_len - (size_t) (line_start - buf))) != NULL) {
                *newline = '\0';
                char path[256];
                int raw;
                char volume_text[64];
                if (sscanf(line_start, "PropertyChanged %255s Volume %63s", path, volume_text) == 2 &&
                    strstr(path, "/a2dpsrc/sink") != NULL &&
                    parse_monitor_volume(volume_text, &raw)) {
                    if (raw != bt_source_vol_last_synced_raw) {
                        bt_source_vol_last_synced_raw = raw;
                        last_synced_app_percent = (float) raw / (float) BT_SOURCE_VOLUME_MAX;
                        audio_set_volume(last_synced_app_percent);
                        int percent = (raw * 100 + BT_SOURCE_VOLUME_MAX / 2) /
                                      BT_SOURCE_VOLUME_MAX;
                        atomic_store_explicit(&bt_source_vol_pending_percent, percent,
                                              memory_order_release);
                    }
                }
                line_start = newline + 1;
            }
            size_t remaining = buf_len - (size_t) (line_start - buf);
            memmove(buf, line_start, remaining);
            buf_len = remaining;
        }

    }

    close(read_fd);
    subprocess_terminate(pid); /* worker owns and reaps its own child */
    atomic_store(&bt_source_vol_sync_active, false);
    return NULL;
}

bool bt_control_source_volume_sync_is_running(void) {
    return atomic_load(&bt_source_vol_sync_active);
}

void bt_control_source_volume_sync_start(void) {
    if (bt_source_vol_sync_joinable) {
        if (atomic_load(&bt_source_vol_sync_active)) return;
        pthread_join(bt_source_vol_sync_thread, NULL);
        bt_source_vol_sync_joinable = false;
    }
    bt_source_vol_sync_active = true;
    bt_source_vol_last_synced_raw = -1;
    atomic_store_explicit(&bt_source_vol_pending_percent, -1, memory_order_relaxed);
    if (pthread_create(&bt_source_vol_sync_thread, NULL, bt_source_vol_sync_thread_func, NULL) == 0)
        bt_source_vol_sync_joinable = true;
    else bt_source_vol_sync_active = false;
}

void bt_control_source_volume_sync_stop(void) {
    if (!bt_source_vol_sync_joinable) return;
    /* Cancellation is observed after any in-flight command and the bounded
     * pipe poll. This join belongs on the lifecycle worker, never LVGL. */
    bt_source_vol_sync_active = false;
    pthread_join(bt_source_vol_sync_thread, NULL);
    bt_source_vol_sync_joinable = false;
    atomic_store_explicit(&bt_source_vol_pending_percent, -1, memory_order_release);
}

bool bt_control_source_volume_sync_consume_percent(int * out_percent) {
    int percent = atomic_exchange_explicit(&bt_source_vol_pending_percent, -1,
                                           memory_order_acq_rel);
    if (percent < 0) return false;
    if (out_percent) *out_percent = percent;
    return true;
}

/* See bt_control_output_disconnect_watch_start()'s own doc comment
 * (bluetooth_control.h) for what this is and why. Plain `monitor`, no `-p`
 * needed -- confirmed via `strings` on the real bluealsactl binary that
 * PCMAdded/PCMRemoved come from its base InterfacesAdded/InterfacesRemoved
 * subscription (path_namespace='/org/bluealsa'), which is always active;
 * `-p` only adds the separate PropertyChanged stream (Volume/Codec/Running/
 * SoftVolume) the two volume-sync threads above already use -- this doesn't
 * need any of that. "/a2dpsrc/sink" matches find_source_pcm_path()'s own
 * naming exactly: the PCM this app itself writes local playback into
 * (audio_output.c's `aplay -D bluealsa`), not any other PCM bluealsa might
 * have (e.g. one from DAC mode). */
static pthread_t bt_output_disconnect_thread;
static atomic_bool bt_output_disconnect_active = false;
static bool bt_output_disconnect_joinable;
static atomic_bool bt_output_disconnect_flag = false;
static pthread_mutex_t bt_output_disconnect_state_mutex = PTHREAD_MUTEX_INITIALIZER;
static char bt_output_disconnect_confirmed_path[256];

#define BT_OUTPUT_DISCONNECT_MAX_ADDED_PATHS 16
/* Codec re-selection after a failed attempt: 2 s, 4 s, 6 s, 8 s apart. */
#define BT_CODEC_RETRY_BASE_MS 2000u
#define BT_CODEC_RETRY_MAX_ATTEMPTS 4

/* Preference worker, owned by the output watch thread. Codec, rate and
 * soft-volume selection run bluealsactl subprocesses bounded by their own
 * timeouts; doing that here keeps the watch thread reading PCMRemoved/
 * PCMAdded, so a disconnect is noticed on time. The watch thread and
 * reconciliation only post requests (short lock, no I/O).
 *
 * One PCM is tracked at a time. A failed codec selection is retried
 * BT_CODEC_RETRY_MAX_ATTEMPTS times, spaced out, and never while a
 * disconnect is pending. The budget belongs to the PCM path: a PCMAdded for
 * the same path (a replacement, which a codec selection itself can cause)
 * asks only for rate and soft volume to be re-applied, so a device that
 * keeps falling back is not switched back and forth without end; only a
 * different path or a confirmed disconnect starts over. */
static struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    pthread_t thread;
    bool running;        /* read/written under mutex */
    atomic_bool stop;
    bool paused;         /* a removal is pending: no new selection */
    bool in_flight;      /* a selection pass is running */
    bool scan_requested; /* look for a PCM that predates the subscription */
    bool codec_due;      /* select the codec for path now */
    bool refresh_due;    /* re-apply rate and soft volume only */
    char path[256];
    int attempts;        /* failed codec selections for path */
    uint32_t retry_at;
} bt_pref = { .mutex = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER };

static bool bt_pref_retry_scheduled_locked(void) {
    return bt_pref.path[0] && bt_pref.attempts > 0 && bt_pref.attempts < BT_CODEC_RETRY_MAX_ATTEMPTS;
}

static void * bt_pref_worker_func(void * arg) {
    (void) arg;
    pthread_mutex_lock(&bt_pref.mutex);
    while (!atomic_load(&bt_pref.stop)) {
        uint32_t now = bt_control_monotonic_ms();
        bool retry_due = !bt_pref.paused && bt_pref_retry_scheduled_locked() &&
                         (int32_t) (now - bt_pref.retry_at) >= 0;
        bool codec_work = !bt_pref.paused && bt_pref.path[0] && (bt_pref.codec_due || retry_due);
        bool refresh_work = !bt_pref.paused && bt_pref.path[0] && bt_pref.refresh_due;
        if (bt_pref.scan_requested) {
            bt_pref.scan_requested = false;
            pthread_mutex_unlock(&bt_pref.mutex);
            char found[256];
            bool present = !atomic_load(&bt_pref.stop) &&
                           find_source_pcm_path_ex(found, sizeof(found), false);
            pthread_mutex_lock(&bt_pref.mutex);
            if (present && !bt_pref.path[0]) {
                snprintf(bt_pref.path, sizeof(bt_pref.path), "%s", found);
                bt_pref.attempts = 0;
                bt_pref.codec_due = true;
            }
            continue;
        }
        if (!codec_work && !refresh_work) {
            uint32_t wait_ms = 1000;
            if (!bt_pref.paused && bt_pref_retry_scheduled_locked()) {
                int32_t left = (int32_t) (bt_pref.retry_at - now);
                wait_ms = left <= 0 ? 0 : (uint32_t) left < wait_ms ? (uint32_t) left : wait_ms;
            }
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += wait_ms / 1000;
            ts.tv_nsec += (long) (wait_ms % 1000) * 1000000L;
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            pthread_cond_timedwait(&bt_pref.cond, &bt_pref.mutex, &ts);
            continue;
        }
        char path[256];
        snprintf(path, sizeof(path), "%s", bt_pref.path);
        bt_pref.codec_due = false;
        bt_pref.refresh_due = false;
        bt_pref.in_flight = true;
        pthread_mutex_unlock(&bt_pref.mutex);

        /* Codec first: selecting one can cycle the PCM, which would
         * otherwise drop a soft-volume setting applied just before. Stop is
         * checked between steps, so it waits for at most one of them. */
        bool applied = codec_work ? bluealsa_apply_codec_preference_cancellable(path, &bt_pref.stop) : true;
        /* Refresh-only passes too: the rate step waits for a verified codec,
         * so it never reconfigures a fallback codec or pre-empts a retry. */
        bool codec_verified = bluealsa_codec_verified(path);
        /* The rate belongs to the codec just selected: skipped while that
         * selection has not succeeded, so a fallback codec's refusal is not
         * recorded (and later applied) in its place. */
        bool rate_reconfigured = applied && codec_verified && !atomic_load(&bt_pref.stop) &&
                                 bluealsa_apply_rate_preference_cancellable(path, &bt_pref.stop);
        if (rate_reconfigured) {
            /* The link was re-established for the rate: the codec it ended
             * up on must be checked again (cheap when unchanged). */
            bluealsa_clear_codec_path(path);
        }
        bluealsa_apply_soft_volume_cancellable(path, &bt_pref.stop);

        pthread_mutex_lock(&bt_pref.mutex);
        bt_pref.in_flight = false;
        if (rate_reconfigured && applied && strcmp(bt_pref.path, path) == 0 &&
            bt_pref.attempts < BT_CODEC_RETRY_MAX_ATTEMPTS)
            bt_pref.codec_due = true;
        if (codec_work && strcmp(bt_pref.path, path) == 0) {
            if (applied) {
                bt_pref.attempts = 0;
            } else if (++bt_pref.attempts < BT_CODEC_RETRY_MAX_ATTEMPTS) {
                bt_pref.retry_at = bt_control_monotonic_ms() + BT_CODEC_RETRY_BASE_MS * (uint32_t) bt_pref.attempts;
            } else {
                DBG_LOG("bt_control: codec selection for %s gave up after %d attempts\n",
                        path, bt_pref.attempts);
            }
        }
    }
    pthread_mutex_unlock(&bt_pref.mutex);
    return NULL;
}

static void bt_pref_start(void) {
    pthread_mutex_lock(&bt_pref.mutex);
    atomic_store(&bt_pref.stop, false);
    bt_pref.paused = false;
    bt_pref.in_flight = false;
    bt_pref.scan_requested = false;
    bt_pref.codec_due = false;
    bt_pref.refresh_due = false;
    bt_pref.path[0] = '\0';
    bt_pref.attempts = 0;
    bt_pref.running = pthread_create(&bt_pref.thread, NULL, bt_pref_worker_func, NULL) == 0;
    pthread_mutex_unlock(&bt_pref.mutex);
}

/* Joins the worker; it finishes at most the step it is running. */
static void bt_pref_stop(void) {
    pthread_mutex_lock(&bt_pref.mutex);
    bool running = bt_pref.running;
    atomic_store(&bt_pref.stop, true);
    pthread_cond_signal(&bt_pref.cond);
    pthread_mutex_unlock(&bt_pref.mutex);
    if (!running) return;
    pthread_join(bt_pref.thread, NULL);
    pthread_mutex_lock(&bt_pref.mutex);
    bt_pref.running = false;
    pthread_mutex_unlock(&bt_pref.mutex);
}

static void bt_pref_request_scan(void) {
    pthread_mutex_lock(&bt_pref.mutex);
    bt_pref.scan_requested = true;
    pthread_cond_signal(&bt_pref.cond);
    pthread_mutex_unlock(&bt_pref.mutex);
}

/* A PCMAdded (or reconciliation). Returns false when no worker is running,
 * leaving the caller to apply inline. */
static bool bt_pref_request(const char * path) {
    pthread_mutex_lock(&bt_pref.mutex);
    bool running = bt_pref.running && !atomic_load(&bt_pref.stop);
    if (running) {
        if (strcmp(bt_pref.path, path) != 0) {
            snprintf(bt_pref.path, sizeof(bt_pref.path), "%.*s", (int) sizeof(bt_pref.path) - 1, path);
            bt_pref.attempts = 0;
            bt_pref.codec_due = true;
            bt_pref.refresh_due = false;
        } else {
            /* Same PCM again. Its soft volume must be re-applied; the codec
             * only when no selection is in flight or scheduled (a settled
             * success is then a cheap latch check). */
            bt_pref.refresh_due = true;
            if (!bt_pref.in_flight && bt_pref.attempts == 0) bt_pref.codec_due = true;
        }
        pthread_cond_signal(&bt_pref.cond);
    }
    pthread_mutex_unlock(&bt_pref.mutex);
    return running;
}

/* Mirrors the watch's pending-removal state: no new selection meanwhile. */
static void bt_pref_set_paused(bool paused) {
    pthread_mutex_lock(&bt_pref.mutex);
    if (bt_pref.paused != paused) {
        bt_pref.paused = paused;
        pthread_cond_signal(&bt_pref.cond);
    }
    pthread_mutex_unlock(&bt_pref.mutex);
}

/* A confirmed disconnect: the next connection starts with a fresh budget. */
static void bt_pref_forget(const char * path) {
    pthread_mutex_lock(&bt_pref.mutex);
    if (!path || strcmp(bt_pref.path, path) == 0) {
        bt_pref.path[0] = '\0';
        bt_pref.attempts = 0;
        bt_pref.codec_due = false;
        bt_pref.refresh_due = false;
    }
    pthread_mutex_unlock(&bt_pref.mutex);
}

static void bt_output_disconnect_process_line(const char * line, char * pending_path,
                                              uint32_t * pending_deadline,
                                              char added_paths[][256], int * added_count) {
    char path[256];
    if (sscanf(line, "PCMRemoved %255s", path) == 1) {
        if (strstr(path, "/a2dpsrc/sink") != NULL) {
            snprintf(pending_path, 256, "%s", path);
            *pending_deadline = bt_control_monotonic_ms() + BT_OUTPUT_RECONFIGURE_GRACE_MS;
            /* Soft volume only: the PCM is recreated by a reconfigure, so
             * that setting must be re-applied. The codec latch deliberately
             * survives here and is cleared only once a disconnect is
             * actually confirmed -- selecting a codec can itself cycle the
             * PCM, and clearing on every PCMRemoved would let the resulting
             * PCMAdded re-select, cycling it again without end. */
            bluealsa_clear_soft_volume_path(path);
            DBG_LOG("bt_control: output_disconnect_watch: PCMRemoved %s -- pending disconnect\n", path);
        }
        return;
    }
    if (strncmp(line, "PCMAdded ", 9) != 0 || sscanf(line, "PCMAdded %255s", path) != 1) return;
    if (strcmp(pending_path, path) == 0) {
        pending_path[0] = '\0';
        *pending_deadline = 0;
        DBG_LOG("bt_control: output_disconnect_watch: PCMAdded %s -- cancelled pending disconnect\n", path);
    }
    /* A replacement can arrive after the deadline but before the UI consumes
     * the event. Cancel only the event for this exact PCM, under the same
     * short lock used by the UI consumer. Never hold it during control I/O. */
    pthread_mutex_lock(&bt_output_disconnect_state_mutex);
    if (strcmp(bt_output_disconnect_confirmed_path, path) == 0) {
        atomic_store(&bt_output_disconnect_flag, false);
        bt_output_disconnect_confirmed_path[0] = '\0';
    }
    pthread_mutex_unlock(&bt_output_disconnect_state_mutex);
    if (*added_count < BT_OUTPUT_DISCONNECT_MAX_ADDED_PATHS)
        snprintf(added_paths[(*added_count)++], 256, "%s", path);
    DBG_LOG("bt_control: output_disconnect_watch: %s\n", line);
}

static bool bt_output_disconnect_read_batch(int fd, char * buf, size_t * used,
                                            char * pending_path, uint32_t * pending_deadline,
                                            char added_paths[][256], int * added_count) {
    if (*used == 511) *used = 0;
    ssize_t n = read(fd, buf + *used, 511 - *used);
    if (n <= 0) return false;
    *used += (size_t)n;
    buf[*used] = '\0';
    char * line = buf;
    char * end;
    while ((end = strchr(line, '\n')) != NULL) {
        *end = '\0';
        bt_output_disconnect_process_line(line, pending_path, pending_deadline,
                                          added_paths, added_count);
        line = end + 1;
    }
    size_t remaining = *used - (size_t)(line - buf);
    memmove(buf, line, remaining);
    *used = remaining;
    return true;
}

static void bt_output_disconnect_publish_if_due(char * pending_path, uint32_t * pending_deadline) {
    if (!pending_path[0] || (int32_t)(bt_control_monotonic_ms() - *pending_deadline) < 0) return;
    pthread_mutex_lock(&bt_output_disconnect_state_mutex);
    if (atomic_load(&bt_output_disconnect_active) && !atomic_load(&bt_output_disconnect_flag)) {
        snprintf(bt_output_disconnect_confirmed_path, sizeof(bt_output_disconnect_confirmed_path), "%s", pending_path);
        atomic_store(&bt_output_disconnect_flag, true);
    }
    pthread_mutex_unlock(&bt_output_disconnect_state_mutex);
    DBG_LOG("bt_control: output_disconnect_watch: PCMRemoved %s -- flagging disconnect\n", pending_path);
    bt_auto_note_disconnect(pending_path);
    bluealsa_clear_codec_path(pending_path); /* real disconnect: next connect must re-select */
    bt_pref_forget(pending_path);
    pending_path[0] = '\0';
    *pending_deadline = 0;
}

static void * bt_output_disconnect_thread_func(void * arg) {
    (void) arg;

    char * argv[] = { (char *) bluealsa_ctl_name(), (char *) "monitor", NULL };
    pid_t pid;
    int read_fd;
    if (!subprocess_popen(argv, &pid, &read_fd)) {
        DBG_LOG("bt_control: output_disconnect_watch: failed to spawn bluealsactl monitor\n");
        atomic_store(&bt_output_disconnect_active, false);
        return NULL;
    }
    DBG_LOG("bt_control: output_disconnect_watch: monitor started (pid %d)\n", (int) pid);

    /* The GUI starts this watch only after it has seen the A2DP link, so the
     * PCM of that connection usually predates the subscription and never
     * produces PCMAdded here. That left a first connection (no BlueZ
     * LastUsed endpoint yet, e.g. right after pairing) on whatever codec
     * BlueZ happened to pick, such as AAC instead of a chosen LDAC, until a
     * reconnect. The worker scans for such a PCM once the monitor has
     * printed its first line: bluealsactl installs its D-Bus matches before
     * printing ServiceRunning/ServiceStopped, so from then on any new PCM is
     * reported below; if nothing arrives the scan still runs after 2 s. */
    bt_pref_start();
    bool scanned_on_ready = false, scanned_on_fallback = false;
    uint32_t scan_fallback_at = bt_control_monotonic_ms() + 2000;

    char buf[512];
    size_t used = 0;
    char pending_path[256] = "";
    uint32_t pending_deadline = 0;
    char added_paths[BT_OUTPUT_DISCONNECT_MAX_ADDED_PATHS][256];
    int added_count = 0;
    while (atomic_load(&bt_output_disconnect_active)) {
        uint32_t now = bt_control_monotonic_ms();
        int timeout_ms = 100;
        if (pending_path[0]) {
            int32_t remaining = (int32_t)(pending_deadline - now);
            timeout_ms = remaining <= 0 ? 0 : (remaining < timeout_ms ? remaining : timeout_ms);
        }
        struct pollfd pfd = { .fd = read_fd, .events = POLLIN };
        int pr = poll(&pfd, 1, timeout_ms);
        if (!atomic_load(&bt_output_disconnect_active)) break;
        if (pr < 0) { if (errno == EINTR) continue; break; }
        bool terminal = false;
        if (pr > 0 && (pfd.revents & POLLIN))
            terminal = !bt_output_disconnect_read_batch(read_fd, buf, &used, pending_path,
                                                         &pending_deadline, added_paths, &added_count);
        /* Scan on readiness; a fallback scan (monitor slow to print) does
         * not replace it, since a PCM could appear between the two. */
        if (!scanned_on_ready && pr > 0 && (pfd.revents & POLLIN) && !terminal) {
            scanned_on_ready = true;
            bt_pref_request_scan();
        } else if (!scanned_on_ready && !scanned_on_fallback &&
                   (int32_t) (bt_control_monotonic_ms() - scan_fallback_at) >= 0) {
            scanned_on_fallback = true;
            bt_pref_request_scan();
        }
        if (!terminal) {
            struct pollfd drain = { .fd = read_fd, .events = POLLIN };
            while (atomic_load(&bt_output_disconnect_active) && poll(&drain, 1, 0) > 0) {
                if (!(drain.revents & POLLIN)) {
                    terminal = (drain.revents & (POLLERR | POLLNVAL | POLLHUP)) != 0;
                    break;
                }
                if (!bt_output_disconnect_read_batch(read_fd, buf, &used, pending_path,
                                                     &pending_deadline, added_paths, &added_count)) {
                    terminal = true;
                    break;
                }
                drain.revents = 0;
            }
        }
        if (!atomic_load(&bt_output_disconnect_active)) break;
        if (terminal || (pr > 0 && (pfd.revents & (POLLERR | POLLNVAL | POLLHUP)))) {
            /* A failed monitor may have missed the replacement. Leave an
             * unconfirmed removal to periodic polling rather than inventing
             * a disconnect after its event stream has ended. */
            break;
        }
        bt_output_disconnect_publish_if_due(pending_path, &pending_deadline);
        bt_pref_set_paused(pending_path[0] != '\0');
        /* Do not let volume subprocesses delay reading a replacement while
         * a removal is pending. Keep the bounded queue until it settles. */
        if (!pending_path[0]) {
            for (int i = 0; i < added_count; i++) {
                if (!bt_pref_request(added_paths[i])) {
                    /* No worker (thread creation failed): apply inline. */
                    bluealsa_apply_codec_preference(added_paths[i]);
                    bluealsa_apply_rate_preference(added_paths[i]);
                    bluealsa_apply_soft_volume(added_paths[i]);
                }
            }
            added_count = 0;
        }
    }

    DBG_LOG("bt_control: output_disconnect_watch: monitor exited (pid %d)\n", (int) pid);
    bt_pref_stop();
    /* The monitor can exit with a removal still unconfirmed, which would
     * leave the latch set and suppress the re-select on the next connect. */
    bluealsa_clear_codec_path(NULL);
    close(read_fd);
    subprocess_terminate(pid);
    atomic_store(&bt_output_disconnect_active, false);
    return NULL;
}

bool bt_control_output_disconnect_watch_is_running(void) {
    return atomic_load(&bt_output_disconnect_active);
}

void bt_control_output_disconnect_watch_start(void) {
    if (bt_output_disconnect_joinable) {
        if (atomic_load(&bt_output_disconnect_active)) return;
        pthread_join(bt_output_disconnect_thread, NULL);
        bt_output_disconnect_joinable = false;
    }
    pthread_mutex_lock(&bt_output_disconnect_state_mutex);
    bt_output_disconnect_active = true;
    bt_output_disconnect_flag = false;
    bt_output_disconnect_confirmed_path[0] = '\0';
    pthread_mutex_unlock(&bt_output_disconnect_state_mutex);
    if (pthread_create(&bt_output_disconnect_thread, NULL, bt_output_disconnect_thread_func, NULL) == 0)
        bt_output_disconnect_joinable = true;
    else bt_output_disconnect_active = false;
}

void bt_control_output_disconnect_watch_stop(void) {
    bt_output_disconnect_active = false;
    if (bt_output_disconnect_joinable) {
        pthread_join(bt_output_disconnect_thread, NULL);
        bt_output_disconnect_joinable = false;
    }
    pthread_mutex_lock(&bt_output_disconnect_state_mutex);
    atomic_store(&bt_output_disconnect_flag, false);
    bt_output_disconnect_confirmed_path[0] = '\0';
    pthread_mutex_unlock(&bt_output_disconnect_state_mutex);
}

bool bt_control_output_disconnect_consume(void) {
    pthread_mutex_lock(&bt_output_disconnect_state_mutex);
    bool disconnected = atomic_exchange(&bt_output_disconnect_flag, false);
    bt_output_disconnect_confirmed_path[0] = '\0';
    pthread_mutex_unlock(&bt_output_disconnect_state_mutex);
    return disconnected;
}

/* Recorded so bt_control_reapply_last_output_settings() (the wedge
 * recovery in bt_control_is_powered(), above) can restore this after a
 * bluetoothd restart -- a fresh bluetoothd/bt_resume-equivalent bring-up
 * always comes back up in the plain a2dp-source default, not whatever
 * profile was actually requested last. */
static bool last_applied_dac_mode_enabled = false;
static bool last_applied_volume_sync_enabled = false;
static bool output_settings_ever_applied = false;

#define BLUEALSA_STARTUP_LOG_PATH "/usr/data/bluealsa_startup.log"

/* Spawns bluealsa daemon, logging startup output if TEST_BUILD_TAG is defined. */
static bool spawn_bluealsa_and_verify(char * const argv[]) {
#if defined(TEST_BUILD_TAG)
    return subprocess_spawn_daemon_logged(argv, BLUEALSA_STARTUP_LOG_PATH);
#else
    return subprocess_spawn_daemon_logged(argv, NULL);
#endif
}

bool bt_control_apply_output_settings(bool dac_mode_enabled, bool volume_sync_enabled) {
    /* Serializes bluealsa/bt-agent respawns and updates last-applied settings
     * to protect against concurrent caller races. */
    pthread_mutex_lock(&bt_chip_mutex);
    pthread_mutex_lock(&bt_daemon_respawn_mutex);
    last_applied_dac_mode_enabled = dac_mode_enabled;
    last_applied_volume_sync_enabled = volume_sync_enabled;
    output_settings_ever_applied = true;
    /* DAC mode always uses BlueALSA's software volume: bluealsa-aplay would
     * otherwise apply the phone's AVRCP volume to an ALSA "Master" control,
     * which this hardware does not have, so phone volume changes were lost. */
    atomic_store(&modern_soft_volume_requested, dac_mode_enabled || !volume_sync_enabled);

    /* Single profile mode: runs a2dp-sink or a2dp-source. */
    /* Clean up existing instances before respawning daemons. The cache is
     * cleared only after the old monitor thread is joined, so it cannot
     * record a path again in between. */
    bt_dac_info_monitor_stop();
    bluealsa_clear_soft_volume_path(NULL);

    bluealsa_backend_t backend = bluealsa_backend();
    subprocess_kill_all_matching(backend.daemon_name);
    subprocess_kill_all_matching("aplay");
    subprocess_kill_all_matching("bt-agent");
    /* usleep(500000) delay TEMPORARILY REMOVED for the same live A/B test
     * as the codec restriction and bluealsa-aplay below -- the stock
     * player's own bluealsa_profile script has zero delay between the
     * kills and respawning bluealsa/bt-agent (both backgrounded with a
     * bare `&`, script just ends), and was just confirmed working
     * flawlessly. */

    char * argv[14];
    int i = 0;
    argv[i++] = (char *) backend.daemon;
    argv[i++] = (char *) "-p";
    argv[i++] = dac_mode_enabled ? (char *) "a2dp-sink" : (char *) "a2dp-source";
    /* Source only. In DAC mode the rate is the phone's choice and the local
     * DAC takes it as-is, so forcing 44.1 there would resample for nothing. */
    if (!dac_mode_enabled && bt_rate_wants_audio_cd()) argv[i++] = (char *) BT_A2DP_FORCE_44K1_ARG;
    if (!dac_mode_enabled && bt_codec_is_sbc_xq())
        argv[i++] = (char *) "--sbc-quality=xq";
    if (dac_mode_enabled) {
        /* aptX and aptX-HD decode reliably and are always offered, on top of
         * BlueALSA's defaults (SBC, AAC). LDAC is the experimental option,
         * and only with the rebuilt decoder. */
        argv[i++] = (char *) "-c";
        argv[i++] = (char *) "aptX";
        argv[i++] = (char *) "-c";
        argv[i++] = (char *) "aptX-HD";
        if (atomic_load(&bt_dac_all_codecs) && bt_ldac_decoder_is_rebuilt()) {
            argv[i++] = (char *) "-c";
            argv[i++] = (char *) "LDAC";
        }
    } else {
        i = bt_codec_append_daemon_args(argv, i);
    }
    argv[i] = NULL;
    if (!spawn_bluealsa_and_verify(argv)) {
        pthread_mutex_unlock(&bt_daemon_respawn_mutex);
        pthread_mutex_unlock(&bt_chip_mutex);
        return false;
    }

    if (dac_mode_enabled) {
        char * agent_argv[] = { (char *) "bt-agent", (char *) "-c", (char *) "NoInputNoOutput", NULL };
        subprocess_spawn_daemon(agent_argv);
    } else {
        char * agent_argv[] = { (char *) "bt-agent", (char *) "-c", (char *) "NoInputNoOutput",
                                 (char *) "-p", (char *) "/usr/data/pin.conf", NULL };
        subprocess_spawn_daemon(agent_argv);
    }

    if (dac_mode_enabled) {
        /* Runs bluealsa-aplay with 200ms buffer (4 periods of 50ms) to ensure
         * exact frame boundaries at both 44.1kHz and 48kHz and reduce latency
         * while buffering against clock drift. */
        char * bluealsa_aplay_argv[] = { (char *) "bluealsa-aplay",
                                          (char *) "--pcm-buffer-time=200000",
                                          (char *) "--pcm-period-time=50000",
                                          NULL };
        subprocess_spawn_daemon(bluealsa_aplay_argv);
    } else {
        char * disc_argv[] = { (char *) "bluetoothctl", (char *) "discoverable", (char *) "off", NULL };
        subprocess_run(disc_argv, NULL, 0);
    }
    if (dac_mode_enabled) bt_dac_info_monitor_start();
    pthread_mutex_unlock(&bt_daemon_respawn_mutex);
    pthread_mutex_unlock(&bt_chip_mutex);
    return true;
}

static void bt_control_reapply_last_output_settings(void) {
    /* Read settings under lock to avoid racing concurrent updates,
     * then call bt_control_apply_output_settings outside the lock. */
    pthread_mutex_lock(&bt_daemon_respawn_mutex);
    bool ever_applied = output_settings_ever_applied;
    bool dac_mode = last_applied_dac_mode_enabled;
    bool volume_sync = last_applied_volume_sync_enabled;
    pthread_mutex_unlock(&bt_daemon_respawn_mutex);

    if (!ever_applied) {
        ensure_bluealsa_running();
        return;
    }
    bt_control_apply_output_settings(dac_mode, volume_sync);
}

bool bt_control_set_codec(const char * codec) {
    if (!codec || (strcmp(codec, "auto") != 0 && strcmp(codec, "ldac") != 0 && strcmp(codec, "ldac_hq") != 0 &&
                   strcmp(codec, "ldac_sq") != 0 && strcmp(codec, "aptx") != 0 &&
                   strcmp(codec, "aac") != 0 && strcmp(codec, "sbc") != 0 &&
                   strcmp(codec, "sbc_xq") != 0)) return false;
    /* BlueALSA 5 receives the preference through its daemon arguments and
     * the bluealsa:CODEC=... PCM name. There is no per-user ALSA config file
     * to generate, and the old ldac_eqmid stanza is not a BlueALSA 5 API. */
    bt_control_restore_codec_preference(codec);
    return true;
}
