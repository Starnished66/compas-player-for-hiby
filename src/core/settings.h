#ifndef SETTINGS_H
#define SETTINGS_H

#include <stdbool.h>
#include <stdint.h>

/* Discrete screen-timeout presets. The original useful coarse progression
 * is preserved, with a new 15-second choice added at the beginning. */
extern const int SCREEN_TIMEOUT_STEPS[];
#define SCREEN_TIMEOUT_STEP_COUNT 7

extern const int SCREEN_DIM_DELAY_STEPS[];
#define SCREEN_DIM_DELAY_STEP_COUNT 7

/* Idle-shutdown choices, in minutes -- same discrete-steps reasoning as
 * SCREEN_TIMEOUT_STEPS above. This is a full poweroff (see idle_shutdown.h),
 * not a sleep, so the steps skew longer than the screen timeout. */
extern const int IDLE_SHUTDOWN_STEPS[];
#define IDLE_SHUTDOWN_STEP_COUNT 5

/* Sleep-timer choices, in minutes -- same discrete-steps reasoning as
 * SCREEN_TIMEOUT_STEPS above. This only persists which duration the quick-
 * drawer sleep icon (gui.c) arms next time it's tapped -- whether the timer
 * is currently counting down is live, session-only state, not saved here. */
extern const int SLEEP_TIMER_STEPS[];
#define SLEEP_TIMER_STEP_COUNT 10
#define SETTINGS_SUBSONIC_SAVED_MAX 16
#define REMOTE_CONTROL_PIN_MAX_LENGTH 12

#define BT_DEVICE_RATE_MAX 8

