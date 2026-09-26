#include "gui.h"
#include "app_clock.h"
#include "gui_library.h"
#include "gui_lock_screen.h"
#include "gui_queue.h"
#include "gui_player.h"
#include "gui_plugins.h"
#include "gui_shell.h"
#include "gui_navigation.h"

#define PLAYLISTS_DIR MUSIC_ROOT_DIR "/Playlists"
#define SUBSONIC_STREAM_CACHE_DIR "/tmp/subsonic_stream_cache"
#include "gui_theme.h"
#include "gui_notifications.h"
#include "gui_settings.h"
#include "gui_network.h"
#include "gui_books.h"
#include "gui_text_input.h"
#include "gui_lyrics.h"
#include "gui_track_info.h"
#include "gui_subsonic.h"
#include "assets.h"
#include "backlight.h"
#include "debug_log.h"
#include "import_web.h"
#include "airplay_control.h"
#include "dlna_control.h"
#include "remote_control.h"
#include "battery.h"
#include "wifi_status.h"
#include "audio.h"
#include "audio_output.h"
#include "file_browser.h"
#include "hw_buttons.h"
#include "metadata.h"
#include "metadata_db.h"
#include "peq.h"
#include "screen_builders.h"
#include "settings.h"
#include "db_log.h"
#include "subsonic_client.h"
#include "http_client.h"
#include "cover_decode.h"
#include "albumart.h"
#include "lyrics.h"
#include "lyrics_layout.h"
#include "remote_track.h"
#include "transition_compositor.h"
#include "wifi_control.h"
#include "bluetooth_control.h"
#include "hiby_sys_server.h"
#ifndef HOST_BUILD
#include "bt_media_player.h"
#endif
#include "headphone_status.h"
#include "plugin_manager.h"
#include "gui_plugin_manage.h"
#include "led_control.h"
#include "charge_limiter.h"
#include "idle_shutdown.h"
#include "power_suspend.h"
#include "device_config.h"
#include "usb_mode_control.h"
#include "usb_dac_bridge.h"
#include "firmware_update.h"
#include "playlist_files.h"
#include "cue_parser.h"
#include "subprocess.h"
#include "timezone_data.h"
#include "timezone_apply.h"
#include "hostname_apply.h"
#include "../core/screenshot.h"

/* --- Theme API (gui_theme.h) --- */



/* --- Notification/Modal API (gui_notifications.h) --- */


gui_busy_handle_t gui_busy_show(const char * title, const char * msg);
void gui_busy_set_progress(gui_busy_handle_t handle, int percent);
void gui_busy_hide(gui_busy_handle_t handle);

gui_busy_handle_t wifi_connect_token = 0;
gui_busy_handle_t wifi_connect_saved_token = 0;
gui_busy_handle_t import_web_stop_token = 0;

#include "src/core/lv_obj.h"
#include "src/core/lv_obj_pos.h"
#include "src/core/lv_obj_style.h"
#include "src/core/lv_obj_style_gen.h"
#include "src/layouts/flex/lv_flex.h"
#include "src/misc/lv_area.h"
#include "src/draw/lv_draw_buf.h"
#include "src/draw/snapshot/lv_snapshot.h"
#include "src/core/lv_refr.h"
#include "src/widgets/image/lv_image.h"
#include "src/drivers/display/fb/lv_linux_fbdev.h"
#include <ctype.h>
#include <dirent.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifdef UI_PERF_TRACE
uint64_t ui_perf_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000ULL + (uint64_t) ts.tv_nsec / 1000ULL;
}
#endif

#ifdef HOST_BUILD
  #define MUSIC_ROOT_DIR "./music"
#else
  /* SD card mount point, per the layout the stock hiby_player uses. */
  #define MUSIC_ROOT_DIR "/data/mnt/sd_0"

  /* main.c's own boot-checkpoint logger (see its own comment there) --
   * gui_init() below calls it directly since the still-unresolved cold-boot
   * hang has moved further into startup than main.c alone can see. Remove
   * alongside main.c's own copy once cold boot is confirmed working. */
  extern void boot_checkpoint(const char * step);

  /* main.c's own best-effort `mount -t vfat .../sd_0` retry (see its own
   * comment on why this needs to exist at all -- no hotplug mechanism for
   * the internal SD card slot exists anywhere on this firmware). Called
   * once at boot from main.c already; poll_sd_card_hotplug() below calls
   * it again periodically so re-inserting the card after boot eventually
   * gets mounted too, not just the very first insertion main.c catches. */
  extern void mount_sd_card_if_needed(void);
#endif

/* Books are deliberately isolated from the music-library root. A recursive
 * .txt search across an entire large SD card is both unexpectedly broad and
 * slow; only this dedicated directory participates in book discovery. */
#define BOOKS_ROOT_DIR MUSIC_ROOT_DIR "/Books"
#define AUDIOBOOKS_LIBRARY_DIR_NAME "Audiobooks"

/* Music browsing benefits from roomier touch targets and artwork, while
 * Settings and the rest of the app retain the denser shared 84px rows. */

/* General UI text uses fallback_font.h's stable app_font_* handles.  Their
 * descriptors are rebuilt transactionally for live Font Size changes, so
 * every existing LVGL style keeps the same pointer while its metrics and
 * multilingual chain change together. */

lv_obj_t * stream_media_screen;

void sync_player_topbar_visibility(lv_obj_t * screen);







/* The 480x320 panel behind the transport controls (title/artist/progress/
 * time/controls_row) -- built in build_player_screen(), but also targeted
 * by poll_cover_decode()/compute_reflection_bytes() below to swap its background
 * between the plain static buttom.png (no embedded art to reflect) and a
 * freshly generated per-track reflection, hence file-scope rather than a
 * local inside build_player_screen(). */
bool favorite_is_set = false;
/* Clock, top bar center: topbar/N.png digit and topbar/colon.png sprite widgets
 * in a fixed HH:MM layout with an optional AM/PM indicator when 12-hour mode is active. */
/* Volume readout, left edge of status bar: speaker icon, up to 3 digit sprites,
 * and headphone icon (shown when headphone jack is connected). Leading unused digit
 * slots are hidden to collapse spacing. */

/* Loud-volume warning color threshold, read once at startup from the stock
 * firmware's own /usr/resource/config.json (see device_config.h) -- e.g. a
 * real R1 Pro had this set to 40 via the stock Settings screen. -1 means
 * "feature disabled" (file missing/host build, or vol_warn_enable=0 in the
 * config), in which case the volume digits always stay their native white
 * and never recolor red, regardless of level. */

/* Quick-access drawer mirrors of the persistent status bar / player-screen
 * widgets above -- kept in sync from the same single update sites (see
 * refresh_clock_label/refresh_battery_topbar/refresh_wifi_icon/
 * set_play_button_state/favorite_icon_event_cb/apply_track_metadata_to_ui)
 * rather than introducing a second, separately-polled source of truth.
 * NULL until build_quick_drawer() runs; every update site guards on that. */

/* The quick drawer brightness slider and label are refreshed both at build time
 * and whenever the drawer opens to ensure they match the current screen brightness. */


/* Defined with the rest of the Subsonic streaming logic further down;
 * forward-declared here since polling for a finished background download
 * belongs alongside every other per-tick "did a background thing finish"
 * check in update_timer_cb (hw_buttons, audio_consume_track_advanced, ...). */
static void poll_dlna_control(void);
/* Defined alongside library_scan_once()'s own background-thread wrapper,
 * much further down -- forward-declared here since subsonic_library_
 * download_thread_func() (defined well before that point) needs to trigger
 * a rescan once its own batch download finishes. */
void start_library_rescan(void);
void start_library_auto_rescan(void);
void poll_wifi_scan(void);
void poll_wifi_connect(void);
void poll_wifi_connect_saved(void);
void poll_wifi_disconnect(void);
void poll_wifi_forget(void);
void poll_bt_scan(void);
void poll_bt_rate_query(void);
void poll_bt_rate_apply(void);
void poll_bt_connect(void);
void poll_bt_forget(void);

/* Full radio suspend after a long stretch with the screen off and nothing
 * going on -- deliberately separate from (and much longer than) the
 * screen-timeout backlight-off state itself: that can fire as often as
 * every 30s from ordinary momentary inactivity during otherwise-continuous
 * use, and WiFi/Bluetooth reassociation has a real cost, so tying radio
 * suspend to the same short timer would punish a user who's still actively
 * using the device, just with the screen dimming between taps. Skipped
 * entirely while music is playing, either DAC receive mode is on (both need
 * their radio to stay up to receive), or the device is charging (no battery
 * pressure to justify the reassociation cost). Whichever radios were
 * actually on get suspended, and only those get resumed on wake -- a radio
 * the user already had off before sleeping stays off. */
#define RADIO_SUSPEND_DELAY_MS (10 * 60 * 1000)
static uint32_t screen_off_since_tick = 0;
static bool inactivity_dimmed = false;

/* Earliest possible inactivity age for the interactive UI. LVGL's display
 * inactivity timestamp can predate gui_init()'s splash -> Home transition
 * (and, on the target, lv_display_trigger_activity() alone has now been
 * observed not to reliably discard that startup age). Clamp the value used
 * by dimming/timeout to elapsed interactive time so startup can never spend
 * the user's timeout budget. After this age grows past LVGL's normally-reset
 * inactivity value the clamp becomes a no-op, preserving ordinary touch and
 * hardware-button timeout behavior. */
static uint32_t interactive_ui_start_tick = 0;
static bool interactive_ui_started = false;

void gui_reset_interactive_timeout_baseline(void) {
    interactive_ui_start_tick = lv_tick_get();
    interactive_ui_started = true;
    lv_display_trigger_activity(NULL);
}

/* The panel dominates power while lit. Dim well before the configured full
 * timeout, but keep enough time for reading and never override an explicit
 * user's screen-off duration. */
