#include <limits.h>
#include "board_config.h"
#include "usb_mode_control.h"
#include "file_browser.h"
#include "remote_control.h"
#include "hiby_sys_server.h"
#include "backlight.h"
#include "led_control.h"
#include "charge_limiter.h"
#include "headphone_status.h"

/* ---- Playback state and advance machinery ---- */
static char ** playlist = NULL;
static int playlist_count = 0;
static int playlist_index = -1;
static char now_playing_genre[128];
static int now_playing_track_number;
static int * playlist_lazy_sort_order = NULL;
static bool playlist_lazy_order_is_recency = false;
static struct tagcache_snapshot * playlist_lazy_snapshot = NULL;
static file_browser_index_t * playlist_lazy_file_index = NULL;
static int queued_pending_count = 0;
static int queue_sd_dirfd = -1;
static int queue_next_insert_index = -1;
static int consecutive_decoder_failure_skips = 0;
#define MAX_FAILED_PHYSICAL_PATHS 5
static char failed_physical_paths[MAX_FAILED_PHYSICAL_PATHS][PATH_MAX];
static int failed_physical_paths_count = 0;
static uint64_t current_playback_generation = 0;
static int open_sd_queue_dir(void);
static int open_queue_dir_from_snapshot(const struct tagcache_snapshot *snapshot);
static int open_queue_dir_from_file_index(const file_browser_index_t *index);
static void queue_save_cancel(void);
static bool queue_save_active(void);
static bool is_sd_card_path(const char *path);

#include "audio_helpers.h"
#include "hw_volume_coalesce.h"
#include "gui_player.h"
#include "app_clock.h"
#include "gui.h"
#include "gui_theme.h"
#include "gui_notifications.h"
#include "gui_library.h"
#include "gui_queue.h"
#include "playlist_files.h"
#include "path_cache.h"
#include "playback_order.h"
#include "queue_resume.h"
#ifdef HOST_BUILD
#define MUSIC_ROOT_DIR "./music"
#else
#define MUSIC_ROOT_DIR "/data/mnt/sd_0"
#endif
#define PLAYLISTS_DIR MUSIC_ROOT_DIR "/Playlists"
#define SUBSONIC_STREAM_CACHE_DIR "/tmp/subsonic_stream_cache"
#include "gui_lyrics.h"
#include "frosted_glass.h"
#include "player_layout.h"
#include "gui_track_info.h"
#include "gui_settings.h"
#include "gui_subsonic.h"
#include "gui_lock_screen.h"
#include "fallback_font.h"
#include "http_client.h"
#include "screen_builders.h"
#include "metadata.h"
#include "metadata_db.h"
#include "cover_decode.h"
#include "db_log.h"
#include "airplay_control.h"
#include "albumart.h"
#include "audio.h"
#include "settings.h"
#include "assets.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#include "device_config.h"
#include "storage_paths.h"
#include "plugin_manager.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <limits.h>
#include <ctype.h>
#include <math.h>
#include <sys/stat.h>

#define VOLUME_POPUP_TIMEOUT_MS 3000

static lv_obj_t * player_screen = NULL;
lv_obj_t * player_dismiss_btn = NULL;
lv_obj_t * player_overlay_panel = NULL;
lv_obj_t * player_background_img = NULL;
lv_obj_t * cover_card = NULL;
lv_obj_t * cover_img = NULL;
lv_obj_t * artist_label = NULL;
lv_obj_t * album_label = NULL;
lv_obj_t * song_quality_label = NULL;
lv_obj_t * song_bitrate_label = NULL;
lv_obj_t * song_track_label = NULL;
lv_obj_t * song_count_label = NULL;
lv_obj_t * song_title_label = NULL;
lv_obj_t * quality_pill = NULL;
lv_obj_t * format_badge_label = NULL;
lv_obj_t * play_mode_img = NULL;
static lv_obj_t * order_icon = NULL;
lv_obj_t * favorite_circle = NULL;
lv_obj_t * favorite_icon = NULL;
lv_obj_t * play_btn = NULL;
lv_obj_t * prev_btn = NULL;
lv_obj_t * next_btn = NULL;
lv_obj_t * progress_slider = NULL;
lv_obj_t * progress_label = NULL;
lv_obj_t * duration_label = NULL;
lv_obj_t * volume_slider = NULL;

static lv_obj_t * pos_label = NULL;
static lv_obj_t * dur_label = NULL;

static int32_t displayed_progress_percent = -1;
static int displayed_position_second = -1;
static int displayed_duration_second = -1;

lv_obj_t * volume_popup = NULL;
lv_obj_t * volume_popup_backdrop = NULL;
lv_obj_t * more_menu_popup = NULL;
lv_obj_t * more_menu_popup_backdrop = NULL;

static lv_obj_t * volume_popup_track = NULL;
static lv_obj_t * volume_popup_speaker_icon = NULL;
static lv_obj_t * volume_popup_value_label = NULL;
static lv_timer_t * volume_popup_hide_timer = NULL;
static asset_decoded_image_t volume_popup_speaker_image;
/* Decoded copies of btn_play.png / btn_pause.png with the baked-in cyan
 * glyph rewritten to the current accent. Kept across widget teardown so
 * the quick-drawer button (destroyed later in gui_shell_teardown) never
 * holds a freed src pointer during reload. */
static asset_decoded_image_t play_btn_play_img;
static asset_decoded_image_t play_btn_pause_img;
static void load_play_btn_images(void);
static gui_popup_t delete_song_popup;
static lv_obj_t * delete_song_popup_title = NULL;

static lv_image_dsc_t current_cover_dsc;

static uint8_t * current_cover_bytes = NULL;
static int current_cover_for_index = -1;

static uint8_t * current_reflection_bytes = NULL;
static lv_image_dsc_t current_reflection_dsc;

extern lv_obj_t * volume_topbar_btn;
extern lv_obj_t * volume_topbar_label;
extern lv_obj_t * status_bar;
extern lv_obj_t * gui_settings_get_eq_screen();
extern player_settings_t current_settings;
extern bool favorite_is_set;

extern void nav_push(lv_obj_t * screen);
extern void nav_pop(void);
extern void finalize_screen_navigation(lv_obj_t * screen);
extern void generic_back_cb(lv_event_t * e);
extern void quick_drawer_mark_snapshot_dirty(void);
extern void clear_player_source(void);
extern void prev_btn_event_cb(lv_event_t * e);
extern void play_btn_event_cb(lv_event_t * e);
extern void next_btn_event_cb(lv_event_t * e);
extern void play_mode_tap_event_cb(lv_event_t * e);
extern void play_mode_long_press_cb(lv_event_t * e);
extern void slider_event_cb(lv_event_t * e);
extern const char * play_mode_icon_asset(play_mode_t mode);

void poll_volume_popup_timeout(void) {
    if (volume_popup && !lv_obj_has_flag(volume_popup, LV_OBJ_FLAG_HIDDEN)) {
        /* handled by timer */
    }
}


/* Transient volume popup (Task #28 / closes Task #17): built once, hidden,
 * on the top layer -- same reasoning as the status bar, but shown only for
 * a couple of seconds after the hardware volume buttons change the level,
 * then auto-hidden, instead of a permanently visible slider. */

static void volume_popup_hide_timer_cb(lv_timer_t * timer) {
    (void) timer;
    lv_obj_add_flag(volume_popup, LV_OBJ_FLAG_HIDDEN);
    lv_timer_pause(volume_popup_hide_timer);
}

/* hw_volume_coalesce.h -- extracted from what used to be this file's own
 * private volume_hw_pending/volume_hw_apply_timer/volume_drag_active
 * machinery, so the quick drawer's own volume slider (gui_shell.c) can
 * reuse the exact same, already-proven debounce instead of a second,
 * simpler, worse copy of it. Behavior here is unchanged. */
static hw_volume_coalesce_t volume_popup_hv = { .pending = -1 };

/* Handles touch/drag interactions on the popup volume slider. Its knob
 * follows LVGL directly; secondary displays and hardware consume the
 * coalesced value above. */
static void volume_popup_track_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    int32_t percent = lv_slider_get_value(lv_event_get_target(e));

    if (code == LV_EVENT_PRESSED) {
        hw_volume_coalesce_drag_begin(&volume_popup_hv);
        /* Stop the 1.5s auto-hide countdown while a finger's still on it --
         * otherwise a slow drag could get hidden out from under the user
         * mid-interaction. */
        lv_timer_pause(volume_popup_hide_timer);
    } else if (code == LV_EVENT_VALUE_CHANGED) {
        hw_volume_coalesce_drag_update(&volume_popup_hv, (int) percent);
        if (volume_popup_value_label) lv_label_set_text_fmt(volume_popup_value_label, "%d", (int) percent);
        refresh_volume_topbar(percent);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        hw_volume_coalesce_drag_end(&volume_popup_hv, (int) percent);
        if (volume_slider) lv_slider_set_value(volume_slider, percent, LV_ANIM_OFF);
        current_settings.volume = (float) percent / 100.0f;
        settings_save_async(&current_settings);
        lv_timer_reset(volume_popup_hide_timer);
        lv_timer_resume(volume_popup_hide_timer);
    }
}

/* The stock rail sprites are 360 px wide. LVGL centers a background image
 * at its native size instead of scaling it to the part, so they protrude
 * from the 360 px volume rail and the dynamically narrower brightness rail.
 * Paint those two rails natively; knobs use the shared vector ring style. */