typedef struct {
    float volume;              /* 0.0 - 1.0 */
    char last_track[512];      /* absolute path, empty if none */
    double last_position;      /* seconds into last_track */
    int last_source_kind;      /* 0 = unknown/filesystem, 1 = All Songs, 2 = album */
    char last_source_name[128];/* album title when last_source_kind == 2 */

    /* Settings -> Playback -> Resume Last Track.
     * 0 = off (default), 1 = resume and start playing on boot,
     * 2 = load last track and seek to last position but stay paused.
     * Skips tracks in SUBSONIC_STREAM_CACHE_DIR.
     * Car Mode has its own separate, always-on, headphone-gated resume. */
    int resume_mode;

    /* Settings -> Playback -> Play/Pause Button. What the physical
     * play/pause button does: 0 = Play/Pause (default, matches the
     * button's own label), 1 = Previous Track, 2 = Play/Pause on a single
     * press, but Previous Track if a second press follows within
     * PLAY_PAUSE_DOUBLE_CLICK_MS (gui.c) -- lets one physical button cover
     * both without a dedicated previous-track button. */
    int play_pause_button_mode;

    uint32_t accent_color;     /* packed 0xRRGGBB, applied to sliders/switches app-wide */
    bool crossfade_enabled;    /* if true, fade into the next queued track near the current one's end */

    /* Default true -- gapless is this pipeline's normal behavior, not an
     * opt-in. Turning it off makes arm_next_track_for_audio() (gui_player.c)
     * stop arming the next track, so the playback thread reaches a true EOF
     * and the track finishes before the next one is opened, reopening the
     * output device between them. That is the same path a queue already
     * takes at its end, so nothing in audio.c changes.
     *
     * Crossfade needs an armed next track, so it cannot work while this is
     * off; turning this off turns crossfade off too, the same mutual
     * exclusion the DAC-mode toggles use. */
    bool gapless_enabled;

    /* Settings -> Playback -> ReplayGain. 0 = Off (no gain applied), 1 = Per
     * Track (default -- normalizes every track to the same perceived
     * loudness), 2 = Per Album (preserves intentional relative loudness
     * differences between tracks on the same album, falling back to track
     * gain for any track whose file has no album-level tag -- see gui.c's
     * own resolve_replaygain()). Replaced the old plain on/off replaygain_
     * enabled bool -- see settings.c's load path for the migration from
     * that field's old on-disk key. */
    int replaygain_mode;

    /* Car Mode: unplugging power while something is loaded checkpoints position
     * and powers the device off; plugging power back in powers it back on and
     * resumes automatically. Off by default. */
    bool car_mode_enabled;

    /* In-line remote: Enables the use of Volume +/- buttons on the headphones,
     * along with single tapping the play/pause button to play/pause,
     * double tapping to skip to the next track, or triple tapping to go back
     * to the previous track. On by default. */
    bool inline_remote_enabled;

    /* Lyrics: enable the player's lyrics icon/view. On by default. */
    bool lyrics_enabled;

    /* Subsonic-compatible (Subsonic/Navidrome/Airsonic/...) server config.
     * Stored in plaintext like every other setting here -- this project has
     * no secure-storage mechanism (keychain, encrypted-at-rest file, etc.)
     * on either host or target, so pretending otherwise would be dishonest;
     * a personal device's own settings file is the same trust boundary as
     * the passwords already sitting in most people's browser profiles. */
    char subsonic_url[256];      /* e.g. "https://music.example.com:4040", no trailing slash */
    char subsonic_username[128];
    char subsonic_password[128];
    bool subsonic_verify_tls;    /* false = accept self-signed certs for this server (opt-in, see http_client.h) */

    /* Saved Subsonic server profiles (the "Saved Servers" list). Tagcache
     * cannot store these. The live list is the /usr/data sidecar
     * (subsonic_saved_servers.c, via metadata_db.h); this array is a
     * mirror loaded/saved with the rest of credentials so they survive an
     * unmounted SD card. url is the unique key -- saving the same URL
     * again replaces credentials rather than duplicating. */
    struct {
        char url[256];
        char username[128];
        char password[128];
        bool verify_tls;
    } subsonic_saved[SETTINGS_SUBSONIC_SAVED_MAX];
    int subsonic_saved_count;

    /* Bluetooth output settings -- see bluetooth_control.h for the BlueALSA
     * 5 service behavior. */
    bool bt_volume_sync_enabled; /* Mirror hardware volume changes to the paired device. */
    /* a2dp-sink profile: lets another device stream audio TO this one.
     * Runtime-only, never persisted: the single-profile daemon cannot serve
     * a2dp-source at the same time, so restoring this at boot would leave
     * headphones unable to connect with no DAC overlay on screen to exit
     * from. Leaving the overlay already clears it, so a stored "true" only
     * ever came from powering off while the overlay was open. */
    bool bt_dac_mode_enabled;
    char bt_codec[16];           /* "auto"/"ldac_hq"/"ldac_sq"/"aptx"/"aac"/"sbc"/"sbc_xq" */
    /* Resample Bluetooth output with the speex converter instead of
     * alsa-lib's built-in linear one. A track whose rate differs from the
     * A2DP transport's has to be converted either way, and speex measured
     * about a point of CPU more than linear on an R1 while sounding better,
     * so it is on by default and exposed under Bluetooth > Advanced only so
     * it can be turned off. Quality 5 costs far more; quality 10 cannot keep
     * up at all. */
    bool bt_speexrate_enabled;
    /* A2DP transport rate in Hz, or 0 for automatic. Automatic is the
     * default and means 44.1 kHz: resampling only happens when a track's
     * rate differs from the transport's, and CD-derived 44.1 kHz material is
     * the bulk of a typical music library. 44.1 is negotiated directly via a
     * daemon argument; any other rate needs the accessory's link
     * re-established, because A2DP fixes the rate when the configuration is
     * negotiated. */
    unsigned int bt_sample_rate;
    /* Per-accessory overrides of the rate above, because the right rate is a
     * property of the accessory: 44.1 kHz suits headphones fed CD-derived
     * music, while an LDAC device may be worth 96 kHz. Keyed by MAC, oldest
     * entry reused once full. */
    struct {
        char mac[18];
        unsigned int rate;
    } bt_device_rates[BT_DEVICE_RATE_MAX];
    int bt_device_rate_count;
    char bt_last_output_mac[18]; /* verified A2DP source MAC, empty when none is remembered */
    /* When true, BLE devices without a broadcast name are hidden from the
     * "Available Devices" list (shown as raw MAC addresses otherwise).
     * Paired devices are always shown regardless of this setting. */
    bool bt_hide_unnamed_devices;

    /* AirPlay receive mode -- see airplay_control.h. Mutually exclusive with
     * bt_dac_mode_enabled (both would fight over the same physical audio
     * hardware), enforced in gui.c wherever either gets toggled on. */
    bool wifi_dac_mode_enabled;

    /* DLNA/UPnP-AV MediaRenderer receive mode -- see dlna_control.h. Not
     * mutually exclusive with bt_dac_mode_enabled/wifi_dac_mode_enabled
     * the way those two are with each other: unlike them, this doesn't
     * pipe raw external audio straight to the output hardware, it just
     * downloads a cast track and plays it through the normal local
     * decoder/playback pipeline, same as any other track. */
    bool dlna_renderer_enabled;

    /* Phone remote-control over Wi-Fi HTTP and paired Bluetooth RFCOMM -- see
     * remote_control.h. Session-only: startup clears a previously saved true
     * value. Default false; users explicitly choose when either transport
     * can control playback and browse the library. A shared PIN protects the API. */
    bool remote_control_enabled;
    /* Shared PIN required by phone clients after Remote Control is enabled.
     * Empty means the service should generate and persist one on first use.
     * Stored in the regular plaintext settings file like other credentials. */
    char remote_control_pin[REMOTE_CONTROL_PIN_MAX_LENGTH + 1];

    /* Auto screen-timeout (gui.c's update_timer_cb, backed by
     * backlight_set_screen_on()) -- screen_timeout_enabled=false means never
     * auto-off, screen stays on until the power button is pressed.
     * screen_timeout_seconds is meaningful only when enabled, always one of
     * SCREEN_TIMEOUT_STEPS above (see settings_load(), which snaps any
     * hand-edited or legacy value to the nearest step, since the settings
     * file is plaintext and could be hand-edited out of range). */
    bool screen_timeout_enabled;
    int screen_timeout_seconds;

    /* Pre-timeout low-brightness stage. When enabled, gui.c dims an idle
     * screen to BACKLIGHT_MIN_PERCENT before the final timeout. */
    bool screen_dimming_enabled;
    int screen_dim_delay_seconds;

    /* Hide the global status bar (and, on the Player specifically, the
     * standalone back arrow) while the Player or its fullscreen Lyrics
     * view is active. Other screens retain their status information. */
    bool hide_player_topbar;

    /* Charge-status LEDs (/sys/class/leds/{red,blue}, see led_control.h) --
     * false forces both off regardless of charge state, for e.g. leaving the
     * device charging overnight in a dark room. */
    bool led_indicator_enabled;

    /* Settings -> About -> Developer Options -> "Enable debug logging" --
     * writes a detailed, timestamped log of library database scans and
     * album art cache jobs (including lazy load) to .logs/database_artwork.log
     * on the SD card (see db_log.h), and also gates the USB DAC bridge's own
     * diagnostic log to .logs/usb_dac_bridge.log (see usb_dac_bridge.h). */
    bool db_logging_enabled;
    /* Power + Volume Down takes a screenshot. Off by default: the combination
     * is easy to hit by accident in a pocket, and nobody who does not know it
     * exists should find the card filling with images. */
    bool screenshot_combo_enabled;
    /* Developer Options, experimental, off by default. dev_bt_dac_all_codecs
     * (key kept from when it also covered aptX) adds LDAC to the Bluetooth
     * DAC-mode sink, only with the rebuilt decoder, as the stock firmware
     * one crashes; aptX and aptX-HD are always offered. dev_covers_during_playback lets the cover
     * warmer keep reading already generated thumbnails while audio plays;
     * extracting new covers still waits for playback to stop. */
    bool dev_bt_dac_all_codecs;
    bool dev_covers_during_playback;

    /* Caps the PMIC's charge-termination voltage to 4.2V to extend battery
     * longevity, rather than a literal state-of-charge cutoff -- see
     * charge_limiter.h for how this is actually enforced. */
    bool charge_limiter_enabled;

    /* Caps the AXP2101 constant-current charging phase at 500mA. Turning
     * this off deliberately leaves the PMIC's current setting unchanged. */
    bool safe_charging_enabled;

    /* Topbar battery percentage digits next to the battery icon -- the icon
     * itself (charge outline + fill gauge) is always shown regardless of
     * this, only the "NN%" readout is optional. When off, gui.c's
     * sync_topbar_status_icon_positions() lets the Wi-Fi/Bluetooth status
     * icons slide into the space the percentage would otherwise occupy,
     * right next to the battery icon, instead of leaving it empty. On by
     * default, matching the always-on behavior every previous version of
     * this app had. */
    bool show_battery_percent;

    /* Idle action after a long stretch idle (screen off, not playing, not
     * charging) -- either power_suspend_now() (suspend-to-RAM, see
     * idle_suspend_enabled below) or a full poweroff (idle_shutdown.h; the
     * same thing the stock firmware's own "Idle shutdown" setting does,
     * confirmed via strings on the stock binary: /sbin/poweroff,
     * power_save_shutdown_timer). On by default (10 minutes): real-world
     * feedback found users who never open Settings at all reporting
     * overnight battery drain, since a device left screen-off otherwise
     * just sits at full power indefinitely with nothing opted in. Existing
     * installs' saved settings are untouched by this default -- only a
     * fresh settings.txt (settings_load() finding none, see set_defaults())
     * picks it up. */
    bool idle_shutdown_enabled;
    int idle_shutdown_minutes;

    /* When true, the idle action above is power_suspend_now() (quick
     * resume) instead of idle_shutdown_now() (full poweroff) -- see
     * power_suspend.h for how real suspend was made to actually work on
     * this hardware. Only meaningful when idle_shutdown_enabled is true. On
     * by default alongside it: a full poweroff would cost the in-progress
     * queue/position and mean a real boot on every wake, exactly the "why
     * did my music reset" complaint a battery-drain fix shouldn't trade
     * for. */
    bool idle_suspend_enabled;

    /* Internal bookkeeping, not exposed in any UI: whether this install has
     * already gone through settings_load()'s one-time forced migration of
     * the three fields above to their new (on) defaults. Lets an existing
     * settings.txt saved by a version predating that default flip -- which
     * would otherwise keep pinning idle_shutdown_enabled/idle_suspend_enabled/
     * idle_shutdown_minutes back to their old off/off/30 values forever,
     * since settings_save() always writes every field -- get force-migrated
     * exactly once, the same way a brand new install already gets the new
     * defaults from set_defaults(). A user who deliberately turns any of
     * the three back off afterward is not touched again. */
    bool idle_suspend_default_migrated;

    /* USB gadget mode (Storage/USB DAC/ADB): see usb_mode_control.h for
     * usb_mode_t and how each is actually applied. Plain int here (values
     * matching usb_mode_t), not the enum itself, so this header does not
     * need to depend on usb_mode_control.h, same reasoning as bt_codec
     * above being a plain string rather than pulling in an enum from
     * bluetooth_control.h.
     *
     * Startup behavior: if the user enabled ADB, it persists and is
     * re-applied on boot because it lives behind Developer Options as an
     * explicit opt-in. Any other mode (Storage, USB DAC) resets to Storage
     * on boot to prevent unexpected USB configurations or blocking local
     * playback. */
    int usb_mode;

    /* Player queue play mode -- Sequential/Repeat All/Repeat One/Shuffle.
     * Plain int (values matching gui.c's own play_mode_t), same reasoning as
     * usb_mode above: this header doesn't need to depend on gui.c's own
     * playback-orchestration types for a value that's purely persisted user
     * preference. */
    int play_mode;

    /* Swipe up (anywhere on a screen) jumps straight back to Home, the same
     * destination as repeatedly swipe-right-to-go-back all the way out --
     * see screen_gesture_event_cb() in gui.c. A new, less-standard gesture
     * alongside the existing swipe-right (back one level) and swipe-left
     * (jump to Now Playing), opt-out rather than opt-in since it's additive
     * (doesn't remove or reassign either existing gesture) and doesn't
     * fire on a false positive the way a diagonal swipe or an accidental
     * two-finger touch could. */
    bool swipe_up_home_enabled;

    /* Matches a stock-firmware setting: launch at a fixed volume every time
     * rather than wherever the slider was last left. Default matches
     * stock's own default (fixed, 20%) -- disable to fall back to this
     * app's original behavior of resuming at last-used volume. Only
     * startup_volume_fixed_percent is meaningful when
     * startup_volume_fixed_enabled is true; the plain `volume` field above
     * still tracks and persists the live slider position either way, so
     * turning this off later resumes from wherever it was last left, not
     * from stale pre-feature state. */
    bool startup_volume_fixed_enabled;
    int startup_volume_fixed_percent;

    /* Duration the quick-drawer sleep icon arms next time it's tapped --
     * see gui.c's quick_drawer_sleep_event_cb()/poll_sleep_timer(). Always
     * one of SLEEP_TIMER_STEPS (settings_load() snaps any hand-edited or
     * legacy value to the nearest step, same reasoning as
     * screen_timeout_seconds). Whether the timer is currently counting down
     * is live, session-only state, not saved here. */
    int sleep_timer_minutes;

    /* IANA time zone id (e.g. "America/New_York"), applied via
     * timezone_apply() (timezone_apply.h) both at startup and whenever
     * changed from the Time Zone picker (gui.c). Empty string means "not
     * set" -- this app has never applied anything, so the system falls
     * back to whatever (if anything) /usr/data/localtime already points
     * at, which is nothing by default on this hardware (confirmed live:
     * /etc/localtime is a symlink to a target that doesn't exist out of
     * the box), so time displays in UTC until a zone is chosen. */
    char timezone[64];

    /* Settings -> System -> Hostname. Empty means "use the stock device
     * name" (/usr/resource/hostname). When non-empty, applied at boot by
     * hostname_apply(). Requires a reboot to take effect. */
    char hostname[64];

    /* UI text size (Settings -> Display -> Font Size): 0 = Small, 1 =
     * Medium, 2 = "BlindMF" (largest). Applied at startup and live
     * via fallback_font_apply_size_tier(). */
    int font_size_tier;

    /* Settings -> Lyrics Text Size: independent text size control for the
     * fullscreen synchronized lyrics view (1 = Medium, 2 = Large). Defaults to 2 (Large). */
    int lyrics_font_size_tier;

    /* Screen brightness (0-100), applied at startup and updated when adjusted.
     * Defaults to 80. */
    int brightness_percent;

    /* Settings -> System -> "24-Hour Clock". Controls the topbar clock format:
     * true renders 24-hour "%H:%M", false renders 12-hour "%I:%M" with an AM/PM sprite. */
    bool clock_24h;
    bool clock_automatic;
    int64_t clock_manual_epoch;
    int64_t clock_system_reference;

    /* Settings -> Display -> Font. Filename of the user-selected custom Latin
     * TTF font from <SD>/Fonts (e.g. "Roboto-Regular.ttf"). Empty string means
     * built-in Montserrat default. */
    char custom_font[64];
} player_settings_t;