#define VISIBLE_STATUS_POLL_TICKS 4 /* 2 seconds at the 500 ms control tick */
static bool radios_suspended = false;
static bool wifi_was_on_before_suspend = false;
static bool bt_was_on_before_suspend = false;

/* Tracks whether audio was playing as of the last tick the screen was off,
 * so the radio-suspend/idle-shutdown clocks (both anchored on
 * screen_off_since_tick) can be restarted when playback actually stops --
 * see the reset logic where this is used, just below. */
static bool screen_off_playback_active = false;

/* Idle shutdown: a full poweroff (see idle_shutdown.h) after
 * current_settings.idle_shutdown_minutes with the screen off and nothing
 * going on -- same gating as radio-suspend above (not playing, not
 * charging, no DAC receive mode active) since none of those should ever be
 * interrupted by the device turning itself off. idle_shutdown_attempted
/* Idle shutdown logic:
 *
 * Automatically power off if the device is inactive for a user-defined period
 * with the screen off. Shutdown is blocked if music is playing, the device is
 * charging, or if external audio (USB/WiFi/BT DAC) is active.
 *
 * In addition to WiFi and Bluetooth DAC modes, USB DAC mode (USB_MODE_DAC)
 * is checked to prevent idle suspend or radio disconnect while receiving USB audio. */
static bool idle_shutdown_attempted = false;


static bool shutdown_background_work_active(void) {
    return gui_library_has_background_work() || gui_subsonic_has_background_work() ||
           gui_network_has_background_work() || gui_lyrics_has_background_work() ||
           gui_player_has_background_work() || gui_shell_has_background_work() ||
           plugin_manager_has_background_work() || playlist_files_has_active_write() ||
           gui_player_queue_write_busy() || screenshot_is_busy();
}

/* Grace window after resuming from suspend. hw_buttons sets its short-tap flag
 * on button release (handle_key_event, value==0). The physical button press that
 * wakes the kernel from suspend is read asynchronously by the evdev thread,
 * and its release event can be consumed after power_suspend_now() returns.
 * Any power-button press consumed within this window is treated as an echo of
 * the wake press and discarded to avoid toggling the screen back off. */
#define RESUME_POWER_DRAIN_WINDOW_MS 3000
static uint32_t resumed_from_suspend_tick = 0;
static bool resumed_from_suspend_pending = false;
/* power_suspend_now() returns from inside update_timer_cb(), after that
 * callback already sampled screen_was_on/screen_on_now. Remember that
 * out-of-band wake so the next control tick still runs every ordinary
 * screen_just_woke refresh/status/radio-reset action. */
static bool force_screen_just_woke = false;

/* Keeps the three pieces of screen runtime state inseparable. Merely
 * enabling an indev does not restart its paused LVGL read timer, and merely
 * lighting the panel does not restart LVGL's paused refresh timer. Used by
 * both the ordinary on/off edge below and the special suspend-return path,
 * which changes backlight state too late in the current timer callback for
 * that ordinary edge detector to observe it. */
static void apply_screen_runtime_state(bool screen_on) {
    lv_indev_enable(NULL, screen_on);
    audio_set_low_power_mode(!screen_on);

    gui_shell_reset_drag_state();
    gui_library_reset_drag_state();
    gui_lock_screen_reset_drag_state();

    lv_timer_t * refr_timer = lv_display_get_refr_timer(lv_display_get_default());
    if (refr_timer) {
        if (screen_on) lv_timer_resume(refr_timer);
        else lv_timer_pause(refr_timer);
    }

    int indev_timer_count = 0;
    for (lv_indev_t * indev = lv_indev_get_next(NULL); indev; indev = lv_indev_get_next(indev)) {
        lv_timer_t * read_timer = lv_indev_get_read_timer(indev);
        if (!read_timer) continue;
        if (screen_on) lv_timer_resume(read_timer);
        else lv_timer_pause(read_timer);
        indev_timer_count++;
    }

    if (screen_on) lv_async_call(full_redraw_async_cb, NULL);
#ifdef UI_PERF_TRACE
    printf("PERF screen_runtime screen_on=%d refr_timer=%d indev_timers=%d\n",
           screen_on, refr_timer != NULL, indev_timer_count);
#endif
#ifdef UI_GESTURE_TRACE
    printf("[GESTURE_TRACE] apply_screen_runtime_state: screen_on=%d\n", screen_on);
#endif
}

/* Restores backlight, input devices, and inactivity timer state immediately
 * after waking from suspend. Shared between idle-shutdown and Car Mode suspend paths. */
static void resume_from_suspend_fixups(void) {
    backlight_set_screen_on(true);
    apply_screen_runtime_state(true);
    force_screen_just_woke = true;
    lv_display_trigger_activity(NULL);
    resumed_from_suspend_tick = lv_tick_get();
    resumed_from_suspend_pending = true;
    DBG_LOG("resume: grace window armed at tick=%u\n", resumed_from_suspend_tick);
}

/* Defined with the rest of the All Songs screen, much further down --
 * forward-declared here since update_timer_cb() (just below) needs it for
 * the remote-control play-by-index consumer. */
/* Defined with the rest of the remote-control scoped-play machinery, much
 * further down -- forward-declared here since update_timer_cb() (just
 * below) needs it for the remote-control play-by-index consumer. */

/* Defined with the rest of the power-off countdown popup, much further
 * down -- forward-declared here since update_timer_cb() (just below) needs
 * them for the power-button long-press consumer. */

/* Defined alongside the rest of live search's async DB query, much further
 * down -- forward-declared here since update_timer_cb() (just below) polls
 * it every tick, same as poll_cover_decode()/poll_lyrics_load(). */

/* Physical play/pause button handler. In double-click mode (mode 2), a timer
 * window disambiguates single presses from double clicks across poll cycles. */
#define PLAY_PAUSE_DOUBLE_CLICK_MS 700
static lv_timer_t * play_pause_click_timer = NULL;
static int play_pause_click_count = 0;

static void physical_skip_prev_track(void) {
    gui_player_step_manual(-1);
}

static void play_pause_click_timeout_cb(lv_timer_t * timer) {
    (void) timer;
    lv_timer_pause(play_pause_click_timer);
    play_pause_click_count = 0;
    toggle_play_pause();
}

static void handle_physical_play_pause_press(void) {
    int mode = current_settings.play_pause_button_mode;
    if (mode == 1) {
        physical_skip_prev_track();
        return;
    }
    if (mode == 2) {
        if (!play_pause_click_timer) {
            play_pause_click_timer = lv_timer_create(play_pause_click_timeout_cb, PLAY_PAUSE_DOUBLE_CLICK_MS, NULL);
            lv_timer_pause(play_pause_click_timer);
        }
        play_pause_click_count++;
        if (play_pause_click_count >= 2) {
            lv_timer_pause(play_pause_click_timer);
            play_pause_click_count = 0;
            physical_skip_prev_track();
        } else {
            lv_timer_reset(play_pause_click_timer);
            lv_timer_resume(play_pause_click_timer);
        }
        return;
    }
    toggle_play_pause();
}

/* plugin_manager_notify_screen_woke()'s handlers can do real GUI work
 * (LockScreen.lua's own screen_woke handler calls plugin.show_lock_screen(),
 * which nav_push()es a new screen) -- calling that notify function directly
 * from inside the screen_just_woke branch below would be exactly the
 * re-entrant-from-inside-lv_timer_handler() hazard this same function's own
 * comment on full_redraw_async_cb already warns about (a few lines up).
 * Deferred via lv_async_call() the same way, for the same reason. */
static void notify_screen_woke_async_cb(void * unused) {
    (void) unused;
    plugin_manager_notify_screen_woke();
}

/* Keep power-button consumption separate from the 500ms control timer. While
 * the screen is off, main.c may sleep for up to that same interval, making a
 * button wake feel sluggish even though backlight I/O is asynchronous. */
static void process_power_button_events(void) {
    if (resumed_from_suspend_pending && lv_tick_elaps(resumed_from_suspend_tick) >= RESUME_POWER_DRAIN_WINDOW_MS) {
        DBG_LOG("resume: grace window expired unused at tick=%u\n", lv_tick_get());
        resumed_from_suspend_pending = false;
    }

    if (hw_buttons_consume_power()) {
        DBG_LOG("resume: hw_buttons_consume_power() true at tick=%u, pending=%d\n",
                lv_tick_get(), resumed_from_suspend_pending);
        if (resumed_from_suspend_pending) {
            resumed_from_suspend_pending = false;
        } else {
            bool turn_on = !backlight_screen_is_on();
            lv_display_trigger_activity(NULL);
            backlight_set_screen_on(turn_on);
            apply_screen_runtime_state(turn_on);
            if (turn_on) {
                inactivity_dimmed = false;
                force_screen_just_woke = true;
            }
        }
    }

    if (hw_buttons_consume_power_long_press()) {
        if (resumed_from_suspend_pending) {
            resumed_from_suspend_pending = false;
        } else {
            bool was_on = backlight_screen_is_on();
            backlight_set_screen_on(true);
            if (!was_on) {
                apply_screen_runtime_state(true);
                inactivity_dimmed = false;
                force_screen_just_woke = true;
            }
            lv_display_trigger_activity(NULL);
            start_power_off_countdown();
        }
    }
}

static void power_button_timer_cb(lv_timer_t * timer) {
    (void) timer;
    process_power_button_events();
}

