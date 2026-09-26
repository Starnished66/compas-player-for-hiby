extern int subprocess_run(char * const argv[], char ** out_output, int timeout_sec);
#include "gui.h"
#include "gui_network.h"
#include "gui_text_input.h"
#include "audio.h"
#include "plugin_manager.h"
#include "import_web.h"
#include "gui_settings.h"
#include "gui_navigation.h"
#include "gui_shell.h"
#include "screen_builders.h"
#include "fallback_font.h"
#include "settings.h"
#include "assets.h"
#include "device_config.h"
#include "wifi_status.h"
#include "wifi_control.h"
#include "bluetooth_control.h"
#include "usb_mode_control.h"
#include "battery.h"
#include "usb_storage_session.h"
#include "usb_dac_bridge.h"
#include "firmware_update.h"
#include "gui_subsonic.h"
#include "airplay_control.h"
#include "airplay_bridge.h"
#include "airplay_metadata.h"
#include "dlna_control.h"
#include "remote_control.h"
#include "bt_remote_control.h"
#include "debug_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdatomic.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

typedef enum {
    NETWORK_SERVICE_IMPORT,
    NETWORK_SERVICE_REMOTE,
    NETWORK_SERVICE_DLNA,
    NETWORK_SERVICE_COUNT
} network_service_t;

static bool network_service_enqueue(network_service_t service, bool start, bool force_stop);

#ifdef TEST_BOOT_RC
static atomic_bool test_boot_services_started = false;

/* /usr/bin/bt_init creates this only after controller firmware setup. Wait
 * here, off the LVGL thread, so this test path cannot race stock boot's chip
 * initialization. The bounded wait keeps a failed boot from hanging forever. */
static void * test_boot_services_worker(void * arg) {
    (void) arg;
    /* Start Remote Control immediately. Both entry points create their own
     * background workers; the RFCOMM worker waits/retries until BlueZ is
     * available, while the HTTP listener can serve as soon as Wi-Fi is up. */
    remote_control_start();
    bt_remote_control_start();

    /* /usr/bin/bt_init creates this only after controller firmware setup.
     * Wait off the LVGL thread to avoid racing stock boot's chip init. */
    bool init_gate_seen = false;
    for (int i = 0; i < 240; ++i) {
        if (access("/tmp/bt_init_ok", F_OK) == 0) {
            init_gate_seen = true;
            break;
        }
        usleep(500000);
    }
    if (!init_gate_seen) {
        DBG_LOG("test boot RC: Bluetooth initialization gate timed out\n");
        return NULL;
    }

    if (bt_control_init_chip() && !bt_control_is_powered()) bt_control_enable();
    return NULL;
}

#endif

/* Screens owned by this module */
static lv_obj_t * wifi_screen;
static lv_obj_t * wifi_info_screen;
static lv_obj_t * wifi_dns_screen;
static lv_obj_t * bt_screen;
static lv_obj_t * bt_dac_screen;
static lv_obj_t * bt_dac_overlay_screen;
static lv_obj_t * bt_dac_stream_label;
static lv_obj_t * bt_codec_screen;
static lv_obj_t * bt_advanced_screen;
static lv_obj_t * bt_rate_screen;
static lv_obj_t * usb_mode_screen;
static lv_obj_t * usb_dac_overlay_screen;
/* See the mode-switch success path for why this exists rather than reading
 * current_settings.usb_mode. */
static bool usb_dac_mode_active = false;
static lv_obj_t * usb_dac_hint_label;
static lv_obj_t * usb_dac_input_label;
static lv_obj_t * usb_dac_path_label;
static lv_obj_t * import_wifi_screen;
static lv_obj_t * airplay_screen;
static lv_obj_t * airplay_overlay_screen;
static lv_obj_t * airplay_overlay_cover_img;
static lv_obj_t * airplay_overlay_title_label;
static bool airplay_overlay_showing = false;
static lv_image_dsc_t airplay_overlay_cover_dsc;
static uint8_t * airplay_overlay_cover_bytes = NULL;
/* This screen's own stack slot, recorded right before nav_push() -- same
 * import_web_stop_nav_slot pattern used below for Import via Wi-Fi's busy
 * screen. Needed because the overlay can be torn down by session end while
 * the user has already navigated to some other screen on top of it (or
 * underneath it in history); without spawning it, nav_pop() alone would
 * either pop the wrong screen or leave this now-defunct one buried in the
 * back-navigation history for a later "back" to resurface. */
static int airplay_overlay_nav_slot = -1;
static lv_obj_t * dlna_screen;
static lv_obj_t * remote_control_screen;
static lv_obj_t * wireless_screen;
static lv_obj_t * resume_mode_screen;
static lv_obj_t * resume_mode_list;
static lv_obj_t * play_pause_button_mode_screen;
static lv_obj_t * font_size_screen;
static lv_obj_t * replaygain_mode_screen;

/* Externs to gui.c state and functions */
extern player_settings_t current_settings;
extern void nav_push(lv_obj_t * screen);
extern void nav_pop(void);
extern void nav_remove_stack_slot(int depth);
extern void start_library_auto_rescan(void);
extern void finalize_screen_navigation(lv_obj_t * screen);
extern void show_error_toast(const char * msg);
extern void show_info_toast(const char * msg);
extern lv_color_t accent_lv_color(void);
extern lv_obj_t * add_pill_chevron_row(lv_obj_t * list, const char * text, lv_event_cb_t cb);
extern lv_obj_t * add_pill_toggle_row(lv_obj_t * parent, const char * label_text, bool checked, lv_event_cb_t on_click);
extern lv_obj_t * add_pill_row_base(lv_obj_t * list, const char * text);
extern const lv_font_t * gui_theme_font(gui_font_role_t role);
extern void generic_back_cb(lv_event_t * e);
static gui_busy_handle_t wifi_connect_saved_token = 0;
extern gui_busy_handle_t gui_busy_show(const char * title, const char * msg);
extern void gui_busy_hide(gui_busy_handle_t handle);

#ifdef HOST_BUILD
  #define MUSIC_ROOT_DIR "./music"
#else
  #define MUSIC_ROOT_DIR "/data/mnt/sd_0"
#endif


static gui_busy_handle_t wifi_connect_token = 0;
extern bt_device_t bt_scan_results[];
extern int bt_scan_result_count;

/* ---- Wi-Fi settings screen ----
 *
 * Enable/disable toggle at top; everything else (info/manual entry/DNS,
 * memorized networks, available networks) only appears once Wi-Fi is on,
 * rebuilt into the one dynamic wifi_list container by populate_wifi_screen()
 * -- reachable both from the Wireless submenu's Wi-Fi tile and from a
 * long-press on the quick-access drawer's wifi icon (see
 * quick_drawer_wifi_long_press_cb near build_quick_drawer). Scan/connect/
 * disconnect/forget all share the same background-thread-plus-polled-flag
 * shape as the Subsonic connect/download machinery above, and reuse its
 * subsonic_downloading_screen/label as the shared "please wait" interstitial
 * rather than building a second one. */

#define WIFI_MAX_RESULTS 32
#define WIFI_MAX_SAVED 32
static wifi_network_t wifi_scan_results[WIFI_MAX_RESULTS];
static int wifi_scan_result_count = 0;
static wifi_saved_network_t wifi_saved_results[WIFI_MAX_SAVED];
static int wifi_saved_result_count = 0;
static lv_obj_t * wifi_list;
static lv_obj_t * wifi_rescan_btn;
static bool wifi_enable_pending_feedback = false;
static char wifi_connect_pending_ssid[WIFI_MAX_SSID_LEN];

/* Saved-network/configuration reads are deliberately kept separate from the
 * scan buffers.  The worker owns its staging snapshot; the UI copies it only
 * after joining the worker, so no LVGL code and no partially-written strings
 * can cross the thread boundary. */
typedef struct {
    wifi_saved_network_t saved[WIFI_MAX_SAVED];
    int saved_count;
    wifi_info_t info;
    bool info_connected;
    unsigned generation;
} wifi_settings_snapshot_t;

static pthread_t wifi_settings_snapshot_thread;
static bool wifi_settings_snapshot_active = false;
static atomic_bool wifi_settings_snapshot_done_flag = false;
static atomic_uint wifi_settings_snapshot_generation = 1;
static wifi_settings_snapshot_t wifi_settings_snapshot_worker;
static unsigned wifi_settings_snapshot_cached_generation = 0;
static bool wifi_settings_snapshot_cached = false;
static uint32_t wifi_settings_snapshot_tick;
static bool wifi_settings_snapshot_enabled = false;
static wifi_info_t wifi_cached_info;
static bool wifi_cached_info_connected = false;
#ifndef HOST_BUILD
static bool remote_control_last_bt_powered;
static bt_remote_control_status_t remote_control_last_bt_status = BT_REMOTE_CONTROL_DISABLED;
#endif

static void populate_wifi_info_screen(void);
static void populate_import_wifi_screen(void);
static void remote_control_refresh_address(void);
static void remote_control_refresh_pin_label(void);

static void start_wifi_scan(void);
static void wifi_settings_snapshot_invalidate(void);
static void wifi_settings_snapshot_start(void);

static pthread_t wifi_connect_thread;
static bool wifi_connect_active = false;
static atomic_bool wifi_connect_done_flag = false;
static volatile bool wifi_connect_succeeded = false;

typedef struct {
    char ssid[WIFI_MAX_SSID_LEN];
    char password[256];
} wifi_connect_request_t;

static void * wifi_connect_thread_func(void * arg) {
    wifi_connect_request_t * req = (wifi_connect_request_t *) arg;
    bool accepted = wifi_control_connect(req->ssid, req->password[0] != '\0' ? req->password : NULL);
    free(req);

    /* wifi_control_connect() returning true only confirms wpa_cli accepted
     * the configuration, not that association actually happened -- a wrong
     * password fails silently at that layer (see its own doc comment).
     * Poll the real link state for a few seconds before calling this a
     * genuine success, same real-world wait the "Connecting to X..." screen
     * already covers. */
    bool associated = false;
    if (accepted) {
        for (int i = 0; i < 10 && !associated; i++) {
            int level;
            associated = wifi_get_status(&level);
            if (!associated) usleep(500000);
        }
    }
    wifi_connect_succeeded = accepted && associated;
    atomic_store_explicit(&wifi_connect_done_flag, true, memory_order_release); /* written last -- poll_wifi_connect only checks this flag */
    return NULL;
}

static void start_wifi_connect(const char * ssid, const char * password) {
    wifi_connect_request_t * req = malloc(sizeof(*req));
    if (!req) return;
    snprintf(req->ssid, sizeof(req->ssid), "%s", ssid);
    snprintf(req->password, sizeof(req->password), "%s", password ? password : "");

    atomic_store_explicit(&wifi_connect_done_flag, false, memory_order_relaxed);
    wifi_connect_active = true;

    wifi_connect_token = gui_busy_show("Connecting to", ssid);

        if (pthread_create(&wifi_connect_thread, NULL, wifi_connect_thread_func, req) != 0) {
        wifi_connect_active = false;
        free(req);
        gui_busy_hide(wifi_connect_token);
        show_error_toast("Thread launch failed");
    }
}

void poll_wifi_connect(void) {
    if (!wifi_connect_active || !atomic_load_explicit(&wifi_connect_done_flag, memory_order_acquire)) return;

    wifi_connect_active = false;
    pthread_join(wifi_connect_thread, NULL);
    gui_busy_hide(wifi_connect_token);
    if (!wifi_connect_succeeded) {
        show_error_toast("Couldn't connect to Wi-Fi network");
    }
    wifi_settings_snapshot_invalidate();
    wifi_settings_snapshot_start();
    start_wifi_scan(); /* refresh so the list reflects the new connection state */
}

/* Reconnecting to an already-memorized network -- deliberately a separate
 * thread/flag pair from the plain connect above (not a shared "mode" on the
 * same one) rather than reusing wifi_connect_active's state, matching this
 * file's existing convention of one dedicated pair per background Wi-Fi
 * operation (scan/connect/disconnect/forget already each have their own).
 * See wifi_control_connect_saved()'s own doc comment for why this can't
 * just call start_wifi_connect(ssid, NULL) instead. */
static pthread_t wifi_connect_saved_thread;
static bool wifi_connect_saved_active = false;
static atomic_bool wifi_connect_saved_done_flag = false;
static volatile bool wifi_connect_saved_succeeded = false;

typedef struct {
    int id;
} wifi_connect_saved_request_t;

static void * wifi_connect_saved_thread_func(void * arg) {
    wifi_connect_saved_request_t * req = (wifi_connect_saved_request_t *) arg;
    bool accepted = wifi_control_connect_saved(req->id);
    free(req);

    bool associated = false;
    if (accepted) {
        for (int i = 0; i < 10 && !associated; i++) {
            int level;
            associated = wifi_get_status(&level);
            if (!associated) usleep(500000);
        }
    }
    wifi_connect_saved_succeeded = accepted && associated;
    atomic_store_explicit(&wifi_connect_saved_done_flag, true, memory_order_release); /* written last -- poll_wifi_connect_saved only checks this flag */
    return NULL;
}

static void start_wifi_connect_saved(int id, const char * ssid) {
    wifi_connect_saved_request_t * req = malloc(sizeof(*req));
    if (!req) return;
    req->id = id;

    atomic_store_explicit(&wifi_connect_saved_done_flag, false, memory_order_relaxed);
    wifi_connect_saved_active = true;

    wifi_connect_saved_token = gui_busy_show("Connecting to", ssid);

        if (pthread_create(&wifi_connect_saved_thread, NULL, wifi_connect_saved_thread_func, req) != 0) {
        wifi_connect_saved_active = false;
        free(req);
        gui_busy_hide(wifi_connect_saved_token);
        show_error_toast("Thread launch failed");
    }
}

void poll_wifi_connect_saved(void) {
    if (!wifi_connect_saved_active || !atomic_load_explicit(&wifi_connect_saved_done_flag, memory_order_acquire)) return;

    wifi_connect_saved_active = false;
    pthread_join(wifi_connect_saved_thread, NULL);
    gui_busy_hide(wifi_connect_saved_token);
    if (!wifi_connect_saved_succeeded) {
        show_error_toast("Couldn't connect to Wi-Fi network");
    }
    wifi_settings_snapshot_invalidate();
    wifi_settings_snapshot_start();
    start_wifi_scan(); /* refresh so the list reflects the new connection state */
}

static pthread_t wifi_disconnect_thread;
static bool wifi_disconnect_active = false;
static atomic_bool wifi_disconnect_done_flag = false;

static void * wifi_disconnect_thread_func(void * arg) {
    (void) arg;
    wifi_control_disconnect();
    atomic_store_explicit(&wifi_disconnect_done_flag, true, memory_order_release); /* written last -- poll_wifi_disconnect only checks this flag */
    return NULL;
}

static void start_wifi_disconnect(void) {
    atomic_store_explicit(&wifi_disconnect_done_flag, false, memory_order_relaxed);
    wifi_disconnect_active = true;
        if (pthread_create(&wifi_disconnect_thread, NULL, wifi_disconnect_thread_func, NULL) != 0) {
        wifi_disconnect_active = false;
        show_error_toast("Thread launch failed");
    }
}

void poll_wifi_disconnect(void) {
    if (!wifi_disconnect_active || !atomic_load_explicit(&wifi_disconnect_done_flag, memory_order_acquire)) return;
    wifi_disconnect_active = false;
    pthread_join(wifi_disconnect_thread, NULL);
    wifi_settings_snapshot_invalidate();
    wifi_settings_snapshot_start();
    start_wifi_scan(); /* refresh so the list reflects the disconnected state */
}

static void wifi_password_entered_cb(const char * text, void * user_data) {
    (void) user_data;
    start_wifi_connect(wifi_connect_pending_ssid, text);
}

static void wifi_row_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    wifi_network_t * net = &wifi_scan_results[index];
    if (net->is_current) {
        start_wifi_disconnect();
        return;
    }

    snprintf(wifi_connect_pending_ssid, sizeof(wifi_connect_pending_ssid), "%s", net->ssid);
    if (net->secured) {
        show_text_entry("Wi-Fi Password", NULL, true, false, wifi_password_entered_cb, NULL);
    } else {
        start_wifi_connect(net->ssid, NULL);
    }
}

static void wifi_rescan_btn_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    /* An explicit rescan is a new discovery snapshot, not an append to the
     * previous one. Clear stale access points immediately so the screen
     * cannot imply that an old result is still currently visible while the
     * radio is collecting its replacement set. Saved networks remain in
     * their own section because they are configuration, not scan results. */
    wifi_scan_result_count = 0;
    wifi_settings_snapshot_invalidate();
    wifi_settings_snapshot_enabled = true;
    populate_wifi_screen(wifi_control_is_enabled());
    start_wifi_scan();
}

static void * wifi_settings_snapshot_thread_func(void * arg) {
    unsigned generation = (unsigned) (uintptr_t) arg;
    wifi_settings_snapshot_t * snapshot = &wifi_settings_snapshot_worker;

    snapshot->generation = generation;
    snapshot->saved_count = wifi_control_list_saved(snapshot->saved, WIFI_MAX_SAVED);
    snapshot->info_connected = wifi_control_get_info(&snapshot->info);
    atomic_store_explicit(&wifi_settings_snapshot_done_flag, true, memory_order_release);
    return NULL;
}

static void wifi_settings_snapshot_invalidate(void) {
    atomic_fetch_add_explicit(&wifi_settings_snapshot_generation, 1, memory_order_acq_rel);
    wifi_settings_snapshot_cached = false;
    wifi_settings_snapshot_cached_generation = 0;
    wifi_cached_info_connected = false;
    wifi_saved_result_count = 0;
}

static void wifi_settings_snapshot_start(void) {
    if (wifi_settings_snapshot_active || wifi_enable_pending_feedback || !wifi_settings_snapshot_enabled ||
        !gui_shell_wifi_effective_enabled()) return;

    unsigned generation = atomic_load_explicit(&wifi_settings_snapshot_generation, memory_order_acquire);
    if (wifi_settings_snapshot_cached && wifi_settings_snapshot_cached_generation == generation &&
        lv_tick_elaps(wifi_settings_snapshot_tick) < 5000) return;

    atomic_store_explicit(&wifi_settings_snapshot_done_flag, false, memory_order_relaxed);
    wifi_settings_snapshot_active = true;
    if (pthread_create(&wifi_settings_snapshot_thread, NULL, wifi_settings_snapshot_thread_func,
                       (void *) (uintptr_t) generation) != 0) {
        wifi_settings_snapshot_active = false;
    }
}

/* Returns true when a completed snapshot was published.  Rendering is kept
 * in the UI poller, never in the worker, and a generation mismatch discards
 * results collected across an enable/disable transition. */
static bool poll_wifi_settings_snapshot(void) {
    if (!wifi_settings_snapshot_active ||
        !atomic_load_explicit(&wifi_settings_snapshot_done_flag, memory_order_acquire)) return false;

    wifi_settings_snapshot_active = false;
    pthread_join(wifi_settings_snapshot_thread, NULL);

    unsigned generation = atomic_load_explicit(&wifi_settings_snapshot_generation, memory_order_acquire);
    if (wifi_settings_snapshot_worker.generation != generation) {
        wifi_settings_snapshot_start();
        return false;
    }

    wifi_saved_result_count = wifi_settings_snapshot_worker.saved_count;
    memcpy(wifi_saved_results, wifi_settings_snapshot_worker.saved,
           sizeof(wifi_saved_results[0]) * (size_t) wifi_saved_result_count);
    wifi_cached_info = wifi_settings_snapshot_worker.info;
    wifi_cached_info_connected = wifi_settings_snapshot_worker.info_connected;
    wifi_settings_snapshot_cached_generation = generation;
    wifi_settings_snapshot_cached = true;
    wifi_settings_snapshot_tick = lv_tick_get();
    return true;
}

static bool wifi_cached_info_is_current(void) {
    return wifi_settings_snapshot_cached &&
           wifi_settings_snapshot_cached_generation ==
               atomic_load_explicit(&wifi_settings_snapshot_generation, memory_order_acquire);
}