void configure_native_slider_rail(lv_obj_t * slider) {
    lv_obj_set_style_bg_image_src(slider, NULL, LV_PART_MAIN);
    lv_obj_set_style_bg_image_src(slider, NULL, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_make(132, 134, 132), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
}

bool gui_player_volume_control_hit_test(lv_point_t point) {
    if (!volume_popup_track || !volume_popup ||
        lv_obj_has_flag(volume_popup, LV_OBJ_FLAG_HIDDEN)) return false;
    lv_area_t area;
    lv_obj_get_coords(volume_popup_track, &area);
    lv_area_increase(&area, 24, 24); /* matches build_volume_popup()'s hit area */
    return point.x >= area.x1 && point.x <= area.x2 &&
           point.y >= area.y1 && point.y <= area.y2;
}

/* Same card as the quick drawer's volume row (gui_shell.c): 413x73 rounded
 * panel, speaker icon, rail and a right-aligned numeric value, so the
 * hardware-button indicator reads exactly like the drawer's control. */
static void build_volume_popup(void) {
    lv_obj_t * top = lv_layer_top();

    volume_popup = lv_obj_create(top);
    lv_obj_remove_style_all(volume_popup);
    lv_obj_set_size(volume_popup, BOARD_SCALE_PX(413), BOARD_SCALE_PY(73));
    lv_obj_align(volume_popup, LV_ALIGN_TOP_MID, 0, STATUS_BAR_CLEARANCE + BOARD_SCALE_PX(12));
    lv_obj_set_style_bg_color(volume_popup, lv_color_hex(0x151b17), 0);
    lv_obj_set_style_bg_opa(volume_popup, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(volume_popup, BOARD_SCALE_PX(23), 0);
    lv_obj_remove_flag(volume_popup, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(volume_popup, LV_OBJ_FLAG_HIDDEN);

    volume_popup_speaker_icon = lv_image_create(volume_popup);
    const void * speaker = asset_decoded_image_open(&volume_popup_speaker_image, "volume/vol.png")
                         ? asset_decoded_image_source(&volume_popup_speaker_image) : NULL;
    lv_image_set_src(volume_popup_speaker_icon, speaker ? speaker : asset_path("volume/vol.png"));
    lv_obj_align(volume_popup_speaker_icon, LV_ALIGN_LEFT_MID, BOARD_SCALE_PX(21), 0);

    /* Drawer geometry relative to its card (x=34): track at 103, value
     * right edge at 428, a 16px gap before the widest value ("100"). */
    lv_point_t value_size;
    lv_text_get_size(&value_size, "100", &app_font_player_meta, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    int32_t track_w = BOARD_SCALE_PX(428) - value_size.x - BOARD_SCALE_PX(16) - BOARD_SCALE_PX(103);
    if (track_w > BOARD_SCALE_PX(300)) track_w = BOARD_SCALE_PX(300);
    if (track_w < BOARD_SCALE_PX(120)) track_w = BOARD_SCALE_PX(120);

    volume_popup_value_label = lv_label_create(volume_popup);
    lv_obj_add_style(volume_popup_value_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(volume_popup_value_label, &app_font_player_meta, 0);
    lv_obj_align(volume_popup_value_label, LV_ALIGN_RIGHT_MID, -BOARD_SCALE_PX(19), 0);
    lv_label_set_text_fmt(volume_popup_value_label, "%d", (int) gui_player_get_volume_percent());

    volume_popup_track = lv_slider_create(volume_popup);
    lv_obj_set_size(volume_popup_track, track_w, SLIDER_TRACK_HEIGHT);
    lv_obj_align(volume_popup_track, LV_ALIGN_LEFT_MID, BOARD_SCALE_PX(69), 0);
    lv_slider_set_range(volume_popup_track, 0, 100);
    lv_obj_set_style_bg_color(volume_popup_track, lv_color_black(), LV_PART_MAIN);
    lv_obj_add_style(volume_popup_track, gui_theme_accent_style(), LV_PART_INDICATOR);
    lv_obj_add_style(volume_popup_track, gui_theme_accent_knob_style(), LV_PART_KNOB);
    configure_native_slider_rail(volume_popup_track);
    lv_obj_set_style_bg_opa(volume_popup_track, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_width(volume_popup_track, SLIDER_KNOB_SIZE, LV_PART_KNOB);
    lv_obj_set_style_height(volume_popup_track, SLIDER_KNOB_SIZE, LV_PART_KNOB);
    lv_obj_add_event_cb(volume_popup_track, volume_popup_track_event_cb, LV_EVENT_ALL, NULL);

    /* Forgiving vertical capture area for fast drags; the hit test in
     * gui_player_volume_control_hit_test() uses the same 24px margin. */
    lv_obj_set_ext_click_area(volume_popup_track, 24);

    volume_popup_hide_timer = lv_timer_create(volume_popup_hide_timer_cb, 1500, NULL);
    lv_timer_pause(volume_popup_hide_timer);
    /* hw_volume_coalesce_drag_begin() creates its own timer lazily on the
     * first drag -- no eager creation needed here. */
}

/* Generates a "frosted mirror" look behind the transport controls
 * (player_overlay_panel, built in build_player_screen()): a heavily
 * blurred, darkened, vertically-mirrored copy of the album art,
 * replacing the flat buttom.png placeholder. Generated fresh from the
 * per-track RGB565 buffer decoded by cover_decode_to_rgb565(). */
static inline int32_t player_x(int32_t px) { return BOARD_SCALE_PX(px); }
static inline int32_t player_y(int32_t px) { return (int32_t) (((int64_t) px * BOARD_SCREEN_HEIGHT + 400) / 800); }
static inline int32_t player_s(int32_t px) {
    int32_t sx = player_x(px), sy = player_y(px);
    return sx < sy ? sx : sy;
}

#define REFLECTION_WIDTH BOARD_SCREEN_WIDTH
/* Small work-buffer + bilinear-both-ways architecture, matching gui_lyrics.c's
 * own proven compute_lyrics_backdrop_bytes() exactly (same work size, same
 * blur radius/passes, same bilinear downsample-then-upsample shape) instead
 * of this function's own earlier nearest-neighbor row-duplication approach.
 * That earlier approach hard-repeated each small-buffer source row into
 * ~2-3 destination rows with a hard boundary between them -- real banding in
 * the underlying image that no amount of per-pixel dithering can fix, only
 * mask. The lyrics screen never showed this "shimmer" on the same hardware
 * with the same darken range; this is the actual reason why, not the
 * dither-timing or panel-compositing theories tried (and fixed as real, but
 * insufficient) earlier. Also dramatically cheaper: 6000 work pixels here
 * vs the old 153600, closing the audio-thread-starvation risk at the root
 * instead of needing the cooperative-yield workaround. */
#define REFLECTION_WORK_WIDTH 60
#define REFLECTION_WORK_HEIGHT 100
#define REFLECTION_BLUR_RADIUS 5
#define REFLECTION_BLUR_PASSES 3
/* Kept as an integer ratio (channel * NUM / DEN) rather than a float --
 * matches this codebase's general preference for integer arithmetic on the
 * embedded target, and there's no accuracy need here that would justify a
 * float. */
#define REFLECTION_DARKEN_NUM 1
#define REFLECTION_DARKEN_DEN 2

/* Same bilinear-sample helper as gui_lyrics.c's own bilerp_plane() (not
 * shared between the two files -- both are small, self-contained, and this
 * keeps compute_reflection_bytes() free-standing). x_fp/y_fp are 16.16
 * fixed-point coordinates into `plane`. */
static uint8_t bilerp_plane(const uint8_t * plane, int width, int height, uint32_t x_fp, uint32_t y_fp) {
    int x0 = (int) (x_fp >> 16), y0 = (int) (y_fp >> 16);
    int x1 = x0 + 1 < width ? x0 + 1 : x0;
    int y1 = y0 + 1 < height ? y0 + 1 : y0;
    uint32_t fx = x_fp & 0xffffu, fy = y_fp & 0xffffu;
    uint32_t top = ((uint32_t) plane[y0 * width + x0] * (65536u - fx) +
                    (uint32_t) plane[y0 * width + x1] * fx) >> 16;
    uint32_t bottom = ((uint32_t) plane[y1 * width + x0] * (65536u - fx) +
                       (uint32_t) plane[y1 * width + x1] * fx) >> 16;
    return (uint8_t) ((top * (65536u - fy) + bottom * fy) >> 16);
}

/* Pure pixel math -- builds the reflection from a given COVER_ART_WIDTH x
 * COVER_ART_HEIGHT RGB565 buffer and returns a freshly malloc'd
 * REFLECTION_WIDTH x BOARD_SCREEN_HEIGHT RGB565 buffer, or NULL on allocation
 * failure. Caller owns the result. Accepts blur radius, passes, and darken
 * fraction parameters with defensive clamping. */
uint8_t * compute_reflection_bytes(const uint8_t * cover_bytes, int blur_radius, int blur_passes, int darken_num, int darken_den) {
    if (darken_den <= 0) darken_den = 1;
    if (blur_radius < 0) blur_radius = 0;
    if (blur_passes < 0) blur_passes = 0;
    if (darken_num < 0) darken_num = 0;

    int w = REFLECTION_WORK_WIDTH, h = REFLECTION_WORK_HEIGHT;
    uint8_t * r = malloc((size_t) w * h);
    uint8_t * g = malloc((size_t) w * h);
    uint8_t * b = malloc((size_t) w * h);
    uint8_t * tmp = malloc((size_t) w * h);
    if (!r || !g || !b || !tmp) {
        free(r); free(g); free(b); free(tmp);
        return NULL;
    }

    /* Bilinear downsample straight from the real (COVER_ART_WIDTH x
     * COVER_ART_HEIGHT) cover into this tiny work buffer, center-cropping
     * the source to REFLECTION_WIDTH:BOARD_SCREEN_HEIGHT's aspect first --
     * same shape as compute_lyrics_backdrop_bytes()'s own crop_w/crop_x,
     * adapted to this function's own destination aspect ratio. */
    int crop_w = COVER_ART_HEIGHT * REFLECTION_WIDTH / BOARD_SCREEN_HEIGHT;
    if (crop_w > COVER_ART_WIDTH) crop_w = COVER_ART_WIDTH;
    int crop_x = (COVER_ART_WIDTH - crop_w) / 2;
    const uint16_t * source = (const uint16_t *) cover_bytes;
    for (int y = 0; y < h; y++) {
        uint32_t sy_fp = h > 1 ? (uint32_t) (((uint64_t) y * (COVER_ART_HEIGHT - 1) << 16) / (h - 1)) : 0;
        int sy = (int) (sy_fp >> 16);
        int sy1 = sy + 1 < COVER_ART_HEIGHT ? sy + 1 : sy;
        uint32_t fy = sy_fp & 0xffffu;
        for (int x = 0; x < w; x++) {
            uint32_t sx_fp = (uint32_t) crop_x << 16;
            if (w > 1) sx_fp += (uint32_t) (((uint64_t) x * (crop_w - 1) << 16) / (w - 1));
            int sx = (int) (sx_fp >> 16);
            int sx1 = sx + 1 < COVER_ART_WIDTH ? sx + 1 : sx;
            uint32_t fx = sx_fp & 0xffffu;
            uint16_t px00 = source[sy * COVER_ART_WIDTH + sx], px01 = source[sy * COVER_ART_WIDTH + sx1];
            uint16_t px10 = source[sy1 * COVER_ART_WIDTH + sx], px11 = source[sy1 * COVER_ART_WIDTH + sx1];
            uint16_t pixels[4] = { px00, px01, px10, px11 };
            uint8_t * channels[3] = { r, g, b };
            for (int c = 0; c < 3; c++) {
                uint32_t values[4];
                for (int p = 0; p < 4; p++) {
                    if (c == 0) values[p] = ((pixels[p] >> 11) & 0x1F) * 255 / 31;
                    else if (c == 1) values[p] = ((pixels[p] >> 5) & 0x3F) * 255 / 63;
                    else values[p] = (pixels[p] & 0x1F) * 255 / 31;
                }
                uint32_t top = (values[0] * (65536u - fx) + values[1] * fx) >> 16;
                uint32_t bottom = (values[2] * (65536u - fx) + values[3] * fx) >> 16;
                channels[c][y * w + x] = (uint8_t) ((top * (65536u - fy) + bottom * fy) >> 16);
            }
        }
    }

    /* Separable box blur on the tiny work buffer -- one horizontal pass
     * (per row) then one vertical pass (per column), on each channel
     * independently, for blur_passes passes. */
    for (int pass = 0; pass < blur_passes; pass++) {
        for (int y = 0; y < h; y++) box_blur_1d(r + y * w, tmp + y * w, w, 1, blur_radius);
        memcpy(r, tmp, (size_t) w * h);
        for (int x = 0; x < w; x++) box_blur_1d(r + x, tmp + x, h, w, blur_radius);
        memcpy(r, tmp, (size_t) w * h);
    }
    for (int pass = 0; pass < blur_passes; pass++) {
        for (int y = 0; y < h; y++) box_blur_1d(g + y * w, tmp + y * w, w, 1, blur_radius);
        memcpy(g, tmp, (size_t) w * h);
        for (int x = 0; x < w; x++) box_blur_1d(g + x, tmp + x, h, w, blur_radius);
        memcpy(g, tmp, (size_t) w * h);
    }
    for (int pass = 0; pass < blur_passes; pass++) {
        for (int y = 0; y < h; y++) box_blur_1d(b + y * w, tmp + y * w, w, 1, blur_radius);
        memcpy(b, tmp, (size_t) w * h);
        for (int x = 0; x < w; x++) box_blur_1d(b + x, tmp + x, h, w, blur_radius);
        memcpy(b, tmp, (size_t) w * h);
    }

    uint8_t * out_bytes = malloc((size_t) REFLECTION_WIDTH * BOARD_SCREEN_HEIGHT * 2);
    if (!out_bytes) {
        free(r); free(g); free(b); free(tmp);
        return NULL;
    }

    /* Bilinear upsample from the tiny blurred work buffer to the full
     * REFLECTION_WIDTH x BOARD_SCREEN_HEIGHT output, darkening and dithering
     * at each FINAL destination pixel's own (x,y) -- smooth interpolation
     * here (not nearest-neighbor row duplication) is what actually removes
     * the banding that looked like "shimmer" once densely dithered. Still
     * touches all REFLECTION_WIDTH*BOARD_SCREEN_HEIGHT destination pixels
     * (the earlier full-resolution pass's own audio-starvation risk applies
     * here too, just to a cheaper per-pixel cost now), so keep the same
     * cooperative yield this codebase already uses elsewhere for background
     * decode work. */
    uint16_t * out = (uint16_t *) out_bytes;
#define REFLECTION_UPSAMPLE_YIELD_ROWS 40
    for (int y = 0; y < BOARD_SCREEN_HEIGHT; y++) {
        if (y > 0 && (y % REFLECTION_UPSAMPLE_YIELD_ROWS) == 0) usleep(1000);
        uint32_t y_fp = h > 1 ? (uint32_t) (((uint64_t) y * (h - 1) << 16) / (BOARD_SCREEN_HEIGHT - 1)) : 0;
        uint16_t * out_row = out + (size_t) y * REFLECTION_WIDTH;
        for (int x = 0; x < REFLECTION_WIDTH; x++) {
            uint32_t x_fp = w > 1 ? (uint32_t) (((uint64_t) x * (w - 1) << 16) / (REFLECTION_WIDTH - 1)) : 0;
            int rv = bilerp_plane(r, w, h, x_fp, y_fp) * darken_num / darken_den;
            int gv = bilerp_plane(g, w, h, x_fp, y_fp) * darken_num / darken_den;
            int bv = bilerp_plane(b, w, h, x_fp, y_fp) * darken_num / darken_den;
            out_row[x] = rgb888_to_565_spatial_dithered(rv, gv, bv, x, y);
        }
    }
#undef REFLECTION_UPSAMPLE_YIELD_ROWS
    free(r); free(g); free(b); free(tmp);
    return out_bytes;
}

typedef struct {
    bool flat;
    bool has_bg_color;
    uint32_t bg_color;
    int blur_radius;
    int blur_passes;
    int darken_num;
    int darken_den;
} player_frost_params_t;

/* Resolves effective frosted-glass layout parameters from player_layout_config,
 * applying defaults and defensive clamping. Reads the global layout config once
 * into a local snapshot to avoid data races with UI thread updates. */
static player_frost_params_t resolve_player_frost_params(void) {
    player_layout_config_t cfg = player_layout_config;
    player_frost_params_t params = {
        .flat = false,
        .has_bg_color = false,
        .bg_color = 0,
        .blur_radius = REFLECTION_BLUR_RADIUS,
        .blur_passes = REFLECTION_BLUR_PASSES,
        .darken_num = REFLECTION_DARKEN_NUM,
        .darken_den = REFLECTION_DARKEN_DEN,
    };

    if (cfg.configured) {
        if (cfg.flat) {
            params.flat = true;
            params.has_bg_color = cfg.has_bg_color;
            params.bg_color = cfg.bg_color;
            return params;
        }
        if (cfg.has_blur_radius) {
            params.blur_radius = cfg.blur_radius;
            if (params.blur_radius < 0) params.blur_radius = 0;
            if (params.blur_radius > 64) params.blur_radius = 64;
        }
        if (cfg.has_blur_passes) {
            params.blur_passes = cfg.blur_passes;
            if (params.blur_passes < 0) params.blur_passes = 0;
            if (params.blur_passes > 16) params.blur_passes = 16;
        }
        if (cfg.has_darken) {
            params.darken_num = cfg.darken_num;
            params.darken_den = cfg.darken_den;
            if (params.darken_num < 0) params.darken_num = 0;
            if (params.darken_num > 64) params.darken_num = 64;
            if (params.darken_den < 1) params.darken_den = 1;
            if (params.darken_den > 64) params.darken_den = 64;
        }
    }

    return params;
}

static void apply_player_flat_background(bool has_bg_color, uint32_t bg_color) {
    if (player_background_img) {
        lv_obj_add_flag(player_background_img, LV_OBJ_FLAG_HIDDEN);
        lv_image_set_src(player_background_img, NULL);
    }
    lv_image_cache_drop(&current_reflection_dsc);
    uint8_t * old_reflection_bytes = current_reflection_bytes;
    current_reflection_bytes = NULL;
    current_reflection_dsc.data = NULL;
    current_reflection_dsc.data_size = 0;
    gui_shell_refresh_quick_drawer_cover();
    free(old_reflection_bytes);

    if (!player_overlay_panel) return;

    if (has_bg_color) {
        lv_obj_set_style_bg_color(player_overlay_panel, lv_color_hex(bg_color), 0);
    } else {
        lv_obj_remove_local_style_prop(player_overlay_panel, LV_STYLE_BG_COLOR, 0);
    }
    lv_obj_set_style_bg_opa(player_overlay_panel, LV_OPA_COVER, 0);
    player_transition_mark_dirty();
}

void gui_player_refresh_frosted_background(void) {
    if (!player_overlay_panel) return;

    player_frost_params_t params = resolve_player_frost_params();
    if (params.flat) {
        /* Flat mode is a plain color fill -- doesn't need decoded cover
         * pixels, so it must apply even before any track has played or
         * after a failed decode, unlike the blur branch below. */
        apply_player_flat_background(params.has_bg_color, params.bg_color);
        return;
    }
    if (!current_cover_bytes) {
        /* Nothing to blur yet -- next decode picks up the current config.
         * Still clear any flat-mode BG_COLOR left over from a previous
         * live switch away from flat, so a stale tint doesn't linger
         * behind the frosted image once a cover does decode. */
        if (player_background_img) {
            lv_obj_add_flag(player_background_img, LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_remove_local_style_prop(player_overlay_panel, LV_STYLE_BG_COLOR, 0);
        return;
    }

    if (player_background_img) {
        lv_obj_add_flag(player_background_img, LV_OBJ_FLAG_HIDDEN);
    }
    free(current_reflection_bytes);
    current_reflection_bytes = compute_reflection_bytes(current_cover_bytes, params.blur_radius, params.blur_passes, params.darken_num, params.darken_den);

    /* TRANSP, not COVER -- player_background_img (the frosted image, about
     * to be shown) fully covers this panel; keeping the panel's own paint
     * opaque underneath it forces LVGL to composite two full-screen opaque
     * layers every frame for no visible benefit. The lyrics screen's own
     * analogous wrapper (dark_overlay in gui_lyrics.c) is deliberately kept
     * transparent for exactly this reason -- its own comment explains
     * darkness is already baked into its backdrop image. */
    lv_obj_remove_local_style_prop(player_overlay_panel, LV_STYLE_BG_COLOR, 0);
    lv_obj_set_style_bg_opa(player_overlay_panel, LV_OPA_TRANSP, 0);

    if (current_reflection_bytes && player_background_img) {
        memset(&current_reflection_dsc, 0, sizeof(current_reflection_dsc));
        current_reflection_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        current_reflection_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
        current_reflection_dsc.header.w = REFLECTION_WIDTH;
        current_reflection_dsc.header.h = BOARD_SCREEN_HEIGHT;
        current_reflection_dsc.header.stride = REFLECTION_WIDTH * 2;
        current_reflection_dsc.data = current_reflection_bytes;
        current_reflection_dsc.data_size = (uint32_t) REFLECTION_WIDTH * BOARD_SCREEN_HEIGHT * 2;
        lv_image_set_src(player_background_img, &current_reflection_dsc);
        lv_obj_set_pos(player_background_img, 0, 0);
        lv_obj_set_size(player_background_img, REFLECTION_WIDTH, BOARD_SCREEN_HEIGHT);
        lv_image_set_pivot(player_background_img, 0, 0);
        lv_image_set_scale_x(player_background_img, LV_SCALE_NONE);
        lv_image_set_scale_y(player_background_img, LV_SCALE_NONE);
        lv_obj_remove_flag(player_background_img, LV_OBJ_FLAG_HIDDEN);
    } else if (player_background_img) {
        lv_obj_add_flag(player_background_img, LV_OBJ_FLAG_HIDDEN);
    }
    player_transition_mark_dirty();
}

/* Cover art decoding (cover_decode_to_rgb565()) and reflection blur
 * computation (compute_reflection_bytes()) are offloaded to a background
 * pthread to avoid blocking the UI thread. Frost parameters (radius,
 * passes, darken ratio, or flat styling) are snapshotted on the UI thread
 * into each request before launching the worker thread, avoiding data races
 * against live player_layout_config modifications. launch_cover_decode_req()
 * spawns the worker thread, and poll_cover_decode() (called periodically
 * from update_timer_cb) applies the decoded RGB565 buffers or flat styling
 * to the image widgets once ready. */
typedef struct {
    int for_index;
    unsigned int generation;
    /* Skip the generated-cache fast path and the negative cache: set for the
     * re-read after a cover reload, whose tag-derived key may name a cache
     * file the reload did not delete. */
    bool force_source;
    uint8_t * picture_data; /* owned download buffer for streamed artwork */
    uint32_t picture_size;

    /* Set instead of picture_data/picture_size for a Subsonic stream's cover
     * art (see launch_cover_decode_from_url()) -- there's no local file to
     * have already extracted embedded art from, so the compressed image
     * bytes are fetched here, on the same background thread, before falling
     * into the same decode path local art already uses. Empty string for
     * every other caller. */
    char stream_url[1536];
    bool stream_verify_tls;
    /* Local track identifying its generated album cache. The lookup and
     * file read stay on this worker thread. Artist/
     * album come from the already-parsed track (or the DB), never from a
     * second metadata_read() -- that re-parse OOMs on huge ID3/APIC tags. */
    char local_track_path[PATH_MAX];
    char artist[128];
    char album[128];
    char album_artist[128];

    /* Resolved frosted-glass parameters snapshotted from player_layout_config
     * on the UI thread before launching the worker thread. */
    bool flat;
    bool has_bg_color;
    uint32_t bg_color;
    int blur_radius;
    int blur_passes;
    int darken_num;
    int darken_den;
} cover_decode_request_t;

static pthread_t cover_decode_thread;
static bool cover_decode_active = false;
static atomic_bool cover_decode_done_flag = false;

/* Result fields, written by cover_decode_thread_func() on the background
 * thread and consumed (freed or applied) by poll_cover_decode() on the main
 * thread once cover_decode_done_flag is seen true -- never touched by both
 * at once, same "background thread writes then sets the flag last, main
 * thread only reads after seeing the flag" contract as every other _done_
 * flag in this file. */
static int cover_decode_result_for_index;
static unsigned int cover_decode_result_generation;
static audio_current_format_info_t cover_decode_result_format;
/* UI-thread cache, keyed by path rather than a mutable playlist index. */
static audio_current_format_info_t player_cached_format;
static bool cover_decode_result_ok;
static uint16_t * cover_decode_result_pixels;    /* COVER_ART_WIDTH x COVER_ART_HEIGHT RGB565, owned */
static uint8_t * cover_decode_result_reflection; /* REFLECTION_WIDTH x BOARD_SCREEN_HEIGHT RGB565, owned */
static bool cover_decode_result_flat;
static bool cover_decode_result_has_bg_color;
static uint32_t cover_decode_result_bg_color;
/* Snapshotted at request-launch time, same as the 3 fields above -- carried
 * through so poll_cover_decode() can tell whether player_layout_config
 * changed while this decode was in flight (see its own comment). */
static int cover_decode_result_blur_radius;
static int cover_decode_result_blur_passes;
static int cover_decode_result_darken_num;
static int cover_decode_result_darken_den;

/* Helper called only from the UI thread when constructing a fresh decode request.
 * Resolves current layout frost parameters once and snapshots them into the request. */
static void cover_decode_request_snapshot_frost(cover_decode_request_t * req) {
    player_frost_params_t p = resolve_player_frost_params();
    req->flat = p.flat;
    req->has_bg_color = p.has_bg_color;
    req->bg_color = p.bg_color;
    req->blur_radius = p.blur_radius;
    req->blur_passes = p.blur_passes;
    req->darken_num = p.darken_num;
    req->darken_den = p.darken_den;
}


/* Holds at most one superseded request -- see launch_cover_decode_req()'s own
 * comment on why a track change that arrives while a decode is already in
 * flight can't just be dropped (that would leave the cover permanently
 * stuck showing an earlier track's art). */
static bool cover_decode_pending_valid = false;
static cover_decode_request_t cover_decode_pending;
static atomic_uint cover_decode_generation;
static atomic_bool cover_reload_active;
/* True while a cover decode worker body runs (it may write generated cache
 * files); a cover reload waits for this off the UI thread before deleting. */
static atomic_bool cover_decode_running;
static bool last_local_cover_request_valid;
static cover_decode_request_t last_local_cover_request;

/* Bound reads even if a generated cache file was replaced or corrupted.
 * The expected 480x480 BMP occupies 691254 bytes. */
#define EXTERNAL_COVER_MAX_BYTES (4U * 1024U * 1024U)

static void albumart_info_from_path_tags(const char * track_path, const char * artist, const char * album,
                                          const char * album_artist, albumart_info_t * info) {
    memset(info, 0, sizeof(*info));
    snprintf(info->path, sizeof(info->path), "%s", track_path ? track_path : "");
    snprintf(info->artist, sizeof(info->artist), "%s", artist ? artist : "");
    snprintf(info->album, sizeof(info->album), "%s", album ? album : "");
    snprintf(info->albumartist, sizeof(info->albumartist), "%s", album_artist ? album_artist : "");
}

void albumart_info_from_song_row(const song_row_t * song, albumart_info_t * info) {
    albumart_info_from_path_tags(song->path, song->tags.artist, song->tags.album, song->tags.album_artist, info);
}

/* Session-scoped negative cache for load_cached_player_cover()'s slow path:
 * a track with no art anywhere (no sidecar, no embedded picture) or with
 * tags too empty for albumart_store_rgb565()'s artist+album requirement
 * can never produce a persistable cache entry, so without this,
 * gui_library_generate_player_cover()'s full fork+isolated-extract fallback
 * would silently re-run on every single play of that track -- exactly the
 * per-play cost this feature exists to eliminate. Small, bounded, path-hash
 * keyed (FNV-1a, same convention already used for cache filenames
 * elsewhere in this codebase); cleared by an app restart and after every
 * library publication, so a database update or metadata refresh retries
 * art added mid-session without per-play mtime tracking. */
#define PLAYER_COVER_NEGATIVE_CACHE_SIZE 32
static uint64_t player_cover_negative_cache[PLAYER_COVER_NEGATIVE_CACHE_SIZE];
static int player_cover_negative_cache_next = 0;
/* Bumped by any library publication; the decode worker, the table's only
 * user, empties it on seeing a new value so refreshed art is retried. */
static atomic_uint player_cover_negative_epoch;
static unsigned player_cover_negative_seen_epoch;

void gui_player_invalidate_cover_negatives(void) {
    atomic_fetch_add_explicit(&player_cover_negative_epoch, 1u, memory_order_relaxed);
}

static uint64_t player_cover_path_hash(const char * path) {
    uint64_t hash = UINT64_C(14695981039346656037);
    for (const unsigned char * p = (const unsigned char *) path; *p; p++)
        hash = (hash ^ *p) * UINT64_C(1099511628211);
    return hash;
}

static bool player_cover_decode_cancelled(void * user_data) {
    unsigned int generation = (unsigned int) (uintptr_t) user_data;
    return atomic_load_explicit(&cover_decode_generation, memory_order_relaxed) != generation;
}

static bool player_cover_negative_cache_hit(const char * path) {
    unsigned epoch = atomic_load_explicit(&player_cover_negative_epoch, memory_order_relaxed);
    if (epoch != player_cover_negative_seen_epoch) {
        memset(player_cover_negative_cache, 0, sizeof(player_cover_negative_cache));
        player_cover_negative_cache_next = 0;
        player_cover_negative_seen_epoch = epoch;
    }
    uint64_t h = player_cover_path_hash(path);
    for (int i = 0; i < PLAYER_COVER_NEGATIVE_CACHE_SIZE; i++)
        if (player_cover_negative_cache[i] == h) return true;
    return false;
}

static void player_cover_negative_cache_add(const char * path) {
    player_cover_negative_cache[player_cover_negative_cache_next] = player_cover_path_hash(path);
    player_cover_negative_cache_next = (player_cover_negative_cache_next + 1) % PLAYER_COVER_NEGATIVE_CACHE_SIZE;
}

/* Local playback uses the generated 480px BMP. Never decode the original
 * embedded/sidecar image directly here: cache generation owns that
 * expensive work -- normally the background warmer, or, for a track it
 * hasn't reached yet, the one-time-per-session fallback below. */
/* The generated player-size cache for these tags. require_fresh compares its
 * stored source mtime with this track's; a reload's fallback skips that, as
 * the file was just rebuilt, possibly from a sibling track with another mtime. */
static bool load_generated_player_cover(const albumart_info_t * info, const char * track_path,
                                        unsigned int generation, bool require_fresh,
                                        uint16_t ** out_pixels) {
    char found[PATH_MAX];
    bool have = require_fresh
        ? albumart_generated_cache_fresh(info, ALBUMART_PLAYER_CACHE_SIZE, ALBUMART_PLAYER_CACHE_SIZE,
                                         found, sizeof(found))
        : albumart_generated_cache_find(info, ALBUMART_PLAYER_CACHE_SIZE, ALBUMART_PLAYER_CACHE_SIZE,
                                        found, sizeof(found));
    if (!have) return false;
    uint8_t * data = NULL;
    uint32_t size = 0;
    if (albumart_load_file_ex(found, &data, &size, EXTERNAL_COVER_MAX_BYTES,
                              ARTWORK_PRIO_PLAYER) != ALBUMART_LOAD_OK)
        return false;
    cover_decode_result_t result = cover_decode_to_rgb565_ex(data, size,
        COVER_ART_WIDTH, COVER_ART_HEIGHT, ARTWORK_PRIO_PLAYER,
        player_cover_decode_cancelled, (void *) (uintptr_t) generation, out_pixels);
    free(data);
    if (result != COVER_DECODE_OK) return false;
    DB_LOG("ART_PLAYER", "cache_hit path=%s file=%s", track_path, found);
    return true;
}

static bool load_cached_player_cover(const char * track_path, const char * artist, const char * album,
                                      const char * album_artist, unsigned int generation,
                                      bool force_source, uint16_t ** out_pixels) {
    if (player_cover_decode_cancelled((void *) (uintptr_t) generation)) return false;
    albumart_info_t info;
    albumart_info_from_path_tags(track_path, artist, album, album_artist, &info);
    if (!force_source && load_generated_player_cover(&info, track_path, generation, true, out_pixels))
        return true;

    if (player_cover_decode_cancelled((void *) (uintptr_t) generation)) return false;
    if (!force_source && player_cover_negative_cache_hit(track_path)) {
        DB_LOG("ART_PLAYER", "no_art_known path=%s", track_path);
        return false;
    }

    /* Cache miss -- this track's album hasn't been warmed yet (freshly
     * added, or the background scan just hasn't reached it). Generate and
     * persist the cache now so every later play of this track hits the
     * fast path above. */
    bool no_art_confirmed = false;
    if (gui_library_generate_player_cover(track_path, artist, album, album_artist,
                                          player_cover_decode_cancelled,
                                          (void *) (uintptr_t) generation,
                                          out_pixels, &no_art_confirmed)) {
        DB_LOG("ART_PLAYER", "cache_miss_generated path=%s", track_path);
        return true;
    }
    if (player_cover_decode_cancelled((void *) (uintptr_t) generation)) return false;
    /* A forced re-read prefers the track's own art, but a track without any
     * may still have album art the warmer rebuilt from a sibling track. */
    if (force_source && load_generated_player_cover(&info, track_path, generation, false, out_pixels))
        return true;
    /* Only remember a confirmed "no art anywhere" result -- a transient one
     * (coordinator busy, isolated-helper fork/timeout) must retry on the
     * next play, not get silently and permanently suppressed. */
    DB_LOG("ART_PLAYER", "cache_miss_failed path=%s confirmed=%d", track_path, no_art_confirmed);
    if (no_art_confirmed) player_cover_negative_cache_add(track_path);
    return false;
}

/* Read native FLAC STREAMINFO without opening a second decoder. This is
 * bounded I/O and avoids shared codec initialization or whole-file scans. */
static bool player_read_flac_format(const char * path, audio_current_format_info_t * out) {
    unsigned char header[42];
    FILE * f = fopen(path, "rb");
    if (!f) return false;
    size_t n = fread(header, 1, sizeof(header), f);
    fclose(f);
    if (n != sizeof(header) || memcmp(header, "fLaC", 4) != 0 ||
        (header[4] & 0x7f) != 0 || header[5] != 0 || header[6] != 0 || header[7] != 34)
        return false;
    /* STREAMINFO bytes 10..13 pack rate (20 bits), channels (3), depth (5). */
    unsigned int rate = ((unsigned int) header[18] << 12) |
                        ((unsigned int) header[19] << 4) | (header[20] >> 4);
    unsigned int depth = (((header[20] & 1u) << 4) | (header[21] >> 4)) + 1;
    if (!rate || depth < 4) return false;
    memset(out, 0, sizeof(*out));
    out->valid = true;
    snprintf(out->path, sizeof(out->path), "%s", path);
    out->source_sample_rate = rate;
    out->source_bit_depth = depth;
    return true;
}

static void * cover_decode_thread_func(void * arg) {
    cover_decode_request_t * req = (cover_decode_request_t *) arg;
    uint16_t * pixels = NULL;
    bool ok = false;

    memset(&cover_decode_result_format, 0, sizeof(cover_decode_result_format));
    if (req->local_track_path[0]) {
        if (!audio_get_current_format_info(&cover_decode_result_format) ||
            strcmp(cover_decode_result_format.path, req->local_track_path) != 0) {
            memset(&cover_decode_result_format, 0, sizeof(cover_decode_result_format));
            /* Restored paused tracks have no decoder yet. Probe off the UI
             * thread; never start playback merely to populate the badge. */
            player_read_flac_format(req->local_track_path, &cover_decode_result_format);
        }
    }

    if (req->stream_url[0] != '\0') {
        int status = 0;
        uint8_t * body = NULL;
        size_t body_size = 0;
        if (http_get_to_buffer(req->stream_url, req->stream_verify_tls, &status, &body, &body_size) && status == 200) {
            req->picture_data = body;
            req->picture_size = (uint32_t) body_size;
        } else {
            free(body);
            /* Left NULL/0 -- decode below no-ops, same as "no embedded art"
             * for a local file. No error surfaced beyond that; a failed
             * cover-art fetch isn't worth blocking or retrying playback
             * over. */
        }
        ok = req->picture_data && req->picture_size > 0 &&
             cover_decode_to_rgb565(req->picture_data, req->picture_size, COVER_ART_WIDTH, COVER_ART_HEIGHT, &pixels);
        free(req->picture_data);
        req->picture_data = NULL;
    } else if (req->local_track_path[0]) {
        ok = load_cached_player_cover(req->local_track_path, req->artist, req->album,
                                       req->album_artist, req->generation, req->force_source, &pixels);
    }

    if (player_cover_decode_cancelled((void *) (uintptr_t) req->generation)) {
        free(pixels);
        pixels = NULL;
        ok = false;
    }

    uint8_t * reflection = NULL;
    if (ok && !req->flat) {
        reflection = compute_reflection_bytes((const uint8_t *) pixels,
                                              req->blur_radius,
                                              req->blur_passes,
                                              req->darken_num,
                                              req->darken_den);
    }

    cover_decode_result_for_index = req->for_index;
    cover_decode_result_generation = req->generation;
    cover_decode_result_ok = ok;
    cover_decode_result_pixels = pixels;
    cover_decode_result_reflection = reflection;
    cover_decode_result_flat = req->flat;
    cover_decode_result_has_bg_color = req->has_bg_color;
    cover_decode_result_bg_color = req->bg_color;
    cover_decode_result_blur_radius = req->blur_radius;
    cover_decode_result_blur_passes = req->blur_passes;
    cover_decode_result_darken_num = req->darken_num;
    cover_decode_result_darken_den = req->darken_den;
    free(req);
    atomic_store_explicit(&cover_decode_running, false, memory_order_release);
    atomic_store_explicit(&cover_decode_done_flag, true, memory_order_release); /* written last -- poll_cover_decode() only checks this flag */
    return NULL;
}

/* Takes ownership of picture_data (a malloc()'d buffer from metadata_read(),
 * or NULL). If a decode is already running, this doesn't launch a second
 * one (cover_decode_to_rgb565()/compute_reflection_bytes() are reentrant --
 * no shared state -- but there's no value in two decodes racing when only
 * the newest one's result will ever get applied) -- it instead replaces
 * cover_decode_pending (freeing whatever was queued there before), and
 * poll_cover_decode() launches that pending request the moment the active
 * one finishes. A rapid sequence of track changes only ever actually
 * decodes the first and the final one, which is exactly the set of results
 * that matters. */
static void launch_cover_decode_req(cover_decode_request_t r) {
    if (r.generation == 0)
        r.generation = atomic_fetch_add_explicit(&cover_decode_generation, 1u,
                                                  memory_order_relaxed) + 1u;
    if (r.local_track_path[0]) {
        last_local_cover_request = r;
        last_local_cover_request_valid = true;
    } else if (r.stream_url[0]) {
        last_local_cover_request_valid = false;
    }

    if (atomic_load_explicit(&cover_reload_active, memory_order_relaxed)) {
        free(cover_decode_pending.picture_data);
        cover_decode_pending = r;
        cover_decode_pending_valid = true;
        return;
    }
    if (cover_decode_active) {
        free(cover_decode_pending.picture_data);
        cover_decode_pending = r;
        cover_decode_pending_valid = true;
        return;
    }

    cover_decode_request_t * req = malloc(sizeof(*req));
    if (!req) {
        /* Free owned picture_data and skip decoding if request allocation
         * fails under memory pressure. */
        free(r.picture_data);
        return;
    }
    *req = r;
    atomic_store_explicit(&cover_decode_done_flag, false, memory_order_relaxed);
    cover_decode_active = true;
    /* Marked before the thread exists, so a reload worker can never see
     * "idle" while a created-but-unscheduled decode is about to run. */
    atomic_store_explicit(&cover_decode_running, true, memory_order_release);
    if (pthread_create(&cover_decode_thread, NULL, cover_decode_thread_func, req) != 0) {
        free(req->picture_data);
        free(req);
        cover_decode_active = false;
        atomic_store_explicit(&cover_decode_running, false, memory_order_release);
    }
}


static void launch_cover_decode_from_track(int for_index, const char * track_path, const char * artist,
                                            const char * album, const char * album_artist) {
    cover_decode_request_t r = { .for_index = for_index, .generation = 0,
                                  .picture_data = NULL, .picture_size = 0,
                                  .stream_url = "", .stream_verify_tls = false };
    snprintf(r.local_track_path, sizeof(r.local_track_path), "%s", track_path ? track_path : "");
    snprintf(r.artist, sizeof(r.artist), "%s", artist ? artist : "");
    snprintf(r.album, sizeof(r.album), "%s", album ? album : "");
    snprintf(r.album_artist, sizeof(r.album_artist), "%s", album_artist ? album_artist : "");
    cover_decode_request_snapshot_frost(&r);
    launch_cover_decode_req(r);
}

/* Subsonic streaming's own art source -- see cover_decode_request_t's own
 * comment. url is subsonic_build_cover_art_url()'s output, fetched on the
 * same background thread that would otherwise be decoding already-local
 * bytes, so a slow/flaky connection can't block the UI here either, same
 * reasoning as launch_cover_decode_from_track() itself. */
static void launch_cover_decode_from_url(int for_index, const char * url, bool verify_tls) {
    cover_decode_request_t r = { .for_index = for_index, .generation = 0,
                                  .picture_data = NULL, .picture_size = 0,
                                  .stream_verify_tls = verify_tls };
    snprintf(r.stream_url, sizeof(r.stream_url), "%s", url);
    cover_decode_request_snapshot_frost(&r);
    launch_cover_decode_req(r);
}


static void fit_cover_img_to_card(void) {
    if (!cover_img || !cover_card) return;
    lv_image_header_t header;
    if (lv_image_decoder_get_info(lv_image_get_src(cover_img), &header) != LV_RESULT_OK ||
        header.w <= 0 || header.h <= 0) return;
    int32_t card_w = lv_obj_get_width(cover_card);
    int32_t card_h = lv_obj_get_height(cover_card);
    if (card_w <= 0 || card_h <= 0) {
        card_w = player_s(350);
        card_h = player_s(350);
    }
    int32_t scale_w = (card_w * LV_SCALE_NONE + header.w - 1) / header.w;
    int32_t scale_h = (card_h * LV_SCALE_NONE + header.h - 1) / header.h;
    int32_t scale = scale_w > scale_h ? scale_w : scale_h;
    lv_image_set_scale(cover_img, scale);
    lv_obj_align(cover_img, LV_ALIGN_CENTER, 0, 0);
}

/* Called every tick from update_timer_cb. Applies the finished decode to
 * cover_img/player_overlay_panel -- unless playlist_index has already moved
 * on to a different track by the time this lands (another launch_cover_
 * decode() call superseded it via cover_decode_pending, in which case that
 * one is now either running or about to be), in which case the result is
 * just discarded rather than briefly flashing a stale track's art. */
void poll_cover_decode(void) {
    /* Slides display owned snapshots. Applying artwork now cannot update
     * those frames, but image/layout work can delay their presentation.
     * Leave the completed result owned by the worker until the next poll. */
    if (gui_navigation_transition_in_progress()) return;
    if (!cover_decode_active || !atomic_load_explicit(&cover_decode_done_flag, memory_order_acquire)) return;
    cover_decode_active = false;
    pthread_join(cover_decode_thread, NULL);

    bool result_generation_current = cover_decode_result_generation ==
        atomic_load_explicit(&cover_decode_generation, memory_order_relaxed);

    if (result_generation_current && cover_decode_result_format.valid &&
        strcmp(cover_decode_result_format.path, playlist_path_at(playlist_index)) == 0) {
        player_cached_format = cover_decode_result_format;
        refresh_format_badge();
    }

    if (!result_generation_current || cover_decode_result_for_index != playlist_index) {
        free(cover_decode_result_pixels);
        free(cover_decode_result_reflection);
    } else if (!cover_decode_result_ok) {
        free(cover_decode_result_reflection);
        /* Retarget every live image object before releasing the backing
         * buffers. The lock screen may keep current_cover_dsc as its source,
         * so clear the descriptor before freeing its pixels. */
        if (cover_img) lv_image_set_src(cover_img, asset_path("playing_plane/default_cover_565.png"));
        lv_image_cache_drop(&current_cover_dsc);
        uint8_t * old_cover_bytes = current_cover_bytes;
        current_cover_bytes = NULL;
        current_cover_for_index = -1;
        /* current_cover_dsc.data still points at the old pixels until it is
         * cleared below -- gui_player_get_current_cover_dsc() hands this same static
         * struct's address out to other callers (the lock screen), who keep
         * referencing &current_cover_dsc for as long as they're showing. */
        current_cover_dsc.data = NULL;
        current_cover_dsc.data_size = 0;
        fit_cover_img_to_card();
        /* No in-memory raw bitmap to reflect for the static placeholder
         * cover. Apply a configured flat color if set (it needs no cover
         * pixels), otherwise reset the panel back to its plain background
         * rather than leaving a stale reflection from whatever track played
         * before this one. */
        player_frost_params_t failure_params = resolve_player_frost_params();
        if (failure_params.flat) {
            apply_player_flat_background(failure_params.has_bg_color, failure_params.bg_color);
        } else {
            if (player_background_img) {
                lv_obj_add_flag(player_background_img, LV_OBJ_FLAG_HIDDEN);
                lv_image_set_src(player_background_img, NULL);
            }
            lv_image_cache_drop(&current_reflection_dsc);
            uint8_t * old_reflection_bytes = current_reflection_bytes;
            current_reflection_bytes = NULL;
            current_reflection_dsc.data = NULL;
            current_reflection_dsc.data_size = 0;
            /* Clear any flat-mode BG_COLOR left over from a previous live
             * switch away from flat -- same reasoning as gui_player_
             * refresh_frosted_background()'s own no-cover-yet branch. */
            if (player_overlay_panel)
                lv_obj_remove_local_style_prop(player_overlay_panel, LV_STYLE_BG_COLOR, 0);
            gui_shell_refresh_quick_drawer_cover();
            free(old_reflection_bytes);
        }
        free(old_cover_bytes);
        gui_lock_screen_refresh_cover();
        player_transition_mark_dirty(); /* cover_img just changed to the placeholder -- see the cache's own doc comment */
    } else {
        free(current_cover_bytes);
        current_cover_bytes = (uint8_t *) cover_decode_result_pixels;
        current_cover_for_index = cover_decode_result_for_index;

        /* lv_image_header_t.magic must be set to LV_IMAGE_HEADER_MAGIC;
         * otherwise LVGL's bin decoder treats a 0 magic value as a legacy
         * header format and overwrites header->cf with header->magic (0),
         * corrupting the pixel color format interpretation. */
        memset(&current_cover_dsc, 0, sizeof(current_cover_dsc));
        current_cover_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        current_cover_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
        current_cover_dsc.header.w = COVER_ART_WIDTH;
        current_cover_dsc.header.h = COVER_ART_HEIGHT;
        current_cover_dsc.header.stride = COVER_ART_WIDTH * 2;
        current_cover_dsc.data = current_cover_bytes;
        current_cover_dsc.data_size = (uint32_t) COVER_ART_WIDTH * COVER_ART_HEIGHT * 2;
        lv_image_set_src(cover_img, &current_cover_dsc);
        fit_cover_img_to_card();
        gui_shell_refresh_quick_drawer_cover();
        gui_lock_screen_refresh_cover();

        /* The reflection this decode computed was built from frost params
         * snapshotted when the decode was LAUNCHED. If a plugin changed
         * player_layout_config while this decode was in flight (a rare
         * race, e.g. plugin.set_player_layout()/refresh_theme() firing
         * mid-decode), that stale snapshot must not silently overwrite the
         * live config the plugin just applied -- compare against the
         * current live params and only trust the worker's precomputed
         * result when they still match. */
        player_frost_params_t live_params = resolve_player_frost_params();
        bool snapshot_is_stale = (live_params.flat != cover_decode_result_flat) ||
            (live_params.flat
                ? (live_params.has_bg_color != cover_decode_result_has_bg_color ||
                   (live_params.has_bg_color && live_params.bg_color != cover_decode_result_bg_color))
                : (live_params.blur_radius != cover_decode_result_blur_radius ||
                   live_params.blur_passes != cover_decode_result_blur_passes ||
                   live_params.darken_num != cover_decode_result_darken_num ||
                   live_params.darken_den != cover_decode_result_darken_den));

        if (snapshot_is_stale) {
            /* Discard the worker's now-outdated result and recompute
             * synchronously against the live config and the cover art
             * just installed above -- a one-off UI-thread recompute here
             * is the same acceptable tradeoff gui_player_refresh_frosted_
             * background()'s own live-refresh callers already make. */
            free(cover_decode_result_reflection);
            gui_player_refresh_frosted_background();
        } else if (cover_decode_result_flat) {
            free(cover_decode_result_reflection);
            apply_player_flat_background(cover_decode_result_has_bg_color, cover_decode_result_bg_color);
        } else {
            if (player_background_img) {
                lv_obj_add_flag(player_background_img, LV_OBJ_FLAG_HIDDEN);
            }
            free(current_reflection_bytes);
            current_reflection_bytes = cover_decode_result_reflection;

            /* TRANSP, not COVER -- see gui_player_refresh_frosted_background()'s
             * own comment on this exact line for why. */
            lv_obj_remove_local_style_prop(player_overlay_panel, LV_STYLE_BG_COLOR, 0);
            lv_obj_set_style_bg_opa(player_overlay_panel, LV_OPA_TRANSP, 0);

            if (current_reflection_bytes && player_background_img) {
                /* Same LV_IMAGE_HEADER_MAGIC requirement as current_cover_dsc above. */
                memset(&current_reflection_dsc, 0, sizeof(current_reflection_dsc));
                current_reflection_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
                current_reflection_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
                current_reflection_dsc.header.w = REFLECTION_WIDTH;
                current_reflection_dsc.header.h = BOARD_SCREEN_HEIGHT;
                current_reflection_dsc.header.stride = REFLECTION_WIDTH * 2;
                current_reflection_dsc.data = current_reflection_bytes;
                current_reflection_dsc.data_size = (uint32_t) REFLECTION_WIDTH * BOARD_SCREEN_HEIGHT * 2;
                lv_image_set_src(player_background_img, &current_reflection_dsc);
                lv_obj_set_pos(player_background_img, 0, 0);
                lv_obj_set_size(player_background_img, REFLECTION_WIDTH, BOARD_SCREEN_HEIGHT);
                lv_image_set_pivot(player_background_img, 0, 0);
                lv_image_set_scale_x(player_background_img, LV_SCALE_NONE);
                lv_image_set_scale_y(player_background_img, LV_SCALE_NONE);
                lv_obj_remove_flag(player_background_img, LV_OBJ_FLAG_HIDDEN);
            } else if (player_background_img) {
                lv_obj_add_flag(player_background_img, LV_OBJ_FLAG_HIDDEN);
            }
        }

        /* The drawer shows this same frosted copy, so it can only be
         * refreshed once the new one has replaced the previous track's. */
        gui_shell_refresh_quick_drawer_cover();

        /* Refresh lyrics screen backdrop now that current_cover_bytes has
         * been updated with new cover art. */
        gui_lyrics_on_cover_changed(playlist_index);
        player_transition_mark_dirty(); /* cover_img/player_overlay_panel's reflection just changed -- see the cache's own doc comment */
    }

    cover_decode_result_pixels = NULL;
    cover_decode_result_reflection = NULL;

    if (cover_decode_pending_valid) {
        cover_decode_pending_valid = false;
        launch_cover_decode_req(cover_decode_pending); /* the whole request, not a rebuilt one -- must carry stream_url too, see that field's own comment */
        /* Clear picture_data to avoid double-freeing if another track
         * change occurs while the handed-off decode thread is running. */
        cover_decode_pending.picture_data = NULL;
        cover_decode_pending.picture_size = 0;
    }
}

void gui_player_begin_cover_reload(void) {
    /* New decodes are held (launch_cover_decode_req() parks them in
     * cover_decode_pending) until gui_player_finish_cover_reload(). One
     * already running, local or streamed, finishes normally: the reload
     * worker waits for gui_player_cover_decode_idle() before deleting any
     * file, so it cannot write an old cover back afterwards, and the UI
     * thread never blocks on an isolated extraction here. */
    atomic_store_explicit(&cover_reload_active, true, memory_order_relaxed);
}

bool gui_player_cover_decode_idle(void) {
    return !atomic_load_explicit(&cover_decode_running, memory_order_acquire);
}

void gui_player_finish_cover_reload(void) {
    atomic_store_explicit(&cover_reload_active, false, memory_order_relaxed);
    /* Re-read the playing track's cover whatever album was reloaded: the
     * player's tag-derived key can differ from the library's, and one cached
     * read is cheap. The result replaces (and frees) the old cover; a pending
     * request for a newer track wins instead. */
    cover_decode_request_t req;
    if (cover_decode_pending_valid) {
        req = cover_decode_pending;
    } else if (last_local_cover_request_valid && last_local_cover_request.for_index == playlist_index &&
               strcmp(last_local_cover_request.local_track_path, playlist_path_at(playlist_index)) == 0) {
        req = last_local_cover_request;
        req.picture_data = NULL;
        req.picture_size = 0;
        cover_decode_request_snapshot_frost(&req);
    } else {
        return;
    }
    if (req.local_track_path[0]) {
        /* Held or current local track alike: the cache it would hit may
         * predate the reload, so read the track's own art first and fall
         * back to the rebuilt album cover. Use the library's current tags: a
         * metadata refresh may have renamed the album since playback
         * started, and the reload rebuilt the cache under the new identity. */
        req.force_source = true;
        song_row_t row;
        if (metadata_db_get_song_by_path(req.local_track_path, &row)) {
            snprintf(req.artist, sizeof(req.artist), "%s", row.tags.artist);
            snprintf(req.album, sizeof(req.album), "%s", row.tags.album);
            snprintf(req.album_artist, sizeof(req.album_artist), "%s", row.tags.album_artist);
        }
    }
    memset(&cover_decode_pending, 0, sizeof(cover_decode_pending));
    cover_decode_pending_valid = false;
    req.generation = 0;
    launch_cover_decode_req(req);
}

void gui_player_cancel_cover_reload(void) {
    atomic_store_explicit(&cover_reload_active, false, memory_order_relaxed);
    atomic_fetch_add_explicit(&cover_decode_generation, 1u, memory_order_relaxed);
    if (cover_decode_pending_valid) {
        free(cover_decode_pending.picture_data);
        memset(&cover_decode_pending, 0, sizeof(cover_decode_pending));
        cover_decode_pending_valid = false;
    }
}


/* ---- Lyrics: async .lrc load ------------------------------------------
 * Deliberately independent from the cover-decode pipeline above, despite
 * both running off the UI thread and both being triggered from the same
 * track-change call site (apply_track_metadata_to_ui() below): this job
 * only reads a small text sidecar (lyrics_load_sidecar(), bounded at
 * LYRICS_MAX_FILE_BYTES) rather than decoding/blurring image data, and
 * runs eagerly on every track change (not lazily like the backdrop below)
 * so lyrics are already parsed by the time the user taps the album art.
 * Same "background thread writes then sets a volatile done flag last, poll
 * function on the UI thread only reads after seeing it" contract as
 * cover_decode_done_flag above, and the same "at most one job in flight,
 * a track change arriving mid-load replaces the pending request rather
 * than queuing" shape as launch_cover_decode_req()'s cover_decode_pending. ---- */







/* ---- Lyrics: fullscreen blurred backdrop ------------------------------
 * Generated lazily -- only when the lyrics screen actually opens (see
 * open_lyrics_screen()) -- rather than eagerly per track change like cover
 * art/reflection above: most tracks' lyrics view is never opened, and this
 * needs nothing the async load above produces, just the already-decoded
 * current_cover_bytes. A private copy is taken up front (cover_copy below)
 * rather than reading current_cover_bytes directly from the background
 * thread, since poll_cover_decode() can free and replace that pointer out
 * from under a still-running backdrop job on a rapid track change. ---- */

/* Final brightness is baked in before RGB565 dithering (9/20 net brightness)
 * to avoid an alpha overlay blend re-quantizing the dithered image. */












/* ---- Asynchronous favorite persistence worker -------------------------
 * Favorite persistence calls metadata_db_song_favorite_set(), which may
 * acquire global metadata locks, trigger tagcache updates, or synchronously
 * rewrite and fsync() remote state files (remote_state_set_rating()).
 * Executing this directly on the LVGL UI thread stalls rendering and causes
 * tap latency or freezes.
 *
 * This long-lived worker thread queues and persists favorite requests off the
 * UI thread. Rapid taps on the same track are coalesced using a 150 ms debounce
 * window so intermediate states are collapsed into the final requested state.
 * Distinct tracks maintain independent entries with owned path copies so track
 * changes do not overwrite or corrupt pending persistence operations. ---- */
#define FAVORITE_QUEUE_INITIAL_CAPACITY 8
#define FAVORITE_DEBOUNCE_MS 150
#define FAVORITE_WORKER_STACK_SIZE (128 * 1024)

typedef struct {
    char * path;
    bool is_favorite;
    struct timespec deadline;
} favorite_req_t;

static pthread_t favorite_worker_thread;
static pthread_mutex_t favorite_worker_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t favorite_worker_cond;
static pthread_once_t favorite_worker_once = PTHREAD_ONCE_INIT;
static bool favorite_worker_ready = false;

static favorite_req_t * favorite_queue = NULL;
static int favorite_queue_count = 0;
static int favorite_queue_capacity = 0;

static void * favorite_worker_main(void * unused) {
    (void) unused;
    for (;;) {
        pthread_mutex_lock(&favorite_worker_mutex);
        while (favorite_queue_count == 0) {
            pthread_cond_wait(&favorite_worker_cond, &favorite_worker_mutex);
        }

        int ready_idx = -1;
        while (ready_idx < 0) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);

            struct timespec earliest = favorite_queue[0].deadline;
            int earliest_idx = 0;
            for (int i = 1; i < favorite_queue_count; i++) {
                if (favorite_queue[i].deadline.tv_sec < earliest.tv_sec ||
                    (favorite_queue[i].deadline.tv_sec == earliest.tv_sec &&
                     favorite_queue[i].deadline.tv_nsec < earliest.tv_nsec)) {
                    earliest = favorite_queue[i].deadline;
                    earliest_idx = i;
                }
            }

            if (now.tv_sec > earliest.tv_sec ||
                (now.tv_sec == earliest.tv_sec && now.tv_nsec >= earliest.tv_nsec)) {
                ready_idx = earliest_idx;
                break;
            }

            int ret = pthread_cond_timedwait(&favorite_worker_cond, &favorite_worker_mutex, &earliest);
            (void) ret;
            if (favorite_queue_count == 0) break;
        }

        if (ready_idx < 0) {
            pthread_mutex_unlock(&favorite_worker_mutex);
            continue;
        }

        /* Dequeue the ready request and take ownership */
        char * req_path = favorite_queue[ready_idx].path;
        bool req_fav = favorite_queue[ready_idx].is_favorite;

        for (int i = ready_idx; i < favorite_queue_count - 1; i++) {
            favorite_queue[i] = favorite_queue[i + 1];
        }
        favorite_queue_count--;

        /* Unlock worker mutex before calling metadata DB / file I/O */
        pthread_mutex_unlock(&favorite_worker_mutex);

        if (req_path) {
            metadata_db_song_favorite_set(req_path, req_fav);
            free(req_path);
        }
    }
    return NULL;
}

static void favorite_start_worker(void) {
    pthread_condattr_t cattr;
    if (pthread_condattr_init(&cattr) != 0) {
        fprintf(stderr, "gui_player: failed to initialize favorite worker condition variable\n");
        return;
    }
    if (pthread_condattr_setclock(&cattr, CLOCK_MONOTONIC) != 0 ||
        pthread_cond_init(&favorite_worker_cond, &cattr) != 0) {
        fprintf(stderr, "gui_player: failed to initialize favorite worker condition variable\n");
        pthread_condattr_destroy(&cattr);
        return;
    }
    pthread_condattr_destroy(&cattr);

    favorite_queue = calloc(FAVORITE_QUEUE_INITIAL_CAPACITY, sizeof(*favorite_queue));
    if (!favorite_queue) {
        fprintf(stderr, "gui_player: failed to allocate favorite persistence queue\n");
        pthread_cond_destroy(&favorite_worker_cond);
        return;
    }
    favorite_queue_capacity = FAVORITE_QUEUE_INITIAL_CAPACITY;

    pthread_attr_t attr;
    bool attr_initialized = pthread_attr_init(&attr) == 0;
    if (attr_initialized) pthread_attr_setstacksize(&attr, FAVORITE_WORKER_STACK_SIZE);
    int create_rc = pthread_create(&favorite_worker_thread, attr_initialized ? &attr : NULL,
                                   favorite_worker_main, NULL);
    if (attr_initialized) pthread_attr_destroy(&attr);
    if (create_rc == 0) {
        pthread_detach(favorite_worker_thread);
        favorite_worker_ready = true;
    } else {
        fprintf(stderr, "gui_player: failed to spawn favorite persistence worker thread\n");
        free(favorite_queue);
        favorite_queue = NULL;
        favorite_queue_capacity = 0;
        pthread_cond_destroy(&favorite_worker_cond);
    }
}

static void favorite_queue_submit(const char * path, bool is_favorite) {
    if (!path) return;
    pthread_once(&favorite_worker_once, favorite_start_worker);
    if (!favorite_worker_ready) {
        fprintf(stderr, "gui_player: favorite worker not ready; dropping async persistence\n");
        return;
    }

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    struct timespec deadline = now;
    deadline.tv_nsec += (long) FAVORITE_DEBOUNCE_MS * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec += deadline.tv_nsec / 1000000000L;
        deadline.tv_nsec %= 1000000000L;
    }

    pthread_mutex_lock(&favorite_worker_mutex);

    /* 1. Coalesce by exact path match if already queued */
    for (int i = 0; i < favorite_queue_count; i++) {
        if (strcmp(favorite_queue[i].path, path) == 0) {
            favorite_queue[i].is_favorite = is_favorite;
            favorite_queue[i].deadline = deadline;
            pthread_cond_signal(&favorite_worker_cond);
            pthread_mutex_unlock(&favorite_worker_mutex);
            return;
        }
    }

    /* 2. New track entry: allocate owned path copy */
    char * path_copy = strdup(path);
    if (!path_copy) {
        fprintf(stderr, "gui_player: strdup failed for favorite path '%s'\n", path);
        pthread_mutex_unlock(&favorite_worker_mutex);
        return;
    }

    /* Grow only for distinct tracks. Same-track tap storms are coalesced
     * above, while this path keeps the LVGL thread from waiting for slow
     * metadata or SD-card I/O and never evicts an acknowledged change. */
    if (favorite_queue_count >= favorite_queue_capacity) {
        int new_capacity = favorite_queue_capacity * 2;
        favorite_req_t * grown = realloc(favorite_queue,
                                          sizeof(*favorite_queue) * (size_t) new_capacity);
        if (!grown) {
            fprintf(stderr, "gui_player: failed to grow favorite persistence queue\n");
            free(path_copy);
            pthread_mutex_unlock(&favorite_worker_mutex);
            return;
        }
        favorite_queue = grown;
        favorite_queue_capacity = new_capacity;
    }

    favorite_queue[favorite_queue_count].path = path_copy;
    favorite_queue[favorite_queue_count].is_favorite = is_favorite;
    favorite_queue[favorite_queue_count].deadline = deadline;
    favorite_queue_count++;

    pthread_cond_signal(&favorite_worker_cond);
    pthread_mutex_unlock(&favorite_worker_mutex);
}

void favorite_icon_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (playlist_index < 0 || playlist_index >= playlist_count) return;

    const char * path = playlist_path_at(playlist_index);
    if (!path) return;

    favorite_is_set = !favorite_is_set;
    if (favorite_icon) {
        lv_image_set_src(favorite_icon, asset_path(favorite_is_set ? "playing_plane/collect_in.png" : "playing_plane/collect_out.png"));
    }
    gui_shell_update_quick_drawer_favorite(favorite_is_set);

    favorite_queue_submit(path, favorite_is_set);
}

void arm_next_track_for_audio(int index);

/* Shared by the player screen's own order_icon tap and the Remote Control
 * web UI's mode-cycle button (remote_control_consume_mode_cycle()) -- same
 * single 4-state cycle either way, so both surfaces stay in sync and
 * neither reimplements the mode-advance/persist/re-arm logic. */
void cycle_play_mode(void) {
    play_mode_t mode = (play_mode_t) current_settings.play_mode;
    mode = (play_mode_t) ((mode + 1) % 4);
    gui_player_set_play_mode(mode);
}

void gui_player_set_play_mode(int mode_value) {
    play_mode_t mode = (play_mode_t) mode_value;
    if (mode < PLAY_MODE_SEQUENTIAL || mode > PLAY_MODE_SHUFFLE) return;
    current_settings.play_mode = (int) mode;
    settings_save_async(&current_settings);

    lv_image_set_src(order_icon, asset_path(play_mode_icon_asset(mode)));
    gui_shell_update_quick_drawer_play_mode((int) mode);

    /* What comes after the current track changes with the mode (e.g.
     * entering Shuffle picks a random next instead of index+1) -- re-arm the
     * gapless/crossfade target immediately rather than waiting for whatever
     * was armed under the old mode to turn out wrong. */
    if (playlist_index >= 0) arm_next_track_for_audio(playlist_index);
}

static void order_icon_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    cycle_play_mode();
}

/* refresh_now_playing_indicators() is defined down with the compact-list
 * screens it updates (Artists/Albums/All Songs/group songs) -- forward-
 * declared here since apply_track_metadata_to_ui() below is the single
 * place a real "this track started playing" event is known to have
 * happened, for every playback source (local library, Group Songs, Files,
 * .m3u playlists). */

/* Shared by both an explicit track pick (play_track_at_from) and the audio
 * thread autonomously advancing into a queued next track on its own
 * (on_track_auto_advanced) -- title/folder/art/format-badge/progress-reset
 * are identical either way. Returns the metadata it read (out_meta) so
 * callers that also need ReplayGain (play_track_at_from, to hand it to
 * audio_play_file_at) don't have to read the file's tags a second time. */
/* Set by subsonic_song_row_click_cb() (gui.c, further down) right before a
 * streamed mp3/flac Subsonic queue starts playing -- metadata_read() can't
 * read tags from a network URL the way it does a local file, but the real
 * title/artist/album (and where to fetch cover art) for every song in the
 * queue are already known from the API responses that built it, so they're
 * stashed here, one entry per playlist[] slot (same indexing, built and
 * freed together), and matched by exact URL string in apply_track_metadata_
 * to_ui() below -- the string match (not just the index) is what makes it
 * safe to leave this array up indefinitely rather than needing to track
 * every other place a new, unrelated playlist might start: subsonic_build_
 * stream_url() bakes a fresh random salt into every URL it builds, so a
 * later, different playlist's paths can never coincidentally match one of
 * these even if this array outlives its own playlist (the array is only
 * ever replaced, on the next Subsonic streaming tap, not proactively freed
 * when some other playback source starts). Forward-declared here (rather
 * than moving apply_track_metadata_to_ui() itself) since the Subsonic click
 * handler that WRITES these lives much further down this file, alongside
 * the rest of the Subsonic screens. */

static audio_codec_t info_codec_from_hint(const char * hint) {
    if (!hint || !hint[0]) return AUDIO_CODEC_UNKNOWN;
    if (*hint == '.') hint++;
    if (strcasecmp(hint, "flac") == 0) return AUDIO_CODEC_FLAC;
    if (strcasecmp(hint, "mp3") == 0) return AUDIO_CODEC_MP3;
    if (strcasecmp(hint, "wav") == 0 || strcasecmp(hint, "aif") == 0 ||
        strcasecmp(hint, "aiff") == 0) return AUDIO_CODEC_PCM;
    if (strcasecmp(hint, "dsf") == 0 || strcasecmp(hint, "dff") == 0)
        return AUDIO_CODEC_DSD;
    if (strcasecmp(hint, "aac") == 0 || strcasecmp(hint, "aacp") == 0)
        return AUDIO_CODEC_AAC;
    if (strcasecmp(hint, "ape") == 0) return AUDIO_CODEC_APE;
    if (strcasecmp(hint, "wma") == 0 || strcasecmp(hint, "asf") == 0)
        return AUDIO_CODEC_WMA;
    if (strcasecmp(hint, "opus") == 0) return AUDIO_CODEC_OPUS;
    return AUDIO_CODEC_UNKNOWN; /* M4A/Ogg need the actual decoder to disambiguate. */
}

static void info_container_from_hint(const char * hint, char out[16]) {
    out[0] = '\0';
    if (!hint || !hint[0]) return;
    if (*hint == '.') hint++;
    if (strcasecmp(hint, "aif") == 0 || strcasecmp(hint, "aiff") == 0)
        snprintf(out, 16, "AIFF");
    else if (strcasecmp(hint, "aac") == 0 || strcasecmp(hint, "aacp") == 0)
        snprintf(out, 16, "ADTS");
    else if (strcasecmp(hint, "wma") == 0 || strcasecmp(hint, "asf") == 0)
        snprintf(out, 16, "ASF");
    else if (strcasecmp(hint, "m4a") == 0 || strcasecmp(hint, "mp4") == 0)
        snprintf(out, 16, "M4A");
    else if (strcasecmp(hint, "ogg") == 0 || strcasecmp(hint, "oga") == 0 ||
             strcasecmp(hint, "opus") == 0)
        snprintf(out, 16, "Ogg");
    else {
        size_t i = 0;
        while (hint[i] && i + 1 < 16) {
            out[i] = (char) toupper((unsigned char) hint[i]);
            i++;
        }
        out[i] = '\0';
    }
}

static bool is_http_url(const char * path) {
    if (!path) return false;
    return strncasecmp(path, "http://", 7) == 0 || strncasecmp(path, "https://", 8) == 0;
}

static const char * info_path_hint(const char * path) {
    if (!path) return NULL;
    if (is_http_url(path)) {
        /* Only this app's local-only #.<ext> hint is safe to inspect on a
         * stream. Never treat a URL path/query suffix as a container name:
         * besides being unreliable, a query can contain an auth token and
         * must not reach the Information UI even in truncated form. */
        const char * fragment = strrchr(path, '#');
        return fragment && fragment[1] == '.' ? fragment + 2 : NULL;
    }
    const char * base = strrchr(path, '/');
    base = base ? base + 1 : path;
    const char * dot = strrchr(base, '.');
    return dot ? dot + 1 : NULL;
}

void apply_track_metadata_to_ui(int index, track_metadata_t * out_meta) {
    /* Resolved once -- this is playlist_index's first real touch on every
     * track-start (play_track_at_from()/on_track_auto_advanced() both call
     * this immediately after setting playlist_index), so it's also where a
     * lazy All-Songs slot actually gets strdup'd. Every other playlist[index]
     * use below reuses this same resolved pointer rather than re-deriving
     * it (playlist_path_at() is idempotent/cheap on a re-call either way,
     * but there's no reason to). */
    const char * path = playlist_path_at(index);

    char title[128];
    char folder[128];
    get_display_names(path, title, sizeof(title), folder, sizeof(folder));

    bool is_subsonic_stream = subsonic_stream_meta && index < subsonic_stream_meta_count &&
                               strcmp(path, subsonic_stream_meta[index].url) == 0;
    remote_track_meta_t remote_meta;
    bool is_remote_track = remote_track_meta_copy_for_path(path, &remote_meta);

    if (is_remote_track) {
        /* No file to open for a remote track -- unlike the Subsonic branch
         * below, this also fixes the ReplayGain gap Subsonic has today
         * (that branch never calls metadata_read(), so has_replaygain stays
         * false): the plugin declares replaygain_db up front from its own
         * catalog metadata, fed into resolve_replaygain() the same as any
         * local track's tag would be. */
        memset(out_meta, 0, sizeof(*out_meta));
        if (remote_meta.title[0]) {
            snprintf(out_meta->title, sizeof(out_meta->title), "%s", remote_meta.title);
            out_meta->has_title = true;
        }
        if (remote_meta.artist[0]) {
            snprintf(out_meta->artist, sizeof(out_meta->artist), "%s", remote_meta.artist);
            out_meta->has_artist = true;
        }
        if (remote_meta.album[0]) {
            snprintf(out_meta->album, sizeof(out_meta->album), "%s", remote_meta.album);
            out_meta->has_album = true;
        }
        if (remote_meta.has_replaygain) {
            out_meta->has_replaygain = true;
            out_meta->replaygain_gain_db = remote_meta.replaygain_db;
        }
    } else if (is_subsonic_stream) {
        memset(out_meta, 0, sizeof(*out_meta));
        if (subsonic_stream_meta[index].title[0]) {
            snprintf(out_meta->title, sizeof(out_meta->title), "%s", subsonic_stream_meta[index].title);
            out_meta->has_title = true;
        }
        if (subsonic_stream_meta[index].artist[0]) {
            snprintf(out_meta->artist, sizeof(out_meta->artist), "%s", subsonic_stream_meta[index].artist);
            out_meta->has_artist = true;
        }
        if (subsonic_stream_meta[index].album[0]) {
            snprintf(out_meta->album, sizeof(out_meta->album), "%s", subsonic_stream_meta[index].album);
            out_meta->has_album = true;
        }
    } else {
        metadata_read_without_artwork(path, out_meta); /* already normalized */
    }
    /* Stream and remote catalog text reaches the rows too: same single-line,
     * non-empty rules as file tags. */
    if (is_remote_track || is_subsonic_stream) metadata_normalize_text_tags(out_meta);

    gui_track_info_context_t info = {0};
    snprintf(info.path, sizeof(info.path), "%s", path);
    info.replaygain_mode = current_settings.replaygain_mode;
    info.has_replaygain_track = out_meta->has_replaygain;
    info.replaygain_track_db = out_meta->replaygain_gain_db;
    info.has_replaygain_album = out_meta->has_replaygain_album;
    info.replaygain_album_db = out_meta->replaygain_album_gain_db;
    info.has_track_number = out_meta->has_track_number;
    info.track_number = out_meta->track_number;
    info.has_disc_number = out_meta->has_disc_number;
    info.disc_number = out_meta->disc_number;

    const char * format_hint = info_path_hint(path);
    if (is_remote_track) {
        info.source = GUI_TRACK_SOURCE_PLUGIN;
        snprintf(info.provider, sizeof(info.provider), "%s", remote_meta.provider);
        snprintf(info.track_id, sizeof(info.track_id), "%s", remote_meta.track_id);
        format_hint = remote_meta.codec;
        info.declared_codec = info_codec_from_hint(remote_meta.codec);
        if (info.declared_codec == AUDIO_CODEC_UNKNOWN) format_hint = NULL;
        info.declared_sample_rate = remote_meta.sample_rate;
        info.declared_bit_depth = remote_meta.bit_depth;
        info.declared_channels = remote_meta.channels;
        info.declared_bitrate_kbps = remote_meta.bitrate_kbps;
        info.declared_duration_seconds = (double) remote_meta.duration_ms / 1000.0;
    } else if (is_subsonic_stream) {
        const subsonic_stream_song_meta_t * sm = &subsonic_stream_meta[index];
        info.source = GUI_TRACK_SOURCE_SUBSONIC;
        format_hint = sm->suffix;
        info.declared_codec = info_codec_from_hint(sm->suffix);
        info.declared_sample_rate = sm->sample_rate;
        info.declared_bit_depth = sm->bit_depth;
        info.declared_channels = sm->channels;
        info.declared_bitrate_kbps = sm->bitrate_kbps;
        info.declared_duration_seconds = sm->duration_seconds;
        info.has_track_number = sm->track > 0;
        info.track_number = sm->track;
        info.has_disc_number = sm->disc > 0;
        info.disc_number = sm->disc;
    } else if (is_http_url(path)) {
        info.source = GUI_TRACK_SOURCE_RADIO;
        info.declared_codec = info_codec_from_hint(format_hint);
        if (info.declared_codec == AUDIO_CODEC_UNKNOWN) format_hint = NULL;
    } else {
        info.source = GUI_TRACK_SOURCE_LOCAL;
        info.declared_codec = info_codec_from_hint(format_hint);
    }
    info_container_from_hint(format_hint, info.container);
    gui_track_info_set_current(&info);

    snprintf(now_playing_path, sizeof(now_playing_path), "%s", path);
    refresh_now_playing_indicators();

    const char * title_text = out_meta->has_title ? out_meta->title : title;
    const char * folder_text = out_meta->has_artist ? out_meta->artist : folder;
    const char * album_text = out_meta->has_album ? out_meta->album : "";
    snprintf(now_playing_genre, sizeof(now_playing_genre), "%s", out_meta->has_genre ? out_meta->genre : "");
    now_playing_track_number = out_meta->has_track_number && out_meta->track_number > 0 ? out_meta->track_number : 0;
    if (is_subsonic_stream && subsonic_stream_meta[index].track > 0)
        now_playing_track_number = subsonic_stream_meta[index].track;

    lv_label_set_text(song_title_label, title_text);
    if (album_label) lv_label_set_text(album_label, album_text);
    lv_label_set_text(artist_label, folder_text);
    gui_shell_update_quick_drawer_track(title_text, folder_text, album_text);
    refresh_format_badge();
    if (is_remote_track && remote_meta.artwork_url[0]) {
        launch_cover_decode_from_url(index, remote_meta.artwork_url, remote_meta.verify_tls);
    } else if (is_subsonic_stream && subsonic_stream_meta[index].cover_url[0]) {
        launch_cover_decode_from_url(index, subsonic_stream_meta[index].cover_url, subsonic_stream_meta[index].verify_tls);
    } else if (is_remote_track) {
        /* No artwork_url and nothing local to fall back to -- unlike the
         * plain "no embedded picture" case below, launch_cover_decode_
         * from_track() must not be tried: path is the synthetic remote://
         * key, not a real file. */
    } else {
        free(out_meta->picture_data);
        out_meta->picture_data = NULL;
        launch_cover_decode_from_track(index, path, out_meta->artist, out_meta->album, out_meta->album_artist);
    }

    /* No local sidecar (or embedded tag) makes sense for a Subsonic stream
     * or remote-provider URL -- same reasoning cover art uses a server-
     * supplied cover URL instead of a local-file lookup for those. A
     * streamed track just never gets lyrics in this first release. */
    if (!is_subsonic_stream && !is_remote_track) {
        gui_lyrics_load_track(index, path);
    } else {
        gui_lyrics_load_track(index, NULL);
    }
    /* out_meta->lyrics (populated by the metadata_read() call above, if this
     * track has embedded lyrics) is never read here -- launch_lyrics_load()
     * just above does its own independent metadata_read() on a background
     * thread instead of sharing this one (see lyrics_load_thread_func()'s
     * own comment for why: this function runs synchronously on the UI
     * thread at every track change, and re-parsing tags a second time in
     * the background is cheaper than plumbing a malloc'd pointer through a
     * separate async load path that already does its own file I/O anyway).
     * Always free it here so every caller's stack-local track_metadata_t
     * doesn't leak it. */
    free(out_meta->lyrics);
    out_meta->lyrics = NULL;

    favorite_is_set = metadata_db_song_favorite_is_set(path);
    const char * favorite_icon_asset = favorite_is_set ? "playing_plane/collect_in.png" : "playing_plane/collect_out.png";
    lv_image_set_src(favorite_icon, asset_path(favorite_icon_asset));
    gui_shell_update_quick_drawer_favorite(favorite_is_set);

    /* Once per real "this track started playing" event -- apply_track_
     * metadata_to_ui() is called exactly here for both an explicit pick
     * (play_track_at_from) and a gapless auto-advance (on_track_
     * auto_advanced), never on a repeat UI refresh of the same still-
     * playing track, so this can't double-count. Backs the Most Played
     * auto-generated playlist (Music > Playlists). */
    metadata_db_song_play_count_increment(path);

    /* The real position/duration come from audio_get_*_seconds() once the
     * decoder's opened -- the timer picks that up within its next tick. */
    lv_slider_set_value(progress_slider, 0, LV_ANIM_OFF);
    displayed_progress_percent = 0;
    displayed_position_second = -1;
    displayed_duration_second = -1;
    lv_label_set_text(pos_label, "0:00");
    lv_label_set_text(dur_label, "0:00");

    lv_label_set_text_fmt(song_count_label, "%d/%d", index + 1, playlist_count);
    lv_obj_remove_flag(song_count_label, LV_OBJ_FLAG_HIDDEN);
    player_transition_mark_dirty(); /* title/artist/format badge/progress reset above all just changed player_screen's own content -- see the cache's own doc comment */
}

/* Resolves which of a track's own ReplayGain fields to actually hand to
 * audio.c, per Settings -> Playback -> ReplayGain's mode: Off (no gain at
 * all), Per Track (the default -- normalizes every track to the same
 * perceived loudness), or Per Album (preserves intentional relative
 * loudness differences between tracks on the same album). Falls back to
 * track gain when Per Album is selected but this particular file has no
 * album-level tag -- most taggers only write one or the other depending on
 * whether the rip was tagged as a whole album or track-at-a-time, and
 * silently playing unnormalized instead of falling back to whatever IS
 * available would be a worse outcome than just using track gain here. */
void resolve_replaygain(const track_metadata_t * meta, bool * out_has_gain, double * out_gain_db,
                                bool * out_has_peak, double * out_peak) {
    int mode = current_settings.replaygain_mode;
    if (mode == 2 && meta->has_replaygain_album) {
        *out_has_gain = true;
        *out_gain_db = meta->replaygain_album_gain_db;
        if (meta->has_replaygain_album_peak) {
            *out_has_peak = true;
            *out_peak = meta->replaygain_album_peak;
        } else {
            *out_has_peak = meta->has_replaygain_peak;
            *out_peak = meta->replaygain_peak;
        }
        return;
    }
    bool use_track = mode != 0;
    *out_has_gain = use_track && meta->has_replaygain;
    *out_gain_db = meta->replaygain_gain_db;
    *out_has_peak = use_track && meta->has_replaygain_peak;
    *out_peak = meta->replaygain_peak;
}

/* ---- Delete confirmation popup ---- */

static void hide_delete_song_popup(void) {
    gui_popup_hide(&delete_song_popup);
}

static void delete_song_popup_backdrop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_delete_song_popup();
}

static void delete_song_cancel_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_delete_song_popup();
}

static void delete_song_confirm_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_delete_song_popup();
    if (playlist_index < 0 || playlist_index >= playlist_count) return;

    char * to_delete = strdup(playlist_path_at(playlist_index));
    int del_index = playlist_index;

    /* What to play next is decided before removing the entry: the track
     * shifting into del_index, or the new last track if the deleted one
     * was last. */
    free(playlist[del_index]);
    for (int i = del_index; i < playlist_count - 1; i++) playlist[i] = playlist[i + 1];
    /* Kept in lockstep -- see playlist_lazy_sort_order's own comment. */
    if (playlist_lazy_sort_order) {
        for (int i = del_index; i < playlist_count - 1; i++) playlist_lazy_sort_order[i] = playlist_lazy_sort_order[i + 1];
    }
    playlist_count--;

    if (playlist_count == 0) {
        audio_stop();
        plugin_manager_notify_stopped();
        free(playlist);
        playlist = NULL;
        playlist_index = -1;
        free(playlist_lazy_sort_order);
        playlist_lazy_sort_order = NULL;
        clear_player_source();
        set_play_button_state(false);
        lv_label_set_text(song_title_label, "No track loaded");
        if (album_label) lv_label_set_text(album_label, "");
        now_playing_genre[0] = '\0';
        now_playing_track_number = 0;
        if (artist_label) lv_label_set_text(artist_label, "");
        nav_pop(); /* nothing left to show on the player screen */
    } else {
        int new_index = (del_index < playlist_count) ? del_index : playlist_count - 1;
        play_track_at(new_index);
    }

    unlink(to_delete); /* only after playback has moved off it */
    free(to_delete);
    show_error_toast("Song deleted");
}