static void update_timer_cb(lv_timer_t * timer) {
    (void) timer;

    /* Physical volume/skip/play-pause buttons: applied here, on the one
     * thread allowed to touch LVGL widgets (see hw_buttons.h). Play/pause
     * is a count, not a bool -- this poll only runs every 500ms, and a real
     * double-click's two presses routinely land inside one poll window; a
     * count of 2 here dispatches immediately, twice in a row, letting
     * handle_physical_play_pause_press()'s own click-count state (mode 2)
     * see it as a same-tick double-click without waiting on its timer. */
    int played_paused_count = hw_buttons_consume_play_pause();
    for (int i = 0; i < played_paused_count; i++) {
        handle_physical_play_pause_press();
    }
    /* Physical skip buttons use gui_player_step_manual() so shuffle and repeat
     * modes are respected consistently with on-screen transport controls. */
    bool skipped_next = hw_buttons_consume_next();
    if (skipped_next) {
        gui_player_step_manual(1);
    }
    bool skipped_prev = hw_buttons_consume_prev();
    if (skipped_prev) {
        gui_player_step_manual(-1);
    }
    /* Holding the physical Next button fast-forwards within the track, same
     * as holding the touch one -- see hw_buttons_consume_next_seek_steps()'s
     * own comment. */
    bool next_seek_is_first;
    int next_seek_steps = hw_buttons_consume_next_seek_steps(&next_seek_is_first);
    if (next_seek_steps > 0) {
        gui_player_hw_next_seek_steps(next_seek_steps, next_seek_is_first);
    }
    bool screenshot_request_pending = hw_buttons_consume_screenshot();

#ifndef HOST_BUILD
    /* Bluetooth accessory controls dispatch to UI/player state on this thread. */
    if (bt_media_player_consume_play_pause()) {
        toggle_play_pause();
    }
    if (bt_media_player_consume_next()) {
        gui_player_step_manual(1);
    }
    if (bt_media_player_consume_prev()) {
        gui_player_step_manual(-1);
    }
    /* Keeps PlaybackStatus accurate for whenever BlueZ/the accessory next
     * queries it -- cheap (no subprocess, just a mutex-protected bool),
     * safe to do unconditionally every tick rather than hunting down every
     * call site that can change play state. */
    bt_media_player_notify_playback_state(audio_is_playing());
    bool has_track = gui_player_has_active_track();
    bt_media_player_notify_track(has_track ? gui_player_get_now_playing_title() : "",
                                 has_track ? gui_player_get_now_playing_folder() : "",
                                 has_track ? gui_player_get_now_playing_album() : "",
                                 has_track ? gui_player_get_now_playing_genre() : "",
                                 has_track ? gui_player_get_now_playing_track_number() : 0,
                                 audio_get_position_seconds(), audio_get_duration_seconds());
#endif

    /* Accessory-originated AVRCP volume changes arrive on bluealsa's
     * monitor thread. Mirror the already-applied audio value into every UI
     * and persisted representation here on the sole LVGL thread, so the
     * next hardware-button step starts from what the user actually hears. */
    int bt_synced_volume_percent;
    if (bt_control_source_volume_sync_consume_percent(&bt_synced_volume_percent) &&
        !gui_player_volume_is_being_adjusted()) {
        gui_player_set_volume_percent(bt_synced_volume_percent);
        current_settings.volume = (float) bt_synced_volume_percent / 100.0f;
        settings_save_async(&current_settings);
        show_volume_popup(bt_synced_volume_percent);
        refresh_volume_topbar(bt_synced_volume_percent);
    }

    int volume_delta = hw_buttons_consume_volume_delta();
    if (volume_delta != 0) {
        int32_t new_percent = gui_player_get_volume_percent() + volume_delta;
        if (new_percent < 0) new_percent = 0;
        if (new_percent > 100) new_percent = 100;
        gui_player_set_volume_percent(new_percent);
        audio_set_volume((float) new_percent / 100.0f);
        current_settings.volume = (float) new_percent / 100.0f;
        settings_save_async(&current_settings);
        show_volume_popup(new_percent);
        refresh_volume_topbar(new_percent);
    }

    /* Phone remote-control (Phase 2, remote_control.h): same "background
     * thread sets a flag, this is the one thread allowed to touch LVGL/
     * audio state" pattern as hw_buttons/bt_media_player just above --
     * reuses the exact same shuffle-aware stepping and volume-persistence
     * shape as those, rather than a separate implementation. */
    screenshot_request_pending = remote_control_consume_screenshot() || screenshot_request_pending;
    if (screenshot_request_pending) (void) screenshot_start();
    screenshot_completion_t screenshot_result;
    if (screenshot_poll_completion(&screenshot_result)) {
        if (screenshot_result.saved) {
            show_info_toast("Screenshot saved");
            plugin_manager_notify_screenshot_saved(screenshot_result.path);
        } else {
            char failed_msg[64];
            snprintf(failed_msg, sizeof(failed_msg), "Screenshot failed (%s)",
                     screenshot_result.reason[0] ? screenshot_result.reason : "unknown");
            show_error_toast(failed_msg);
            plugin_manager_notify_screenshot_failed(screenshot_result.reason);
        }
    }

    if (remote_control_consume_play_pause()) {
        toggle_play_pause();
    }
    if (remote_control_consume_next()) {
        gui_player_step_manual(1);
    }
    if (remote_control_consume_prev()) {
        gui_player_step_manual(-1);
    }
    if (remote_control_consume_mode_cycle()) {
        cycle_play_mode();
    }
    int remote_play_mode;
    if (remote_control_consume_play_mode(&remote_play_mode)) {
        gui_player_set_play_mode(remote_play_mode);
    }
    int remote_seek_seconds;
    if (remote_control_consume_seek(&remote_seek_seconds)) {
        audio_seek((double) remote_seek_seconds);
    }
    int remote_volume_percent;
    if (remote_control_consume_volume(&remote_volume_percent) &&
        !gui_player_volume_is_being_adjusted()) {
        gui_player_set_volume_percent(remote_volume_percent);
        audio_set_volume((float) remote_volume_percent / 100.0f);
        current_settings.volume = (float) remote_volume_percent / 100.0f;
        settings_save_async(&current_settings);
        show_volume_popup(remote_volume_percent);
        refresh_volume_topbar(remote_volume_percent);
    }
    int64_t remote_queue_id;
    char remote_queue_catalog_revision[METADATA_DB_CATALOG_REVISION_SIZE] = {0};
    if (remote_control_consume_queue_index(&remote_queue_id, remote_queue_catalog_revision,
                                            sizeof(remote_queue_catalog_revision))) {
        song_row_t remote_queue_row;
        bool found = remote_queue_catalog_revision[0]
            ? metadata_db_catalog_get_song_revision(remote_queue_catalog_revision, remote_queue_id,
                                                    &remote_queue_row) == METADATA_DB_CATALOG_OK
            : metadata_db_get_song_by_id(remote_queue_id, &remote_queue_row);
        if (found) queue_add_song(remote_queue_row.path);
    }
    int remote_queue_remove_offset;
    uint64_t remote_queue_revision;
    if (remote_control_consume_queue_remove(&remote_queue_remove_offset, &remote_queue_revision))
        gui_player_remote_queue_remove(remote_queue_remove_offset, remote_queue_revision);
    if (remote_control_consume_queue_clear(&remote_queue_revision))
        gui_player_remote_queue_clear(remote_queue_revision);
    int64_t remote_play_id;
    char remote_play_playlist[128], remote_play_artist[128], remote_play_album_artist[128], remote_play_album[128];
    char remote_play_catalog_revision[METADATA_DB_CATALOG_REVISION_SIZE] = {0};
    if (remote_control_consume_play_index(&remote_play_id, remote_play_playlist, sizeof(remote_play_playlist),
                                           remote_play_artist, sizeof(remote_play_artist), remote_play_album_artist,
                                           sizeof(remote_play_album_artist), remote_play_album,
                                           sizeof(remote_play_album), remote_play_catalog_revision,
                                           sizeof(remote_play_catalog_revision))) {
        /* remote_play_id is a song id (metadata_db.c's rowid-based
         * song_row_t.id) -- resolve it to a path, then hand that straight to
         * play_remote_control_song() (defined with the rest of the
         * remote-control scoped-play machinery, much further down), which
         * resolves the path plus the playlist/artist/album context into the
         * right playlist and position via its own DB queries, falling back
         * to the whole library (All Songs, by title offset) when no scope
        * applies. */
        song_row_t remote_play_row;
        bool found = remote_play_catalog_revision[0]
            ? metadata_db_catalog_get_song_revision(remote_play_catalog_revision, remote_play_id,
                                                    &remote_play_row) == METADATA_DB_CATALOG_OK
            : metadata_db_get_song_by_id(remote_play_id, &remote_play_row);
        if (found) {
            play_remote_control_song(remote_play_row.path, remote_play_playlist, remote_play_artist,
                                      remote_play_album_artist, remote_play_album);
        }
    }

    /* Auto-stop on headphone-output loss: this hardware has no built-in
     * speaker, so a mid-playback disconnect (headphone jack pulled, or the
     * connected Bluetooth A2DP headphone drops) leaves nowhere for the
     * audio to go -- stop outright rather than leaving it silently playing
     * into nothing. Checked every tick, deliberately NOT gated on
     * screen_on_now below (background, screen-off playback needs the same
     * protection). get_headphone_state() is a cheap sysfs read (no
     * subprocess); the A2DP half reuses whatever poll_refresh_bt_icon()
     * last found rather than re-querying bluealsactl here every tick --
     * that's already throttled to its own ~5s cadence (see
     * refresh_bt_icon_thread_func()), and a few seconds of extra latency on
     * just the Bluetooth half is an acceptable tradeoff against forking a
     * process every single tick.
     *
     * Target-only: HOST_BUILD has no real jack/BT hardware to detect at
     * all (get_headphone_state() is unconditionally HEADPHONE_STATE_NONE there, see
     * headphone_status.h), so this would otherwise fire a spurious "stop"
     * on the very first tick after starting playback in the simulator --
     * there's no real disconnect to protect against on host, since the dev
     * machine's own speakers are the actual output regardless of this
     * app's simulated jack/BT state. */
#ifndef HOST_BUILD
    /* Bluetooth disconnect is debounced on wall-clock time (12s) to prevent
     * transient bluealsactl busy polling failures from falsely cutting playback.
     * Wired headphone disconnects remain immediate via direct sysfs reads. */
#define BT_OUTPUT_DISCONNECT_DEBOUNCE_MS 12000
    {
        static uint32_t bt_disconnected_since_tick = 0; /* 0 = currently connected (or never sampled) */
        if (gui_shell_is_bt_audio_connected()) {
            bt_disconnected_since_tick = 0;
        } else if (bt_disconnected_since_tick == 0) {
            bt_disconnected_since_tick = lv_tick_get();
        }
        /* The monitor confirms a removal after a short reconfiguration
         * grace period: BlueALSA can remove/re-add its PCM while changing
         * sample rates without disconnecting the headphones. Only a
         * confirmed removal bypasses the slower polling fallback above.
         * The event is consumed once; reconnection never auto-resumes. */
        bool bt_disconnect_event = bt_control_output_disconnect_consume();
        if (bt_disconnect_event) {
            gui_shell_notify_bt_audio_disconnected();
        }
        bool bt_connected_debounced = !bt_disconnect_event &&
            (gui_shell_is_bt_audio_connected() ||
             lv_tick_elaps(bt_disconnected_since_tick) < BT_OUTPUT_DISCONNECT_DEBOUNCE_MS);

        static bool last_output_connected = true; /* starts true so nothing fires before any real state has been sampled */
        /* Headphone detection covers both single-ended and balanced jacks. */
        bool output_connected = get_headphone_state() != HEADPHONE_STATE_NONE || bt_connected_debounced;
        /* Pause playback on disconnect while preserving decoder and position
         * so playback can resume when an output is reconnected. Only triggers
         * if audio is actively playing to avoid toggling an already-paused track. */
        bool was_playing = audio_is_playing();
        if (last_output_connected && !output_connected && was_playing) {
            DBG_LOG("gui: output disconnected while playing -- pausing playback\n");
            audio_toggle_pause();
            set_play_button_state(false);
            show_error_toast("Paused: headphones disconnected");
        }
        last_output_connected = output_connected;
    }

    /* Synchronize balanced output routing for hardware supporting balanced jacks. */
    audio_output_sync_balanced_output();
#endif

    /* Car Mode:
     * When external power is disconnected, save current playback position and
     * power off the device (idle_shutdown_now()). When power returns, the device
     * boots and auto-resumes playback.
     *
     * External power presence is edge-triggered using physical power supply state
     * rather than battery charging status, which is a charger-activity signal, not
     * a cable-presence one (see battery_is_charging()'s own doc comment). Power
     * removal must persist for three consecutive 500ms control ticks to filter
     * transient fluctuations.
     *
     * Only triggers if a track is actively playing or paused. */
#ifndef HOST_BUILD
    {
        enum { CAR_POWER_REMOVAL_DEBOUNCE_POLLS = 3 };
        static bool power_sample_initialized = false;
        static bool last_external_power = false;
        static unsigned int power_absent_polls = 0;
        static bool car_shutdown_pending = false;

        if (!current_settings.car_mode_enabled) {
            /* Re-sample from a clean baseline if Car Mode is enabled later;
             * enabling it while already unplugged is not itself a removal
             * edge and must not immediately power the device off. */
            power_sample_initialized = false;
            power_absent_polls = 0;
            car_shutdown_pending = false;
        } else {
            /* Keep the sysfs directory/file reads completely out of the
             * normal 500ms control path when Car Mode is disabled. */
            /* No charge_limiter_is_holding()-based fallback for an UNKNOWN
             * read here (there used to be one): under the old on/off
             * percentage limiter, "holding" only became true once the
             * charger had actually been disabled near 85% SoC while
             * plugged in -- a reasonable proxy for "must be on external
             * power". Under the current voltage-cap design, "holding" just
             * means the 4.2V register write succeeded, which happens
             * throughout an entire charge session regardless of whether
             * power is still connected at the moment of THIS poll (the
             * AXP2101 is I2C-reachable off the system rail either way) --
             * it is no longer meaningful evidence of a physical cable.
             * Treating UNKNOWN as ambiguous (see the branch below) is the
             * existing safe default already. */
            battery_external_power_state_t power_state = battery_get_external_power_state();

            if (power_state == BATTERY_EXTERNAL_POWER_UNKNOWN) {
                /* Consecutive means consecutive valid samples.  A sysfs
                 * failure is not evidence of either state and must not let
                 * two old off samples plus one later off sample confirm a
                 * removal.  An already-confirmed pending shutdown is left
                 * intact; only a positive connected sample cancels it. */
                power_absent_polls = 0;
            } else {
                bool external_power = power_state == BATTERY_EXTERNAL_POWER_CONNECTED;
                if (!power_sample_initialized) {
                    last_external_power = external_power;
                    power_sample_initialized = true;
                    power_absent_polls = 0;
                    DBG_LOG("car_mode: initial external power=%d\n", external_power);
                } else if (external_power) {
                    if (!last_external_power || power_absent_polls > 0 || car_shutdown_pending)
                        DBG_LOG("car_mode: external power present; removal cancelled\n");
                    last_external_power = true;
                    power_absent_polls = 0;
                    car_shutdown_pending = false;
                } else if (last_external_power) {
                    if (power_absent_polls < CAR_POWER_REMOVAL_DEBOUNCE_POLLS) power_absent_polls++;
                    if (power_absent_polls >= CAR_POWER_REMOVAL_DEBOUNCE_POLLS) {
                        last_external_power = false;
                        car_shutdown_pending = audio_is_playing() || audio_is_paused();
                        DBG_LOG("car_mode: external power removal confirmed; shutdown_pending=%d\n",
                                car_shutdown_pending);
                    }
                }
            }
        }

        if (car_shutdown_pending && !shutdown_background_work_active()) {
            current_settings.last_position = audio_get_resume_position_seconds();
            /* This save is intentionally synchronous: idle_shutdown_now() is
             * imminent, so the final playback checkpoint must reach storage
             * before power is removed. */
            settings_save(&current_settings);
            idle_shutdown_now(); /* full poweroff -- does not return, see idle_shutdown.h */
        }
    }
#endif

    /* Feed button activity into the inactivity clock so physical button presses
     * delay auto-timeout during active use without waking an already-off screen. */
    if (played_paused_count > 0 || skipped_next || skipped_prev || volume_delta != 0) {
        lv_display_trigger_activity(NULL);
    }

    bool screen_was_on = backlight_screen_is_on();

    /* Power-button events are consumed by power_button_timer_cb(). The power
     * action overlay itself has no timer to poll -- Power Off/Reboot fire
     * immediately on tap. */

    uint32_t screen_inactive_ms = lv_display_get_inactive_time(NULL);
    if (interactive_ui_started) {
        uint32_t interactive_age_ms = lv_tick_elaps(interactive_ui_start_tick);
        if (screen_inactive_ms > interactive_age_ms) screen_inactive_ms = interactive_age_ms;
    }
    bool screen_on_before_timeout = backlight_screen_is_on();
    /* Exempt lyrics view from dimming and timeout while audio is playing so lyrics
     * remain readable without requiring continuous touch input. When paused, normal
     * timeout applies. */
    bool lyrics_screen_active = lv_screen_active() == gui_lyrics_get_screen() && audio_is_playing();
    uint32_t active_dim_delay_ms = (uint32_t) current_settings.screen_dim_delay_seconds * 1000;
    if (current_settings.screen_timeout_enabled && active_dim_delay_ms >= (uint32_t) current_settings.screen_timeout_seconds * 1000) {
        active_dim_delay_ms = UINT32_MAX; /* Prevent dimming if delay exceeds or equals timeout */
    }
    if (current_settings.screen_dimming_enabled && screen_on_before_timeout &&
        !inactivity_dimmed && !lyrics_screen_active && screen_inactive_ms >= active_dim_delay_ms) {
        backlight_set_dimmed(true);
        inactivity_dimmed = true;
    } else if (screen_on_before_timeout && inactivity_dimmed &&
               (!current_settings.screen_dimming_enabled || lyrics_screen_active || screen_inactive_ms < active_dim_delay_ms)) {
        backlight_set_dimmed(false);
        inactivity_dimmed = false;
    }
    if (current_settings.screen_timeout_enabled && backlight_screen_is_on() &&
        !lyrics_screen_active &&
        screen_inactive_ms >= (uint32_t) current_settings.screen_timeout_seconds * 1000) {
        backlight_set_screen_on(false);
        inactivity_dimmed = false;
    }

    /* Battery: skip every bit of UI-only refresh work below (label updates,
     * and especially the wpa_cli/bluetoothctl subprocess forks behind
     * refresh_wifi_icon()/refresh_bt_icon()) while nobody can see the
     * screen -- otherwise a music session left playing overnight with the
     * screen asleep still forks a bluetoothctl process every
     * WIFI_POLL_TICKS ticks for pixels nobody's looking at. Force one
     * refresh right on wake so nothing looks stale afterwards instead of
     * waiting up to WIFI_POLL_TICKS ticks to catch up. */
    bool screen_on_now = backlight_screen_is_on();
    bool screen_just_woke = screen_on_now && (!screen_was_on || force_screen_just_woke);
    if (screen_just_woke) force_screen_just_woke = false;

    /* Touch input and display refresh follow screen on/off state. Disabling indev
     * devices when screen is off prevents accidental touch events. Physical buttons
     * continue operating via their dedicated evdev thread. */
    if (screen_on_now != screen_was_on) {
        apply_screen_runtime_state(screen_on_now);
    }

    if (screen_just_woke) {
        inactivity_dimmed = false;
        idle_shutdown_attempted = false;
        lv_async_call(notify_screen_woke_async_cb, NULL);
        if (radios_suspended) {
            gui_shell_resume_connections(wifi_was_on_before_suspend, bt_was_on_before_suspend);
            radios_suspended = false;
        }
    } else if (!screen_on_now) {
        bool playing_now_for_idle = audio_is_playing();
        if (screen_was_on) {
            /* Just went to sleep this tick -- the 10-minute countdown starts
             * from here, not from whenever the inactivity that caused it
             * began. */
            screen_off_since_tick = lv_tick_get();
        } else if (screen_off_playback_active && !playing_now_for_idle) {
            /* If playback ended while the screen was already off, restart the idle
             * timer from this moment so radio suspend and idle shutdown wait for
             * the full configured delay after playback stops. */
            screen_off_since_tick = lv_tick_get();
        }
        screen_off_playback_active = playing_now_for_idle;
        /* bt_is_powered_cached, not bt_control_is_powered(), deliberately --
         * the latter forks a process, and this condition is checked every
         * tick while the screen is off, not throttled like refresh_bt_icon()
         * is. The cached value is frozen at whatever it was when the screen
         * went dark, which is fine: nothing in this app can change BT power
         * state without a UI the user can't reach with the screen off. */
        if (!radios_suspended && !current_settings.wifi_dac_mode_enabled && !current_settings.bt_dac_mode_enabled &&
            current_settings.usb_mode != (int) USB_MODE_DAC &&
            !audio_is_playing() && !battery_is_charging() && !shutdown_background_work_active() &&
            lv_tick_elaps(screen_off_since_tick) >= RADIO_SUSPEND_DELAY_MS) {
            gui_shell_suspend_connections(&wifi_was_on_before_suspend, &bt_was_on_before_suspend);
            radios_suspended = true;
        }

#ifdef TEST_BUILD_TAG
        /* Diagnostic logging for idle suspend/shutdown blockers. When an idle
         * deadline expires but background work is active, log which subsystems
         * are reporting busy (throttled to once every 30s). */
        uint32_t idle_elapsed_ms = lv_tick_elaps(screen_off_since_tick);
        bool radio_suspend_due = !radios_suspended && idle_elapsed_ms >= RADIO_SUSPEND_DELAY_MS;
        bool idle_action_due = !idle_shutdown_attempted && current_settings.idle_shutdown_enabled &&
                               idle_elapsed_ms >= (uint32_t) current_settings.idle_shutdown_minutes * 60 * 1000;
        if ((radio_suspend_due || idle_action_due) &&
            !current_settings.wifi_dac_mode_enabled && !current_settings.bt_dac_mode_enabled &&
            current_settings.usb_mode != (int) USB_MODE_DAC && !audio_is_playing() && !battery_is_charging()) {
            bool library_busy = gui_library_has_background_work();
            bool subsonic_busy = gui_subsonic_has_background_work();
            bool network_busy = gui_network_has_background_work();
            bool lyrics_busy = gui_lyrics_has_background_work();
            bool player_busy = gui_player_has_background_work();
            bool shell_busy = gui_shell_has_background_work();
            bool plugins_busy = plugin_manager_has_background_work();
            bool playlist_write_busy = playlist_files_has_active_write() || gui_player_queue_write_busy();
            if (library_busy || subsonic_busy || network_busy || lyrics_busy || player_busy || shell_busy ||
                plugins_busy || playlist_write_busy) {
                static uint32_t last_busy_log_tick = 0;
                if (last_busy_log_tick == 0 || lv_tick_elaps(last_busy_log_tick) >= 30000) {
                    last_busy_log_tick = lv_tick_get();
                    DBG_LOG("gui: idle suspend/shutdown blocked -- library=%d subsonic=%d network=%d lyrics=%d "
                            "player=%d shell=%d plugins=%d playlist_write=%d\n",
                            library_busy, subsonic_busy, network_busy, lyrics_busy, player_busy, shell_busy,
                            plugins_busy, playlist_write_busy);
                }
            }
        }
#endif /* TEST_BUILD_TAG */

        /* Deliberately independent of radios_suspended -- idle_shutdown_minutes
         * is a separate, normally-longer setting than RADIO_SUSPEND_DELAY_MS,
         * and shutdown should still happen on its own schedule even if radio
         * suspend is impossible right now (e.g. wifi_dac_mode_enabled) --
         * except it explicitly shares the same DAC-mode/playing/charging
         * gate, since none of those should ever be interrupted by the
         * device powering itself off out from under them. */
        if (!idle_shutdown_attempted && current_settings.idle_shutdown_enabled &&
            !current_settings.wifi_dac_mode_enabled && !current_settings.bt_dac_mode_enabled &&
            current_settings.usb_mode != (int) USB_MODE_DAC &&
            !audio_is_playing() && !battery_is_charging() && !shutdown_background_work_active() &&
            lv_tick_elaps(screen_off_since_tick) >= (uint32_t) current_settings.idle_shutdown_minutes * 60 * 1000) {
            if (current_settings.idle_suspend_enabled) {
                plugin_manager_notify_suspending();
                led_control_suspend();
                power_suspend_now();
                led_control_resume(current_settings.led_indicator_enabled);

                /* Restore UI state and reset inactivity timers on resume so the
                 * display does not immediately time out again. */
                resume_from_suspend_fixups();
                plugin_manager_notify_system_resumed();

                /* Unlike idle_shutdown_now() (which never returns --
                 * poweroff ends the process), this call CAN legitimately
                 * return without a real user wake (e.g. a spurious IRQ) --
                 * screen_just_woke won't fire to reset this flag in that
                 * case, so reset it here instead. Worst case if the device
                 * really did just wake for a moment is one immediate
                 * re-suspend on the next tick, not getting stuck awake
                 * indefinitely after a single spurious wake. */
                idle_shutdown_attempted = false;
            } else {
                idle_shutdown_attempted = true;
                idle_shutdown_now();
            }
        }
    }

    if (screen_on_now) {
        gui_shell_update_topbar(screen_just_woke);
    }

    /* Also unconditional on screen state -- charging happens with the
     * screen off far more often than on, so gating this the same way as the
     * topbar refreshes above would leave it capped at whatever percent the
     * screen happened to be on last. charge_limiter_poll() throttles its
     * own actual sysfs work internally, so calling it every tick here is
     * cheap. */
    charge_limiter_poll(current_settings.charge_limiter_enabled, false);
    safe_charging_poll(current_settings.safe_charging_enabled, false);
    led_control_poll(current_settings.led_indicator_enabled);
    int volume_percent = (int) (audio_get_volume() * 100.0f + 0.5f);
    if (volume_percent < 0) volume_percent = 0;
    if (volume_percent > 100) volume_percent = 100;
    plugin_manager_notify_volume_changed(volume_percent);
    plugin_manager_poll_battery();
    headphone_status_refresh_earpods_adc();

    if (current_settings.remote_control_enabled) {
        gui_player_sync_remote_queue();
        /* No separate now-playing metadata cache exists in this app beyond
         * what's already on screen -- song_title_label/artist_label
         * are this app's own single source of truth for title/artist (see
         * apply_track_metadata_to_ui()), so read them back rather than
         * standing up a second copy of the same state just for this.
         * album_label is read back the same way below. */
        const char * now_playing_path = gui_player_has_active_track()
                ? gui_player_get_current_track_path()
                : NULL;
        remote_control_notify_status(audio_is_playing(), audio_is_paused(), gui_player_get_now_playing_title(),
                                      gui_player_get_now_playing_folder(), gui_player_get_now_playing_album(), now_playing_path,
                                      (int) audio_get_position_seconds(), (int) audio_get_duration_seconds(),
                                      audio_get_volume(), current_settings.play_mode);
    }

    /* Polling for in-flight async operations (wifi/bt connect, library scan,
     * subsonic sync, ...) keeps running regardless of screen state -- these
     * only do real work when a request the user already made is still
     * pending, so gating them on screen state would leave that operation
     * stuck until the user wakes the screen back up. */
    poll_subsonic_download();
    poll_subsonic_library_download();
    poll_dlna_control();
    poll_subsonic_connect();
    poll_subsonic_browse();
    poll_wifi_scan();
    poll_wifi_connect();
    poll_wifi_connect_saved();
    poll_wifi_disconnect();
    poll_wifi_forget();
    gui_shell_poll();
    plugin_manager_poll();
    poll_bt_scan();
    poll_bt_rate_query();
    poll_bt_rate_apply();
    poll_bt_connect();
    poll_bt_forget();
    poll_library_rescan();
    poll_sd_format();
    poll_import_web_stop();
    poll_usb_mode_switch();
    poll_usb_storage_hotplug();
    poll_sd_card_hotplug();
    gui_library_poll_playlists();
    gui_library_poll_track_probes();
    poll_cover_decode();
    gui_lyrics_poll_load();
    gui_lyrics_poll_backdrop();
    poll_search_job();

    uint64_t playback_err_gen = 0;
    audio_error_t playback_err = audio_consume_error_ex(&playback_err_gen);
    if (playback_err != AUDIO_ERROR_NONE) {
        gui_player_handle_playback_error_ex(playback_err, playback_err_gen);
    } else if (audio_consume_track_advanced()) {
        gui_player_handle_auto_advance();
    } else if (audio_consume_track_finished()) {
        gui_player_handle_track_finished();
    }
    gui_player_poll_confirmed_playback();
    static bool queue_checkpoint_failure_notified;
    if (gui_player_queue_checkpoint_failed()) {
        if (!queue_checkpoint_failure_notified) {
            show_error_toast("Queue checkpoint failed; storage may be read-only");
            queue_checkpoint_failure_notified = true;
        }
    } else queue_checkpoint_failure_notified = false;
    static bool numeric_failure_notified;
    if (metadata_db_numeric_write_failed()) {
        if (!numeric_failure_notified) {
            show_error_toast("Playback history could not be saved");
            numeric_failure_notified = true;
        }
    } else numeric_failure_notified = false;
    gui_queue_poll();
    gui_network_poll_airplay_overlay();
    gui_track_info_poll();
    gui_library_poll_boot_prompt();

    /* All correctness-critical work above (buttons, queue transitions and
     * completion of requested background operations) still runs at 500 ms.
     * Everything below only redraws the player screen, so skip it while the
     * panel is physically off. This preserves hardware-button latency while
     * eliminating invisible slider/label rendering and asset I/O. */
    if (!backlight_screen_is_on()) return;

    /* The slide displays frozen frames; defer purely visual refreshes until
     * it settles, while keeping the playback/hardware polling above live. */
    if (gui_navigation_transition_in_progress()) return;

    if (!gui_player_has_active_track() || gui_player_is_seeking()) return;

    gui_player_update_progress();

    refresh_format_badge();
}