static void set_wifi_rescan_active(bool active) {
    if (!wifi_rescan_btn) return;
    lv_label_set_text(wifi_rescan_btn, active ? "Scanning..." : "Rescan");
    lv_obj_set_style_text_color(wifi_rescan_btn,
                                active ? lv_color_make(160, 160, 160) : accent_lv_color(), 0);
    align_screen_header_action(wifi_rescan_btn, 20);
    if (active) {
        lv_obj_remove_flag(wifi_rescan_btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_state(wifi_rescan_btn, LV_STATE_DISABLED);
    } else {
        lv_obj_clear_state(wifi_rescan_btn, LV_STATE_DISABLED);
        lv_obj_add_flag(wifi_rescan_btn, LV_OBJ_FLAG_CLICKABLE);
    }
}

static pthread_t wifi_scan_thread;
static bool wifi_scan_active = false;
static atomic_bool wifi_scan_done_flag = false;

static void * wifi_scan_thread_func(void * arg) {
    (void) arg;
    /* No auto-enable here anymore -- the screen's own toggle row is now the
     * explicit way to turn Wi-Fi on, so a stray Rescan tap while it's off
     * should just find nothing rather than silently flipping the radio on
     * behind the toggle's back. */
    wifi_control_scan_start();
    sleep(3); /* wpa_cli's scan is async -- give the radio time before reading scan_results */
    wifi_scan_result_count = wifi_control_get_results(wifi_scan_results, WIFI_MAX_RESULTS);
    atomic_store_explicit(&wifi_scan_done_flag, true, memory_order_release); /* written last -- poll_wifi_scan only checks this flag */
    return NULL;
}

/* Runs fully in the background, same as the stock player -- no busy
 * screen, no navigation at all. The list just updates in place
 * (populate_wifi_screen(), below) once the scan finishes, whichever screen
 * the user happens to be on by then. An earlier version pushed a
 * "Scanning for networks..." interstitial shared with bt scan/the bt
 * toggle's own overlay -- see git history if that's ever needed again, but
 * it was also the source of a real, repeatedly-hit stuck-screen bug
 * (multiple uncoordinated users of one shared overlay, plus the overlay
 * getting buried under further navigation), which not having an overlay at
 * all sidesteps entirely. */
static void start_wifi_scan(void) {
    if (wifi_scan_active) return;

    atomic_store_explicit(&wifi_scan_done_flag, false, memory_order_relaxed);
    wifi_scan_active = true;
    set_wifi_rescan_active(true);

    if (pthread_create(&wifi_scan_thread, NULL, wifi_scan_thread_func, NULL) != 0) {
        wifi_scan_active = false;
        set_wifi_rescan_active(false);
    }
}

void poll_wifi_scan(void) {
    if (wifi_settings_snapshot_enabled && !gui_shell_wifi_effective_enabled()) {
        wifi_settings_snapshot_enabled = false;
        wifi_settings_snapshot_invalidate();
    }
    bool snapshot_published = poll_wifi_settings_snapshot();
    bool scan_published = false;
    if (wifi_scan_active && atomic_load_explicit(&wifi_scan_done_flag, memory_order_acquire)) {
        wifi_scan_active = false;
        pthread_join(wifi_scan_thread, NULL);
        set_wifi_rescan_active(false);
        scan_published = true;
    }

    if ((snapshot_published || scan_published) &&
        wifi_screen && gui_navigation_is_top(wifi_screen))
        populate_wifi_screen(wifi_control_is_enabled());

    if (snapshot_published) {
        if (wifi_info_screen && gui_navigation_is_top(wifi_info_screen)) populate_wifi_info_screen();
        if (import_wifi_screen && gui_navigation_is_top(import_wifi_screen)) populate_import_wifi_screen();
        if (remote_control_screen && gui_navigation_is_top(remote_control_screen)) remote_control_refresh_address();
    }
    if (remote_control_screen && gui_navigation_is_top(remote_control_screen))
        remote_control_refresh_pin_label();
#ifndef HOST_BUILD
    /* These snapshots are updated by the service thread / regular shell poll;
     * never query bluetoothd from the UI thread. */
    bt_remote_control_status_t bt_status = bt_remote_control_status();
    if (remote_control_screen && gui_navigation_is_top(remote_control_screen) &&
        (remote_control_last_bt_powered != bt_is_powered_cached ||
         remote_control_last_bt_status != bt_status)) {
        remote_control_last_bt_powered = bt_is_powered_cached;
        remote_control_last_bt_status = bt_status;
        remote_control_refresh_address();
    }
#endif
    if ((wifi_screen && gui_navigation_is_top(wifi_screen)) ||
        (wifi_info_screen && gui_navigation_is_top(wifi_info_screen)) ||
        (import_wifi_screen && gui_navigation_is_top(import_wifi_screen)) ||
        (remote_control_screen && gui_navigation_is_top(remote_control_screen)))
        wifi_settings_snapshot_start();
}

static void show_wifi_toggle_pending_async(void * user_data) {
    bool enabled = (bool) (intptr_t) user_data;
    if (!gui_navigation_is_top(gui_network_get_wifi_screen())) return;

    wifi_enable_pending_feedback = enabled;
    wifi_settings_snapshot_invalidate();
    wifi_settings_snapshot_enabled = enabled;
    if (enabled) wifi_scan_result_count = 0;
    populate_wifi_screen(enabled);
    set_wifi_rescan_active(enabled || wifi_scan_active);
}

void gui_network_show_wifi_toggle_pending(bool enabled) {
    /* Rebuilding wifi_list synchronously from its toggle row callback would
     * delete LVGL's active event target. Defer by one UI turn, matching the
     * safe pattern used elsewhere for destructive screen rebuilds. */
    lv_async_call(show_wifi_toggle_pending_async, (void *) (intptr_t) enabled);
}

void gui_network_wifi_toggle_completed(bool enabled) {
    wifi_enable_pending_feedback = false;
    wifi_settings_snapshot_invalidate();
    wifi_settings_snapshot_enabled = enabled;
    if (enabled) wifi_settings_snapshot_start();
    if (!gui_navigation_is_top(gui_network_get_wifi_screen())) {
        set_wifi_rescan_active(wifi_scan_active);
        return;
    }

    populate_wifi_screen(enabled);
    if (enabled) start_wifi_scan();
    else set_wifi_rescan_active(wifi_scan_active);
}

static pthread_t wifi_forget_thread;
static bool wifi_forget_active = false;
static atomic_bool wifi_forget_done_flag = false;

typedef struct {
    int id;
} wifi_forget_request_t;

static void * wifi_forget_thread_func(void * arg) {
    wifi_forget_request_t * req = (wifi_forget_request_t *) arg;
    wifi_control_forget(req->id);
    free(req);
    atomic_store_explicit(&wifi_forget_done_flag, true, memory_order_release); /* written last -- poll_wifi_forget only checks this flag */
    return NULL;
}

static void start_wifi_forget(int id) {
    wifi_forget_request_t * req = malloc(sizeof(*req));
    if (!req) return;
    req->id = id;
    atomic_store_explicit(&wifi_forget_done_flag, false, memory_order_relaxed);
    wifi_forget_active = true;
        if (pthread_create(&wifi_forget_thread, NULL, wifi_forget_thread_func, req) != 0) {
        wifi_forget_active = false;
        free(req);
    }
}

void poll_wifi_forget(void) {
    if (!wifi_forget_active || !atomic_load_explicit(&wifi_forget_done_flag, memory_order_acquire)) return;
    wifi_forget_active = false;
    pthread_join(wifi_forget_thread, NULL);
    wifi_settings_snapshot_invalidate();
    wifi_settings_snapshot_start();
    start_wifi_scan(); /* refresh both memorized and available sections */
}

/* ---- Wi-Fi saved-network action popup ------------------------------------
 * Presents options to Connect or Forget for a selected memorized network,
 * matching bt_action_popup's overlay and backdrop pattern. */
static gui_popup_t wifi_action_popup;
static lv_obj_t * wifi_action_popup_title;
static int wifi_action_popup_network_index = -1;

static void hide_wifi_action_popup(void) {
    gui_popup_hide(&wifi_action_popup);
    wifi_action_popup_network_index = -1;
}

static void wifi_action_popup_backdrop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_wifi_action_popup();
}

static void wifi_action_connect_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (wifi_action_popup_network_index < 0 || wifi_action_popup_network_index >= wifi_saved_result_count) {
        hide_wifi_action_popup();
        return;
    }
    wifi_saved_network_t * net = &wifi_saved_results[wifi_action_popup_network_index];
    int id = net->id;
    char ssid[WIFI_MAX_SSID_LEN];
    snprintf(ssid, sizeof(ssid), "%s", net->ssid);
    hide_wifi_action_popup();
    start_wifi_connect_saved(id, ssid);
}

static void wifi_action_forget_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (wifi_action_popup_network_index < 0 || wifi_action_popup_network_index >= wifi_saved_result_count) {
        hide_wifi_action_popup();
        return;
    }
    int id = wifi_saved_results[wifi_action_popup_network_index].id;
    hide_wifi_action_popup();
    start_wifi_forget(id);
}

void build_wifi_action_popup(void) {
    wifi_action_popup.popup = build_confirm_popup("", LV_LABEL_LONG_DOT, &wifi_action_popup_title, NULL, "Connect",
                                                   accent_lv_color(), wifi_action_connect_cb, NULL, "Forget",
                                                   lv_color_make(255, 120, 120), wifi_action_forget_cb, NULL,
                                                   wifi_action_popup_backdrop_cb, &wifi_action_popup.backdrop);
}

static void show_wifi_action_popup(int index) {
    wifi_action_popup_network_index = index;
    lv_label_set_text(wifi_action_popup_title, wifi_saved_results[index].ssid);
    gui_popup_show(&wifi_action_popup);
}

static void wifi_saved_row_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    show_wifi_action_popup(index);
}

/* ---- Wi-Fi Info screen (read-only) ---- */

static lv_obj_t * wifi_info_list;

static void populate_wifi_info_screen(void) {
    lv_obj_clean(wifi_info_list);

    wifi_settings_snapshot_start();
    if (!wifi_cached_info_is_current() || !wifi_cached_info_connected) {
        lv_obj_t * label = lv_label_create(wifi_info_list);
        lv_label_set_text(label, wifi_settings_snapshot_active ? "Loading..." : "Not connected");
        lv_obj_add_style(label, &style_theme_text_muted, 0);
        lv_obj_set_style_pad_left(label, BOARD_SCALE_PX(24), 0);
        return;
    }

    wifi_info_t info = wifi_cached_info;
    static const char * signal_labels[] = { "Weak", "Fair", "Good", "Excellent" };
    int signal_index = (info.signal_level >= 0 && info.signal_level <= 3) ? info.signal_level : 0;
    char lines[5][80];
    snprintf(lines[0], sizeof(lines[0]), "SSID: %s", info.ssid);
    snprintf(lines[1], sizeof(lines[1]), "IP Address: %s", info.ip[0] ? info.ip : "-");
    snprintf(lines[2], sizeof(lines[2]), "Gateway: %s", info.gateway[0] ? info.gateway : "-");
    snprintf(lines[3], sizeof(lines[3]), "MAC Address: %s", info.mac[0] ? info.mac : "-");
    snprintf(lines[4], sizeof(lines[4]), "Signal: %s", signal_labels[signal_index]);

    for (int i = 0; i < 5; i++) {
        lv_obj_t * label = lv_label_create(wifi_info_list);
        lv_label_set_text(label, lines[i]);
        lv_obj_add_style(label, &style_theme_text_primary, 0);
        lv_obj_set_style_text_font(label, &LIST_ROW_FONT, 0);
        lv_obj_set_style_pad_left(label, BOARD_SCALE_PX(24), 0);
        lv_obj_set_style_pad_top(label, BOARD_SCALE_PX(12), 0);
    }
}

static lv_obj_t * build_wifi_info_screen(void) {
    lv_obj_t * title_label; /* unused after build -- title never changes */
    return build_subsonic_list_screen("Wi-Fi Info", &title_label, &wifi_info_list);
}

static void open_wifi_info_screen(void) {
    wifi_settings_snapshot_invalidate();
    wifi_settings_snapshot_enabled = true;
    populate_wifi_info_screen();
    nav_push(wifi_info_screen);
}

static void wifi_info_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    open_wifi_info_screen();
}

/* ---- Manual SSID entry -- chains the same show_text_entry() flow scan
 * results already use for a secured network's password, just starting from
 * a typed SSID instead of a tapped scan row. An empty password submission
 * connects as an open network (start_wifi_connect() already treats an empty
 * password as "no password"), so there's no separate "is this secured?"
 * question to ask up front. ---- */

static void wifi_manual_ssid_entered_cb(const char * text, void * user_data) {
    (void) user_data;
    snprintf(wifi_connect_pending_ssid, sizeof(wifi_connect_pending_ssid), "%s", text);
    show_text_entry("Wi-Fi Password", NULL, true, false, wifi_password_entered_cb, NULL);
}

static void wifi_manual_entry_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    show_text_entry("Network Name (SSID)", NULL, false, false, wifi_manual_ssid_entered_cb, NULL);
}

/* ---- DNS settings screen -- view/edit the 2 servers currently in
 * /etc/resolv.conf (see wifi_control_get_dns()'s own doc comment for why
 * this is a "until the next DHCP renewal" override, not a permanent one).
 * ---- */

static lv_obj_t * wifi_dns_list;
static char wifi_dns_servers[2][16];
static int wifi_dns_server_count = 0;

static void wifi_dns_row_cb(lv_event_t * e);

static void populate_wifi_dns_screen(void) {
    lv_obj_clean(wifi_dns_list);

    char current[2][16];
    int count = wifi_control_get_dns(current, 2);
    for (int i = 0; i < 2; i++) {
        snprintf(wifi_dns_servers[i], sizeof(wifi_dns_servers[i]), "%s", i < count ? current[i] : "");
    }
    wifi_dns_server_count = count;

    static const char * slot_labels[2] = { "Primary DNS", "Secondary DNS" };
    for (int i = 0; i < 2; i++) {
        char text[64];
        snprintf(text, sizeof(text), "%s: %s", slot_labels[i], wifi_dns_servers[i][0] ? wifi_dns_servers[i] : "Not set");
        lv_obj_t * row = add_pill_chevron_row(wifi_dns_list, text, NULL);
        lv_obj_add_event_cb(row, wifi_dns_row_cb, LV_EVENT_CLICKED, (void *) (intptr_t) i);
    }
}

static void wifi_dns_entered_cb(const char * text, void * user_data) {
    int slot = (int) (intptr_t) user_data;
    snprintf(wifi_dns_servers[slot], sizeof(wifi_dns_servers[slot]), "%s", text);
    if (slot + 1 > wifi_dns_server_count) wifi_dns_server_count = slot + 1;

    const char * servers[2] = { wifi_dns_servers[0], wifi_dns_servers[1] };
    wifi_control_set_dns(servers, wifi_dns_server_count);
    populate_wifi_dns_screen();
}

static void wifi_dns_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int slot = (int) (intptr_t) lv_event_get_user_data(e);
    show_text_entry(slot == 0 ? "Primary DNS" : "Secondary DNS", wifi_dns_servers[slot], false, false, wifi_dns_entered_cb,
                    (void *) (intptr_t) slot);
}

static lv_obj_t * build_wifi_dns_screen(void) {
    lv_obj_t * title_label; /* unused after build -- title never changes */
    return build_subsonic_list_screen("DNS Settings", &title_label, &wifi_dns_list);
}

static void open_wifi_dns_screen(void) {
    populate_wifi_dns_screen();
    nav_push(wifi_dns_screen);
}

static void wifi_dns_settings_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    open_wifi_dns_screen();
}

/* ---- Master rebuild for the Wi-Fi screen's one dynamic list: toggle row,
 * then (only while enabled) the info/manual-entry/DNS rows, memorized
 * networks, and available networks -- called after every state change
 * (toggle, scan, connect, disconnect, forget) rather than patched
 * incrementally, same full-rebuild convention as every other list in this
 * file. ---- */
/* enabled is passed in rather than read via wifi_control_is_enabled()
 * internally so the tap-to-toggle handler (quick_drawer_wifi_event_cb) can
 * pass its own optimistic target state instead of the real (not-yet-caught-
 * up) one -- see that function's own comment. Every other caller just
 * passes the real wifi_control_is_enabled() value, same as before this was
 * a parameter. */
void populate_wifi_screen(bool enabled) {
    lv_obj_clean(wifi_list);
    wifi_settings_snapshot_enabled = enabled;

    if (wifi_rescan_btn) {
        if (enabled) lv_obj_remove_flag(wifi_rescan_btn, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(wifi_rescan_btn, LV_OBJ_FLAG_HIDDEN);
    }

    /* quick_drawer_wifi_event_cb is the SAME real toggle-thread trigger the
     * drawer's own wifi icon uses -- no drawer-specific logic in it, so
     * it's reused directly here rather than duplicating the thread-kickoff
     * boilerplate. poll_wifi_toggle() repopulates this screen once the
     * toggle completes. */
    add_pill_toggle_row(wifi_list, "Wi-Fi", enabled, quick_drawer_wifi_event_cb);

    if (!enabled) return;

    wifi_settings_snapshot_start();

    add_pill_chevron_row(wifi_list, "Wi-Fi Info", wifi_info_row_cb);
    add_pill_chevron_row(wifi_list, "Manual SSID Entry", wifi_manual_entry_row_cb);
    add_pill_chevron_row(wifi_list, "DNS Settings", wifi_dns_settings_row_cb);

    add_section_header(wifi_list, "Memorized Networks");
    /* During an optimistic cold enable there is no wpa_supplicant control
     * socket yet. Keep the last cached saved-network rows instead of
     * launching synchronous wpa_cli subprocesses that cannot succeed and
     * would undermine the immediate feedback this state exists to provide. */
    bool have_settings_snapshot = wifi_cached_info_is_current();
    if (wifi_saved_result_count == 0) {
        lv_obj_t * label = lv_label_create(wifi_list);
        lv_label_set_text(label, have_settings_snapshot ? "No memorized networks" : "Loading Wi-Fi settings...");
        lv_obj_add_style(label, &style_theme_text_muted, 0);
        lv_obj_set_style_pad_left(label, BOARD_SCALE_PX(24), 0);
    }
    /* Cross-reference wpa_cli status against saved network SSIDs to indicate
     * which saved network is currently connected. */
    bool wifi_currently_connected = have_settings_snapshot && wifi_cached_info_connected;
    for (int i = 0; i < wifi_saved_result_count; i++) {
        lv_obj_t * row = lv_obj_create(wifi_list);
        lv_obj_set_size(row, LIST_ROW_WIDTH, LIST_ROW_HEIGHT);
        lv_obj_set_style_radius(row, LIST_ROW_RADIUS, 0);
        lv_obj_set_style_bg_color(row, LIST_ROW_BG_COLOR, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        bool is_connected = wifi_currently_connected && strcmp(wifi_saved_results[i].ssid, wifi_cached_info.ssid) == 0;
        char text[WIFI_MAX_SSID_LEN + 20];
        snprintf(text, sizeof(text), "%s%s", wifi_saved_results[i].ssid, is_connected ? "  - Connected" : "");

        lv_obj_t * label = lv_label_create(row);
        lv_label_set_text(label, text);
        lv_obj_add_style(label, &style_theme_text_primary, 0);
        lv_obj_set_style_text_font(label, &LIST_ROW_FONT, 0);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, LIST_ROW_LABEL_INSET, 0);

        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, wifi_saved_row_click_cb, LV_EVENT_CLICKED, (void *) (intptr_t) i);
    }

    add_section_header(wifi_list, "Available Networks");
    for (int i = 0; i < wifi_scan_result_count; i++) {
        wifi_network_t * net = &wifi_scan_results[i];

        lv_obj_t * row = lv_obj_create(wifi_list);
        lv_obj_set_size(row, LIST_ROW_WIDTH, LIST_ROW_HEIGHT);
        lv_obj_set_style_radius(row, LIST_ROW_RADIUS, 0);
        lv_obj_set_style_bg_color(row, LIST_ROW_BG_COLOR, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t * icon = lv_image_create(row);
        char icon_path[40];
        snprintf(icon_path, sizeof(icon_path), "topbar/wifi_connect_%d.png", net->signal_level);
        lv_image_set_src(icon, asset_path(icon_path));
        lv_obj_align(icon, LV_ALIGN_LEFT_MID, BOARD_SCALE_PX(20), 0);

        lv_obj_t * label = lv_label_create(row);
        char text[256];
        snprintf(text, sizeof(text), "%.128s%s%s", net->ssid, net->secured ? "  (secured)" : "",
                 net->is_current ? "  - Connected" : "");
        lv_label_set_text(label, text);
        lv_obj_add_style(label, &style_theme_text_primary, 0);
        lv_obj_set_style_text_font(label, &LIST_ROW_FONT, 0);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, BOARD_SCALE_PX(64), 0);

        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, wifi_row_click_cb, LV_EVENT_CLICKED, (void *) (intptr_t) i);
    }
    if (wifi_scan_result_count == 0) {
        lv_obj_t * label = lv_label_create(wifi_list);
        lv_label_set_text(label, wifi_enable_pending_feedback ? "Scanning for networks..." : "No networks found");
        lv_obj_add_style(label, &style_theme_text_muted, 0);
        lv_obj_set_style_pad_left(label, BOARD_SCALE_PX(24), 0);
    }
}

static lv_obj_t * build_wifi_screen(void) {
    lv_obj_t * title_label; /* unused after build -- title never changes */
    lv_obj_t * scr = build_subsonic_list_screen("Wi-Fi", &title_label, &wifi_list);

    wifi_rescan_btn = lv_label_create(scr);
    lv_label_set_text(wifi_rescan_btn, "Rescan");
    lv_obj_set_style_text_color(wifi_rescan_btn, accent_lv_color(), 0);
    lv_obj_set_style_text_font(wifi_rescan_btn, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    align_screen_header_action(wifi_rescan_btn, 20);
    lv_obj_add_flag(wifi_rescan_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(wifi_rescan_btn, wifi_rescan_btn_cb, LV_EVENT_CLICKED, NULL);

    return scr;
}

void open_wifi_screen(void) {
    bool enabled = wifi_control_is_enabled();
    wifi_settings_snapshot_invalidate();
    populate_wifi_screen(enabled);
    nav_push(wifi_screen);
    if (enabled) start_wifi_scan(); /* auto-refresh -- matches the old always-scan-on-open behavior, but only when there's a radio to scan with */
}

static void wifi_tile_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    open_wifi_screen();
}

/* ---- Bluetooth settings screen ----
 *
 * Same shape as the Wi-Fi screen above: full scan + pair/connect, reachable
 * from the Wireless submenu's Bluetooth tile and from a long-press on the
 * drawer's bt icon. bt_control_scan() blocks for its whole scan window
 * itself (bluetoothctl's own --timeout), so the scan thread has no separate
 * sleep to add, unlike wifi_scan_thread_func above. */

static lv_obj_t * bt_list;

static void start_bt_scan(void);

static pthread_t bt_connect_thread;
static bool bt_connect_active = false;
static atomic_bool bt_connect_done_flag = false;
static volatile bool bt_connect_succeeded = false;

/* Which device's row (see add_bt_device_row()) should show inline
 * "Connecting..."/"Failed to connect" status -- an earlier version of this
 * blocked the whole screen with a "Pairing & connecting..." overlay
 * instead, same class of issue as the scan/toggle overlays removed
 * elsewhere in this file (see start_wifi_scan()'s comment): running fully
 * in the background with inline per-row feedback, matching the stock
 * player, means there's no screen to get stuck on at all. */
static char bt_connecting_mac[18] = "";
static char bt_connect_failed_mac[18] = "";

typedef struct {
    char mac[18];
} bt_connect_request_t;

static void * bt_connect_thread_func(void * arg) {
    bt_connect_request_t * req = (bt_connect_request_t *) arg;
    bt_connect_succeeded = bt_control_connect(req->mac);
    free(req);
    atomic_store_explicit(&bt_connect_done_flag, true, memory_order_release); /* written last -- poll_bt_connect only checks this flag */
    return NULL;
}

static void start_bt_connect(const char * mac) {
    if (bt_connect_active) return; /* one at a time -- bt_connect_thread/succeeded are single shared slots */

    bt_connect_request_t * req = malloc(sizeof(*req));
    if (!req) return;
    snprintf(req->mac, sizeof(req->mac), "%s", mac);

    atomic_store_explicit(&bt_connect_done_flag, false, memory_order_relaxed);
    bt_connect_active = true;
    snprintf(bt_connecting_mac, sizeof(bt_connecting_mac), "%s", mac);
    bt_connect_failed_mac[0] = '\0';
    populate_bt_screen(); /* immediate inline "Connecting..." on that row */

        if (pthread_create(&bt_connect_thread, NULL, bt_connect_thread_func, req) != 0) {
        bt_connect_active = false;
        free(req);
        if (bt_list) lv_obj_invalidate(bt_list);
    }
}

void poll_bt_connect(void) {
    if (!bt_connect_active || !atomic_load_explicit(&bt_connect_done_flag, memory_order_acquire)) return;

    bt_connect_active = false;
    char connected_mac[18];
    snprintf(connected_mac, sizeof(connected_mac), "%s", bt_connecting_mac);
    pthread_join(bt_connect_thread, NULL);
    if (!bt_connect_succeeded) {
        snprintf(bt_connect_failed_mac, sizeof(bt_connect_failed_mac), "%s", connected_mac);
    }
    for (int i = 0; i < bt_scan_result_count; i++) {
        if (strcmp(bt_scan_results[i].mac, connected_mac) != 0) continue;
        bt_scan_results[i].paired = bt_connect_succeeded || bt_scan_results[i].paired;
        bt_scan_results[i].connected = bt_connect_succeeded;
        break;
    }
    bt_connecting_mac[0] = '\0';
    populate_bt_screen();
}

static pthread_t bt_forget_thread;
static bool bt_forget_active = false;
static atomic_bool bt_forget_done_flag = false;

typedef struct {
    char mac[18];
    bool succeeded;
} bt_forget_request_t;

/* Kept alive past the thread completing (freed in poll_bt_forget() instead
 * of the thread func itself) so the completion handler still knows which
 * device to update in bt_scan_results -- see poll_bt_forget()'s own
 * comment for why. */
static bt_forget_request_t * bt_forget_pending_req = NULL;

static void * bt_forget_thread_func(void * arg) {
    bt_forget_request_t * req = (bt_forget_request_t *) arg;
    req->succeeded = bt_control_forget(req->mac);
    atomic_store_explicit(&bt_forget_done_flag, true, memory_order_release); /* written last -- poll_bt_forget only checks this flag */
    return NULL;
}

static void start_bt_forget(const char * mac) {
    if (bt_forget_active) return;
    gui_shell_cancel_bt_reconnect();
    bt_forget_request_t * req = malloc(sizeof(*req));
    if (!req) return;
    snprintf(req->mac, sizeof(req->mac), "%s", mac);
    bt_forget_pending_req = req;

    atomic_store_explicit(&bt_forget_done_flag, false, memory_order_relaxed);
    bt_forget_active = true;
        if (pthread_create(&bt_forget_thread, NULL, bt_forget_thread_func, req) != 0) {
        bt_forget_active = false;
        bt_forget_pending_req = NULL;
        free(req);
    }
}

/* Updates bt_scan_results locally and redraws the Bluetooth screen upon
 * forgetting a device without running a full background scan. */
void poll_bt_forget(void) {
    if (!bt_forget_active || !atomic_load_explicit(&bt_forget_done_flag, memory_order_acquire)) return;
    bt_forget_active = false;
    pthread_join(bt_forget_thread, NULL);

    if (bt_forget_pending_req->succeeded) {
        /* A refresh launched after the initial manual cancellation may still
         * have observed the device before Forget completed. Invalidate that
         * positive snapshot before changing the remembered preference. */
        gui_shell_cancel_bt_reconnect();
        for (int i = 0; i < bt_scan_result_count; i++) {
            if (strcmp(bt_scan_results[i].mac, bt_forget_pending_req->mac) == 0) {
                bt_scan_results[i].paired = false;
                bt_scan_results[i].connected = false;
                break;
            }
        }
        if (strcmp(current_settings.bt_last_output_mac, bt_forget_pending_req->mac) == 0) {
            current_settings.bt_last_output_mac[0] = '\0';
            settings_save_async(&current_settings);
        }
    }
    free(bt_forget_pending_req);
    bt_forget_pending_req = NULL;
    populate_bt_screen();
}

/* ---- Bluetooth device action popup --------------------------------------
 * Displays an action popup offering Connect or Forget based on the device's
 * current paired/connected state. */
static gui_popup_t bt_action_popup;
static lv_obj_t * bt_action_popup_title;
static lv_obj_t * bt_action_connect_row;
static lv_obj_t * bt_action_forget_row;
static char bt_action_popup_device_mac[18] = "";

static void hide_bt_action_popup(void) {
    gui_popup_hide(&bt_action_popup);
    bt_action_popup_device_mac[0] = '\0';
}

static void bt_action_popup_backdrop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_bt_action_popup();
}

