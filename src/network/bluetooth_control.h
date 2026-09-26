#ifndef BLUETOOTH_CONTROL_H
#define BLUETOOTH_CONTROL_H

#include <stdbool.h>
#include <stddef.h>

typedef bool (*bt_control_cancel_callback_t)(void * ctx);

typedef struct {
    char mac[18]; /* "XX:XX:XX:XX:XX:XX" + NUL */
    char name[64];
    bool paired;
    bool connected;
} bt_device_t;

/* Negotiated Bluetooth transport/decoded PCM information. This describes
 * BlueALSA's incoming A2DP stream, not the original media file on the
 * phone/PC (which may already have been resampled by the host stack). */
typedef struct {
    bool available;
    bool running;
    char codec[32];
    char pcm_format[32];
    unsigned int sample_rate;
    unsigned int channels;
    unsigned int bit_depth; /* 0 when pcm_format cannot be mapped safely */
} bt_dac_stream_info_t;

/* Non-blocking, thread-safe cached snapshot. */
void bt_control_get_dac_stream_info(bt_dac_stream_info_t * out);

/* bluetoothd, BlueALSA 5, and a NoInputNoOutput pairing agent are already
 * running on this device; everything here just drives bluetoothctl's
 * non-interactive CLI mode (`bluetoothctl <command> [args]`), which
 * produces clean output with no ANSI/prompt junk to strip. */

bool bt_control_is_powered(void);

/* Brings up the Bluetooth chip if it isn't already active (no hci0) by
 * running /usr/bin/bt_resume. No-op if hci0 already exists.
 * Call off the UI thread as this takes several seconds. */
bool bt_control_init_chip(void);
/* Reconcile the persisted outgoing codec with any daemon started by the
 * stock boot scripts. Call from a worker after Bluetooth startup readiness. */
bool bt_control_reconcile_source_settings(void);

/* Each blocks for about a second (bluetoothctl's own controller-power round
 * trip) -- call off the UI thread, same as everything else here. */
void bt_control_enable(void);
void bt_control_disable(void);

/* True if any paired device currently has an active connection. Cheap
 * relative to bt_control_scan() (no discovery window): just `info` on each
 * already-paired device, same query bt_control_scan() already does per
 * device, without the scan step. Still forks one process per paired device,
 * so callers should poll this at a throttled cadence, not every frame. */
bool bt_control_is_connected(void);

/* Like bt_control_is_connected(), but avoids the per-paired-device fork
 * loop when the bluetoothctl on this system supports filtering `devices` by
 * property (the same capability bt_control_list_paired_states() already
 * detects and caches for the `Paired` filter) -- a single
 * `bluetoothctl devices Connected` call replaces querying every paired
 * device individually on that path. Falls back to bt_control_is_connected()
 * only when that capability is not (yet) known to be available; a failed
 * `devices Connected` call on a system already known to support it does
 * NOT fall back, to avoid reintroducing the O(N) fork cost on exactly the
 * transient failures most likely during Bluetooth power-on -- see the .c
 * file's own comment. Returns 1/0/-1 (connected / not connected / could not
 * determine this cycle), not a bool -- callers should keep their last known
 * state on -1 rather than treat it as "nothing connected".
 *
 * Note the fast path answers a very slightly different question than the
 * per-device path: `devices Connected` reports any currently-connected
 * device, not only paired ones, so it can briefly disagree with
 * bt_control_is_connected() during an in-progress pairing handshake. Given
 * this is only used for the topbar/quick-drawer "is Bluetooth connected to
 * something" icon (not anything pairing-sensitive), that's an accepted
 * tradeoff, not a bug. This is what that icon refresh should poll instead of
 * bt_control_is_connected() -- see the .c file's own comment on why the
 * naive per-device version noticeably competes with the UI thread for CPU
 * on systems with several paired devices. */
int bt_control_any_paired_connected(void);

/* Like bt_control_is_connected(), but returns the full per-device
 * paired/connected breakdown instead of collapsing it into a single bool --
 * see the .c file for why (avoids paying for the same per-device query loop
 * twice per poll cycle). Returns how many entries were written into out[]
 * (capped at max_count, same convention as bt_control_scan()), or -1 if the
 * underlying query itself failed -- see the .c file's own comment on why
 * that's kept distinct from a genuine empty list. Blocking; call off the
 * UI thread. */
int bt_control_list_paired_states(bt_device_t * out, int max_count);