/* Same true-black default screen_builders.c's own style_theme_screen_bg is
 * initialized with. Kept as a separate plain literal (not that shared
 * style) specifically for gui_show_boot_splash() below -- that function
 * runs from main.c before gui_init() (and screen_builders_init_list_row_style(),
 * which initializes style_theme_screen_bg) has ever run, so the shared
 * style would still be all-zero memory at that point. */
#define SCREEN_BG_COLOR lv_color_make(0, 0, 0)

/* Real device uptime (lv_tick_get() is CLOCK_MONOTONIC-backed, see main.c's
 * custom_tick_get()) at which gui_show_boot_splash() below was called --
 * read back by gui_init() near the end of its own setup to decide how much
 * longer, if any, the splash needs to stay up. 0 is never a real tick value
 * this far into boot, so it doubles as "splash was never shown". */
static uint32_t boot_splash_start_tick = 0;

/* The bootloader now owns the longer (up to 5s) SD-discovery window before
 * launching this process. Keep a short player-owned splash so framebuffer
 * takeover remains visually clean, without stacking the former additional
 * three-second minimum onto every bootloader-managed startup. Standalone
 * test launches still retain a visible one-second splash. Bluetooth does
 * not extend this global wait; gui_shell.c tracks its readiness separately. */
#define BOOT_SPLASH_MIN_DISPLAY_MS 1000