static void bt_action_connect_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (strlen(bt_action_popup_device_mac) != 17) {
        hide_bt_action_popup();
        return;
    }
    char mac[18];
    snprintf(mac, sizeof(mac), "%s", bt_action_popup_device_mac);
    hide_bt_action_popup();
    gui_shell_cancel_bt_reconnect();
    start_bt_connect(mac);
}

static void bt_action_forget_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (strlen(bt_action_popup_device_mac) != 17) {
        hide_bt_action_popup();
        return;
    }
    char mac[18];
    snprintf(mac, sizeof(mac), "%s", bt_action_popup_device_mac);
    hide_bt_action_popup();
    start_bt_forget(mac);
}

void build_bt_action_popup(void) {
    bt_action_popup.popup = build_confirm_popup("", LV_LABEL_LONG_DOT, &bt_action_popup_title, NULL, "Connect",
                                                 accent_lv_color(), bt_action_connect_cb, &bt_action_connect_row, "Forget",
                                                 lv_color_make(255, 120, 120), bt_action_forget_cb, &bt_action_forget_row,
                                                 bt_action_popup_backdrop_cb, &bt_action_popup.backdrop);
}

static void show_bt_action_popup(int index) {
    if (index < 0 || index >= bt_scan_result_count) return;
    bt_device_t * dev = &bt_scan_results[index];
    snprintf(bt_action_popup_device_mac, sizeof(bt_action_popup_device_mac), "%s", dev->mac);

    lv_label_set_text(bt_action_popup_title, dev->name[0] != '\0' ? dev->name : dev->mac);

    /* Connect makes sense unless already connected. Forget makes sense for
     * anything paired OR connected (the "OR connected" is just a safety net
     * -- a connected-but-somehow-not-paired device shouldn't be able to land
     * on a popup with neither action available). */
    bool show_connect = !dev->connected;
    bool show_forget = dev->paired || dev->connected;
    /* No manual reposition needed -- bt_action_popup's rows are flex-column
     * children now (build_confirm_popup()), so hiding one lets the other
     * reflow up to take its place automatically. */
    if (show_connect) lv_obj_remove_flag(bt_action_connect_row, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(bt_action_connect_row, LV_OBJ_FLAG_HIDDEN);
    if (show_forget) lv_obj_remove_flag(bt_action_forget_row, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(bt_action_forget_row, LV_OBJ_FLAG_HIDDEN);

    gui_popup_show(&bt_action_popup);
}

static void bt_row_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    show_bt_action_popup(index);
}

static void bt_rescan_btn_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    /* bt_scan_results contains both paired devices and transient discovery
     * results. Preserve the former (pairing is persistent state), but drop
     * every old Available Devices entry before beginning a new discovery
     * window. The completed scan replaces this compacted set as usual. */
    int paired_count = 0;
    for (int i = 0; i < bt_scan_result_count; i++) {
        if (!bt_scan_results[i].paired) continue;
        if (paired_count != i) bt_scan_results[paired_count] = bt_scan_results[i];
        paired_count++;
    }
    bt_scan_result_count = paired_count;
    populate_bt_screen();
    start_bt_scan();
}

static lv_obj_t * bt_rescan_btn;

static void set_bt_rescan_active(bool active) {
    if (!bt_rescan_btn) return;
    lv_label_set_text(bt_rescan_btn, active ? "Scanning..." : "Rescan");
    lv_obj_set_style_text_color(bt_rescan_btn,
                                active ? lv_color_make(160, 160, 160) : accent_lv_color(), 0);
    align_screen_header_action(bt_rescan_btn, 20);
    if (active) {
        lv_obj_remove_flag(bt_rescan_btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_state(bt_rescan_btn, LV_STATE_DISABLED);
    } else {
        lv_obj_clear_state(bt_rescan_btn, LV_STATE_DISABLED);
        lv_obj_add_flag(bt_rescan_btn, LV_OBJ_FLAG_CLICKABLE);
    }
}

/* Checks if the device name is a MAC address placeholder (e.g. "5D-C4-96-7E-56-88")
 * substituted by bluetoothctl when no advertised name is present. */
static bool bt_name_is_mac_placeholder(const bt_device_t * dev) {
    if (dev->name[0] == '\0') return true;
    size_t mac_len = strlen(dev->mac);
    if (strlen(dev->name) != mac_len) return false;
    for (size_t i = 0; i < mac_len; i++) {
        char m = dev->mac[i];
        char n = dev->name[i];
        if (m == ':') { if (n != '-' && n != ':') return false; }
        else if (m != n) return false;
    }
    return true;
}

/* Extra row height (beyond LIST_ROW_WIDTH's normal LIST_ROW_HEIGHT) when a
 * row shows the codec subtitle line -- one more small-font line plus a
 * little breathing room, not a full second LIST_ROW_HEIGHT line. */
#define BT_DEVICE_ROW_CODEC_EXTRA_HEIGHT BOARD_SCALE_PX(40)

static void format_rate(char * out, size_t size, unsigned int rate);

static void add_bt_device_row(lv_obj_t * parent, int index) {
    bt_device_t * dev = &bt_scan_results[index];

    /* Codec is only meaningful for the ONE device actually carrying A2DP
     * audio right now -- dev->connected alone isn't enough (a paired,
     * link-level-connected non-audio BLE peripheral would also read
     * connected=true here), so this also checks the row's own MAC against
     * bt_connected_mac_cached, the accessory bt_control_get_connected_
     * device_mac()/_codec() actually queried. Both come from the same
     * background poll as the rest of this screen's live state (see
     * refresh_bt_icon_thread_func()) -- never a synchronous bluealsactl
     * call here on the UI thread. */
    bool show_codec = dev->connected && bt_connected_codec_cached[0] != '\0' &&
                       strcasecmp(dev->mac, bt_connected_mac_cached) == 0;

    lv_obj_t * row = lv_obj_create(parent);
    lv_obj_set_size(row, LIST_ROW_WIDTH, LIST_ROW_HEIGHT + (show_codec ? BT_DEVICE_ROW_CODEC_EXTRA_HEIGHT : 0));
    lv_obj_set_style_radius(row, LIST_ROW_RADIUS, 0);
    lv_obj_set_style_bg_color(row, LIST_ROW_BG_COLOR, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * label = lv_label_create(row);
    char text[96];
    const char * status;
    if (strcmp(dev->mac, bt_connecting_mac) == 0) status = "  - Connecting...";
    else if (strcmp(dev->mac, bt_connect_failed_mac) == 0) status = "  - Failed to connect";
    else if (dev->connected) status = "  - Connected";
    else if (dev->paired) status = "  - Paired";
    else status = "";
    snprintf(text, sizeof(text), "%s%s", dev->name[0] != '\0' ? dev->name : dev->mac, status);
    lv_label_set_text(label, text);
    lv_obj_add_style(label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(label, &LIST_ROW_FONT, 0);
    if (show_codec) {
        lv_obj_align(label, LV_ALIGN_TOP_LEFT, LIST_ROW_LABEL_INSET, BOARD_SCALE_PX(14));
    } else {
        lv_obj_align(label, LV_ALIGN_LEFT_MID, LIST_ROW_LABEL_INSET, 0);
    }

    if (show_codec) {
        lv_obj_t * codec_label = lv_label_create(row);
        /* Codec and rate together: the negotiated link is not necessarily the
         * codec preference in Settings, and the frequency is the other half of
         * what tells you which link you actually got. Rate is omitted rather
         * than guessed when bluealsa does not report one. */
        char codec_text[96]; /* codec (32) + separator + formatted rate */
        if (bt_connected_rate_cached) {
            char rate[32];
            format_rate(rate, sizeof(rate), bt_connected_rate_cached);
            snprintf(codec_text, sizeof(codec_text), "%s · %s", bt_connected_codec_cached, rate);
        } else {
            snprintf(codec_text, sizeof(codec_text), "%s", bt_connected_codec_cached);
        }
        lv_label_set_text(codec_label, codec_text);
        lv_obj_add_style(codec_label, &style_theme_text_muted, 0);
        lv_obj_set_style_text_font(codec_label, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
        lv_obj_align(codec_label, LV_ALIGN_BOTTOM_LEFT, LIST_ROW_LABEL_INSET, BOARD_SCALE_PX(-12));
    }

    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, bt_row_click_cb, LV_EVENT_CLICKED, (void *) (intptr_t) index);
}

static pthread_t bt_scan_thread;
static bool bt_scan_active = false;
static atomic_bool bt_scan_done_flag = false;
static bt_device_t bt_scan_pending_results[BT_MAX_RESULTS];
static int bt_scan_pending_result_count = 0;

/* Set before the thread starts, so the write happens-before the read. */
static bool bt_scan_want_inquiry = true;

static void * bt_scan_thread_func(void * arg) {
    (void) arg;
    bt_scan_pending_result_count = bt_scan_want_inquiry
        ? bt_control_scan(6, bt_scan_pending_results, BT_MAX_RESULTS)
        : bt_control_list_devices(bt_scan_pending_results, BT_MAX_RESULTS);
    atomic_store_explicit(&bt_scan_done_flag, true, memory_order_release); /* written last -- poll_bt_scan only checks this flag */
    return NULL;
}

/* Runs Bluetooth scan in a background thread without navigation or busy screen.
 * With inquiry false it only re-reads the devices BlueZ already knows, which
 * leaves an active A2DP stream alone. */
static void start_bt_scan_ex(bool inquiry) {
    if (bt_scan_active) return;

    bt_scan_want_inquiry = inquiry;

    atomic_store_explicit(&bt_scan_done_flag, false, memory_order_relaxed);
    bt_scan_active = true;
    set_bt_rescan_active(true);

    if (pthread_create(&bt_scan_thread, NULL, bt_scan_thread_func, NULL) != 0) {
        bt_scan_active = false;
        set_bt_rescan_active(false);
    }
}

static void start_bt_scan(void) { start_bt_scan_ex(true); }

void poll_bt_scan(void) {
    if (!bt_scan_active || !atomic_load_explicit(&bt_scan_done_flag, memory_order_acquire)) return;

    bt_scan_active = false;
    pthread_join(bt_scan_thread, NULL);
    set_bt_rescan_active(false);
    if (bt_scan_pending_result_count >= 0 && bt_scan_pending_result_count <= BT_MAX_RESULTS) {
        memcpy(bt_scan_results, bt_scan_pending_results,
               (size_t) bt_scan_pending_result_count * sizeof(bt_scan_results[0]));
        bt_scan_result_count = bt_scan_pending_result_count;
    }
    populate_bt_screen(); /* refresh the list either way -- worth keeping current even if the user already backed out */
}

/* ---- Bluetooth DAC screen -- lets this device accept incoming A2DP audio
 * (another device streaming TO it, using it as an external DAC) rather than
 * only ever sending audio out. See bt_control_apply_output_settings()'s own
 * doc comment for exactly what toggling this does at the bluealsa/
 * discoverability level -- it's a real running-service restart, not just a
 * flag flip. ---- */

static lv_obj_t * bt_dac_list;

static void bt_dac_enable_row_cb(lv_event_t * e);

/* Explanation + a single "Enable" action, not a toggle -- turning Bluetooth
 * DAC on takes over the whole screen with its own overlay (see
 * build_bt_dac_overlay_screen() below), matching USB DAC mode's own design
 * (build_usb_dac_overlay_screen()): the device can't sensibly be used as a
 * normal music player while it's set up to receive and play audio FROM
 * another device, so that isn't something a background toggle should hide.
 * open_bt_dac_screen() only ever reaches this screen when Bluetooth DAC is
 * currently off -- while on, it goes straight to the overlay instead. */
void populate_bt_dac_screen(void) {
    lv_obj_clean(bt_dac_list);

    lv_obj_t * explanation = lv_label_create(bt_dac_list);
    lv_label_set_text(explanation,
                      "When on, this device stays visible and pairable to other Bluetooth devices, "
                      "so a phone or computer can stream audio TO it and play through this device's "
                      "own output -- using it as an external DAC.");
    lv_obj_set_width(explanation, lv_pct(90));
    lv_label_set_long_mode(explanation, LV_LABEL_LONG_WRAP);
    lv_obj_add_style(explanation, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(explanation, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_set_style_pad_left(explanation, BOARD_SCALE_PX(24), 0);
    lv_obj_set_style_pad_top(explanation, BOARD_SCALE_PX(12), 0);
    lv_obj_set_style_pad_bottom(explanation, BOARD_SCALE_PX(12), 0);

    lv_obj_t * row = add_pill_row_base(bt_dac_list, "Enable Bluetooth DAC");
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, bt_dac_enable_row_cb, LV_EVENT_CLICKED, NULL);
}

/* Shared by bt_dac_enable_row_cb() and open_bt_dac_screen()'s "already
 * enabled" shortcut -- reapplies the bluealsa/bt-agent sink configuration
 * rather than relying solely on current_settings.bt_dac_mode_enabled, ensuring
 * the underlying pipeline matches the active UI state. */
static void enable_bt_dac_and_show_overlay(void) {
    gui_shell_cancel_bt_reconnect();
    current_settings.bt_dac_mode_enabled = true;
    settings_save(&current_settings);
    start_bt_apply_output_settings(true, current_settings.bt_volume_sync_enabled);

    /* Bluetooth DAC, AirPlay, and local playback share the same physical ALSA
     * audio device and are mutually exclusive. Enabling Bluetooth DAC stops local
     * playback and turns off AirPlay if active. */
    if (audio_is_playing()) {
        audio_stop();
        set_play_button_state(false);
        plugin_manager_notify_stopped();
    }
    if (current_settings.wifi_dac_mode_enabled) {
        current_settings.wifi_dac_mode_enabled = false;
        settings_save(&current_settings);
        airplay_control_stop();
    }

    nav_push(bt_dac_overlay_screen);
}

static void bt_dac_enable_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    enable_bt_dac_and_show_overlay();
}

static lv_obj_t * build_bt_dac_screen(void) {
    lv_obj_t * title_label; /* unused after build -- title never changes */
    return build_subsonic_list_screen("Bluetooth DAC", &title_label, &bt_dac_list);
}

static void open_bt_dac_screen(void) {
    /* Bluetooth itself has to be on before any of this makes sense -- only
     * actually reachable via the DAC home tile's "Bluetooth DAC" row
     * (build_dac_home_screen()): the Bluetooth settings screen's own
     * "Bluetooth DAC" chevron row only exists while populate_bt_screen()
     * has already gated its whole extra-rows section on Bluetooth being
     * powered (see its own "if (!powered) return;"), so that path can
     * never reach here with Bluetooth off in the first place -- but the DAC
     * home tile has no such gating, so tapping it with Bluetooth off used
     * to head straight into enable_bt_dac_and_show_overlay(), which just
     * spawns bluealsa/bt-agent against a radio that isn't even on. */
    if (!bt_is_powered_cached) {
        show_error_toast("Enable Bluetooth in settings to use BT DAC mode");
        return;
    }
    /* Already on -- re-apply and go straight to the overlay rather than
     * showing the "explanation + Enable" screen for a mode that's already
     * supposed to be active (see enable_bt_dac_and_show_overlay()'s own
     * comment on why re-applying here, not just trusting the flag,
     * matters). */
    if (current_settings.bt_dac_mode_enabled) {
        enable_bt_dac_and_show_overlay();
        return;
    }
    populate_bt_dac_screen();
    nav_push(bt_dac_screen);
}

void bt_dac_settings_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    open_bt_dac_screen();
}

/* ---- Bluetooth DAC overlay + leave-confirmation popup -- same shape as
 * USB DAC mode's own (build_usb_dac_overlay_screen()/
 * build_usb_dac_leave_popup()): full-screen takeover, no swipe-to-back, the
 * only way out is the back button which asks for confirmation first. ---- */
static gui_popup_t bt_dac_leave_popup;

static void hide_bt_dac_leave_popup(void) {
    gui_popup_hide(&bt_dac_leave_popup);
}

static void bt_dac_leave_popup_backdrop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_bt_dac_leave_popup();
}

static void bt_dac_leave_cancel_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_bt_dac_leave_popup();
}

static void bt_dac_leave_confirm_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_bt_dac_leave_popup();
    nav_pop(); /* leave the DAC overlay screen */

    current_settings.bt_dac_mode_enabled = false;
    settings_save(&current_settings);
    start_bt_apply_output_settings(false, current_settings.bt_volume_sync_enabled);
}

static void bt_dac_overlay_back_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    gui_popup_show(&bt_dac_leave_popup);
}

void build_bt_dac_leave_popup(void) {
    bt_dac_leave_popup.popup = build_confirm_popup("Leave Bluetooth DAC mode?", LV_LABEL_LONG_WRAP, NULL, NULL, "Leave",
                                                    lv_color_make(255, 120, 120), bt_dac_leave_confirm_cb, NULL, "Cancel",
                                                    accent_lv_color(), bt_dac_leave_cancel_cb, NULL,
                                                    bt_dac_leave_popup_backdrop_cb, &bt_dac_leave_popup.backdrop);
}