/* Defined alongside build_confirm_popup() above -- see its own doc comment
 * for why this is a separate shared shape (an N-row menu, not a yes/no
 * confirmation) and why both are forward-declared here. */
/* menu_popup_row_t and build_menu_popup declared in gui.h */

static void build_delete_song_popup(void) {
    /* LV_LABEL_LONG_DOT, not WRAP -- this title's text is set later
     * (delete_song_confirm_prompt() below) to "Delete <filename>?..." with
     * an arbitrary-length real filename spliced in, so it needs to
     * truncate rather than potentially wrap across several lines. */
    delete_song_popup.popup = build_confirm_popup("", LV_LABEL_LONG_DOT, &delete_song_popup_title, NULL, "Delete",
                                             lv_color_make(255, 120, 120), delete_song_confirm_cb, NULL, "Cancel",
                                             accent_lv_color(), delete_song_cancel_cb, NULL,
                                             delete_song_popup_backdrop_cb, &delete_song_popup.backdrop);
}

/* ---- The "more" 3-row menu itself ---- */

void hide_more_menu_popup(void) {
    lv_obj_add_flag(more_menu_popup_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(more_menu_popup, LV_OBJ_FLAG_HIDDEN);
}

bool gui_player_boot_prompt_blocked(void) {
    return (volume_popup && !lv_obj_has_flag(volume_popup, LV_OBJ_FLAG_HIDDEN)) ||
           (more_menu_popup && !lv_obj_has_flag(more_menu_popup, LV_OBJ_FLAG_HIDDEN)) ||
           (more_menu_popup_backdrop && !lv_obj_has_flag(more_menu_popup_backdrop, LV_OBJ_FLAG_HIDDEN));
}

static void more_menu_popup_backdrop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_more_menu_popup();
}

static void more_menu_add_to_playlist_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_more_menu_popup();
    if (playlist_index < 0) return;
    open_add_to_playlist_for(playlist_path_at(playlist_index));
}

static void more_menu_queue_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_more_menu_popup();
    open_queue_screen();
}

static void more_menu_information_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_more_menu_popup();
    gui_track_info_open();
}

static void more_menu_eq_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_more_menu_popup();
    nav_push(gui_settings_get_eq_screen());
}

static void more_menu_delete_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_more_menu_popup();
    if (playlist_index < 0 || playlist_index >= playlist_count) return;

    lv_label_set_text_fmt(delete_song_popup_title, "Delete %s?\nThis cannot be undone.", basename_of(playlist_path_at(playlist_index)));
    gui_popup_show(&delete_song_popup);
}

static void more_icon_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (playlist_index < 0) return;

    lv_obj_remove_flag(more_menu_popup_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(more_menu_popup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(more_menu_popup_backdrop);
    lv_obj_move_foreground(more_menu_popup);
}

static void build_more_menu_popup(void) {
    static const menu_popup_row_t rows[] = {
        { "Queue", more_menu_queue_cb, false },
        { "Add to Playlist", more_menu_add_to_playlist_cb, false },
        { "Information", more_menu_information_cb, false },
        { "EQ", more_menu_eq_cb, false },
        { "Delete", more_menu_delete_cb, true },
    };
    more_menu_popup = build_menu_popup(rows, (int) (sizeof(rows) / sizeof(rows[0])), more_menu_popup_backdrop_cb,
                                        &more_menu_popup_backdrop);
}

/* Song context menu moved to gui_queue.c. */

typedef struct {
    lv_obj_t * obj;
    int32_t x, y, w, h;
    int32_t compact_x, compact_y, compact_w, compact_h;
    int32_t height_style;
    int32_t normal_pad_left;
    int32_t centered_text_inset;
} player_lyrics_geometry_t;

static bool player_lyrics_open;
static bool player_lyrics_animating;
#define PLAYER_LYRICS_MORPH_OBJECT_COUNT 4
static player_lyrics_geometry_t player_lyrics_geometry[PLAYER_LYRICS_MORPH_OBJECT_COUNT];
static lv_obj_t * player_lyrics_hidden[32];
static unsigned player_lyrics_hidden_count;
static int32_t player_lyrics_content_top;
static lv_image_header_t player_lyrics_cover_header;

static int32_t player_lyrics_lerp(int32_t a, int32_t b, int32_t progress) {
    return a + (int32_t) (((int64_t) (b - a) * progress) / 1024);
}

static void player_lyrics_morph(void * unused, int32_t progress) {
    (void) unused;
    for (unsigned i = 0; i < PLAYER_LYRICS_MORPH_OBJECT_COUNT; ++i) {
        player_lyrics_geometry_t * g = &player_lyrics_geometry[i];
        lv_obj_set_pos(g->obj, player_lyrics_lerp(g->x, g->compact_x, progress),
                              player_lyrics_lerp(g->y, g->compact_y, progress));
        lv_obj_set_size(g->obj, player_lyrics_lerp(g->w, g->compact_w, progress),
                               player_lyrics_lerp(g->h, g->compact_h, progress));
        if (i) {
            /* Move the visible text, not just its label box. Keeping LEFT
             * alignment throughout the morph avoids the old end-of-exit
             * snap from the box's left edge to its centered text position. */
            lv_obj_set_style_pad_left(g->obj, g->normal_pad_left +
                player_lyrics_lerp(g->centered_text_inset, 0, progress), 0);
        }
    }
    /* The decoded image stays the same: only its scale and card bounds move.
     * Cache the image header once per transition, not on each animation tick. */
    int32_t size = player_lyrics_lerp(player_lyrics_geometry[0].w,
                                    player_lyrics_geometry[0].compact_w, progress);
    if (player_lyrics_cover_header.w && player_lyrics_cover_header.h) {
        int32_t sx = (size * LV_SCALE_NONE + player_lyrics_cover_header.w - 1) / player_lyrics_cover_header.w;
        int32_t sy = (size * LV_SCALE_NONE + player_lyrics_cover_header.h - 1) / player_lyrics_cover_header.h;
        lv_image_set_scale(cover_img, sx > sy ? sx : sy);
        /* LVGL alignment writes coordinates; it is not a persistent layout
         * constraint. Recenter as the parent card changes size so the image
         * follows the card throughout the reverse morph instead of snapping
         * into place only when fit_cover_img_to_card() runs at completion. */
        lv_obj_center(cover_img);
    }
}

static void player_lyrics_restore_controls(void) {
    for (unsigned i = 0; i < player_lyrics_hidden_count; ++i)
        lv_obj_remove_flag(player_lyrics_hidden[i], LV_OBJ_FLAG_HIDDEN);
    player_lyrics_hidden_count = 0;
    for (unsigned i = 1; i < PLAYER_LYRICS_MORPH_OBJECT_COUNT; ++i) {
        lv_obj_set_style_pad_left(player_lyrics_geometry[i].obj,
                                 player_lyrics_geometry[i].normal_pad_left, 0);
        lv_obj_set_height(player_lyrics_geometry[i].obj, player_lyrics_geometry[i].height_style);
        lv_obj_set_style_text_align(player_lyrics_geometry[i].obj, LV_TEXT_ALIGN_CENTER, 0);
    }
}

static void player_lyrics_morph_done(lv_anim_t * anim) {
    (void) anim;
    player_lyrics_animating = false;
    if (player_lyrics_open) {
        gui_lyrics_show_embedded(player_screen, player_lyrics_content_top,
                                BOARD_SCREEN_HEIGHT - player_lyrics_content_top - player_y(32));
    } else {
        player_lyrics_restore_controls();
    }
    fit_cover_img_to_card();
    player_transition_mark_dirty();
}

static void player_lyrics_hide_object(lv_obj_t * obj) {
    if (!obj || lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) return;
    if (player_lyrics_hidden_count >= sizeof(player_lyrics_hidden) / sizeof(player_lyrics_hidden[0])) return;
    player_lyrics_hidden[player_lyrics_hidden_count++] = obj;
    lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

static void player_lyrics_set_open(bool open, bool animate) {
    if (!player_screen || (animate && player_lyrics_animating)) return;
    if (open == player_lyrics_open && !player_lyrics_animating) return;
    lv_anim_delete(player_screen, player_lyrics_morph);
    if (open) {
        lv_obj_update_layout(player_screen);
        lv_obj_t * objects[] = { cover_card, song_title_label, artist_label, album_label };
        int32_t cover_size = player_s(112);
        int32_t left = player_x(24), top = player_y(48);
        int32_t text_x = left + cover_size + player_x(16);
        int32_t text_y = top;
        for (unsigned i = 0; i < PLAYER_LYRICS_MORPH_OBJECT_COUNT; ++i) {
            player_lyrics_geometry_t * g = &player_lyrics_geometry[i];
            g->obj = objects[i];
            g->x = lv_obj_get_x(g->obj); g->y = lv_obj_get_y(g->obj);
            g->w = lv_obj_get_width(g->obj); g->h = lv_obj_get_height(g->obj);
            g->height_style = lv_obj_get_style_height(g->obj, 0);
            g->normal_pad_left = lv_obj_get_style_pad_left(g->obj, 0);
            g->compact_x = i ? text_x : left;
            g->compact_y = i ? text_y : top;
            g->compact_w = i ? BOARD_SCREEN_WIDTH - text_x - left : cover_size;
            g->compact_h = i ? g->h : cover_size;
            if (i) {
                text_y += g->compact_h + player_y(6);
                lv_obj_set_style_text_align(g->obj, LV_TEXT_ALIGN_LEFT, 0);
            }
        }
        player_lyrics_content_top = (text_y > top + cover_size ? text_y : top + cover_size) + player_y(16);
        /* Hide both visual controls and their separate, enlarged touch targets.
         * Remember only visible objects so hidden state is restored exactly. */
        for (uint32_t i = 0; i < lv_obj_get_child_count(player_screen); ++i) {
            lv_obj_t * child = lv_obj_get_child(player_screen, i);
            if (child == player_overlay_panel || child == cover_card || child == song_title_label ||
                child == album_label || child == artist_label) continue;
            player_lyrics_hide_object(child);
        }
        player_lyrics_hide_object(favorite_circle);
    } else {
        gui_lyrics_hide_embedded();
    }
    /* Measure once per direction, since a new track may have been selected
     * while lyrics were open. Do not measure fonts on animation frames. */
    for (unsigned i = 1; i < PLAYER_LYRICS_MORPH_OBJECT_COUNT; ++i) {
        player_lyrics_geometry_t * g = &player_lyrics_geometry[i];
        lv_point_t text_size;
        lv_text_get_size(&text_size, lv_label_get_text(g->obj),
                        lv_obj_get_style_text_font(g->obj, 0),
                        lv_obj_get_style_text_letter_space(g->obj, 0),
                        lv_obj_get_style_text_line_space(g->obj, 0),
                        LV_COORD_MAX, LV_TEXT_FLAG_EXPAND);
        int32_t available = g->w - g->normal_pad_left - lv_obj_get_style_pad_right(g->obj, 0);
        g->centered_text_inset = text_size.x < available ? (available - text_size.x) / 2 : 0;
        lv_obj_set_style_text_align(g->obj, LV_TEXT_ALIGN_LEFT, 0);
    }
    player_lyrics_open = open;
    memset(&player_lyrics_cover_header, 0, sizeof(player_lyrics_cover_header));
    lv_image_decoder_get_info(lv_image_get_src(cover_img), &player_lyrics_cover_header);
    if (!animate) {
        player_lyrics_morph(NULL, open ? 1024 : 0);
        player_lyrics_morph_done(NULL);
        return;
    }
    player_lyrics_animating = true;
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, player_screen);
    lv_anim_set_values(&anim, open ? 0 : 1024, open ? 1024 : 0);
    lv_anim_set_duration(&anim, 240);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_in_out);
    lv_anim_set_exec_cb(&anim, player_lyrics_morph);
    lv_anim_set_completed_cb(&anim, player_lyrics_morph_done);
    lv_anim_start(&anim);
}