/* Displays the boot splash image immediately after framebuffer initialization.
 *
 * Uses asset_path("boot_animation/en/0.jpg"), matching the JPEG drawn by the
 * bootloader to provide a seamless visual handoff without decode discrepancies.
 *
 * An override placed at /usr/data/theme_overrides/boot_animation/en/0.jpg is
 * picked up automatically via asset_path(). If the file does not exist,
 * SCREEN_BG_COLOR provides a clean black background.
 *
 * lv_layer_top() is hidden while the splash is displayed so status bar icons
 * and overlays built during gui_init() do not appear until the splash finishes. */
void gui_show_boot_splash(void) {
    boot_splash_start_tick = lv_tick_get();

    lv_obj_add_flag(lv_layer_top(), LV_OBJ_FLAG_HIDDEN);

    lv_obj_t * scr = lv_obj_create(NULL);
    /* Plain literal, not style_theme_screen_bg -- this runs from main.c
     * before gui_init() (and its lv_style_init(&style_theme_screen_bg))
     * has ever run, so that style object would still be all-zero memory
     * here. */
    lv_obj_set_style_bg_color(scr, SCREEN_BG_COLOR, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);

    lv_obj_t * img = lv_image_create(scr);
    lv_image_set_src(img, asset_path("boot_animation/en/0.jpg"));
    lv_obj_center(img);

    lv_screen_load(scr);
    lv_timer_handler(); /* force an immediate render -- nothing else pumps the loop until main.c's own main loop starts */
}