static lv_obj_t * build_bt_dac_overlay_screen(void) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

    build_screen_header(scr, NULL, bt_dac_overlay_back_cb, NULL, NULL);

    lv_obj_t * icon = lv_image_create(scr);
    lv_image_set_src(icon, asset_path("bt/bt.png"));
    lv_obj_align(icon, LV_ALIGN_CENTER, 0, BOARD_SCALE_PX(-40));

    lv_obj_t * status_label = lv_label_create(scr);
    lv_label_set_text(status_label, "Bluetooth DAC mode");
    lv_obj_add_style(status_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(status_label, gui_theme_font(GUI_FONT_ROLE_TITLE), 0);
    lv_obj_align_to(status_label, icon, LV_ALIGN_OUT_BOTTOM_MID, 0, BOARD_SCALE_PX(24));

    lv_obj_t * hint_label = lv_label_create(scr);
    lv_label_set_text(hint_label, "This device is now receiving Bluetooth audio");
    lv_obj_add_style(hint_label, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(hint_label, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    /* Same fixed-width + center treatment as the USB DAC screen's own
     * matching labels (see build_usb_dac_overlay_screen()'s own comment)
     * -- kept consistent between the two DAC mode screens on request, and
     * this specific string is long enough to need an explicit width to
     * wrap within regardless (no width means no wrap boundary, so it would
     * otherwise render on one line and can overflow past the screen edge). */
    lv_obj_set_width(hint_label, LV_PCT(90));
    lv_obj_set_style_text_align(hint_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align_to(hint_label, status_label, LV_ALIGN_OUT_BOTTOM_MID, 0, BOARD_SCALE_PX(8));

    bt_dac_stream_label = lv_label_create(scr);
    lv_label_set_text(bt_dac_stream_label, "Waiting for Bluetooth stream…");
    lv_obj_add_style(bt_dac_stream_label, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(bt_dac_stream_label, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_set_width(bt_dac_stream_label, LV_PCT(90));
    lv_obj_set_style_text_align(bt_dac_stream_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align_to(bt_dac_stream_label, hint_label, LV_ALIGN_OUT_BOTTOM_MID, 0, BOARD_SCALE_PX(8));

    return scr;
}

/* ---- Codec selection screen -- single-select list, current choice shown
 * via an accent-colored border rather than a checkmark glyph (same
 * indicator build_accent_color_screen's swatches use; safer than a unicode
 * checkmark that this project's subset LVGL fonts may not actually have a
 * glyph for). The preference is applied through the BlueALSA 5 source daemon
 * reconciliation path. ---- */

typedef struct {
    const char * value; /* matches player_settings_t.bt_codec / bt_control_set_codec()'s argument */
    const char * label;
} bt_codec_option_t;

static const bt_codec_option_t bt_codec_options[] = {
    { "auto", "Auto" },      { "ldac_hq", "LDAC Quality" }, { "ldac_sq", "LDAC Standard" },
    { "aptx", "aptX" },      { "aac", "AAC" },              { "sbc", "SBC" },
    { "sbc_xq", "SBC-XQ" },
};
#define BT_CODEC_OPTION_COUNT (sizeof(bt_codec_options) / sizeof(bt_codec_options[0]))

static lv_obj_t * bt_codec_list;

static void bt_codec_option_row_cb(lv_event_t * e);

static void populate_bt_codec_screen(void) {
    lv_obj_clean(bt_codec_list);
    for (size_t i = 0; i < BT_CODEC_OPTION_COUNT; i++) {
        bool selected = strcmp(current_settings.bt_codec, bt_codec_options[i].value) == 0;
        add_pill_option_row(bt_codec_list, bt_codec_options[i].label,
                            selected, bt_codec_option_row_cb, (void *) (intptr_t) i);
    }
}

static void bt_codec_option_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    if (index < 0 || (size_t) index >= BT_CODEC_OPTION_COUNT) return;
    const char * codec = bt_codec_options[index].value;
    bool codec_changed = strcmp(current_settings.bt_codec, codec) != 0;
    if (!bt_control_set_codec(codec)) {
        show_error_toast("Could not save Bluetooth codec");
        return;
    }
    snprintf(current_settings.bt_codec, sizeof(current_settings.bt_codec), "%s", codec);
    settings_save_async(&current_settings);
    populate_bt_codec_screen();
    if (codec_changed) show_info_toast("Turn Bluetooth off and on to apply");
}

/* ---- Bluetooth > Advanced ---------------------------------------------
 * Settings that are set once and rarely revisited, kept off the Bluetooth
 * screen so the device list stays the focus there. ---- */
static lv_obj_t * bt_advanced_list;
static lv_obj_t * bt_rate_list;

static void bt_rate_option_row_cb(lv_event_t * e);

/* Rates offered by the connected accessory for the codec in use. Queried
 * when the screen opens, because the set depends on both the accessory and
 * the negotiated codec. */
#define BT_RATE_MAX_OPTIONS 12
static unsigned int bt_rate_options[BT_RATE_MAX_OPTIONS];
static int bt_rate_option_count;
/* True while the background query is in flight, so the list can say so. */
static bool bt_rate_query_active = false;

static void bt_rate_format(char * out, size_t out_size, unsigned int rate) {
    if (rate % 1000 == 0) snprintf(out, out_size, "%u kHz", rate / 1000);
    else snprintf(out, out_size, "%u.%u kHz", rate / 1000, (rate % 1000) / 100);
}

/* Rate shown and edited is the connected accessory's own, falling back to the
 * global default when nothing is connected. */
static unsigned int bt_rate_selected(void) {
    return settings_bt_rate_for(&current_settings, bt_connected_mac_cached);
}

static void populate_bt_rate_screen(void) {
    lv_obj_clean(bt_rate_list);
    add_pill_option_row(bt_rate_list, "Automatic (44.1 kHz)", bt_rate_selected() == 0,
                        bt_rate_option_row_cb, (void *) (intptr_t) 0);

    /* Nothing connected means nothing to query, and only 44.1 can be applied
     * without an accessory present to negotiate with. */
    if (bt_rate_option_count == 0) {
        add_pill_option_row(bt_rate_list, "44.1 kHz", bt_rate_selected() == 44100,
                            bt_rate_option_row_cb, (void *) (intptr_t) 44100);
        add_section_header(bt_rate_list, bt_rate_query_active ? "Reading supported rates..."
                                                              : "Connect a device to see its supported rates");
        return;
    }
    for (int i = 0; i < bt_rate_option_count; i++) {
        char label[32];
        bt_rate_format(label, sizeof(label), bt_rate_options[i]);
        add_pill_option_row(bt_rate_list, label, bt_rate_selected() == bt_rate_options[i],
                            bt_rate_option_row_cb, (void *) (intptr_t) bt_rate_options[i]);
    }
}

static void populate_bt_advanced_screen(void);
/* Defined with the Bluetooth screen further down; the Advanced screen owns
 * the rows now but the callbacks stay next to the state they touch. */
static void bt_volume_sync_toggle_cb(lv_event_t * e);
static void bt_hide_unnamed_toggle_cb(lv_event_t * e);
static void bt_codec_settings_row_cb(lv_event_t * e);

/* Cycling the accessory's link blocks for seconds, so it runs on a worker
 * while the screen stays live. */
static pthread_t bt_rate_apply_thread;
static bool bt_rate_apply_active = false;
static atomic_bool bt_rate_apply_done_flag = false;
/* Captured at launch so a status refresh cannot change what is being applied
 * half way through. */
static unsigned int bt_rate_apply_target = 0;

static void * bt_rate_apply_thread_func(void * arg) {
    (void) arg;
    (void) bt_control_reconnect_for_rate_change(bt_rate_apply_target);
    atomic_store_explicit(&bt_rate_apply_done_flag, true, memory_order_release); /* written last */
    return NULL;
}

void poll_bt_rate_apply(void) {
    if (!bt_rate_apply_active || !atomic_load_explicit(&bt_rate_apply_done_flag, memory_order_acquire)) return;
    bt_rate_apply_active = false;
    pthread_join(bt_rate_apply_thread, NULL);
    /* No outcome toast: the accessory may still need reconnecting by hand,
     * so "applied" or "failed" here would contradict what the user is about
     * to do. The redrawn list shows the rate that actually took. */
    if (gui_navigation_is_top(bt_rate_screen)) populate_bt_rate_screen();
}

static void bt_rate_option_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    unsigned int rate = (unsigned int) (intptr_t) lv_event_get_user_data(e);
    bool changed = bt_rate_selected() != rate;
    /* Remembered against the accessory when one is connected, so each keeps
     * its own; with nothing connected this sets the default for accessories
     * that have no entry yet. */
    if (bt_connected_mac_cached[0]) settings_bt_set_rate_for(&current_settings, bt_connected_mac_cached, rate);
    else current_settings.bt_sample_rate = rate;
    settings_save_async(&current_settings);
    bt_control_choose_sample_rate(rate);
    populate_bt_rate_screen();
    if (!changed) return;

    /* The rate is fixed when the A2DP configuration is negotiated, and these
     * accessories refuse to renegotiate a live transport, so the link has to
     * be re-established for the choice to take effect. Only the link cycles;
     * the radio stays on. With nothing connected there is nothing to cycle
     * and the choice simply applies on the next connection. */
    if (!gui_shell_is_bt_audio_connected()) return;
    if (bt_rate_apply_active) {
        /* Silently dropping this would leave the setting saying one thing and
         * the transport doing another. */
        show_info_toast("Still applying the previous choice");
        return;
    }
    atomic_store_explicit(&bt_rate_apply_done_flag, false, memory_order_relaxed);
    bt_rate_apply_target = rate;
    bt_rate_apply_active = true;
    if (pthread_create(&bt_rate_apply_thread, NULL, bt_rate_apply_thread_func, NULL) != 0) {
        bt_rate_apply_active = false;
        show_info_toast("Turn Bluetooth off and on to apply");
        return;
    }
    show_info_toast("Headset may disconnect, manual reconnection might be required");
}

static lv_obj_t * build_bt_rate_screen(void) {
    lv_obj_t * title_label;
    return build_subsonic_list_screen("Sample Rate", &title_label, &bt_rate_list);
}

/* The query forks bluealsactl, which must never happen on the UI thread:
 * this screen would otherwise freeze for as long as the call takes, the same
 * stall populate_bt_screen() documents for bt_control_is_powered(). The
 * screen opens with what is already known and fills in when the worker
 * finishes. */
static pthread_t bt_rate_query_thread;
static atomic_bool bt_rate_query_done_flag = false;
static unsigned int bt_rate_query_results[BT_RATE_MAX_OPTIONS];
static int bt_rate_query_result_count;

static void * bt_rate_query_thread_func(void * arg) {
    (void) arg;
    bt_rate_query_result_count = bt_control_get_available_rates(bt_rate_query_results, BT_RATE_MAX_OPTIONS);
    atomic_store_explicit(&bt_rate_query_done_flag, true, memory_order_release); /* written last */
    return NULL;
}

/* The screen shows one accessory's rates and edits that accessory's entry,
 * so a device changing underneath it has to redraw or it would edit the
 * wrong one. */
void gui_network_notify_bt_device_changed(void) {
    if (!bt_rate_screen || !gui_navigation_is_top(bt_rate_screen)) return;
    bt_rate_option_count = 0;
    populate_bt_rate_screen();
}

void poll_bt_rate_query(void) {
    if (!bt_rate_query_active || !atomic_load_explicit(&bt_rate_query_done_flag, memory_order_acquire)) return;
    bt_rate_query_active = false;
    pthread_join(bt_rate_query_thread, NULL);
    if (bt_rate_query_result_count > 0) {
        memcpy(bt_rate_options, bt_rate_query_results, sizeof(bt_rate_options));
        bt_rate_option_count = bt_rate_query_result_count;
    }
    /* Repainted even when the query found nothing, so the "reading" note is
     * replaced rather than left up until the screen is reopened. */
    if (gui_navigation_is_top(bt_rate_screen)) populate_bt_rate_screen();
}

static void bt_rate_settings_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    bt_rate_option_count = 0;
    populate_bt_rate_screen();
    nav_push(bt_rate_screen);
    if (gui_shell_is_bt_audio_connected() && !bt_rate_query_active) {
        atomic_store_explicit(&bt_rate_query_done_flag, false, memory_order_relaxed);
        bt_rate_query_active = true;
        if (pthread_create(&bt_rate_query_thread, NULL, bt_rate_query_thread_func, NULL) != 0)
            bt_rate_query_active = false;
    }
}

static void bt_speexrate_toggle_cb(lv_event_t * e) {
    /* add_pill_toggle_row() makes the switch itself non-clickable and fires
     * CLICKED on the row, so the state is flipped here rather than read back
     * off the widget. */
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    current_settings.bt_speexrate_enabled = !current_settings.bt_speexrate_enabled;
    settings_save_async(&current_settings);
    bt_control_set_speexrate_enabled(current_settings.bt_speexrate_enabled);
    populate_bt_advanced_screen();
}

static void populate_bt_advanced_screen(void) {
    lv_obj_clean(bt_advanced_list);
    add_pill_toggle_row(bt_advanced_list, "Bluetooth Volume Sync",
                        current_settings.bt_volume_sync_enabled, bt_volume_sync_toggle_cb);
    add_pill_chevron_row(bt_advanced_list, "Codec", bt_codec_settings_row_cb);
    add_pill_chevron_row(bt_advanced_list, "Sample Rate", bt_rate_settings_row_cb);
    /* Only does anything for a track whose rate differs from the negotiated
     * transport rate; with Sample Rate at 44.1 kHz most tracks never reach
     * the resampler at all. */
    add_pill_toggle_row(bt_advanced_list, "Speex Resampling",
                        current_settings.bt_speexrate_enabled, bt_speexrate_toggle_cb);
    add_pill_toggle_row(bt_advanced_list, "Hide Unnamed Devices",
                        current_settings.bt_hide_unnamed_devices, bt_hide_unnamed_toggle_cb);
}

static lv_obj_t * build_bt_advanced_screen(void) {
    lv_obj_t * title_label;
    return build_subsonic_list_screen("Advanced", &title_label, &bt_advanced_list);
}

static void bt_advanced_settings_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    populate_bt_advanced_screen();
    nav_push(bt_advanced_screen);
}

static lv_obj_t * build_bt_codec_screen(void) {
    lv_obj_t * title_label; /* unused after build -- title never changes */
    return build_subsonic_list_screen("Codec", &title_label, &bt_codec_list);
}

static void open_bt_codec_screen(void) {
    populate_bt_codec_screen();
    nav_push(bt_codec_screen);
}

static void bt_codec_settings_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    open_bt_codec_screen();
}

/* ---- Resume Last Track selection screen (Settings > Music Settings >
 * Playback > Resume Last Track) -- same accent-colored-border single-select shape as Font Size
 * just below, but takes effect on the NEXT cold boot (there's nothing to
 * apply live -- this only ever matters at startup), so no reboot popup is
 * needed, just a toast confirming the choice. See gui_init()'s own resume
 * path for what each option actually does and the Subsonic-cache guard it
 * shares with Car Mode's separate, always-on resume mechanism. ---- */

typedef struct {
    int mode; /* matches player_settings_t.resume_mode */
    const char * label;
} resume_mode_option_t;

static const resume_mode_option_t resume_mode_options[] = {
    { 0, "Off" }, { 1, "Resume and Play" }, { 2, "Resume, but Paused" },
};
#define RESUME_MODE_OPTION_COUNT (sizeof(resume_mode_options) / sizeof(resume_mode_options[0]))

static void resume_mode_option_row_cb(lv_event_t * e);

static void populate_resume_mode_screen(void) {
    lv_obj_clean(resume_mode_list);
    for (size_t i = 0; i < RESUME_MODE_OPTION_COUNT; i++) {
        bool selected = current_settings.resume_mode == resume_mode_options[i].mode;
        add_pill_option_row(resume_mode_list, resume_mode_options[i].label,
                            selected, resume_mode_option_row_cb, (void *) (intptr_t) i);
    }
}

static void resume_mode_option_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    current_settings.resume_mode = resume_mode_options[index].mode;
    settings_save(&current_settings);
    populate_resume_mode_screen();
    show_info_toast("Applies next time you launch the app");
}

static lv_obj_t * build_resume_mode_screen(void) {
    lv_obj_t * title_label; /* unused after build -- title never changes */
    return build_subsonic_list_screen("Resume Last Track", &title_label, &resume_mode_list);
}

static void open_resume_mode_screen(void) {
    populate_resume_mode_screen();
    nav_push(resume_mode_screen);
}

void resume_mode_settings_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    open_resume_mode_screen();
}

/* ---- Play/Pause button behavior selection screen (Settings > Music Settings
 * > Controls & Interface > Play/Pause Button) -- same accent-colored-border
 * single-select shape as Resume Last Track just above. Feature request: the
 * physical play/pause button can act as Previous Track instead, or combine
 * both via a short
 * double-click window -- see handle_physical_play_pause_press() near
 * update_timer_cb for the actual dispatch. Unlike Resume Last Track, this
 * takes effect on the very next physical button press, not just at next
 * boot. ---- */

typedef struct {
    int mode; /* matches player_settings_t.play_pause_button_mode */
    const char * label;
} play_pause_button_mode_option_t;

static const play_pause_button_mode_option_t play_pause_button_mode_options[] = {
    { 0, "Play/Pause" }, { 1, "Previous Track" }, { 2, "Play/Pause + Previous Track (Double-Click)" },
};
#define PLAY_PAUSE_BUTTON_MODE_OPTION_COUNT (sizeof(play_pause_button_mode_options) / sizeof(play_pause_button_mode_options[0]))

static lv_obj_t * play_pause_button_mode_list;

static void play_pause_button_mode_option_row_cb(lv_event_t * e);

static void populate_play_pause_button_mode_screen(void) {
    lv_obj_clean(play_pause_button_mode_list);
    for (size_t i = 0; i < PLAY_PAUSE_BUTTON_MODE_OPTION_COUNT; i++) {
        bool selected = current_settings.play_pause_button_mode == play_pause_button_mode_options[i].mode;
        add_pill_option_row(play_pause_button_mode_list, play_pause_button_mode_options[i].label,
                            selected, play_pause_button_mode_option_row_cb, (void *) (intptr_t) i);
    }
}

static void play_pause_button_mode_option_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    current_settings.play_pause_button_mode = play_pause_button_mode_options[index].mode;
    settings_save(&current_settings);
    populate_play_pause_button_mode_screen();
    show_info_toast("Applies immediately");
}

static lv_obj_t * build_play_pause_button_mode_screen(void) {
    lv_obj_t * title_label; /* unused after build -- title never changes */
    return build_subsonic_list_screen("Play/Pause Button", &title_label, &play_pause_button_mode_list);
}

static void open_play_pause_button_mode_screen(void) {
    populate_play_pause_button_mode_screen();
    nav_push(play_pause_button_mode_screen);
}

void play_pause_button_mode_settings_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    open_play_pause_button_mode_screen();
}

/* ---- Font size selection screen (Settings > Font Size).  A changed tier
 * is applied behind one rendered black frame, then navigation is reset
 * directly to Home without disturbing playback or background services. ---- */

typedef struct {
    int tier; /* matches player_settings_t.font_size_tier */
    const char * label;
} font_size_option_t;

static const font_size_option_t font_size_options[] = {
    { 0, "Small" }, { 1, "Medium" }, { 2, "BlindMF" },
};
#define FONT_SIZE_OPTION_COUNT (sizeof(font_size_options) / sizeof(font_size_options[0]))

static lv_obj_t * font_size_list;

static void font_size_option_row_cb(lv_event_t * e);

static void populate_font_size_screen(void) {
    lv_obj_clean(font_size_list);
    for (size_t i = 0; i < FONT_SIZE_OPTION_COUNT; i++) {
        bool selected = current_settings.font_size_tier == font_size_options[i].tier;
        add_pill_option_row(font_size_list, font_size_options[i].label,
                            selected, font_size_option_row_cb, (void *) (intptr_t) i);
    }
}

static void font_size_apply_timer_cb(lv_timer_t * timer) {
    lv_obj_t * mask = (lv_obj_t *)lv_timer_get_user_data(timer);
    int target = (int)(intptr_t)lv_obj_get_user_data(mask) - 1;
    lv_timer_delete(timer);
    if (!fallback_font_apply_size_tier(target)) {
        lv_obj_delete(mask);
        show_error_toast("Could not apply font size");
        return;
    }

    /* Stable font addresses mean existing object styles now see the new
     * descriptors.  Notify LVGL once, refresh only cached pixel geometry,
     * and discard every bitmap captured with the old metrics.  All of this
     * is one-shot work inside the mask; no timer or extra allocation remains
     * afterward. */
    gui_navigation_invalidate_font_snapshots();
    current_settings.font_size_tier = target;
    settings_save(&current_settings);
    lv_obj_report_style_change(NULL);
    /* Every live screen, not only the Font Size nav stack. Music Settings
     * and the other unused Settings submenus keep the previous tier's
     * USER_3 label boxes unless they are in this walk. */
    screen_builders_refresh_all_font_geometry();
    gui_settings_refresh_font_geometry();
    gui_player_refresh_font_geometry();
    compact_list_refresh_all();
    quick_drawer_mark_snapshot_dirty();
    nav_reset_to_home();
    screen_builders_refresh_font_geometry(lv_screen_active());
    lv_obj_invalidate(lv_screen_active());
    lv_obj_invalidate(lv_layer_top());
    lv_obj_invalidate(lv_layer_sys());

    lv_obj_delete(mask);
}

static void font_size_option_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    int target = font_size_options[index].tier;
    if (target == current_settings.font_size_tier) return;

    lv_obj_t * mask = lv_obj_create(lv_layer_sys());
    lv_obj_set_user_data(mask, (void *)(intptr_t)(target + 1));
    lv_display_t * display = lv_display_get_default();
    lv_obj_set_size(mask,
                    lv_display_get_horizontal_resolution(display),
                    lv_display_get_vertical_resolution(display));
    lv_obj_align(mask, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(mask, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(mask, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(mask, 0, 0);
    lv_obj_set_style_radius(mask, 0, 0);
    lv_obj_set_style_pad_all(mask, 0, 0);
    lv_obj_remove_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_foreground(mask);
    lv_obj_invalidate(mask);

    /* Leave enough time for the normal refresh timer to paint the mask;
     * synchronous lv_refr_now() here would re-enter rendering from an input
     * callback and was the source of earlier transition flicker. */
    lv_timer_create(font_size_apply_timer_cb, 35, mask);
}

static lv_obj_t * build_font_size_screen(void) {
    lv_obj_t * title_label; /* unused after build -- title never changes */
    return build_subsonic_list_screen("Font Size", &title_label, &font_size_list);
}

static void open_font_size_screen(void) {
    populate_font_size_screen();
    nav_push(font_size_screen);
}

void font_size_settings_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    open_font_size_screen();
}



/* ---- ReplayGain mode (Settings -> Playback) -- 3-way option (Off, Per Track,
 * Per Album). Applied immediately to take effect on the next started or queued
 * track without loudness jumps during current playback. ---- */

typedef struct {
    int mode; /* matches player_settings_t.replaygain_mode */
    const char * label;
} replaygain_mode_option_t;

static const replaygain_mode_option_t replaygain_mode_options[] = {
    { 0, "Off" }, { 1, "Per Track" }, { 2, "Per Album" },
};
#define REPLAYGAIN_MODE_OPTION_COUNT (sizeof(replaygain_mode_options) / sizeof(replaygain_mode_options[0]))

static lv_obj_t * replaygain_mode_list;

static void replaygain_mode_option_row_cb(lv_event_t * e);

static void populate_replaygain_mode_screen(void) {
    lv_obj_clean(replaygain_mode_list);
    for (size_t i = 0; i < REPLAYGAIN_MODE_OPTION_COUNT; i++) {
        bool selected = current_settings.replaygain_mode == replaygain_mode_options[i].mode;
        add_pill_option_row(replaygain_mode_list, replaygain_mode_options[i].label,
                            selected, replaygain_mode_option_row_cb, (void *) (intptr_t) i);
    }
}

static void replaygain_mode_option_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    current_settings.replaygain_mode = replaygain_mode_options[index].mode;
    settings_save(&current_settings);
    populate_replaygain_mode_screen();
    if (gui_player_has_active_track()) arm_next_track_for_audio(gui_player_get_playlist_index());
}

static lv_obj_t * build_replaygain_mode_screen(void) {
    lv_obj_t * title_label; /* unused after build -- title never changes */
    return build_subsonic_list_screen("ReplayGain", &title_label, &replaygain_mode_list);
}

static void open_replaygain_mode_screen(void) {
    populate_replaygain_mode_screen();
    nav_push(replaygain_mode_screen);
}

void replaygain_mode_settings_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    open_replaygain_mode_screen();
}

/* ---- USB device mode (Settings > USB Mode) -- single-select list with an
 * accent-colored border on the active option. Applying a mode modifies
 * configfs/sysfs in a worker thread polled from update_timer_cb to prevent
 * blocking the UI thread. ---- */

typedef struct {
    usb_mode_t mode;
    const char * label;
} usb_mode_option_t;

static const usb_mode_option_t usb_mode_options[] = {
    { USB_MODE_STORAGE, "Storage" },
    { USB_MODE_DAC, "USB DAC" },
    { USB_MODE_ADB, "ADB" },
};
#define USB_MODE_OPTION_COUNT (sizeof(usb_mode_options) / sizeof(usb_mode_options[0]))