/* Loads settings from disk into *out. If the settings file doesn't exist or
 * can't be parsed, *out is populated with sensible defaults (volume 1.0, no
 * last track, auto-resume on) and false is returned. */
bool settings_load(player_settings_t * out);

/* Writes *settings to disk, overwriting any existing file. Writes to a
 * temporary file and renames it into place, so a crash or power loss
 * mid-write can't corrupt the settings file. */
void settings_save(const player_settings_t * settings);

/* Bluetooth transport rate remembered per accessory; falls back to
 * bt_sample_rate when that accessory has no entry of its own. */
unsigned int settings_bt_rate_for(const player_settings_t * settings, const char * mac);
void settings_bt_set_rate_for(player_settings_t * settings, const char * mac, unsigned int rate);

/* Queue a durable save without blocking the caller on filesystem syncs.
 * Rapid requests are coalesced to the newest complete snapshot. */
void settings_save_async(const player_settings_t * settings);

/* Upserts a Saved Servers profile by URL. Does not write disk -- call
 * settings_save() after, same as every other settings mutation. */
void settings_subsonic_server_upsert(player_settings_t * settings, const char * url, const char * username,
                                      const char * password, bool verify_tls);

/* Settings > System > Factory Reset: wipes all configuration files and data
 * in /usr/data except the "mnt" directory (the SD card mount point), then reboots
 * into default settings. */
void settings_factory_reset(void);


extern player_settings_t current_settings;
#endif /* SETTINGS_H */