const char * basename_of(const char * path) {
    const char * slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}


const char * gui_plugin_get_play_mode(void) {
    switch ((play_mode_t) current_settings.play_mode) {
        case PLAY_MODE_REPEAT_ALL: return "repeat_all";
        case PLAY_MODE_REPEAT_ONE: return "repeat_one";
        case PLAY_MODE_SHUFFLE:    return "shuffle";
        case PLAY_MODE_SEQUENTIAL:
        default:                   return "sequential";
    }
    return "sequential";
}

const char * gui_plugin_get_current_track_path(void) {
    return gui_player_has_active_track() ? gui_player_get_current_track_path() : NULL;
}

char ** gui_plugin_get_artist_albums(const char * artist, int * out_count) {
    *out_count = 0;
    int64_t count64 = metadata_db_count_albums_for_group(METADATA_DB_GROUP_ARTIST, artist);
    if (count64 <= 0 || count64 > INT_MAX) return NULL;
    int album_count = (int) count64;
    group_row_t * rows = malloc(sizeof(*rows) * (size_t) album_count);
    char ** names = calloc((size_t) album_count, sizeof(*names));
    if (!rows || !names) { free(rows); free(names); return NULL; }
    int n = metadata_db_get_albums_for_group(METADATA_DB_GROUP_ARTIST, artist, 0, album_count, rows);
    for (int i = 0; i < n; i++) {
        names[i] = strdup(rows[i].name);
        if (!names[i]) {
            for (int j = 0; j < i; j++) free(names[j]);
            free(names);
            free(rows);
            return NULL;
        }
    }
    free(rows);
    *out_count = n;
    return names;
}

static char ** load_plugin_album_group_paths(const char * album, const char * album_artist, int * out_count) {
    *out_count = 0;
    char ** paths = NULL;
    size_t capacity = 0;
    int loaded = 0;
    song_row_t rows[64];
    for (;;) {
        int got = metadata_db_get_album_songs(album, album_artist, loaded, rows, 64);
        if (got <= 0) break;
        if ((size_t) (loaded + got) > capacity) {
            size_t new_capacity = capacity == 0 ? 64 : capacity * 2;
            while (new_capacity < (size_t) (loaded + got)) new_capacity *= 2;
            char ** grown = realloc(paths, new_capacity * sizeof(*paths));
            if (!grown) {
                for (int j = 0; j < loaded; j++) free(paths[j]);
                free(paths);
                return NULL;
            }
            paths = grown;
            capacity = new_capacity;
        }
        for (int i = 0; i < got; i++) {
            paths[loaded + i] = strdup(rows[i].path);
            if (!paths[loaded + i]) {
                for (int j = 0; j < loaded + i; j++) free(paths[j]);
                free(paths);
                return NULL;
            }
        }
        loaded += got;
        if (got < 64) break;
    }
    if (loaded == 0) {
        free(paths);
        return NULL;
    }
    *out_count = loaded;
    return paths;
}

char ** gui_plugin_get_album_tracks(const char * artist, const char * album, int * out_count) {
    *out_count = 0;
    if (!artist || !artist[0] || !album || !album[0]) return NULL;

    int64_t count64 = metadata_db_count_albums_for_group(METADATA_DB_GROUP_ARTIST, artist);
    if (count64 <= 0 || count64 > INT_MAX) return NULL;
    int total = (int) count64;

    char matched_album_artist[128] = "";
    int match_count = 0;
    group_row_t rows[32];
    int offset = 0;
    while (offset < total) {
        int want = total - offset;
        if (want > 32) want = 32;
        int got = metadata_db_get_albums_for_group(METADATA_DB_GROUP_ARTIST, artist, offset, want, rows);
        if (got <= 0) break;
        for (int i = 0; i < got; i++) {
            if (strcasecmp(rows[i].name, album) != 0) continue;
            match_count++;
            snprintf(matched_album_artist, sizeof(matched_album_artist), "%s", rows[i].album_artist);
        }
        offset += got;
        if (got < want) break;
    }
    if (match_count != 1) return NULL;

    return load_plugin_album_group_paths(album, matched_album_artist, out_count);
}

char ** gui_plugin_get_next_album_tracks(const char * artist, const char * current_album, int * out_count) {
    *out_count = 0;
    int64_t count64 = metadata_db_count_albums_for_group(METADATA_DB_GROUP_ARTIST, artist);
    if (count64 <= 1 || count64 > INT_MAX) return NULL;
    group_row_t rows[32];
    int offset = 0;
    while (offset < count64) {
        int want = (int) (count64 - offset);
        if (want > 32) want = 32;
        int got = metadata_db_get_albums_for_group(METADATA_DB_GROUP_ARTIST, artist, offset, want, rows);
        if (got <= 0) break;
        for (int i = 0; i < got; i++) {
            if (strcasecmp(rows[i].name, current_album) != 0) continue;
            if (i + 1 < got) return load_plugin_album_group_paths(rows[i + 1].name, rows[i + 1].album_artist, out_count);
            group_row_t next;
            if (metadata_db_get_albums_for_group(METADATA_DB_GROUP_ARTIST, artist, offset + got, 1, &next) == 1)
                return load_plugin_album_group_paths(next.name, next.album_artist, out_count);
            return NULL;
        }
        offset += got;
        if (got < want) break;
    }
    return NULL;
}

/* Plugin library API moved to gui_plugins.c */

/* A stuck read on a corrupted SD card block (confirmed on a real device via
 * /proc/<pid>/task/<tid>/status+wchan showing the main thread parked in
 * __bread_gfp, a kernel block-device read, after the card came back from an
 * unclean unmount) lands the calling thread in an uninterruptible (D-state)
 * kernel wait. That can't be interrupted by a signal, alarm(), or
 * pthread_cancel() -- the only way to bound it is to run the risky call on
 * its own throwaway thread and simply stop waiting on it if it doesn't
 * finish in time. On timeout, the thread and its heap-allocated work struct
 * below are deliberately never joined or freed: the worker may still be
 * blocked in the kernel indefinitely, joining it would just reintroduce the
 * exact hang this exists to avoid, and freeing memory it might still write
 * to would be a use-after-free. This leaks one thread and one small
 * allocation per genuinely stuck path for the life of the process, which
 * only happens on real filesystem corruption -- a bounded cost, and far
 * better than the whole UI freezing. */
/* Library scan moved to gui_library.c */

/* Search bindings and the All Songs, Recently Added, and grouped-song
 * implementations moved to gui_library.c. */