static lv_obj_t * usb_mode_list;

static pthread_t usb_mode_switch_thread;
static bool usb_mode_switch_active = false;
static bool usb_mode_switch_queued = false;
/* UI-owned guard; the detached worker only publishes completion. It may
 * wait indefinitely for an old bridge writer, so never join it on the UI. */
static bool usb_bridge_restart_active = false;
static atomic_bool usb_bridge_restart_done = false;
static atomic_bool usb_mode_switch_done_flag = false;
static volatile bool usb_mode_switch_succeeded = false;
static usb_mode_t usb_mode_switch_target;
static bool usb_cable_state_initialized;
static bool usb_cable_was_connected;
static bool usb_storage_rebind_pending;
static usb_storage_session_t usb_storage_session;
static void flush_sd_view_after_usb_storage(void);
static uint32_t usb_storage_host_check_tick;

static void usb_mode_option_row_cb(lv_event_t * e);
/* ADB is a separate toggle, not a third equal option alongside Storage/DAC
 * -- matches the stock firmware's own USB device mode screen (confirmed by
 * the user against a real stock device: ADB overrides Storage/DAC while
 * on, turning it off falls back to the other two). This also matches the
 * actual gadget architecture: ADB lives under its own "adb_demo" configfs
 * directory, entirely separate from the "android0" one Storage and DAC
 * share (see usb_mode_control.c), so it really is orthogonal rather than a
 * third position in the same rotation. */
static bool adb_owns_usb_port(void);

static void populate_usb_mode_screen(void) {
    lv_obj_clean(usb_mode_list);

    /* ADB itself is toggled from Developer Options, not here, but it still
     * owns the port while on, so this screen keeps showing that the other
     * modes are unavailable. */
    bool adb_active = adb_owns_usb_port();

    for (size_t i = 0; i < USB_MODE_OPTION_COUNT; i++) {
        if (usb_mode_options[i].mode == USB_MODE_ADB) continue; /* handled by the toggle above */

        bool selected = !adb_active && current_settings.usb_mode == (int) usb_mode_options[i].mode;
        lv_obj_t * row = add_pill_option_row(usb_mode_list, usb_mode_options[i].label,
                                            selected, usb_mode_option_row_cb, (void *) (intptr_t) i);

        if (adb_active) {
            /* Storage/DAC aren't meaningful choices while ADB owns the USB
             * port -- dimmed and inert rather than removed, so the row
             * stays put (no layout jump) and it's visually clear these
             * exist but aren't the current mode. */
            lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_opa(row, LV_OPA_50, 0);
        }
    }
}

static void * usb_mode_switch_thread_func(void * arg) {
    (void) arg;
    usb_mode_switch_succeeded = usb_mode_control_apply(usb_mode_switch_target);
    atomic_store_explicit(&usb_mode_switch_done_flag, true, memory_order_release); /* written last -- poll_usb_mode_switch only checks this flag */
    return NULL;
}

static void * usb_bridge_restart_thread_func(void * arg) {
    (void) arg;
    usb_dac_bridge_start();
    atomic_store_explicit(&usb_bridge_restart_done, true, memory_order_release);
    return NULL;
}

/* Returns whether the worker actually started. A launch that fails runs no
 * poll, so nothing else will ever correct a control left flipped in
 * anticipation of it -- the caller has to know. */
static bool launch_usb_mode_switch(void) {
    usb_mode_switch_queued = false;
    atomic_store_explicit(&usb_mode_switch_done_flag, false, memory_order_relaxed);
    int rc = pthread_create(&usb_mode_switch_thread, NULL, usb_mode_switch_thread_func, NULL);
    if (rc != 0) {
        /* Without this rollback, a transient low-memory/thread-creation
         * failure leaves the guard latched forever: every later tap is
         * ignored until the player or device is restarted. */
        usb_mode_switch_active = false;
        show_error_toast("Could not start USB mode switch");
        return false;
    }
    return true;
}

/* Returns whether the request was accepted. A caller that flips its own UI
 * ahead of the hardware needs to know when a tap was dropped, or the control
 * ends up showing a state nothing is working toward. */
bool start_usb_mode_switch(usb_mode_t target) {
    if (usb_mode_switch_active) return false;
    gui_library_suspend_boot_prompt();
    usb_mode_switch_target = target;
    usb_mode_switch_active = true;
    /* Preserve the explicit request without letting gadget teardown race a
     * watchdog start already in flight. Polling launches it once safe. */
    usb_mode_switch_queued = usb_bridge_restart_active;
    if (!usb_mode_switch_queued) return launch_usb_mode_switch();
    return true;
}

/* The state the ADB control should show: the target while a switch is in
 * flight, otherwise the recorded mode. */
bool gui_network_adb_active(void) {
    if (usb_mode_switch_active) return usb_mode_switch_target == USB_MODE_ADB;
    return current_settings.usb_mode == (int) USB_MODE_ADB;
}

/* Whether ADB still has a claim on the port, which is deliberately more
 * cautious than gui_network_adb_active(). Leaving ADB does not free the port
 * until the switch actually lands, so the modes that cannot be chosen yet stay
 * dimmed for the whole transition in both directions rather than offering rows
 * whose taps the in-flight guard would only reject. */
static bool adb_owns_usb_port(void) {
    bool recorded_adb = current_settings.usb_mode == (int) USB_MODE_ADB;
    if (usb_mode_switch_active) return recorded_adb || usb_mode_switch_target == USB_MODE_ADB;
    return recorded_adb;
}

void poll_usb_mode_switch(void) {
    if (usb_bridge_restart_active &&
        atomic_load_explicit(&usb_bridge_restart_done, memory_order_acquire)) {
        usb_bridge_restart_active = false;
    }
    if (usb_mode_switch_queued && !usb_bridge_restart_active) {
        /* Deferred launch: same failure consequence as the immediate one, but
         * the original caller has long since returned, so the control it
         * flipped has to be corrected from here. */
        if (!launch_usb_mode_switch()) gui_settings_sync_adb_toggle();
    }
    /* If set to USB DAC mode but the audio bridge is not running, restart it.
     * Skipped while a mode switch is in flight to avoid racing
     * usb_mode_control_apply(). */
    if (!usb_mode_switch_active && !usb_bridge_restart_active &&
        current_settings.usb_mode == (int) USB_MODE_DAC) {
        usb_dac_stream_info_t info;
        usb_dac_bridge_get_stream_info(&info);
        if (!info.bridge_running) {
            pthread_attr_t attr;
            if (pthread_attr_init(&attr) == 0) {
                if (pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED) == 0) {
                    pthread_t thread;
                    atomic_store_explicit(&usb_bridge_restart_done, false, memory_order_relaxed);
                    usb_bridge_restart_active = true;
                    if (pthread_create(&thread, &attr, usb_bridge_restart_thread_func, NULL) != 0)
                        usb_bridge_restart_active = false;
                }
                pthread_attr_destroy(&attr);
            }
        }
    }

    if (!usb_mode_switch_active || usb_mode_switch_queued || !atomic_load_explicit(&usb_mode_switch_done_flag, memory_order_acquire)) return;
    usb_mode_switch_active = false;
    pthread_join(usb_mode_switch_thread, NULL);

    /* usb_mode_control_apply() only reports success once the target
     * gadget is actually verified bound, not just "the script ran" -- on
     * failure current_settings.usb_mode is deliberately left untouched
     * (the switch genuinely didn't happen) rather than optimistically
     * recording what was merely requested. */
    if (!usb_mode_switch_succeeded) {
        const char * label = "USB mode";
        for (size_t i = 0; i < USB_MODE_OPTION_COUNT; i++) {
            if (usb_mode_options[i].mode == usb_mode_switch_target) { label = usb_mode_options[i].label; break; }
        }
        char msg[64];
        snprintf(msg, sizeof(msg), "Failed to switch to %s", label);
        show_error_toast(msg);
        /* apply() tears the old gadget down before bringing the target up, so
         * a failure can leave neither bound -- the recorded mode is then not
         * merely unchanged, it is wrong. Re-read the hardware so the rollback
         * below reflects what is actually there. */
        usb_mode_t actual_usb_mode;
        if (usb_mode_control_detect_current(&actual_usb_mode))
            current_settings.usb_mode = (int) actual_usb_mode;
        populate_usb_mode_screen(); /* in case current_settings.usb_mode's actual row needs re-highlighting */
        gui_settings_sync_adb_toggle(); /* roll the optimistic flip back to what the hardware actually did */
        return;
    }

    /* Leaving Storage while the PC is still attached ends its access too. */
    if (current_settings.usb_mode == (int) USB_MODE_STORAGE && usb_mode_switch_target != USB_MODE_STORAGE &&
        usb_storage_session.host_seen)
        flush_sd_view_after_usb_storage();
    current_settings.usb_mode = (int) usb_mode_switch_target;
    settings_save(&current_settings);
    /* Process-lifetime truth for "the DAC overlay owns the UI", as opposed to
     * current_settings.usb_mode, which settings.h documents as only a UI hint
     * and which silently keeps a stale DAC value across a restart with no
     * cable attached (gui.c's startup correction only applies when
     * usb_mode_control_detect_current() can see a bound gadget, and it can't
     * when nothing is plugged in). Set here rather than next to the nav_push
     * below so leaving DAC mode clears it just as reliably. */
    usb_dac_mode_active = (usb_mode_switch_target == USB_MODE_DAC);
    populate_usb_mode_screen(); /* refresh which row shows the accent border */
    gui_settings_sync_adb_toggle();

    /* DAC mode takes over the whole screen with its own overlay (matching
     * the stock firmware's hiby_usb.view) -- the device can't sensibly be
     * used as a normal music player while it's presenting itself to a PC
     * as a USB sound card. Storage/ADB have no such conflict (the app
     * keeps running normally), so they don't get an overlay. */
    if (usb_mode_switch_target == USB_MODE_DAC) {
        /* Same three-way mutual exclusion as bt_dac_toggle_cb()/
         * airplay_toggle_cb() -- all three (local playback, Bluetooth DAC,
         * AirPlay) fight over the same hw:0,0 output. usb_dac_bridge_start()
         * (called from usb_mode_control_apply(), already run on the
         * background thread by this point) already called audio_stop()
         * itself, but that only stops the backend -- the play button icon
         * needs its own explicit refresh, same as every other call site
         * that stops playback out from under the UI. */
        if (audio_is_playing() || audio_is_paused()) {
            audio_stop();
            set_play_button_state(false);
            plugin_manager_notify_stopped();
        }
        if (current_settings.bt_dac_mode_enabled) {
            current_settings.bt_dac_mode_enabled = false;
            settings_save(&current_settings);
            start_bt_apply_output_settings(false, current_settings.bt_volume_sync_enabled);
        }
        if (current_settings.wifi_dac_mode_enabled) {
            current_settings.wifi_dac_mode_enabled = false;
            settings_save(&current_settings);
            airplay_control_stop();
        }
        nav_push(usb_dac_overlay_screen);
    }
}

/* A fresh boot intentionally has no gadget bound, but previously nothing
 * reacted when a PC was plugged in: Storage only appeared after manually
 * switching to DAC/ADB and back, because those taps happened to perform the
 * missing UDC bind. Reapply Storage on every physical connection edge so a
 * PC enumerates it immediately. Never override an active DAC/ADB session. */
/* The card stays mounted here while a PC writes it over USB storage, so the
 * kernel keeps cached names for folders and files the PC renamed or deleted:
 * a renamed Playlists folder still opened by its old name, listing files that
 * then failed to open. Once the PC is done, write back anything the player
 * wrote and drop the unused dentry/inode caches so every later lookup reads
 * the card again. No unmount; in-use entries are left alone by the kernel. */
static void flush_sd_view_after_usb_storage(void) {
#ifndef HOST_BUILD
    sync();
    int fd = open("/proc/sys/vm/drop_caches", O_WRONLY | O_CLOEXEC);
    if (fd < 0) return;
    if (write(fd, "2", 1) != 1) fprintf(stderr, "[usb] drop_caches failed after storage session\n");
    close(fd);
#endif
}

void poll_usb_storage_hotplug(void) {
    battery_external_power_state_t power = battery_get_external_power_state();
    /* A failed power-supply read is not an unplug. Preserve both the cable
     * edge and confirmed host session until power presence is known again. */
    if (power == BATTERY_EXTERNAL_POWER_UNKNOWN) return;
    bool connected = power == BATTERY_EXTERNAL_POWER_CONNECTED;
    bool storage_configured = false;
    if (connected && !usb_storage_session.host_seen && !usb_mode_switch_active &&
        (!usb_cable_state_initialized || !usb_cable_was_connected ||
         lv_tick_elaps(usb_storage_host_check_tick) >= 250)) {
        usb_storage_host_check_tick = lv_tick_get();
        storage_configured = usb_mode_control_storage_host_configured();
    }
    bool storage_was_active = gui_network_usb_storage_session_active();
    bool storage_session_ended = usb_storage_session_poll(&usb_storage_session,
                                                          connected, storage_configured);
    if (!usb_cable_state_initialized) {
        usb_cable_state_initialized = true;
        usb_cable_was_connected = connected;
        /* Storage is the default gadget when a PC is plugged in, including
         * a cable already present at boot. ADB and DAC stay opt-in from
         * Settings; usb_mode_control_apply() tears Storage down first. */
        usb_storage_rebind_pending = connected;
    } else if (connected && !usb_cable_was_connected) {
        usb_storage_rebind_pending = true;
    }
    usb_cable_was_connected = connected;
    if (!storage_was_active && gui_network_usb_storage_session_active())
        gui_library_suspend_boot_prompt();
    if (storage_session_ended) flush_sd_view_after_usb_storage();
    /* Not while the device is a USB sound card. host_seen latches for the
     * whole cable session, so a Storage phase before the user switched to
     * DAC (poll_usb_storage_hotplug() force-binds Storage on every fresh
     * connect) was still latched here and fired a rescan on unplug -- the
     * modal "Updating music database..." taking over the DAC overlay, long
     * after the host was done with the card. The latch itself is already
     * cleared by usb_storage_session_poll() above, so this only suppresses
     * the rescan; anything the PC wrote during that earlier Storage phase
     * is picked up by the next ordinary rescan trigger or a manual one from
     * Settings. */
    if (storage_session_ended && !usb_dac_mode_active && gui_library_auto_rescan_enabled()) {
        gui_library_request_files_changed_prompt();
    }

    if (!connected || !usb_storage_rebind_pending || usb_mode_switch_active) return;

    usb_mode_t live_mode;
    bool have_live_mode = usb_mode_control_detect_current(&live_mode);
    if (have_live_mode && (live_mode == USB_MODE_DAC || live_mode == USB_MODE_ADB)) {
        usb_storage_rebind_pending = false;
        return;
    }

    usb_storage_rebind_pending = false;
    start_usb_mode_switch(USB_MODE_STORAGE);
}

bool gui_network_usb_storage_session_active(void) {
    return usb_storage_session.host_seen && usb_cable_was_connected;
}

static void usb_mode_option_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    usb_mode_t target = usb_mode_options[index].mode;
    if (current_settings.usb_mode == (int) target) return;
    start_usb_mode_switch(target);
}

static lv_obj_t * build_usb_mode_screen(void) {
    lv_obj_t * title_label; /* unused after build -- title never changes */
    return build_subsonic_list_screen("USB Mode", &title_label, &usb_mode_list);
}

static void open_usb_mode_screen(void) {
    /* current_settings.usb_mode is only a UI hint (see settings.h's own
     * comment: never re-applied to hardware on startup, so it can't be
     * trusted to reflect reality after a reboot or any change made outside
     * this app) -- correct it against live gadget state whenever that
     * state is unambiguous, rather than showing a stale/wrong row
     * highlighted, which is exactly what was happening before (always
     * defaulted to showing Storage selected regardless of what was
     * actually connected). Just sysfs reads, fast enough for the UI
     * thread. */
    usb_mode_t detected;
    if (usb_mode_control_detect_current(&detected) && current_settings.usb_mode != (int) detected) {
        current_settings.usb_mode = (int) detected;
        settings_save(&current_settings);
    }
    populate_usb_mode_screen();
    nav_push(usb_mode_screen);
}

void usb_mode_settings_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    open_usb_mode_screen();
}

/* ---- USB DAC mode overlay + leave-confirmation popup --------------------
 * Full-screen takeover matching the stock firmware's hiby_usb.view (a
 * centered USB icon + a top-left back button, nothing else) -- deliberately
 * skips finalize_screen_navigation()'s swipe-to-back/swipe-to-player-screen
 * wiring, same reasoning as build_import_wifi_screen()'s own comment: the
 * only way out is the back button, which asks for confirmation rather than
 * leaving immediately, and an accidental swipe bypassing that would defeat
 * the point. The confirmation popup itself reuses bt_action_popup's own
 * hand-built top-layer overlay shape (this codebase doesn't use LVGL's
 * lv_msgbox anywhere). ---- */
static gui_popup_t usb_dac_leave_popup;

static void hide_usb_dac_leave_popup(void) {
    gui_popup_hide(&usb_dac_leave_popup);
}

static void usb_dac_leave_popup_backdrop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_usb_dac_leave_popup(); /* tap-outside = cancel, matches bt_action_popup precedent */
}

static void usb_dac_leave_cancel_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_usb_dac_leave_popup();
}

static void usb_dac_leave_confirm_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_usb_dac_leave_popup();
    nav_pop(); /* leave the DAC overlay screen */
    start_usb_mode_switch(USB_MODE_STORAGE); /* "switch back to Storage (Default)" */
}

static void usb_dac_overlay_back_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    gui_popup_show(&usb_dac_leave_popup);
}

void build_usb_dac_leave_popup(void) {
    usb_dac_leave_popup.popup = build_confirm_popup("Leave USB DAC mode?", LV_LABEL_LONG_WRAP, NULL, NULL, "Leave",
                                                     lv_color_make(255, 120, 120), usb_dac_leave_confirm_cb, NULL, "Cancel",
                                                     accent_lv_color(), usb_dac_leave_cancel_cb, NULL,
                                                     usb_dac_leave_popup_backdrop_cb, &usb_dac_leave_popup.backdrop);
}