/* True if a real A2DP-source PCM (this device -> a connected headphone/
 * speaker) is currently registered with bluealsa -- a stronger, audio-
 * specific signal than bt_control_is_connected()/bt_control_list_paired_states()
 * (those report ANY paired device with an active connection, not
 * necessarily one that actually supports/negotiated A2DP audio). Forks a
 * process (`bluealsactl list-pcms`); call off the UI thread. */
bool bt_control_is_a2dp_source_connected(void);

/* Writes the connected A2DP-source accessory's Bluetooth MAC address (e.g.
 * "DC:69:E2:99:43:06") into out, extracted from the same bluealsa PCM path
 * bt_control_is_a2dp_source_connected() already checks for
 * ("/org/bluealsa/hci0/dev_XX_XX_XX_XX_XX_XX/a2dpsrc/sink" -- underscores
 * swapped back to colons). Returns false (out left untouched) if nothing's
 * connected. Same subprocess cost as the other bt_control_* calls here;
 * call off the UI thread. */
bool bt_control_get_connected_device_mac(char * out, size_t out_size);

/* Writes the ACTUAL negotiated A2DP codec the connected accessory is
 * currently streaming with (e.g. "AAC", "LDAC", "SBC" -- whatever
 * `bluealsactl info` reports as "Selected codec") into out. This is the
 * real, live-negotiated codec, NOT current_settings.bt_codec (this app's
 * own PREFERRED codec setting, Settings > Bluetooth > Codec) -- those can
 * differ if the accessory doesn't support the preferred one and bluealsa
 * fell back to something else. Same subprocess cost as the other
 * bt_control_* calls here; call off the UI thread. Returns false (out left
 * untouched) if nothing's connected. */
bool bt_control_get_connected_device_codec(char * out, size_t out_size);

/* As above, and also the negotiated sampling frequency in Hz -- the codec is
 * only half of what tells you which link you actually got. Pass NULL for
 * out_sample_rate to skip it. Same subprocess cost; call off the UI thread. */
bool bt_control_get_connected_device_stream(char * out, size_t out_size, unsigned int * out_sample_rate);

/* Blocking: scans for `seconds` (bluetoothctl's own --timeout), then reads
 * back the combined paired+discovered device list via `info` on each one
 * for Paired/Connected state. Call off the UI thread. Returns how many
 * devices were written into out[] (capped at max_count). */
/* Known devices only, no inquiry: safe to call while audio is streaming. */
int bt_control_list_devices(bt_device_t * out, int max_count);

int bt_control_scan(int seconds, bt_device_t * out, int max_count);

/* Pair (if not already) + trust + connect, in that order -- trusting first
 * means a future reconnect (e.g. after the device is turned off and back
 * on) won't need to go through this flow again, matching normal phone/OS
 * Bluetooth UX. The already-running NoInputNoOutput agent auto-accepts
 * "Just Works" pairing with no PIN prompt needed from this app. Blocking;
 * call off the UI thread. */
bool bt_control_connect(const char * mac);

bool bt_control_disconnect(const char * mac);

/* Unpairs and removes the saved device entry (bluetoothctl remove) --
 * disconnects it first if currently connected, as part of the same
 * command. Blocking; call off the UI thread. */
bool bt_control_forget(const char * mac);

/* Bounded, cancellation-aware reconnect of an already-paired trusted A2DP
 * headset. Never pairs or trusts. Call only from the reconnect worker; all
 * Bluetooth chip/control operations are serialized with manual actions. */
bool bt_control_reconnect_paired(const char * preferred_mac,
                                bt_control_cancel_callback_t cancel_cb,
                                void * cancel_ctx);

/* Output settings use the firmware's BlueALSA 5 service, with an
 * a2dp-source profile for this device sending audio OUT to
 * headphones/speakers. Changing either setting kills and relaunches that
 * service with the full combination implied by both current values (callers
 * pass both, not just the one that changed). This is a real running-service
 * restart, briefly interrupting in-progress Bluetooth audio; blocking, call
 * off the UI thread.
 *
 * dac_mode_enabled adds the a2dp-sink profile (so another device can
 * stream audio TO this one, using it as an external DAC) and, when
 * turning it on, also starts bluealsa-aplay to actually route that
 * incoming audio to the hardware output and makes the adapter
 * discoverable+pairable so a phone can find and connect to it as a sink
 * target; turning it off stops bluealsa-aplay and discoverability again.
 * volume_sync_enabled selects whether the app mirrors volume through the
 * BlueALSA 5 control API. */
bool bt_control_apply_output_settings(bool dac_mode_enabled, bool volume_sync_enabled);

/* Select the outgoing BlueALSA 5 encoder preference. `codec` is one of
 * "auto"/"ldac_hq"/"ldac_sq"/"aptx"/"aac"/"sbc"/"sbc_xq". The active
 * daemon is reconciled separately by bt_control_reconcile_source_settings().
 * This only updates the in-memory preference; settings persistence belongs to
 * the caller. */