static void cover_img_tap_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (player_lyrics_open)
        player_lyrics_set_open(false, true);
    else if (current_settings.lyrics_enabled)
        player_lyrics_set_open(true, true);
}

static void player_lyrics_screen_unloaded_cb(lv_event_t * e) {
    (void) e;
    player_lyrics_set_open(false, false);
}


typedef struct {
    lv_obj_t * img;
    const char * normal_path;
    const char * pressed_path;
} transport_btn_ctx_t;

static void transport_btn_press_event_cb(lv_event_t * e) {
    transport_btn_ctx_t * ctx = (transport_btn_ctx_t *) lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        lv_image_set_src(ctx->img, asset_path(ctx->pressed_path));
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        lv_image_set_src(ctx->img, asset_path(ctx->normal_path));
    }
}

static void transport_btn_ctx_delete_cb(lv_event_t * e) {
    free(lv_event_get_user_data(e));
}

/* Long-press seeking on next/prev buttons (fast-forward/rewind).
 *
 * `transport_seek_target_seconds` accumulates seek offsets persistently
 * so rapid repeat events advance smoothly without re-reading intermediate
 * positions before earlier seeks complete.
 *
 * Seeking triggers on LV_EVENT_LONG_PRESSED and repeats on
 * LV_EVENT_LONG_PRESSED_REPEAT. Target positions are clamped short of EOF
 * by TRANSPORT_SEEK_EOF_GUARD_SECONDS to prevent auto-advancing into the
 * next track while holding.
 *
 * If playback generation changes during a hold (indicating a track
 * transition), `transport_seek_hold_cancelled` stops further seeks until
 * the next press.
 *
 * `dir` is +1 for forward seeking and -1 for rewinding. */
#define TRANSPORT_SEEK_STEP_SECONDS 3.0
#define TRANSPORT_SEEK_EOF_GUARD_SECONDS 0.5

/* Shared math for both the touch long-press repeat below and the physical
 * Next button's own hold (gui_player_hw_next_seek_steps()) -- each input
 * source keeps its own target_seconds/playback_generation/hold_cancelled
 * triple so a hold on one doesn't interfere with a hold on the other. */
static void apply_transport_seek_step(double dir, int step_count, bool is_first, double * target_seconds,
                                       uint64_t * playback_generation, bool * hold_cancelled) {
    if (step_count <= 0) return;
    uint64_t gen = audio_get_playback_generation();
    if (is_first) {
        *target_seconds = audio_get_position_seconds();
        *playback_generation = gen;
        *hold_cancelled = false;
    } else if (gen != *playback_generation) {
        *hold_cancelled = true;
    }
    if (*hold_cancelled) return;
    *target_seconds += dir * TRANSPORT_SEEK_STEP_SECONDS * (double) step_count;
    if (*target_seconds < 0.0) *target_seconds = 0.0;
    double duration = audio_get_duration_seconds();
    double max_target = duration > TRANSPORT_SEEK_EOF_GUARD_SECONDS ? duration - TRANSPORT_SEEK_EOF_GUARD_SECONDS : 0.0;
    if (*target_seconds > max_target) *target_seconds = max_target;
    audio_seek(*target_seconds);
}

static double transport_seek_target_seconds;
static uint64_t transport_seek_playback_generation;
static bool transport_seek_hold_cancelled;

static void transport_seek_repeat_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_LONG_PRESSED && code != LV_EVENT_LONG_PRESSED_REPEAT) return;
    double dir = (double) (intptr_t) lv_event_get_user_data(e);
    apply_transport_seek_step(dir, 1, code == LV_EVENT_LONG_PRESSED, &transport_seek_target_seconds,
                               &transport_seek_playback_generation, &transport_seek_hold_cancelled);
}

/* Physical Next button, held -- see hw_buttons_consume_next_seek_steps()'s
 * own comment. Called from gui.c's update_timer_cb with however many steps
 * accumulated since the last poll. */
static double hw_next_seek_target_seconds;
static uint64_t hw_next_seek_playback_generation;
static bool hw_next_seek_hold_cancelled;

void gui_player_hw_next_seek_steps(int step_count, bool is_first) {
    apply_transport_seek_step(1.0, step_count, is_first, &hw_next_seek_target_seconds,
                               &hw_next_seek_playback_generation, &hw_next_seek_hold_cancelled);
}

/* LV_EVENT_CLICKED still fires on release even after a long press (LVGL's
 * own documented behavior -- "regardless of long press"), so a hold-then-
 * release would otherwise ALSO skip/restart the track right after fast-
 * forwarding/rewinding it. Same fix already established in this codebase
 * for the exact same problem (quick_drawer_wifi_long_press_fired/
 * quick_drawer_bt_long_press_fired in gui_shell.c): a flag set here on
 * LV_EVENT_LONG_PRESSED, checked and cleared at the top of the CLICKED
 * handler to skip that one release's normal tap action.
 *
 * Bug caught in review: that flag was previously only ever cleared inside
 * the CLICKED handler -- but a press that ends via LV_EVENT_PRESS_LOST
 * (finger dragged off the object before release) never gets a matching
 * CLICKED at all (confirmed against lv_indev.c: PRESS_LOST and RELEASED/
 * CLICKED are emitted from disjoint code paths, never both for the same
 * press). A long-hold-then-drag-off would leave the flag stuck true
 * forever, silently discarding the very next legitimate short tap on that
 * same button. transport_long_press_cancel_cb() below clears it on
 * PRESS_LOST too so a cancelled hold can't outlive its own press. */
static bool next_btn_long_press_fired = false;
static bool prev_btn_long_press_fired = false;

static void transport_long_press_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_LONG_PRESSED) return;
    bool * fired = (bool *) lv_event_get_user_data(e);
    *fired = true;
}

static void transport_long_press_cancel_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_PRESS_LOST) return;
    bool * fired = (bool *) lv_event_get_user_data(e);
    *fired = false;
}

#ifdef UI_GESTURE_TRACE
static void debug_transport_btn_all_cb(lv_event_t * e) {
    printf("[TRANSPORT_TRACE] obj=%p code=%d clickable=%d state=0x%x\n",
           (void *) lv_event_get_target(e), (int) lv_event_get_code(e),
           lv_obj_has_flag(lv_event_get_target(e), LV_OBJ_FLAG_CLICKABLE),
           (unsigned) lv_obj_get_state(lv_event_get_target(e)));
}
#endif

/* Extends the vertical reach of transport buttons (mode/play/prev/next/more)
 * up to a single shared top line (roughly level with song_count_label).
 * A separate invisible, absolutely-positioned sibling marked
 * LV_OBJ_FLAG_IGNORE_LAYOUT provides vertical hit area without altering
 * horizontal padding between neighboring buttons.
 *
 * `top_y`/`bottom_y_exclusive` are absolute screen coordinates resolved
 * after lv_obj_update_layout(). The resulting object covers the hit region
 * seamlessly. */
static lv_obj_t * add_transport_hit_target(lv_obj_t * scr, int32_t center_x, int32_t width, int32_t top_y,
                                     int32_t bottom_y_exclusive, lv_event_cb_t cb, lv_color_t debug_color) {
    lv_obj_t * ext = lv_obj_create(scr);
    lv_obj_remove_style_all(ext);
    lv_obj_set_pos(ext, center_x - width / 2, top_y);
    lv_obj_set_size(ext, width, bottom_y_exclusive - top_y);
    lv_obj_add_flag(ext, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_remove_flag(ext, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ext, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ext, cb, LV_EVENT_CLICKED, NULL);
#ifdef UI_HITBOX_DEBUG
    lv_obj_set_style_border_width(ext, 3, 0);
    lv_obj_set_style_border_color(ext, debug_color, 0);
    lv_obj_set_style_border_opa(ext, LV_OPA_COVER, 0);
#else
    (void) debug_color;
#endif
    return ext;
}

/* Transport icons are visual-only elements inside controls_row.
 * Forwarding the hit target's PRESSED/RELEASED/PRESS_LOST onto the
 * underlying icon's state triggers icon_press_style dimming on real touch. */
static void forward_press_state_to_icon_cb(lv_event_t * e) {
    lv_obj_t * icon = (lv_obj_t *) lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) lv_obj_add_state(icon, LV_STATE_PRESSED);
    else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) lv_obj_clear_state(icon, LV_STATE_PRESSED);
}

static void library_btn_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_pop();
}

static lv_timer_t * pending_progress_seek_timer = NULL;
/* The actual seek target -- audio.c binds this percentage to the current
 * playback generation and converts it only after that generation's decoder
 * publishes total_frames, so a slow track start cannot reuse the previous
 * track's duration. */
static double pending_progress_seek_percent = 0.0;
/* Best-effort ESTIMATE of where that percent lands, in seconds -- used only
 * by the "has the seek landed yet" poll heuristic below, never passed to
 * audio_seek()/audio_seek_percent() itself. Fine if this is occasionally
 * stale (same track-switch race as above): the only consequence is the
 * progress UI falling back to its normal 8-tick timeout instead of
 * detecting the landed position early, not an incorrect seek. */
static double pending_progress_seek_seconds = 0.0;
bool user_seeking = false;
/* True from finger-up until audio position catches the requested seek, so
 * the 500ms progress poll does not yank the knob back to the pre-seek
 * playhead while the playback thread is applying it. */
static bool progress_awaiting_seek = false;
static int progress_awaiting_seek_ticks = 0;

static void cancel_pending_progress_seek(void) {
    if (pending_progress_seek_timer) {
        lv_timer_delete(pending_progress_seek_timer);
        pending_progress_seek_timer = NULL;
    }
}

static void pending_progress_seek_timer_cb(lv_timer_t * timer) {
    (void) timer;
    pending_progress_seek_timer = NULL;
    audio_seek_percent(pending_progress_seek_percent);
}

static void player_label_enable_marquee(lv_obj_t * label) {
    static lv_anim_t player_marquee_anim;
    static bool initialized = false;
    if (!initialized) {
        lv_anim_init(&player_marquee_anim);
        /* 2s, matching player_secondary_label_enable_marquee()'s own wait
         * below -- was 3s, inconsistent with it for no functional reason. */
        lv_anim_set_delay(&player_marquee_anim, 2000);
        lv_anim_set_repeat_delay(&player_marquee_anim, 2000);
        initialized = true;
    }

    /* Local style properties outrank row_label_enable_marquee()'s shared
     * style. Install the player-only pause before that helper enables and
     * refreshes circular scrolling, while retaining its speed duration. */
    lv_obj_set_style_anim(label, &player_marquee_anim, LV_PART_MAIN);
    row_label_enable_marquee(label);
}

static void player_secondary_label_enable_marquee(lv_obj_t * label) {
    static lv_anim_t player_secondary_marquee_anim;
    static bool initialized = false;
    if (!initialized) {
        lv_anim_init(&player_secondary_marquee_anim);
        lv_anim_set_delay(&player_secondary_marquee_anim, 2000);
        lv_anim_set_repeat_delay(&player_secondary_marquee_anim, 2000);
        initialized = true;
    }

    lv_obj_set_style_anim(label, &player_secondary_marquee_anim, LV_PART_MAIN);
    row_label_enable_marquee(label);
}

/* Fixed three-line metadata block above the cover: title, artist, album.
 * Each row is one line of its own font, stacked with a uniform gap. The
 * fonts are fixed per board, so this resolves to one layout per board. */
static void player_layout_metadata_stack(void) {
    if (!song_title_label || !album_label || !artist_label) return;

    const lv_font_t * title_font = lv_obj_get_style_text_font(song_title_label, 0);
    const lv_font_t * artist_font = lv_obj_get_style_text_font(artist_label, 0);
    const lv_font_t * album_font = lv_obj_get_style_text_font(album_label, 0);
    int32_t title_h = title_font ? lv_font_get_line_height(title_font) : player_y(32);
    int32_t artist_h = artist_font ? lv_font_get_line_height(artist_font) : player_y(26);
    int32_t album_h = album_font ? lv_font_get_line_height(album_font) : player_y(26);

    const int32_t title_y = player_y(36);
    /* Close the gaps rather than let the block reach the cover. */
    int32_t gap = player_y(6);
    int32_t room = player_y(152) - title_y - title_h - artist_h - album_h;
    if (2 * gap > room) gap = room > 0 ? room / 2 : 0;
    const int32_t artist_y = title_y + title_h + gap;
    const int32_t album_y = artist_y + artist_h + gap;

    lv_obj_set_y(song_title_label, title_y);
    lv_obj_set_height(song_title_label, title_h);
    lv_obj_set_y(artist_label, artist_y);
    lv_obj_set_height(artist_label, artist_h);
    lv_obj_set_y(album_label, album_y);
    lv_obj_set_height(album_label, album_h);

    player_transition_mark_dirty();
}

static void progress_slider_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * slider = lv_event_get_target(e);

    if (code == LV_EVENT_PRESSED) {
        user_seeking = true;
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        double duration = audio_get_duration_seconds();
        int32_t percent = lv_slider_get_value(slider);
        pending_progress_seek_percent = (double) percent;
        pending_progress_seek_seconds = duration * ((double) percent / 100.0);
        displayed_progress_percent = percent;
        progress_awaiting_seek = (duration > 0.0);
        progress_awaiting_seek_ticks = 0;
        cancel_pending_progress_seek();
        pending_progress_seek_timer = lv_timer_create(pending_progress_seek_timer_cb, 150, NULL);
        lv_timer_set_repeat_count(pending_progress_seek_timer, 1);
        user_seeking = false;
    }
}