/* ---- Subsonic-compatible network streaming ----
 *
 * mp3/flac songs (the two decoders audio.c can open against a true network
 * stream -- see decoder_open()'s own comment in audio.c) play directly off
 * the server via subsonic_song_row_click_cb() below, no local file at all.
 * Every other format (aac/ogg/wma/m4a/ape/opus/wav/aiff/dsf -- whatever
 * this server's own transcoding settings hand back) still downloads the
 * whole track first via the mechanism right below, before handing it to the
 * existing local-file-only decoder for that format: retrofitting every
 * remaining decoder here (the vendored AIFF/DSD/AAC/ALAC/APE ones, all
 * built around a plain seekable FILE*) for streaming reads is a much bigger
 * project than this round's scope. The downloaded copy lands on the SD card
 * (SUBSONIC_STREAM_CACHE_DIR below), not /tmp -- see that macro's own
 * comment for why. */


/* Background download thread + a "done" flag polled from update_timer_cb --
 * the same shape as audio.c's own track_finished flag, for the same reason:
 * LVGL isn't thread-safe, so the thread can't touch any screen/widget
 * state directly. */
static volatile bool download_success_flag = false;

typedef struct {
    char url[1536];
    char dest_path[512];
    bool verify_tls;
} download_request_t;




/* DLNA/UPnP-AV cast playback:
 * When a cast track is downloaded and ready, hand it to on_file_selected()
 * for local playback using the file's embedded metadata tags. */
static void poll_dlna_control(void) {
    char path[512], title[256], artist[256], album[256];
    if (dlna_control_consume_ready_track(path, sizeof(path), title, sizeof(title),
                                          artist, sizeof(artist), album, sizeof(album))) {
        char ** playlist = malloc(sizeof(char *));
        playlist[0] = strdup(path);
        clear_player_source(); /* cast from another device -- no on-device list to go back to */
        on_file_selected(playlist, 1, 0);
    }
    if (dlna_control_consume_stop_requested()) {
        audio_stop();
        set_play_button_state(false);
        plugin_manager_notify_stopped();
    }
    dlna_control_notify_status(audio_is_playing(), audio_is_paused());
}



static lv_obj_t * build_stream_media_screen(void) {
    static icon_grid_item_t items[1 + PLUGIN_MAX_STREAM_TILES];
    items[0] = (icon_grid_item_t){ "submenu/subsonic.png", NULL, "Subsonic", subsonic_tile_cb, NULL };

    int count = 1;
    int plugin_count = plugin_manager_get_stream_tile_count();
    for (int i = 0; i < plugin_count && i < PLUGIN_MAX_STREAM_TILES; i++) {
        items[count++] = (icon_grid_item_t){
            plugin_manager_get_stream_tile_icon(i), plugin_manager_get_stream_tile_icon_selected(i),
            plugin_manager_get_stream_tile_label(i), plugin_stream_tile_click_cb, (void *) (intptr_t) i
        };
    }

    lv_obj_t * scr = build_category_menu_screen("Stream Media", generic_back_cb, items, count,
                                                &launcher_layout_config.stream_media);
    finalize_screen_navigation(scr);
    return scr;
}

/* build_stream_media_screen() is static to this file, so gui_reload.c's
 * in-process UI reload (which needs to tear down and rebuild the same
 * screen gui_init() below builds once at boot) goes through these two
 * wrappers instead of calling it directly. */
void gui_stream_media_teardown(void) {
    if (stream_media_screen) {
        lv_obj_delete(stream_media_screen);
        stream_media_screen = NULL;
    }
}

void gui_stream_media_rebuild(void) {
    stream_media_screen = build_stream_media_screen();
}

void gui_stream_media_refresh(void) {
    lv_obj_t * old = stream_media_screen;
    lv_obj_t * fresh = build_stream_media_screen();
    if (!fresh) return;
    stream_media_screen = fresh;
    gui_navigation_replace_static_screen(2, old, fresh);
    if (old) lv_obj_delete(old);
}

/* Keep migration's first scan deferred until screen setup has completed.
 * This uses the same one-shot timer shape as fallback_font.c's deferred
 * load because it cannot start synchronously during the cache load. */
static void migration_rescan_timer_cb(lv_timer_t * timer) {
    lv_timer_delete(timer);
    start_library_auto_rescan();
}

#define MIGRATION_RESCAN_DELAY_MS 500
static void migration_schedule_deferred_rescan(void) {
    lv_timer_create(migration_rescan_timer_cb, MIGRATION_RESCAN_DELAY_MS, NULL);
}