bool bt_control_set_codec(const char * codec);

/* Restore the saved outgoing encoder preference without doing I/O. */
void bt_control_restore_codec_preference(const char * codec);

/* Selects the speex resampler for Bluetooth output instead of alsa-lib's
 * built-in linear one. On by default; roughly a point of CPU more. */
void bt_control_set_speexrate_enabled(bool enabled);
/* Experimental: also offer LDAC in DAC mode (aptX and aptX-HD always are).
 * No effect on firmware without the rebuilt decoder, whose stock one
 * crashes. */
void bt_control_set_dac_all_codecs(bool enabled);

/* Requested A2DP transport rate in Hz, or 0 for BlueALSA's own choice
 * (highest up to 48 kHz). 44100 is applied as a daemon argument, so it needs
 * a Bluetooth off/on cycle; any other rate is selected per connection. */
void bt_control_set_sample_rate(unsigned int rate);
/* Same, for a rate the user just chose: also forgets a rate the connected
 * accessory refused before, which bt_control_set_sample_rate() keeps so a
 * reconnect does not retry it. */
void bt_control_choose_sample_rate(unsigned int rate);

/* Cycles the connected accessory's link so a changed transport rate is
 * negotiated. The radio stays on; only the device link drops. Blocks for
 * several seconds, so call it from a worker thread. False when nothing is
 * connected, in which case the new rate applies on the next connection. */
bool bt_control_reconnect_for_rate_change(unsigned int rate);

/* Rates the connected accessory supports for the codec currently in use,
 * newest query each call. Returns the count written, 0 when nothing is
 * connected or the rates could not be read. */
int bt_control_get_available_rates(unsigned int * out, int max_count);
/* ALSA PCM used for outgoing Bluetooth audio; an explicit preference selects
 * its BlueALSA CODEC parameter, while auto leaves negotiation unchanged. */
const char * bt_control_get_playback_pcm(void);

/* Prepares an ALSA PCM pinned to the rate the A2DP transport already
 * negotiated and reports its name, so playback never asks BlueALSA to change
 * the transport's rate (which recreates it and drops the accessory). False
 * when the transport cannot be read; open bt_control_get_playback_pcm()
 * directly in that case. */
bool bt_control_prepare_playback_pcm(char * out, size_t out_size);

/* Keeps this app's own playback volume and a connected a2dp-source
 * accessory's (headphones/speaker this device streams TO) AVRCP volume in
 * sync, in both directions -- the headphones' own volume buttons update
 * this app's volume, and this app's own volume slider/hardware buttons
 * update whatever level the headphones show. See the .c file's own comment
 * above bt_control_source_volume_sync_start() for why this doesn't
 * introduce a second, compounding gain stage. Call start whenever
 * Bluetooth output is actually in use (mirror audio_set_bt_output()'s own
 * gating -- gui.c calls both together) and stop when it isn't; both are
 * idempotent. Serialize start/stop calls on one lifecycle worker; stop can
 * wait for child cleanup and must not run on the LVGL thread. */
void bt_control_source_volume_sync_start(void);
void bt_control_source_volume_sync_stop(void);
/* Nonblocking status snapshots, safe from the UI thread. */
bool bt_control_source_volume_sync_is_running(void);
bool bt_control_source_volume_sync_consume_percent(int * out_percent);

/* Fast disconnect detection for a2dp-source output PCM via `bluealsactl monitor`.
 * Confirms absence after a short grace interval: sample-rate/codec changes can
 * remove and re-create the same PCM without disconnecting the headphones.
 *
 * Same start/stop lifecycle convention as bt_control_source_volume_sync_start()/
 * _stop() just above -- start whenever Bluetooth output is actually in use,
 * stop when it isn't, both idempotent. bt_control_output_disconnect_consume()
 * is edge-triggered: true (and clears itself) the first poll after a real
 * removal remained absent through the grace interval, false otherwise.
 * A matching addition cancels a pending or unconsumed removal. EOF does not
 * confirm absence; periodic polling remains the fallback for a lost monitor.
 * The consumer performs no subprocess calls or waits for this grace period. */
#define BT_OUTPUT_RECONFIGURE_GRACE_MS 750
void bt_control_output_disconnect_watch_start(void);
void bt_control_output_disconnect_watch_stop(void);
bool bt_control_output_disconnect_watch_is_running(void);
bool bt_control_output_disconnect_consume(void);

#endif /* BLUETOOTH_CONTROL_H */