static lv_obj_t * build_player_screen(uint32_t screen_width, uint32_t screen_height) {
    (void) screen_width;
    (void) screen_height;
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_add_style(scr, &style_theme_screen_bg, 0);

    /* Full-screen fallback surface and background reflection image */
    player_overlay_panel = lv_obj_create(scr);
    lv_obj_set_style_pad_all(player_overlay_panel, 0, 0);
    lv_obj_set_size(player_overlay_panel, BOARD_SCREEN_WIDTH, BOARD_SCREEN_HEIGHT);
    lv_obj_set_pos(player_overlay_panel, 0, 0);
    lv_obj_add_style(player_overlay_panel, &style_theme_screen_bg, 0);
    lv_obj_set_style_bg_opa(player_overlay_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(player_overlay_panel, 0, 0);
    lv_obj_set_style_radius(player_overlay_panel, 0, 0);
    lv_obj_remove_flag(player_overlay_panel, LV_OBJ_FLAG_SCROLLABLE);

    player_background_img = lv_image_create(player_overlay_panel);
    lv_obj_add_flag(player_background_img, LV_OBJ_FLAG_HIDDEN);

    /* Cover card (clipped, crisp, unblurred) */
    cover_card = lv_obj_create(scr);
    lv_obj_remove_style_all(cover_card);
    /* Centered directly from the resolved card size rather than a fixed
     * offset literal -- that would only center a player_s(350) card when
     * player_x and player_s agree, which is true on R1 but not on the other
     * boards. 350x350 matches COVER_ART_WIDTH/HEIGHT (gui.h) exactly, so the
     * reference board needs no runtime scale at all in fit_cover_img_to_card(). */
    lv_obj_set_pos(cover_card, (BOARD_SCREEN_WIDTH - player_s(350)) / 2, player_y(152));
    lv_obj_set_size(cover_card, player_s(350), player_s(350));
    lv_obj_set_style_radius(cover_card, player_s(10), 0);
    lv_obj_set_style_clip_corner(cover_card, true, 0);
    lv_obj_set_style_bg_opa(cover_card, 0, 0);
    lv_obj_remove_flag(cover_card, LV_OBJ_FLAG_SCROLLABLE);

    cover_img = lv_image_create(cover_card);
    lv_image_set_src(cover_img, asset_path("playing_plane/default_cover_565.png"));
    lv_obj_add_flag(cover_img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(cover_img, cover_img_tap_cb, LV_EVENT_CLICKED, NULL);

    favorite_circle = lv_obj_create(cover_card);
    lv_obj_remove_style_all(favorite_circle);
    lv_obj_set_pos(favorite_circle, player_s(296), player_s(296)); /* 352-44-12=296 from each edge */
    lv_obj_set_size(favorite_circle, player_s(44), player_s(44));
    lv_obj_set_style_radius(favorite_circle, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(favorite_circle, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(favorite_circle, LV_OPA_40, 0);
    lv_obj_add_style(favorite_circle, gui_theme_accent_outline_style(), 0);
    lv_obj_remove_flag(favorite_circle, LV_OBJ_FLAG_SCROLLABLE);

    favorite_icon = lv_image_create(favorite_circle);
    lv_image_set_src(favorite_icon, asset_path("playing_plane/collect_out.png"));
    lv_obj_add_style(favorite_icon, &icon_press_style, LV_STATE_PRESSED);
    lv_obj_center(favorite_icon);

    /* The supplied reference is the R1 480x800 composition. Keep the
     * existing compact typography on shorter panels so the three metadata
     * lines do not collide with the cover there. */
    const bool reference_player = BOARD_SCREEN_HEIGHT >= 800;
    const lv_font_t * player_title_font = &app_font_player_title;
    const lv_font_t * player_meta_font = &app_font_player_meta;

    song_title_label = lv_label_create(scr);
    lv_label_set_text(song_title_label, "No track loaded");
    lv_obj_add_style(song_title_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(song_title_label, player_title_font, 0);
    lv_obj_set_style_text_align(song_title_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_x(song_title_label, player_x(22));
    lv_obj_set_width(song_title_label, BOARD_SCREEN_WIDTH - 2 * player_x(22));
    player_label_enable_marquee(song_title_label);

    album_label = lv_label_create(scr);
    lv_label_set_text(album_label, "");
    lv_obj_add_style(album_label, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(album_label, player_meta_font, 0);
    lv_obj_set_style_text_align(album_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_x(album_label, player_x(22));
    /* Content height so circular marquee does not fall back to vertical scrolling. */
    lv_obj_set_width(album_label, BOARD_SCREEN_WIDTH - 2 * player_x(22));
    lv_obj_set_height(album_label, LV_SIZE_CONTENT);
    player_secondary_label_enable_marquee(album_label);

    artist_label = lv_label_create(scr);
    lv_label_set_text(artist_label, "");
    lv_obj_add_style(artist_label, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(artist_label, player_meta_font, 0);
    lv_obj_set_style_text_align(artist_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_x(artist_label, player_x(22));
    /* Circular marquee falls back to vertical scrolling when text exceeds
     * a fixed height. Fit the actual font metrics (including loaded fallback
     * fonts) while retaining the fixed width for horizontal overflow. */
    lv_obj_set_width(artist_label, BOARD_SCREEN_WIDTH - 2 * player_x(22));
    lv_obj_set_height(artist_label, LV_SIZE_CONTENT);
    player_secondary_label_enable_marquee(artist_label);
    player_layout_metadata_stack();

    /* Back/dismiss button. build_header_back_button()'s shared 64x64 default
     * (TITLE_ROW_HEIGHT, used by every other screen's header) would reach
     * further into the album label's row than the symmetric 44x44 "more"
     * target on the opposite corner -- shrink to match it instead of
     * needlessly widening this corner's overlap with the metadata block. */
    player_dismiss_btn = build_header_back_button(scr, library_btn_event_cb);
    lv_obj_set_size(player_dismiss_btn, player_s(44), player_s(44));
    lv_obj_set_pos(player_dismiss_btn, player_x(22), player_y(62));
    /* The reference keeps the status bar unobstructed and has no visible
     * back arrow on the Player. Retain the transparent hit target so the
     * established tap-to-dismiss behavior remains available. */
    lv_obj_t * dismiss_arrow = lv_obj_get_child(player_dismiss_btn, 0);
    if (dismiss_arrow) lv_obj_add_flag(dismiss_arrow, LV_OBJ_FLAG_HIDDEN);

    /* Quality pill */
    quality_pill = lv_obj_create(scr);
    lv_obj_remove_style_all(quality_pill);
    /* This label is intentionally not marquee-animated: the pill grows to
     * the complete, short format string and therefore never clips codec,
     * bit-depth, or sample-rate information. */
    lv_obj_set_size(quality_pill, LV_SIZE_CONTENT, player_s(36));
    lv_obj_align(quality_pill, LV_ALIGN_TOP_MID, 0, player_y(518));
    lv_obj_set_style_pad_hor(quality_pill, player_s(14), 0);
    lv_obj_set_style_pad_ver(quality_pill, player_s(6), 0);
    lv_obj_set_style_pad_column(quality_pill, player_s(10), 0);
    lv_obj_set_flex_flow(quality_pill, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(quality_pill, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_radius(quality_pill, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(quality_pill, LV_OPA_20, 0);
    lv_obj_set_style_bg_color(quality_pill, lv_color_hex(0x000000), 0);
    lv_obj_remove_flag(quality_pill, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * quality_waveform = lv_image_create(quality_pill);
    lv_image_set_src(quality_waveform, asset_path("playing_plane/quality_waveform.png"));
    lv_image_set_scale(quality_waveform, (player_s(20) * LV_SCALE_NONE) / 20);
    lv_obj_set_size(quality_waveform, player_s(20), player_s(20));
    lv_obj_add_style(quality_waveform, gui_theme_accent_style(), 0);
    lv_obj_set_style_bg_opa(quality_waveform, LV_OPA_TRANSP, 0);
    lv_obj_set_style_image_recolor_opa(quality_waveform, LV_OPA_COVER, 0);

    format_badge_label = lv_label_create(quality_pill);
    lv_label_set_text(format_badge_label, "");
    lv_obj_add_style(format_badge_label, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(format_badge_label, &app_font_16, 0);
    lv_obj_set_width(format_badge_label, LV_SIZE_CONTENT);

    /* Progress bar (native rail, not the old fixed PNG sprites) */
    progress_slider = lv_slider_create(scr);
    lv_obj_set_pos(progress_slider, player_x(30), player_y(575));
    lv_obj_set_size(progress_slider, player_x(420), SLIDER_TRACK_HEIGHT);
    lv_slider_set_range(progress_slider, 0, 100);
    lv_slider_set_value(progress_slider, 0, LV_ANIM_OFF);
    configure_native_slider_rail(progress_slider);
    lv_obj_add_style(progress_slider, gui_theme_accent_style(), LV_PART_INDICATOR);
    lv_obj_add_style(progress_slider, gui_theme_accent_knob_style(), LV_PART_KNOB);
    lv_obj_set_style_width(progress_slider, SLIDER_KNOB_SIZE, LV_PART_KNOB);
    lv_obj_set_style_height(progress_slider, SLIDER_KNOB_SIZE, LV_PART_KNOB);
    lv_obj_set_ext_click_area(progress_slider, player_y(20));
    lv_obj_add_event_cb(progress_slider, progress_slider_event_cb, LV_EVENT_ALL, NULL);
    register_swipe_dead_zone(progress_slider);

    /* Time row */
    lv_obj_t * time_row = lv_obj_create(scr);
    lv_obj_set_pos(time_row, player_x(30), player_y(610));
    lv_obj_set_size(time_row, player_x(420), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(time_row, 0, 0);
    lv_obj_set_style_border_width(time_row, 0, 0);
    lv_obj_set_style_pad_all(time_row, 0, 0);
    lv_obj_remove_flag(time_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(time_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(time_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    pos_label = lv_label_create(time_row);
    lv_label_set_text(pos_label, "0:00");
    lv_obj_add_style(pos_label, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(pos_label, player_meta_font, 0);

    dur_label = lv_label_create(time_row);
    lv_label_set_text(dur_label, "0:00");
    lv_obj_add_style(dur_label, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(dur_label, player_meta_font, 0);

    /* Current position in the active queue, overlaid on the cover. */
    song_count_label = lv_label_create(cover_card);
    lv_label_set_text(song_count_label, "--/--");
    lv_obj_add_style(song_count_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(song_count_label, &app_font_16, 0);
    lv_obj_set_style_bg_color(song_count_label, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(song_count_label, LV_OPA_50, 0);
    lv_obj_set_style_radius(song_count_label, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_hor(song_count_label, player_s(9), 0);
    lv_obj_set_style_pad_ver(song_count_label, player_s(5), 0);
    lv_obj_set_pos(song_count_label, player_s(10), player_s(10));
    lv_obj_remove_flag(song_count_label, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(song_count_label, LV_OBJ_FLAG_HIDDEN);

    /* Transport row: order, previous, play/pause, next, more. Cover taps are
     * the sole lyrics actuator, leaving the final slot for song actions. */
    lv_obj_t * controls_row = lv_obj_create(scr);
    lv_obj_remove_style_all(controls_row);
    const int32_t transport_top = player_y(645);
    lv_obj_set_pos(controls_row, 0, transport_top);
    lv_obj_set_size(controls_row, BOARD_SCREEN_WIDTH, player_y(94));
    lv_obj_remove_flag(controls_row, LV_OBJ_FLAG_SCROLLABLE);

    order_icon = lv_image_create(controls_row);
    lv_image_set_src(order_icon, asset_path(play_mode_icon_asset((play_mode_t) current_settings.play_mode)));
    lv_obj_add_style(order_icon, &icon_press_style, LV_STATE_PRESSED);

    prev_btn = lv_image_create(controls_row);
    lv_image_set_src(prev_btn, asset_path("playing_plane/btn_prev.png"));
    transport_btn_ctx_t * prev_ctx = malloc(sizeof(transport_btn_ctx_t));
    if (!prev_ctx) return NULL;
    *prev_ctx = (transport_btn_ctx_t){ prev_btn, "playing_plane/btn_prev.png", "playing_plane/btn_prev_s.png" };

    /* btn_play.png/btn_pause.png already contain their own filled accent
     * circle -- no separate outline ring needed here, unlike favorite_circle/
     * quality_pill which wrap plain glyphs/text with no ring of their own. */
    play_btn = lv_image_create(controls_row);
    load_play_btn_images();
    lv_image_set_src(play_btn, gui_player_play_btn_image_src(audio_is_playing()));
    lv_obj_add_style(play_btn, &icon_press_style, LV_STATE_PRESSED);

    lv_obj_t * more_icon = lv_image_create(controls_row);
    lv_image_set_src(more_icon, asset_path("playing_plane/ic_more.png"));
    lv_obj_add_style(more_icon, &icon_press_style, LV_STATE_PRESSED);

    next_btn = lv_image_create(controls_row);
    lv_image_set_src(next_btn, asset_path("playing_plane/btn_next.png"));
    transport_btn_ctx_t * next_ctx = malloc(sizeof(transport_btn_ctx_t));
    if (!next_ctx) {
        free(prev_ctx);
        return NULL;
    }
    *next_ctx = (transport_btn_ctx_t){ next_btn, "playing_plane/btn_next.png", "playing_plane/btn_next_s.png" };

    lv_obj_update_layout(controls_row);
    int32_t row_h = lv_obj_get_height(controls_row);
    const int32_t transport_center_y = transport_top + row_h / 2;
    lv_obj_set_pos(play_btn, player_x(240) - lv_obj_get_width(play_btn) / 2, (row_h - lv_obj_get_height(play_btn)) / 2);
    lv_obj_set_pos(order_icon, player_x(57) - lv_obj_get_width(order_icon) / 2, (row_h - lv_obj_get_height(order_icon)) / 2);
    lv_obj_set_pos(prev_btn, player_x(142) - lv_obj_get_width(prev_btn) / 2, (row_h - lv_obj_get_height(prev_btn)) / 2);
    lv_obj_set_pos(more_icon, player_x(423) - lv_obj_get_width(more_icon) / 2, (row_h - lv_obj_get_height(more_icon)) / 2);
    lv_obj_set_pos(next_btn, player_x(338) - lv_obj_get_width(next_btn) / 2, (row_h - lv_obj_get_height(next_btn)) / 2);

    /* Part D: Hit targets */
    lv_obj_update_layout(scr);

    int32_t order_hit_w = player_s(56); if (order_hit_w < 44) order_hit_w = 44;
    int32_t order_hit_h = player_s(56); if (order_hit_h < 44) order_hit_h = 44;
    lv_obj_t * order_hit = add_transport_hit_target(scr, player_x(57), order_hit_w,
                                transport_center_y - order_hit_h / 2, transport_center_y + (order_hit_h + 1) / 2, order_icon_event_cb,
                                lv_palette_main(LV_PALETTE_RED));
    lv_obj_add_event_cb(order_hit, forward_press_state_to_icon_cb, LV_EVENT_PRESSED, order_icon);
    lv_obj_add_event_cb(order_hit, forward_press_state_to_icon_cb, LV_EVENT_RELEASED, order_icon);
    lv_obj_add_event_cb(order_hit, forward_press_state_to_icon_cb, LV_EVENT_PRESS_LOST, order_icon);

    int32_t prev_hit_w = player_s(56); if (prev_hit_w < 44) prev_hit_w = 44;
    int32_t prev_hit_h = player_s(56); if (prev_hit_h < 44) prev_hit_h = 44;
    lv_obj_t * prev_hit = add_transport_hit_target(scr, player_x(142), prev_hit_w,
                                transport_center_y - prev_hit_h / 2, transport_center_y + (prev_hit_h + 1) / 2, prev_btn_event_cb,
                                lv_palette_main(LV_PALETTE_GREEN));
    lv_obj_add_event_cb(prev_hit, transport_btn_press_event_cb, LV_EVENT_PRESSED, prev_ctx);
    lv_obj_add_event_cb(prev_hit, transport_btn_press_event_cb, LV_EVENT_RELEASED, prev_ctx);
    lv_obj_add_event_cb(prev_hit, transport_btn_press_event_cb, LV_EVENT_PRESS_LOST, prev_ctx);
    lv_obj_add_event_cb(prev_hit, transport_btn_ctx_delete_cb, LV_EVENT_DELETE, prev_ctx);
    lv_obj_add_event_cb(prev_hit, transport_long_press_cb, LV_EVENT_LONG_PRESSED, &prev_btn_long_press_fired);
    lv_obj_add_event_cb(prev_hit, transport_long_press_cancel_cb, LV_EVENT_PRESS_LOST, &prev_btn_long_press_fired);
    lv_obj_add_event_cb(prev_hit, transport_seek_repeat_cb, LV_EVENT_LONG_PRESSED, (void *) (intptr_t) -1);
    lv_obj_add_event_cb(prev_hit, transport_seek_repeat_cb, LV_EVENT_LONG_PRESSED_REPEAT, (void *) (intptr_t) -1);

    int32_t play_hit_w = player_s(94); if (play_hit_w < 44) play_hit_w = 44;
    int32_t play_hit_h = player_s(94); if (play_hit_h < 44) play_hit_h = 44;
    lv_obj_t * play_hit = add_transport_hit_target(scr, player_x(240), play_hit_w,
                                transport_center_y - play_hit_h / 2, transport_center_y + (play_hit_h + 1) / 2, play_btn_event_cb,
                                lv_palette_main(LV_PALETTE_BLUE));
    lv_obj_add_event_cb(play_hit, forward_press_state_to_icon_cb, LV_EVENT_PRESSED, play_btn);
    lv_obj_add_event_cb(play_hit, forward_press_state_to_icon_cb, LV_EVENT_RELEASED, play_btn);
    lv_obj_add_event_cb(play_hit, forward_press_state_to_icon_cb, LV_EVENT_PRESS_LOST, play_btn);

    int32_t next_hit_w = player_s(56); if (next_hit_w < 44) next_hit_w = 44;
    int32_t next_hit_h = player_s(56); if (next_hit_h < 44) next_hit_h = 44;
    lv_obj_t * next_hit = add_transport_hit_target(scr, player_x(338), next_hit_w,
                                transport_center_y - next_hit_h / 2, transport_center_y + (next_hit_h + 1) / 2, next_btn_event_cb,
                                lv_palette_main(LV_PALETTE_ORANGE));
    lv_obj_add_event_cb(next_hit, transport_btn_press_event_cb, LV_EVENT_PRESSED, next_ctx);
    lv_obj_add_event_cb(next_hit, transport_btn_press_event_cb, LV_EVENT_RELEASED, next_ctx);
    lv_obj_add_event_cb(next_hit, transport_btn_press_event_cb, LV_EVENT_PRESS_LOST, next_ctx);
    lv_obj_add_event_cb(next_hit, transport_btn_ctx_delete_cb, LV_EVENT_DELETE, next_ctx);
    lv_obj_add_event_cb(next_hit, transport_long_press_cb, LV_EVENT_LONG_PRESSED, &next_btn_long_press_fired);
    lv_obj_add_event_cb(next_hit, transport_long_press_cancel_cb, LV_EVENT_PRESS_LOST, &next_btn_long_press_fired);
    lv_obj_add_event_cb(next_hit, transport_seek_repeat_cb, LV_EVENT_LONG_PRESSED, (void *) (intptr_t) 1);
    lv_obj_add_event_cb(next_hit, transport_seek_repeat_cb, LV_EVENT_LONG_PRESSED_REPEAT, (void *) (intptr_t) 1);

    int32_t more_hit_size = player_s(56); if (more_hit_size < 44) more_hit_size = 44;
    lv_obj_t * more_hit = add_transport_hit_target(scr, player_x(423), more_hit_size,
                                transport_center_y - more_hit_size / 2,
                                transport_center_y + (more_hit_size + 1) / 2,
                                more_icon_event_cb,
                                lv_palette_main(LV_PALETTE_PURPLE));
    lv_obj_add_event_cb(more_hit, forward_press_state_to_icon_cb, LV_EVENT_PRESSED, more_icon);
    lv_obj_add_event_cb(more_hit, forward_press_state_to_icon_cb, LV_EVENT_RELEASED, more_icon);
    lv_obj_add_event_cb(more_hit, forward_press_state_to_icon_cb, LV_EVENT_PRESS_LOST, more_icon);

#ifdef UI_GESTURE_TRACE
    lv_obj_add_event_cb(order_hit, debug_transport_btn_all_cb, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(play_hit, debug_transport_btn_all_cb, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(prev_hit, debug_transport_btn_all_cb, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(next_hit, debug_transport_btn_all_cb, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(more_hit, debug_transport_btn_all_cb, LV_EVENT_ALL, NULL);
#endif

    lv_area_t favorite_area;
    lv_obj_get_coords(favorite_circle, &favorite_area);
    int32_t fav_w = lv_area_get_width(&favorite_area);
    int32_t fav_h = lv_area_get_height(&favorite_area);
    int32_t fav_hit_w = fav_w < 44 ? 44 : fav_w;
    int32_t fav_hit_h = fav_h < 44 ? 44 : fav_h;
    lv_obj_t * favorite_hit = lv_obj_create(scr);
    lv_obj_remove_style_all(favorite_hit);
    lv_obj_set_pos(favorite_hit,
                   favorite_area.x1 - (fav_hit_w - fav_w) / 2,
                   favorite_area.y1 - (fav_hit_h - fav_h) / 2);
    lv_obj_set_size(favorite_hit, fav_hit_w, fav_hit_h);
    lv_obj_add_flag(favorite_hit, LV_OBJ_FLAG_IGNORE_LAYOUT | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(favorite_hit, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(favorite_hit, favorite_icon_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(favorite_hit, forward_press_state_to_icon_cb, LV_EVENT_PRESSED, favorite_icon);
    lv_obj_add_event_cb(favorite_hit, forward_press_state_to_icon_cb, LV_EVENT_RELEASED, favorite_icon);
    lv_obj_add_event_cb(favorite_hit, forward_press_state_to_icon_cb, LV_EVENT_PRESS_LOST, favorite_icon);
#ifdef UI_HITBOX_DEBUG
    lv_obj_set_style_border_width(favorite_hit, 3, 0);
    lv_obj_set_style_border_color(favorite_hit, lv_palette_main(LV_PALETTE_PINK), 0);
    lv_obj_set_style_border_opa(favorite_hit, LV_OPA_COVER, 0);
#endif

    volume_slider = lv_slider_create(scr);
    lv_obj_add_flag(volume_slider, LV_OBJ_FLAG_HIDDEN);
    lv_slider_set_range(volume_slider, 0, 100);
    lv_slider_set_value(volume_slider, (int32_t) (audio_get_volume() * 100.0f), LV_ANIM_OFF);

    finalize_screen_navigation(scr);
    lv_obj_add_event_cb(scr, player_lyrics_screen_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
    fit_cover_img_to_card();
    return scr;
}




void show_volume_popup(int32_t percent) {
    if (!volume_popup || !volume_popup_track) return;
    /* A BT/remote/hw-button echo writing the slider under a live finger
     * snaps the knob back to the last applied value, then the next indev
     * sample jumps it forward again. */
    bool dragged = lv_slider_is_dragged(volume_popup_track);
    if (!dragged) {
        lv_slider_set_value(volume_popup_track, percent, LV_ANIM_OFF);
        if (volume_popup_value_label) lv_label_set_text_fmt(volume_popup_value_label, "%d", (int) percent);
    }
    lv_obj_remove_flag(volume_popup, LV_OBJ_FLAG_HIDDEN);
    if (volume_popup_hide_timer && !dragged) {
        lv_timer_reset(volume_popup_hide_timer);
        lv_timer_resume(volume_popup_hide_timer);
    }
}

void refresh_format_badge(void) {
    if (playlist_index < 0) {
        if (format_badge_label) lv_label_set_text(format_badge_label, "");
        gui_shell_update_quick_drawer_format("");
        return;
    }

    const char * path = playlist_path_at(playlist_index);

    remote_track_meta_t remote_meta;
    bool is_remote_track = remote_track_meta_copy_for_path(path, &remote_meta);
    char ext[16] = "";
    if (is_remote_track && remote_meta.codec[0]) {
        size_t i = 0;
        for (const char * p = remote_meta.codec; *p && i < sizeof(ext) - 1; p++, i++) {
            ext[i] = (char) toupper((unsigned char) *p);
        }
        ext[i] = '\0';
    } else if (!is_remote_track) {
        const char * dot = strrchr(path, '.');
        if (dot) {
            size_t i = 0;
            for (const char * p = dot + 1; *p && i < sizeof(ext) - 1; p++, i++) {
                ext[i] = (char) toupper((unsigned char) *p);
            }
            ext[i] = '\0';
        }
    }

    unsigned int sample_rate = is_remote_track ? remote_meta.sample_rate : 0;
    audio_current_format_info_t fmt_info;
    unsigned int bit_depth = is_remote_track ? remote_meta.bit_depth : 0;
    if (player_cached_format.valid && strcmp(player_cached_format.path, path) == 0) {
        sample_rate = player_cached_format.source_sample_rate;
        bit_depth = player_cached_format.source_bit_depth;
    }
    if (audio_get_current_format_info(&fmt_info) && fmt_info.valid &&
        strcmp(fmt_info.path, path) == 0) {
        sample_rate = fmt_info.source_sample_rate;
        bit_depth = fmt_info.source_bit_depth;
    }
    {
        char text[96];
        if (sample_rate > 0 && bit_depth > 0) {
            snprintf(text, sizeof(text), "%s %u-bit / %.1fkHz", ext, bit_depth, sample_rate / 1000.0);
        } else if (sample_rate > 0) {
            snprintf(text, sizeof(text), "%s  %.1fkHz", ext, sample_rate / 1000.0);
        } else if (bit_depth > 0) {
            snprintf(text, sizeof(text), "%s %u-bit", ext, bit_depth);
        } else {
            snprintf(text, sizeof(text), "%s", ext);
        }
        /* Polling unchanged text must not restart its marquee every tick. */
        if (format_badge_label && strcmp(lv_label_get_text(format_badge_label), text) != 0)
            lv_label_set_text(format_badge_label, text);
        gui_shell_update_quick_drawer_format(text);
    }
}





/* Path of whichever song is currently playing, or an empty string if
 * nothing is (or the current track isn't part of the local library, e.g.
 * an Airplay/DLNA source). Set once per real track-start in apply_track_
 * metadata_to_ui(), from that function's own local `path`. This is the
 * single source of truth every now-playing indicator (Artists/Albums/All
 * Songs/group-songs rows) reads from -- each resolves it to a display
 * position via its own DB query (metadata_db_get_group_offset()/
 * metadata_db_get_song_title_offset()/a direct path comparison), rather
 * than an in-memory array index -- see refresh_now_playing_indicators()
 * below. */
char now_playing_path[600] = "";

/* Where the current playlist came from. Deliberately NOT derived
 * from `playlist` itself: that's just a flat array of paths with no
 * memory of which screen/group built it. Each interactive play-launch
 * site (all_songs_row_click_cb, group_song_row_click_cb,
 * files_search_row_click_cb, on_file_browser_selected) calls the
 * matching set_player_source_*() helper right before on_file_selected();
 * the Subsonic-download and DLNA-cast play sites call
 * clear_player_source() instead, since a streamed/cast single track has
 * no on-device list to go back to. */
/* player_source_kind_t defined in gui.h */

player_source_kind_t player_source_kind = PLAYER_SOURCE_NONE;

int player_source_all_songs_index = -1; /* row index into all_songs_list -- the DB's own title-sorted order */
int player_source_recently_added_index = -1; /* row index into recently_added_list -- the DB's own first_seen-DESC order */

/* Own copy of the group's song entries at the moment playback started --
 * group_songs_entries/count/title_label themselves just describe
 * whichever group gui_library_get_group_songs_screen() CURRENTLY shows, which can change
 * (browsing to a different artist/album, or a library rescan) before the
 * playback continues. group_song_entry_t (gui.c further down) is
 * declared after this point in the file -- forward-declared here since
 * this struct only needs a pointer to it, not its layout. */
/* group_song_entry_t defined in gui_library.h */
char * player_source_group_title = NULL;
group_song_entry_t * player_source_group_entries = NULL;
int player_source_group_count = 0;
int player_source_group_pos = -1; /* row index within the group */

char player_source_file_browser_dir[PATH_MAX];
int player_source_file_browser_row = -1;
/* Set while the shared Group Songs screen represents an album.  The screen
 * is reused for artists, favorites and playlists too, so its title alone
 * cannot identify the source type for persistence. */

/* Resume-but-paused is a true deferred start.  Starting audio and then
 * immediately pausing races the output open on a headphone-less boot; an
 * ALSA failure can consume the queue before the pause lands. */
bool deferred_resume_pending = false;
double deferred_resume_position = 0.0;

/* Queue play mode -- cycled via the order/loop/single/random icon on the
 * player screen (order_icon_event_cb). Persisted as current_settings.play_mode
 * (plain int, see settings.h). Sequential is the only mode where reaching the
 * end of the queue actually stops playback instead of continuing somewhere. */
/* play_mode_t defined in gui.h */


/* Shuffle "bag": a permutation of 0..playlist_count-1 walked front-to-back
 * rather than picking a fresh random index every time, so every track plays
 * exactly once before any repeats -- a bare `rand() % count` on every
 * advance can (and eventually will) replay the same track twice in a row or
 * leave others unplayed for a long stretch, which reads as broken shuffle
 * rather than random. Regenerated (and reshuffled) whenever it's stale --
 * see ensure_shuffle_order_current(). */
static int * shuffle_order = NULL;
static int shuffle_order_count = 0; /* playlist_count this bag was generated for -- staleness check */
static int shuffle_pos = -1;        /* index into shuffle_order such that shuffle_order[shuffle_pos] == playlist_index */

/* Precomputed reshuffled continuation bag for the shuffle-wrap case.
 * Invalidated on playlist change by ensure_shuffle_order_current(). */
static int * pending_shuffle_order = NULL;
static uint64_t queue_revision = 1;




/* Shared style object driving the app-wide accent color (sliders, checked
 * switches, the selected EQ band) -- one lv_style_t whose bg_color gets
 * updated in place whenever the user picks a new color, so every widget
 * that has this style attached re-renders automatically without having to
 * walk and restyle each one individually. */
/* style_accent defined in gui_theme.c */
/* Plain light-gray text style for the *unselected* EQ band labels -- kept
 * as its own shared style (rather than a one-off lv_obj_set_style_text_color)
 * so toggling selection is just swapping which shared style is attached,
 * with no risk of a local per-object style override taking priority over
 * style_accent and silently defeating the highlight. */
/* accent colors and styles moved to gui_theme.c */

/* Generic back-stack, replacing the old pairwise hardcoded back targets
 * (settings always -> browser, eq always -> settings). Every screen's back
 * button and the left-to-right swipe gesture just call nav_pop(); forward
 * navigation calls nav_push(). Root (gui_shell_get_home_screen()) is seeded once in
 * gui_init() and is never popped past. */
#define NAV_STACK_MAX 16

/* Navigation stack and transitions moved to gui_navigation.c */

/* Persistent status bar (clock + battery/wifi icons), built once on LVGL's
 * top layer rather than duplicated into every build_XXX_screen() -- objects
 * on the top layer are drawn over whichever screen is currently active, on
 * every screen, automatically. Left non-clickable throughout so touches
 * still fall through to the real screen underneath (confirmed by reading
 * lv_indev.c's search order: layer_top is checked before the active screen,
 * but only objects with LV_OBJ_FLAG_CLICKABLE ever hit-test positive, so a
 * plain informational bar like this never intercepts anything). Battery
 * percentage (battery_get_percent()), wifi connection state
 * (wifi_get_status()), and the clock are all real, live data -- the
 * battery reads -1 on host (no /sys/class/power_supply there), in which
 * case the label is just left blank and only the plain icon shows; wifi
 * always reads disconnected on host too, since there's no wlan0
 * wpa_supplicant instance to query there, same honest "no data" treatment
 * either way. Battery percentage is rendered the same sprite-digit way as
 * the clock/volume readouts (see battery_topbar_group below), not an
 * lv_label, for the same size/style match. */
/* Status bar and Quick Drawer moved to gui_shell.c */


void reset_decoder_failure_tracking(void) {
    consecutive_decoder_failure_skips = 0;
    failed_physical_paths_count = 0;
}

static bool is_failed_physical_path(const char * path) {
    if (!path || !path[0]) return false;
    for (int i = 0; i < failed_physical_paths_count; i++) {
        if (strcmp(failed_physical_paths[i], path) == 0) return true;
    }
    return false;
}

static void record_failed_physical_path(const char * path) {
    if (!path || !path[0]) return;
    if (is_failed_physical_path(path)) return;
    if (failed_physical_paths_count < MAX_FAILED_PHYSICAL_PATHS) {
        size_t len = strlen(path);
        if (len < sizeof(failed_physical_paths[failed_physical_paths_count])) {
            memcpy(failed_physical_paths[failed_physical_paths_count], path, len + 1);
            failed_physical_paths_count++;
        }
    }
}

void free_playlist(void) {
    queue_revision++;
    queued_pending_count = 0;
    queue_next_insert_index = -1;
    free(shuffle_order); shuffle_order = NULL; shuffle_order_count = 0; shuffle_pos = -1;
    free(pending_shuffle_order); pending_shuffle_order = NULL;
    reset_decoder_failure_tracking();
    current_playback_generation = 0;
    for (int i = 0; i < playlist_count; i++) free(playlist[i]); /* free(NULL) (an unresolved lazy slot) is a safe no-op */
    free(playlist);
    playlist = NULL;
    playlist_count = 0;
    playlist_index = -1;
    if (queue_sd_dirfd >= 0) { close(queue_sd_dirfd); queue_sd_dirfd = -1; }
    free(playlist_lazy_sort_order);
    playlist_lazy_sort_order = NULL;
    if (playlist_lazy_snapshot) metadata_db_snapshot_close(playlist_lazy_snapshot);
    playlist_lazy_snapshot = NULL;
    if (playlist_lazy_file_index) file_browser_index_close(playlist_lazy_file_index);
    playlist_lazy_file_index = NULL;
}

static void queue_pin_sd_dir_if_needed(void) {
    if (queue_sd_dirfd >= 0) return;
    for (int i = 0; i < playlist_count; i++) {
        const char * p = playlist[i];
        if (p && is_sd_card_path(p)) {
            queue_sd_dirfd = open_sd_queue_dir();
            return;
        }
    }
}

/* Defined further down, with the rest of the lazy-All-Songs-queue
 * machinery -- forward-declared here since every reader of playlist[]'s
 * actual STRING CONTENT
 * between here and there (favorite toggle, gapless preload, play_track_at_
 * from, ...) must resolve through it rather than indexing playlist[]
 * directly, or a lazy All-Songs queue's unresolved NULL slots would crash
 * them. See playlist_lazy_sort_order's own comment for the full picture. */

const char * play_mode_icon_asset(play_mode_t mode) {
    switch (mode) {
        case PLAY_MODE_REPEAT_ALL: return "playing_plane/loop.png";
        case PLAY_MODE_REPEAT_ONE: return "playing_plane/single.png";
        case PLAY_MODE_SHUFFLE:    return "playing_plane/random.png";
        default:                   return "playing_plane/order.png";
    }
}

/* Fisher-Yates shuffle of a fresh 0..count-1 permutation. */
static void fisher_yates_shuffle(int * arr, int count) {
    for (int i = count - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        int tmp = arr[i];
        arr[i] = arr[j];
        arr[j] = tmp;
    }
}

/* (Re)generates shuffle_order if it's stale (playlist size changed, or it
 * was never generated) and points shuffle_pos at wherever the currently
 * playing track landed in the new order -- switching into shuffle mode, or
 * the playlist changing size mid-shuffle, should never itself jump away
 * from whatever's already playing. */
static void ensure_shuffle_order_current(void) {
    if (shuffle_order && shuffle_order_count == playlist_count) return;

    static bool rng_seeded = false;
    if (!rng_seeded) {
        srand((unsigned int) time(NULL));
        rng_seeded = true;
    }

    free(shuffle_order);
    shuffle_order = malloc(sizeof(int) * (size_t) playlist_count);
    if (!shuffle_order) { shuffle_order_count = 0; shuffle_pos = -1; return; }
    for (int i = 0; i < playlist_count; i++) shuffle_order[i] = i;
    fisher_yates_shuffle(shuffle_order, playlist_count);
    shuffle_order_count = playlist_count;

    /* A pending wrap-continuation order (see compute_auto_advance_index()'s
     * own comment) sized for whatever playlist_count was true when it was
     * generated is no longer valid once the playlist itself has changed --
     * without this, a stale pending_shuffle_order could later get promoted
     * by commit_auto_advance() and paired with the NEW (different)
     * playlist_count, a real out-of-bounds read the first time shuffle_pos
     * advances past the old, smaller array's real size. */
    free(pending_shuffle_order);
    pending_shuffle_order = NULL;

    shuffle_pos = 0;
    if (playlist_index >= 0) {
        for (int i = 0; i < playlist_count; i++) {
            if (shuffle_order[i] == playlist_index) {
                int first = shuffle_order[0];
                shuffle_order[0] = shuffle_order[i];
                shuffle_order[i] = first;
                shuffle_pos = 0;
                break;
            }
        }
    }
    /* Explicit additions must occupy the next slots in a fresh bag too. */
    for (int n = 0; n < queued_pending_count; n++) {
        int physical = playlist_index + 1 + n;
        int found = 0;
        while (found < playlist_count && shuffle_order[found] != physical) found++;
        if (found == playlist_count) continue;
        memmove(shuffle_order + found, shuffle_order + found + 1, (size_t) (playlist_count - found - 1) * sizeof(int));
        if (found <= shuffle_pos) shuffle_pos--;
        int at = shuffle_pos + 1 + n;
        memmove(shuffle_order + at + 1, shuffle_order + at, (size_t) (playlist_count - at - 1) * sizeof(int));
        shuffle_order[at] = physical;
    }
}

static void queue_shuffle_insert(int physical, int added) {
    int old_count = playlist_count - added;
    if (shuffle_order && shuffle_order_count == old_count) {
        int * grown = realloc(shuffle_order, (size_t) playlist_count * sizeof(int));
        if (grown) {
            shuffle_order = grown;
            int at = shuffle_pos + 1 + physical - playlist_index - 1;
            if (at < 0) at = 0;
            if (at > old_count) at = old_count;
            playback_order_insert(shuffle_order, old_count, physical, at, added);
            shuffle_order_count = playlist_count;
        } else { free(shuffle_order); shuffle_order = NULL; shuffle_order_count = 0; }
    }
    free(pending_shuffle_order); pending_shuffle_order = NULL;
}

static void queue_shuffle_remove(int physical) {
    playback_order_note_removed(shuffle_order, &shuffle_order_count, &shuffle_pos, physical, playlist_count);
    free(pending_shuffle_order); pending_shuffle_order = NULL;
}


/* What track to move to when the current one finishes naturally (auto-
 * advance) -- as opposed to compute_manual_step_index() below, for an
 * explicit Prev/Next tap. Returns -1 for "stop, end of queue" (only
 * possible in Sequential mode).
 *
 * Deliberately read-only (never mutates shuffle_pos/shuffle_order) --
 * arm_next_track_for_audio() calls this speculatively, well before the
 * transition it's asking about is actually confirmed (gapless preload/
 * crossfade prep), so it must be safe to call more than once for the same
 * `index` and always get the same answer. commit_auto_advance() below is
 * the only thing allowed to actually advance the shuffle state, called
 * exactly once at the point a transition is confirmed real. */
int compute_auto_advance_index(int index) {
    if (index < 0 || playlist_count <= 0) return -1;

    /* A queued song always plays next, ahead of play_mode entirely (Repeat
     * One included -- queueing something is an explicit override of
     * whatever's currently looping) -- see queued_pending_count's own
     * comment. */
    if (queued_pending_count > 0 && index + 1 < playlist_count) return index + 1;

    switch ((play_mode_t) current_settings.play_mode) {
        case PLAY_MODE_REPEAT_ONE:
            return index;
        case PLAY_MODE_REPEAT_ALL:
            return (index + 1) % playlist_count;
        case PLAY_MODE_SHUFFLE: {
            ensure_shuffle_order_current();
            if (!shuffle_order) return -1;
            int peek_pos = shuffle_pos + 1;
            if (peek_pos < playlist_count) return shuffle_order[peek_pos];

            /* Bag exhausted -- precompute the reshuffled continuation now
             * rather than waiting for commit_auto_advance(), so whatever
             * gets armed for gapless preload here and whatever that commit
             * later confirms are guaranteed to be the same track.
             *
             * Precomputed once per wrap so repeated calls for the same
             * transition return a consistent answer. commit_auto_advance()
             * consumes and clears pending_shuffle_order when the wrap is
             * confirmed. */
            if (!pending_shuffle_order) {
                pending_shuffle_order = malloc(sizeof(int) * (size_t) playlist_count);
                if (!pending_shuffle_order) return -1;
                memcpy(pending_shuffle_order, shuffle_order, sizeof(int) * (size_t) playlist_count);
                fisher_yates_shuffle(pending_shuffle_order, playlist_count);
            }
            return pending_shuffle_order[0];
        }
        case PLAY_MODE_SEQUENTIAL:
        default:
            return (index + 1 < playlist_count) ? index + 1 : -1;
    }
}

static bool is_failed_physical_track_cb(int index, void * userdata) {
    (void) userdata;
    const char * path = playlist_path_at(index);
    return is_failed_physical_path(path);
}

static void commit_decoder_failure_advance_plan(const failure_advance_plan_t * plan) {
    if (!plan || plan->target_index < 0) return;

    if (plan->queued_consumed > 0) {
        if (plan->queued_consumed >= queued_pending_count) {
            queued_pending_count = 0;
            queue_next_insert_index = -1;
        } else {
            queued_pending_count -= plan->queued_consumed;
        }
    }

    if ((play_mode_t) current_settings.play_mode == PLAY_MODE_SHUFFLE && plan->shuffle_steps > 0) {
        if (plan->shuffle_wrapped && pending_shuffle_order) {
            free(shuffle_order);
            shuffle_order = pending_shuffle_order;
            pending_shuffle_order = NULL;
            shuffle_order_count = playlist_count;
            shuffle_pos = (shuffle_pos + plan->shuffle_steps) % playlist_count;
        } else {
            shuffle_pos += plan->shuffle_steps;
            if (shuffle_pos >= playlist_count) shuffle_pos = playlist_count - 1;
        }
    }
}

/* Call exactly once, right when an auto-advance computed by
 * compute_auto_advance_index() actually happens (the playback thread really
 * moved on, or the hard-restart fallback is about to play that index after
 * a true end-of-playlist/failed-file) -- advances shuffle_pos, swapping in
 * the reshuffled bag precomputed above if a wrap happened. No-op outside
 * Shuffle mode, where compute_auto_advance_index() is already pure/stateless. */
void commit_auto_advance(void) {
    /* Matches compute_auto_advance_index()'s own queue-priority check --
     * a queue-jump never steps through the shuffle bag (shuffle_pos is
     * untouched), so it's handled here first and returns before any of the
     * shuffle bookkeeping below. */
    if (queued_pending_count > 0) {
        if (shuffle_order && shuffle_order_count == playlist_count) shuffle_pos++;
        queued_pending_count--;
        if (queued_pending_count == 0) queue_next_insert_index = -1;
        return;
    }

    if ((play_mode_t) current_settings.play_mode != PLAY_MODE_SHUFFLE) return;

    int peek_pos = shuffle_pos + 1;
    if (peek_pos < playlist_count) {
        shuffle_pos = peek_pos;
        return;
    }
    if (pending_shuffle_order) {
        free(shuffle_order);
        shuffle_order = pending_shuffle_order;
        pending_shuffle_order = NULL;
        shuffle_order_count = playlist_count;
        shuffle_pos = 0;
    }
}

/* What track an explicit Prev/Next button tap should move to. direction is
 * +1 (Next) or -1 (Prev). Repeat One doesn't affect manual skipping (only
 * auto-advance-at-end) -- a deliberate Next tap should never mean "restart
 * this same track". Returns -1 for "no-op, already at that edge". */
int compute_manual_step_index(int index, int direction) {
    if (index < 0 || playlist_count <= 0) return -1;
    if (direction < 0 && queued_pending_count > 0) {
        queued_pending_count = 0; queue_next_insert_index = -1;
    }

    /* Same queue-priority override as compute_auto_advance_index()/
     * commit_auto_advance() -- a manual Next tap should land on a queued
     * song too, not skip past it into whatever play_mode would otherwise
     * pick. Every caller of this function (direction=+1 case) is a real,
     * one-shot commit (touchscreen/hw button/BT remote/phone remote Next,
     * each behind its own edge-triggered "consume" flag) -- never a
     * speculative preview call -- so decrementing here is safe. */
    if (direction > 0 && queued_pending_count > 0 && index + 1 < playlist_count) {
        if (shuffle_order && shuffle_order_count == playlist_count) shuffle_pos++;
        queued_pending_count--;
        if (queued_pending_count == 0) queue_next_insert_index = -1;
        return index + 1;
    }

    if ((play_mode_t) current_settings.play_mode == PLAY_MODE_SHUFFLE) {
        ensure_shuffle_order_current();
        if (!shuffle_order) return -1;
        int new_pos = shuffle_pos + direction;
        if (new_pos < 0) return -1; /* no history to go back past */
        if (new_pos >= playlist_count) {
            fisher_yates_shuffle(shuffle_order, playlist_count);
            new_pos = 0;
        }
        shuffle_pos = new_pos;
        return shuffle_order[shuffle_pos];
    }

    if ((play_mode_t) current_settings.play_mode == PLAY_MODE_REPEAT_ALL) {
        return (index + direction + playlist_count) % playlist_count;
    }

    int next = index + direction;
    return (next >= 0 && next < playlist_count) ? next : -1;
}

/* Splits a full path into a display title (filename, no extension) and the
 * name of its containing folder. No tag/metadata parsing yet, so the
 * filename is the best "song title" available. */
void get_display_names(const char * path, char * title_out, size_t title_size,
                               char * folder_out, size_t folder_size) {
    if (!path) {
        if (title_out && title_size > 0) title_out[0] = '\0';
        if (folder_out && folder_size > 0) folder_out[0] = '\0';
        return;
    }
    const char * slash = strrchr(path, '/');
    const char * filename = slash ? slash + 1 : path;

    const char * dot = strrchr(filename, '.');
    size_t len = dot ? (size_t) (dot - filename) : strlen(filename);
    if (title_out && title_size > 0) {
        char * tmp = (char *) malloc(len + 1);
        if (tmp) {
            memcpy(tmp, filename, len);
            tmp[len] = '\0';
            utf8_truncate_safe(title_out, tmp, title_size);
            utf8_sanitize(title_out);
            free(tmp);
        } else {
            utf8_truncate_safe(title_out, filename, title_size);
            utf8_sanitize(title_out);
        }
    }

    if (folder_out && folder_size > 0) folder_out[0] = '\0';
    if (slash && folder_out && folder_size > 0) {
        char dir_path[PATH_MAX];
        size_t dir_len = (size_t) (slash - path);
        if (dir_len >= sizeof(dir_path)) dir_len = sizeof(dir_path) - 1;
        memcpy(dir_path, path, dir_len);
        dir_path[dir_len] = '\0';

        const char * folder_slash = strrchr(dir_path, '/');
        const char * folder_name = folder_slash ? folder_slash + 1 : dir_path;
        utf8_truncate_safe(folder_out, folder_name, folder_size);
        utf8_sanitize(folder_out);
    }
}



/* Tint the outer ring of the outline play/pause assets while preserving
 * their white center symbols. Also support the older stock assets:
 * stock btn_play.png / btn_pause.png are a white disc with a baked-in
 * #009FF6 play/pause mark. LVGL image_recolor mixes every pixel toward the
 * recolor color, so applying gui_theme_accent_style() would tint the disc
 * as well (same COVER flattening apply_accent_color() documents for
 * on.png). Rewrite only pixels that aren't white / near-white, keeping the
 * disc and the anti-aliased circle edge, and mixing the glyph toward the
 * current accent. */
static void recolor_play_btn_accent(lv_draw_buf_t * buf, lv_color_t accent)
{
    if (!buf || !buf->data || !buf->header.w || !buf->header.h ||
        buf->header.cf != LV_COLOR_FORMAT_ARGB8888) return;
    uint32_t w = buf->header.w;
    uint32_t h = buf->header.h;
    uint32_t stride = buf->header.stride;
    uint8_t ar = accent.red;
    uint8_t ag = accent.green;
    uint8_t ab = accent.blue;
    for (uint32_t y = 0; y < h; y++) {
        lv_color32_t * row = (lv_color32_t *) (buf->data + y * stride);
        for (uint32_t x = 0; x < w; x++) {
            if (row[x].alpha == 0) continue;
            /* Normalize to the image ellipse. The outer quarter of its
             * radius contains the ring, outside either center glyph.
             * Preserve alpha so antialiased edges remain smooth. */
            int32_t dx = ((int32_t) (2 * x + 1) - (int32_t) w) * 256 / (int32_t) w;
            int32_t dy = ((int32_t) (2 * y + 1) - (int32_t) h) * 256 / (int32_t) h;
            if (dx * dx + dy * dy >= 192 * 192) {
                row[x].red = ar;
                row[x].green = ag;
                row[x].blue = ab;
                continue;
            }
            uint8_t minc = row[x].red;
            if (row[x].green < minc) minc = row[x].green;
            if (row[x].blue < minc) minc = row[x].blue;
            uint8_t t = (uint8_t) (255 - minc);
            /* Disc interior is white / near-white; the glyph is the stock
             * cyan. Skip the disc so only the mark is retinted. */
            if (t < 16) continue;
            row[x].red   = (uint8_t) (255 - ((uint16_t) (255 - ar) * t) / 255);
            row[x].green = (uint8_t) (255 - ((uint16_t) (255 - ag) * t) / 255);
            row[x].blue  = (uint8_t) (255 - ((uint16_t) (255 - ab) * t) / 255);
        }
    }
}

static void load_play_btn_images(void)
{
    asset_decoded_image_close(&play_btn_play_img);
    asset_decoded_image_close(&play_btn_pause_img);
    lv_color_t accent = accent_lv_color();
    if (asset_decoded_image_open(&play_btn_play_img, "playing_plane/btn_play.png"))
        recolor_play_btn_accent((lv_draw_buf_t *) play_btn_play_img.decoder.decoded, accent);
    if (asset_decoded_image_open(&play_btn_pause_img, "playing_plane/btn_pause.png"))
        recolor_play_btn_accent((lv_draw_buf_t *) play_btn_pause_img.decoder.decoded, accent);
}

const void * gui_player_play_btn_image_src(bool is_playing)
{
    const void * src = asset_decoded_image_source(is_playing ? &play_btn_pause_img : &play_btn_play_img);
    if (src) return src;
    return asset_path(is_playing ? "playing_plane/btn_pause.png" : "playing_plane/btn_play.png");
}

void refresh_play_btn_icon(void)
{
    load_play_btn_images();
    bool is_playing = audio_is_playing();
    if (play_btn) lv_image_set_src(play_btn, gui_player_play_btn_image_src(is_playing));
    gui_shell_update_quick_drawer_play_state(is_playing);
    player_transition_mark_dirty(); /* play_btn lives on player_screen -- see the cache's own doc comment */
}

void set_play_button_state(bool is_playing) {
    if (play_btn) lv_image_set_src(play_btn, gui_player_play_btn_image_src(is_playing));
    player_transition_mark_dirty(); /* play_btn lives on player_screen -- see the cache's own doc comment */
    gui_shell_update_quick_drawer_play_state(is_playing);
}

/* resolve_replaygain moved to gui_player.c */

/* Tells audio.c what comes after `index` (per the current play mode -- see
 * compute_auto_advance_index()) so its playback thread can gapless-handoff
 * or crossfade into it near `index`'s natural end without a GUI round-trip.
 * Must be re-called (from on_track_auto_advanced) every time the thread
 * advances on its own, or the chain of automatic transitions stops after
 * one hop. */
void arm_next_track_for_audio(int index) {
    queue_revision++;
    if (current_settings.play_mode == PLAY_MODE_SHUFFLE) {
        ensure_shuffle_order_current();
        if (shuffle_order) {
            for (int i = 0; i < playlist_count; i++) if (shuffle_order[i] == index) { shuffle_pos = i; break; }
        }
    }
    /* Gapless off: arm nothing, exactly as if this were the end of the
     * queue. The playback thread then reaches a true EOF, and
     * gui_player_handle_track_finished() computes the same next index and
     * opens it fresh -- which reopens the output device, producing the gap
     * between tracks that turning this off is asking for. Auto-advance
     * itself is unaffected; only the seamless handoff is given up. */
    int next_index = compute_auto_advance_index(index);
    if (next_index < 0 || !current_settings.gapless_enabled) {
        audio_set_next_track(NULL, false, 0.0, false, 0.0);
        return;
    }
    const char * next_path = playlist_path_at(next_index);
    remote_track_meta_t next_remote_meta;
    bool next_is_remote_track = remote_track_meta_copy_for_path(next_path, &next_remote_meta);
    bool has_gain, has_peak;
    double gain_db, peak;
    if (next_is_remote_track) {
        /* metadata_read() can't open a synthetic "remote://" path -- same
         * gap this fixes in apply_track_metadata_to_ui() for the CURRENT
         * track, needed again here for the gapless-prefetched NEXT one
         * (on_track_auto_advanced()'s own comment: audio.c applies whatever
         * gain was armed here, not anything recomputed at handoff time). */
        has_gain = next_remote_meta.has_replaygain;
        gain_db = next_remote_meta.replaygain_db;
        has_peak = false;
        peak = 0.0;
    } else {
        track_metadata_t next_meta;
        metadata_read_without_artwork(next_path, &next_meta);
        resolve_replaygain(&next_meta, &has_gain, &gain_db, &has_peak, &peak);
        free(next_meta.picture_data); /* only needed the gain/peak fields, not the art or lyrics */
        free(next_meta.lyrics);
    }
    audio_set_next_track(next_path, has_gain, gain_db, has_peak, peak);
}

/* Song long-press context menu's "Add to Queue" -- splices `path` into the
 * live playlist right after whatever was queued last (or right after the
 * currently-playing track, if the queue's currently empty), so repeated
 * Add to Queue taps play back in the order they were added. See
 * queued_pending_count's own comment for why this reuses playlist[]
 * directly instead of a separate list. No-op with a toast if nothing's
 * playing -- there's no "currently playing track" position to queue
 * after. */
void queue_add_song(const char * path) {
    if (path && path[0]) gui_player_queue_add_many(&path, 1);
}

void gui_player_queue_add_many(const char * const * paths, int count) {
    if (audio_consume_track_advanced()) gui_player_handle_auto_advance();
    if (count <= 0) return;
    if (count > INT_MAX - playlist_count || count > INT_MAX - queued_pending_count) return;

    char ** copies = calloc((size_t) count, sizeof(*copies));
    if (!copies) return;
    for (int i = 0; i < count; i++) {
        copies[i] = paths[i] ? strdup(paths[i]) : NULL;
        if (!copies[i]) {
            for (int j = 0; j < count; j++) free(copies[j]);
            free(copies);
            return;
        }
    }

    if (!gui_player_has_active_track()) {
        free_playlist();
        clear_player_source();
        playlist = copies; playlist_count = count; playlist_index = 0;
        queued_pending_count = count - 1;
        queue_next_insert_index = count > 1 ? count : -1;
        track_metadata_t meta;
        apply_track_metadata_to_ui(0, &meta);
        set_play_button_state(false);
        deferred_resume_pending = true; deferred_resume_position = 0.0;
        snprintf(current_settings.last_track, sizeof(current_settings.last_track), "%s", playlist[0]);
        current_settings.last_position = 0;
        current_settings.last_source_kind = 0;
        current_settings.last_source_name[0] = '\0';
        settings_save_async(&current_settings);
        show_info_toast("Queue ready. Press Play to start.");
        return;
    }
    int pos = (queue_next_insert_index >= 0 && queue_next_insert_index <= playlist_count)
        ? queue_next_insert_index : playlist_index + 1;
    char ** grown = realloc(playlist, sizeof(*playlist) * (size_t) (playlist_count + count));
    if (!grown) {
        for (int i = 0; i < count; i++) free(copies[i]);
        free(copies);
        return;
    }
    playlist = grown;
    int * grown_order = playlist_lazy_sort_order
        ? realloc(playlist_lazy_sort_order, sizeof(*playlist_lazy_sort_order) * (size_t) (playlist_count + count))
        : NULL;
    if (playlist_lazy_sort_order && !grown_order) {
        for (int i = 0; i < count; i++) free(copies[i]);
        free(copies);
        return;
    }
    if (grown_order) playlist_lazy_sort_order = grown_order;

    memmove(&playlist[pos + count], &playlist[pos],
            sizeof(*playlist) * (size_t) (playlist_count - pos));
    if (playlist_lazy_sort_order) {
        memmove(&playlist_lazy_sort_order[pos + count], &playlist_lazy_sort_order[pos],
                sizeof(*playlist_lazy_sort_order) * (size_t) (playlist_count - pos));
    }
    for (int i = 0; i < count; i++) {
        playlist[pos + i] = copies[i];
        if (playlist_lazy_sort_order) playlist_lazy_sort_order[pos + i] = -1;
    }
    free(copies);
    playlist_count += count;
    queued_pending_count += count;
    queue_next_insert_index = pos + count;
    queue_shuffle_insert(pos, count);
    lv_label_set_text_fmt(song_count_label, "%d/%d", playlist_index + 1, playlist_count);
    arm_next_track_for_audio(playlist_index);

    char msg[64];
    snprintf(msg, sizeof(msg), "Added %d songs to queue", count);
    show_info_toast(msg);
}

void queue_remove_song_at_offset(int offset) {
    if (offset < 0 || offset >= queued_pending_count || playlist_index < 0) return;
    int pos = playlist_index + 1 + offset;
    queue_shuffle_remove(pos);
    free(playlist[pos]);
    memmove(&playlist[pos], &playlist[pos + 1], sizeof(char *) * (size_t) (playlist_count - pos - 1));
    if (playlist_lazy_sort_order) {
        memmove(&playlist_lazy_sort_order[pos], &playlist_lazy_sort_order[pos + 1],
                sizeof(int) * (size_t) (playlist_count - pos - 1));
    }
    playlist_count--;
    queued_pending_count--;
    queue_next_insert_index = queued_pending_count > 0 ? playlist_index + 1 + queued_pending_count : -1;
    lv_label_set_text_fmt(song_count_label, "%d/%d", playlist_index + 1, playlist_count);
    arm_next_track_for_audio(playlist_index);
    show_info_toast("Removed from queue");
}

void queue_clear_pending(void) {
    while (queued_pending_count > 0) queue_remove_song_at_offset(queued_pending_count - 1);
    show_info_toast("Queue cleared");
}

/* Bluetooth DAC mode and AirPlay receive mode both feed real-time audio
 * from another device straight into this device's own physical ALSA
 * hardware (hw:0,0) -- confirmed by checking what each one's consumer
 * process (aplay -D bluealsa; shairport -o ot) actually targets. Local
 * playback uses that same hardware directly via tinyalsa, so all three are
 * mutually exclusive: only one may be "using" the speaker/DAC output at a
 * time. bt_dac_toggle_cb()/airplay_toggle_cb() turn each other off when
 * either is enabled; this covers local playback's side of the same rule.
 *
 * bt_dac_mode_enabled is also gated on bt_is_powered_cached: it's a
 * persisted setting, so it can still read true from a previous session
 * after a reboot where Bluetooth was never turned back on (chip
 * re-init isn't automatic -- see TESTING.md). With Bluetooth actually off,
 * bt_control_apply_output_settings() never started bluealsa/aplay, so
 * nothing is really using the DAC output -- blocking playback in that case
 * was a real dead end: the Bluetooth DAC toggle to turn it back off only
 * appears once Bluetooth is powered, which needs a chip re-init the app
 * itself has no way to trigger. */
/* Checks whether Bluetooth DAC or USB DAC mode is actively blocking
 * local playback. AirPlay discoverability does not block local playback;
 * active streams are auto-disconnected instead. */
static const char * external_dac_block_reason(void) {
    if (current_settings.bt_dac_mode_enabled && bt_is_powered_cached) return "Turn off Bluetooth DAC to play music on this device";
    if (current_settings.usb_mode == USB_MODE_DAC) return "Exit USB DAC mode to play music on this device";
    return NULL;
}

/* Cached now-playing metadata for plugin.get_now_playing() -- populated at
 * the same two call sites that fire the "track_started" plugin event
 * (notify_plugin_track_started() below), so there's no separate tracking
 * needed. plugin_now_playing_loaded distinguishes "nothing has ever played
 * this session" from "a track is playing with an empty title" -- the title
 * buffer alone can't tell those apart. */
char plugin_now_playing_title[128];
char plugin_now_playing_artist[128];
char plugin_now_playing_album[128];
double plugin_now_playing_duration;
bool plugin_now_playing_loaded = false;

/* Shared by play_track_at_from()/on_track_auto_advanced() below -- caches
 * meta into the plugin_now_playing_* globals above and fires the
 * "track_started" plugin event. Called after audio_play_file_at()/the
 * gapless handoff has already happened in both call sites, so
 * audio_get_duration_seconds() reflects the new track. */
static void notify_plugin_track_started(const track_metadata_t * meta, const char * path) {
    snprintf(plugin_now_playing_title, sizeof(plugin_now_playing_title), "%s", meta->title);
    snprintf(plugin_now_playing_artist, sizeof(plugin_now_playing_artist), "%s", meta->artist);
    snprintf(plugin_now_playing_album, sizeof(plugin_now_playing_album), "%s", meta->album);
    plugin_now_playing_duration = audio_get_duration_seconds();
    plugin_now_playing_loaded = true;

    remote_track_meta_t remote_meta;
    bool is_remote_track = remote_track_meta_copy_for_path(path, &remote_meta);
    plugin_manager_notify_track_started(meta->title, meta->artist, meta->album, plugin_now_playing_duration,
                                         is_remote_track ? remote_meta.provider : "", is_remote_track ? remote_meta.track_id : "");
}

static void play_track_at_from_internal(int index, double start_seconds, bool push_nav, bool skip_airplay_disconnect) {
    if (index < 0 || index >= playlist_count) return;

    /* Symmetric with airplay_bridge.c's own side of this: an actual
     * incoming AirPlay stream starting stops local playback (airplay_
     * bridge_is_streaming() transitioning, see gui_network.c's gui_network_
     * poll_airplay_overlay()); starting local playback here while a stream
     * is already active should equally win and disconnect it, rather than
     * the two silently fighting over the shared output device or this call
     * failing/behaving oddly underneath a session it doesn't know about.
     * No-op when nothing is actively streaming. Must run before the block-
     * reason check below: that check no longer even considers AirPlay
     * (see external_dac_block_reason()'s own comment), but this call is
     * also what actually frees the output device before ensure_device()
     * further down tries to claim it.
     *
     * skip_airplay_disconnect exists solely for toggle_play_pause()'s own
     * resume-after-takeover path: it already called airplay_control_
     * disconnect_active_stream() itself to decide whether to resume at all,
     * and airplay_bridge_stop()/airplay_control_stop() are fire-and-forget
     * (see their own comments) -- is_streaming() can still read true for a
     * short window afterward. Calling disconnect a second time here could
     * see that stale true, and kill+respawn shairport a second time right
     * on top of the first respawn still settling. Every OTHER caller has
     * not already disconnected anything, so they must still run this call
     * normally. */
    if (!skip_airplay_disconnect) {
        airplay_control_disconnect_active_stream();
    }

    const char * block_reason = external_dac_block_reason();
    if (block_reason) {
        show_error_toast(block_reason);
        return;
    }

    cancel_pending_progress_seek();
    user_seeking = false;
    progress_awaiting_seek = false;
    deferred_resume_pending = false;
    deferred_resume_position = 0.0;
    playlist_index = index;

    track_metadata_t meta;
    apply_track_metadata_to_ui(index, &meta); /* resolves this slot if it's a still-lazy All Songs entry */
    const char * path = playlist_path_at(index);
    bool has_gain, has_peak;
    double gain_db, peak;
    resolve_replaygain(&meta, &has_gain, &gain_db, &has_peak, &peak);
    audio_play_file_at(path, start_seconds, has_gain, gain_db, has_peak, peak);
    current_playback_generation = audio_get_playback_generation();
    arm_next_track_for_audio(index);

#ifndef HOST_BUILD
    hiby_sys_server_report_metadata(meta.title, meta.artist, meta.album, meta.genre,
                                     (long) (audio_get_duration_seconds() * 1000.0));
    hiby_sys_server_report_position((long) (start_seconds * 1000.0));
#endif
    notify_plugin_track_started(&meta, path);

    set_play_button_state(true);
    /* Manual next/previous and a deferred startup resume can already be on
     * the player screen.  Do not stack a duplicate copy of the same screen
     * just because playback is being (re)started there. */
    if (push_nav && lv_screen_active() != player_screen) nav_push(player_screen);

    /* A remote track's stream_url can expire or be single-use -- resuming
     * into it blind on next launch can't work the way resuming a local
     * file (or even a Subsonic stream, which at least re-derives its own
     * salted URL from a stable server+song id) can. Leave last_track
     * untouched rather than saving a synthetic key that build_saved_
     * resume_playlist() has no way to turn back into a playable URL --
     * same "don't build expiring-URL-aware resume in this pass" scope
     * call as everywhere else remote tracks touch existing machinery. */
    if (path[0] && !remote_track_path_is_remote(path)) {
        snprintf(current_settings.last_track, sizeof(current_settings.last_track), "%s", path);
        current_settings.last_position = start_seconds;
    }
    settings_save_async(&current_settings);
}

void play_track_at_from(int index, double start_seconds) {
    reset_decoder_failure_tracking();
    play_track_at_from_internal(index, start_seconds, true, false);
}

/* Called from update_timer_cb when audio_consume_track_advanced() reports
 * the playback thread moved on to the queued next track by itself (gapless
 * handoff or a completed crossfade) -- audio is already playing it, so
 * unlike play_track_at_from() this must NOT call audio_play_file_at()
 * (that would hard-restart audio that's already mid-track). */
void on_track_auto_advanced(int index) {
    if (index < 0 || index >= playlist_count) return;

    cancel_pending_progress_seek();
    user_seeking = false;
    progress_awaiting_seek = false;
    playlist_index = index;

    track_metadata_t meta;
    apply_track_metadata_to_ui(index, &meta); /* audio.c already applied this track's ReplayGain during the handoff */
    current_playback_generation = audio_get_playback_generation();
    arm_next_track_for_audio(index);

#ifndef HOST_BUILD
    hiby_sys_server_report_metadata(meta.title, meta.artist, meta.album, meta.genre,
                                     (long) (audio_get_duration_seconds() * 1000.0));
    hiby_sys_server_report_position(0);
#endif
    notify_plugin_track_started(&meta, playlist_path_at(index));

    set_play_button_state(true);

    /* See play_track_at_from()'s own comment on why a remote track's
     * synthetic key is never saved as last_track. */
    const char *advanced_path = playlist_path_at(index);
    if (advanced_path[0] && !remote_track_path_is_remote(advanced_path)) {
        snprintf(current_settings.last_track, sizeof(current_settings.last_track), "%s", advanced_path);
        current_settings.last_position = 0.0;
    }
    settings_save_async(&current_settings);
}

void play_track_at(int index) {
    reset_decoder_failure_tracking();
    play_track_at_from_internal(index, 0.0, true, false);
}

void on_file_selected(char ** new_playlist, int count, int selected_index) {
    free_playlist();
    playlist = new_playlist;
    playlist_count = count;
    queue_pin_sd_dir_if_needed();
    /* A brand new playback context (a fresh song tapped from any list)
     * invalidates whatever was queued against the OLD playlist[] -- those
     * array positions no longer mean anything once the array itself has
     * been replaced. Matches every other music app: starting something new
     * clears "Up Next". */
    queued_pending_count = 0;
    queue_next_insert_index = -1;
    play_track_at(selected_index);
}

/* Same as on_file_selected() above, but for a tap that needs to start
 * partway into the track rather than at 0:00 -- currently only CUE track
 * playback (see cue_track_row_click_cb()): each of new_playlist's entries
 * is the SAME physical audio file repeated once per CUE track, and
 * start_seconds is the tapped track's own INDEX 01 offset within it. */
void on_file_selected_at(char ** new_playlist, int count, int selected_index, double start_seconds) {
    free_playlist();
    playlist = new_playlist;
    playlist_count = count;
    queue_pin_sd_dir_if_needed();
    queued_pending_count = 0;
    queue_next_insert_index = -1;
    play_track_at_from(selected_index, start_seconds);
}

/* set_player_source_group_songs() is defined further down, right after
 * group_songs_entries/count/title_label -- it needs those already in
 * scope. These three don't. */
void clear_player_source(void) {
    player_source_kind = PLAYER_SOURCE_NONE;
    player_source_all_songs_index = -1;
    player_source_recently_added_index = -1;
    free(player_source_group_title);
    player_source_group_title = NULL;
    free_group_song_entries(player_source_group_entries, player_source_group_count);
    player_source_group_entries = NULL;
    player_source_group_count = 0;
    player_source_group_pos = -1;
    player_source_file_browser_row = -1;
}

void set_player_source_all_songs(int display_index) {
    clear_player_source();
    player_source_kind = PLAYER_SOURCE_ALL_SONGS;
    player_source_all_songs_index = display_index;
    current_settings.last_source_kind = 1;
    current_settings.last_source_name[0] = '\0';
}

void set_player_source_recently_added(int display_index) {
    clear_player_source();
    player_source_kind = PLAYER_SOURCE_RECENTLY_ADDED;
    player_source_recently_added_index = display_index;
    /* No dedicated resume kind for this source -- falls back to "unknown",
     * same as Favorites/Most Played/user playlists (see group_song_row_
     * click_cb's own last_source_kind assignment); only All Songs and Album
     * get a real boot-resume slot (settings.h's own last_source_kind
     * comment). */
    current_settings.last_source_kind = 0;
    current_settings.last_source_name[0] = '\0';
}


void set_player_source_group_songs_direct(const group_song_entry_t * entries, int count, const char * title, int selected_index) {
    clear_player_source();
    player_source_kind = PLAYER_SOURCE_GROUP_SONGS;
    copy_group_song_entries(&player_source_group_entries, entries, count);
    player_source_group_count = count;
    player_source_group_pos = selected_index;
    if (title) player_source_group_title = strdup(title);
    current_settings.last_source_kind = 2;
    snprintf(current_settings.last_source_name, sizeof(current_settings.last_source_name), "%s", title ? title : "");
}

void set_player_source_file_browser(const char * dir, int row) {
    clear_player_source();
    player_source_kind = PLAYER_SOURCE_FILE_BROWSER;
    snprintf(player_source_file_browser_dir, sizeof(player_source_file_browser_dir), "%s", dir);
    player_source_file_browser_row = row;
    current_settings.last_source_kind = 0;
    current_settings.last_source_name[0] = '\0';
}

/* Wraps on_file_selected() as file_browser_init()'s select_cb, rather than
 * passing on_file_selected directly, so the source snapshot above only
 * ever gets set for an actual folder-browse tap -- on_file_selected()
 * itself is shared by every play-launch path (All Songs, group songs,
 * Subsonic downloads, DLNA casts, ...) and has no way to tell which of
 * them is calling it. file_browser_get_last_selected_dir()/_row() are
 * only valid synchronously within file_browser.c's own select_cb() call,
 * which is exactly where this reads them. */
void on_file_browser_selected(char ** new_playlist, int count, int selected_index) {
    set_player_source_file_browser(file_browser_get_last_selected_dir(), file_browser_get_last_selected_row());
    on_file_selected(new_playlist, count, selected_index);
}

void on_file_browser_index_selected(file_browser_index_t *index, unsigned playable_count,
                                    unsigned selected_playable) {
    if (!index || !playable_count || selected_playable >= playable_count) {
        if (index) file_browser_index_close(index);
        return;
    }
    char ** slots = calloc(playable_count, sizeof(*slots));
    int * order = malloc((size_t) playable_count * sizeof(*order));
    if (!slots || !order) {
        free(slots); free(order); file_browser_index_close(index); return;
    }
    int pinned_queue_dirfd = open_queue_dir_from_file_index(index);
    free_playlist();
    playlist = slots; playlist_count = (int) playable_count;
    playlist_lazy_sort_order = order; playlist_lazy_order_is_recency = false;
    playlist_lazy_file_index = index;
    for (unsigned i = 0; i < playable_count; i++) order[i] = (int) i;
    queue_sd_dirfd = pinned_queue_dirfd;
    queued_pending_count = 0; queue_next_insert_index = -1;
    set_player_source_file_browser(file_browser_get_last_selected_dir(), file_browser_get_last_selected_row());
    play_track_at((int) selected_playable);
}

void toggle_play_pause(void) {
    reset_decoder_failure_tracking();
    if (playlist_index < 0) return; /* nothing loaded yet */
    /* Same reasoning as play_track_at_from_internal()'s own call -- local
     * playback (resuming here) should win over an active AirPlay stream,
     * not silently fight it or no-op underneath a session it doesn't know
     * about. No-op when nothing is actively streaming. */
    if (airplay_control_disconnect_active_stream()) {
        /* AirPlay's own audio_stop() already tore down have_current back
         * when the stream first took over the output device (see airplay_
         * bridge.c's run_session()) -- audio_toggle_pause() further below
         * checks !have_current and would be a guaranteed no-op here, doing
         * nothing while set_play_button_state(false) claims it did.
         * Resume directly from the pending-aware checkpoint rather than
         * falling through to a no-op pause toggle.
         *
         * Calls play_track_at_from_internal() directly (skip_airplay_
         * disconnect=true) rather than the public play_track_at_from() --
         * that would call airplay_control_disconnect_active_stream() a
         * SECOND time. airplay_control_stop()/airplay_bridge_stop() are
         * fire-and-forget (see their own comments), so is_streaming() can
         * still read true for a short window after the call just above
         * returned; a second disconnect seeing that stale true would kill
         * and respawn shairport again right on top of the first respawn
         * still settling. reset_decoder_failure_tracking() was already
         * called at the top of this function, so skip play_track_at_from()'s
         * own redundant call to it too. */
        play_track_at_from_internal(playlist_index, audio_get_resume_position_seconds(), true, true);
        return;
    }
    if (deferred_resume_pending) {
        double start_seconds = deferred_resume_position;
        deferred_resume_pending = false;
        deferred_resume_position = 0.0;
        play_track_at_from(playlist_index, start_seconds);
        return;
    }
    /* Same DAC-mode exclusion as play_track_at_from() -- only blocks
     * resuming (paused -> playing), pausing an already-playing track always
     * goes through (though bt_dac_toggle_cb()/airplay_toggle_cb() already
     * stop playback the moment either DAC mode turns on, so in practice
     * there's nothing left playing to pause by the time this could
     * matter). */
    if (!audio_is_playing()) {
        const char * block_reason = external_dac_block_reason();
        if (block_reason) {
            show_error_toast(block_reason);
            return;
        }
    }
    audio_toggle_pause();
    bool now_playing = audio_is_playing();
    set_play_button_state(now_playing);

    if (now_playing) {
        plugin_manager_notify_resumed();
    } else {
        /* Checkpoint the resume position on pause -- a natural point to
         * persist, and far less write-heavy than saving on every tick.
         * Also urgently (re)writes the queue-checkpoint file (the file
         * startup restore actually reads for SD/internal tracks, separate
         * from settings.txt's own last_position field), so a pause
         * immediately followed by any kind of reboot does not resume from
         * a stale pre-pause position. Keep this settings save synchronous:
         * the urgent checkpoint is part of the same reboot-sensitive pause
         * persistence boundary. */
        current_settings.last_position = audio_get_resume_position_seconds();
        settings_save(&current_settings);
        gui_player_queue_checkpoint_urgent();
        plugin_manager_notify_paused();
    }
}

void play_btn_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    toggle_play_pause();
}

/* Standard CD-player/iPod convention: a tap partway into a track restarts
 * it; only a tap already near the start (first press rewinds, second press
 * goes to previous song) moves to the actual previous track -- no separate
 * double-tap timer needed, since "already near the start" is naturally true
 * right after the first tap rewound it. */
#define PREV_BUTTON_REWIND_THRESHOLD_SECONDS 3.0

void prev_btn_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    /* See transport_long_press_cb()'s own comment -- LV_EVENT_CLICKED still
     * fires on release even after a long press, so a hold-to-rewind session
     * would otherwise ALSO restart/skip to the previous track right after. */
    if (prev_btn_long_press_fired) {
        prev_btn_long_press_fired = false;
        return;
    }
    reset_decoder_failure_tracking();
    if (playlist_index < 0) return;

    if (audio_get_position_seconds() > PREV_BUTTON_REWIND_THRESHOLD_SECONDS) {
        audio_seek(0.0);
        return;
    }

    gui_player_step_manual(-1);
}

void next_btn_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (next_btn_long_press_fired) {
        next_btn_long_press_fired = false;
        return;
    }
    reset_decoder_failure_tracking();
    if (playlist_index < 0) return;
    gui_player_step_manual(1);
}



/* The one place crossfade is applied, for both entry points (the Settings
 * switch below and the quick drawer's own tile) -- the gapless implication
 * here is easy to apply in one and forget in the other.
 *
 * Crossfade blends into an ARMED next track, so it cannot engage at all
 * while gapless is off, because then nothing gets armed (see
 * arm_next_track_for_audio()). Enabling crossfade therefore switches
 * gapless back on rather than leaving a control that reads On and silently
 * does nothing. */
void gui_player_set_crossfade_enabled(bool enabled) {
    current_settings.crossfade_enabled = enabled;
    if (enabled) current_settings.gapless_enabled = true;
    audio_set_crossfade_enabled(enabled);
    settings_save_async(&current_settings);
    /* Apply to the next transition, not the one after it. */
    if (gui_player_has_active_track()) arm_next_track_for_audio(playlist_index);
    refresh_quick_drawer_crossfade_icon(); /* see its own comment -- keeps the drawer icon in sync */
    /* Both of gapless's own surfaces, since enabling crossfade may have just
     * switched it back on. Done here rather than in each caller so no entry
     * point can forget one of them. */
    gui_settings_sync_gapless_toggle();
    gui_shell_refresh_quick_drawer_expansion_toggles();
}

void crossfade_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    gui_player_set_crossfade_enabled(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
}

/* gui_player_set_crossfade_enabled()'s counterpart, and for the same reason:
 * two entry points (the Settings switch and the quick drawer tile) share
 * one rule, so it cannot be applied in one and missed in the other.
 *
 * Turning gapless off turns crossfade off with it -- crossfade has nothing
 * to blend into once no next track is armed. */
void gui_player_set_gapless_enabled(bool enabled) {
    current_settings.gapless_enabled = enabled;
    if (!enabled && current_settings.crossfade_enabled) {
        /* Sets crossfade false and saves; deliberately called before this
         * function's own save so the two flags persist together. */
        gui_player_set_crossfade_enabled(false);
        gui_settings_sync_crossfade_toggle();
    }
    settings_save_async(&current_settings);
    /* Apply to the next transition, not the one after it: an already-armed
     * next track would otherwise still hand off gaplessly once more. */
    if (gui_player_has_active_track()) arm_next_track_for_audio(playlist_index);
    gui_settings_sync_gapless_toggle();
    gui_shell_refresh_quick_drawer_expansion_toggles();
}

void gapless_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    gui_player_set_gapless_enabled(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
}

void car_mode_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    current_settings.car_mode_enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    settings_save_async(&current_settings);
}

void inline_remote_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    current_settings.inline_remote_enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    headphone_status_refresh_earpods_adc();
    settings_save_async(&current_settings);
}

void lyrics_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    current_settings.lyrics_enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    settings_save_async(&current_settings);
}

void swipe_up_home_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    current_settings.swipe_up_home_enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    lv_obj_t * active = lv_screen_active();
    gui_shell_set_home_indicator_visible(current_settings.swipe_up_home_enabled &&
                                         active != gui_shell_get_home_screen() &&
                                         active != gui_lyrics_get_screen());
    settings_save_async(&current_settings);
}

void hide_player_topbar_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    current_settings.hide_player_topbar = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    settings_save_async(&current_settings);
    sync_player_topbar_visibility(lv_screen_active());
    player_transition_mark_dirty(); /* topbar/back-button target-state visibility just changed -- see the cache's own doc comment */
}

void led_indicator_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    current_settings.led_indicator_enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    led_control_apply(current_settings.led_indicator_enabled);
    settings_save_async(&current_settings);
}

void charge_limiter_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    current_settings.charge_limiter_enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    charge_limiter_poll(current_settings.charge_limiter_enabled, true);
    settings_save_async(&current_settings);
}

void safe_charging_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    current_settings.safe_charging_enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    safe_charging_poll(current_settings.safe_charging_enabled, true);
    settings_save_async(&current_settings);
}

/* refresh_battery_topbar() (defined earlier in this file, topbar setup near
 * gui_init()'s own layout code) already re-syncs the wifi/bt icon positions
 * itself whenever battery_topbar_group's hidden flag actually changes --
 * called here so toggling this switch is reflected immediately, without
 * waiting for the next battery poll tick. */
void battery_percent_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    current_settings.show_battery_percent = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    settings_save_async(&current_settings);
    refresh_battery_topbar();
}

/* refresh_clock_label() re-syncs the AM/PM sprite's own hidden flag itself
 * -- called here so toggling this switch is reflected immediately, same
 * reasoning as battery_percent_switch_event_cb() just above. */
void clock_24h_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    current_settings.clock_24h = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    app_clock_get_persistence(&current_settings.clock_manual_epoch,
                              &current_settings.clock_system_reference);
    settings_save_async(&current_settings);
    refresh_clock_label();
}

/* Log-scale mapping so the slider gives fine control at low frequencies
 * (where the ear is more sensitive to small Hz changes) instead of wasting
 * most of the slider's travel on the top octave. */



void gui_player_init(uint32_t screen_width, uint32_t screen_height) {
    /* Pay thread and stack setup during initialization, not inside the first
     * favorite tap where it would visibly block the LVGL event callback. */
    pthread_once(&favorite_worker_once, favorite_start_worker);
    build_volume_popup();
    build_delete_song_popup();
    build_more_menu_popup();
    gui_track_info_init();
    player_screen = build_player_screen(screen_width, screen_height);
    /* Apply any player_layout_config already set (a plugin's top-level code
     * runs before this on boot/reload, per gui_plugin_set_player_layout()'s
     * own comment) to the freshly-built overlay right away, rather than
     * waiting for the next track's cover decode to pick it up. No-ops
     * cleanly if no config was ever set or no cover is loaded yet. */
    gui_player_refresh_frosted_background();
}

/* For gui_reload.c's in-process UI reload -- deletes every screen/popup this
 * module owns so gui_player_init() can rebuild them from a clean slate
 * without leaking the old objects. volume_popup/delete_song_popup/
 * more_menu_popup and their backdrops are built directly on lv_layer_top()
 * (see build_confirm_popup()'s own comment), not as children of
 * player_screen, so deleting player_screen alone would not reach them. */
void gui_player_teardown(void) {
    player_lyrics_set_open(false, false);
    /* build_volume_popup() creates this with an unguarded lv_timer_create()
     * -- for gui_reload.c's in-process UI reload, delete it here or the old
     * (leaked) timer keeps firing volume_popup_hide_timer_cb() forever,
     * which itself references the GLOBAL volume_popup_hide_timer variable
     * (by then reassigned to the NEW timer) -- so the leaked OLD timer
     * would keep incorrectly pausing/resetting the NEW one on every tick,
     * never itself getting paused, instead of a clean single timer. */
    if (volume_popup_hide_timer) { lv_timer_del(volume_popup_hide_timer); volume_popup_hide_timer = NULL; }
    /* hw_volume_coalesce_teardown() can't re-read a live slider itself (it
     * doesn't know which widget owns the drag) -- that's a deliberate
     * caller-side step, done here exactly like before the extraction. */
    if (volume_popup_hv.drag_active && volume_popup_track)
        hw_volume_coalesce_drag_update(&volume_popup_hv, (int) lv_slider_get_value(volume_popup_track));
    int volume_teardown_pending = volume_popup_hv.pending;
    hw_volume_coalesce_teardown(&volume_popup_hv);
    if (volume_teardown_pending >= 0) {
        current_settings.volume = (float) volume_teardown_pending / 100.0f;
        settings_save(&current_settings); /* Final teardown checkpoint must reach storage. */
    }
    if (volume_popup) { lv_obj_delete(volume_popup); volume_popup = NULL; }
    volume_popup_speaker_icon = NULL;
    volume_popup_value_label = NULL;
    asset_decoded_image_close(&volume_popup_speaker_image);
    volume_popup_track = NULL;
    gui_popup_teardown(&delete_song_popup);
    if (more_menu_popup) { lv_obj_delete(more_menu_popup); more_menu_popup = NULL; }
    if (more_menu_popup_backdrop) { lv_obj_delete(more_menu_popup_backdrop); more_menu_popup_backdrop = NULL; }
    gui_track_info_teardown();
    if (player_screen) { lv_obj_delete(player_screen); player_screen = NULL; }
    /* player_overlay_panel/cover_img are descendants of player_screen, so
     * lv_obj_delete() above already destroyed them -- null the pointers too
     * so gui_player_refresh_frosted_background() (reachable from a plugin's
     * own top-level code via plugin.set_player_layout(), which can run
     * before gui_player_init() rebuilds this screen on reload) sees its
     * existing '!player_overlay_panel' guard correctly no-op instead of
     * writing LVGL styles to freed memory. */
    player_overlay_panel = NULL;
    player_background_img = NULL;
    cover_card = NULL;
    cover_img = NULL;
    artist_label = NULL;
    album_label = NULL;
    song_count_label = NULL;
    song_title_label = NULL;
    quality_pill = NULL;
    format_badge_label = NULL;
    order_icon = NULL;
    favorite_circle = NULL;
    favorite_icon = NULL;
    play_btn = NULL;
    prev_btn = NULL;
    next_btn = NULL;
    progress_slider = NULL;
    pos_label = NULL;
    dur_label = NULL;
    player_dismiss_btn = NULL;
    volume_slider = NULL;
}

void gui_player_refresh_static_assets(void) {
    refresh_play_btn_icon();
    if (!volume_popup) return;
    asset_decoded_image_close(&volume_popup_speaker_image);
    const void * speaker = asset_decoded_image_open(&volume_popup_speaker_image, "volume/vol.png")
                         ? asset_decoded_image_source(&volume_popup_speaker_image) : NULL;
    if (volume_popup_speaker_icon)
        lv_image_set_src(volume_popup_speaker_icon, speaker ? speaker : asset_path("volume/vol.png"));
}


void gui_format_time(double seconds, char * buf, size_t buf_size) {
    if (!buf || buf_size == 0) return;
    if (seconds <= 0.0) {
        snprintf(buf, buf_size, "0:00");
        return;
    }
    uint64_t total = (uint64_t) (seconds + 0.5);
    uint64_t hours = total / 3600;
    unsigned int minutes = (unsigned int) ((total % 3600) / 60);
    unsigned int secs = (unsigned int) (total % 60);
    if (hours > 0)
        snprintf(buf, buf_size, "%llu:%02u:%02u", (unsigned long long) hours, minutes, secs);
    else
        snprintf(buf, buf_size, "%u:%02u", minutes, secs);
}

void gui_player_poll_confirmed_playback(void) {
    if (consecutive_decoder_failure_skips > 0 && audio_is_playing()) {
        if (audio_get_position_seconds() >= 3.0) {
            reset_decoder_failure_tracking();
        }
    }
}

void gui_player_update_progress(void) {
    gui_player_poll_confirmed_playback();
    if (deferred_resume_pending) return; /* audio_get_position_seconds()/
        audio_get_duration_seconds() are both meaningless here -- no decoder is open
        yet. Preserve prepare_deferred_resume()'s seeded slider/label values until the
        user presses Play (which clears deferred_resume_pending and opens the real
        decoder, at which point this function's normal live-value logic takes over
        correctly on its own). */
    double position = audio_get_position_seconds();
    double duration = audio_get_duration_seconds();

    if (progress_awaiting_seek) {
        if (fabs(position - pending_progress_seek_seconds) < 0.4 ||
            ++progress_awaiting_seek_ticks >= 8)
            progress_awaiting_seek = false;
        else
            return;
    }

    if (duration > 0 && progress_slider) {
        int32_t percent = (int32_t) ((position / duration) * 100.0);
        if (percent != displayed_progress_percent) {
            displayed_progress_percent = percent;
            lv_slider_set_value(progress_slider, percent, LV_ANIM_OFF);
        }
    }

    int position_second = (int) position;
    int duration_second = (int) duration;
    if (pos_label && position_second != displayed_position_second) {
        char pos_str[24];
        displayed_position_second = position_second;
        gui_format_time(position, pos_str, sizeof(pos_str));
        lv_label_set_text(pos_label, pos_str);
    }
    if (dur_label && duration_second != displayed_duration_second) {
        char dur_str[24];
        displayed_duration_second = duration_second;
        gui_format_time(duration, dur_str, sizeof(dur_str));
        lv_label_set_text(dur_label, dur_str);
    }
}

bool gui_player_has_background_work(void) {
    return cover_decode_active || gui_player_queue_write_busy() || queue_save_active();
}

void gui_player_cancel_background_work(void) {
    queue_save_cancel();
    if (cover_decode_active) {
        pthread_join(cover_decode_thread, NULL);
        cover_decode_active = false;
        free(cover_decode_result_pixels);
        free(cover_decode_result_reflection);
        cover_decode_result_pixels = NULL;
        cover_decode_result_reflection = NULL;
        atomic_store_explicit(&cover_decode_done_flag, false, memory_order_relaxed);
    }
    if (atomic_load_explicit(&cover_reload_active, memory_order_relaxed))
        gui_player_cancel_cover_reload();
}

const char * playlist_path_at(int index) {
    if (index < 0 || index >= playlist_count) return "";
    if (playlist[index]) return playlist[index];
    if (!playlist_lazy_sort_order) return "";
    int sort_pos = playlist_lazy_sort_order[index];
    char resolved[PATH_MAX];
    bool got = playlist_lazy_snapshot && metadata_db_snapshot_path_at(playlist_lazy_snapshot, sort_pos,
                                                                       resolved, sizeof(resolved));
    if (!got && playlist_lazy_file_index)
        got = file_browser_index_playable_path_at(playlist_lazy_file_index, (unsigned) sort_pos,
                                                  resolved, sizeof(resolved));
    /* A missed read must stay unresolved. Caching "" makes the slot look
     * empty forever and a later play would save that as the resume path. */
    if (!got || !resolved[0]) return "";
    char *copy = strdup(resolved);
    if (!copy) return "";
    playlist[index] = copy;
    return copy;
}

void on_file_selected_lazy_all_songs(int selected_index) {
    int64_t count64 = metadata_db_get_song_count();
    int count = count64 > 0 ? (int) count64 : 0;
    if (count <= 0 || selected_index < 0 || selected_index >= count) return;

    char ** new_pl = calloc((size_t) count, sizeof(char *));
    int * new_order = malloc(sizeof(int) * (size_t) count);
    if (!new_pl || !new_order) {
        free(new_pl);
        free(new_order);
        return;
    }

    metadata_db_snapshot_t * snapshot = metadata_db_snapshot_open(false);
    if (!snapshot || metadata_db_snapshot_count(snapshot) != count) {
        if (snapshot) metadata_db_snapshot_close(snapshot);
        free(new_pl); free(new_order); return;
    }
    free_playlist();
    playlist_lazy_snapshot = snapshot;
    playlist = new_pl;
    playlist_lazy_sort_order = new_order;
    queue_sd_dirfd = open_queue_dir_from_snapshot(playlist_lazy_snapshot);
    playlist_count = count;
    for (int i = 0; i < count; i++) playlist_lazy_sort_order[i] = i;
    playlist_lazy_order_is_recency = false;
    queued_pending_count = 0;
    play_track_at(selected_index);
}

void on_file_selected_lazy_recently_added(int selected_index) {
    int64_t count64 = metadata_db_get_song_count();
    int count = count64 > 0 ? (int) count64 : 0;
    if (count <= 0 || selected_index < 0 || selected_index >= count) return;

    char ** new_pl = calloc((size_t) count, sizeof(char *));
    int * new_order = malloc(sizeof(int) * (size_t) count);
    if (!new_pl || !new_order) {
        free(new_pl);
        free(new_order);
        return;
    }

    metadata_db_snapshot_t * snapshot = metadata_db_snapshot_open(true);
    if (!snapshot || metadata_db_snapshot_count(snapshot) != count) {
        if (snapshot) metadata_db_snapshot_close(snapshot);
        free(new_pl); free(new_order); return;
    }
    free_playlist();
    playlist_lazy_snapshot = snapshot;
    playlist = new_pl;
    playlist_lazy_sort_order = new_order;
    queue_sd_dirfd = open_sd_queue_dir();
    playlist_count = count;
    for (int i = 0; i < count; i++) playlist_lazy_sort_order[i] = i;
    playlist_lazy_order_is_recency = true;
    queued_pending_count = 0;
    play_track_at(selected_index);
}

static bool resume_playlist_needs_lazy_order = false;
static file_browser_index_t *resume_file_index = NULL;
#ifdef HOST_BUILD
#define QUEUE_RESUME_PATH "./.compas/queue.bin"
#define SD_QUEUE_RESUME_PATH "./music/.compas/queue.bin"
#define SD_QUEUE_RESUME_DIR "./music/.compas"
#define QUEUE_RESUME_LEGACY_PATH "./open_hiby_player_queue.bin"
#define SD_QUEUE_RESUME_LEGACY_PATH "./music/.open_hiby_player/queue.bin"
#else
#define QUEUE_RESUME_PATH "/usr/data/.compas/queue.bin"
#define SD_QUEUE_RESUME_PATH "/data/mnt/sd_0/.compas/queue.bin"
#define SD_QUEUE_RESUME_DIR "/data/mnt/sd_0/.compas"
#define QUEUE_RESUME_LEGACY_PATH "/usr/data/open_hiby_player_queue.bin"
#define SD_QUEUE_RESUME_LEGACY_PATH "/data/mnt/sd_0/.open_hiby_player/queue.bin"
#endif

/* A lazy All-Songs queue has no paths until playlist_path_at() resolves them.
 * Persisting one above this bound would materialize and strdup the whole
 * library, which is unbounded work for a periodic checkpoint on the target.
 * The settings last_track/last_source_kind resume path remains available. */
#define LAZY_QUEUE_CHECKPOINT_MAX_ENTRIES 32768

static atomic_bool sd_card_absent_immediate = false;

void gui_player_notify_sd_unmounted_immediate(void) {
    atomic_store(&sd_card_absent_immediate, true);
}

/* Call on every poll that currently observes the SD card mounted, regardless
 * of whether a full confirmed mount/unmount transition fires -- a transient
 * one-poll blip also calls gui_player_notify_sd_unmounted_immediate() above,
 * and this is what un-sticks the resulting flag for a card that was never
 * actually removed. */
void gui_player_notify_sd_mounted(void) {
    atomic_store(&sd_card_absent_immediate, false);
}

static bool is_sd_card_path(const char * path) {
    if (!path || !path[0]) return false;
    size_t root_len = strlen(MUSIC_ROOT_DIR);
    if (strncmp(path, MUSIC_ROOT_DIR, root_len) != 0) return false;
    return path[root_len] == '/' || path[root_len] == '\0';
}

static queue_resume_t restored_queue;
static bool have_restored_queue;

typedef struct {
    int target_dirfd;
    char target_name[NAME_MAX];
    bool target_is_sd;
    queue_resume_t queue;
} checkpoint_worker_ctx_t;

static checkpoint_worker_ctx_t checkpoint_ctx;
static pthread_t checkpoint_thread;
static bool checkpoint_running;
static atomic_bool checkpoint_done;
static atomic_bool checkpoint_urgent_pending;
static atomic_bool checkpoint_large_lazy_cleanup_pending;
static atomic_bool checkpoint_failure_pending;

static bool queue_checkpoint_is_large_lazy(void) {
    return playlist_lazy_sort_order != NULL &&
           playlist_count > LAZY_QUEUE_CHECKPOINT_MAX_ENTRIES;
}

static int open_queue_dir(const char * path) {
    return open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
}

static int open_sd_queue_dir(void) {
    /* Pin the root mount before creating/opening the child.  All later worker
     * operations use this descriptor, so a card replacement cannot redirect
     * rename/unlink to the new card. */
    int rootfd = open(MUSIC_ROOT_DIR, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (rootfd < 0) return -1;
    (void) mkdirat(rootfd, COMPAS_STORAGE_DIR, 0755);
    int dirfd = openat(rootfd, COMPAS_STORAGE_DIR, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    close(rootfd);
    return dirfd;
}

static int open_queue_dir_from_snapshot(const metadata_db_snapshot_t *snapshot) {
    int dbfd = metadata_db_snapshot_dup_directory_fd(snapshot);
    if (dbfd < 0) return -1;
    int rootfd = openat(dbfd, "..", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    close(dbfd);
    if (rootfd < 0) return -1;
    (void) mkdirat(rootfd, COMPAS_STORAGE_DIR, 0755);
    int dirfd = openat(rootfd, COMPAS_STORAGE_DIR, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    close(rootfd);
    return dirfd;
}

static int open_queue_dir_from_file_index(const file_browser_index_t *index) {
    const char *dir = file_browser_index_directory(index);
    size_t root_len = strlen(MUSIC_ROOT_DIR);
    if (!dir || strncmp(dir, MUSIC_ROOT_DIR, root_len) != 0 ||
        (dir[root_len] != '/' && dir[root_len] != '\0')) return -1;
    int fd = file_browser_index_dup_directory_fd(index);
    if (fd < 0) return -1;
    const char *suffix = dir + root_len;
    unsigned levels = 0;
    for (const char *p = suffix; *p;) {
        while (*p == '/') p++;
        if (!*p) break;
        levels++;
        while (*p && *p != '/') p++;
    }
    for (unsigned i = 0; i < levels; i++) {
        int parent = openat(fd, "..", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (parent < 0) { close(fd); return -1; }
        close(fd); fd = parent;
    }
    (void) mkdirat(fd, COMPAS_STORAGE_DIR, 0755);
    int queuefd = openat(fd, COMPAS_STORAGE_DIR, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    close(fd);
    return queuefd;
}

static int open_checkpoint_target_dir(const char * path) {
    char dir[PATH_MAX];
    const char * slash = strrchr(path, '/');
    if (!slash) return open_queue_dir(".");
    size_t n = (size_t) (slash - path);
    if (n == 0) { dir[0] = '/'; dir[1] = 0; }
    else if (n < sizeof(dir)) { memcpy(dir, path, n); dir[n] = 0; }
    else return -1;
    return open_queue_dir(dir);
}

static void remove_queue_checkpoint_at(int dirfd, const char * name) {
    if (dirfd >= 0 && name) (void) unlinkat(dirfd, name, 0);
}

static void remove_legacy_sd_checkpoint(int compas_dirfd) {
    int rootfd = openat(compas_dirfd, "..", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (rootfd < 0) return;
    int legacyfd = openat(rootfd, LEGACY_STORAGE_DIR, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (legacyfd >= 0) { (void) unlinkat(legacyfd, "queue.bin", 0); close(legacyfd); }
    close(rootfd);
}

static void retire_legacy_checkpoint(const checkpoint_worker_ctx_t * ctx) {
    if (!ctx) return;
    if (!ctx->target_is_sd) {
        (void) unlink(QUEUE_RESUME_LEGACY_PATH);
        return;
    }
    remove_legacy_sd_checkpoint(ctx->target_dirfd);
}

/* Plain uint64_t, not atomic: this target's toolchain lacks a native 64-bit
 * atomic instruction and needs libatomic for one (confirmed -- linking
 * atomic_uint_least64_t here fails with "undefined reference to
 * __atomic_store_8" on this project's mipsel cross-compiler, which isn't
 * linked against libatomic), so an 8-byte atomic isn't a free change here
 * the way the existing 1-byte atomic_bool usage in this file is. The
 * checkpoint worker thread can write checkpoint_revision_sd to 0 on failure
 * at the same time gui_player_handle_sd_unmount() resets it to 0 on the UI
 * thread on a confirmed unmount -- both only ever write the same value (0),
 * so this is a logically harmless race, accepted rather than pulling in a
 * new link dependency to make it formally well-defined. */
static uint64_t checkpoint_revision_internal;
static double checkpoint_position_internal = -1;
static uint64_t checkpoint_revision_sd;
static double checkpoint_position_sd = -1;

static void * queue_checkpoint_worker(void * arg) {
    checkpoint_worker_ctx_t * ctx = (checkpoint_worker_ctx_t *) arg;
    bool wrote = queue_resume_write_at(ctx->target_dirfd, ctx->target_name, &ctx->queue);
    if (!wrote) {
        /* Retry on the next checkpoint; never replace a valid file on failure. */
        atomic_store(&checkpoint_failure_pending, true);
    } else {
        atomic_store(&checkpoint_failure_pending, false);
        retire_legacy_checkpoint(ctx);
    }
    queue_resume_free(&ctx->queue);
    if (atomic_load(&checkpoint_large_lazy_cleanup_pending)) {
        remove_queue_checkpoint_at(ctx->target_dirfd, ctx->target_name);
    }
    close(ctx->target_dirfd);
    ctx->target_dirfd = -1;
    atomic_store(&checkpoint_done, true);
    return NULL;
}

bool gui_player_queue_write_busy(void) {
    return checkpoint_running && !atomic_load(&checkpoint_done);
}

/* UI code may poll this and present one persistent status indicator; the flag
 * is deliberately level-triggered so a failed read-only card does not create
 * a toast on every periodic checkpoint attempt. */
bool gui_player_queue_checkpoint_failed(void) {
    return atomic_load(&checkpoint_failure_pending);
}

void gui_player_queue_checkpoint(void) {
    if (queue_checkpoint_is_large_lazy()) {
        /* Set this before observing the worker: if it is just finishing, the
         * worker will remove anything it wrote; the join path below covers the
         * race where it has already published checkpoint_done. */
        atomic_store(&checkpoint_large_lazy_cleanup_pending, true);
        atomic_store(&checkpoint_urgent_pending, false);
        if (checkpoint_running) {
            if (!atomic_load(&checkpoint_done)) return;
            pthread_join(checkpoint_thread, NULL);
            checkpoint_running = false;
        }
        int internal_dirfd = open_checkpoint_target_dir(QUEUE_RESUME_PATH);
        if (internal_dirfd >= 0) { remove_queue_checkpoint_at(internal_dirfd, "queue.bin"); close(internal_dirfd); }
        if (queue_sd_dirfd >= 0) remove_queue_checkpoint_at(queue_sd_dirfd, "queue.bin");
        (void) unlink(QUEUE_RESUME_LEGACY_PATH);
        if (queue_sd_dirfd >= 0) remove_legacy_sd_checkpoint(queue_sd_dirfd);
        checkpoint_revision_internal = 0;
        checkpoint_position_internal = -1;
        checkpoint_revision_sd = 0;
        checkpoint_position_sd = -1;
        atomic_store(&checkpoint_large_lazy_cleanup_pending, false);
        return;
    }

    if (checkpoint_running) {
        if (!atomic_load(&checkpoint_done)) return;
        pthread_join(checkpoint_thread, NULL);
        checkpoint_running = false;
        if (atomic_load(&checkpoint_failure_pending)) {
            checkpoint_revision_internal = 0;
            checkpoint_revision_sd = 0;
        }
    }
    atomic_store(&checkpoint_large_lazy_cleanup_pending, false);
    if (!gui_player_has_active_track()) return;
    double position = deferred_resume_pending ? deferred_resume_position : audio_get_resume_position_seconds();

    bool is_sd_persistable = true;
    for (int i = 0; i < playlist_count; i++) {
        const char * p = playlist_path_at(i);
        if (!p || !p[0] || !is_sd_card_path(p) ||
            strncmp(p, SUBSONIC_STREAM_CACHE_DIR, strlen(SUBSONIC_STREAM_CACHE_DIR)) == 0) {
            is_sd_persistable = false;
            break;
        }
    }

    bool sd_mounted = sd_card_root_is_mounted() && !atomic_load(&sd_card_absent_immediate);

    const char * target_path = QUEUE_RESUME_PATH;
    const char * target_name = "queue.bin";
    int target_dirfd = open_checkpoint_target_dir(target_path);
    uint64_t * last_rev = &checkpoint_revision_internal;
    double * last_pos = &checkpoint_position_internal;

    if (is_sd_persistable && sd_mounted) {
        target_name = "queue.bin";
        target_path = SD_QUEUE_RESUME_PATH;
        if (target_dirfd >= 0) close(target_dirfd);
        /* A restored queue.bin fills playlist[] without going through
         * on_file_selected(), which is the only place that used to pin this
         * directory. Leaving it at -1 made every boot checkpoint fail before
         * any write, and the toast blamed a read-only card. */
        if (queue_sd_dirfd < 0) queue_sd_dirfd = open_sd_queue_dir();
        target_dirfd = queue_sd_dirfd >= 0 ? dup(queue_sd_dirfd) : -1;
        last_rev = &checkpoint_revision_sd;
        last_pos = &checkpoint_position_sd;
    }

    if (queue_revision == *last_rev && fabs(position - *last_pos) < 1.0) { if (target_dirfd >= 0) close(target_dirfd); return; }
    if (target_dirfd < 0) { *last_rev = 0; atomic_store(&checkpoint_failure_pending, true); return; }

    queue_resume_t s = { .count = playlist_count, .current = playlist_index, .pending = queued_pending_count,
        .mode = current_settings.play_mode, .shuffle_pos = shuffle_pos, .position = position };
    s.paths = calloc((size_t) s.count, sizeof(char *));
    if (!s.paths) { close(target_dirfd); return; }
    bool ok = true;
    for (int i = 0; i < s.count; i++) {
        const char * path = playlist_path_at(i);
        if (!path || !path[0] || remote_track_path_is_remote(path) ||
            strncmp(path, SUBSONIC_STREAM_CACHE_DIR, strlen(SUBSONIC_STREAM_CACHE_DIR)) == 0) { ok = false; break; }
        s.paths[i] = strdup(path);
        if (!s.paths[i]) { ok = false; break; }
    }
    if (ok && current_settings.play_mode == PLAY_MODE_SHUFFLE) {
        ensure_shuffle_order_current();
        s.shuffle_pos = shuffle_pos;
        s.order = malloc((size_t) s.count * sizeof(int));
        ok = s.order && shuffle_order;
        if (ok) memcpy(s.order, shuffle_order, (size_t) s.count * sizeof(int));
        if (ok && pending_shuffle_order) {
            s.continuation = malloc((size_t) s.count * sizeof(int));
            ok = s.continuation != NULL;
            if (ok) memcpy(s.continuation, pending_shuffle_order, (size_t) s.count * sizeof(int));
        }
    }
    if (!ok) { queue_resume_free(&s); close(target_dirfd); return; }

    checkpoint_ctx.target_dirfd = target_dirfd;
    snprintf(checkpoint_ctx.target_name, sizeof(checkpoint_ctx.target_name), "%s", target_name);
    checkpoint_ctx.target_is_sd = is_sd_persistable && sd_mounted;
    checkpoint_ctx.queue = s;
    *last_rev = queue_revision;
    *last_pos = position;

    atomic_store(&checkpoint_done, false);
    checkpoint_running = pthread_create(&checkpoint_thread, NULL, queue_checkpoint_worker, &checkpoint_ctx) == 0;
    if (!checkpoint_running) { queue_resume_free(&checkpoint_ctx.queue); close(target_dirfd); checkpoint_ctx.target_dirfd = -1; *last_rev = 0; }
}

/* Like gui_player_queue_checkpoint(), but guarantees the request is not silently
 * dropped if a previous checkpoint's background worker is still writing: it marks
 * an urgent-pending flag instead, which gui_player_queue_poll_urgent() below
 * retries on the very next UI poll tick once the in-flight worker finishes,
 * rather than waiting for the next scheduled 30-second/1-second periodic tick. */
void gui_player_queue_checkpoint_urgent(void) {
    if (checkpoint_running && !atomic_load(&checkpoint_done)) {
        atomic_store(&checkpoint_urgent_pending, true);
        return;
    }
    gui_player_queue_checkpoint();
}

/* Called every UI poll tick (gui_queue_poll()) to retry a checkpoint request that
 * gui_player_queue_checkpoint_urgent() deferred because a worker was already
 * in flight. No-op unless a retry is actually pending. */
void gui_player_queue_poll_urgent(void) {
    if (!atomic_load(&checkpoint_urgent_pending)) return;
    if (checkpoint_running && !atomic_load(&checkpoint_done)) return; /* still busy */
    atomic_store(&checkpoint_urgent_pending, false);
    gui_player_queue_checkpoint();
}

void gui_player_queue_flush(void) {
    if (checkpoint_running) {
        pthread_join(checkpoint_thread, NULL);
        checkpoint_running = false;
    }
    gui_player_queue_checkpoint();
    if (checkpoint_running) {
        pthread_join(checkpoint_thread, NULL);
        checkpoint_running = false;
    }
}

void gui_player_handle_sd_unmount(void) {
    atomic_store(&sd_card_absent_immediate, true);
    checkpoint_revision_sd = 0;
    checkpoint_position_sd = -1;

    /* Deliberately do NOT join checkpoint_thread here, even if one is still
     * running: this runs on the UI thread (via poll_sd_card_hotplug()'s
     * 500ms timer), and the worker may be blocked inside a write()/fsync()
     * to the very card that's being pulled out from under it -- joining
     * unconditionally here would risk freezing the whole UI on a stalled/
     * hung SD write. This is safe to skip: the worker only ever touches its
     * own deep-copied checkpoint_ctx.queue snapshot (built synchronously
     * before pthread_create()), never the live playlist[] this function is
     * about to free, so there's no actual data race to guard against by
     * waiting for it here. It will finish (or fail harmlessly, retried on
     * the next checkpoint) on its own; checkpoint_running/checkpoint_done
     * are already correctly rechecked, non-blockingly, by the next call to
     * gui_player_queue_checkpoint(). */

    /* Lazy song queues belong to the SD database even when no path has been
     * resolved yet. Never resolve every entry of a 300K queue on the UI
     * thread just to decide whether it must be cleared. */
    bool has_sd_playlist = playlist_lazy_sort_order != NULL;
    for (int i = 0; !has_sd_playlist && i < playlist_count; i++) {
        if (is_sd_card_path(playlist[i])) {
            has_sd_playlist = true;
            break;
        }
    }
    if (!has_sd_playlist) return;

    const char * cur_path = playlist_path_at(playlist_index);
    if (is_sd_card_path(cur_path)) {
        audio_stop();
        plugin_manager_notify_stopped();
    }
    free_playlist();
    clear_player_source();
    set_play_button_state(false);
    if (song_title_label) lv_label_set_text(song_title_label, "No track loaded");
    if (album_label) lv_label_set_text(album_label, "");
    now_playing_genre[0] = '\0';
    now_playing_track_number = 0;
    if (artist_label) lv_label_set_text(artist_label, "");
    if (lv_screen_active() == player_screen) nav_pop();
}

bool build_sd_card_resume_playlist(char *** out_playlist, int * out_count, int * out_index, double * out_position) {
    resume_playlist_needs_lazy_order = false;
    queue_resume_free(&restored_queue);
    have_restored_queue = false;

    if (!queue_resume_read(SD_QUEUE_RESUME_PATH, &restored_queue) &&
        !queue_resume_read(SD_QUEUE_RESUME_LEGACY_PATH, &restored_queue)) return false;

    if (restored_queue.count <= 0 || restored_queue.current < 0 || restored_queue.current >= restored_queue.count) {
        queue_resume_free(&restored_queue);
        return false;
    }

    bool all_sd = true;
    for (int i = 0; i < restored_queue.count; i++) {
        const char * path = restored_queue.paths[i];
        if (!path || !path[0] || !is_sd_card_path(path) ||
            strncmp(path, SUBSONIC_STREAM_CACHE_DIR, strlen(SUBSONIC_STREAM_CACHE_DIR)) == 0) {
            all_sd = false;
            break;
        }
    }
    if (!all_sd) {
        queue_resume_free(&restored_queue);
        return false;
    }

    *out_playlist = restored_queue.paths;
    *out_count = restored_queue.count;
    *out_index = restored_queue.current;
    if (out_position) *out_position = restored_queue.position;
    restored_queue.paths = NULL;
    have_restored_queue = true;
    return true;
}

bool gui_player_restore_sd_queue(bool is_boot) {
    atomic_store(&sd_card_absent_immediate, false);

    if (is_boot) {
        /* This can be called a second time shortly after boot -- once from
         * gui_init() directly, and again from poll_sd_card_hotplug()'s
         * boot-time-mount-race recheck (which covers the case where the
         * card wasn't mounted yet when gui_init() ran) -- both with
         * is_boot=true. Block on ANY already-loaded track, not just an
         * SD-based one: an SD-based track means the first call (or the
         * pre-existing car-mode/resume_mode fallback) already handled it,
         * so re-running here would free_playlist() and restart playback
         * moments after it had already started correctly -- but a NON-SD
         * track is just as much a reason to skip (e.g. the user started a
         * Subsonic stream while the card was out, reinserted it -- the
         * confirmed-reinsertion path already correctly preserved that
         * session, is_boot=true must not then clobber it on the very next
         * poll just because it doesn't recognize the path as "already SD").
         * Only actually restore here if nothing is loaded at all -- that's
         * the genuine late-mount case this second call exists for. */
        if (gui_player_has_active_track()) return false;
    } else {
        /* Preserve any loaded non-SD session, not just an actively-playing
         * one -- a paused (or deferred-pending) stream is just as much a
         * session the user cares about and doesn't want silently replaced
         * by a newly-inserted card's queue. */
        if (gui_player_has_active_track()) {
            const char * cur = playlist_path_at(playlist_index);
            if (!is_sd_card_path(cur)) return false;
        }
    }

    char ** resume_playlist = NULL;
    int resume_count = 0, resume_index = 0;
    double position = 0.0;
    if (!build_sd_card_resume_playlist(&resume_playlist, &resume_count, &resume_index, &position)) return false;

    /* install_saved_resume_playlist() only swaps the queue bookkeeping
     * (free_playlist() + replace playlist[]/playlist_count) -- it never
     * touches the audio subsystem. The guards above block the common cases
     * where something is still active, but the is_boot=false path
     * deliberately still allows through an active track that's already
     * SD-based (a genuinely unusual leftover, since gui_player_handle_sd_unmount()
     * normally clears that on confirmed unmount) -- if that's somehow still
     * playing here, stop it first so playback and the newly-installed queue
     * never disagree about what's current. */
    if (gui_player_has_active_track() && audio_is_playing()) audio_stop();

    if (!install_saved_resume_playlist(resume_playlist, resume_count)) return false;

    if (current_settings.resume_mode == 1) {
        play_track_at_from(resume_index, position);
    } else if (current_settings.resume_mode == 2) {
        prepare_deferred_resume(resume_index, position);
    } else {
        /* Resume disabled: load and display the queue/track, but don't
         * resume playback position -- deferred_resume_pending still needs
         * to be set (position 0, not the checkpoint's) so a later Play
         * actually starts this track from the top, matching the existing
         * "queue loaded, not playing" pattern used elsewhere in this file.
         * Without it, playlist_index is set but nothing is "current" in the
         * audio subsystem yet, so toggle_play_pause() would be a no-op. */
        playlist_index = resume_index;
        track_metadata_t meta;
        apply_track_metadata_to_ui(resume_index, &meta);
        set_play_button_state(false);
        deferred_resume_pending = true; deferred_resume_position = 0.0;
    }
    return true;
}

bool gui_player_queue_restart_displayed(int selected) {
    if (playlist_count <= 0 || selected < 0 || selected >= playlist_count) return false;
    int *order = NULL, count = 0, current = 0;
    uint64_t revision = 0;
    if (!gui_player_queue_snapshot(&order, &count, &current, &revision) || count != playlist_count) {
        free(order);
        return false;
    }
    char **next_paths = NULL;
    int *next_ranks = NULL;
    if (!playback_order_resequence(order, count, playlist, playlist_lazy_sort_order, &next_paths, &next_ranks)) {
        free(order);
        return false;
    }
    free(order);
    free(playlist);
    free(playlist_lazy_sort_order);
    playlist = next_paths;
    playlist_lazy_sort_order = next_ranks;
    playlist_index = selected;
    queued_pending_count = 0;
    queue_next_insert_index = -1;
    free(shuffle_order);
    shuffle_order = NULL;
    shuffle_order_count = 0;
    shuffle_pos = -1;
    free(pending_shuffle_order);
    pending_shuffle_order = NULL;
    return true;
}

bool build_saved_resume_playlist(char *** out_playlist, int * out_count, int * out_index) {
    resume_playlist_needs_lazy_order = false;
    if (resume_file_index) {
        file_browser_index_close(resume_file_index);
        resume_file_index = NULL;
    }
    queue_resume_free(&restored_queue);
    have_restored_queue = false;
    if (queue_resume_read(QUEUE_RESUME_PATH, &restored_queue) ||
        queue_resume_read(QUEUE_RESUME_LEGACY_PATH, &restored_queue)) {
        bool local = true;
        for (int i = 0; i < restored_queue.count; i++) {
            const char * path = restored_queue.paths[i];
            if (remote_track_path_is_remote(path) || strncmp(path, SUBSONIC_STREAM_CACHE_DIR, strlen(SUBSONIC_STREAM_CACHE_DIR)) == 0)
                local = false;
        }
        if (local && strcmp(restored_queue.paths[restored_queue.current], current_settings.last_track) == 0) {
            *out_playlist = restored_queue.paths;
            *out_count = restored_queue.count;
            *out_index = restored_queue.current;
            restored_queue.paths = NULL;
            current_settings.last_position = restored_queue.position;
            have_restored_queue = true;
            return true;
        }
        queue_resume_free(&restored_queue);
    }
    song_row_t current;
    bool indexed = metadata_db_get_song_by_path(current_settings.last_track, &current);

    if (indexed && current_settings.last_source_kind == 2 && current_settings.last_source_name[0] &&
        strcasecmp(current.tags.album, current_settings.last_source_name) == 0) {
        int64_t count64 = metadata_db_count_songs_filtered(NULL, NULL, current.tags.album_artist, current.tags.album, NULL);
        if (count64 > 0 && count64 <= INT_MAX) {
            int count = (int) count64;
            group_song_entry_t * entries = calloc((size_t) count, sizeof(*entries));
            char ** paths = calloc((size_t) count, sizeof(*paths));
            song_row_t rows[64];
            int loaded = 0, selected = -1;
            while (entries && paths && loaded < count) {
                int want = count - loaded;
                if (want > 64) want = 64;
                int got = metadata_db_get_songs_filtered_page(NULL, NULL, current.tags.album_artist,
                                                               current.tags.album, NULL, loaded, want, rows);
                if (got <= 0) break;
                for (int i = 0; i < got; i++) {
                    char title[128];
                    metadata_db_song_display_title(&rows[i], title, sizeof(title));
                    entries[loaded + i].path = strdup(rows[i].path);
                    entries[loaded + i].title = strdup(title);
                    paths[loaded + i] = strdup(rows[i].path);
                    if (strcmp(rows[i].path, current_settings.last_track) == 0) selected = loaded + i;
                    if (!entries[loaded + i].path || !entries[loaded + i].title || !paths[loaded + i]) {
                        got = 0;
                        break;
                    }
                }
                if (got <= 0) break;
                loaded += got;
                if (got < want) break;
            }
            if (loaded == count && selected >= 0) {
                set_player_source_group_songs_direct(entries, count, current.tags.album, selected);
                free_group_song_entries(entries, count);
                *out_playlist = paths;
                *out_count = count;
                *out_index = selected;
                return true;
            }
            if (entries) free_group_song_entries(entries, loaded);
            if (paths) {
                for (int i = 0; i < loaded; i++) free(paths[i]);
                free(paths);
            }
        }
    } else if (indexed && current_settings.last_source_kind == 1) {
        int64_t count64 = metadata_db_get_song_count();
        int64_t selected64 = metadata_db_get_song_title_offset(current_settings.last_track);
        if (count64 > 0 && count64 <= INT_MAX && selected64 >= 0 && selected64 < count64) {
            *out_playlist = calloc((size_t) count64, sizeof(char *));
            if (!*out_playlist) return false;
            *out_count = (int) count64;
            *out_index = (int) selected64;
            resume_playlist_needs_lazy_order = true;
            set_player_source_all_songs(*out_index);
            return true;
        }
    }

    /* A Files folder, including one too large to checkpoint, reopens its
     * directory index. Do not fall through to a full path allocation. */
    file_browser_index_t *folder = NULL;
    unsigned playable = 0, selected = 0;
    if (!current_settings.last_track[0] ||
        !file_browser_open_lazy_directory(current_settings.last_track, &folder, &playable, &selected) ||
        playable == 0 || playable > (unsigned) INT_MAX) {
        if (folder) file_browser_index_close(folder);
        return false;
    }
    char **slots = calloc(playable, sizeof(*slots));
    if (!slots) {
        file_browser_index_close(folder);
        return false;
    }
    *out_playlist = slots;
    *out_count = (int) playable;
    *out_index = (int) selected;
    resume_file_index = folder;
    return true;
}

bool install_saved_resume_playlist(char ** resume_playlist, int resume_count) {
    file_browser_index_t *file_index = resume_file_index;
    resume_file_index = NULL;
    int * new_order = NULL;
    if (resume_playlist_needs_lazy_order || file_index) {
        new_order = malloc(sizeof(int) * (size_t) resume_count);
        if (!new_order) {
            for (int i = 0; i < resume_count; i++) free(resume_playlist[i]);
            free(resume_playlist);
            if (file_index) file_browser_index_close(file_index);
            return false;
        }
        for (int i = 0; i < resume_count; i++) new_order[i] = i;
    }
    metadata_db_snapshot_t * resume_snapshot = NULL;
    if (resume_playlist_needs_lazy_order && !file_index) {
        resume_snapshot = metadata_db_snapshot_open(false);
        if (!resume_snapshot || metadata_db_snapshot_count(resume_snapshot) != resume_count) {
            if (resume_snapshot) metadata_db_snapshot_close(resume_snapshot);
            free(new_order);
            for (int i = 0; i < resume_count; i++) free(resume_playlist[i]);
            free(resume_playlist);
            return false;
        }
    }
    free_playlist();
    playlist_lazy_snapshot = resume_snapshot;
    playlist_lazy_file_index = file_index;
    playlist = resume_playlist;
    playlist_count = resume_count;
    playlist_lazy_sort_order = new_order;
    playlist_lazy_order_is_recency = false;
    if (playlist_lazy_file_index) queue_sd_dirfd = open_queue_dir_from_file_index(playlist_lazy_file_index);
    else if (playlist_lazy_snapshot) queue_sd_dirfd = open_queue_dir_from_snapshot(playlist_lazy_snapshot);
    if (have_restored_queue) {
        queued_pending_count = restored_queue.pending;
        queue_next_insert_index = queued_pending_count ? restored_queue.current + 1 + queued_pending_count : -1;
        current_settings.play_mode = restored_queue.mode;
        shuffle_order = restored_queue.order; restored_queue.order = NULL;
        pending_shuffle_order = restored_queue.continuation; restored_queue.continuation = NULL;
        shuffle_order_count = shuffle_order ? resume_count : 0;
        shuffle_pos = restored_queue.shuffle_pos;
        queue_resume_free(&restored_queue);
        have_restored_queue = false;
    }
    return true;
}

void prepare_deferred_resume(int index, double start_seconds) {
    playlist_index = index;
    track_metadata_t meta;
    apply_track_metadata_to_ui(index, &meta);
    set_play_button_state(false);
    deferred_resume_pending = true;
    deferred_resume_position = start_seconds;

    /* apply_track_metadata_to_ui() just reset the progress slider/labels to
     * 0:00 on the assumption a real decoder is about to open and the timer
     * (gui_player_update_progress()) will pick up the true position/duration
     * within its next tick -- see that function's own comment. That doesn't
     * happen here: deferred resume deliberately leaves the audio subsystem
     * untouched until the user presses Play (see install_saved_resume_
     * playlist()'s own comment), so audio_get_position_seconds()/
     * audio_get_duration_seconds() both read 0 the whole time this screen
     * sits paused, and the progress UI would otherwise falsely show the
     * track at its very beginning until Play is pressed. Probe the file
     * directly instead -- the same lightweight, decoder-open/close-only
     * call (no ALSA/output-device involvement) the Library screen's own
     * track-info display already uses for showing duration without
     * playing a track (see format_music_submenu_identity() in
     * gui_library.c) -- so the seek bar and elapsed time reflect the real
     * resume point immediately, not just after Play is pressed. */
    const char * path = playlist_path_at(index);
    audio_current_format_info_t probe;
    if (path && isfinite(start_seconds) && audio_probe_file_format(path, &probe) && probe.duration_seconds > 0.0) {
        double position = start_seconds;
        if (position < 0.0) position = 0.0;
        if (position > probe.duration_seconds) position = probe.duration_seconds;

        int32_t percent = (int32_t) ((position / probe.duration_seconds) * 100.0);
        displayed_progress_percent = percent;
        lv_slider_set_value(progress_slider, percent, LV_ANIM_OFF);

        displayed_position_second = (int) position;
        displayed_duration_second = (int) probe.duration_seconds;
        char time_str[24];
        gui_format_time(position, time_str, sizeof(time_str));
        lv_label_set_text(pos_label, time_str);
        gui_format_time(probe.duration_seconds, time_str, sizeof(time_str));
        lv_label_set_text(dur_label, time_str);
    }

    nav_push(player_screen);
}


int gui_player_get_playlist_count(void) {
    return playlist_count;
}

int gui_player_get_playlist_index(void) {
    return playlist_index;
}

bool gui_player_has_active_track(void) {
    return playlist_index >= 0 && playlist_index < playlist_count;
}

const char * gui_player_get_current_track_path(void) {
    if (playlist_index < 0 || playlist_index >= playlist_count) return "";
    return playlist_path_at(playlist_index);
}

const char * gui_player_get_track_path_at(int index) {
    return playlist_path_at(index);
}

int gui_player_get_queued_count(void) {
    return queued_pending_count;
}

const char * gui_player_get_queued_path_at(int offset) {
    if (offset < 0 || offset >= queued_pending_count || playlist_index < 0) return "";
    int pos = playlist_index + 1 + offset;
    if (pos >= playlist_count) return "";
    return playlist_path_at(pos);
}

void gui_player_queue_add(const char * path) {
    queue_add_song(path);
}

void gui_player_queue_remove_at(int offset) {
    queue_remove_song_at_offset(offset);
}

void gui_player_queue_clear(void) {
    queue_clear_pending();
}

uint64_t gui_player_queue_revision(void) { return queue_revision; }

bool gui_player_queue_snapshot(int ** order, int * count, int * current, uint64_t * revision) {
    *order = NULL; *count = playlist_count; *current = playlist_index; *revision = queue_revision;
    if (!playlist_count) return true;
    int * result = malloc((size_t) playlist_count * sizeof(int));
    if (!result) return false;
    if (current_settings.play_mode == PLAY_MODE_SHUFFLE) {
        ensure_shuffle_order_current();
        if (!shuffle_order) { free(result); return false; }
        memcpy(result, shuffle_order, (size_t) playlist_count * sizeof(int));
        *current = shuffle_pos;
    } else for (int i = 0; i < playlist_count; i++) result[i] = i;
    *order = result;
    return true;
}

/* Publish the displayed Up Next order only when the queue revision changes.
 * The remote listener owns its own copy, so it never reads playlist state
 * from its worker thread. This also seeds the endpoint the first time
 * Remote Control is enabled after boot/restoration. */
void gui_player_sync_remote_queue(void) {
    static uint64_t published_revision = 0;
    if (queue_revision == published_revision) return;
    int * order = NULL, count = 0, current = -1;
    uint64_t revision = 0;
    if (!gui_player_queue_snapshot(&order, &count, &current, &revision)) return;
    if (revision == published_revision) { free(order); return; }
    int upcoming = current >= 0 && current < count ? count - current - 1 : 0;
    const char ** paths = upcoming ? malloc(sizeof(*paths) * (size_t) upcoming) : NULL;
    if (upcoming && !paths) { free(order); return; }
    for (int i = 0; i < upcoming; i++) paths[i] = playlist_path_at(order[current + 1 + i]);
    remote_control_sync_queue(paths, upcoming, revision);
    free(paths);
    free(order);
    published_revision = revision;
}

bool gui_player_remote_queue_remove(int offset, uint64_t revision) {
    if (revision != queue_revision || offset < 0) return false;
    int * order = NULL, count = 0, current = -1;
    uint64_t actual = 0;
    if (!gui_player_queue_snapshot(&order, &count, &current, &actual)) return false;
    free(order);
    if (actual != revision || current < 0 || current + 1 + offset >= count) return false;
    return gui_player_queue_edit(revision, current + 1 + offset, -1);
}

bool gui_player_remote_queue_clear(uint64_t revision) {
    if (revision != queue_revision) return false;
    gui_player_queue_clear_all();
    return revision != queue_revision;
}

/* Materialize a displayed cycle before editing. The audio owns its decoder;
 * rearranging path slots does not restart the current track. */
static bool queue_materialize_order(void) {
    int * order = NULL, count, current;
    uint64_t revision;
    if (!gui_player_queue_snapshot(&order, &count, &current, &revision)) return false;
    if (!count) { free(order); return true; }
    if (playlist_lazy_sort_order) {
        /* Keep the database row mapping as the queue's storage.  Reorder the
         * row ids in displayed order and leave all paths unresolved; this
         * makes edit/select/clear work on huge lazy queues without copying the
         * library into RAM. */
        int * mapped = malloc((size_t) count * sizeof(*mapped));
        char ** mapped_paths = calloc((size_t) count, sizeof(*mapped_paths));
        if (!mapped || !mapped_paths) { free(mapped); free(mapped_paths); free(order); return false; }
        for (int i = 0; i < count; i++) {
            mapped[i] = playlist_lazy_sort_order[order[i]];
            mapped_paths[i] = playlist[order[i]];
        }
        free(order);
        free(playlist);
        free(playlist_lazy_sort_order);
        playlist = mapped_paths;
        playlist_lazy_sort_order = mapped;
        playlist_index = current;
        if (shuffle_order && shuffle_order_count == count) {
            for (int i = 0; i < count; i++) shuffle_order[i] = i;
            shuffle_pos = current;
        }
        free(pending_shuffle_order); pending_shuffle_order = NULL;
        queue_next_insert_index = queued_pending_count ? current + 1 + queued_pending_count : -1;
        return true;
    }
    char ** paths = calloc((size_t) count, sizeof(*paths));
    if (!paths) { free(order); return false; }
    for (int i = 0; i < count; i++) {
        const char * path = playlist_path_at(order[i]);
        if (!path || !path[0]) { free(paths); free(order); return false; }
        paths[i] = playlist[order[i]];
    }
    free(playlist); playlist = paths;
    free(playlist_lazy_sort_order); playlist_lazy_sort_order = NULL;
    playlist_index = current;
    if (shuffle_order && shuffle_order_count == count) {
        for (int i = 0; i < count; i++) shuffle_order[i] = i;
        shuffle_pos = current;
    }
    free(pending_shuffle_order); pending_shuffle_order = NULL;
    queue_next_insert_index = queued_pending_count ? current + 1 + queued_pending_count : -1;
    free(order);
    return true;
}

bool gui_player_queue_edit(uint64_t revision, int from, int to) {
    if (audio_consume_track_advanced()) gui_player_handle_auto_advance();
    if (revision != queue_revision) return false;
    int * order = NULL, count, current;
    uint64_t actual;
    if (!gui_player_queue_snapshot(&order, &count, &current, &actual)) return false;
    free(order);
    if (from <= current || from >= count || (to >= 0 && (to <= current || to >= count))) return false;
    if (!queue_materialize_order()) return false;
    if (playlist_lazy_sort_order) {
        int moved = playlist_lazy_sort_order[from];
        char * moved_path = playlist[from];
        if (to < 0) {
            /* Materialize left an identity bag of the old length. Remove
             * this row from it before the count changes, or the next arm
             * rebuilds a random bag. */
            queue_shuffle_remove(from);
            memmove(&playlist_lazy_sort_order[from], &playlist_lazy_sort_order[from + 1],
                    (size_t) (playlist_count - from - 1) * sizeof(*playlist_lazy_sort_order));
            playlist_count--;
            free(playlist[from]);
            memmove(&playlist[from], &playlist[from + 1], (size_t) (playlist_count - from) * sizeof(*playlist));
            char ** paths_shrunk = realloc(playlist, (size_t) playlist_count * sizeof(*playlist));
            if (paths_shrunk || playlist_count == 0) playlist = paths_shrunk;
            int * shrunk = realloc(playlist_lazy_sort_order, (size_t) playlist_count * sizeof(*playlist_lazy_sort_order));
            if (shrunk || playlist_count == 0) playlist_lazy_sort_order = shrunk;
            if (from <= playlist_index + queued_pending_count) queued_pending_count--;
        } else {
            if (from < to) memmove(&playlist_lazy_sort_order[from], &playlist_lazy_sort_order[from + 1],
                                    (size_t) (to - from) * sizeof(*playlist_lazy_sort_order));
            else memmove(&playlist_lazy_sort_order[to + 1], &playlist_lazy_sort_order[to],
                          (size_t) (from - to) * sizeof(*playlist_lazy_sort_order));
            playlist_lazy_sort_order[to] = moved;
            if (from < to) memmove(&playlist[from], &playlist[from + 1], (size_t) (to - from) * sizeof(*playlist));
            else memmove(&playlist[to + 1], &playlist[to], (size_t) (from - to) * sizeof(*playlist));
            playlist[to] = moved_path;
            int pending_end = playlist_index + queued_pending_count;
            if (from <= pending_end && to > pending_end) queued_pending_count = to - playlist_index;
            else if (from > pending_end && to <= pending_end) queued_pending_count++;
        }
        queue_next_insert_index = queued_pending_count ? playlist_index + 1 + queued_pending_count : -1;
        arm_next_track_for_audio(playlist_index);
        return true;
    }
    char * path = playlist[from];
    int pending_end = playlist_index + queued_pending_count;
    if (to < 0) {
        queue_shuffle_remove(from);
        memmove(playlist + from, playlist + from + 1, (size_t) (playlist_count - from - 1) * sizeof(*playlist));
        free(path); playlist_count--;
        if (from <= pending_end) queued_pending_count--;
    } else {
        if (from < to) memmove(playlist + from, playlist + from + 1, (size_t) (to - from) * sizeof(*playlist));
        else memmove(playlist + to + 1, playlist + to, (size_t) (from - to) * sizeof(*playlist));
        playlist[to] = path;
        /* Crossing the priority boundary explicitly schedules that prefix. */
        if (from <= pending_end && to > pending_end) queued_pending_count = to - playlist_index;
        else if (from > pending_end && to <= pending_end) queued_pending_count++;
    }
    queue_next_insert_index = queued_pending_count ? playlist_index + 1 + queued_pending_count : -1;
    arm_next_track_for_audio(playlist_index);
    return true;
}

bool gui_player_queue_select(uint64_t revision, int index) {
    if (audio_consume_track_advanced()) gui_player_handle_auto_advance();
    if (revision != queue_revision || index < 0 || index >= playlist_count) return false;
    if (!queue_materialize_order()) return false;
    if (index > playlist_index && index <= playlist_index + queued_pending_count)
        queued_pending_count -= index - playlist_index;
    else if (index != playlist_index) queued_pending_count = 0;
    queue_next_insert_index = queued_pending_count ? index + 1 + queued_pending_count : -1;
    if (shuffle_order) shuffle_pos = index;
    play_track_at(index);
    return true;
}

/* Clears the ENTIRE queue -- played history and upcoming alike -- leaving
 * only the single track actively playing, at position 0. Not a full stop:
 * there is no "go idle, nothing loaded" path anywhere in this player (every
 * screen that starts playback always hands it a full playlist to build
 * one), and clearing the queue was never a request to stop the track
 * already sounding out of the speaker, only to empty everything around it
 * -- the same convention other media players' own "Clear queue" actions
 * follow for the same reason. queue_materialize_order() above already
 * normalizes playlist[]/shuffle_order[] to plain identity order against
 * playlist_index, so the only bookkeeping left here is collapsing both
 * down to that one surviving element. */
void gui_player_queue_clear_all(void) {
    if (audio_consume_track_advanced()) gui_player_handle_auto_advance();
    if (!gui_player_has_active_track()) return;
    if (playlist_lazy_sort_order) {
        const char * current_path = playlist_path_at(playlist_index);
        if (!current_path || !current_path[0]) return;
        char * keep = strdup(current_path);
        if (!keep) return;
        char ** replacement = calloc(1, sizeof(*replacement));
        if (!replacement) { free(keep); return; }
        replacement[0] = keep;
        for (int i = 0; i < playlist_count; i++) free(playlist[i]);
        free(playlist); playlist = replacement;
        free(playlist_lazy_sort_order); playlist_lazy_sort_order = NULL;
        if (playlist_lazy_snapshot) metadata_db_snapshot_close(playlist_lazy_snapshot);
        playlist_lazy_snapshot = NULL;
        if (playlist_lazy_file_index) file_browser_index_close(playlist_lazy_file_index);
        playlist_lazy_file_index = NULL;
        playlist_count = 1; playlist_index = 0;
        free(shuffle_order); shuffle_order = NULL; shuffle_order_count = 0;
        free(pending_shuffle_order); pending_shuffle_order = NULL;
        queued_pending_count = 0; queue_next_insert_index = -1;
        arm_next_track_for_audio(playlist_index);
        return;
    }
    if (!queue_materialize_order()) return;
    for (int i = 0; i < playlist_count; i++) {
        if (i != playlist_index) free(playlist[i]);
    }
    playlist[0] = playlist[playlist_index];
    playlist_count = 1;
    playlist_index = 0;
    if (shuffle_order) {
        shuffle_order[0] = 0;
        shuffle_order_count = 1;
        shuffle_pos = 0;
    }
    queued_pending_count = 0; queue_next_insert_index = -1;
    arm_next_track_for_audio(playlist_index);
}

void gui_player_queue_play_next(const char * path) {
    if (audio_consume_track_advanced()) gui_player_handle_auto_advance();
    int old_count = playlist_count;
    int old_insert = queue_next_insert_index;
    queue_next_insert_index = playlist_index + 1;
    queue_add_song(path);
    if (playlist_count == old_count) queue_next_insert_index = old_insert;
    else queue_next_insert_index = playlist_index + 1 + queued_pending_count;
}

typedef struct {
    int *order, *ranks;
    char **explicit_paths;
    metadata_db_snapshot_t *snapshot;
    file_browser_index_t *file_index;
    atomic_bool *cancel;
    char resolved[PATH_MAX];
} queue_save_provider_ctx_t;
static bool queue_save_path_provider(void * context, int index, const char ** path) {
    queue_save_provider_ctx_t * ctx = context;
    if (ctx->cancel && atomic_load(ctx->cancel)) return false;
    int physical = ctx->order[index];
    const char * p = ctx->explicit_paths[physical];
    if (!p && ctx->snapshot &&
        !metadata_db_snapshot_path_at(ctx->snapshot, ctx->ranks[physical],
                                      ctx->resolved, sizeof(ctx->resolved))) return false;
    if (!p && ctx->file_index &&
        !file_browser_index_playable_path_at(ctx->file_index, (unsigned) ctx->ranks[physical],
                                             ctx->resolved, sizeof(ctx->resolved))) return false;
    if (!p && !ctx->snapshot && !ctx->file_index) return false;
    if (!p) p = ctx->resolved;
    if (!p || !p[0] || remote_track_path_is_remote(p) ||
        strncmp(p, SUBSONIC_STREAM_CACHE_DIR, strlen(SUBSONIC_STREAM_CACHE_DIR)) == 0) return false;
    *path = p;
    return true;
}

typedef struct queue_save_worker_state {
    int dirfd, count;
    char name[NAME_MAX], leaf[NAME_MAX];
    queue_save_provider_ctx_t provider;
    atomic_bool done;
    atomic_bool cancel;
    bool ok;
    pthread_t thread;
} queue_save_worker_state_t;
static queue_save_worker_state_t *queue_save_worker;
static bool queue_save_active(void) { return queue_save_worker != NULL; }

static void *queue_save_worker_func(void *arg) {
    queue_save_worker_state_t *w = arg;
    w->ok = playlist_files_write_new_stream_at(w->dirfd, w->name, w->count,
                                               queue_save_path_provider, &w->provider,
                                               w->leaf, sizeof(w->leaf));
    atomic_store(&w->done, true);
    return NULL;
}
static void queue_save_cancel(void) {
    if (!queue_save_worker) return;
    atomic_store(&queue_save_worker->cancel, true);
    pthread_join(queue_save_worker->thread, NULL);
    if (queue_save_worker->dirfd >= 0) close(queue_save_worker->dirfd);
    if (queue_save_worker->provider.snapshot) metadata_db_snapshot_close(queue_save_worker->provider.snapshot);
    if (queue_save_worker->provider.file_index) file_browser_index_close(queue_save_worker->provider.file_index);
    for (int i = 0; i < queue_save_worker->count; i++) free(queue_save_worker->provider.explicit_paths[i]);
    free(queue_save_worker->provider.explicit_paths); free(queue_save_worker->provider.ranks);
    free(queue_save_worker->provider.order); free(queue_save_worker); queue_save_worker = NULL;
}

bool gui_player_queue_save_as(const char * name) {
    if (queue_save_worker) return false;
    int * order = NULL, count, current;
    uint64_t revision;
    if (!gui_player_queue_snapshot(&order, &count, &current, &revision)) return false;
    queue_save_worker_state_t *w = calloc(1, sizeof(*w));
    if (!w) { free(order); return false; }
    if (!name || !name[0] || strlen(name) >= sizeof(w->name)) { free(w); free(order); return false; }
    w->count = count; snprintf(w->name, sizeof(w->name), "%s", name);
    atomic_store(&w->cancel, false);
    w->provider.cancel = &w->cancel;
    w->provider.order = order; w->provider.ranks = calloc((size_t) count, sizeof(int));
    w->provider.explicit_paths = calloc((size_t) count, sizeof(char *));
    if (!w->provider.ranks || !w->provider.explicit_paths) { free(w->provider.ranks); free(w->provider.explicit_paths); free(order); free(w); return false; }
    for (int i = 0; i < count; i++) {
        w->provider.ranks[i] = playlist_lazy_sort_order ? playlist_lazy_sort_order[i] : -1;
        if (playlist[i]) {
            w->provider.explicit_paths[i] = strdup(playlist[i]);
            if (!w->provider.explicit_paths[i]) {
                for (int j = 0; j < i; j++) free(w->provider.explicit_paths[j]);
                free(w->provider.explicit_paths); free(w->provider.ranks); free(order); free(w); return false;
            }
        }
    }
    if (playlist_lazy_snapshot) {
        w->provider.snapshot = metadata_db_snapshot_retain(playlist_lazy_snapshot);
        if (!w->provider.snapshot) { for (int i = 0; i < count; i++) free(w->provider.explicit_paths[i]); free(w->provider.explicit_paths); free(w->provider.ranks); free(order); free(w); return false; }
    }
    if (playlist_lazy_file_index && !file_browser_index_retain(playlist_lazy_file_index, &w->provider.file_index)) {
        if (w->provider.snapshot) metadata_db_snapshot_close(w->provider.snapshot);
        for (int i = 0; i < count; i++) free(w->provider.explicit_paths[i]); free(w->provider.explicit_paths); free(w->provider.ranks); free(order); free(w); return false;
    }
    bool has_sd = false;
    for (int i = 0; i < count; i++) if (playlist[i] && is_sd_card_path(playlist[i])) has_sd = true;
    if (has_sd && queue_sd_dirfd < 0) {
        if (w->provider.snapshot) metadata_db_snapshot_close(w->provider.snapshot);
        if (w->provider.file_index) file_browser_index_close(w->provider.file_index);
        for (int i = 0; i < count; i++) free(w->provider.explicit_paths[i]);
        free(w->provider.explicit_paths); free(w->provider.ranks); free(order); free(w);
        return false;
    }
    if (queue_sd_dirfd >= 0) {
        int parent = openat(queue_sd_dirfd, "..", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (parent >= 0) (void) mkdirat(parent, "Playlists", 0755);
        w->dirfd = parent >= 0 ? openat(parent, "Playlists", O_RDONLY | O_DIRECTORY | O_CLOEXEC) : -1;
        if (parent >= 0) close(parent);
    } else w->dirfd = open(PLAYLISTS_DIR, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (w->dirfd < 0 || pthread_create(&w->thread, NULL, queue_save_worker_func, w) != 0) {
        if (w->dirfd >= 0) close(w->dirfd);
        if (w->provider.snapshot) metadata_db_snapshot_close(w->provider.snapshot);
        if (w->provider.file_index) file_browser_index_close(w->provider.file_index);
        for (int i = 0; i < count; i++) free(w->provider.explicit_paths[i]);
        free(w->provider.explicit_paths); free(w->provider.ranks); free(order); free(w); return false;
    }
    queue_save_worker = w;
    return true;
}

bool gui_player_queue_save_as_poll(bool *done, bool *ok) {
    if (!queue_save_worker) { if (done) *done = true; if (ok) *ok = false; return false; }
    if (!atomic_load(&queue_save_worker->done)) { if (done) *done = false; return true; }
    queue_save_worker_state_t *w = queue_save_worker;
    pthread_join(w->thread, NULL);
    struct stat job_dir, current_dir;
    bool same_destination = fstat(w->dirfd, &job_dir) == 0 &&
                            stat(PLAYLISTS_DIR, &current_dir) == 0 &&
                            job_dir.st_dev == current_dir.st_dev && job_dir.st_ino == current_dir.st_ino;
    bool cache_bound = false;
    if (same_destination) {
        int parent = openat(w->dirfd, "..", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (parent >= 0) { cache_bound = path_cache_bind_directory_fd(parent); close(parent); }
    }
    bool result = w->ok && same_destination && cache_bound && metadata_db_storage_current();
    if (result) { char path[PATH_MAX]; snprintf(path, sizeof(path), "%s/%s", PLAYLISTS_DIR, w->leaf); metadata_db_playlist_insert_one(path); }
    if (done) *done = true;
    if (ok) *ok = result;
    if (w->provider.snapshot) metadata_db_snapshot_close(w->provider.snapshot);
    if (w->dirfd >= 0) close(w->dirfd);
    if (w->provider.file_index) file_browser_index_close(w->provider.file_index);
    for (int i = 0; i < w->count; i++) free(w->provider.explicit_paths[i]);
    free(w->provider.explicit_paths); free(w->provider.ranks); free(w->provider.order); free(w); queue_save_worker = NULL;
    return true;
}

void gui_player_play_at(int index) {
    play_track_at(index);
}

void gui_player_play_at_from(int index, double start_seconds) {
    play_track_at_from(index, start_seconds);
}

void gui_player_step_manual(int direction) {
    reset_decoder_failure_tracking();
    if (playlist_index < 0) return;
    int next_idx = compute_manual_step_index(playlist_index, direction);
    if (next_idx >= 0) play_track_at(next_idx);
    else plugin_manager_notify_queue_exhausted(direction);
}

lv_obj_t * gui_player_get_screen(void) {
    return player_screen;
}

lv_obj_t * gui_player_get_cover_img(void) {
    return cover_img;
}

const lv_image_dsc_t * gui_player_get_current_reflection_dsc(void) {
    return current_reflection_bytes ? &current_reflection_dsc : NULL;
}

bool gui_player_copy_cover_rgb565(int for_index, uint8_t * out, size_t out_size) {
    const size_t required = (size_t) COVER_ART_WIDTH * COVER_ART_HEIGHT * 2;
    if (!out || out_size < required || !current_cover_bytes || current_cover_for_index != for_index)
        return false;
    memcpy(out, current_cover_bytes, required);
    return true;
}

bool gui_player_is_seeking(void) {
    return user_seeking;
}


int32_t gui_player_get_volume_percent(void) {
    return volume_slider ? lv_slider_get_value(volume_slider) : (int32_t)(audio_get_volume() * 100.0f);
}

void gui_player_set_volume_percent(int32_t percent) {
    /* External/hardware writes are newer than any coalesced drag sample. */
    hw_volume_coalesce_cancel(&volume_popup_hv);
    if (volume_slider) lv_slider_set_value(volume_slider, percent, LV_ANIM_OFF);
}

bool gui_player_volume_is_being_adjusted(void) {
    return (volume_popup_track && lv_slider_is_dragged(volume_popup_track)) ||
           (volume_slider && lv_slider_is_dragged(volume_slider));
}

const char * gui_player_get_now_playing_title(void) {
    return song_title_label ? lv_label_get_text(song_title_label) : "";
}

const char * gui_player_get_now_playing_folder(void) {
    return artist_label ? lv_label_get_text(artist_label) : "";
}

const char * gui_player_get_now_playing_album(void) {
    return album_label ? lv_label_get_text(album_label) : "";
}

const char * gui_player_get_now_playing_genre(void) {
    return now_playing_genre;
}

int gui_player_get_now_playing_track_number(void) {
    return now_playing_track_number;
}


void gui_player_handle_auto_advance(void) {
    if (playlist_index < 0) return;
    reset_decoder_failure_tracking();
    int advanced_index = compute_auto_advance_index(playlist_index);
    if (advanced_index >= 0) {
        commit_auto_advance();
        on_track_auto_advanced(advanced_index);
    }
}

void gui_player_handle_track_finished(void) {
    if (playlist_index < 0) return;
    reset_decoder_failure_tracking();
    int finished_index = compute_auto_advance_index(playlist_index);
    if (finished_index >= 0) {
        commit_auto_advance();
        play_track_at_from_internal(finished_index, 0.0, false, false);
    } else {
        set_play_button_state(false);
    }
}

void gui_player_handle_playback_error_ex(audio_error_t err, uint64_t err_generation) {
    if (err == AUDIO_ERROR_NONE) return;
    if (playlist_index < 0) return;

    /* Stale-vs-race generation check: only reject an error OLDER than what
     * this function has last synced to -- playback_generation only ever
     * increases, so err_generation < current_playback_generation
     * unambiguously means this error is about a track already moved past.
     *
     * A plain != check is wrong here: the audio thread also bumps
     * playback_generation on its OWN via a crossfade/gapless promotion
     * (audio.c's track_advanced=true sites), which the GUI only learns
     * about by consuming audio_consume_track_advanced() -- and gui.c's
     * poll checks for an error FIRST, in a single if/else-if chain, so
     * that consume never runs this tick if an error is also pending. If
     * the newly-promoted track then fails within the same ~500ms poll
     * gap (easily possible -- the audio thread runs far more iterations
     * than that per poll), the error's generation is NEWER than what this
     * function has seen, not stale -- but a plain != check can't tell
     * "newer" from "older" and would silently drop a real failure while
     * audio_consume_error_ex() has already cleared it for good, leaving
     * the UI believing a track that never actually played is still
     * playing. Catch up to that pending promotion first (the same call
     * gui.c's own else-if branch would have made this tick, had the error
     * not taken priority) so the failure is attributed to the track it
     * actually happened on. */
    if (err_generation != 0 && current_playback_generation != 0 &&
        err_generation != current_playback_generation) {
        if (err_generation < current_playback_generation) return;
        if (!audio_consume_track_advanced()) return;
        gui_player_handle_auto_advance();
        if (current_playback_generation != err_generation) return;
    }

    if (err == AUDIO_ERROR_OUTPUT_FAILED) {
        /* Checkpoint confirmed position so pressing Play can retry cleanly */
        double pos = audio_get_resume_position_seconds();
        deferred_resume_position = (pos > 0.0) ? pos : 0.0;
        deferred_resume_pending = true;
        set_play_button_state(false);
        show_error_toast("Playback error: audio output failed");
        return;
    }

    if (err == AUDIO_ERROR_DECODER_FAILED) {
        /* Record the failed track's physical path */
        const char * failed_path = playlist_path_at(playlist_index);
        if (failed_path) record_failed_physical_path(failed_path);

        consecutive_decoder_failure_skips++;
        int max_skips = (playlist_count < 5) ? playlist_count : 5;
        if (consecutive_decoder_failure_skips >= max_skips) {
            set_play_button_state(false);
            deferred_resume_pending = false;
            deferred_resume_position = 0.0;
            reset_decoder_failure_tracking();
            show_error_toast("Playback stopped: too many unplayable tracks");
            return;
        }

        if ((play_mode_t) current_settings.play_mode == PLAY_MODE_SHUFFLE) {
            ensure_shuffle_order_current();
            if (!pending_shuffle_order) {
                pending_shuffle_order = malloc(sizeof(int) * (size_t) playlist_count);
                if (pending_shuffle_order) {
                    memcpy(pending_shuffle_order, shuffle_order, sizeof(int) * (size_t) playlist_count);
                    fisher_yates_shuffle(pending_shuffle_order, playlist_count);
                }
            }
        }

        failure_advance_plan_t plan = compute_decoder_failure_advance_plan(
            playlist_index,
            playlist_count,
            (int) current_settings.play_mode,
            queued_pending_count,
            shuffle_order,
            shuffle_pos,
            pending_shuffle_order,
            is_failed_physical_track_cb,
            NULL
        );

        if (plan.target_index < 0) {
            set_play_button_state(false);
            deferred_resume_pending = false;
            deferred_resume_position = 0.0;
            reset_decoder_failure_tracking();
            show_error_toast("Playback stopped: too many unplayable tracks");
            return;
        }

        commit_decoder_failure_advance_plan(&plan);
        deferred_resume_pending = false;
        deferred_resume_position = 0.0;
        show_error_toast("Skipped unplayable track");
        play_track_at_from_internal(plan.target_index, 0.0, false, false);
    }
}

void gui_player_handle_playback_error(audio_error_t err) {
    gui_player_handle_playback_error_ex(err, 0);
}


void gui_player_sync_topbar_visibility(lv_obj_t * screen) {
    if (player_dismiss_btn) {
        if (player_lyrics_open || player_lyrics_animating ||
            (current_settings.hide_player_topbar && screen == player_screen))
            lv_obj_add_flag(player_dismiss_btn, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_remove_flag(player_dismiss_btn, LV_OBJ_FLAG_HIDDEN);
    }
}

void gui_player_refresh_font_geometry(void) {
    player_layout_metadata_stack();
}


lv_obj_t * gui_player_get_dismiss_btn(void) {
    return player_dismiss_btn;
}

const lv_image_dsc_t * gui_player_get_current_cover_dsc(void) {
    if (!current_cover_bytes) return NULL;
    return &current_cover_dsc;
}