void gui_init(uint32_t screen_width, uint32_t screen_height) {
#ifndef HOST_BUILD
    boot_checkpoint("gui_init entered");
#endif
    settings_load(&current_settings);
    bt_control_restore_codec_preference(current_settings.bt_codec);
    bt_control_set_speexrate_enabled(current_settings.bt_speexrate_enabled);
    bt_control_set_dac_all_codecs(current_settings.dev_bt_dac_all_codecs);
    gui_library_set_covers_during_playback(current_settings.dev_covers_during_playback);
    bt_control_set_sample_rate(current_settings.bt_sample_rate);
    db_log_set_enabled(current_settings.db_logging_enabled);
    hw_buttons_set_screenshot_combo_enabled(current_settings.screenshot_combo_enabled);
    usb_dac_bridge_set_debug_log_enabled(current_settings.db_logging_enabled);
    headphone_status_refresh_earpods_adc();
    app_clock_init(current_settings.clock_automatic, current_settings.clock_manual_epoch,
                   current_settings.clock_system_reference);
#ifndef HOST_BUILD
    boot_checkpoint("settings_load done");
#endif
    /* Must run before anything could turn Wi-Fi/Bluetooth on and trigger
     * wifi_on.sh/bt_init's own one-time read of the file this bind-mounts
     * over -- see hostname_apply()'s own comment. */
    gui_theme_init();
    gui_notifications_init();
    hostname_apply(current_settings.hostname);

    /* Must run before any screen below captures a gui_theme_font(GUI_FONT_ROLE_SUBTEXT)/20/22/28
     * pointer into its own style -- see this function's own doc comment. */

    /* Correct the persisted USB mode against live gadget state before
     * anything else reads it (external_dac_block_reason() in particular --
     * a stale usb_mode==USB_MODE_DAC left over from a previous run would
     * otherwise wrongly block local playback from the moment the app
     * starts). Hardware USB gadget state doesn't survive a reboot, but this
     * binary can also be killed and relaunched without a reboot (as it was
     * repeatedly during development), which is exactly when the persisted
     * value and reality can disagree. */
    usb_mode_t detected_usb_mode;
    bool detected = usb_mode_control_detect_current(&detected_usb_mode);
    if (detected) current_settings.usb_mode = (int) detected_usb_mode;

    /* ADB is the one mode that survives a restart, because enabling it means
     * going into Developer Options and asking for it. Anything else starts as
     * Storage: DAC in particular would otherwise block local playback on a
     * device that may not even be plugged into anything.
     *
     * Applied through the ordinary switch path rather than inline --
     * usb_mode_control_apply() spends seconds in settle and retry loops, which
     * would stall startup if it ran on this thread. poll_usb_mode_switch()
     * completes it in the background. */
    if (!detected) {
        if (current_settings.usb_mode == (int) USB_MODE_ADB) {
            start_usb_mode_switch(USB_MODE_ADB);
        } else if (current_settings.usb_mode != (int) USB_MODE_STORAGE) {
            current_settings.usb_mode = (int) USB_MODE_STORAGE;
            settings_save(&current_settings);
        }
    }

    /* current_settings.volume itself is left untouched here even when the
     * fixed-startup path below is taken -- it keeps tracking "last used"
     * independently (still updated normally by volume_slider_event_cb's
     * RELEASED case during the session), so turning startup_volume_fixed_enabled
     * back off later resumes from wherever the slider was really last left,
     * not from stale fixed-mode state. */
    audio_set_volume(current_settings.startup_volume_fixed_enabled
                          ? (float) current_settings.startup_volume_fixed_percent / 100.0f
                          : current_settings.volume); /* picked up below when the volume slider reads audio_get_volume() */
    audio_set_crossfade_enabled(current_settings.crossfade_enabled);

    /* Apply saved brightness level at startup. */
    backlight_set_normal_percent(current_settings.brightness_percent);

    led_control_apply(current_settings.led_indicator_enabled);
    charge_limiter_poll(current_settings.charge_limiter_enabled, true);
    safe_charging_poll(current_settings.safe_charging_enabled, true);
    if (current_settings.timezone[0] != '\0') timezone_apply(current_settings.timezone);

    /* Receiver/server modes are session-only:
     * Never restore AirPlay, DLNA, or Remote Control automatically after a
     * process start to avoid unintentional listener exposure and resource usage.
     * Remote Control can use either Wi-Fi or Bluetooth after explicit activation. */
    bool network_modes_changed = current_settings.wifi_dac_mode_enabled ||
                                 current_settings.dlna_renderer_enabled ||
                                 current_settings.remote_control_enabled;
    current_settings.wifi_dac_mode_enabled = false;
    current_settings.dlna_renderer_enabled = false;
    current_settings.remote_control_enabled = false;
    if (network_modes_changed) settings_save_async(&current_settings);

    /* The quick drawer's own open/close drag tracking is polled from
     * update_timer_cb instead (poll_quick_drawer_drag()) -- see its comment
     * for why event-based approaches (both indev-wide LV_EVENT_PRESSED/
     * PRESSING and per-object LV_EVENT_GESTURE bubbling) proved unreliable
     * here specifically. */

    /* LV_OPA_80, not LV_OPA_COVER -- see apply_accent_color()'s own comment
     * on this same property for why COVER flattens on.png's handle and
     * track into one indistinguishable solid color. Mirrored here since
     * this is the initial setup at boot, before apply_accent_color() might
     * ever run again. */



    /* Discovers plugin rows/tiles by loading and running every .lua file
     * under <SD card>/.plugins/ -- run early, well before Books, Settings,
     * or Stream Media (the current plugin entry points, see build_books_
     * screen()/build_settings_screen()/build_stream_media_screen()) could
     * plausibly be reached. */
    plugin_manager_init();
#ifndef HOST_BUILD
    boot_checkpoint("pre-screen-build setup done");
#endif

    gui_player_init(screen_width, screen_height);
#ifndef HOST_BUILD
    boot_checkpoint("build_player_screen done");
#endif
    gui_lyrics_init();
    /* Converts any pre-existing absolute-path playlist entries (everything
     * written before playlist_files_append()/_create() started writing
     * relative ones) to relative -- see playlist_files_migrate_to_relative()'s
     * own comment. Runs before the first read of any playlist below. Host-only
     * exclusion matches migrate_old_db_if_needed()'s own -- ./music/Playlists
     * locally is dev test fixtures, not real user data. */
#ifndef HOST_BUILD
    playlist_files_migrate_to_relative(PLAYLISTS_DIR);
#endif
    library_load_from_cache_only();
#ifndef HOST_BUILD
    boot_checkpoint("library_load_from_cache_only done");
#endif
    /* Migration keeps its automatic first scan, even if the destination
     * cache reports FRESH. A non-migration FRESH result is handled by the
     * library's settled build prompt. */
    if (metadata_db_migration_needed() && gui_library_auto_rescan_enabled())
        migration_schedule_deferred_rescan();
    /* No whole-library load anywhere in this boot path, on purpose --
     * remote_control.c queries metadata_db.c directly (its own METADATA_DB_
     * GUARD) rather than needing a synced copy of the library, and each of
     * the five build_*_screen() calls above already activates its own
     * paged provider against the DB internally (see build_all_songs_
     * screen()'s own comment), so there's nothing left that would need a
     * whole-library array at boot for any reason, Remote Control enabled
     * or not. */
/* A-Z index & search registered in gui_library_init */


/* files_search and artist_albums initialized in gui_library_init */
    gui_text_input_init();
    stream_media_screen = build_stream_media_screen();


    /* Artists/Albums, unlike the rest of this file's ~25 build_subsonic_
     * list_screen() screens, can genuinely scale with library size --
     * getArtists.view has no size cap and getAlbumList2.view's own "every
     * album" browse returns up to 500 (see subsonic_client.c) -- so these
     * two use the same virtualized compact_list build_all_songs_screen()/
     * build_artists_screen()/etc. already do, not the plain flex list every
     * other (inherently small/bounded) settings/browse screen in this file
     * shares. Built empty (item_count 0); populated later via compact_list_
     * set_items() once a real fetch actually completes (poll_subsonic_
     * connect()/poll_subsonic_browse()), same lazy-population shape as the
     * local library screens. enable_now_playing is false for both -- a
     * Subsonic artist/album row has no local playlist_index to highlight
     * against. */
    gui_subsonic_init();

    /* Artists/Albums, unlike the rest of this file's ~25 build_subsonic_
     * list_screen() screens, can genuinely scale with library size --
     * getArtists.view has no size cap and getAlbumList2.view's own "every
     * album" browse returns up to 500 (see subsonic_client.c) -- so these
     * two use the same virtualized compact_list build_all_songs_screen()/
     * build_artists_screen()/etc. already do, not the plain flex list every
     * other (inherently small/bounded) settings/browse screen in this file
     * shares. Built empty (item_count 0); populated later via compact_list_
     * set_items() once a real fetch actually completes (poll_subsonic_
     * connect()/poll_subsonic_browse()), same lazy-population shape as the
     * local library screens. enable_now_playing is false for both -- a
     * Subsonic artist/album row has no local playlist_index to highlight
     * against. */

    gui_library_init();
    /* Warms the first ALBUM_THUMBNAIL_CACHE_SIZE album thumbnails into RAM
     * (and the rest onto disk) before the user ever opens Albums. */
    gui_library_start_boot_thumbnail_warmup();
    gui_network_init();
    gui_settings_init();
    gui_plugin_manage_init();
    gui_books_init();
    build_power_off_countdown_popup();
    gui_queue_init();
    gui_plugins_init();
    gui_shell_init(screen_width, screen_height);
    gui_navigation_init();
#ifndef HOST_BUILD
    boot_checkpoint("all screens built");
#endif



#ifndef HOST_BUILD
    /* Holds the boot splash (gui_show_boot_splash(), called from main.c
     * before any of this function's own work) on screen for at least
     * BOOT_SPLASH_MIN_DISPLAY_MS of real boot time, piggybacking on
     * whatever this function's own setup (library load, screen building)
     * already consumed -- only adds *additional* wait if that finished
     * faster, so this isn't always a flat tax on boot time. This also
     * delays start_refresh_bt_icon() (called above) by the same margin.
     *
     * Bluetooth readiness is handled asynchronously by gui_shell.c and does
     * not extend this splash minimum. */
    while (boot_splash_start_tick != 0 &&
           lv_tick_get() - boot_splash_start_tick < BOOT_SPLASH_MIN_DISPLAY_MS) {
        uint32_t wait_ms = lv_timer_handler();
        if (wait_ms > 50) wait_ms = 50;
        usleep(wait_ms * 1000);
    }
    boot_checkpoint("boot splash settle wait done");

    /* Reveals the status bar/popups/quick drawer -- see gui_show_boot_
     * splash()'s own comment for why these were hidden in the first place
     * (they all live on lv_layer_top(), which paints above any screen
     * unconditionally, splash included). */
    lv_obj_remove_flag(lv_layer_top(), LV_OBJ_FLAG_HIDDEN);
#endif
    gui_library_boot_ready(lv_tick_get());

    /* gui_shell_get_home_screen() is the permanent root of the nav stack -- nav_pop() never
     * goes past it. Load it first so there's always something valid on
     * screen even before any auto-resume logic below runs. */


    /* Initialize user inactivity baseline at the splash-to-Home transition,
     * start runtime update timer, and install gesture indev hooks. */
    gui_reset_interactive_timeout_baseline();
    lv_timer_create(power_button_timer_cb, 25, NULL);
    lv_timer_create(update_timer_cb, 500, NULL);
    lv_indev_t * gesture_indev = find_pointer_indev();
    gui_shell_install_indev_hooks(gesture_indev);
#ifndef HOST_BUILD
    boot_checkpoint("lv_screen_load(gui_shell_get_home_screen()) done");
#endif

    /* Auto-resume playback on startup:
     * If an SD card is mounted and contains a saved queue checkpoint, restore it
     * first. Otherwise fall back to Car Mode and the opt-in "Resume Last Track"
     * setting from internal storage / metadata database.
     * Tracks in SUBSONIC_STREAM_CACHE_DIR are skipped to prevent resuming into
     * transient cache files without network connectivity. */
    bool sd_restored = false;
    if (sd_card_root_is_mounted()) {
        sd_restored = gui_player_restore_sd_queue(true);
    }

    if (!sd_restored && current_settings.car_mode_enabled && current_settings.last_track[0] != '\0' &&
        strncmp(current_settings.last_track, SUBSONIC_STREAM_CACHE_DIR, strlen(SUBSONIC_STREAM_CACHE_DIR)) != 0) {
#ifndef HOST_BUILD
        /* Car Mode expects a connected headphone/aux jack to resume into.
         * If no headphone is connected at boot, skip auto-resume and disable
         * Car Mode to prevent unexpected playback or boot issues. */
        if (get_headphone_state() == HEADPHONE_STATE_NONE) {
            current_settings.car_mode_enabled = false;
            settings_save_async(&current_settings);
            show_info_toast("Car Mode disabled: no headphone detected at boot");
        } else
#endif
        {
            char ** resume_playlist;
            int resume_count, resume_index;
            if (build_saved_resume_playlist(&resume_playlist, &resume_count, &resume_index)) {
                if (install_saved_resume_playlist(resume_playlist, resume_count)) {
                    /* play_track_at_from() itself nav_push()es gui_player_get_screen() on top
                     * of the seeded root, so a back-swipe from the resumed player
                     * correctly lands back on the home screen. */
                    play_track_at_from(resume_index, current_settings.last_position);
                }
            }
        }
    } else if (!sd_restored && current_settings.resume_mode != 0 && current_settings.last_track[0] != '\0' &&
               strncmp(current_settings.last_track, SUBSONIC_STREAM_CACHE_DIR, strlen(SUBSONIC_STREAM_CACHE_DIR)) != 0) {
        /* General "Resume Last Track" (Settings -> Playback):
         * Resumes the last played local track on launch without requiring
         * headphone presence. */
        char ** resume_playlist;
        int resume_count, resume_index;
        if (build_saved_resume_playlist(&resume_playlist, &resume_count, &resume_index)) {
            if (install_saved_resume_playlist(resume_playlist, resume_count)) {
                if (current_settings.resume_mode == 2) {
                    /* Do not open ALSA at all until the user presses Play.  On
                     * a headphone-less boot, start-then-pause could lose the
                     * race to an output-open failure and consume this queue. */
                    prepare_deferred_resume(resume_index, current_settings.last_position);
                } else play_track_at_from(resume_index, current_settings.last_position);
            }
        }
    }
#ifndef HOST_BUILD
    boot_checkpoint("gui_init returning");
#endif

    /* Deliberately the very last thing gui_init() does -- see
     * fallback_font_schedule_deferred_load()'s own doc comment for why this
     * can't just load the font directly here or anywhere earlier in this
     * function (the auto-resume path just above is exactly the kind of
     * pre-first-frame call site that hung boot on the previous attempt at
     * this). */
    fallback_font_schedule_deferred_load();
#ifdef TEST_BOOT_RC
    /* The production startup path above deliberately resets session-only
     * modes and persists them as disabled. Re-enable only in memory for this
     * test build; never change the user's saved setting. The service startup
     * does blocking Bluetooth work on its own worker thread. */
    current_settings.remote_control_enabled = true;
    gui_network_start_test_boot_services();
#endif
}


void gui_deinit(void) {
    gui_library_cancel_scan();
    gui_library_cancel_background_work();
    gui_subsonic_cancel_background_work();
    gui_network_cancel_background_work();
    gui_lyrics_cancel_background_work();
    gui_player_cancel_background_work();
    gui_shell_cancel_background_work();
}