static lv_obj_t * build_usb_dac_overlay_screen(void) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

    build_screen_header(scr, NULL, usb_dac_overlay_back_cb, NULL, NULL);

    lv_obj_t * icon = lv_image_create(scr);
    lv_image_set_src(icon, asset_path("usb/usb.png"));
    lv_obj_align(icon, LV_ALIGN_CENTER, 0, BOARD_SCALE_PX(-40));

    lv_obj_t * status_label = lv_label_create(scr);
    lv_label_set_text(status_label, "USB DAC mode");
    lv_obj_add_style(status_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(status_label, gui_theme_font(GUI_FONT_ROLE_TITLE), 0);
    lv_obj_align_to(status_label, icon, LV_ALIGN_OUT_BOTTOM_MID, 0, BOARD_SCALE_PX(24));

    /* Use fixed width with centered text alignment so the labels stay centered
     * when their text content updates dynamically. */
    usb_dac_hint_label = lv_label_create(scr);
    lv_label_set_text(usb_dac_hint_label, "Waiting for USB audio…");
    lv_obj_add_style(usb_dac_hint_label, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(usb_dac_hint_label, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_set_width(usb_dac_hint_label, LV_PCT(90));
    lv_obj_set_style_text_align(usb_dac_hint_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align_to(usb_dac_hint_label, status_label, LV_ALIGN_OUT_BOTTOM_MID, 0, BOARD_SCALE_PX(8));

    usb_dac_input_label = lv_label_create(scr);
    lv_obj_add_style(usb_dac_input_label, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(usb_dac_input_label, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_set_width(usb_dac_input_label, LV_PCT(90));
    lv_obj_set_style_text_align(usb_dac_input_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align_to(usb_dac_input_label, usb_dac_hint_label, LV_ALIGN_OUT_BOTTOM_MID, 0, BOARD_SCALE_PX(8));

    usb_dac_path_label = lv_label_create(scr);
    lv_obj_add_style(usb_dac_path_label, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(usb_dac_path_label, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_set_width(usb_dac_path_label, LV_PCT(90));
    lv_obj_set_style_text_align(usb_dac_path_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align_to(usb_dac_path_label, usb_dac_input_label, LV_ALIGN_OUT_BOTTOM_MID, 0, BOARD_SCALE_PX(4));

    return scr;
}

static void set_label_if_changed(lv_obj_t * label, const char * text) {
    if (label && strcmp(lv_label_get_text(label), text) != 0) lv_label_set_text(label, text);
}

static void format_rate(char * out, size_t size, unsigned int rate) {
    if (rate && rate % 1000 == 0) snprintf(out, size, "%u kHz", rate / 1000);
    else if (rate) snprintf(out, size, "%.1f kHz", (double) rate / 1000.0);
    else snprintf(out, size, "Unknown rate");
}

static lv_timer_t * dac_stream_labels_timer = NULL;

static void dac_stream_labels_timer_cb(lv_timer_t * timer) {
    (void) timer;
    bt_dac_stream_info_t bt;
    bt_control_get_dac_stream_info(&bt);
    char text[160];
    if (!bt.available || !bt.running) {
        snprintf(text, sizeof(text), "Waiting for Bluetooth stream…");
    } else {
        char rate[32];
        format_rate(rate, sizeof(rate), bt.sample_rate);
        const char * format = bt.bit_depth ? NULL : (bt.pcm_format[0] ? bt.pcm_format : "Unknown format");
        if (bt.bit_depth)
            snprintf(text, sizeof(text), "%s · %s · %u-bit", bt.codec[0] ? bt.codec : "Unknown codec", rate, bt.bit_depth);
        else
            snprintf(text, sizeof(text), "%s · %s · %s", bt.codec[0] ? bt.codec : "Unknown codec", rate, format);
    }
    set_label_if_changed(bt_dac_stream_label, text);

    usb_dac_stream_info_t usb;
    usb_dac_bridge_get_stream_info(&usb);
    set_label_if_changed(usb_dac_hint_label, usb.streaming ? "This device is now a USB sound card" : "Waiting for USB audio…");
    char input_rate[32], output_rate[32];
    format_rate(input_rate, sizeof(input_rate), usb.input_sample_rate);
    format_rate(output_rate, sizeof(output_rate), usb.output_sample_rate);
    snprintf(text, sizeof(text), "USB input: %s · %u-bit", input_rate, usb.input_bit_depth);
    set_label_if_changed(usb_dac_input_label, text);
    snprintf(text, sizeof(text), "DAC path: %s · %u-bit", output_rate, usb.output_bit_depth);
    set_label_if_changed(usb_dac_path_label, text);
}

static void bt_volume_sync_toggle_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    current_settings.bt_volume_sync_enabled = !current_settings.bt_volume_sync_enabled;
    settings_save(&current_settings);
    start_bt_apply_output_settings(current_settings.bt_dac_mode_enabled, current_settings.bt_volume_sync_enabled);
    populate_bt_advanced_screen(); /* the row lives on Advanced now */
}

/* Display filter toggle to hide unnamed Bluetooth devices (e.g. BLE beacons)
 * from the Available Devices list. Paired devices are never hidden. */
static void bt_hide_unnamed_toggle_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    current_settings.bt_hide_unnamed_devices = !current_settings.bt_hide_unnamed_devices;
    settings_save(&current_settings);
    populate_bt_advanced_screen(); /* moves the switch the user just tapped */
    populate_bt_screen();          /* and the device list it filters, for the way back */
}

/* ---- Master rebuild for the Bluetooth screen's one dynamic list: toggle
 * row, then (only while powered) volume sync/DAC/Codec rows, Paired
 * Devices, and Available Devices -- same full-rebuild-on-any-change
 * convention as populate_wifi_screen() above. bt_scan_results already holds
 * both paired and freshly-discovered devices from one bt_control_scan()
 * call; this just splits that single list into the two sections by
 * ->paired rather than needing two separate scans. ---- */
void populate_bt_screen(void) {
    /* Preserve scroll position across periodic screen rebuilds so the view
     * does not snap back to the top while browsing devices. */
    int32_t saved_scroll_y = lv_obj_get_scroll_y(bt_list);

    lv_obj_clean(bt_list);

    /* bt_is_powered_cached, not bt_control_is_powered() -- same reasoning
     * as external_dac_block_reason()'s own use of the cache (see its
     * comment): calling bt_control_is_powered() directly here forked
     * bluetoothctl show synchronously on the UI thread every time this
     * screen opened, which on a real device produced a multi-second freeze
     * entering the Bluetooth submenu (a milder version of the same
     * pre-existing hang class start_refresh_bt_icon()'s own comment
     * documents). The cache is kept fresh in the background regardless. */
    bool powered = bt_is_powered_cached;
    if (bt_rescan_btn) {
        if (powered) lv_obj_remove_flag(bt_rescan_btn, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(bt_rescan_btn, LV_OBJ_FLAG_HIDDEN);
    }
    /* quick_drawer_bt_event_cb is the SAME real toggle-thread trigger the
     * drawer's own bt icon uses -- see populate_wifi_screen()'s identical
     * reasoning for wifi. */
    add_pill_toggle_row(bt_list, "Bluetooth", powered, quick_drawer_bt_event_cb);

    if (!powered) {
        lv_obj_scroll_to_y(bt_list, saved_scroll_y, LV_ANIM_OFF);
        return;
    }

    add_pill_chevron_row(bt_list, "Bluetooth DAC", bt_dac_settings_row_cb);
    add_pill_chevron_row(bt_list, "Advanced", bt_advanced_settings_row_cb);

    add_section_header(bt_list, "Paired Devices");
    int paired_shown = 0;
    for (int i = 0; i < bt_scan_result_count; i++) {
        if (!bt_scan_results[i].paired) continue;
        add_bt_device_row(bt_list, i);
        paired_shown++;
    }
    if (paired_shown == 0) {
        lv_obj_t * label = lv_label_create(bt_list);
        lv_label_set_text(label, "No paired devices");
        lv_obj_add_style(label, &style_theme_text_muted, 0);
        lv_obj_set_style_pad_left(label, BOARD_SCALE_PX(24), 0);
    }

    add_section_header(bt_list, "Available Devices");
    int available_shown = 0;
    for (int i = 0; i < bt_scan_result_count; i++) {
        if (bt_scan_results[i].paired) continue;
        if (current_settings.bt_hide_unnamed_devices && bt_name_is_mac_placeholder(&bt_scan_results[i])) continue;
        add_bt_device_row(bt_list, i);
        available_shown++;
    }
    if (available_shown == 0) {
        lv_obj_t * label = lv_label_create(bt_list);
        lv_label_set_text(label, "No devices found");
        lv_obj_add_style(label, &style_theme_text_muted, 0);
        lv_obj_set_style_pad_left(label, BOARD_SCALE_PX(24), 0);
    }

    lv_obj_scroll_to_y(bt_list, saved_scroll_y, LV_ANIM_OFF);
}

static lv_obj_t * build_bluetooth_screen(void) {
    lv_obj_t * title_label; /* unused after build -- title never changes */
    lv_obj_t * scr = build_subsonic_list_screen("Bluetooth", &title_label, &bt_list);

    bt_rescan_btn = lv_label_create(scr);
    lv_label_set_text(bt_rescan_btn, "Rescan");
    lv_obj_set_style_text_color(bt_rescan_btn, accent_lv_color(), 0);
    lv_obj_set_style_text_font(bt_rescan_btn, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    align_screen_header_action(bt_rescan_btn, 20);
    lv_obj_add_flag(bt_rescan_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(bt_rescan_btn, bt_rescan_btn_cb, LV_EVENT_CLICKED, NULL);

    return scr;
}

void open_bluetooth_screen(void) {
    populate_bt_screen();
    nav_push(bt_screen);
    /* Auto-refresh on open -- cached power state, not a fresh
     * bt_control_is_powered() call, same reasoning as populate_bt_screen().
     * An inquiry is skipped while an accessory is connected: discovery makes
     * the controller interleave with the A2DP link and the music audibly
     * stutters for the whole scan window. The known-device list still
     * refreshes, and Rescan is there when discovery is actually wanted. */
    if (bt_is_powered_cached) start_bt_scan_ex(!gui_shell_is_bt_audio_connected());
}

static void bt_tile_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    open_bluetooth_screen();
}

/* ---- Import via Wi-Fi screen -- this is just the on-device "here's the
 * address to open" front end; the actual file-manager webapp is the stock
 * firmware's own thttpd + CGI binaries, managed via import_web.h. ---- */

static lv_obj_t * import_wifi_status_label;
static lv_obj_t * import_wifi_url_label;
#if LV_USE_QRCODE
static lv_obj_t * import_wifi_qrcode;
#endif

static void populate_import_wifi_screen(void) {
    wifi_settings_snapshot_start();
    if (!wifi_cached_info_is_current() || !wifi_cached_info_connected || wifi_cached_info.ip[0] == '\0') {
        lv_label_set_text(import_wifi_status_label, "Connect to Wi-Fi first");
        lv_label_set_text(import_wifi_url_label, "");
#if LV_USE_QRCODE
        lv_obj_add_flag(import_wifi_qrcode, LV_OBJ_FLAG_HIDDEN);
#endif
        return;
    }

    char url[64];
    snprintf(url, sizeof(url), "http://%s:4399", wifi_cached_info.ip);
    lv_label_set_text(import_wifi_status_label, "Open this address on your phone or computer:");
    lv_label_set_text(import_wifi_url_label, url);
#if LV_USE_QRCODE
    lv_obj_remove_flag(import_wifi_qrcode, LV_OBJ_FLAG_HIDDEN);
    lv_qrcode_update(import_wifi_qrcode, url, strlen(url));
#endif
}

/* Shared N-row action-menu popup shape is built by build_menu_popup in gui.c. */

/* Background worker and polling state for stopping the Wi-Fi web import server
 * asynchronously without blocking the UI thread during process teardown. */
static pthread_t import_web_stop_thread;
static bool import_web_stop_active = false;
static bool import_web_stop_via_service_worker = false;
static atomic_bool import_web_stop_done_flag = false;
/* Import via Wi-Fi screen's own stack slot, recorded right before the busy
 * screen gets pushed on top of it -- spliced out via nav_remove_stack_slot()
 * once teardown finishes (same pattern text_entry_commit()/
 * poll_subsonic_download() already use), so backing out later lands on
 * whatever was open before Import via Wi-Fi, not on this now-defunct
 * "Closing Web Server..." screen or a stale extra copy of the screen
 * underneath it. */
static int import_web_stop_nav_slot = -1;

/* True from a successful open_import_wifi_screen() until teardown (either
 * the back button or gui_network_handle_wifi_disabled()) is INITIATED --
 * i.e. "the web server is running or a stop for it hasn't been kicked off
 * yet". Distinct from import_web_stop_active (which only covers the actual
 * background-thread window): this is what lets gui_network_handle_wifi_
 * disabled() tell "nothing to do" apart from "need to tear down", and
 * flips false the instant teardown starts so a second caller (back button
 * racing the wifi-disabled path, or vice versa) sees it as already handled
 * rather than starting a second worker. */
static bool import_web_active = false;

/* Set by gui_network_handle_wifi_disabled() right before it kicks off the
 * same async stop worker/busy-overlay import_wifi_back_cb() below uses --
 * consumed once by poll_import_web_stop() so the automatic rescan below
 * only ever fires after a real user-initiated exit, never merely because
 * Wi-Fi disappeared out from under the screen (which may not have
 * downloaded anything worth rescanning for at all). */
static bool import_web_stop_suppress_rescan = false;

/* All service lifecycle calls below are process/socket teardown or setup and
 * must not run on the LVGL thread.  Keep one bounded, joinable worker and a
 * tiny FIFO so Wi-Fi-loss cleanup can serialize several stops without ever
 * overlapping a start/stop for the same service.  The worker contains no UI
 * code; completion is joined and consumed by poll_network_service_ops(). */
typedef struct {
    network_service_t service;
    bool start;
} network_service_job_t;

#define NETWORK_SERVICE_QUEUE_CAPACITY 8
static pthread_mutex_t network_service_mutex = PTHREAD_MUTEX_INITIALIZER;
static network_service_job_t network_service_queue[NETWORK_SERVICE_QUEUE_CAPACITY];
static unsigned network_service_queue_head;
static unsigned network_service_queue_count;
static bool network_service_busy_flags[NETWORK_SERVICE_COUNT];
static bool network_service_stop_queued[NETWORK_SERVICE_COUNT];
static bool network_service_worker_active;
static pthread_t network_service_thread;
static atomic_bool network_service_done = false;

static void poll_network_service_ops(void);

static void * network_service_worker_func(void * arg) {
    (void) arg;
    for (;;) {
        network_service_job_t job;
        pthread_mutex_lock(&network_service_mutex);
        if (network_service_queue_count == 0) {
            pthread_mutex_unlock(&network_service_mutex);
            break;
        }
        job = network_service_queue[network_service_queue_head];
        network_service_queue_head = (network_service_queue_head + 1) % NETWORK_SERVICE_QUEUE_CAPACITY;
        network_service_queue_count--;
        pthread_mutex_unlock(&network_service_mutex);

        if (job.service == NETWORK_SERVICE_IMPORT) {
            if (job.start) import_web_start();
            else import_web_stop();
        } else if (job.service == NETWORK_SERVICE_REMOTE) {
            if (job.start) {
                remote_control_start();
#ifndef HOST_BUILD
                bt_remote_control_start();
#endif
            } else {
                remote_control_stop();
#ifndef HOST_BUILD
                bt_remote_control_stop();
#endif
            }
        } else if (job.service == NETWORK_SERVICE_DLNA) {
            if (job.start) dlna_control_start();
            else dlna_control_stop();
        }
        pthread_mutex_lock(&network_service_mutex);
        bool same_service_queued = false;
        for (unsigned i = 0; i < network_service_queue_count; i++) {
            unsigned index = (network_service_queue_head + i) % NETWORK_SERVICE_QUEUE_CAPACITY;
            if (network_service_queue[index].service == job.service) {
                same_service_queued = true;
                break;
            }
        }
        if (!same_service_queued) network_service_busy_flags[job.service] = false;
        if (!job.start) network_service_stop_queued[job.service] = false;
        pthread_mutex_unlock(&network_service_mutex);
    }
    atomic_store_explicit(&network_service_done, true, memory_order_release);
    return NULL;
}

static bool network_service_is_busy(network_service_t service) {
    pthread_mutex_lock(&network_service_mutex);
    bool busy = network_service_busy_flags[service];
    pthread_mutex_unlock(&network_service_mutex);
    return busy;
}

static bool network_service_launch_locked(void) {
    atomic_store_explicit(&network_service_done, false, memory_order_relaxed);
    network_service_worker_active = true;
    if (pthread_create(&network_service_thread, NULL, network_service_worker_func, NULL) != 0) {
        network_service_worker_active = false;
        return false;
    }
    return true;
}

static bool network_service_enqueue(network_service_t service, bool start, bool force_stop) {
    /* A completed worker may still await the next UI poll. Reap it before
     * appending work, otherwise a new job could be stranded behind a thread
     * that has already exited. */
    poll_network_service_ops();
    pthread_mutex_lock(&network_service_mutex);
    if ((!force_stop && network_service_busy_flags[service]) ||
        (force_stop && network_service_stop_queued[service]) ||
        network_service_queue_count >= NETWORK_SERVICE_QUEUE_CAPACITY) {
        pthread_mutex_unlock(&network_service_mutex);
        return false;
    }
    unsigned tail = (network_service_queue_head + network_service_queue_count) % NETWORK_SERVICE_QUEUE_CAPACITY;
    bool was_busy = network_service_busy_flags[service];
    bool had_stop = network_service_stop_queued[service];
    network_service_queue[tail] = (network_service_job_t){ .service = service, .start = start };
    network_service_queue_count++;
    network_service_busy_flags[service] = true;
    if (!start) network_service_stop_queued[service] = true;
    if (!network_service_worker_active) {
        if (!network_service_launch_locked()) {
            network_service_queue_count--;
            network_service_busy_flags[service] = was_busy;
            network_service_stop_queued[service] = had_stop;
            network_service_worker_active = false;
            pthread_mutex_unlock(&network_service_mutex);
            return false;
        }
    }
    pthread_mutex_unlock(&network_service_mutex);
    return true;
}

static void poll_network_service_ops(void) {
    pthread_mutex_lock(&network_service_mutex);
    bool active = network_service_worker_active;
    pthread_mutex_unlock(&network_service_mutex);
    if (active && !atomic_load_explicit(&network_service_done, memory_order_acquire)) return;
    if (active) pthread_join(network_service_thread, NULL);
    pthread_mutex_lock(&network_service_mutex);
    if (active) network_service_worker_active = false;
    /* The worker may have observed an empty queue just before an enqueue
     * published a job and set done. Re-launch any such job while holding the
     * lifecycle lock; this closes the exit/enqueue handshake race. */
    if (network_service_queue_count > 0) {
        /* Retry creation on every UI poll. A transient pthread resource
         * failure must not strand a busy flag or an Import teardown. */
        (void) network_service_launch_locked();
    }
    pthread_mutex_unlock(&network_service_mutex);
}

static void * import_web_stop_thread_func(void * arg) {
    (void) arg;
    import_web_stop();
    atomic_store_explicit(&import_web_stop_done_flag, true, memory_order_release); /* written last -- poll_import_web_stop() only checks this flag */
    return NULL;
}

void poll_import_web_stop(void) {
    poll_network_service_ops();
    if (!import_web_stop_active) return;
    if (import_web_stop_via_service_worker) {
        if (network_service_is_busy(NETWORK_SERVICE_IMPORT)) return;
    } else if (!atomic_load_explicit(&import_web_stop_done_flag, memory_order_acquire)) {
        return;
    }

    import_web_stop_active = false;
    if (!import_web_stop_via_service_worker) pthread_join(import_web_stop_thread, NULL);
    import_web_stop_via_service_worker = false;

    if (import_web_stop_nav_slot >= 0 && import_web_stop_nav_slot < gui_navigation_get_depth()) {
        nav_remove_stack_slot(import_web_stop_nav_slot);
    }
    gui_busy_hide(import_web_stop_token);
    import_web_stop_nav_slot = -1;

    bool suppress_rescan = import_web_stop_suppress_rescan;
    import_web_stop_suppress_rescan = false;
    if (suppress_rescan) return;

    /* Automatically rescan with the "Updating music database..." busy
     * screen, preserving recovered snapshots and libraries that failed to load. */
    if (gui_library_auto_rescan_enabled()) start_library_auto_rescan();
}

/* Shared by import_wifi_back_cb() (user-initiated exit) and import_wifi_
 * handle_wifi_disabled() (Wi-Fi-loss-initiated) below -- both need the exact
 * same async stop-worker/busy-overlay sequence, just with a different
 * suppress_rescan value. import_web_active is deliberately only cleared
 * AFTER pthread_create() actually succeeds, not optimistically beforehand:
 * if thread launch fails, import_web_stop() never runs and thttpd/
 * udp_server stay up, so leaving import_web_active true keeps that
 * reflected -- a retry (another back-button tap, or a later Wi-Fi-disabled
 * confirmation) still sees a stop as needed instead of wrongly treating
 * this as already handled and silently leaving the servers running
 * forever. */
static void import_web_stop_start(bool suppress_rescan) {
    import_web_stop_suppress_rescan = suppress_rescan;
    import_web_stop_nav_slot = gui_navigation_get_depth() - 1; /* this screen's own slot, before pushing the busy screen on top of it */
    import_web_stop_token = gui_busy_show("Closing\nWeb Server...", "");

    import_web_stop_active = true;
    import_web_stop_via_service_worker = false;
    if (network_service_is_busy(NETWORK_SERVICE_IMPORT)) {
        if (!network_service_enqueue(NETWORK_SERVICE_IMPORT, false, true)) {
            import_web_stop_active = false;
            import_web_stop_suppress_rescan = false;
            gui_busy_hide(import_web_stop_token);
            show_error_toast("Thread launch failed");
            return;
        }
        import_web_stop_via_service_worker = true;
        import_web_active = false;
        return;
    }
    atomic_store_explicit(&import_web_stop_done_flag, false, memory_order_relaxed);
    if (pthread_create(&import_web_stop_thread, NULL, import_web_stop_thread_func, NULL) != 0) {
        import_web_stop_active = false;
        import_web_stop_suppress_rescan = false;
        gui_busy_hide(import_web_stop_token);
        show_error_toast("Thread launch failed");
        return; /* import_web_active stays true -- nothing was actually stopped */
    }
    import_web_active = false; /* only now that the stop is genuinely in flight */
}

static void import_wifi_back_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    import_web_stop_start(false); /* a real user exit always wants the rescan prompt */
}

/* Wi-Fi-disabled counterpart to import_wifi_back_cb() above -- triggered by
 * gui_network_handle_wifi_disabled() instead of the user's own back-button
 * tap, and skips the rescan popup on completion since nothing was actually
 * imported this session. import_web_active being true guarantees import_
 * wifi_screen is the currently active screen: it skips finalize_screen_
 * navigation() (no swipe-to-back/swipe-up-home), so the explicit back
 * button -- which only runs import_web_stop_start() once it actually
 * launches the worker -- is the only way to leave it. Guarding on
 * import_web_stop_active additionally means a teardown already in flight
 * (from either caller) is left alone rather than racing a second worker
 * against it. */
static void import_wifi_handle_wifi_disabled(void) {
    if (!import_web_active || import_web_stop_active) return; /* not running, or already tearing down */
    import_web_stop_start(true);
}

static lv_obj_t * build_import_wifi_screen(void) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_add_style(scr, &style_theme_screen_bg, 0);

    build_screen_header(scr, "Import via Wi-Fi", import_wifi_back_cb, NULL, NULL);

    import_wifi_status_label = lv_label_create(scr);
    lv_obj_set_width(import_wifi_status_label, lv_pct(90));
    lv_label_set_long_mode(import_wifi_status_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(import_wifi_status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_style(import_wifi_status_label, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(import_wifi_status_label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_obj_align(import_wifi_status_label, LV_ALIGN_TOP_MID, 0, STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT + BOARD_SCALE_PX(30));

#if LV_USE_QRCODE
    import_wifi_qrcode = lv_qrcode_create(scr);
    lv_qrcode_set_size(import_wifi_qrcode, BOARD_SCALE_PX(220));
    lv_qrcode_set_dark_color(import_wifi_qrcode, lv_color_black());
    lv_qrcode_set_light_color(import_wifi_qrcode, lv_color_white());
    lv_obj_set_style_border_width(import_wifi_qrcode, 4, 0);
    lv_obj_set_style_border_color(import_wifi_qrcode, lv_color_white(), 0);
    lv_obj_align(import_wifi_qrcode, LV_ALIGN_TOP_MID, 0, STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT + BOARD_SCALE_PX(110));
#endif

    import_wifi_url_label = lv_label_create(scr);
    lv_obj_set_style_text_color(import_wifi_url_label, accent_lv_color(), 0);
    lv_obj_set_style_text_font(import_wifi_url_label, gui_theme_font(GUI_FONT_ROLE_ROW), 0);
    lv_obj_align(import_wifi_url_label, LV_ALIGN_TOP_MID, 0, STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT + BOARD_SCALE_PX(350));

    /* Deliberately skips finalize_screen_navigation()'s swipe-to-back --
     * an accidental swipe here would leave thttpd/udp_server running with
     * nothing to tear them down, same reasoning as text_entry_screen
     * skipping it for its keyboard. The back button above is the only way
     * out, and always runs import_web_stop(). */
    return scr;
}

/* ---- Shared Wi-Fi dependency guard for AirPlay/DLNA/Import via Wi-Fi --
 * these require the Wi-Fi radio/interface to be enabled
 * (NOT association with an access point; individual screens keep showing
 * their own existing "connect first" state for that). Checks gui_shell.c's
 * effective state (an in-flight disable counts as already off, see that
 * function's own comment) rather than wifi_control_is_enabled() directly,
 * shows the exact required toast, and returns false -- callers must bail
 * out without navigating, touching a setting, or starting anything. Shared
 * by the tile taps below (screen not open yet) and each feature's own
 * enable/toggle path (screen may already be open when Wi-Fi goes away). */
static bool wifi_feature_guard(void) {
    if (gui_shell_wifi_effective_enabled()) return true;
    show_error_toast("Enable WiFi to access");
    return false;
}

static void open_import_wifi_screen(void) {
    if (!wifi_feature_guard()) return;
    if (import_web_active || import_web_stop_active || network_service_is_busy(NETWORK_SERVICE_IMPORT)) {
        show_info_toast("Web Server is busy");
        return;
    }
    wifi_settings_snapshot_invalidate();
    wifi_settings_snapshot_enabled = true;
    if (!network_service_enqueue(NETWORK_SERVICE_IMPORT, true, false)) {
        show_error_toast("Thread launch failed");
        return;
    }
    populate_import_wifi_screen();
    nav_push(import_wifi_screen);
    import_web_active = true;
}

static void import_wifi_tile_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    open_import_wifi_screen();
}

/* ---- AirPlay screen -- WiFi analogue of Bluetooth DAC mode, see
 * airplay_control.h for the real mechanism (stock firmware's own shairport
 * binary). Same shape as build_bt_dac_screen(): a toggle plus an
 * explanation, and the same three-way mutual exclusion with local playback
 * and Bluetooth DAC. ---- */

static lv_obj_t * airplay_list;

static void airplay_toggle_cb(lv_event_t * e);

static void populate_airplay_screen(void) {
    lv_obj_clean(airplay_list);
    add_pill_toggle_row(airplay_list, "AirPlay", current_settings.wifi_dac_mode_enabled, airplay_toggle_cb);

    lv_obj_t * explanation = lv_label_create(airplay_list);
    lv_label_set_text(explanation,
                      "When on, this device is visible to AirPlay senders on your Wi-Fi network -- stream "
                      "audio from an iPhone, iPad, or Mac to be played through this device's own output.");
    lv_obj_set_width(explanation, lv_pct(90));
    lv_label_set_long_mode(explanation, LV_LABEL_LONG_WRAP);
    lv_obj_add_style(explanation, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(explanation, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_set_style_pad_left(explanation, BOARD_SCALE_PX(24), 0);
    lv_obj_set_style_pad_top(explanation, BOARD_SCALE_PX(12), 0);
}

/* /usr/resource/hostname is the same file wifi_on.sh already reads for the
 * DHCP hostname it advertises -- reusing it here keeps the name AirPlay
 * senders see consistent with how this device already identifies itself on
 * the network, rather than inventing a second name. "HiBy_Music" matches
 * that script's own fallback default when the file is missing. */
void get_device_name(char * out, size_t out_size) {
    snprintf(out, out_size, "HiBy_Music");
    FILE * f = fopen("/usr/resource/hostname", "r");
    if (f) {
        if (fgets(out, (int) out_size, f)) out[strcspn(out, "\n")] = '\0';
        fclose(f);
    }
}

/* The settings screens below are built lazily, so anything reachable from
 * the quick drawer (which can toggle these long before their own screen has
 * ever been opened) has to refresh through these rather than calling
 * populate_*() directly -- lv_obj_clean() on a NULL list is not survivable. */
static void refresh_airplay_screen_if_built(void) {
    if (airplay_list) populate_airplay_screen();
}

/* Shared by the AirPlay settings screen's toggle row and the quick drawer's
 * AirPlay tile -- the drawer must not reimplement any of the Wi-Fi guard,
 * transactional-start, or BT-DAC mutual-exclusion logic below. Returns the
 * resulting enabled state, which is NOT always the requested one (a rejected
 * enable returns false). */
bool gui_network_toggle_airplay(void) {
    bool turning_on = !current_settings.wifi_dac_mode_enabled;
    /* Guard the enable path even though the tile itself is already guarded
     * (airplay_tile_cb() below) -- this screen can already be open when
     * Wi-Fi goes away, so a stale toggle tap must still be rejected here.
     * Checked before touching the setting at all, so a rejected enable
     * never even briefly persists as on. Turning OFF is always allowed,
     * Wi-Fi state notwithstanding -- an already-enabled feature must still
     * be toggleable off. */
    if (turning_on && !wifi_feature_guard()) {
        refresh_airplay_screen_if_built(); /* nothing changed, but keeps the toggle row's own drawn state honest */
        return current_settings.wifi_dac_mode_enabled;
    }
    current_settings.wifi_dac_mode_enabled = turning_on;
    settings_save_async(&current_settings);

    if (current_settings.wifi_dac_mode_enabled) {
        char name[64];
        get_device_name(name, sizeof(name));
        if (!airplay_control_start(name)) {
            /* Startup is transactional (see airplay_control_start()'s own
             * comment) -- a false return means nothing was actually left
             * running, so just undo the toggle rather than reporting the
             * device as "on" when it can't produce sound or be discovered. */
            current_settings.wifi_dac_mode_enabled = false;
            settings_save_async(&current_settings);
            show_error_toast("Failed to enable AirPlay");
            refresh_airplay_screen_if_built();
            return current_settings.wifi_dac_mode_enabled;
        }

        /* Deliberately does NOT touch local playback here -- toggling
         * AirPlay only makes this device discoverable/ready to receive
         * (airplay_bridge_start() starts in a LISTENING state, see its own
         * comment); an actual incoming AirPlay stream starting is what
         * stops local playback (gui_network.c's gui_network_poll_airplay_
         * overlay(), reacting to airplay_bridge_is_streaming()), not this
         * toggle. Still mutually exclusive with Bluetooth DAC mode
         * specifically, unlike local playback: BT DAC mode's bluealsa-aplay
         * writes straight to the local hw:0 device as an entirely separate
         * process, outside this app's own audio_output.c arbitration, so
         * the two really can conflict at the hardware level the moment
         * AirPlay does start streaming -- turning BT DAC off here, rather
         * than only when AirPlay actually starts streaming, avoids ever
         * having both simultaneously fighting over real hardware. */
        if (current_settings.bt_dac_mode_enabled) {
            current_settings.bt_dac_mode_enabled = false;
            settings_save(&current_settings);
            start_bt_apply_output_settings(false, current_settings.bt_volume_sync_enabled);
        }
    } else {
    }

    refresh_airplay_screen_if_built();
    return current_settings.wifi_dac_mode_enabled;
}

static void airplay_toggle_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    gui_network_toggle_airplay();
}

static lv_obj_t * build_airplay_screen(void) {
    lv_obj_t * title_label; /* unused after build -- title never changes */
    return build_subsonic_list_screen("AirPlay", &title_label, &airplay_list);
}

static void open_airplay_screen(void) {
    populate_airplay_screen();
    nav_push(airplay_screen);
}

static void airplay_tile_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!wifi_feature_guard()) return;
    open_airplay_screen();
}

/* ---- AirPlay overlay: shown only while a stream is actually active -----
 * Dedicated overlay screen displaying cover art, song name, and stream info
 * updated dynamically via airplay_metadata.c. Navigating back leaves the
 * stream running in the background until it completes or is stopped. ---- */

static void airplay_overlay_back_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_pop();
}

static lv_obj_t * build_airplay_overlay_screen(void) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

    build_screen_header(scr, NULL, airplay_overlay_back_cb, NULL, NULL);

    airplay_overlay_cover_img = lv_image_create(scr);
    lv_image_set_src(airplay_overlay_cover_img, asset_path("playing_plane/default_cover_565.png"));
    lv_obj_align(airplay_overlay_cover_img, LV_ALIGN_CENTER, 0, BOARD_SCALE_PX(-60));

    airplay_overlay_title_label = lv_label_create(scr);
    lv_label_set_text(airplay_overlay_title_label, "AirPlay");
    lv_obj_add_style(airplay_overlay_title_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(airplay_overlay_title_label, gui_theme_font(GUI_FONT_ROLE_TITLE), 0);
    lv_obj_set_width(airplay_overlay_title_label, lv_pct(85));
    lv_label_set_long_mode(airplay_overlay_title_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(airplay_overlay_title_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align_to(airplay_overlay_title_label, airplay_overlay_cover_img, LV_ALIGN_OUT_BOTTOM_MID, 0, BOARD_SCALE_PX(24));

    /* The AirPlay settings tile's own "wireless/airplay.png" is a stock
     * theme2 asset with a completely different (colored, wireless-grid)
     * style -- not the recognizable AirPlay-audio glyph (concentric arcs
     * over an upward triangle). This is a project-owned custom asset
     * instead: assets/theme2/playing_plane/airplay_logo_white.png, a
     * solid-white recolor of the standard glyph (transparent background,
     * alpha-preserved from the original anti-aliased edges) sized for this
     * overlay specifically. Resolved via asset_path()'s own THEME_OVERRIDE_
     * ROOT mechanism on target (see assets.c's own comment -- exactly the
     * "asset this app adds with no stock equivalent" case that exists for)
     * and committed directly under assets/theme2/ for the host build,
     * matching stream_media/subsonic.png's own precedent. */
    lv_obj_t * airplay_logo = lv_image_create(scr);
    lv_image_set_src(airplay_logo, asset_path("playing_plane/airplay_logo_white.png"));
    lv_obj_align(airplay_logo, LV_ALIGN_BOTTOM_MID, 0, BOARD_SCALE_PX(-24));

    return scr;
}

/* Poll from update_timer_cb, same shape as every other background-thread-
 * to-UI-thread poll in this codebase. Shows/hides the overlay based on
 * airplay_bridge_is_streaming() transitions and applies whatever metadata
 * has arrived meanwhile -- decoupled from whether the overlay happens to
 * be the currently active screen, so metadata keeps updating even if the
 * user navigated away from it. */
void gui_network_poll_airplay_overlay(void) {
    /* Self-heals a respawn failure from airplay_control_disconnect_active_
     * stream()'s kill-and-respawn (gui_player.c, triggered by local playback
     * resuming while AirPlay was streaming) -- that path deliberately
     * ignores airplay_control_start()'s return value (see its own comment),
     * so this is the only place such a failure ever gets noticed and the
     * persisted setting corrected, same as airplay_toggle_cb()'s own
     * synchronous failure handling a few hundred lines up in this file. */
    if (current_settings.wifi_dac_mode_enabled && !airplay_control_is_active()) {
        current_settings.wifi_dac_mode_enabled = false;
        settings_save(&current_settings);
        show_error_toast("AirPlay stopped unexpectedly");
        populate_airplay_screen();
    }

    bool streaming = airplay_bridge_is_streaming();
    if (streaming != airplay_overlay_showing) {
        airplay_overlay_showing = streaming;
        if (streaming) {
            /* airplay_bridge.c's own reader thread already called
             * audio_stop() before flipping is_streaming() true (see its own
             * comment) -- that only stops the backend, same as usb_mode_
             * control_apply()'s own DAC-mode takeover a few hundred lines up
             * in this file; the play button icon and plugin state still
             * need their own explicit refresh here on the UI thread, or
             * they'd keep claiming playback is active/paused for a track
             * that AirPlay just silently took the output device out from
             * under. */
            set_play_button_state(false);
            plugin_manager_notify_stopped();

            airplay_overlay_nav_slot = gui_navigation_get_depth(); /* this screen's own slot, about to be occupied */
            nav_push(airplay_overlay_screen);
        } else {
            /* Reset to defaults here, on SESSION END, not on the next
             * session's start -- metadata for a new session can (and
             * regularly does) arrive slightly before airplay_bridge_is_
             * streaming() flips true for it (see the unconditional-apply
             * comment below), and resetting on start would wipe that
             * early update right back out, leaving the "AirPlay"
             * placeholder up for the entire session since nothing else
             * would trigger a re-apply. Resetting here instead means by
             * the time a NEW session's early metadata can possibly arrive,
             * this session's leftover content is already gone -- closing
             * the same staleness gap without racing a legitimate early
             * update for the next session. */
            lv_label_set_text(airplay_overlay_title_label, "AirPlay");
            free(airplay_overlay_cover_bytes);
            airplay_overlay_cover_bytes = NULL;
            lv_image_set_src(airplay_overlay_cover_img, asset_path("playing_plane/default_cover_565.png"));

            if (lv_screen_active() == airplay_overlay_screen) {
                /* Only auto-dismiss if the user is still looking at it. */
                nav_pop();
            } else if (airplay_overlay_nav_slot >= 0 && airplay_overlay_nav_slot < gui_navigation_get_depth()) {
                /* User already navigated elsewhere with the overlay still
                 * buried somewhere in their back-navigation history --
                 * splice its now-defunct slot out entirely (same pattern as
                 * poll_import_web_stop()'s import_web_stop_nav_slot) rather
                 * than leaving it there for a later "back" to resurface a
                 * screen with no active stream behind it. */
                nav_remove_stack_slot(airplay_overlay_nav_slot);
            }
            airplay_overlay_nav_slot = -1;
        }
    }

    airplay_metadata_update_t upd;
    if (!airplay_metadata_consume_update(&upd)) return;

    /* Applied regardless of `streaming` -- the metadata and PCM bridge are
     * two independent threads/connections (airplay_metadata.c vs. airplay_
     * bridge.c), so a title/cover update for a session can arrive slightly
     * before airplay_bridge_is_streaming() actually flips true. Discarding
     * it here (an earlier version of this function did) meant the overlay
     * showed the plain "AirPlay" placeholder for the entire first track of
     * every session unless a later track change happened to resend it.
     * Applying it always is safe: the streaming->false reset above already
     * runs the instant a session ends, before a NEW session's own metadata
     * could possibly arrive, so nothing from a PREVIOUS session is ever
     * still sitting in these widgets by the time an early update for a new
     * one shows up here. */
    lv_label_set_text(airplay_overlay_title_label, upd.title[0] ? upd.title : "AirPlay");

    if (!upd.has_cover) {
        free(airplay_overlay_cover_bytes);
        airplay_overlay_cover_bytes = NULL;
        lv_image_set_src(airplay_overlay_cover_img, asset_path("playing_plane/default_cover_565.png"));
        return;
    }

    free(airplay_overlay_cover_bytes);
    airplay_overlay_cover_bytes = (uint8_t *) upd.cover_pixels;

    /* LV_IMAGE_HEADER_MAGIC is required so LVGL's image decoders recognize
     * the custom RGB565 image descriptor without attempting format fixups. */
    memset(&airplay_overlay_cover_dsc, 0, sizeof(airplay_overlay_cover_dsc));
    airplay_overlay_cover_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    airplay_overlay_cover_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    airplay_overlay_cover_dsc.header.w = AIRPLAY_COVER_WIDTH;
    airplay_overlay_cover_dsc.header.h = AIRPLAY_COVER_HEIGHT;
    airplay_overlay_cover_dsc.header.stride = AIRPLAY_COVER_WIDTH * 2;
    airplay_overlay_cover_dsc.data = airplay_overlay_cover_bytes;
    airplay_overlay_cover_dsc.data_size = (uint32_t) AIRPLAY_COVER_WIDTH * AIRPLAY_COVER_HEIGHT * 2;
    lv_image_set_src(airplay_overlay_cover_img, &airplay_overlay_cover_dsc);
}

/* ---- DLNA screen -- toggles the DLNA DMR service (dmrd) to allow UPnP/DLNA
 * controllers on the local Wi-Fi network to stream to this device. ---- */

static lv_obj_t * dlna_list;

static void dlna_toggle_cb(lv_event_t * e);

static void populate_dlna_screen(void) {
    lv_obj_clean(dlna_list);
    add_pill_toggle_row(dlna_list, "DLNA Renderer", current_settings.dlna_renderer_enabled, dlna_toggle_cb);

    lv_obj_t * explanation = lv_label_create(dlna_list);
    lv_label_set_text(explanation,
                      "When on, this device is visible to DLNA/UPnP controller apps on your Wi-Fi network -- cast "
                      "a track from one to play it here. Pause, mute, volume, and seek from the controller app "
                      "aren't supported; use this device's own controls instead once a track starts.");
    lv_obj_set_width(explanation, lv_pct(90));
    lv_label_set_long_mode(explanation, LV_LABEL_LONG_WRAP);
    lv_obj_add_style(explanation, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(explanation, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_set_style_pad_left(explanation, BOARD_SCALE_PX(24), 0);
    lv_obj_set_style_pad_top(explanation, BOARD_SCALE_PX(12), 0);
}

static void refresh_dlna_screen_if_built(void) {
    if (dlna_list) populate_dlna_screen();
}

/* gui_network_toggle_airplay()'s twin for DLNA -- same reason the drawer
 * routes through it rather than setting the flag itself. */
bool gui_network_toggle_dlna(void) {
    bool turning_on = !current_settings.dlna_renderer_enabled;
    /* Same defensive enable guard as airplay_toggle_cb() -- this screen can
     * already be open when Wi-Fi is disabled elsewhere, so the tile guard
     * alone (dlna_tile_cb() below) isn't sufficient. */
    if (turning_on && !wifi_feature_guard()) {
        refresh_dlna_screen_if_built();
        return current_settings.dlna_renderer_enabled;
    }
    if (network_service_is_busy(NETWORK_SERVICE_DLNA) ||
        !network_service_enqueue(NETWORK_SERVICE_DLNA, turning_on, false)) {
        show_info_toast("Service is busy");
        refresh_dlna_screen_if_built();
        return current_settings.dlna_renderer_enabled;
    }
    current_settings.dlna_renderer_enabled = turning_on;
    settings_save_async(&current_settings);

    refresh_dlna_screen_if_built();
    return current_settings.dlna_renderer_enabled;
}

static void dlna_toggle_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    gui_network_toggle_dlna();
}

static lv_obj_t * build_dlna_screen(void) {
    lv_obj_t * title_label; /* unused after build -- title never changes */
    return build_subsonic_list_screen("DLNA", &title_label, &dlna_list);
}

static void open_dlna_screen(void) {
    populate_dlna_screen();
    nav_push(dlna_screen);
}

/* ---- Remote Control screen (Wireless -> "Open Link") -- exposes the
 * HTTP address over Wi-Fi and the RFCOMM service name over Bluetooth.
 * Built on the same flex list as the DLNA/Wi-Fi screens: standard toggle
 * row, a "Connection PIN" row with a refresh icon to replace the PIN, then the
 * QR/URL lines styled like build_import_wifi_screen()'s -- the same
 * "here's an address, scan or type it" moment for a different feature. ---- */

static lv_obj_t * remote_control_toggle_img;
static lv_obj_t * remote_control_pin_label;
static char remote_control_displayed_pin[REMOTE_CONTROL_PIN_MAX_LENGTH + 1];
static bool remote_control_displayed_pin_valid;
static lv_obj_t * remote_control_status_label;
static lv_obj_t * remote_control_url_label;
static lv_obj_t * remote_control_bluetooth_label;
#if LV_USE_QRCODE
static lv_obj_t * remote_control_qrcode;
#endif

/* The screen is a flex column (build_subsonic_list_screen()), so positions
 * follow the content automatically. An empty label still occupies a text
 * line there, though, so hide the address lines whose text is empty instead
 * of leaving blank gaps. Call after every text change below. */
static void remote_control_hide_empty_lines(void) {
    lv_obj_t * lines[] = { remote_control_url_label, remote_control_bluetooth_label };
    for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) {
        const char * text = lv_label_get_text(lines[i]);
        if (text && text[0]) lv_obj_remove_flag(lines[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(lines[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static void remote_control_refresh_pin_label(void) {
    if (!remote_control_pin_label) return;
    char pin[REMOTE_CONTROL_PIN_MAX_LENGTH + 1] = {0};
    remote_control_get_pin(pin, sizeof(pin));
    if (remote_control_displayed_pin_valid && strcmp(remote_control_displayed_pin, pin) == 0) return;
    /* Empty only if the system random source failed; the server refuses to
     * start in that state too, so say so rather than show a fake PIN. */
    lv_label_set_text(remote_control_pin_label, pin[0] ? pin : "Unavailable");
    snprintf(remote_control_displayed_pin, sizeof(remote_control_displayed_pin), "%s", pin);
    remote_control_displayed_pin_valid = true;
}

static void remote_control_refresh_address(void) {
    remote_control_refresh_pin_label();
    if (!current_settings.remote_control_enabled) {
        lv_label_set_text(remote_control_status_label,
                           "Turn this on to see the address here.");
        lv_label_set_text(remote_control_url_label, "");
        lv_label_set_text(remote_control_bluetooth_label, "");
#if LV_USE_QRCODE
        lv_obj_add_flag(remote_control_qrcode, LV_OBJ_FLAG_HIDDEN);
#endif
        remote_control_hide_empty_lines();
        return;
    }

    wifi_settings_snapshot_start();
    bool wifi_available = gui_shell_wifi_effective_enabled() && wifi_cached_info_is_current() &&
                          wifi_cached_info_connected && wifi_cached_info.ip[0] != '\0';
    bool bluetooth_available = false;
    bool bluetooth_powered = false;
    bt_remote_control_status_t bluetooth_status = BT_REMOTE_CONTROL_DISABLED;
#ifndef HOST_BUILD
    bluetooth_powered = bt_is_powered_cached;
    bluetooth_status = bt_remote_control_status();
    bluetooth_available = bluetooth_powered && bt_remote_control_is_advertising();
#endif
    if (!wifi_available && !bluetooth_available) {
        if (bluetooth_powered && bluetooth_status == BT_REMOTE_CONTROL_FAILED)
            lv_label_set_text(remote_control_status_label, "Bluetooth remote service is unavailable; retrying.");
        else if (bluetooth_powered)
            lv_label_set_text(remote_control_status_label, "Bluetooth remote service is starting…");
        else
            lv_label_set_text(remote_control_status_label, "Enable Wi-Fi or Bluetooth to connect.");
        lv_label_set_text(remote_control_url_label, "");
        if (bluetooth_powered && bluetooth_status == BT_REMOTE_CONTROL_FAILED)
            lv_label_set_text(remote_control_bluetooth_label, "Bluetooth: registration failed; retrying");
        else if (bluetooth_powered)
            lv_label_set_text(remote_control_bluetooth_label, "Bluetooth: waiting for service registration");
        else
            lv_label_set_text(remote_control_bluetooth_label, "");
#if LV_USE_QRCODE
        lv_obj_add_flag(remote_control_qrcode, LV_OBJ_FLAG_HIDDEN);
#endif
        remote_control_hide_empty_lines();
        return;
    }

    lv_label_set_text(remote_control_status_label, "Connect using either available route:");
    if (wifi_available) {
        char url[64];
        snprintf(url, sizeof(url), "Wi-Fi: http://%s:8899", wifi_cached_info.ip);
        lv_label_set_text(remote_control_url_label, url);
    } else {
        lv_label_set_text(remote_control_url_label, "");
    }
    if (bluetooth_available) lv_label_set_text(remote_control_bluetooth_label, "Bluetooth: Compas Remote Control");
    else if (bluetooth_powered && bluetooth_status == BT_REMOTE_CONTROL_FAILED)
        lv_label_set_text(remote_control_bluetooth_label, "Bluetooth: registration failed; retrying");
    else if (bluetooth_powered)
        lv_label_set_text(remote_control_bluetooth_label, "Bluetooth: waiting for service registration");
    else lv_label_set_text(remote_control_bluetooth_label, "");
#if LV_USE_QRCODE
    if (wifi_available) {
        char url[64];
        snprintf(url, sizeof(url), "http://%s:8899", wifi_cached_info.ip);
        lv_obj_remove_flag(remote_control_qrcode, LV_OBJ_FLAG_HIDDEN);
        lv_qrcode_update(remote_control_qrcode, url, strlen(url));
    } else {
        lv_obj_add_flag(remote_control_qrcode, LV_OBJ_FLAG_HIDDEN);
    }
#endif
    remote_control_hide_empty_lines();
}

static void refresh_remote_control_screen_if_built(void) {
    if (remote_control_toggle_img) remote_control_refresh_address();
}

#ifdef TEST_BOOT_RC
/* gui_init invokes this on the LVGL thread after all screens are built. Sync
 * the cached screen's switch and address before starting asynchronous service
 * bring-up so the test-only boot state is visible immediately. */
void gui_network_start_test_boot_services(void) {
    bool expected = false;
    if (!atomic_compare_exchange_strong(&test_boot_services_started, &expected, true)) return;
    if (remote_control_toggle_img)
        lv_obj_add_state(remote_control_toggle_img, LV_STATE_CHECKED);
    refresh_remote_control_screen_if_built();

    pthread_t thread;
    if (pthread_create(&thread, NULL, test_boot_services_worker, NULL) != 0) {
        atomic_store(&test_boot_services_started, false);
        DBG_LOG("test boot RC: could not create startup worker\n");
        return;
    }
    pthread_detach(thread);
}
#endif

/* gui_network_toggle_airplay()'s twin for the phone remote-control server --
 * shown as "RC" on the quick drawer's own tile, where there is no room for
 * the full name. */
bool gui_network_toggle_remote_control(void) {
    bool turning_on = !current_settings.remote_control_enabled;
    if (network_service_is_busy(NETWORK_SERVICE_REMOTE) ||
        !network_service_enqueue(NETWORK_SERVICE_REMOTE, turning_on, false)) {
        show_info_toast("Service is busy");
        refresh_remote_control_screen_if_built();
        return current_settings.remote_control_enabled;
    }
    current_settings.remote_control_enabled = turning_on;
    settings_save_async(&current_settings);

    /* Real lv_switch now (see build_remote_control_screen()'s own comment)
     * -- CHECKED state alone drives its visual, no sprite swap needed. */
    if (remote_control_toggle_img) {
        if (current_settings.remote_control_enabled) lv_obj_add_state(remote_control_toggle_img, LV_STATE_CHECKED);
        else lv_obj_clear_state(remote_control_toggle_img, LV_STATE_CHECKED);
    }
    refresh_remote_control_screen_if_built();
    return current_settings.remote_control_enabled;
}

static void remote_control_toggle_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    gui_network_toggle_remote_control();
}

/* ---- New-PIN confirmation for the PIN row's refresh icon. Regenerating
 * immediately locks out every app and browser still using the old PIN, so it
 * asks first, like the other disruptive actions in this file
 * (bt_dac_leave_popup). Built once in
 * gui_network_init() on lv_layer_top(), torn down in gui_network_teardown(). */
static gui_popup_t remote_control_new_pin_popup;

static void remote_control_new_pin_popup_dismiss_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    gui_popup_hide(&remote_control_new_pin_popup);
}

static void remote_control_new_pin_confirm_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    gui_popup_hide(&remote_control_new_pin_popup);
    if (!remote_control_generate_new_pin()) {
        show_info_toast("Could not generate a new PIN");
        return;
    }
    remote_control_refresh_pin_label();
    show_info_toast("New PIN generated");
}

static void remote_control_regenerate_pin_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    gui_popup_show(&remote_control_new_pin_popup);
}

static void build_remote_control_new_pin_popup(void) {
    remote_control_new_pin_popup.popup = build_confirm_popup(
        "Generate a new PIN?", LV_LABEL_LONG_WRAP, NULL,
        "Apps and browsers using the current PIN will need the new one to reconnect.", "Generate",
        lv_color_make(255, 120, 120), remote_control_new_pin_confirm_cb, NULL, "Cancel", accent_lv_color(),
        remote_control_new_pin_popup_dismiss_cb, NULL, remote_control_new_pin_popup_dismiss_cb,
        &remote_control_new_pin_popup.backdrop);
}

/* Standard settings-list layout (build_subsonic_list_screen(), the same
 * flex column as the Wi-Fi and Bluetooth screens): the on/off toggle row,
 * then the PIN row with a refresh icon to replace the PIN. The connection
 * details follow as wrapped text lines. */
static lv_obj_t * build_remote_control_screen(void) {
    lv_obj_t * title_label;
    lv_obj_t * list;
    lv_obj_t * scr = build_subsonic_list_screen("Remote Control", &title_label, &list);

    lv_obj_t * toggle_row = add_pill_toggle_row(list, "Remote Control", current_settings.remote_control_enabled,
                                                remote_control_toggle_cb);
    /* add_pill_toggle_row() creates the label first and the switch last;
     * gui_network_toggle_remote_control() drives this switch's state. */
    remote_control_toggle_img = lv_obj_get_child(toggle_row, -1);

    lv_obj_t * pin_row = add_pill_row_base(list, "Connection PIN");

    /* Refresh icon at the row's trailing edge (same -20 inset as the toggle
     * and chevron rows) regenerates the PIN; LV_SYMBOL_REFRESH comes from the
     * Montserrat fallback chain behind the app font. The glyph is small, so
     * widen its touch area. */
    lv_obj_t * regenerate_icon = lv_label_create(pin_row);
    lv_label_set_text(regenerate_icon, LV_SYMBOL_REFRESH);
    lv_obj_set_style_text_color(regenerate_icon, accent_lv_color(), 0);
    lv_obj_set_style_text_font(regenerate_icon, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_obj_align(regenerate_icon, LV_ALIGN_RIGHT_MID, -20, 0);
    lv_obj_add_flag(regenerate_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(regenerate_icon, BOARD_SCALE_PX(20));
    lv_obj_add_event_cb(regenerate_icon, remote_control_regenerate_pin_cb, LV_EVENT_CLICKED, NULL);

    /* The PIN sits just left of the icon. lv_obj_align() (unlike
     * lv_obj_align_to()) keeps the right-edge anchor as the text changes
     * between a PIN and "Unavailable". */
    lv_obj_update_layout(regenerate_icon);
    int32_t icon_width = lv_obj_get_width(regenerate_icon);
    remote_control_pin_label = lv_label_create(pin_row);
    lv_obj_add_style(remote_control_pin_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(remote_control_pin_label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_obj_align(remote_control_pin_label, LV_ALIGN_RIGHT_MID, -20 - icon_width - BOARD_SCALE_PX(16), 0);
    remote_control_displayed_pin_valid = false;
    remote_control_refresh_pin_label();

    lv_obj_t * explanation = lv_label_create(list);
    lv_label_set_text(explanation,
                      "Connect over Wi-Fi or Bluetooth to see what's playing, control playback, and browse "
                      "your library. Enter this PIN when the app or browser asks for it; Bluetooth also "
                      "requires pairing.");
    lv_obj_set_width(explanation, lv_pct(90));
    lv_label_set_long_mode(explanation, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(explanation, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_style(explanation, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(explanation, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_set_style_pad_top(explanation, BOARD_SCALE_PX(12), 0);

    remote_control_status_label = lv_label_create(list);
    lv_obj_set_width(remote_control_status_label, lv_pct(90));
    lv_label_set_long_mode(remote_control_status_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(remote_control_status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_style(remote_control_status_label, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(remote_control_status_label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);

#if LV_USE_QRCODE
    remote_control_qrcode = lv_qrcode_create(list);
    lv_qrcode_set_size(remote_control_qrcode, BOARD_SCALE_PX(220));
    lv_qrcode_set_dark_color(remote_control_qrcode, lv_color_black());
    lv_qrcode_set_light_color(remote_control_qrcode, lv_color_white());
    lv_obj_set_style_border_width(remote_control_qrcode, 4, 0);
    lv_obj_set_style_border_color(remote_control_qrcode, lv_color_white(), 0);
    lv_obj_add_flag(remote_control_qrcode, LV_OBJ_FLAG_HIDDEN);
#endif

    remote_control_url_label = lv_label_create(list);
    lv_obj_set_width(remote_control_url_label, lv_pct(90));
    lv_label_set_long_mode(remote_control_url_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(remote_control_url_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(remote_control_url_label, accent_lv_color(), 0);
    lv_obj_set_style_text_font(remote_control_url_label, gui_theme_font(GUI_FONT_ROLE_ROW), 0);
    remote_control_bluetooth_label = lv_label_create(list);
    lv_obj_set_width(remote_control_bluetooth_label, lv_pct(90));
    lv_label_set_long_mode(remote_control_bluetooth_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(remote_control_bluetooth_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_style(remote_control_bluetooth_label, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(remote_control_bluetooth_label, gui_theme_font(GUI_FONT_ROLE_ROW), 0);
    lv_obj_set_style_pad_bottom(remote_control_bluetooth_label, BOARD_SCALE_PX(24), 0);
    /* open_remote_control_screen() fills these in every time the screen
     * opens; hide the still-empty lines until then. */
    remote_control_hide_empty_lines();

    return scr;
}

static void open_remote_control_screen(void) {
    wifi_settings_snapshot_invalidate();
    wifi_settings_snapshot_enabled = true;
    remote_control_refresh_address();
    nav_push(remote_control_screen);
}

static void remote_control_tile_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    open_remote_control_screen();
}

static void dlna_tile_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!wifi_feature_guard()) return;
    open_dlna_screen();
}

/* Centralized Wi-Fi-disabled cleanup for Wi-Fi-dependent features
 * -- called by gui_shell.c's poll_wifi_toggle() only once a Wi-Fi disable is
 * authoritatively confirmed (see that call site's own comment). Stops
 * whichever of AirPlay/DLNA/Import via Wi-Fi is actually running and
 * corrects their persisted settings, refreshing any of their
 * screens/toggles already built so nothing shows stale "on" state. A single
 * settings_save() at the end covers every flag this pass touched, rather
 * than one write per feature. */
void gui_network_handle_wifi_disabled(void) {
    bool settings_changed = false;

    /* Subsonic requests are ordinary background sockets, not persistent
     * services with a settings toggle. Interrupt them before tearing down
     * the rest of the Wi-Fi-owned features so their active flags cannot
     * veto idle suspend after the radio is already off. */
    gui_subsonic_handle_wifi_disabled();

    if (current_settings.wifi_dac_mode_enabled) {
        airplay_control_stop();
        current_settings.wifi_dac_mode_enabled = false;
        settings_changed = true;
        populate_airplay_screen();
    }

    if (current_settings.dlna_renderer_enabled) {
        if (network_service_enqueue(NETWORK_SERVICE_DLNA, false, true)) {
            current_settings.dlna_renderer_enabled = false;
            settings_changed = true;
            populate_dlna_screen();
        }
    }

    /* Import via Wi-Fi has no persisted enabled flag (session-only, started
     * only when its screen opens) -- import_wifi_handle_wifi_disabled() is
     * a cheap no-op whenever it isn't currently running. */
    import_wifi_handle_wifi_disabled();

    if (settings_changed) settings_save(&current_settings);
}

static lv_obj_t * build_wireless_screen(void) {
    static icon_grid_item_t items[6];
    /* No .bg_image here: the stock per-tile background pill isn't present in
     * every board's own firmware (missing on R1, so the tile silently rendered
     * without one there) and measurably added render/decode cost on R3 Pro II
     * for a background that isn't load-bearing to this screen. */
    items[0] = (icon_grid_item_t){ "wireless/list_wifi.png", NULL, "Wi-Fi", wifi_tile_cb, NULL };
    items[1] = (icon_grid_item_t){ "wireless/list_bt.png", NULL, "Bluetooth", bt_tile_cb, NULL };
    items[2] = (icon_grid_item_t){ "wireless/list_airplay.png", NULL, "AirPlay", airplay_tile_cb, NULL };
    items[3] = (icon_grid_item_t){ "wireless/list_dlna.png", NULL, "DLNA", dlna_tile_cb, NULL };
    items[4] = (icon_grid_item_t){ "wireless/list_remote.png", NULL, "Remote", remote_control_tile_cb, NULL };
    items[5] = (icon_grid_item_t){ "wireless/list_import.png", NULL, "Import", import_wifi_tile_cb, NULL };
    lv_obj_t * scr = build_category_menu_screen("Wireless", generic_back_cb, items, 6,
                                                &launcher_layout_config.wireless);
    finalize_screen_navigation(scr);
    return scr;
}

void gui_network_refresh_wireless_screen(void) {
    lv_obj_t * old = wireless_screen;
    lv_obj_t * fresh = build_wireless_screen();
    if (!fresh) return;
    wireless_screen = fresh;
    gui_navigation_replace_static_screen(3, old, fresh);
    if (old) lv_obj_delete(old);
}

void gui_network_init(void) {
    wifi_screen = build_wifi_screen();
    wifi_info_screen = build_wifi_info_screen();
    wifi_dns_screen = build_wifi_dns_screen();
    bt_screen = build_bluetooth_screen();
    bt_dac_screen = build_bt_dac_screen();
    bt_dac_overlay_screen = build_bt_dac_overlay_screen();
    bt_codec_screen = build_bt_codec_screen();
    bt_advanced_screen = build_bt_advanced_screen();
    bt_rate_screen = build_bt_rate_screen();
    font_size_screen = build_font_size_screen();
    replaygain_mode_screen = build_replaygain_mode_screen();
    resume_mode_screen = build_resume_mode_screen();
    play_pause_button_mode_screen = build_play_pause_button_mode_screen();
    usb_mode_screen = build_usb_mode_screen();
    usb_dac_overlay_screen = build_usb_dac_overlay_screen();
    import_wifi_screen = build_import_wifi_screen();
    airplay_screen = build_airplay_screen();
    airplay_overlay_screen = build_airplay_overlay_screen();
    dlna_screen = build_dlna_screen();
    remote_control_screen = build_remote_control_screen();
    wireless_screen = build_wireless_screen();
    build_bt_action_popup();
    build_wifi_action_popup();
    build_usb_dac_leave_popup();
    build_bt_dac_leave_popup();
    build_remote_control_new_pin_popup();
    dac_stream_labels_timer_cb(NULL);
    /* Guarded like gui_library.c's az_index_drag_timer -- gui_network_init()
     * can run again after a UI reload, and an unguarded lv_timer_create()
     * here would leak/duplicate a recurring timer each time. */
    if (!dac_stream_labels_timer) dac_stream_labels_timer = lv_timer_create(dac_stream_labels_timer_cb, 500, NULL);
}

/* For gui_reload.c's in-process UI reload -- deletes every screen/popup this
 * module owns so gui_network_init() can rebuild them from a clean slate
 * without leaking the old objects. Deliberately does NOT touch
 * dac_stream_labels_timer (already guarded/reused correctly by gui_network_
 * init() itself). The five popup-and-backdrop pairs below are built
 * directly on lv_layer_top() (see build_confirm_popup()'s own comment), not
 * as children of any of these screens, so each needs its own explicit
 * deletion. */
void gui_network_teardown(void) {
    wifi_settings_snapshot_enabled = false;
    wifi_settings_snapshot_invalidate();
    gui_popup_teardown(&bt_action_popup);
    gui_popup_teardown(&wifi_action_popup);
    gui_popup_teardown(&usb_dac_leave_popup);
    gui_popup_teardown(&bt_dac_leave_popup);
    gui_popup_teardown(&remote_control_new_pin_popup);

    /* The Remote Control screen's widgets die with remote_control_screen
     * below; clear the pointers so the quick-drawer toggle and the periodic
     * poll (which check them) can't touch freed objects before a rebuild. */
    remote_control_toggle_img = NULL;
    remote_control_pin_label = NULL;
    remote_control_displayed_pin_valid = false;
    remote_control_status_label = NULL;
    remote_control_url_label = NULL;
    remote_control_bluetooth_label = NULL;
#if LV_USE_QRCODE
    remote_control_qrcode = NULL;
#endif

    if (wifi_screen) { lv_obj_delete(wifi_screen); wifi_screen = NULL; }
    if (wifi_info_screen) { lv_obj_delete(wifi_info_screen); wifi_info_screen = NULL; }
    if (wifi_dns_screen) { lv_obj_delete(wifi_dns_screen); wifi_dns_screen = NULL; }
    if (bt_screen) { lv_obj_delete(bt_screen); bt_screen = NULL; }
    if (bt_dac_screen) { lv_obj_delete(bt_dac_screen); bt_dac_screen = NULL; }
    if (bt_dac_overlay_screen) { lv_obj_delete(bt_dac_overlay_screen); bt_dac_overlay_screen = NULL; }
    if (bt_codec_screen) { lv_obj_delete(bt_codec_screen); bt_codec_screen = NULL; }
    if (bt_advanced_screen) { lv_obj_delete(bt_advanced_screen); bt_advanced_screen = NULL; }
    if (bt_rate_screen) { lv_obj_delete(bt_rate_screen); bt_rate_screen = NULL; }
    if (font_size_screen) { lv_obj_delete(font_size_screen); font_size_screen = NULL; }
    if (replaygain_mode_screen) { lv_obj_delete(replaygain_mode_screen); replaygain_mode_screen = NULL; }
    if (resume_mode_screen) { lv_obj_delete(resume_mode_screen); resume_mode_screen = NULL; }
    if (play_pause_button_mode_screen) { lv_obj_delete(play_pause_button_mode_screen); play_pause_button_mode_screen = NULL; }
    if (usb_mode_screen) { lv_obj_delete(usb_mode_screen); usb_mode_screen = NULL; }
    if (usb_dac_overlay_screen) { lv_obj_delete(usb_dac_overlay_screen); usb_dac_overlay_screen = NULL; }
    if (import_wifi_screen) { lv_obj_delete(import_wifi_screen); import_wifi_screen = NULL; }
    if (airplay_screen) { lv_obj_delete(airplay_screen); airplay_screen = NULL; }
    if (airplay_overlay_screen) { lv_obj_delete(airplay_overlay_screen); airplay_overlay_screen = NULL; }
    if (dlna_screen) { lv_obj_delete(dlna_screen); dlna_screen = NULL; }
    if (remote_control_screen) { lv_obj_delete(remote_control_screen); remote_control_screen = NULL; }
    if (wireless_screen) { lv_obj_delete(wireless_screen); wireless_screen = NULL; }
}

bool gui_network_has_background_work(void) {
    pthread_mutex_lock(&network_service_mutex);
    bool network_service_work = network_service_worker_active || network_service_queue_count > 0;
    pthread_mutex_unlock(&network_service_mutex);
    return wifi_connect_active || wifi_connect_saved_active || wifi_disconnect_active ||
           wifi_scan_active || wifi_forget_active || wifi_settings_snapshot_active ||
           bt_connect_active || bt_forget_active ||
           bt_scan_active || usb_mode_switch_active || usb_bridge_restart_active || import_web_stop_active ||
           network_service_work;
}

void gui_network_cancel_background_work(void) {
    /* The detached bridge restart owns no UI objects and may be stuck in
     * start() indefinitely. Leave its guard intact; never join it here. */
    if (wifi_connect_active) {
        pthread_join(wifi_connect_thread, NULL);
        wifi_connect_active = false;
        gui_busy_hide(wifi_connect_token);
    }
    if (wifi_connect_saved_active) {
        pthread_join(wifi_connect_saved_thread, NULL);
        wifi_connect_saved_active = false;
        gui_busy_hide(wifi_connect_saved_token);
    }
    if (wifi_disconnect_active) {
        pthread_join(wifi_disconnect_thread, NULL);
        wifi_disconnect_active = false;
    }
    if (wifi_scan_active) {
        pthread_join(wifi_scan_thread, NULL);
        wifi_scan_active = false;
    }
    if (wifi_forget_active) {
        pthread_join(wifi_forget_thread, NULL);
        wifi_forget_active = false;
    }
    if (wifi_settings_snapshot_active) {
        pthread_join(wifi_settings_snapshot_thread, NULL);
        wifi_settings_snapshot_active = false;
    }
    if (bt_connect_active) {
        pthread_join(bt_connect_thread, NULL);
        bt_connect_active = false;
    }
    if (bt_forget_active) {
        pthread_join(bt_forget_thread, NULL);
        bt_forget_active = false;
    }
    if (bt_rate_query_active) {
        pthread_join(bt_rate_query_thread, NULL);
        bt_rate_query_active = false;
    }
    if (bt_rate_apply_active) {
        pthread_join(bt_rate_apply_thread, NULL);
        bt_rate_apply_active = false;
    }
    if (bt_scan_active) {
        pthread_join(bt_scan_thread, NULL);
        bt_scan_active = false;
    }
    if (usb_mode_switch_active) {
        if (!usb_mode_switch_queued) pthread_join(usb_mode_switch_thread, NULL);
        usb_mode_switch_queued = false;
        usb_mode_switch_active = false;
    }
    /* Drain the serialized service worker before teardown. If a worker could
     * not be created, execute its bounded queue synchronously so cancellation
     * cannot leave lifecycle flags or an Import busy overlay permanently set. */
    pthread_mutex_lock(&network_service_mutex);
    bool service_worker_active = network_service_worker_active;
    pthread_t service_thread = network_service_thread;
    pthread_mutex_unlock(&network_service_mutex);
    if (service_worker_active) pthread_join(service_thread, NULL);
    pthread_mutex_lock(&network_service_mutex);
    network_service_worker_active = false;
    pthread_mutex_unlock(&network_service_mutex);
    for (;;) {
        pthread_mutex_lock(&network_service_mutex);
        if (network_service_queue_count == 0) {
            pthread_mutex_unlock(&network_service_mutex);
            break;
        }
        network_service_job_t job = network_service_queue[network_service_queue_head];
        network_service_queue_head = (network_service_queue_head + 1) % NETWORK_SERVICE_QUEUE_CAPACITY;
        network_service_queue_count--;
        network_service_busy_flags[job.service] = false;
        if (!job.start) network_service_stop_queued[job.service] = false;
        pthread_mutex_unlock(&network_service_mutex);
        if (job.service == NETWORK_SERVICE_IMPORT) {
            if (job.start) import_web_start(); else import_web_stop();
        } else if (job.service == NETWORK_SERVICE_REMOTE) {
            if (job.start) {
                remote_control_start();
#ifndef HOST_BUILD
                bt_remote_control_start();
#endif
            } else {
                remote_control_stop();
#ifndef HOST_BUILD
                bt_remote_control_stop();
#endif
            }
        } else if (job.service == NETWORK_SERVICE_DLNA) {
            if (job.start) dlna_control_start(); else dlna_control_stop();
        }
    }
    if (import_web_stop_active) {
        if (!import_web_stop_via_service_worker) pthread_join(import_web_stop_thread, NULL);
        import_web_stop_active = false;
        import_web_stop_via_service_worker = false;
        gui_busy_hide(import_web_stop_token);
    }
}


lv_obj_t * gui_network_get_wifi_screen(void) { return wifi_screen; }
lv_obj_t * gui_network_get_bt_screen(void) { return bt_screen; }
lv_obj_t * gui_network_get_wireless_screen(void) { return wireless_screen; }
lv_obj_t * gui_network_get_bt_dac_overlay(void) { return bt_dac_overlay_screen; }
lv_obj_t * gui_network_get_usb_dac_overlay(void) { return usb_dac_overlay_screen; }

bool gui_network_usb_dac_mode_active(void) { return usb_dac_mode_active; }
lv_obj_t * gui_network_get_import_wifi_screen(void) { return import_wifi_screen; }

bool gui_network_usb_prompt_invalidated(void) {
    /* Check the live UDC state so a confirm click between polls cannot race
     * a newly-configured USB storage host. */
    return usb_mode_switch_active || usb_mode_switch_queued || usb_bridge_restart_active ||
           usb_dac_mode_active || gui_network_usb_storage_session_active() ||
           usb_mode_control_storage_host_configured();
}

bool gui_network_boot_prompt_blocked(void) {
    lv_obj_t * active = lv_screen_active();
    return gui_network_usb_prompt_invalidated() || active == bt_dac_screen || active == bt_dac_overlay_screen ||
           active == usb_dac_overlay_screen || active == airplay_overlay_screen;
}
