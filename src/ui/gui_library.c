#include "gui_navigation.h"
#include "gui_library.h"
#include "gui_network.h"
#include "gui_shell.h"
#include "gui_lock_screen.h"
#include "gui_queue.h"
#include "assets.h"
#include "idle_shutdown.h"
#include "backlight.h"
#include "usb_mode_control.h"
#include "gui_player.h"
#include "frosted_glass.h"
#include "src/draw/lv_draw_buf.h"
#include "src/draw/snapshot/lv_snapshot.h"

extern void on_file_browser_selected(char ** new_playlist, int count, int selected_index);
typedef struct file_browser_index file_browser_index_t;
extern void on_file_browser_index_selected(file_browser_index_t *index, unsigned playable_count,
                                           unsigned selected_playable);
extern void open_queue_screen(void);
extern void nav_remove_stack_slot(int depth);
extern void mount_sd_card_if_needed(void);
extern void library_scan_once(void);
extern void library_load_from_cache_only(void);
void refresh_artist_albums_now_playing_indicator(void);
#include "gui.h"
#include "gui_theme.h"
#include "gui_notifications.h"
#include "gui_settings.h"
#include "gui_text_input.h"
#include "gui_books.h"
#include "fallback_font.h"
#include "plugin_manager.h"
#include "screen_builders.h"
#include "metadata.h"
#include "metadata_db.h"
#include "metadata_refresh_targets.h"
#include "storage_migration.h"
#include "sd_fsck_run.h"
#include "tagcache.h"
#include "file_browser.h"
#include "sd_card_identity.h"
#include "playlist_files.h"
#include "cue_parser.h"
#include "cover_decode.h"
#include "albumart.h"
#include "artwork_coordinator.h"
#include "device_config.h"
#include "settings.h"
#include "audio.h"
#include "track_probe.h"
#include "subprocess.h"
#include "db_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <limits.h>
#include <ctype.h>
#include <time.h>
#include <stdarg.h>
#include <errno.h>
#include <sched.h>
/* SCHED_BATCH below is glibc's own <bits/sched.h> policy constant, gated
 * behind _GNU_SOURCE there -- this file deliberately doesn't define that
 * (see search_matches()'s own comment on strcasestr() for why). The raw
 * kernel UAPI header defines the same numeric policy constants ungated on
 * both glibc and musl (confirmed in both toolchains' linux/sched.h), so
 * this alone is enough on host and target alike without touching feature-
 * test macros anywhere in this file. */
#include <linux/sched.h>
#include <sys/resource.h>

#ifdef HOST_BUILD
  #define MUSIC_ROOT_DIR "./music"
#else
  #define MUSIC_ROOT_DIR "/data/mnt/sd_0"
#endif

#define PLAYLISTS_DIR MUSIC_ROOT_DIR "/Playlists"
#define COMPACT_LIST_PAGE_CACHE_SIZE 64
/* STATUS_BAR_CLEARANCE is provided by screen_builders.h. */
#define EXTERNAL_COVER_MAX_BYTES (4U * 1024U * 1024U)
#define ALBUM_THUMB_GEN_BATCH 16
#define ALBUM_THUMB_GEN_INTER_ALBUM_US 100000 /* 100 ms yield between albums */

typedef enum {
    THUMBNAIL_KEY_ALBUM,
    THUMBNAIL_KEY_ARTIST
} thumbnail_key_kind_t;

typedef struct {
    thumbnail_key_kind_t key_kind;
    metadata_db_group_kind_t artist_group_kind;
} thumbnail_decorator_context_t;

static const thumbnail_decorator_context_t album_thumbnail_context = {
    .key_kind = THUMBNAIL_KEY_ALBUM,
    .artist_group_kind = METADATA_DB_GROUP_ARTIST
};
static const thumbnail_decorator_context_t artist_thumbnail_context = {
    .key_kind = THUMBNAIL_KEY_ARTIST,
    .artist_group_kind = METADATA_DB_GROUP_ARTIST
};
static const thumbnail_decorator_context_t album_artist_thumbnail_context = {
    .key_kind = THUMBNAIL_KEY_ARTIST,
    .artist_group_kind = METADATA_DB_GROUP_ALBUM_ARTIST
};

static lv_obj_t * album_thumbnail_active_list = NULL;
extern bool library_rescan_active;
static atomic_int album_thumbnail_view_generation = 0;
static atomic_int album_thumbnail_cache_epoch = 0;
static atomic_int album_thumbnail_queue_count;

static void album_thumbnail_cache_epoch_advance(void) {
    atomic_fetch_add(&album_thumbnail_cache_epoch, 1);
}
static bool group_songs_source_is_album = false;
typedef enum {
    GROUP_SONGS_REFRESH_NONE,
    GROUP_SONGS_REFRESH_FAVORITES,
    GROUP_SONGS_REFRESH_MOST_PLAYED,
    GROUP_SONGS_REFRESH_M3U,
    GROUP_SONGS_REFRESH_ALBUM,
    GROUP_SONGS_REFRESH_ARTIST
} group_songs_refresh_source_t;
static group_songs_refresh_source_t group_songs_refresh_source;
static char group_songs_refresh_album[128];
static char group_songs_refresh_album_artist[128];
static char group_songs_refresh_artist_filter[128];
static char group_songs_refresh_artist_name[128];
static metadata_db_group_kind_t group_songs_refresh_artist_kind;
static char group_songs_refresh_m3u_path[PATH_MAX];
static gui_busy_handle_t library_rescan_token = 0;
static gui_busy_handle_t sd_format_token = 0;
static atomic_int library_scan_progress_total = 0;
static atomic_int library_scan_progress_done = 0;
/* Directory entries visited so far by the discovery walk (before _total is known). */
static atomic_int library_scan_walk_items = 0;
/* Which step the busy screen describes; set by the scan worker. */
enum { LIBRARY_SCAN_PHASE_WALK, LIBRARY_SCAN_PHASE_TAGS, LIBRARY_SCAN_PHASE_SAVE };
static atomic_int library_scan_phase = LIBRARY_SCAN_PHASE_WALK;
static atomic_bool library_scan_cancel_requested = false;
void gui_library_cancel_scan(void);

static uint64_t albums_page_open_requested_ms;
static uint64_t album_lazy_job_started_ms;
static unsigned album_lazy_queued;
static unsigned album_lazy_completed;
static unsigned album_lazy_with_art;
static unsigned album_lazy_stale;

static void album_thumbnail_screen_loaded_cb(lv_event_t * e);
static void album_thumbnail_screen_unloaded_cb(lv_event_t * e);
static void album_thumbnail_scroll_cb(lv_event_t * e);
static void thumbnail_row_decorator(lv_obj_t * list, lv_obj_t * row, lv_obj_t * image,
                                    int logical_index, int pool_slot, int64_t representative_song_id,
                                    uint64_t artwork_key, const char * artwork_name, void * ctx);
static void refresh_thumbnail_list_visible(lv_obj_t * list);
static void refresh_all_thumbnail_lists(void);
static bool release_thumbnail_dsc(const lv_image_dsc_t * dsc);
static void refresh_group_song_thumbnails(void);
static int artists_fetch_page(void * ctx, int offset, int count, compact_list_page_row_t out_rows[]);
static int albums_fetch_page(void * ctx, int offset, int count, compact_list_page_row_t out_rows[]);
static int album_artists_fetch_page(void * ctx, int offset, int count, compact_list_page_row_t out_rows[]);
static int all_songs_fetch_page(void * ctx, int offset, int count, compact_list_page_row_t out_rows[]);
static void music_files_tile_cb(lv_event_t * e);
static void album_more_click_cb(int index);
static void album_row_long_press_cb(int index);
static void artist_collection_more_click_cb(int index);
static void artist_collection_long_press_cb(int index);
static void album_artist_collection_more_click_cb(int index);
static void album_artist_collection_long_press_cb(int index);
static void artist_album_more_click_cb(int index);
static void build_collection_menus(void);

/* Screen pointers owned by this module */
static lv_obj_t * music_screen = NULL;
static lv_obj_t * files_screen = NULL;
static lv_obj_t * files_search_list = NULL;

static lv_obj_t * all_songs_screen = NULL;
static lv_obj_t * all_songs_list = NULL;

static lv_obj_t * recently_added_screen = NULL;
static lv_obj_t * recently_added_list = NULL;

static lv_obj_t * artists_screen = NULL;
static lv_obj_t * artists_list = NULL;

static lv_obj_t * albums_screen = NULL;
static lv_obj_t * albums_list = NULL;

static lv_obj_t * album_artist_screen = NULL;
static lv_obj_t * album_artist_list = NULL;

static lv_obj_t * group_songs_screen = NULL;
static lv_obj_t * group_songs_list = NULL;
static lv_obj_t * group_songs_title_label = NULL;

static lv_obj_t * artist_albums_screen = NULL;
static lv_obj_t * artist_albums_list = NULL;
static lv_obj_t * artist_albums_title_label = NULL;

static lv_obj_t * playlists_screen = NULL;
static lv_obj_t * playlists_list = NULL;
static lv_obj_t * playlists_refresh_icon = NULL;
static bool playlists_manual_refresh = false;

static lv_obj_t * cue_tracks_screen = NULL;
static lv_obj_t * cue_tracks_list = NULL;
static lv_obj_t * cue_tracks_title_label = NULL;

static lv_obj_t * add_to_playlist_screen = NULL;
static lv_obj_t * add_to_playlist_list = NULL;

static lv_obj_t * collection_menu_popup = NULL;
static lv_obj_t * collection_menu_backdrop = NULL;
static lv_obj_t * album_collection_menu_popup = NULL;
static lv_obj_t * album_collection_menu_backdrop = NULL;
static bool collection_menu_is_album;
static char collection_menu_name[128];
static char collection_menu_album_artist[128];
static metadata_db_group_kind_t collection_menu_artist_kind;
static int collection_menu_song_count;
/* Credited artist the album menu was opened under, empty when it was not
 * opened from an Artists drill-down. */
static char collection_menu_artist[128];

/* Externs to player/queue and global state */
extern player_settings_t current_settings;
extern void nav_push(lv_obj_t * screen);
extern void nav_pop(void);
extern void nav_reset_to_home(void);
extern void finalize_screen_navigation(lv_obj_t * screen);
extern void on_file_selected(char ** new_playlist, int count, int selected_index);
extern const char * playlist_path_at(int index);
extern void enable_gesture_bubble_recursive(lv_obj_t * obj);
extern void register_static_snapshot(int index, lv_obj_t * screen);
extern void unregister_static_snapshot(lv_obj_t * screen);




/* Which song this screen adds to whatever playlist is picked -- set by
 * open_add_to_playlist_for() right before nav_push(), not always the
 * currently-playing track anymore (see that function's own comment: the
 * song long-press context menu reaches this same screen for an arbitrary
 * row, not just the player's own "..." menu). */
static char add_to_playlist_target_path[600] = ""; /* 600, matching song_row_t.path's own bound (metadata_db.h) */
static void populate_playlists_screen(void);
static bool standalone_playlist_create;

static void new_playlist_name_done_cb(const char * text, void * user_data) {
    (void) user_data;
    if (text[0] == '\0') return;

    char created_path[512];
    bool ok = playlist_files_create(PLAYLISTS_DIR, text, add_to_playlist_target_path, created_path, sizeof(created_path));
    if (ok) metadata_db_playlist_insert_one(created_path);
    show_error_toast(ok ? "Playlist created" : "Failed to create playlist");
    if (!ok) return;
    if (standalone_playlist_create) populate_playlists_screen();
    else nav_pop();
}

static void new_playlist_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    standalone_playlist_create = lv_event_get_user_data(e) != NULL;
    if (standalone_playlist_create) add_to_playlist_target_path[0] = '\0';
    show_text_entry("Playlist Name", "", false, false, new_playlist_name_done_cb, NULL);
}

static void existing_playlist_row_cb(lv_event_t * e) {
    const char * path = (const char *) lv_event_get_user_data(e);
    if (add_to_playlist_target_path[0] == '\0') return;

    /* Check for duplicates before appending to avoid adding the same track twice. */
    if (playlist_files_contains(path, add_to_playlist_target_path)) {
        show_error_toast("Song already added");
        nav_pop();
        return;
    }

    bool ok = playlist_files_append(path, add_to_playlist_target_path);
    show_error_toast(ok ? "Added to playlist" : "Failed to add to playlist");
    nav_pop();
}

static void playlist_picker_path_free_cb(lv_event_t * e) {
    free(lv_event_get_user_data(e));
}

static void populate_add_to_playlist_screen(void) {
    lv_obj_clean(add_to_playlist_list);

    lv_obj_t * new_row = lv_obj_create(add_to_playlist_list);
    lv_obj_set_size(new_row, LIST_ROW_WIDTH, LIST_ROW_HEIGHT);
    lv_obj_set_style_radius(new_row, LIST_ROW_RADIUS, 0);
    lv_obj_set_style_bg_color(new_row, LIST_ROW_BG_COLOR, 0);
    lv_obj_set_style_bg_opa(new_row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(new_row, 0, 0);
    lv_obj_remove_flag(new_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(new_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(new_row, new_playlist_row_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * new_label = lv_label_create(new_row);
    lv_label_set_text(new_label, "+ New Playlist");
    lv_obj_set_style_text_color(new_label, accent_lv_color(), 0);
    lv_obj_set_style_text_font(new_label, &LIST_ROW_FONT, 0);
    lv_obj_align(new_label, LV_ALIGN_LEFT_MID, LIST_ROW_LABEL_INSET, 0);

    char ** paths;
    int count;
    /* Persistent cache read, not a live scan -- same reasoning as
     * populate_playlists_screen()'s own comment; this screen is reachable
     * from the player's "more" menu, so it was paying the same ~5s SD-card
     * walk on every open too. */
    metadata_db_load_all_playlists(&paths, &count);
    if (count == 0) return;

    for (int i = 0; i < count; i++) {
        lv_obj_t * row = lv_obj_create(add_to_playlist_list);
        lv_obj_set_size(row, LIST_ROW_WIDTH, LIST_ROW_HEIGHT);
        lv_obj_set_style_radius(row, LIST_ROW_RADIUS, 0);
        lv_obj_set_style_bg_color(row, LIST_ROW_BG_COLOR, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, existing_playlist_row_cb, LV_EVENT_CLICKED, paths[i]);
        lv_obj_add_event_cb(row, playlist_picker_path_free_cb, LV_EVENT_DELETE, paths[i]);

        lv_obj_t * label = lv_label_create(row);
        lv_label_set_text(label, basename_of(paths[i]));
        lv_obj_add_style(label, &style_theme_text_primary, 0);
        lv_obj_set_style_text_font(label, &LIST_ROW_FONT, 0);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, LIST_ROW_LABEL_INSET, 0);
    }
    free(paths);
}

static lv_obj_t * build_add_to_playlist_screen(void) {
    lv_obj_t * title_label;
    return build_subsonic_list_screen("Add to Playlist", &title_label, &add_to_playlist_list);
}

/* Shared entry point into the Add to Playlist picker -- the player's own
 * "..." menu (more_menu_add_to_playlist_cb() below) and the song long-press
 * context menu (song_context_menu_add_to_playlist_cb(), defined with the
 * rest of that popup further down) both reach this screen for whatever
 * song path they have, not necessarily the currently-playing one. */
void open_add_to_playlist_for(const char * path) {
    snprintf(add_to_playlist_target_path, sizeof(add_to_playlist_target_path), "%s", path);
    populate_add_to_playlist_screen();
    nav_push(add_to_playlist_screen);
}

void on_cue_file_selected(const char * cue_path);

static lv_obj_t * build_files_screen(void) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_add_style(scr, &style_theme_screen_bg, 0);

    build_screen_header(scr, "Files", generic_back_cb, NULL, NULL);

    file_browser_init(scr, MUSIC_ROOT_DIR, on_file_browser_selected, on_cue_file_selected);
    file_browser_set_index_select_cb(on_file_browser_index_selected);

    finalize_screen_navigation(scr);
    return scr;
}

/* One song within a group_songs_screen listing (Artist/Album Artist's
 * albums, one album, Favorites, Most Played, a user .m3u playlist) -- an
 * owned path and display title resolved via targeted database query. */
/* group_song_entry_t defined in gui_library.h */

void free_group_song_entries(group_song_entry_t * entries, int count) {
    if (!entries) return;
    for (int i = 0; i < count; i++) {
        free(entries[i].path);
        free(entries[i].title);
    }
    free(entries);
}

bool copy_group_song_entries(group_song_entry_t ** out, const group_song_entry_t * entries, int count) {
    *out = NULL;
    if (count <= 0) return true;
    group_song_entry_t * copy = calloc((size_t) count, sizeof(*copy));
    if (!copy) return false;
    for (int i = 0; i < count; i++) {
        copy[i].path = strdup(entries[i].path ? entries[i].path : "");
        copy[i].title = strdup(entries[i].title ? entries[i].title : "");
        copy[i].disc_number = entries[i].disc_number;
        copy[i].show_disc_header = entries[i].show_disc_header;
        copy[i].song_id = entries[i].song_id;
        copy[i].artwork_key = entries[i].artwork_key;
        if (!copy[i].path || !copy[i].title) {
            free_group_song_entries(copy, count);
            return false;
        }
    }
    *out = copy;
    return true;
}

/* search_remap_index */
int search_remap_index(search_binding_id_t binding_id, int display_index);

/* Paged All Songs -- see build_all_songs_screen()'s own comment. offset is
 * a position in the DB's own title-sorted order (metadata_db_get_songs_
 * filtered_page() with every filter NULL), the exact same order playlist_
 * lazy_sort_order/on_file_selected_lazy_all_songs() already assume, so a
 * row tapped here and the playback queue it starts always agree on what
 * comes next. Heap-allocated, not a stack array -- see remote_control.c's
 * own build_library_json() comment on why a COMPACT_LIST_PAGE_CACHE_SIZE-
 * sized array of song_row_t (~1.25KB each) has no business on a stack. */
static const char * song_quality_asset_for_path(const char * path) {
    const char * ext = strrchr(path ? path : "", '.');
    if (!ext) return "";
    if (!strcasecmp(ext, ".dsf") || !strcasecmp(ext, ".dff"))
        return "touch_list/quality_hr.png";
    if (!strcasecmp(ext, ".flac") || !strcasecmp(ext, ".wav") || !strcasecmp(ext, ".aif") ||
        !strcasecmp(ext, ".aiff") || !strcasecmp(ext, ".ape") || !strcasecmp(ext, ".alac"))
        return "touch_list/quality_high.png";
    return "touch_list/quality_nomal.png";
}

static const char * library_codec_name(audio_codec_t codec) {
    switch (codec) {
        case AUDIO_CODEC_FLAC: return "FLAC";
        case AUDIO_CODEC_MP3: return "MP3";
        case AUDIO_CODEC_PCM: return "PCM";
        case AUDIO_CODEC_DSD: return "DSD";
        case AUDIO_CODEC_AAC: return "AAC";
        case AUDIO_CODEC_ALAC: return "ALAC";
        case AUDIO_CODEC_APE: return "APE";
        case AUDIO_CODEC_WMA: return "WMA";
        case AUDIO_CODEC_OPUS: return "Opus";
        case AUDIO_CODEC_VORBIS: return "Vorbis";
        case AUDIO_CODEC_UNKNOWN: break;
    }
    return "Audio";
}

static void format_music_submenu_identity(const song_row_t * song, char * out, size_t out_size) {
    char title[128];
    metadata_db_song_display_title(song, title, sizeof(title));
    /* Building a list must not open every track's decoder. The displayed
     * page is enriched by gui_library_poll_track_probes() after it opens. */
    const char * ext = strrchr(song->path, '.');
    snprintf(out, out_size, "%s\n%s", title, ext && ext[1] ? ext + 1 : "Audio");
}

static int effective_disc_number(const song_row_t * song) {
    return song && song->tags.disc_number > 0 ? song->tags.disc_number : 1;
}

static bool album_songs_are_multi_disc(const song_row_t * songs, int count) {
    if (!songs || count <= 1) return false;
    int first = effective_disc_number(&songs[0]);
    for (int i = 1; i < count; i++) {
        if (effective_disc_number(&songs[i]) != first) return true;
    }
    return false;
}

static int cmp_song_disc_track(const void * a, const void * b) {
    const song_row_t * sa = (const song_row_t *) a;
    const song_row_t * sb = (const song_row_t *) b;
    int da = effective_disc_number(sa), db = effective_disc_number(sb);
    if (da != db) return da < db ? -1 : 1;
    int ta = sa->tags.track_number, tb = sb->tags.track_number;
    if (ta > 0 || tb > 0) {
        if (ta <= 0) return 1;
        if (tb <= 0) return -1;
        if (ta != tb) return ta < tb ? -1 : 1;
    }
    return strcasecmp(sa->path, sb->path);
}

static void set_album_disc_headers(const song_row_t * songs, int count,
                                   bool multi_disc, group_song_entry_t * entries) {
    int previous_disc = effective_disc_number(&songs[0]);
    for (int i = 0; i < count; i++) {
        entries[i].disc_number = effective_disc_number(&songs[i]);
        entries[i].show_disc_header = multi_disc && (i == 0 || entries[i].disc_number != previous_disc);
        previous_disc = entries[i].disc_number;
    }
}

void gui_library_format_song_identity(const song_row_t * row,
                                      char * title, size_t title_size,
                                      char * subtitle, size_t subtitle_size) {
    char artist[121], album[121];
    metadata_db_song_display_title(row, title, title_size);
    utf8_truncate_safe(artist, row->tags.artist[0] ? row->tags.artist : "Unknown artist", sizeof(artist));
    utf8_truncate_safe(album, row->tags.album[0] ? row->tags.album : "Unknown album", sizeof(album));
    snprintf(subtitle, subtitle_size, "%s · %s", artist, album);
}

static uint64_t song_album_thumbnail_key(const song_row_t * row) {
    albumart_info_t info = {0};
    albumart_info_from_song_row(row, &info);
    if (info.album[0] && (info.albumartist[0] || info.artist[0]))
        return albumart_thumbnail_key(&info);

    /* Untagged tracks cannot use albumart.c's persistent artist/album name
     * key. Group them by their containing album directory in RAM, skipping a
     * Rockbox-style disc folder when the track is stored one level below it. */
    char directory[sizeof(row->path)];
    snprintf(directory, sizeof(directory), "%s", row->path);
    char * slash = strrchr(directory, '/');
    if (!slash) return albumart_artist_thumbnail_key(row->path);
    *slash = '\0';
    char * base = strrchr(directory, '/');
    base = base ? base + 1 : directory;
    if (albumart_is_disc_folder(base)) {
        slash = strrchr(directory, '/');
        if (slash) *slash = '\0';
    }
    return albumart_artist_thumbnail_key(directory);
}

static void fill_song_page_visual(compact_list_page_row_t * out, const song_row_t * row) {
    gui_library_format_song_identity(row, out->label, sizeof(out->label),
                                     out->subtitle, sizeof(out->subtitle));
    out->identity = row->id;
    out->artwork_key = song_album_thumbnail_key(row);
    snprintf(out->trailing_asset, sizeof(out->trailing_asset), "%s", song_quality_asset_for_path(row->path));
}

static int all_songs_fetch_page(void * ctx, int offset, int count, compact_list_page_row_t out_rows[]) {
    (void) ctx;
    song_row_t * rows = malloc(sizeof(song_row_t) * (size_t) count);
    int n = rows ? metadata_db_get_songs_filtered_page(NULL, NULL, NULL, NULL, NULL, offset, count, rows) : 0;
    for (int i = 0; i < n; i++) fill_song_page_visual(&out_rows[i], &rows[i]);
    free(rows);
    return n;
}

/* Resolves one All-Songs display position to a real path -- for the
 * long-press context menu, which needs a path directly (unlike the click
 * handler, which goes through play_track_at_from() -> playlist_path_at()'s
 * own resolution instead). false if display_index is out of range (e.g.
 * raced a rescan shrinking the library between the tap and this lookup --
 * same stale-reference tolerance as playlist_path_at()'s own comment). */
static bool all_songs_resolve_path_at(int display_index, char * out, size_t out_size) {
    song_row_t row;
    if (metadata_db_get_songs_filtered_page(NULL, NULL, NULL, NULL, NULL, display_index, 1, &row) != 1) return false;
    snprintf(out, out_size, "%s", row.path);
    return true;
}

static void all_songs_row_click_cb(int display_index) {
    if (library_rescan_active) return;
    display_index = search_remap_index(SEARCH_BINDING_ALL_SONGS, display_index);
    /* on_file_selected_lazy_all_songs() builds the queue lazily, identity-
     * mapped into the DB's own title-sorted order (the same display order
     * this screen's paged provider uses) instead of eagerly strdup'ing
     * every one of the library's paths on every tap -- see its own
     * comment. Tap-to-play from All Songs is DB-driven end to end now. */
    set_player_source_all_songs(display_index);
    on_file_selected_lazy_all_songs(display_index);
}

static void all_songs_row_long_press_cb(int display_index) {
    if (library_rescan_active) return;
    display_index = search_remap_index(SEARCH_BINDING_ALL_SONGS, display_index);
    char path[600];
    if (all_songs_resolve_path_at(display_index, path, sizeof(path))) open_song_context_menu(path);
}

/* Builds the "All Songs" screen with a paged provider
 * (compact_list_set_paged_provider()) so items are loaded incrementally
 * on scroll rather than materializing the full library in memory. */
static lv_obj_t * build_all_songs_screen(void) {
    lv_obj_t * scr = build_compact_list_screen("All Songs", generic_back_cb, NULL, 0, all_songs_row_click_cb,
                                                all_songs_row_long_press_cb, &all_songs_list, NULL,
                                                LIST_ROW_WIDTH_WIDE, true, accent_lv_color());
    compact_list_set_row_height(all_songs_list, MUSIC_LIST_ROW_HEIGHT);
    compact_list_set_paged_provider(all_songs_list, all_songs_fetch_page, NULL, (int) metadata_db_get_song_count());
    compact_list_set_row_decorator(all_songs_list, thumbnail_row_decorator,
                                    (void *) &album_thumbnail_context);
    lv_obj_add_event_cb(scr, album_thumbnail_screen_loaded_cb, LV_EVENT_SCREEN_LOADED, all_songs_list);
    lv_obj_add_event_cb(scr, album_thumbnail_screen_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, all_songs_list);
    lv_obj_add_event_cb(all_songs_list, album_thumbnail_scroll_cb, LV_EVENT_SCROLL_BEGIN, NULL);
    lv_obj_add_event_cb(all_songs_list, album_thumbnail_scroll_cb, LV_EVENT_SCROLL_END, NULL);
    finalize_screen_navigation(scr);
    return scr;
}

/* Recently Added -- same paged compact_list architecture as All Songs above
 * (see build_all_songs_screen()'s own comment on why: a bounded page at a
 * time regardless of library size, not the show_group_songs()/group_song_
 * entry_t path Favorites/Most Played use, which loads its whole, small,
 * deliberately-capped result set into RAM up front -- Recently Added has no
 * such cap, it's every song in the library just reordered by first_seen, so
 * it needs the same scale guarantee All Songs has). Only difference from
 * All Songs is the ORDER BY (first_seen DESC, rowid DESC via metadata_db_
 * get_songs_page_by_recency() instead of title via metadata_db_get_songs_
 * filtered_page()) -- no search/A-Z index wired up here, unlike All Songs,
 * since neither makes as much sense against a recency-ordered list. */
static void playlist_start_options(bool recent);
static void format_song_identity(const song_row_t * row, char * out, size_t size) {
    char title[128], subtitle[256];
    gui_library_format_song_identity(row, title, sizeof(title), subtitle, sizeof(subtitle));
    snprintf(out, size, "%s\n%s", title, subtitle);
}

static int recently_added_fetch_page(void * ctx, int offset, int count, compact_list_page_row_t out_rows[]) {
    (void) ctx;
    int prefix = 0;
    if (offset == 0 && count > 0) {
        memset(&out_rows[0], 0, sizeof(out_rows[0]));
        snprintf(out_rows[0].label, sizeof(out_rows[0].label), "Play All");
        out_rows[0].is_action = true;
        snprintf(out_rows[0].subtitle, sizeof(out_rows[0].subtitle), "Choose sequential or shuffle playback");
        prefix = 1; count--; offset++;
    }
    if (count <= 0) return prefix;
    song_row_t * rows = malloc(sizeof(song_row_t) * (size_t) count);
    int n = rows ? metadata_db_get_songs_page_by_recency(offset - 1, count, rows) : 0;
    for (int i = 0; i < n; i++) {
        fill_song_page_visual(&out_rows[prefix + i], &rows[i]);
    }
    free(rows);
    return n + prefix;
}

/* Same role as all_songs_resolve_path_at() above, against the recency order. */
static bool recently_added_resolve_path_at(int display_index, char * out, size_t out_size) {
    song_row_t row;
    if (metadata_db_get_songs_page_by_recency(display_index, 1, &row) != 1) return false;
    snprintf(out, out_size, "%s", row.path);
    return true;
}

static void recently_added_row_click_cb(int display_index) {
    if (library_rescan_active) return;
    if (display_index == 0) { playlist_start_options(true); return; }
    display_index--;
    /* on_file_selected_lazy_recently_added() builds the queue lazily,
     * identity-mapped into the DB's own first_seen-DESC order (the same
     * display order this screen's paged provider uses) -- see its own
     * comment. Tap-to-play here is DB-driven end to end, same as All Songs. */
    set_player_source_recently_added(display_index);
    on_file_selected_lazy_recently_added(display_index);
}

static void recently_added_row_long_press_cb(int display_index) {
    if (library_rescan_active) return;
    if (display_index <= 0) return;
    display_index--;
    char path[600];
    if (recently_added_resolve_path_at(display_index, path, sizeof(path))) open_song_context_menu(path);
}

static lv_obj_t * build_recently_added_screen(void) {
    lv_obj_t * scr = build_compact_list_screen("Recently Added", generic_back_cb, NULL, 0, recently_added_row_click_cb,
                                                recently_added_row_long_press_cb, &recently_added_list, NULL,
                                                LIST_ROW_WIDTH_WIDE, true, accent_lv_color());
    compact_list_set_row_height(recently_added_list, MUSIC_LIST_ROW_HEIGHT);
    compact_list_set_paged_provider(recently_added_list, recently_added_fetch_page, NULL,
                                     (int) metadata_db_get_song_count() + 1);
    compact_list_set_row_decorator(recently_added_list, thumbnail_row_decorator,
                                    (void *) &album_thumbnail_context);
    lv_obj_add_event_cb(scr, album_thumbnail_screen_loaded_cb, LV_EVENT_SCREEN_LOADED, recently_added_list);
    lv_obj_add_event_cb(scr, album_thumbnail_screen_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, recently_added_list);
    lv_obj_add_event_cb(recently_added_list, album_thumbnail_scroll_cb, LV_EVENT_SCROLL_BEGIN, NULL);
    lv_obj_add_event_cb(recently_added_list, album_thumbnail_scroll_cb, LV_EVENT_SCROLL_END, NULL);
    finalize_screen_navigation(scr);
    return scr;
}

static void all_songs_tile_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(all_songs_screen);
}

/* Shared drill-down target for both Artists and Albums: one persistent
 * screen whose row list is rebuilt each time (same "one object, rebuilt on
 * demand" approach file_browser.c uses for its own directory listing)
 * rather than pre-building a screen per artist/album -- a real library can
 * have hundreds of those, most never opened in a given session. */
static lv_obj_t * group_songs_edit_btn;
static group_song_entry_t * group_songs_entries; /* owned -- see set_group_songs_entries() */
static int group_songs_count;

/* Frees the previous group_songs_entries (if any) and replaces it with an
 * owned copy of entries[0..count) -- every show_group_songs_editable() call
 * and every "Removed from playlist" refresh goes through this single choke
 * point so the ownership rule (group_songs_entries is always either NULL or
 * a malloc'd copy this screen owns outright) never has to be re-derived at
 * each call site. Caller's own entries[] can be freed immediately after. */
static void set_group_songs_entries(const group_song_entry_t * entries, int count) {
    free_group_song_entries(group_songs_entries, group_songs_count);
    group_songs_entries = NULL;
    group_songs_count = copy_group_song_entries(&group_songs_entries, entries, count) ? count : 0;
}

/* Sibling to set_group_songs_entries() that takes ownership of an already
 * allocated entries[] array rather than copying it, avoiding duplicate
 * allocations for large song lists. The caller transfers ownership and
 * must not free `entries` or its elements afterward. */
static void set_group_songs_entries_owned(group_song_entry_t * entries, int count) {
    free_group_song_entries(group_songs_entries, group_songs_count);
    group_songs_entries = entries;
    group_songs_count = count;
}

/* A pathological album can contain tens of thousands of tracks. Building one
 * LVGL object per track caused a large post-scan RSS spike and could make ADB
 * unresponsive. Keep the backing group intact for playback, but materialize
 * only one bounded page of rows at a time. */
#define GROUP_SONGS_PAGE_SIZE 200
static int group_songs_page_start;

/* One bounded job at a time. The worker owns path copies and never sees
 * LVGL objects or group_songs_entries. Page generations invalidate late
 * results without waiting for the current decoder to finish. */
static track_probe_job_t * group_probe_job;
static uint64_t group_probe_generation = 1;
static uint64_t group_probe_job_generation;
static uint64_t group_probe_requested_generation;
static int group_probe_page_start;
static bool group_probe_storage_available = true;
static bool group_probe_retry_pending;
static uint32_t group_probe_retry_tick;

static void cancel_group_song_probes(void) {
    ++group_probe_generation;
    group_probe_retry_pending = false;
    if (group_probe_job) track_probe_job_cancel(group_probe_job);
}

/* Now-playing indicator bar -- recreated fresh every populate_group_songs_
 * rows() call (that function's own lv_obj_clean(group_songs_list) destroys
 * whatever was here before, same as every row), so this pointer is only
 * ever valid between one populate call and the next, never stale across
 * one -- see refresh_group_songs_now_playing_indicator()'s own comment. */
static lv_obj_t * group_songs_now_playing_bar;
static lv_obj_t * group_songs_visible_rows[GROUP_SONGS_PAGE_SIZE];
static lv_obj_t * group_songs_thumbnail_images[GROUP_SONGS_PAGE_SIZE];
static bool group_songs_music_submenu;

/* Forward-declared here (defined after on_file_selected()) because
 * set_player_source_group_songs() needs group_songs_entries/count/title_label
 * already in scope, which the following definitions did not have. */
/* set_player_source_group_songs_direct defined in gui.c */


static void set_player_source_group_songs(int pos) {
    set_player_source_group_songs_direct(group_songs_entries, group_songs_count,
                                          lv_label_get_text(group_songs_title_label), pos);
    current_settings.last_source_kind = group_songs_source_is_album ? 2 : 0;
    snprintf(current_settings.last_source_name, sizeof(current_settings.last_source_name), "%s",
             group_songs_source_is_album ? lv_label_get_text(group_songs_title_label) : "");
}

/* Non-NULL only when the currently shown group is a user-created .m3u
 * playlist (set by show_m3u_playlist() via show_group_songs_editable()),
 * enabling the edit/remove-song UI. NULL for Artists/Albums/Favorites/
 * Most Played, which do not back onto a rewritable file. Borrowed pointer
 * from playlists_m3u_paths[]; valid for the lifetime of the screen.
 * group_songs_edit_mode is reset to false on every fresh entry. */
static const char * group_songs_edit_m3u_path = NULL;
static char group_songs_owned_m3u_path[PATH_MAX];
static void group_song_move_row_cb(lv_event_t * e);
static void reload_edited_playlist(void);
static struct stat group_songs_file_stat;
static bool group_songs_file_stat_valid;
static bool group_playlist_unchanged(void) {
    struct stat st;
    return group_songs_edit_m3u_path && group_songs_file_stat_valid &&
        stat(group_songs_edit_m3u_path, &st) == 0 && st.st_ino == group_songs_file_stat.st_ino &&
        st.st_dev == group_songs_file_stat.st_dev && st.st_size == group_songs_file_stat.st_size &&
        st.st_mtim.tv_sec == group_songs_file_stat.st_mtim.tv_sec &&
        st.st_mtim.tv_nsec == group_songs_file_stat.st_mtim.tv_nsec;
}
static bool group_songs_edit_mode = false;

/* Defined near show_m3u_playlist() further down, where the playlist file
 * helpers are in scope -- forward-declared here since
 * populate_group_songs_rows() needs to wire it up as the remove icon's
 * click handler. */
static void group_song_remove_row_cb(lv_event_t * e);
static void populate_group_songs_rows(void);

static void group_songs_prev_page_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || group_songs_page_start <= 0) return;
    group_songs_page_start -= GROUP_SONGS_PAGE_SIZE;
    if (group_songs_page_start < 0) group_songs_page_start = 0;
    populate_group_songs_rows();
}

static void group_songs_next_page_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int next = group_songs_page_start + GROUP_SONGS_PAGE_SIZE;
    if (next >= group_songs_count) return;
    group_songs_page_start = next;
    populate_group_songs_rows();
}

static lv_obj_t * add_group_songs_page_row(const char * text, lv_event_cb_t cb) {
    lv_obj_t * row = lv_label_create(group_songs_list);
    lv_obj_add_style(row, &list_row_style, 0);
    if (group_songs_music_submenu) lv_obj_set_width(row, lv_pct(100));
    lv_obj_add_style(row, &list_row_pressed_style, LV_STATE_PRESSED);
    row_label_enable_marquee(row);
    lv_obj_set_style_height(row, MUSIC_LIST_ROW_HEIGHT, LV_PART_MAIN);
    lv_obj_set_style_pad_top(row,
        (MUSIC_LIST_ROW_HEIGHT - lv_font_get_line_height(&LIST_ROW_FONT)) / 2, LV_PART_MAIN);
    lv_obj_set_style_text_align(row, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_label_set_text(row, text);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, NULL);
    return row;
}

/* Defined near populate_playlists_screen() further down -- forward-declared
 * here since group_song_remove_row_cb() (below) and playlist_row_click_cb()
 * both need to refresh the Playlists screen's list after auto-deleting a
 * playlist that's become empty. */
static void populate_playlists_screen(void);

/* Suppresses the follow-up LV_EVENT_CLICKED event when a long press has
 * already triggered the context menu on a group song row. */
static bool group_song_row_long_press_fired = false;

static void group_play_at(int pos) {
    if (library_rescan_active) return;
    if (pos < 0 || pos >= group_songs_count) return;
    if (group_songs_edit_m3u_path && !group_playlist_unchanged()) {
        reload_edited_playlist(); show_info_toast("Playlist changed. Select a song again."); return;
    }
    /* Allocate and copy playlist paths using calloc to handle potential
     * allocation failures cleanly. */
    char ** playlist_copy = calloc((size_t) group_songs_count, sizeof(char *));
    if (!playlist_copy) return;
    bool ok = true;
    for (int i = 0; i < group_songs_count; i++) {
        playlist_copy[i] = strdup(group_songs_entries[i].path);
        if (!playlist_copy[i]) {
            ok = false;
            break;
        }
    }
    if (!ok) {
        for (int i = 0; i < group_songs_count; i++) free(playlist_copy[i]);
        free(playlist_copy);
        return;
    }
    set_player_source_group_songs(pos);
    on_file_selected(playlist_copy, group_songs_count, pos);
}

static void group_song_row_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (group_song_row_long_press_fired) { group_song_row_long_press_fired = false; return; }
    group_play_at((int) (intptr_t) lv_event_get_user_data(e));
}

static lv_obj_t * playlist_start_popup, * playlist_start_backdrop;
static bool playlist_start_recent;

static void playlist_start_cancel(lv_event_t * e) {
    (void) e;
    lv_obj_add_flag(playlist_start_popup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(playlist_start_backdrop, LV_OBJ_FLAG_HIDDEN);
}
static void playlist_start_selected(lv_event_t * e, bool shuffle) {
    if (library_rescan_active) return;
    playlist_start_cancel(e);
    int count = playlist_start_recent ? (int) metadata_db_get_song_count() : group_songs_count;
    if (count <= 0) { show_info_toast("Playlist is empty"); return; }
    int index = 0;
    if (shuffle) {
        unsigned int value = (unsigned int) time(NULL) ^ lv_tick_get();
        int fd = open("/dev/urandom", O_RDONLY);
        if (fd >= 0) { ssize_t n = read(fd, &value, sizeof(value)); (void) n; close(fd); }
        index = (int) (value % (unsigned int) count);
    }
    gui_player_set_play_mode(shuffle ? PLAY_MODE_SHUFFLE : PLAY_MODE_SEQUENTIAL);
    if (playlist_start_recent) {
        set_player_source_recently_added(index);
        on_file_selected_lazy_recently_added(index);
    } else group_play_at(index);
}
static void playlist_start_sequential(lv_event_t * e) { playlist_start_selected(e, false); }
static void playlist_start_shuffle(lv_event_t * e) { playlist_start_selected(e, true); }
static void playlist_start_options(bool recent) {
    playlist_start_recent = recent;
    if (!playlist_start_popup) {
        static const menu_popup_row_t rows[] = {
            { "Start sequentially", playlist_start_sequential, false },
            { "Shuffle from a random song", playlist_start_shuffle, false },
            { "Cancel", playlist_start_cancel, false },
        };
        playlist_start_popup = build_menu_popup(rows, 3, playlist_start_cancel, &playlist_start_backdrop);
    }
    lv_obj_remove_flag(playlist_start_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(playlist_start_popup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(playlist_start_backdrop);
    lv_obj_move_foreground(playlist_start_popup);
}
static void group_start_options_cb(lv_event_t * e) {
    (void) e; playlist_start_options(false);
}

static void group_song_row_long_press_cb(lv_event_t * e) {
    if (library_rescan_active) return;
    if (lv_event_get_code(e) != LV_EVENT_LONG_PRESSED) return;
    group_song_row_long_press_fired = true;
    int pos = (int) (intptr_t) lv_event_get_user_data(e);
    open_song_context_menu(group_songs_entries[pos].path);
}

/* Music's Artist/Album drill-down uses a deliberate two-line identity.
 * Playlist and Queue rows intentionally retain their established shared
 * builder geometry. */
static void layout_music_submenu_row_text(lv_obj_t * row) {
    if (!row || lv_obj_get_child_count(row) < 2) return;
    lv_obj_t * primary = lv_obj_get_child(row, 0);
    lv_obj_t * secondary = lv_obj_get_child(row, 1);
    if (lv_obj_has_flag(secondary, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_set_y(primary, BOARD_SCALE_PX(28));
        return;
    }
    lv_obj_set_y(primary, BOARD_SCALE_PX(18));
    lv_obj_set_y(secondary, BOARD_SCALE_PX(62));
}

void gui_library_poll_track_probes(void) {
    bool visible = group_songs_screen && lv_screen_active() == group_songs_screen &&
                   group_songs_music_submenu && group_probe_storage_available && backlight_screen_is_on();
    if (!visible && group_probe_job && group_probe_job_generation == group_probe_generation)
        cancel_group_song_probes();

    if (group_probe_job) {
        bool current = visible && group_probe_job_generation == group_probe_generation;
        /* Take the completion snapshot before draining results, so a final
         * result published just after next() returns false is not lost. */
        bool done = track_probe_job_done(group_probe_job);
        bool drained = false;
        if (current) {
            /* Limit label work per tick; never rebuild the list or reset
             * its scroll position when a probe completes. */
            for (int budget = 0; budget < 4; ++budget) {
                size_t index;
                audio_current_format_info_t info;
                if (!track_probe_job_next(group_probe_job, &index, &info)) {
                    drained = true;
                    break;
                }
                int pos = group_probe_page_start + (int)index;
                if (!info.valid || !(info.duration_seconds > 0.0 &&
                    info.duration_seconds <= UINT_MAX - 1.0) ||
                    index >= GROUP_SONGS_PAGE_SIZE || pos >= group_songs_count) continue;
                char detail[64];
                unsigned int seconds = (unsigned int)(info.duration_seconds + 0.5);
                snprintf(detail, sizeof(detail), "%u:%02u · %s", seconds / 60, seconds % 60,
                         library_codec_name(info.codec));
                const char * old = group_songs_entries[pos].title;
                const char * newline = strchr(old, '\n');
                size_t title_len = newline ? (size_t)(newline - old) : strlen(old);
                char * title = malloc(title_len + strlen(detail) + 2);
                if (!title) continue;
                memcpy(title, old, title_len);
                title[title_len] = '\n';
                strcpy(title + title_len + 1, detail);
                free(group_songs_entries[pos].title);
                group_songs_entries[pos].title = title;
                lv_obj_t * row = group_songs_visible_rows[index];
                if (row) {
                    lv_obj_t * secondary = lv_obj_get_child(row, 1);
                    lv_label_set_text(secondary, detail);
                    lv_obj_remove_flag(secondary, LV_OBJ_FLAG_HIDDEN);
                    layout_music_submenu_row_text(row);
                }
            }
        }
        if (done && (!current || drained)) {
            track_probe_job_destroy(group_probe_job);
            group_probe_job = NULL;
        }
    }

    if (!visible || group_probe_job || group_probe_requested_generation == group_probe_generation)
        return;
    if (group_probe_retry_pending && lv_tick_elaps(group_probe_retry_tick) < 1000) return;
    int count = group_songs_count - group_songs_page_start;
    if (count > GROUP_SONGS_PAGE_SIZE) count = GROUP_SONGS_PAGE_SIZE;
    if (count <= 0) return;
    const char * paths[GROUP_SONGS_PAGE_SIZE];
    for (int i = 0; i < count; ++i) paths[i] = group_songs_entries[group_songs_page_start + i].path;
    group_probe_job = track_probe_job_start(paths, (size_t)count);
    if (!group_probe_job) {
        group_probe_retry_pending = true;
        group_probe_retry_tick = lv_tick_get();
        return;
    }
    group_probe_retry_pending = false;
    group_probe_requested_generation = group_probe_generation;
    group_probe_job_generation = group_probe_generation;
    group_probe_page_start = group_songs_page_start;
}

/* Positions/shows or hides group_songs_now_playing_bar against the CURRENT
 * group_songs_entries/count -- callable standalone (no row rebuild, no
 * scroll reset) whenever now_playing_path changes while this screen
 * is open, and also called once at the end of populate_group_songs_rows()
 * itself so a freshly opened group (or an edit-mode toggle, which also goes
 * through a full repopulate) starts with the right state. The visible-row
 * table lets the marker follow LVGL's final flex layout, including runtime
 * font/touch sizing and any action or paging rows before the song. */
static void refresh_group_songs_now_playing_indicator(void) {
    if (!group_songs_now_playing_bar) return;

    int match = -1;
    if (now_playing_path[0]) {
        for (int i = 0; i < group_songs_count; i++) {
            if (strcmp(group_songs_entries[i].path, now_playing_path) == 0) { match = i; break; }
        }
    }

    int page_end = group_songs_page_start + GROUP_SONGS_PAGE_SIZE;
    if (page_end > group_songs_count) page_end = group_songs_count;
    if (match < group_songs_page_start || match >= page_end) {
        lv_obj_add_flag(group_songs_now_playing_bar, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_t * row = group_songs_visible_rows[match - group_songs_page_start];
    if (!row) {
        lv_obj_add_flag(group_songs_now_playing_bar, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    /* Runtime font/touch sizing can make the rendered row taller than its
     * compile-time minimum. Follow the real laid-out object rather than
     * reconstructing its bounds from fixed height and gap constants. */
    lv_obj_update_layout(group_songs_list);
    lv_obj_remove_flag(group_songs_now_playing_bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(group_songs_now_playing_bar, 0, lv_obj_get_y(row));
    lv_obj_set_height(group_songs_now_playing_bar, lv_obj_get_height(row));
}

/* Rebuilds group_songs_list's rows from whatever group_songs_entries/count
 * currently hold, in either of two shapes: a plain tappable-to-play label
 * (list_row_style, same as every other group -- Artists/Albums/Favorites/
 * Most Played always use this one) when not actively editing a playlist, or
 * a row with a trailing remove icon (no play-on-tap -- see
 * group_song_remove_row_cb() below) when group_songs_edit_mode is on for an
 * editable .m3u playlist. Split out from show_group_songs_editable() so the
 * remove callback and the Edit/Done toggle can both redraw in place without
 * re-deriving the group or nav_push()ing a second copy of this screen. */
static void populate_group_songs_rows(void) {
    if (album_thumbnail_active_list == group_songs_list) {
        album_thumbnail_view_generation++;
        album_thumbnail_queue_count = 0;
    }
    cancel_group_song_probes();
    lv_obj_clean(group_songs_list);
    memset(group_songs_visible_rows, 0, sizeof(group_songs_visible_rows));
    memset(group_songs_thumbnail_images, 0, sizeof(group_songs_thumbnail_images));

    bool editable = group_songs_edit_m3u_path != NULL;
    if (editable) {
        lv_obj_clear_flag(group_songs_edit_btn, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(group_songs_edit_btn, group_songs_edit_mode ? "Done" : "Edit");
    } else {
        lv_obj_add_flag(group_songs_edit_btn, LV_OBJ_FLAG_HIDDEN);
    }

    bool editing = editable && group_songs_edit_mode;
    if (!editing) {
        lv_obj_t * start = add_group_songs_page_row(LV_SYMBOL_PLAY "  Play All", group_start_options_cb);
        lv_obj_add_style(start, &style_theme_card_bg, 0);
    }
    if (group_songs_count == 0) {
        build_list_message(group_songs_list, "Playlist is empty", "Add songs from a song menu.");
    }

    if (group_songs_count <= GROUP_SONGS_PAGE_SIZE) group_songs_page_start = 0;
    if (group_songs_page_start >= group_songs_count && group_songs_count > 0) {
        group_songs_page_start = ((group_songs_count - 1) / GROUP_SONGS_PAGE_SIZE) * GROUP_SONGS_PAGE_SIZE;
    }
    int page_end = group_songs_page_start + GROUP_SONGS_PAGE_SIZE;
    if (page_end > group_songs_count) page_end = group_songs_count;

    if (group_songs_page_start > 0) {
        char page_text[96];
        snprintf(page_text, sizeof(page_text), "Previous  •  %d–%d of %d",
                 group_songs_page_start + 1, page_end, group_songs_count);
        add_group_songs_page_row(page_text, group_songs_prev_page_cb);
    }

    for (int i = group_songs_page_start; i < page_end; i++) {
        if (group_songs_entries[i].show_disc_header) {
            char header[32];
            snprintf(header, sizeof(header), "Disc %d", group_songs_entries[i].disc_number);
            add_section_header(group_songs_list, header);
        }
        if (editing) {
            lv_obj_t * row = build_music_list_row(group_songs_list, group_songs_entries[i].title, NULL, BOARD_SCALE_PX(190));
            if (group_songs_music_submenu) lv_obj_set_width(row, lv_pct(100));
            group_songs_visible_rows[i - group_songs_page_start] = row;
            if (group_songs_music_submenu) layout_music_submenu_row_text(row);
            for (int direction = 0; direction < 2; direction++) {
                lv_obj_t * move = lv_label_create(row);
                lv_label_set_text(move, direction ? LV_SYMBOL_DOWN : LV_SYMBOL_UP);
                lv_obj_align(move, LV_ALIGN_RIGHT_MID, direction ? BOARD_SCALE_PX(-80) : BOARD_SCALE_PX(-130), 0);
                lv_obj_set_ext_click_area(move, BOARD_SCALE_PX(12));
                lv_obj_add_flag(move, LV_OBJ_FLAG_CLICKABLE);
                lv_obj_add_event_cb(move, group_song_move_row_cb, LV_EVENT_CLICKED, (void *) (intptr_t) (i * 2 + direction));
            }

            lv_obj_t * remove_icon = lv_image_create(row);
            lv_image_set_src(remove_icon, asset_path("touch_list/del.png"));
            lv_obj_align(remove_icon, LV_ALIGN_RIGHT_MID, BOARD_SCALE_PX(-20), 0);
            lv_obj_add_flag(remove_icon, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(remove_icon, group_song_remove_row_cb, LV_EVENT_CLICKED, (void *) (intptr_t) i);
        } else {
            /* One lv_label via the shared list_row_style, not a container +
             * child label each with their own local style properties -- see
             * list_row_style's own doc comment (screen_builders.h). */
            lv_obj_t * row = build_music_list_row(group_songs_list, group_songs_entries[i].title, NULL, BOARD_SCALE_PX(70));
            if (group_songs_music_submenu) lv_obj_set_width(row, lv_pct(100));
            group_songs_visible_rows[i - group_songs_page_start] = row;
            if (group_songs_music_submenu) layout_music_submenu_row_text(row);

            lv_obj_t * quality = lv_image_create(row);
            lv_image_set_src(quality, asset_path(song_quality_asset_for_path(group_songs_entries[i].path)));
            /* Child alignment is relative to the label's padded content
             * box. Cancel the 70px text reserve so the badge is physically
             * 14px from the card edge (same rule as compact-list rows). */
            lv_obj_align(quality, LV_ALIGN_RIGHT_MID, BOARD_SCALE_PX(-14), 0);
            lv_obj_remove_flag(quality, LV_OBJ_FLAG_CLICKABLE);

            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(row, group_song_row_click_cb, LV_EVENT_CLICKED, (void *) (intptr_t) i);
            lv_obj_add_event_cb(row, group_song_row_long_press_cb, LV_EVENT_LONG_PRESSED, (void *) (intptr_t) i);
        }
    }

    if (page_end < group_songs_count) {
        char page_text[96];
        snprintf(page_text, sizeof(page_text), "Next  •  %d–%d of %d",
                 group_songs_page_start + 1, page_end, group_songs_count);
        add_group_songs_page_row(page_text, group_songs_next_page_cb);
    }

    /* Recreated fresh here (lv_obj_clean() above just destroyed whatever
     * was here before) rather than kept as a truly persistent object --
     * every row in this list gets rebuilt on every populate call already,
     * so this just follows the same pattern. LV_OBJ_FLAG_IGNORE_LAYOUT
     * keeps this list's own flex column layout from trying to stack it in
     * as another row, same trick build_icon_grid_screen() uses for its
     * divider lines. Positioned/shown by refresh_group_songs_now_playing_
     * indicator() below, not here -- that also runs standalone (no rebuild)
     * whenever now_playing_path changes while this screen stays
     * open, e.g. a gapless auto-advance to the next track in the group. */
    group_songs_now_playing_bar = lv_obj_create(group_songs_list);
    lv_obj_remove_style_all(group_songs_now_playing_bar);
    lv_obj_set_size(group_songs_now_playing_bar, BOARD_SCALE_PX(5), MUSIC_LIST_ROW_HEIGHT);
    lv_obj_set_style_bg_color(group_songs_now_playing_bar, accent_lv_color(), 0);
    lv_obj_set_style_bg_opa(group_songs_now_playing_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(group_songs_now_playing_bar, 2, 0);
    lv_obj_add_flag(group_songs_now_playing_bar, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_remove_flag(group_songs_now_playing_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(group_songs_now_playing_bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(group_songs_now_playing_bar, LV_OBJ_FLAG_HIDDEN);
    refresh_group_songs_now_playing_indicator();
    refresh_group_song_thumbnails();
}

/* Every screen's back button is a fixed 64x64 at the screen's own left
 * edge (build_group_songs_screen() below, build_subsonic_list_screen()) --
 * this is where a title label should start, not centered over top of it. */
#define TITLE_LABEL_LEFT_INSET 76 /* 64px back button + 12px breathing room */
/* Default right margin for a title with no right-side icon of its own on
 * that particular screen -- most build_subsonic_list_screen() callers
 * (Playlists, Saved Servers, New Connection, Queue, DLNA, Remote Control,
 * the plugin list pool, ...) never get anywhere near this wide anyway
 * (always a short fixed string), so this is mostly headroom for the ones
 * that do have a long dynamic title but no button beside it (the local
 * library's Artist -> Albums drill-down, artist_albums_screen). */
#define TITLE_LABEL_DEFAULT_RIGHT_MARGIN 20

/* Narrows an already left-aligned, auto-scrolling title label (see
 * build_subsonic_list_screen()'s and build_group_songs_screen()'s own
 * construction) so its right edge stops before `right_icon`'s own left
 * edge, instead of running underneath it -- call once, right after
 * right_icon's own final on-screen position is set (real coordinates, not
 * a pending layout -- lv_label/lv_image both size/position synchronously
 * on lv_obj_align(), no intervening refresh pass needed). Safe to call
 * even while right_icon is currently hidden (e.g. a Download button only
 * shown for some views of a reused screen) -- a hidden object still has a
 * real position, and reserving room for it whether or not it's showing
 * right now is simpler and safer than tracking two different widths.
 *
 * Uses lv_obj_get_coords() after lv_obj_update_layout() on the common
 * parent so absolute coordinates are resolved before calculating the
 * available title width. */
void reserve_title_width_before(lv_obj_t * title, lv_obj_t * right_icon) {
    lv_obj_update_layout(lv_obj_get_parent(title));

    lv_area_t title_area, icon_area;
    lv_obj_get_coords(title, &title_area);
    lv_obj_get_coords(right_icon, &icon_area);
    int32_t width = icon_area.x1 - title_area.x1 - 12;
    if (width < 40) width = 40; /* never collapse to nothing/negative on a pathological layout */
    lv_obj_set_width(title, width);
}

static void group_songs_edit_btn_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    group_songs_edit_mode = !group_songs_edit_mode;
    populate_group_songs_rows();
}

/* entries[0..count) is copied into group_songs_entries (see set_group_songs_
 * entries()) -- the caller's own array/buffer can be freed immediately after
 * this call returns, unlike the old group_t-based API where the group_t's
 * .indices had to stay valid for as long as this screen kept showing it. */
static void show_group_songs_editable(const char * name, const group_song_entry_t * entries, int count,
                                       const char * editable_m3u_path, bool music_submenu) {
    group_songs_source_is_album = group_songs_refresh_source == GROUP_SONGS_REFRESH_ALBUM;
    group_songs_music_submenu = music_submenu;
    if (editable_m3u_path) snprintf(group_songs_owned_m3u_path, sizeof(group_songs_owned_m3u_path), "%s", editable_m3u_path);
    group_songs_edit_m3u_path = editable_m3u_path ? group_songs_owned_m3u_path : NULL;
    group_songs_file_stat_valid = editable_m3u_path && stat(editable_m3u_path, &group_songs_file_stat) == 0;
    group_songs_edit_mode = false;
    set_group_songs_entries(entries, count);
    group_songs_page_start = 0;

    lv_label_set_text(group_songs_title_label, name);
    populate_group_songs_rows();

    nav_push(group_songs_screen);
}

void show_group_songs(const char * name, const group_song_entry_t * entries, int count) {
    group_songs_refresh_source = name && strcmp(name, "Favorites") == 0 ? GROUP_SONGS_REFRESH_FAVORITES
                                : name && strcmp(name, "Most Played") == 0 ? GROUP_SONGS_REFRESH_MOST_PLAYED
                                : GROUP_SONGS_REFRESH_NONE;
    show_group_songs_editable(name, entries, count, NULL, false);
}

static void show_music_group_songs(const char * name, const char * album_artist, const char * artist_filter,
                                   const group_song_entry_t * entries, int count) {
    group_songs_refresh_source = GROUP_SONGS_REFRESH_ALBUM;
    snprintf(group_songs_refresh_album, sizeof(group_songs_refresh_album), "%s", name ? name : "");
    snprintf(group_songs_refresh_album_artist, sizeof(group_songs_refresh_album_artist), "%s",
             album_artist ? album_artist : "");
    snprintf(group_songs_refresh_artist_filter, sizeof(group_songs_refresh_artist_filter), "%s",
             artist_filter ? artist_filter : "");
    show_group_songs_editable(name, entries, count, NULL, true);
}

/* Ownership-transferring sibling of show_group_songs() -- see set_group_
 * songs_entries_owned()'s own comment for why this exists. Never editable
 * (no m3u path makes sense for a flattened, non-playlist view), same as
 * plain show_group_songs()'s own default. Caller must not free `entries`
 * after this call -- ownership has moved to group_songs_entries. */
static void show_group_songs_take_ownership(const char * name, group_song_entry_t * entries, int count,
                                            bool push_screen) {
    group_songs_source_is_album = false;
    group_songs_music_submenu = true;
    group_songs_edit_m3u_path = NULL;
    group_songs_edit_mode = false;
    set_group_songs_entries_owned(entries, count);
    group_songs_page_start = 0;

    lv_label_set_text(group_songs_title_label, name);
    populate_group_songs_rows();

    if (push_screen) nav_push(group_songs_screen);
}

static lv_obj_t * build_group_songs_screen(void) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_add_style(scr, &style_theme_screen_bg, 0);

    group_songs_title_label = build_screen_header(scr, "", generic_back_cb, NULL, NULL);
    /* Hidden by default -- populate_group_songs_rows() (via
     * show_group_songs_editable()) is what actually shows this, and only
     * for a group backed by an editable .m3u playlist. */
    group_songs_edit_btn = lv_label_create(scr);
    lv_label_set_text(group_songs_edit_btn, "Edit");
    lv_obj_set_style_text_color(group_songs_edit_btn, accent_lv_color(), 0);
    lv_obj_set_style_text_font(group_songs_edit_btn, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    align_screen_header_action(group_songs_edit_btn, BOARD_SCALE_PX(20));
    lv_obj_add_flag(group_songs_edit_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(group_songs_edit_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(group_songs_edit_btn, group_songs_edit_btn_cb, LV_EVENT_CLICKED, NULL);
    /* Reserved whether or not Edit is currently visible -- see
     * reserve_title_width_before()'s own comment on why that's safe/
     * simpler than tracking two different widths. */
    reserve_title_width_before(group_songs_title_label, group_songs_edit_btn);

    group_songs_list = lv_obj_create(scr);
    lv_obj_set_size(group_songs_list, lv_pct(100),
                    lv_display_get_vertical_resolution(lv_display_get_default()) - STATUS_BAR_CLEARANCE -
                        TITLE_ROW_HEIGHT);
    lv_obj_align(group_songs_list, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(group_songs_list, 0, 0);
    lv_obj_set_style_border_width(group_songs_list, 0, 0);
    /* Clear padding so rows align flush with container edges. */
    lv_obj_set_style_pad_all(group_songs_list, 0, 0);
    lv_obj_set_scroll_dir(group_songs_list, LV_DIR_VER); /* see build_icon_grid_screen's comment */
    lv_obj_set_flex_flow(group_songs_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(group_songs_list, GUI_ROW_GAP, 0);
    lv_obj_set_style_pad_top(group_songs_list, GUI_ROW_GAP, 0);

    lv_obj_add_event_cb(scr, album_thumbnail_screen_loaded_cb, LV_EVENT_SCREEN_LOADED, group_songs_list);
    lv_obj_add_event_cb(scr, album_thumbnail_screen_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, group_songs_list);
    lv_obj_add_event_cb(group_songs_list, album_thumbnail_scroll_cb, LV_EVENT_SCROLL_BEGIN, NULL);
    lv_obj_add_event_cb(group_songs_list, album_thumbnail_scroll_cb, LV_EVENT_SCROLL_END, NULL);

    finalize_screen_navigation(scr);
    return scr;
}

/* Reusable on-screen keyboard text entry -- the app had none of this
 * before network streaming needed a way to type a server URL/username/
 * password. One persistent screen (title + textarea + T9 keypad),
 * repurposed for whatever's being entered via show_text_entry() rather
 * than a screen per use, same "one object, rebuilt/retargeted on demand"
 * approach as group_songs_screen above. Deliberately skips
 * finalize_screen_navigation() (no swipe-to-go-back) -- a swipe gesture
 * while typing on the keypad would be an easy accidental way to lose
 * whatever was being entered. (text_entry_done_cb_t itself is forward-
 * declared earlier, alongside show_text_entry(), for the PEQ screen's
 * tap-to-edit handlers.)
 *
 * T9 multi-tap keypad using themed keyboard assets. 12-key layout (1-9, Mode,
 * Shift, plus Del/Left/Right/Enter/Space) in a 4-column x 5-row grid; keys 2-9
 * cycle through their letter group (e.g. key 2 -> a -> b -> c) on repeated taps within
 * TEXT_ENTRY_MULTITAP_MS, matching classic phone-keypad multi-tap text
 * entry. Three keypad modes (ABC/NUM/SYM), cycled via the Mode key: ABC
 * cycles letters per key (0 and 1 have no letter group on this asset set,
 * so they always insert their literal digit); NUM makes every digit key
 * insert its literal digit directly, no cycling; SYM cycles each key's own
 * punctuation set (symbol0.png..symbol9.png). PEQ's numeric-only fields
 * (show_text_entry()'s `numeric` flag) skip all three and lock permanently
 * to plain-digit entry, hiding Mode/Shift (neither means anything for a
 * frequency/gain/Q value) and exposing a dedicated "./-" key instead (gain
 * needs negative numbers; nothing else here provides them once Mode is
 * hidden).
 *
 * Scope cuts, deliberate for this first pass: num_more.png/symbol_more.png
 * (a second punctuation page) aren't wired up -- symbol0-9 alone covers
 * ordinary punctuation (. , ! ? etc.), and every text_entry field here is a
 * short single-line value (URLs, names, credentials), not prose. The
 * select/ subfolder's pressed-state art isn't used either (no PRESSED/
 * RELEASED image swap, unlike screen_builders.c's icon-grid tiles) --
 * LV_EVENT_CLICKED alone is enough feedback for a keypad tap. bg.png/
 * text_bg.png/cursor.png (decorative chrome) and ok.png/ok_s.png (redundant
 * with enter.png's own baked-in "Enter" text) are unused. char_l.png/
 * char_u.png are never referenced -- confirmed byte-identical to each other
 * and to no clear distinct purpose from char.png (the Mode key's own ABC
 * glyph) by direct pixel inspection. */

/* ---- Virtualized local-album thumbnails -------------------------------
 * Only the 20 recycled compact-list rows can request artwork. One worker at
 * a time reads/decodes a representative song's embedded or Rockbox albumart
 * file, while a 250-entry RGB565 LRU cache keeps the visible window plus
 * scroll headroom bounded at ~2531 KiB. Persistent sized files live in
 * MUSIC_ROOT_DIR/.compas/albumart/<artist>-<album>.72x72.bmp. */
#define ALBUM_THUMBNAIL_PX ALBUMART_THUMBNAIL_SIZE
#define ALBUM_PLAYER_CACHE_PX ALBUMART_PLAYER_CACHE_SIZE
#define ALBUM_THUMBNAIL_CACHE_SIZE 250

typedef struct {
    uint64_t key;
    thumbnail_key_kind_t key_kind;
    bool known; /* true even when pixels==NULL: negative cache for albums without art */
    uint8_t * pixels;
    lv_image_dsc_t dsc;
    uint32_t last_use;
} album_thumbnail_cache_entry_t;

typedef struct {
    uint64_t artist_key;
    metadata_db_group_kind_t artist_group_kind;
    uint64_t album_key;
    thumbnail_key_kind_t album_key_kind;
    int64_t representative_song_id;
    bool known;
    bool has_album;
    uint32_t last_use;
} artist_thumbnail_alias_entry_t;

typedef struct {
    int64_t song_id;
    uint64_t key;
    thumbnail_key_kind_t key_kind;
    metadata_db_group_kind_t artist_group_kind;
    char artist_name[128];
    int view_generation;
    int cache_epoch;
    int logical_index;
    lv_obj_t * list;
} album_thumbnail_request_t;

#define ALBUM_THUMBNAIL_QUEUE_SIZE 20

static album_thumbnail_cache_entry_t album_thumbnail_cache[ALBUM_THUMBNAIL_CACHE_SIZE];
/* Aliases are small lookup records, not decoded images, so they have their
 * own dynamically sized table (declared above) and do not consume RGB565
 * cache entries. */
static uint32_t album_thumbnail_use_counter;
static pthread_t album_thumbnail_thread;
static atomic_bool album_thumbnail_active;
static bool album_thumbnail_active_key_valid;
static uint64_t album_thumbnail_active_key;
static thumbnail_key_kind_t album_thumbnail_active_key_kind;
static int album_thumbnail_active_cache_epoch;
static int album_thumbnail_active_view_generation;
static int album_thumbnail_active_logical_index;
static metadata_db_group_kind_t album_thumbnail_active_artist_group_kind;
static atomic_bool album_thumbnail_done;
static int64_t album_thumbnail_result_song_id;
static uint64_t album_thumbnail_result_key;
static thumbnail_key_kind_t album_thumbnail_result_key_kind;
static thumbnail_key_kind_t album_thumbnail_result_request_key_kind;
static int album_thumbnail_result_cache_epoch;
static uint8_t * album_thumbnail_result_pixels;
/* Set alongside result_pixels in album_thumbnail_thread_func(), valid only
 * when result_pixels is NULL (a failed decode) -- lets album_thumbnail_poll_cb()
 * ask artwork_failure_cache_is_blocked() whether THIS specific failure (already
 * recorded by album_thumbnail_load_or_decode_ex() on every one of its own
 * exit paths) was temporary or permanent, instead of caching every failure as
 * a permanent "no art" the way an unconditional `known = true` used to. */
static time_t album_thumbnail_result_source_mtime;
static bool album_thumbnail_result_have_source_mtime;
static bool album_thumbnail_result_negative_confirmed;
static bool album_thumbnail_result_has_alias;
static bool album_thumbnail_result_artist_negative_confirmed;
static uint64_t album_thumbnail_result_artist_key;
static metadata_db_group_kind_t album_thumbnail_result_artist_group_kind;
static uint64_t album_thumbnail_result_alias_album_key;
static int64_t album_thumbnail_result_alias_song_id;
static lv_timer_t * album_thumbnail_poll_timer;
static album_thumbnail_request_t album_thumbnail_queue[ALBUM_THUMBNAIL_QUEUE_SIZE];
static bool album_thumbnail_scrolling;
    /* Boot-time RAM cache preload: piggybacks on the existing persistent
     * warmer thread (album_thumb_gen_thread_func) to also hand its first
     * ALBUM_THUMBNAIL_CACHE_SIZE decoded thumbnails to the main/LVGL thread
     * for insertion into album_thumbnail_cache[], instead of only writing the
     * on-disk sized BMP sidecar. Single-slot rendezvous (like the on-screen
     * lazy-decode hand-off above) -- the warmer thread blocks after staging
     * one result until album_thumbnail_poll_cb() has consumed it, so at most
     * one pending buffer ever exists and the cache array is still only ever
     * written from the main thread. */
    static int album_thumb_gen_ram_filled;
    static uint64_t album_thumb_gen_cached_keys[ALBUM_THUMBNAIL_CACHE_SIZE];
    static thumbnail_key_kind_t album_thumb_gen_cached_key_kinds[ALBUM_THUMBNAIL_CACHE_SIZE];
    static int album_thumb_gen_cached_key_count;
    /* Aliases already resolved in RAM when this warmer run started (the
     * alias table itself is UI-thread only): a restarted run skips them. */
    typedef struct { uint64_t key; metadata_db_group_kind_t kind; } album_thumb_gen_alias_key_t;
    static album_thumb_gen_alias_key_t * album_thumb_gen_known_aliases;
    static size_t album_thumb_gen_known_alias_count;
    /* EMPTY: slot free, warmer may stage the next result into it.
     * PENDING: warmer has staged a result and is waiting.
     * CLAIMED: album_thumbnail_poll_cb() has taken ownership and is still
     * reading/committing it -- the warmer must not touch the staged fields
     * again until this goes back to EMPTY. A plain 2-state pending/not-
     * pending flag let the warmer's own wait-loop-exit race the callback:
     * clearing "pending" and THEN reading the payload left a window where
     * the warmer saw the slot as free and overwrote the fields the callback
     * was still using. The CAS transitions below close that window. */
    enum { ALBUM_BOOT_PRELOAD_EMPTY = 0, ALBUM_BOOT_PRELOAD_PENDING, ALBUM_BOOT_PRELOAD_CLAIMED };
    static atomic_int album_boot_preload_state;
    static uint64_t album_boot_preload_result_key;
    static thumbnail_key_kind_t album_boot_preload_result_key_kind;
    static int64_t album_boot_preload_result_song_id;
    static uint8_t * album_boot_preload_result_pixels;
    static bool album_boot_preload_result_have_source_mtime;
    static time_t album_boot_preload_result_source_mtime;
    enum { ALBUM_BOOT_RESULT_ALBUM, ALBUM_BOOT_RESULT_ARTIST };
    static int album_boot_preload_result_kind;
    static int album_boot_preload_result_cache_epoch;
    static uint64_t album_boot_preload_result_artist_key;
    static metadata_db_group_kind_t album_boot_preload_result_artist_group_kind;
    static bool album_boot_preload_result_artist_has_alias;
    static bool album_boot_preload_result_artist_negative_confirmed;
    static uint64_t album_boot_preload_result_alias_album_key;
    static int64_t album_boot_preload_result_alias_song_id;
static bool album_thumb_gen_user_visible;
static bool album_thumb_gen_force_reload;
static bool album_thumb_gen_single_album;
static bool album_thumb_gen_altstack_already_installed;
static int64_t album_thumb_gen_target_song_id;
static atomic_int album_thumb_gen_error_count;
static bool album_thumbnail_list_is_visible(lv_obj_t * list) {
    if (!list) return false;
    lv_obj_t * active = lv_screen_active();
    if ((list == albums_list && active == albums_screen) ||
        (list == artist_albums_list && active == artist_albums_screen) ||
        (list == artists_list && active == artists_screen) ||
        (list == album_artist_list && active == album_artist_screen) ||
        (list == all_songs_list && active == all_songs_screen) ||
        (list == recently_added_list && active == recently_added_screen) ||
        (list == group_songs_list && active == group_songs_screen))
        return true;
    return list == files_search_list && active == files_screen &&
           !lv_obj_has_flag(files_search_list, LV_OBJ_FLAG_HIDDEN);
}

static artist_thumbnail_alias_entry_t * artist_thumbnail_alias_cache;
static size_t artist_thumbnail_alias_cache_capacity;

/* True when the visible thumbnail list shows a row that a just-committed
 * entry can change: a row keyed by it, or an artist row whose alias borrows
 * that album. Independent of view generations, so a result staged before a
 * scroll still repaints the rows that are on screen now. */
static bool thumbnail_active_list_shows(thumbnail_key_kind_t key_kind, uint64_t key) {
    lv_obj_t * list = album_thumbnail_active_list;
    if (!list || !album_thumbnail_list_is_visible(list)) return false;
    if (list == group_songs_list) return true;
    if (compact_list_shows_artwork_key(list, key)) return true;
    if (key_kind != THUMBNAIL_KEY_ALBUM) return false;
    for (size_t i = 0; i < artist_thumbnail_alias_cache_capacity; i++) {
        const artist_thumbnail_alias_entry_t * e = &artist_thumbnail_alias_cache[i];
        if (e->known && e->has_album && e->album_key == key &&
            compact_list_shows_artwork_key(list, e->artist_key))
            return true;
    }
    return false;
}

static bool album_thumbnail_lazy_work_pending(void) {
    return atomic_load(&album_thumbnail_queue_count) > 0 || atomic_load(&album_thumbnail_active);
}

static album_thumbnail_cache_entry_t * album_thumbnail_cache_find(thumbnail_key_kind_t key_kind, uint64_t key) {
    for (int i = 0; i < ALBUM_THUMBNAIL_CACHE_SIZE; i++) {
        if (album_thumbnail_cache[i].known && album_thumbnail_cache[i].key_kind == key_kind &&
            album_thumbnail_cache[i].key == key) {
            album_thumbnail_cache[i].last_use = ++album_thumbnail_use_counter;
            return &album_thumbnail_cache[i];
        }
    }
    return NULL;
}

static bool artist_thumbnail_alias_cache_resize(int artist_count, int album_artist_count) {
    size_t wanted = (size_t) (artist_count > 0 ? artist_count : 0) +
                    (size_t) (album_artist_count > 0 ? album_artist_count : 0);
    if (wanted == artist_thumbnail_alias_cache_capacity) return true;
    if (wanted == 0) {
        free(artist_thumbnail_alias_cache);
        artist_thumbnail_alias_cache = NULL;
        artist_thumbnail_alias_cache_capacity = 0;
        return true;
    }
    artist_thumbnail_alias_entry_t * grown = realloc(artist_thumbnail_alias_cache,
                                                       wanted * sizeof(*grown));
    if (!grown) return false;
    if (wanted > artist_thumbnail_alias_cache_capacity)
        memset(grown + artist_thumbnail_alias_cache_capacity, 0,
               (wanted - artist_thumbnail_alias_cache_capacity) * sizeof(*grown));
    artist_thumbnail_alias_cache = grown;
    artist_thumbnail_alias_cache_capacity = wanted;
    return true;
}

static bool artist_thumbnail_alias_cache_resize_for_library(void) {
    int artist_count = 0, album_artist_count = 0, album_count = 0;
    metadata_db_get_group_counts(&artist_count, &album_artist_count, &album_count);
    return artist_thumbnail_alias_cache_resize(artist_count, album_artist_count);
}

static artist_thumbnail_alias_entry_t * artist_thumbnail_alias_find(
        uint64_t artist_key, metadata_db_group_kind_t artist_group_kind) {
    for (size_t i = 0; i < artist_thumbnail_alias_cache_capacity; i++) {
        if (artist_thumbnail_alias_cache[i].known &&
            artist_thumbnail_alias_cache[i].artist_key == artist_key &&
            artist_thumbnail_alias_cache[i].artist_group_kind == artist_group_kind) {
            artist_thumbnail_alias_cache[i].last_use = ++album_thumbnail_use_counter;
            return &artist_thumbnail_alias_cache[i];
        }
    }
    return NULL;
}

static void artist_thumbnail_alias_cache_commit(uint64_t artist_key,
                                                 metadata_db_group_kind_t artist_group_kind,
                                                 bool has_album,
                                                 uint64_t album_key, int64_t representative_song_id) {
    if (!artist_thumbnail_alias_cache_capacity &&
        !artist_thumbnail_alias_cache_resize_for_library()) return;
    if (!artist_thumbnail_alias_cache_capacity) return;
    size_t victim = SIZE_MAX;
    for (size_t i = 0; i < artist_thumbnail_alias_cache_capacity; i++) {
        if (artist_thumbnail_alias_cache[i].known &&
            artist_thumbnail_alias_cache[i].artist_key == artist_key &&
            artist_thumbnail_alias_cache[i].artist_group_kind == artist_group_kind) {
            victim = i;
            break;
        }
    }
    if (victim == SIZE_MAX) {
        uint32_t oldest = UINT32_MAX;
        for (size_t i = 0; i < artist_thumbnail_alias_cache_capacity; i++) {
            if (!artist_thumbnail_alias_cache[i].known) {
                victim = i;
                break;
            }
            if (artist_thumbnail_alias_cache[i].last_use < oldest) {
                oldest = artist_thumbnail_alias_cache[i].last_use;
                victim = i;
            }
        }
    }
    if (victim == SIZE_MAX) return;
    artist_thumbnail_alias_cache[victim] = (artist_thumbnail_alias_entry_t) {
        .artist_key = artist_key,
        .artist_group_kind = artist_group_kind,
        .album_key = album_key,
        .album_key_kind = THUMBNAIL_KEY_ALBUM,
        .representative_song_id = representative_song_id,
        .known = true,
        .has_album = has_album,
        .last_use = ++album_thumbnail_use_counter
    };
}

static void artist_thumbnail_alias_cache_clear(void) {
    free(artist_thumbnail_alias_cache);
    artist_thumbnail_alias_cache = NULL;
    artist_thumbnail_alias_cache_capacity = 0;
}

static void artist_thumbnail_alias_cache_remove(uint64_t artist_key,
                                                 metadata_db_group_kind_t artist_group_kind) {
    artist_thumbnail_alias_entry_t * entry = artist_thumbnail_alias_find(artist_key, artist_group_kind);
    if (entry) memset(entry, 0, sizeof(*entry));
}

static void album_thumbnail_queue_remove(thumbnail_key_kind_t key_kind, uint64_t key) {
    for (int i = 0; i < album_thumbnail_queue_count; ) {
        if (album_thumbnail_queue[i].key_kind != key_kind || album_thumbnail_queue[i].key != key) {
            i++;
            continue;
        }
        memmove(&album_thumbnail_queue[i], &album_thumbnail_queue[i + 1],
                sizeof(album_thumbnail_queue[0]) * (size_t) (album_thumbnail_queue_count - i - 1));
        album_thumbnail_queue_count--;
    }
}

/* Alias results are per scope: Artists and Album Artists can share a name
 * (same key) and still resolve differently. */
static void album_thumbnail_queue_remove_artist(uint64_t key, metadata_db_group_kind_t group_kind) {
    for (int i = 0; i < album_thumbnail_queue_count; ) {
        if (album_thumbnail_queue[i].key_kind != THUMBNAIL_KEY_ARTIST || album_thumbnail_queue[i].key != key ||
            album_thumbnail_queue[i].artist_group_kind != group_kind) {
            i++;
            continue;
        }
        memmove(&album_thumbnail_queue[i], &album_thumbnail_queue[i + 1],
                sizeof(album_thumbnail_queue[0]) * (size_t) (album_thumbnail_queue_count - i - 1));
        album_thumbnail_queue_count--;
    }
}

/* Returns true when a row of the visible list lost this entry's image; the
 * caller re-decorates only after the slot is fully cleared or replaced, so
 * the decorator never sees a half-evicted "known, no art" entry. */
static bool album_thumbnail_cache_evict_entry(album_thumbnail_cache_entry_t * e) {
    uint8_t * retired_pixels = e->pixels;
    bool visible_lost = false;
    if (retired_pixels) {
        e->pixels = NULL;
        e->dsc.data = NULL;
        visible_lost = release_thumbnail_dsc(&e->dsc); /* after clearing, so a re-decorate cannot re-attach it */
        free(retired_pixels);
    }
    memset(e, 0, sizeof(*e));
    return visible_lost;
}

static void album_thumbnail_cache_evict(album_thumbnail_cache_entry_t * e) {
    if (album_thumbnail_cache_evict_entry(e)) refresh_thumbnail_list_visible(album_thumbnail_active_list);
}

static void album_thumbnail_cache_remove(thumbnail_key_kind_t key_kind, uint64_t key) {
    for (int i = 0; i < ALBUM_THUMBNAIL_CACHE_SIZE; i++) {
        album_thumbnail_cache_entry_t * e = &album_thumbnail_cache[i];
        if (!e->known || e->key_kind != key_kind || e->key != key) continue;
        album_thumbnail_cache_evict(e);
        return;
    }
}

static void album_thumbnail_cache_remove_kind(thumbnail_key_kind_t key_kind) {
    bool visible_lost = false;
    for (int i = 0; i < ALBUM_THUMBNAIL_CACHE_SIZE; i++)
        if (album_thumbnail_cache[i].known && album_thumbnail_cache[i].key_kind == key_kind &&
            album_thumbnail_cache_evict_entry(&album_thumbnail_cache[i]))
            visible_lost = true;
    if (visible_lost) refresh_thumbnail_list_visible(album_thumbnail_active_list);
}

static void album_thumbnail_cache_clear(void) {
    album_thumbnail_cache_epoch_advance();
    uint8_t * retired_pixels[ALBUM_THUMBNAIL_CACHE_SIZE];
    for (int i = 0; i < ALBUM_THUMBNAIL_CACHE_SIZE; i++) {
        retired_pixels[i] = album_thumbnail_cache[i].pixels;
        album_thumbnail_cache[i].pixels = NULL;
        album_thumbnail_cache[i].dsc.data = NULL;
    }
    refresh_all_thumbnail_lists();
    for (int i = 0; i < ALBUM_THUMBNAIL_CACHE_SIZE; i++) {
        free(retired_pixels[i]);
        memset(&album_thumbnail_cache[i], 0, sizeof(album_thumbnail_cache[i]));
    }
    artist_thumbnail_alias_cache_clear();
    album_thumbnail_use_counter = 0;
    artwork_failure_cache_clear();
}

    /* Evicts the LRU slot (or the first unused one) and installs `pixels` (or
     * a durable "known, no art" marker for a PERMANENT failure) into it.
     * Shared by the on-screen lazy-decode commit and the boot-time RAM
     * preload hand-off further below -- both only ever run on the main/LVGL
     * thread, the only thread allowed to touch album_thumbnail_cache[].
     * Takes ownership of `pixels`: either stores it or frees it, never both. */
static void album_thumbnail_cache_commit(thumbnail_key_kind_t key_kind, uint64_t key,
                                         int64_t source_song_id, uint8_t * pixels,
                                         bool have_source_mtime, time_t source_mtime,
                                         bool negative_confirmed) {
        /* A failed decode (pixels == NULL) is only worth caching as a durable
         * "known, no art" entry if album_thumbnail_load_or_decode_ex() itself
         * already recorded it as PERMANENT (corrupt/oversized/genuinely
         * absent source). A TEMPORARY failure must NOT be cached here the
         * same way -- album_thumbnail_cache_find()'s only gate is `known`, so
         * caching it at all would make a transient hiccup permanent for this
         * song for the rest of this cache slot's lifetime. Leaving the slot
         * at its just-memset "unused" state (known=false) instead lets a
         * future request legitimately retry this song. */
        bool commit_known = pixels != NULL || negative_confirmed;
        bool victim_visible_lost = false;
        if (!commit_known && key_kind == THUMBNAIL_KEY_ALBUM && have_source_mtime) {
            artwork_fail_reason_t fail_reason = ARTWORK_FAIL_NONE;
            if (artwork_failure_cache_is_blocked(source_song_id, source_mtime, &fail_reason) &&
                fail_reason == ARTWORK_FAIL_PERMANENT) {
                commit_known = true;
            }
        }
        int victim = -1;
        /* An interrupted-then-restarted boot preload pass can reach the same
         * song_id twice (it always restarts at offset 0). Without this check
         * that would waste a second slot on a duplicate instead of just
         * refreshing the one that's already there. The on-screen lazy-decode
         * path never hits this: queue_album_thumbnail() already skips
         * queuing a song album_thumbnail_cache_find() reports as known. A
         * full pass for the match has to run before the unused/LRU pass
         * below, which stops at the first unused slot -- a duplicate further
         * down the array would otherwise never be seen. */
        for (int i = 0; i < ALBUM_THUMBNAIL_CACHE_SIZE; i++) {
            if (album_thumbnail_cache[i].known && album_thumbnail_cache[i].key_kind == key_kind &&
                album_thumbnail_cache[i].key == key) {
                victim = i;
                break;
            }
        }
        /* A TEMPORARY failure must not blank a previously valid entry for
         * this song. pixels is NULL whenever commit_known is false. */
        if (victim >= 0 && !commit_known) {
            free(pixels);
            return;
        }
        if (victim < 0) {
            uint32_t oldest = UINT32_MAX;
            for (int i = 0; i < ALBUM_THUMBNAIL_CACHE_SIZE; i++) {
                if (!album_thumbnail_cache[i].known) { victim = i; break; }
                if (album_thumbnail_cache[i].last_use < oldest) {
                    oldest = album_thumbnail_cache[i].last_use;
                    victim = i;
                }
            }
        }
        album_thumbnail_cache_entry_t * e = &album_thumbnail_cache[victim];
        uint8_t * retired_pixels = e->pixels;
        if (retired_pixels) {
            /* A leading lv_image can retain &e->dsc after its row last ran
             * the decorator. Freeing pixels first made that image descriptor
             * point into released heap memory until the row happened to be
             * recycled: a redraw/scroll use-after-free on libraries larger
             * than the LRU. Mark this as a known no-art entry temporarily and
             * repaint every visible row so all references are detached before
             * releasing/reusing the slot. */
            e->pixels = NULL;
            e->dsc.data = NULL;
            victim_visible_lost = release_thumbnail_dsc(&e->dsc); /* after clearing, so a re-decorate cannot re-attach it */
            free(retired_pixels);
        }
        memset(e, 0, sizeof(*e));
        if (commit_known) {
            e->key = key;
            e->key_kind = key_kind;
            e->known = true;
            e->pixels = pixels;
            e->last_use = ++album_thumbnail_use_counter;
            if (e->pixels) {
                e->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
                e->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
                e->dsc.header.w = ALBUM_THUMBNAIL_PX;
                e->dsc.header.h = ALBUM_THUMBNAIL_PX;
                e->dsc.header.stride = ALBUM_THUMBNAIL_PX * 2;
                e->dsc.data = e->pixels;
                e->dsc.data_size = ALBUM_THUMBNAIL_PX * ALBUM_THUMBNAIL_PX * 2;
            }
        } else {
            free(pixels);
        }
        album_thumbnail_queue_remove(key_kind, key);
        /* Only now is the victim slot fully replaced. */
        if (victim_visible_lost) refresh_thumbnail_list_visible(album_thumbnail_active_list);
}

static bool album_thumbnail_commit_boot_preload(void) {
    int expected = ALBUM_BOOT_PRELOAD_PENDING;
    if (!atomic_compare_exchange_strong(&album_boot_preload_state, &expected,
                                         ALBUM_BOOT_PRELOAD_CLAIMED))
        return false;
    bool committed = false;
    if (album_boot_preload_result_cache_epoch == atomic_load(&album_thumbnail_cache_epoch)) {
        if (album_boot_preload_result_kind == ALBUM_BOOT_RESULT_ALBUM) {
            album_thumbnail_cache_commit(album_boot_preload_result_key_kind,
                album_boot_preload_result_key, album_boot_preload_result_song_id,
                album_boot_preload_result_pixels, album_boot_preload_result_have_source_mtime,
                album_boot_preload_result_source_mtime, false);
            committed = true;
        } else {
            if (album_boot_preload_result_artist_has_alias ||
                album_boot_preload_result_artist_negative_confirmed) {
                artist_thumbnail_alias_cache_commit(album_boot_preload_result_artist_key,
                    album_boot_preload_result_artist_group_kind,
                    album_boot_preload_result_artist_has_alias,
                    album_boot_preload_result_alias_album_key,
                    album_boot_preload_result_alias_song_id);
                album_thumbnail_queue_remove_artist(album_boot_preload_result_artist_key,
                                                    album_boot_preload_result_artist_group_kind);
                if (album_boot_preload_result_artist_has_alias)
                    album_thumbnail_cache_remove(THUMBNAIL_KEY_ARTIST,
                                                  album_boot_preload_result_artist_key);
                committed = true;
            }
            if (album_boot_preload_result_pixels) {
                album_thumbnail_cache_commit(album_boot_preload_result_key_kind,
                    album_boot_preload_result_key, album_boot_preload_result_song_id,
                    album_boot_preload_result_pixels, false, 0, false);
                committed = true;
            }
        }
        album_boot_preload_result_pixels = NULL;
    } else {
        free(album_boot_preload_result_pixels);
        album_boot_preload_result_pixels = NULL;
    }
    /* The warmer now runs while list screens are open: repaint only when a
     * row on screen actually shows what was committed, not once per album. */
    /* An artist result may carry an album's pixels too: check both keys. */
    bool shown = (album_boot_preload_result_kind == ALBUM_BOOT_RESULT_ARTIST &&
                  thumbnail_active_list_shows(THUMBNAIL_KEY_ARTIST, album_boot_preload_result_artist_key)) ||
                 thumbnail_active_list_shows(album_boot_preload_result_key_kind, album_boot_preload_result_key);
    if (committed && shown) refresh_thumbnail_list_visible(album_thumbnail_active_list);
    atomic_store(&album_boot_preload_state, ALBUM_BOOT_PRELOAD_EMPTY);
    return true;
}

static void album_thumbnail_discard_boot_preload(void) {
    int expected = ALBUM_BOOT_PRELOAD_PENDING;
    if (!atomic_compare_exchange_strong(&album_boot_preload_state, &expected,
                                         ALBUM_BOOT_PRELOAD_CLAIMED))
        return;
    free(album_boot_preload_result_pixels);
    album_boot_preload_result_pixels = NULL;
    atomic_store(&album_boot_preload_state, ALBUM_BOOT_PRELOAD_EMPTY);
}

static bool album_thumb_gen_cached_key(thumbnail_key_kind_t key_kind, uint64_t key) {
    for (int i = 0; i < album_thumb_gen_cached_key_count; i++)
        if (album_thumb_gen_cached_key_kinds[i] == key_kind &&
            album_thumb_gen_cached_keys[i] == key) return true;
    return false;
}

static bool album_thumbnail_sized_cache_hit(const albumart_info_t * info, char * found, size_t found_size) {
    return albumart_sized_thumb_fresh(info, ALBUM_THUMBNAIL_PX, ALBUM_THUMBNAIL_PX, found, found_size);
}

static bool album_player_cache_hit(const albumart_info_t * info, char * found, size_t found_size) {
    return albumart_generated_cache_fresh(info, ALBUM_PLAYER_CACHE_PX, ALBUM_PLAYER_CACHE_PX,
                                          found, found_size);
}

#define THUMBNAIL_SIDECAR_MAX_BYTES (2U * 1024U * 1024U)
#define ALBUM_ART_METADATA_TIMEOUT_MS 5000
/* Small startup allowance; the helper has a bounded address-space budget
 * and the returned picture is admitted at its actual compressed size. */
#define ALBUM_ART_METADATA_START_BYTES (1024U * 1024U)

/* Bound on how many other songs from the same album to probe for embedded
 * art once the album's own representative song (first_song_id) has none
 * and no sidecar exists either -- covers rips where only some tracks carry
 * embedded art (e.g. a short intro/interlude or bonus track).
 * Kept small: each probe is a full isolated-process extraction. */
#define ALBUM_ART_SIBLING_FALLBACK_MAX 5

/* Persistent warmer state flags */
static pthread_t album_thumb_gen_thread;
static atomic_bool album_thumb_gen_active;
static atomic_bool album_thumb_gen_cancel;
static atomic_int album_thumb_gen_generation;
static bool album_thumb_gen_thread_joinable;
static atomic_int album_thumb_gen_done_count;
static atomic_int album_thumb_gen_total_count;
static atomic_int album_thumb_gen_cache_epoch;
/* Experimental (Developer Options): keep reading already generated 72px
 * covers while audio plays. playback_pass_epoch records the cache epoch whose
 * playback-time pass finished, so the retry path does not rerun it every
 * backoff; anything that needs real extraction waits for playback to stop. */
static atomic_bool album_thumb_gen_covers_during_playback;
static atomic_int album_thumb_gen_playback_pass_epoch = -1;

void gui_library_set_covers_during_playback(bool enabled) {
    atomic_store(&album_thumb_gen_covers_during_playback, enabled);
}

static bool album_thumb_gen_paused_for_playback(bool user_visible) {
    return !user_visible && audio_is_playing() && !atomic_load(&album_thumb_gen_covers_during_playback);
}
static atomic_bool album_thumb_gen_retry_pending;
static uint32_t album_thumb_gen_retry_tick;
static volatile bool sd_format_active = false;

static bool album_thumb_gen_should_cancel(int my_generation) {
    return atomic_load(&album_thumb_gen_cancel) ||
           atomic_load(&album_thumb_gen_generation) != my_generation ||
           atomic_load(&album_thumb_gen_cache_epoch) != atomic_load(&album_thumbnail_cache_epoch);
}

static void cancel_album_thumbnail_generation(void) {
    if (!atomic_load(&album_thumb_gen_active)) return;
    atomic_store(&album_thumb_gen_cancel, true);
    atomic_fetch_add(&album_thumb_gen_generation, 1);
}

/* Waits for thumbnail generation thread completion. Requires both the
 * joinable flag and a non-NULL thread handle before calling pthread_join(). */
static void reap_album_thumbnail_generation(void) {
    if (!album_thumb_gen_thread_joinable) return;
    album_thumb_gen_thread_joinable = false;
    if (!album_thumb_gen_thread) return;
    pthread_join(album_thumb_gen_thread, NULL);
}

/* Dynamic cancellation callbacks */
static bool album_thumb_gen_cancel_cb(void * user_data) {
    int my_gen = (int) (intptr_t) user_data;
    return album_thumb_gen_should_cancel(my_gen) ||
           (!album_thumb_gen_user_visible && album_thumbnail_lazy_work_pending());
}

static bool album_thumbnail_cancel_cb(void * user_data) {
    int cache_epoch = (int) (intptr_t) user_data;
    return cache_epoch != atomic_load(&album_thumbnail_cache_epoch);
}

typedef struct {
    int kind;
    int cache_epoch;
    thumbnail_key_kind_t key_kind;
    uint64_t key;
    int64_t song_id;
    uint8_t * pixels;
    bool have_source_mtime;
    time_t source_mtime;
    uint64_t artist_key;
    metadata_db_group_kind_t artist_group_kind;
    bool artist_has_alias;
    bool artist_negative_confirmed;
    uint64_t alias_album_key;
    int64_t alias_song_id;
} album_thumbnail_handoff_t;

static bool album_thumb_gen_handoff_result(int my_generation,
                                            const album_thumbnail_handoff_t * result) {
    while (atomic_load(&album_boot_preload_state) != ALBUM_BOOT_PRELOAD_EMPTY) {
        if (album_thumb_gen_should_cancel(my_generation)) {
            free(result->pixels);
            return false;
        }
        usleep(5000);
    }
    if (album_thumb_gen_should_cancel(my_generation)) {
        free(result->pixels);
        return false;
    }

    atomic_store(&album_boot_preload_state, ALBUM_BOOT_PRELOAD_CLAIMED);
    album_boot_preload_result_kind = result->kind;
    album_boot_preload_result_cache_epoch = result->cache_epoch;
    album_boot_preload_result_key_kind = result->key_kind;
    album_boot_preload_result_key = result->key;
    album_boot_preload_result_song_id = result->song_id;
    album_boot_preload_result_pixels = result->pixels;
    album_boot_preload_result_have_source_mtime = result->have_source_mtime;
    album_boot_preload_result_source_mtime = result->source_mtime;
    album_boot_preload_result_artist_key = result->artist_key;
    album_boot_preload_result_artist_group_kind = result->artist_group_kind;
    album_boot_preload_result_artist_has_alias = result->artist_has_alias;
    album_boot_preload_result_artist_negative_confirmed = result->artist_negative_confirmed;
    album_boot_preload_result_alias_album_key = result->alias_album_key;
    album_boot_preload_result_alias_song_id = result->alias_song_id;
    atomic_store(&album_boot_preload_state, ALBUM_BOOT_PRELOAD_PENDING);

    while (atomic_load(&album_boot_preload_state) != ALBUM_BOOT_PRELOAD_EMPTY) {
        if (album_thumb_gen_should_cancel(my_generation)) {
            int expected_state = ALBUM_BOOT_PRELOAD_PENDING;
            if (atomic_compare_exchange_strong(&album_boot_preload_state, &expected_state,
                                               ALBUM_BOOT_PRELOAD_CLAIMED)) {
                free(album_boot_preload_result_pixels);
                album_boot_preload_result_pixels = NULL;
                atomic_store(&album_boot_preload_state, ALBUM_BOOT_PRELOAD_EMPTY);
                return false;
            }
        }
        usleep(5000);
    }
    return true;
}

static time_t album_source_mtime_with_art_mtime(const song_row_t * song, uint32_t art_mtime) {
    time_t max_mtime = (time_t) art_mtime;
    struct stat st;
    if (song && song->path[0] && stat(song->path, &st) == 0) {
        if (st.st_mtime > max_mtime) max_mtime = st.st_mtime;
        /* Parent folder mtime (detects adding or modifying cover.jpg) */
        char dir[PATH_MAX];
        snprintf(dir, sizeof(dir), "%s", song->path);
        char * slash = strrchr(dir, '/');
        if (slash) {
            *slash = '\0';
            if (stat(dir, &st) == 0 && st.st_mtime > max_mtime) max_mtime = st.st_mtime;
        }
    }
    return max_mtime;
}

static time_t album_source_mtime(const song_row_t * song, const albumart_info_t * info) {
    return album_source_mtime_with_art_mtime(song, albumart_source_mtime(info));
}

/* The warmer builds the player-sized cache from the original compressed art
 * while it is already resident for the 72px decode.  Keeping this out of the
 * visible thumbnail path prevents scrolling from triggering a second large
 * decode; the player can then open the persistent 480px BMP without any
 * metadata extraction or lazy source decode.
 *
 * When a fresh 480px decode actually runs (warmer, cache miss) and is stored,
 * *out_player_pixels receives those pixels so the caller can derive the 72px
 * thumbnail from them instead of entropy-decoding the same source a second
 * time.  On every skip path (not warmer, cache already present) and every
 * failure, *out_player_pixels is NULL -- caller then does the 72px decode
 * itself, same as before.  Caller owns and must free() a non-NULL return. */
/* Set by the warmer body on its own thread: also materialize the 480px
 * player cache while decoding. Independent of the coordinator priority, so
 * an explicit reload can run at thumbnail priority during playback. */
static __thread bool album_art_materialize_player;
/* Set when the last decode on this thread returned a user's own sized
 * thumbnail (a valid cover.72x72 beside the music) instead of writing a
 * generated 72px copy. */
static __thread bool album_art_thumb_from_user_file;

static cover_decode_result_t album_thumbnail_maybe_store_player_cache(
        const albumart_info_t * info, const uint8_t * data, uint32_t size,
        artwork_priority_t prio, artwork_cancel_fn cancel_cb, void * user_data,
        uint16_t ** out_player_pixels) {
    if (out_player_pixels) *out_player_pixels = NULL;
    if (!album_art_materialize_player || !info || !data || size == 0)
        return COVER_DECODE_OK;
    char found[PATH_MAX];
    if (album_player_cache_hit(info, found, sizeof(found))) return COVER_DECODE_OK;

    uint16_t * pixels = NULL;
    cover_decode_result_t res = cover_decode_to_rgb565_ex(
        data, size, ALBUM_PLAYER_CACHE_PX, ALBUM_PLAYER_CACHE_PX, prio,
        cancel_cb, user_data, &pixels);
    if (res == COVER_DECODE_OK && pixels &&
        !albumart_store_rgb565(info, ALBUM_PLAYER_CACHE_PX, ALBUM_PLAYER_CACHE_PX, pixels))
        res = COVER_DECODE_FAIL_ALLOC;
    if (res == COVER_DECODE_OK && pixels && out_player_pixels) {
        *out_player_pixels = pixels;
        pixels = NULL;
    }
    free(pixels);
    return res;
}

/* 72px pixels: cover-fit from a just-decoded 480px buffer when the warmer
 * actually produced one, otherwise a single decode at thumbnail size. */
static cover_decode_result_t album_thumbnail_pixels_from_source(
        const uint8_t * data, uint32_t size, const uint16_t * player_pixels,
        artwork_priority_t prio, artwork_cancel_fn cancel_cb, void * user_data,
        uint16_t ** out_pixels) {
    if (player_pixels) {
        *out_pixels = cover_resize_rgb565(player_pixels,
                                          ALBUM_PLAYER_CACHE_PX, ALBUM_PLAYER_CACHE_PX,
                                          ALBUM_THUMBNAIL_PX, ALBUM_THUMBNAIL_PX);
        return *out_pixels ? COVER_DECODE_OK : COVER_DECODE_FAIL_ALLOC;
    }
    return cover_decode_to_rgb565_ex(data, size, ALBUM_THUMBNAIL_PX, ALBUM_THUMBNAIL_PX,
                                     prio, cancel_cb, user_data, out_pixels);
}

/* On-demand fallback for the Playing Now page when the warmer hasn't
 * produced the player-sized cache for this track yet: same source order as
 * album_thumbnail_load_or_decode_ex() (external sidecar, then embedded
 * picture via the bounded/isolated extraction below -- never a plain
 * metadata_read(), which can OOM on a huge APIC/covr tag), decodes and
 * persists the cache, and returns display-sized pixels so the caller has
 * something to show immediately instead of waiting for the warmer.
 *
 * *out_no_art_confirmed is set true only when this track was actually
 * determined to have no art anywhere (safe for the caller to stop asking
 * about it), and left false on every transient failure (coordinator/
 * memory-pressure admission refused, isolated-helper fork/pipe/timeout
 * failure, cancellation) -- a caller keying a negative cache off this must
 * not do so on a transient false, or a passing resource hiccup would
 * permanently hide art that is actually there. */
/* Shared by both sources below: decodes+stores the 480px player cache from
 * BORROWED `data`/`size` (caller still owns and must free it) and, on
 * success, resizes it into *out_pixels for immediate display. Deliberately
 * makes no negative-cache/"no art confirmed" decision of its own -- source
 * selection, input ownership, and confirmation semantics all stay the
 * caller's responsibility, since a permanent failure decoding ONE source
 * must not by itself confirm "no art anywhere" while another source still
 * has a chance (see gui_library_generate_player_cover()'s own comment). */
static cover_decode_result_t generate_player_cover_from_data(
        const albumart_info_t * info, const uint8_t * data, uint32_t size,
        artwork_cancel_fn cancel_cb, void * user_data, uint16_t ** out_pixels) {
    uint16_t * cache_pixels = NULL;
    cover_decode_result_t res = cover_decode_to_rgb565_ex(
        data, size, ALBUM_PLAYER_CACHE_PX, ALBUM_PLAYER_CACHE_PX,
        ARTWORK_PRIO_PLAYER, cancel_cb, user_data, &cache_pixels);
    if (res == COVER_DECODE_OK && cache_pixels) {
        if (cancel_cb && cancel_cb(user_data)) {
            free(cache_pixels);
            return COVER_DECODE_FAIL_CANCELLED;
        }
        albumart_store_rgb565(info, ALBUM_PLAYER_CACHE_PX, ALBUM_PLAYER_CACHE_PX, cache_pixels);
        *out_pixels = cover_resize_rgb565(cache_pixels, ALBUM_PLAYER_CACHE_PX, ALBUM_PLAYER_CACHE_PX,
                                          COVER_ART_WIDTH, COVER_ART_HEIGHT);
        if (!*out_pixels) res = COVER_DECODE_FAIL_ALLOC;
    }
    free(cache_pixels);
    return res;
}

bool gui_library_generate_player_cover(const char * track_path, const char * artist,
                                       const char * album, const char * album_artist,
                                       artwork_cancel_fn cancel_cb, void * user_data,
                                       uint16_t ** out_pixels, bool * out_no_art_confirmed) {
    *out_pixels = NULL;
    if (out_no_art_confirmed) *out_no_art_confirmed = false;
    if (!track_path || !track_path[0]) return false;

    albumart_info_t info = {0};
    snprintf(info.path, sizeof(info.path), "%s", track_path);
    snprintf(info.artist, sizeof(info.artist), "%s", artist ? artist : "");
    snprintf(info.album, sizeof(info.album), "%s", album ? album : "");
    snprintf(info.albumartist, sizeof(info.albumartist), "%s", album_artist ? album_artist : "");

    /* Step 1: try an external sidecar file first. A permanent DECODE failure
     * here (corrupt/oversized/unsupported -- as opposed to merely failing
     * to LOAD, or a transient/cancelled result) falls through to embedded
     * extraction below instead of giving up -- this function used to keep
     * whatever sidecar bytes loaded and never attempt embedded art at all
     * once ALBUMART_LOAD_OK was seen, permanently hiding real embedded art
     * behind one corrupt sidecar file. */
    char found[PATH_MAX];
    if (albumart_search_files(&info, "", found, sizeof(found))) {
        uint8_t * sidecar_data = NULL;
        uint32_t sidecar_size = 0;
        albumart_load_result_t load = albumart_load_file_ex(found, &sidecar_data, &sidecar_size,
                                                             THUMBNAIL_SIDECAR_MAX_BYTES, ARTWORK_PRIO_PLAYER);
        if (load == ALBUMART_LOAD_TEMPORARY) return false; /* transient -- do not confirm "no art" */
        if (load == ALBUMART_LOAD_OK) {
            DB_LOG("ART_PLAYER", "generate_source path=%s source=sidecar", track_path);
            cover_decode_result_t res = generate_player_cover_from_data(
                &info, sidecar_data, sidecar_size, cancel_cb, user_data, out_pixels);
            free(sidecar_data);
            if (res == COVER_DECODE_OK && *out_pixels) return true;
            if (res == COVER_DECODE_FAIL_CANCELLED) return false;
            if (cover_decode_result_is_temporary(res)) return false; /* transient -- do not confirm "no art" */
            /* Permanent sidecar decode failure -- fall through to embedded
             * extraction. Must NOT confirm "no art" here on its own; only
             * the fully-exhausted embedded path below may do that. */
        }
        /* ALBUMART_LOAD_INVALID (missing/unreadable/oversized) also falls
         * through here with no special-casing needed. */
    }

    /* Step 2: embedded picture from the audio file's own tags. Reached when
     * there's no sidecar, the sidecar failed to load, or it loaded but
     * failed to decode permanently. */
    artwork_acquire_result_t admission = artwork_coordinator_acquire(
        ARTWORK_PRIO_PLAYER, ALBUM_ART_METADATA_START_BYTES, 300, cancel_cb, user_data);
    if (admission != ARTWORK_ACQUIRE_OK) return false; /* transient -- coordinator busy/cancelled */
    track_metadata_t meta;
    memset(&meta, 0, sizeof(meta));
    metadata_artwork_result_t artwork_result = metadata_read_artwork_isolated(
        track_path, &meta, ALBUM_ART_METADATA_TIMEOUT_MS, ARTWORK_PRIO_PLAYER);
    artwork_coordinator_release(ARTWORK_PRIO_PLAYER);
    uint8_t * embedded_data = meta.picture_data;
    uint32_t embedded_size = meta.picture_size;
    free(meta.lyrics);
    if (!info.artist[0]) snprintf(info.artist, sizeof(info.artist), "%s", meta.artist);
    if (!info.album[0]) snprintf(info.album, sizeof(info.album), "%s", meta.album);
    if (!info.albumartist[0]) snprintf(info.albumartist, sizeof(info.albumartist), "%s", meta.album_artist);

    if (cancel_cb && cancel_cb(user_data)) {
        free(embedded_data);
        return false;
    }

    if (!embedded_data || embedded_size == 0) {
        free(embedded_data);
        /* Confirmed empty only if the embedded-picture helper actually ran
         * to completion and found nothing (or found something invalid) --
         * not if it merely failed to run (fork/pipe/timeout). Reaching here
         * already means embedded extraction had its chance regardless of
         * whether a sidecar file existed or how it failed. */
        if (out_no_art_confirmed && (artwork_result == METADATA_ARTWORK_NOT_FOUND ||
                                     artwork_result == METADATA_ARTWORK_INVALID ||
                                     artwork_result == METADATA_ARTWORK_TOO_LARGE))
            *out_no_art_confirmed = true;
        return false;
    }

    DB_LOG("ART_PLAYER", "generate_source path=%s source=embedded", track_path);
    cover_decode_result_t res = generate_player_cover_from_data(
        &info, embedded_data, embedded_size, cancel_cb, user_data, out_pixels);
    free(embedded_data);
    if (res != COVER_DECODE_OK && out_no_art_confirmed && cover_decode_result_is_permanent(res))
        *out_no_art_confirmed = true; /* corrupt/oversized source -- won't change until re-tagged */
    return res == COVER_DECODE_OK;
}

/* Result of one embedded-art probe against a single sibling song file --
 * distinguishes "this song genuinely has no picture" (keep trying other
 * siblings) from every transient/cancelled outcome (stop probing without
 * ever letting the caller record a permanent album-level failure over a
 * passing interruption -- mirrors how Step 3 above treats the primary
 * song's own cancelled/temporary outcomes). */
typedef enum {
    SIBLING_ART_FOUND,      /* success -- *out_pixels populated */
    SIBLING_ART_NOT_FOUND,  /* probe completed, this song has no usable picture */
    SIBLING_ART_TEMPORARY,  /* busy/low-memory/timeout/temporary decode failure */
    SIBLING_ART_CANCELLED,  /* cancelled or suspended -- record nothing */
} sibling_art_result_t;

/* One bounded attempt at embedded-picture extraction from a single sibling
 * song file, used only by the Step 3b fallback below (the primary
 * representative song still goes through the separately inlined Step 3
 * above, which this mirrors). On success, also persists the sized
 * thumbnail/player caches keyed by `info`, i.e. by album identity, not by
 * which song file supplied the bytes. */
static sibling_art_result_t album_thumbnail_try_embedded_art_from_path(
        const albumart_info_t * info, const char * path, artwork_priority_t prio,
        artwork_cancel_fn cancel_cb, void * user_data, uint16_t ** out_pixels) {
    artwork_acquire_result_t admission = artwork_coordinator_acquire(
        prio, ALBUM_ART_METADATA_START_BYTES, 300, cancel_cb, user_data);
    if (admission == ARTWORK_ACQUIRE_CANCELLED || admission == ARTWORK_ACQUIRE_SUSPENDED)
        return SIBLING_ART_CANCELLED;
    if (admission != ARTWORK_ACQUIRE_OK)
        return SIBLING_ART_TEMPORARY;

    track_metadata_t meta;
    memset(&meta, 0, sizeof(meta));
    metadata_artwork_result_t metadata_result =
        metadata_read_artwork_isolated(path, &meta, ALBUM_ART_METADATA_TIMEOUT_MS, prio);
    artwork_coordinator_release(prio);
    uint8_t * data = meta.picture_data;
    uint32_t size = meta.picture_size;
    free(meta.lyrics);

    if (!data || size == 0) {
        free(data);
        return metadata_result == METADATA_ARTWORK_TEMPORARY_FAILURE
            ? SIBLING_ART_TEMPORARY : SIBLING_ART_NOT_FOUND;
    }

    uint16_t * player_pixels = NULL;
    cover_decode_result_t player_res = album_thumbnail_maybe_store_player_cache(
        info, data, size, prio, cancel_cb, user_data, &player_pixels);
    if (player_res == COVER_DECODE_FAIL_CANCELLED) {
        free(player_pixels);
        free(data);
        return SIBLING_ART_CANCELLED;
    }
    cover_decode_result_t res = album_thumbnail_pixels_from_source(
        data, size, player_pixels, prio, cancel_cb, user_data, out_pixels);
    free(player_pixels);
    free(data);
    if (res == COVER_DECODE_OK && *out_pixels) {
        albumart_store_rgb565(info, ALBUM_THUMBNAIL_PX, ALBUM_THUMBNAIL_PX, *out_pixels);
        return SIBLING_ART_FOUND;
    }
    if (res == COVER_DECODE_FAIL_CANCELLED) return SIBLING_ART_CANCELLED;
    if (cover_decode_result_is_temporary(res)) return SIBLING_ART_TEMPORARY;
    return SIBLING_ART_NOT_FOUND;
}

/* Rockbox albumart search, then embedded picture. A successful decode is
 * written as MUSIC_ROOT_DIR/.compas/albumart/<artist>-<album>.72x72.bmp
 * so the next pass is a small BMP load instead of a JPEG/PNG decode.
 * Albums without artist+album tags still decode, but cannot be stored.
 * Checks the negative failure cache first to prevent repeated failed decodes.
 * If sized thumbnail or sidecar is invalid/unreadable, falls through to embedded art. */
static bool album_thumbnail_load_or_decode_with_mtime(const song_row_t * song, artwork_priority_t prio,
                                                       artwork_cancel_fn cancel_cb, void * user_data,
                                                       time_t source_mtime, bool sized_cache_checked,
                                                       const char * sidecar_path, bool sidecar_checked,
                                                       uint16_t ** out_pixels) {
    *out_pixels = NULL;
    if (!song || !song->path[0]) return false;

    albumart_info_t info;
    albumart_info_from_song_row(song, &info);

    uint64_t mtime_t0 = db_log_now_ms();
    DB_LOG("ART_LAZY", "source_mtime_ms=%llu song=%lld", (unsigned long long) (db_log_now_ms() - mtime_t0),
           (long long) song->id);

    artwork_fail_reason_t fail_reason = ARTWORK_FAIL_NONE;
    if (artwork_failure_cache_is_blocked(song->id, source_mtime, &fail_reason)) return false;

    char found[PATH_MAX];
    char thumbnail_found[PATH_MAX];
    uint8_t * data = NULL;
    uint32_t size = 0;
    bool have_cached_thumbnail = false;
    album_art_thumb_from_user_file = false;

    /* Step 1: Try sized Rockbox thumbnail cache (.72x72.bmp) */
    bool step1_hit = !sized_cache_checked &&
                     album_thumbnail_sized_cache_hit(&info, thumbnail_found, sizeof(thumbnail_found));
    DB_LOG("ART_LAZY", "step1 song=%lld artist=%s albumartist=%s album=%s key=%016llx hit=%d found=%s",
           (long long) song->id, info.artist, info.albumartist, info.album,
           (unsigned long long) albumart_debug_thumbnail_key(&info), step1_hit,
           step1_hit ? thumbnail_found : "");
    if (step1_hit) {
        albumart_load_result_t load = albumart_load_file_ex(thumbnail_found, &data, &size, THUMBNAIL_SIDECAR_MAX_BYTES, prio);
        if (load == ALBUMART_LOAD_TEMPORARY) {
            artwork_failure_cache_record(song->id, source_mtime, ARTWORK_FAIL_TEMPORARY);
            return false; /* Do not delete a valid cache file under memory pressure. */
        }
        if (load == ALBUMART_LOAD_OK) {
            cover_decode_result_t res = cover_decode_to_rgb565_ex(data, size, ALBUM_THUMBNAIL_PX, ALBUM_THUMBNAIL_PX,
                                                                  prio, cancel_cb, user_data, out_pixels);
            free(data);
            data = NULL;
            size = 0;
            if (res == COVER_DECODE_OK && *out_pixels) {
                /* A warmer pass must continue to the original source when
                 * only the 72px cache exists, so it can materialize 480px.
                 * Keep the fresh 72px pixels while doing that work; do not
                 * decode or rewrite the thumbnail a second time. */
                if (!album_art_materialize_player || album_player_cache_hit(&info, found, sizeof(found)))
                    return true;
                have_cached_thumbnail = true;
                album_art_thumb_from_user_file = !albumart_is_generated_cache_file(thumbnail_found);
            }
            if (res == COVER_DECODE_FAIL_CANCELLED) return false;
            if (cover_decode_result_is_temporary(res)) {
                artwork_failure_cache_record(song->id, source_mtime, ARTWORK_FAIL_TEMPORARY);
                return false;
            }
            /* Corrupt sized thumbnail -> drop it (only if the app generated it;
             * the search also finds user images) and fall through to sources */
            if (res != COVER_DECODE_OK && albumart_is_generated_cache_file(thumbnail_found))
                unlink(thumbnail_found);
        } else if (albumart_is_generated_cache_file(thumbnail_found)) {
            unlink(thumbnail_found);
        }
    }

    /* Step 2: Try external sidecar file (cover.jpg, folder.jpg, etc.) */
    bool sidecar_searched = false;
    if (sidecar_checked) {
        if (sidecar_path && sidecar_path[0]) {
            int sidecar_len = snprintf(found, sizeof(found), "%s", sidecar_path);
            sidecar_searched = sidecar_len > 0 && (size_t) sidecar_len < sizeof(found);
        }
    } else {
        sidecar_searched = albumart_search_files(&info, "", found, sizeof(found));
    }
    if (sidecar_searched) {
        albumart_load_result_t load = albumart_load_file_ex(found, &data, &size, THUMBNAIL_SIDECAR_MAX_BYTES, prio);
        if (load == ALBUMART_LOAD_TEMPORARY) {
            if (have_cached_thumbnail) return true;
            artwork_failure_cache_record(song->id, source_mtime, ARTWORK_FAIL_TEMPORARY);
            return false;
        }
        if (load == ALBUMART_LOAD_OK) {
            uint16_t * player_pixels = NULL;
            cover_decode_result_t player_res = album_thumbnail_maybe_store_player_cache(
                &info, data, size, prio, cancel_cb, user_data, &player_pixels);
            if (player_res == COVER_DECODE_FAIL_CANCELLED) {
                free(player_pixels);
                free(data);
                if (have_cached_thumbnail) {
                    free(*out_pixels);
                    *out_pixels = NULL;
                }
                return false;
            }
            if (have_cached_thumbnail) {
                free(player_pixels);
                free(data);
                return true;
            }
            /* Any OTHER temporary player-cache failure (memory admission,
             * coordinator busy) must not abort the whole attempt --
             * player_pixels is already NULL on any player_res failure (see
             * album_thumbnail_maybe_store_player_cache()'s own contract), so
             * falling through to the much smaller thumbnail-only decode
             * below costs nothing extra and can very plausibly still
             * succeed under the same pressure that blocked the larger 480px
             * attempt. Only the outcome of THAT decode (checked below) gets
             * recorded/returned. */
            cover_decode_result_t res = album_thumbnail_pixels_from_source(
                data, size, player_pixels, prio, cancel_cb, user_data, out_pixels);
            free(player_pixels);
            free(data);
            data = NULL;
            size = 0;
            if (res == COVER_DECODE_OK && *out_pixels) {
                albumart_store_rgb565(&info, ALBUM_THUMBNAIL_PX, ALBUM_THUMBNAIL_PX, *out_pixels);
                return true;
            }
            if (res == COVER_DECODE_FAIL_CANCELLED) return false;
            if (cover_decode_result_is_temporary(res)) {
                artwork_failure_cache_record(song->id, source_mtime, ARTWORK_FAIL_TEMPORARY);
                return false;
            }
            /* Corrupt or oversized sidecar falls through to embedded art */
        }
    }

    /* Step 3: Try embedded picture from audio file */
    /* Serialize extraction with decoding too. The helper bounds parser
     * allocations, and the parent admits the actual returned picture size. */
    artwork_acquire_result_t admission = artwork_coordinator_acquire(
        prio, ALBUM_ART_METADATA_START_BYTES, 300, cancel_cb, user_data);
    if (admission == ARTWORK_ACQUIRE_CANCELLED || admission == ARTWORK_ACQUIRE_SUSPENDED) return false;
    if (admission != ARTWORK_ACQUIRE_OK) {
        artwork_failure_cache_record(song->id, source_mtime, ARTWORK_FAIL_TEMPORARY);
        return false;
    }
    track_metadata_t meta;
    memset(&meta, 0, sizeof(meta));
    metadata_artwork_result_t metadata_result =
        metadata_read_artwork_isolated(song->path, &meta, ALBUM_ART_METADATA_TIMEOUT_MS, prio);
    artwork_coordinator_release(prio);
    data = meta.picture_data;
    size = meta.picture_size;
    free(meta.lyrics);
    if (!info.artist[0]) snprintf(info.artist, sizeof(info.artist), "%s", meta.artist);
    if (!info.album[0]) snprintf(info.album, sizeof(info.album), "%s", meta.album);
    if (!info.albumartist[0]) snprintf(info.albumartist, sizeof(info.albumartist), "%s", meta.album_artist);

    if (data && size > 0) {
        uint16_t * player_pixels = NULL;
        cover_decode_result_t player_res = album_thumbnail_maybe_store_player_cache(
            &info, data, size, prio, cancel_cb, user_data, &player_pixels);
        if (player_res == COVER_DECODE_FAIL_CANCELLED) {
            free(player_pixels);
            free(data);
            if (have_cached_thumbnail) {
                free(*out_pixels);
                *out_pixels = NULL;
            }
            return false;
        }
        if (have_cached_thumbnail) {
            free(player_pixels);
            free(data);
            return true;
        }
        /* See the mirrored sidecar-path comment above (Step 2) -- any OTHER
         * temporary player-cache failure falls through to the smaller
         * thumbnail-only decode instead of aborting immediately. */
        cover_decode_result_t res = album_thumbnail_pixels_from_source(
            data, size, player_pixels, prio, cancel_cb, user_data, out_pixels);
        free(player_pixels);
        free(data);
        data = NULL;
        size = 0;
        if (res == COVER_DECODE_OK && *out_pixels) {
            albumart_store_rgb565(&info, ALBUM_THUMBNAIL_PX, ALBUM_THUMBNAIL_PX, *out_pixels);
            return true;
        }
        if (res == COVER_DECODE_FAIL_CANCELLED) return false;
        if (cover_decode_result_is_temporary(res)) {
            artwork_failure_cache_record(song->id, source_mtime, ARTWORK_FAIL_TEMPORARY);
            return false;
        }
    }

    if (metadata_result == METADATA_ARTWORK_TEMPORARY_FAILURE) {
        if (have_cached_thumbnail) return true;
        artwork_failure_cache_record(song->id, source_mtime, ARTWORK_FAIL_TEMPORARY);
        return false;
    }

    if (have_cached_thumbnail) return true;

    /* Step 3b: the representative song for this album may simply be a track
     * with no embedded picture of its own (e.g. a short intro/interlude or
     * bonus track) even though sibling tracks in the same album do carry
     * art -- and Step 2 already searched the whole folder for a sidecar, so
     * a missing sidecar here means there truly isn't one. Try a bounded
     * number of other songs from the same album before giving up on the album
     * entirely. A cancelled/suspended or merely transient outcome on any
     * probe stops the loop immediately without recording anything permanent --
     * only a probe that actually completed and found nothing counts toward
     * "try the next sibling". */
    if (info.album[0] && info.albumartist[0]) {
        song_row_t siblings[ALBUM_ART_SIBLING_FALLBACK_MAX];
        int sibling_count = metadata_db_get_album_songs(info.album, info.albumartist, 0,
                                                         siblings, ALBUM_ART_SIBLING_FALLBACK_MAX);
        for (int i = 0; i < sibling_count; i++) {
            if (siblings[i].id == song->id || !siblings[i].path[0]) continue;
            if (cancel_cb && cancel_cb(user_data)) return false;
            sibling_art_result_t sr = album_thumbnail_try_embedded_art_from_path(
                &info, siblings[i].path, prio, cancel_cb, user_data, out_pixels);
            if (sr == SIBLING_ART_FOUND) return true;
            if (sr == SIBLING_ART_CANCELLED) return false;
            if (sr == SIBLING_ART_TEMPORARY) {
                artwork_failure_cache_record(song->id, source_mtime, ARTWORK_FAIL_TEMPORARY);
                return false;
            }
            /* SIBLING_ART_NOT_FOUND -- try the next sibling */
        }
    }

    /* Step 4: No valid artwork found or permanent decode failure */
    artwork_failure_cache_record(song->id, source_mtime, ARTWORK_FAIL_PERMANENT);
    return false;
}

static bool album_thumbnail_load_or_decode_ex(const song_row_t * song, artwork_priority_t prio,
                                              artwork_cancel_fn cancel_cb, void * user_data,
                                              uint16_t ** out_pixels) {
    if (!song || !song->path[0]) {
        *out_pixels = NULL;
        return false;
    }
    albumart_info_t info;
    albumart_info_from_song_row(song, &info);
    time_t source_mtime = album_source_mtime(song, &info);
    return album_thumbnail_load_or_decode_with_mtime(song, prio, cancel_cb, user_data,
                                                      source_mtime, false, NULL, false, out_pixels);
}

typedef enum {
    ARTIST_THUMB_FOUND,
    ARTIST_THUMB_MISSING,
    ARTIST_THUMB_TEMPORARY,
    ARTIST_THUMB_CANCELLED,
    ARTIST_THUMB_OWN_IMAGE
} artist_thumbnail_result_t;

static bool thumbnail_directory_of(const char * path, char * out, size_t out_size) {
    if (!path || !path[0] || !out || out_size == 0) return false;
    snprintf(out, out_size, "%s", path);
    char * slash = strrchr(out, '/');
    if (!slash) return false;
    if (slash == out) slash[1] = '\0';
    else *slash = '\0';
    return true;
}

static bool thumbnail_parent_directory(const char * directory, char * out, size_t out_size) {
    if (!directory || !directory[0] || !out || out_size == 0) return false;
    snprintf(out, out_size, "%s", directory);
    size_t len = strlen(out);
    while (len > 1 && out[len - 1] == '/') out[--len] = '\0';
    char * slash = strrchr(out, '/');
    if (!slash) return false;
    if (slash == out) slash[1] = '\0';
    else *slash = '\0';
    return true;
}

static const char * thumbnail_directory_name(const char * path) {
    const char * slash = strrchr(path ? path : "", '/');
    return slash ? slash + 1 : (path ? path : "");
}

static bool artist_thumbnail_folder_for_song(const song_row_t * song, const char * artist,
                                              char * artist_dir, size_t artist_dir_size,
                                              char * album_dir, size_t album_dir_size) {
    char track_dir[PATH_MAX];
    if (!song || !thumbnail_directory_of(song->path, track_dir, sizeof(track_dir))) return false;
    snprintf(album_dir, album_dir_size, "%s", track_dir);
    if (albumart_is_disc_folder(thumbnail_directory_name(track_dir))) {
        if (!thumbnail_parent_directory(track_dir, album_dir, album_dir_size)) return false;
    }
    if (!thumbnail_parent_directory(album_dir, artist_dir, artist_dir_size)) return false;

    /* A library with tracks directly in each artist directory has no album
     * subdirectory to step through. Accept that layout only when the folder
     * itself matches the artist tag and its parent is the music root. */
    if (!strcmp(artist_dir, MUSIC_ROOT_DIR) && artist[0] &&
        strcasecmp(thumbnail_directory_name(album_dir), artist) == 0) {
        snprintf(artist_dir, artist_dir_size, "%s", album_dir);
        album_dir[0] = '\0';
    }

    if (!artist_dir[0] || strcmp(artist_dir, MUSIC_ROOT_DIR) == 0 ||
        (album_dir[0] && strcmp(artist_dir, album_dir) == 0))
        return false;
    struct stat st;
    return stat(artist_dir, &st) == 0 && S_ISDIR(st.st_mode);
}

static uint64_t artist_directory_signature(const char * artist, metadata_db_group_kind_t group_kind,
                                           const song_row_t * representative,
                                           const char * artist_dir) {
    uint64_t hash = UINT64_C(14695981039346656037);
    const unsigned char * name = (const unsigned char *) (artist ? artist : "");
    while (*name) hash = (hash ^ *name++) * UINT64_C(1099511628211);
    hash = (hash ^ (unsigned char) group_kind) * UINT64_C(1099511628211);
    const char * paths[] = { artist_dir, representative ? representative->path : NULL };
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        const char * path = paths[i];
        if (!path || !path[0]) continue;
        for (const unsigned char * p = (const unsigned char *) path; *p; p++)
            hash = (hash ^ *p) * UINT64_C(1099511628211);
        struct stat st;
        uint64_t mtime = stat(path, &st) == 0 ? (uint64_t) st.st_mtime : 0;
        for (int b = 0; b < 8; b++) hash = (hash ^ (unsigned char) (mtime >> (b * 8))) * UINT64_C(1099511628211);
    }

    int64_t album_count = metadata_db_count_albums_for_group(group_kind, artist);
    int album_total = album_count > INT_MAX ? INT_MAX : (int) album_count;
    group_row_t albums[16];
    for (int offset = 0; offset < album_total; ) {
        int want = album_total - offset;
        if (want > 16) want = 16;
        int got = metadata_db_get_albums_for_group(group_kind, artist, offset, want, albums);
        if (got <= 0) break;
        for (int i = 0; i < got; i++) {
            song_row_t song;
            if (!metadata_db_get_song_by_id(albums[i].first_song_id, &song)) continue;
            for (const unsigned char * p = (const unsigned char *) song.path; *p; p++)
                hash = (hash ^ *p) * UINT64_C(1099511628211);
            struct stat st;
            uint64_t mtime = stat(song.path, &st) == 0 ? (uint64_t) st.st_mtime : 0;
            for (int b = 0; b < 8; b++) hash = (hash ^ (unsigned char) (mtime >> (b * 8))) * UINT64_C(1099511628211);
            char directory[PATH_MAX], album_directory[PATH_MAX];
            if (thumbnail_directory_of(song.path, directory, sizeof(directory))) {
                struct stat dir_st;
                uint64_t dir_mtime = stat(directory, &dir_st) == 0 ? (uint64_t) dir_st.st_mtime : 0;
                for (int b = 0; b < 8; b++) hash = (hash ^ (unsigned char) (dir_mtime >> (b * 8))) * UINT64_C(1099511628211);
                if (albumart_is_disc_folder(thumbnail_directory_name(directory)) &&
                    thumbnail_parent_directory(directory, album_directory, sizeof(album_directory)) &&
                    stat(album_directory, &dir_st) == 0) {
                    dir_mtime = (uint64_t) dir_st.st_mtime;
                    for (int b = 0; b < 8; b++)
                        hash = (hash ^ (unsigned char) (dir_mtime >> (b * 8))) * UINT64_C(1099511628211);
                }
            }
        }
        offset += got;
        if (got < want) break;
    }
    return hash ? hash : 1;
}

static bool artist_thumbnail_load_persistent(const char * artist, uint32_t source_mtime,
                                             artwork_priority_t prio,
                                             artwork_cancel_fn cancel_cb, void * user_data,
                                             uint16_t ** out_pixels, bool * out_temporary) {
    *out_temporary = false;
    char found[PATH_MAX];
    if (!albumart_artist_sized_thumb_fresh(artist, source_mtime, ALBUM_THUMBNAIL_PX,
                                           ALBUM_THUMBNAIL_PX, found, sizeof(found)))
        return false;
    uint8_t * data = NULL;
    uint32_t size = 0;
    albumart_load_result_t load = albumart_load_file_ex(found, &data, &size,
                                                         THUMBNAIL_SIDECAR_MAX_BYTES,
                                                         prio);
    if (load == ALBUMART_LOAD_TEMPORARY) {
        *out_temporary = true;
        return false;
    }
    if (load != ALBUMART_LOAD_OK) {
        unlink(found);
        return false;
    }
    cover_decode_result_t res = cover_decode_to_rgb565_ex(data, size, ALBUM_THUMBNAIL_PX,
                                                           ALBUM_THUMBNAIL_PX,
                                                           prio,
                                                           cancel_cb, user_data, out_pixels);
    free(data);
    if (res == COVER_DECODE_OK && *out_pixels) return true;
    if (res == COVER_DECODE_FAIL_CANCELLED) return false;
    if (cover_decode_result_is_temporary(res)) *out_temporary = true;
    else unlink(found);
    return false;
}

typedef struct {
    uint16_t * pixels;
    thumbnail_key_kind_t pixel_key_kind;
    uint64_t pixel_key;
    int64_t pixel_song_id;
    time_t pixel_source_mtime;
    bool pixel_have_source_mtime;
    bool has_alias;
    uint64_t alias_album_key;
    int64_t alias_song_id;
    bool negative_confirmed;
} artist_thumbnail_resolution_t;

static void artist_thumbnail_set_alias(uint64_t album_key, int64_t song_id,
                                       artist_thumbnail_resolution_t * resolution) {
    resolution->has_alias = true;
    resolution->alias_album_key = album_key;
    resolution->alias_song_id = song_id;
}

static artist_thumbnail_result_t artist_thumbnail_from_album_group(
        const char * artist, metadata_db_group_kind_t group_kind,
        artwork_priority_t prio,
        artwork_cancel_fn cancel_cb, void * cancel_user_data,
        artist_thumbnail_resolution_t * resolution) {
    int64_t album_count = metadata_db_count_albums_for_group(group_kind, artist);
    int total = album_count > INT_MAX ? INT_MAX : (int) album_count;
    group_row_t albums[16];
    for (int offset = 0; offset < total; ) {
        if (cancel_cb && cancel_cb(cancel_user_data)) return ARTIST_THUMB_CANCELLED;
        int want = total - offset;
        if (want > 16) want = 16;
        int n = metadata_db_get_albums_for_group(group_kind, artist, offset, want, albums);
        if (n <= 0) break;
        for (int i = 0; i < n; i++) {
            if (cancel_cb && cancel_cb(cancel_user_data)) return ARTIST_THUMB_CANCELLED;
            song_row_t song;
            if (!metadata_db_get_song_by_id(albums[i].first_song_id, &song)) continue;
            albumart_info_t info;
            albumart_info_from_song_row(&song, &info);
            uint64_t album_key = song_album_thumbnail_key(&song);
            char sidecar_path[PATH_MAX];
            bool have_sidecar = false;
            uint32_t thumb_source_mtime = albumart_source_mtime_with_path(&info, sidecar_path,
                                                 sizeof(sidecar_path), &have_sidecar);
            time_t failure_mtime = album_source_mtime_with_art_mtime(&song, thumb_source_mtime);
            char cache_path[PATH_MAX];
            if (albumart_sized_thumb_fresh_with_source_mtime(&info, ALBUM_THUMBNAIL_PX,
                    ALBUM_THUMBNAIL_PX, thumb_source_mtime, cache_path, sizeof(cache_path))) {
                artist_thumbnail_set_alias(album_key, song.id, resolution);
                albumart_artist_alias_store(artist, (unsigned int) group_kind, album_key, song.id);
                return ARTIST_THUMB_FOUND;
            }

            artwork_fail_reason_t reason = ARTWORK_FAIL_NONE;
            if (artwork_failure_cache_is_blocked(song.id, failure_mtime, &reason) &&
                reason == ARTWORK_FAIL_PERMANENT)
                continue;

            uint16_t * pixels = NULL;
            bool found = album_thumbnail_load_or_decode_with_mtime(&song, prio,
                            cancel_cb, cancel_user_data,
                            failure_mtime, true, sidecar_path, true, &pixels);
            if (!found || !pixels) {
                free(pixels);
                if (cancel_cb && cancel_cb(cancel_user_data)) return ARTIST_THUMB_CANCELLED;
                if (artwork_failure_cache_is_blocked(song.id, failure_mtime, &reason) &&
                    reason == ARTWORK_FAIL_PERMANENT)
                    continue;
                return ARTIST_THUMB_TEMPORARY;
            }
            artwork_failure_cache_note_success(song.id);
            artist_thumbnail_set_alias(album_key, song.id, resolution);
            if (albumart_sized_thumb_fresh_with_source_mtime(&info, ALBUM_THUMBNAIL_PX,
                    ALBUM_THUMBNAIL_PX, thumb_source_mtime, cache_path, sizeof(cache_path)))
                albumart_artist_alias_store(artist, (unsigned int) group_kind, album_key, song.id);
            resolution->pixels = pixels;
            resolution->pixel_key_kind = THUMBNAIL_KEY_ALBUM;
            resolution->pixel_key = album_key;
            resolution->pixel_song_id = song.id;
            return ARTIST_THUMB_FOUND;
        }
        offset += n;
        if (n < want) break;
    }
    return ARTIST_THUMB_MISSING;
}

static artist_thumbnail_result_t artist_thumbnail_load_or_decode(
        int64_t representative_song_id, metadata_db_group_kind_t group_kind, const char * artist_name,
        artwork_priority_t prio, bool load_own_image,
        uint64_t artist_key, artwork_cancel_fn cancel_cb, void * cancel_user_data,
        artist_thumbnail_resolution_t * resolution) {
    memset(resolution, 0, sizeof(*resolution));
    resolution->pixel_key_kind = THUMBNAIL_KEY_ARTIST;
    resolution->pixel_key = artist_key;
    song_row_t representative;
    if (!metadata_db_get_song_by_id(representative_song_id, &representative)) return ARTIST_THUMB_TEMPORARY;
    const char * artist = artist_name && artist_name[0] ? artist_name :
                          group_kind == METADATA_DB_GROUP_ALBUM_ARTIST
                              ? representative.tags.album_artist : representative.tags.artist;
    if (!artist || !artist[0]) return ARTIST_THUMB_MISSING;

    char artist_dir[PATH_MAX] = "", album_dir[PATH_MAX] = "";
    bool have_artist_dir = artist_thumbnail_folder_for_song(&representative, artist,
                            artist_dir, sizeof(artist_dir), album_dir, sizeof(album_dir));
    char source_path[PATH_MAX];
    bool have_artist_source = have_artist_dir &&
        albumart_find_artist_sidecar(artist_dir, source_path, sizeof(source_path));
    if (have_artist_source && !load_own_image) return ARTIST_THUMB_OWN_IMAGE;
    if (have_artist_source) {
        struct stat source_st;
        uint32_t source_mtime = stat(source_path, &source_st) == 0
                                    ? (uint32_t) source_st.st_mtime : 0;
        bool cache_temporary = false;
        if (artist_thumbnail_load_persistent(artist, source_mtime, prio,
                cancel_cb, cancel_user_data,
                &resolution->pixels, &cache_temporary))
            return ARTIST_THUMB_FOUND;
        if (cache_temporary) return ARTIST_THUMB_TEMPORARY;

        uint8_t * data = NULL;
        uint32_t size = 0;
        albumart_load_result_t load = albumart_load_file_ex(source_path, &data, &size,
                                THUMBNAIL_SIDECAR_MAX_BYTES, prio);
        if (load == ALBUMART_LOAD_TEMPORARY) return ARTIST_THUMB_TEMPORARY;
        if (load == ALBUMART_LOAD_OK) {
            cover_decode_result_t decoded = cover_decode_to_rgb565_ex(data, size,
                    ALBUM_THUMBNAIL_PX, ALBUM_THUMBNAIL_PX, prio,
                    cancel_cb, cancel_user_data, &resolution->pixels);
            free(data);
            if (decoded == COVER_DECODE_OK && resolution->pixels) {
                albumart_artist_store_rgb565(artist, source_mtime, ALBUM_THUMBNAIL_PX,
                                             ALBUM_THUMBNAIL_PX, resolution->pixels);
                return ARTIST_THUMB_FOUND;
            }
            if (decoded == COVER_DECODE_FAIL_CANCELLED) return ARTIST_THUMB_CANCELLED;
            if (cover_decode_result_is_temporary(decoded)) return ARTIST_THUMB_TEMPORARY;
        }
        free(data);
    }

    uint64_t stored_album_key = 0;
    int64_t stored_album_song_id = 0;
    if (albumart_artist_alias_load(artist, (unsigned int) group_kind,
                                   &stored_album_key, &stored_album_song_id)) {
        song_row_t stored_album_song;
        albumart_info_t stored_album_info;
        char found[PATH_MAX];
        bool valid = metadata_db_get_song_by_id(stored_album_song_id, &stored_album_song) &&
                     song_album_thumbnail_key(&stored_album_song) == stored_album_key &&
                     stored_album_song.tags.album[0] &&
                     metadata_db_get_album_for_group_offset(group_kind, artist,
                         stored_album_song.tags.album, stored_album_song.tags.album_artist) >= 0;
        if (valid) {
            albumart_info_from_song_row(&stored_album_song, &stored_album_info);
            uint32_t source_mtime = albumart_source_mtime(&stored_album_info);
            valid = albumart_sized_thumb_fresh_with_source_mtime(&stored_album_info,
                         ALBUM_THUMBNAIL_PX, ALBUM_THUMBNAIL_PX, source_mtime,
                         found, sizeof(found));
        }
        if (valid) {
            artist_thumbnail_set_alias(stored_album_key, stored_album_song_id, resolution);
            return ARTIST_THUMB_FOUND;
        }
        albumart_artist_alias_remove(artist, (unsigned int) group_kind);
    }

    /* The signature stats every album of this artist, so compute it only
     * when a no-art marker exists to validate or is about to be written;
     * artists whose cover resolves never pay for it. */
    const char * signature_dir = have_artist_dir ? artist_dir : "";
    if (albumart_artist_negative_exists(artist) &&
        albumart_artist_negative_fresh(artist, artist_directory_signature(
            artist, group_kind, &representative, signature_dir))) {
        resolution->negative_confirmed = true;
        return ARTIST_THUMB_MISSING;
    }

    artist_thumbnail_result_t result = artist_thumbnail_from_album_group(
        artist, group_kind, prio, cancel_cb, cancel_user_data, resolution);
    if (result == ARTIST_THUMB_FOUND) return result;
    if (result == ARTIST_THUMB_MISSING) {
        albumart_artist_store_negative(artist, artist_directory_signature(
            artist, group_kind, &representative, signature_dir));
        resolution->negative_confirmed = true;
    }
    return result;
}

static bool album_thumb_gen_warm_artist_aliases(int my_generation) {
    const metadata_db_group_kind_t kinds[] = {
        METADATA_DB_GROUP_ARTIST,
        METADATA_DB_GROUP_ALBUM_ARTIST
    };
    int artist_count = 0, album_artist_count = 0, album_count = 0;
    int counts[2];
    metadata_db_get_group_counts(&artist_count, &album_artist_count, &album_count);
    counts[0] = artist_count;
    counts[1] = album_artist_count;
    (void) album_count;

    group_row_t rows[ALBUM_THUMB_GEN_BATCH];
    for (size_t kind_index = 0; kind_index < sizeof(kinds) / sizeof(kinds[0]); kind_index++) {
        metadata_db_group_kind_t group_kind = kinds[kind_index];
        for (int offset = 0; offset < counts[kind_index];) {
            if (album_thumb_gen_should_cancel(my_generation)) return false;
            if (audio_is_playing() || album_thumbnail_lazy_work_pending() ||
                !artwork_check_memory_admission(ARTWORK_PRIO_WARMER,
                                                ALBUM_ART_METADATA_START_BYTES)) {
                atomic_store(&album_thumb_gen_retry_pending, true);
                return false;
            }

            int n = metadata_db_get_groups_page(group_kind, offset,
                                                ALBUM_THUMB_GEN_BATCH, rows);
            if (n <= 0) break;
            for (int i = 0; i < n; i++) {
                if (album_thumb_gen_should_cancel(my_generation)) return false;
                if (audio_is_playing() || album_thumbnail_lazy_work_pending()) {
                    atomic_store(&album_thumb_gen_retry_pending, true);
                    return false;
                }

                uint64_t artist_key = albumart_artist_thumbnail_key(rows[i].name);
                bool alias_known = false;
                for (size_t k = 0; k < album_thumb_gen_known_alias_count && !alias_known; k++)
                    alias_known = album_thumb_gen_known_aliases[k].key == artist_key &&
                                  album_thumb_gen_known_aliases[k].kind == group_kind;
                if (alias_known || album_thumb_gen_cached_key(THUMBNAIL_KEY_ARTIST, artist_key)) {
                    atomic_fetch_add(&album_thumb_gen_done_count, 1);
                    continue;
                }

                artist_thumbnail_resolution_t resolution;
                artist_thumbnail_result_t result = artist_thumbnail_load_or_decode(
                    rows[i].first_song_id, group_kind, rows[i].name, ARTWORK_PRIO_WARMER, false,
                    artist_key, album_thumb_gen_cancel_cb, (void *) (intptr_t) my_generation,
                    &resolution);
                if (result == ARTIST_THUMB_OWN_IMAGE) {
                    atomic_fetch_add(&album_thumb_gen_done_count, 1);
                    continue;
                }
                if (result == ARTIST_THUMB_CANCELLED) {
                    if (!album_thumb_gen_should_cancel(my_generation))
                        atomic_store(&album_thumb_gen_retry_pending, true);
                    free(resolution.pixels);
                    return false;
                }
                if (result == ARTIST_THUMB_TEMPORARY) {
                    atomic_store(&album_thumb_gen_retry_pending, true);
                    free(resolution.pixels);
                    atomic_fetch_add(&album_thumb_gen_done_count, 1);
                    continue;
                }

                if (resolution.has_alias || resolution.negative_confirmed || resolution.pixels) {
                    album_thumbnail_handoff_t handoff = {
                        .kind = ALBUM_BOOT_RESULT_ARTIST,
                        .cache_epoch = atomic_load(&album_thumb_gen_cache_epoch),
                        .key_kind = resolution.pixel_key_kind,
                        .key = resolution.pixel_key,
                        .song_id = resolution.pixel_song_id > 0
                                       ? resolution.pixel_song_id : rows[i].first_song_id,
                        .pixels = (uint8_t *) resolution.pixels,
                        .artist_key = artist_key,
                        .artist_group_kind = group_kind,
                        .artist_has_alias = resolution.has_alias,
                        .artist_negative_confirmed = resolution.negative_confirmed,
                        .alias_album_key = resolution.alias_album_key,
                        .alias_song_id = resolution.alias_song_id
                    };
                    if (!album_thumb_gen_handoff_result(my_generation, &handoff)) return false;
                } else {
                    free(resolution.pixels);
                }
                atomic_fetch_add(&album_thumb_gen_done_count, 1);
                usleep(ALBUM_THUMB_GEN_INTER_ALBUM_US); /* same cooling yield as the album pass */
            }
            offset += n;
            if (n < ALBUM_THUMB_GEN_BATCH) break;
        }
    }
    return true;
}

static void * album_thumbnail_thread_func(void * arg) {
    install_thread_crash_altstack(); /* see its own comment (main.c) */
#ifdef UI_PERF_TRACE
    uint64_t perf_start_us = ui_perf_now_us();
#endif
    album_thumbnail_request_t * req = (album_thumbnail_request_t *) arg;
    /* Cached once -- see library_scan_once()'s own comment on why an
     * unconditional db_log_now_ms() call here would cost a real syscall on
     * every lazy-loaded thumbnail even with logging disabled. */
    bool db_logging = db_log_enabled();
    uint64_t started_ms = db_logging ? db_log_now_ms() : 0;
    thumbnail_key_kind_t key_kind = req->key_kind;
    if (db_logging && key_kind == THUMBNAIL_KEY_ALBUM)
        DB_LOG("ART_LAZY", "decode_begin song=%lld row=%d cache_epoch=%d rss_kb=%ld",
               (long long) req->song_id, req->logical_index, req->cache_epoch, db_log_rss_kb());
    else if (db_logging)
        DB_LOG("ART_ARTIST", "decode_begin song=%lld row=%d cache_epoch=%d key=%016llx rss_kb=%ld",
               (long long) req->song_id, req->logical_index, req->cache_epoch,
               (unsigned long long) req->key, db_log_rss_kb());
    uint16_t * pixels = NULL;
    album_thumbnail_result_have_source_mtime = false;
    album_thumbnail_result_negative_confirmed = false;
    album_thumbnail_result_has_alias = false;
    album_thumbnail_result_artist_negative_confirmed = false;
    album_thumbnail_result_artist_key = 0;
    album_thumbnail_result_alias_album_key = 0;
    album_thumbnail_result_alias_song_id = 0;
    uint64_t pixel_key = req->key;
    thumbnail_key_kind_t pixel_key_kind = req->key_kind;
    if (req->key_kind == THUMBNAIL_KEY_ALBUM) {
        song_row_t song;
        if (metadata_db_get_song_by_id(req->song_id, &song)) {
            albumart_info_t info;
            albumart_info_from_song_row(&song, &info);
            album_thumbnail_result_source_mtime = album_source_mtime(&song, &info);
            album_thumbnail_result_have_source_mtime = true;
            album_thumbnail_load_or_decode_with_mtime(&song, ARTWORK_PRIO_THUMBNAIL,
                    album_thumbnail_cancel_cb, (void *) (intptr_t) req->cache_epoch,
                    album_thumbnail_result_source_mtime, false, NULL, false, &pixels);
            if (pixels) {
                album_thumbnail_result_have_source_mtime = false;
                artwork_failure_cache_note_success(req->song_id);
            }
        }
    } else {
        artist_thumbnail_resolution_t resolution;
        (void) artist_thumbnail_load_or_decode(req->song_id, req->artist_group_kind, req->artist_name,
                    ARTWORK_PRIO_THUMBNAIL, true, req->key, album_thumbnail_cancel_cb,
                    (void *) (intptr_t) req->cache_epoch, &resolution);
        pixels = resolution.pixels;
        pixel_key = resolution.pixel_key;
        pixel_key_kind = resolution.pixel_key_kind;
        album_thumbnail_result_has_alias = resolution.has_alias;
        album_thumbnail_result_artist_negative_confirmed = resolution.negative_confirmed;
        album_thumbnail_result_artist_key = req->key;
        album_thumbnail_result_artist_group_kind = req->artist_group_kind;
        album_thumbnail_result_alias_album_key = resolution.alias_album_key;
        album_thumbnail_result_alias_song_id = resolution.alias_song_id;
    }
    album_thumbnail_result_song_id = req->song_id;
    album_thumbnail_result_key = pixel_key;
    album_thumbnail_result_key_kind = pixel_key_kind;
    album_thumbnail_result_request_key_kind = req->key_kind;
    album_thumbnail_result_cache_epoch = req->cache_epoch;
    int result_logical_index = req->logical_index;
    album_thumbnail_result_pixels = (uint8_t *) pixels;
    free(req);
    album_thumbnail_done = true;
    if (db_logging && key_kind == THUMBNAIL_KEY_ALBUM)
        DB_LOG("ART_LAZY", "decode_end song=%lld row=%d art=%d elapsed_ms=%llu rss_kb=%ld",
               (long long) album_thumbnail_result_song_id, result_logical_index,
               album_thumbnail_result_pixels != NULL,
               (unsigned long long) (db_log_now_ms() - started_ms), db_log_rss_kb());
    else if (db_logging)
        DB_LOG("ART_ARTIST", "decode_end song=%lld row=%d art=%d negative=%d elapsed_ms=%llu rss_kb=%ld",
               (long long) album_thumbnail_result_song_id, result_logical_index,
               album_thumbnail_result_pixels != NULL, album_thumbnail_result_negative_confirmed,
               (unsigned long long) (db_log_now_ms() - started_ms), db_log_rss_kb());
#ifdef UI_PERF_TRACE
    printf("PERF album_thumb song=%lld total_us=%llu pixels=%d\n",
           (long long) album_thumbnail_result_song_id,
           (unsigned long long) (ui_perf_now_us() - perf_start_us),
           album_thumbnail_result_pixels != NULL);
#endif
    return NULL;
}

/* ---- Persistent (post-scan) album thumbnail generation ----------------
 * Runs once after Update Music Database completes, writing sized BMP files
 * in short bounded batches with generous cooperative delays so CPU and SD
 * activity never cause overheating or starve audio playback. Suspends
 * automatically when audio is playing, visible lazy work is pending, or
 * memory is low. */
#define ALBUM_THUMB_GEN_INTER_BATCH_US 1000000 /* 1.0 s pause between batches */

/* A reload must leave both generated sizes on disk, except for albums
 * albumart_store_rgb565() cannot name (no album or artist tag). */
static bool album_thumbnail_reload_persisted(const song_row_t * song) {
    albumart_info_t info;
    albumart_info_from_song_row(song, &info);
    if (!info.album[0] || !(info.albumartist[0] || info.artist[0])) return true;
    /* Generated files, or a user's cover.72x72 that this decode actually
     * read successfully; a user file's mere presence proves nothing. */
    char found[PATH_MAX];
    return (album_art_thumb_from_user_file ||
            albumart_generated_cache_fresh(&info, ALBUM_THUMBNAIL_PX, ALBUM_THUMBNAIL_PX,
                                           found, sizeof(found))) &&
           albumart_generated_cache_fresh(&info, ALBUM_PLAYER_CACHE_PX, ALBUM_PLAYER_CACHE_PX,
                                          found, sizeof(found));
}

/* Artist cache keys of every artist named on an album's tracks (full tag
 * and each split name, matching how artist rows are grouped). */
static bool album_artist_cache_keys(const char * album, const char * album_artist,
                                    uint64_t ** out_keys, size_t * out_count) {
    *out_keys = NULL;
    *out_count = 0;
    size_t capacity = 0;
    song_row_t songs[32];
    for (int offset = 0;;) {
        int count = metadata_db_get_album_songs(album, album_artist, offset, songs,
                                                (int) (sizeof(songs) / sizeof(songs[0])));
        if (count <= 0) break;
        for (int i = 0; i < count; i++) {
            char names[TAGCACHE_ARTIST_SPLIT_MAX + 1][TAGCACHE_TAG_MAX];
            int n = tagcache_artist_names(songs[i].tags.artist, names, TAGCACHE_ARTIST_SPLIT_MAX);
            if (n < 0) n = 0;
            snprintf(names[n++], sizeof(names[0]), "%s", songs[i].tags.artist);
            for (int j = 0; j < n; j++) {
                if (!names[j][0]) continue;
                uint64_t key = albumart_artist_thumbnail_key(names[j]);
                bool seen = false;
                for (size_t k = 0; k < *out_count && !seen; k++) seen = (*out_keys)[k] == key;
                if (seen) continue;
                if (*out_count == capacity) {
                    size_t next = capacity ? capacity * 2 : 16;
                    uint64_t * grown = realloc(*out_keys, next * sizeof(*grown));
                    if (!grown) return false;
                    *out_keys = grown;
                    capacity = next;
                }
                (*out_keys)[(*out_count)++] = key;
            }
        }
        offset += count;
        if (count < (int) (sizeof(songs) / sizeof(songs[0]))) break;
    }
    return true;
}

static void * album_thumb_gen_thread_func(void * arg) {
    if (!album_thumb_gen_altstack_already_installed)
        install_thread_crash_altstack(); /* see its own comment (main.c) */
    album_thumb_gen_altstack_already_installed = false;
    int my_generation = (int) (intptr_t) arg;
    bool user_visible = album_thumb_gen_user_visible;
    bool force_reload = album_thumb_gen_force_reload;
    bool single_album = album_thumb_gen_single_album;
    /* A user-started reload may run during playback, which the coordinator
     * refuses at warmer priority. */
    artwork_priority_t decode_prio = user_visible ? ARTWORK_PRIO_THUMBNAIL : ARTWORK_PRIO_WARMER;
    album_art_materialize_player = true;
    uint64_t started_ms = db_log_enabled() ? db_log_now_ms() : 0;
    int generated = 0, cached = 0, missing = 0, failed = 0;
    DB_LOG("ART_CACHE", "worker_begin generation=%d total=%d rss_kb=%ld", my_generation,
           atomic_load(&album_thumb_gen_total_count), db_log_rss_kb());
#ifdef UI_PERF_TRACE
    uint64_t perf_start_us = ui_perf_now_us();
    int perf_generated = 0, perf_skipped = 0, perf_missing = 0, perf_failed = 0;
#endif
    atomic_store(&album_thumb_gen_done_count, 0);

    if (album_thumb_gen_should_cancel(my_generation)) goto done;
    if (!user_visible && album_thumbnail_lazy_work_pending()) {
        atomic_store(&album_thumb_gen_retry_pending, true);
        goto done;
    }

    if (force_reload) {
        bool storage_ok = metadata_db_storage_current();
#ifndef HOST_BUILD
        storage_ok = storage_ok && sd_card_root_is_mounted();
#endif
        if (!storage_ok) {
            atomic_fetch_add(&album_thumb_gen_error_count, 1);
            goto done;
        }
        /* gui_player_begin_cover_reload() already invalidated any player
         * decode; wait here, off the UI thread, until one still running can
         * no longer write an old cover back after the deletion below. */
        while (!gui_player_cover_decode_idle()) {
            if (album_thumb_gen_should_cancel(my_generation)) goto done;
            usleep(10000);
        }
        bool deleted = false;
        if (single_album) {
            song_row_t target_song;
            if (!metadata_db_get_song_by_id(album_thumb_gen_target_song_id, &target_song)) {
                atomic_fetch_add(&album_thumb_gen_error_count, 1);
                goto done;
            }
            albumart_info_t info;
            albumart_info_from_song_row(&target_song, &info);
            /* Compilations: every track artist may hold a stale "no art"
             * marker with no alias pointing at this album. */
            uint64_t * artist_keys = NULL;
            size_t artist_key_count = 0;
            bool keys_ok = album_artist_cache_keys(target_song.tags.album, target_song.tags.album_artist,
                                                   &artist_keys, &artist_key_count);
            deleted = keys_ok && albumart_delete_generated_cache_for_album(&info, artist_keys, artist_key_count);
            free(artist_keys);
        } else {
            deleted = albumart_delete_all_generated_caches();
        }
        if (!deleted) {
            atomic_fetch_add(&album_thumb_gen_error_count, 1);
            goto done;
        }
    }

    group_row_t rows[ALBUM_THUMB_GEN_BATCH];
    int offset = 0;
    for (;;) {
        if (album_thumb_gen_should_cancel(my_generation)) break;

        /* Suspend warmer while audio is playing, visible lazy work is pending, or memory is low */
        if (album_thumb_gen_paused_for_playback(user_visible) ||
            (!user_visible && album_thumbnail_lazy_work_pending()) ||
            !artwork_check_memory_admission(decode_prio, ALBUM_ART_METADATA_START_BYTES)) {
            if (user_visible) atomic_fetch_add(&album_thumb_gen_error_count, 1);
            else atomic_store(&album_thumb_gen_retry_pending, true);
            goto done;
        }

        int n;
        if (single_album) {
            if (offset > 0) break;
            memset(rows, 0, sizeof(rows));
            rows[0].first_song_id = album_thumb_gen_target_song_id;
            n = 1;
        } else {
            n = metadata_db_get_albums_page_filtered(NULL, offset, ALBUM_THUMB_GEN_BATCH, rows);
        }
        if (n <= 0) break;
        for (int i = 0; i < n; i++) {
            if (album_thumb_gen_should_cancel(my_generation)) goto done;

            if (force_reload) {
                bool storage_ok = metadata_db_storage_current();
#ifndef HOST_BUILD
                storage_ok = storage_ok && sd_card_root_is_mounted();
#endif
                if (!storage_ok) {
                    atomic_fetch_add(&album_thumb_gen_error_count, 1);
                    goto done;
                }
            }

            /* Check suspension before each album */
            if (album_thumb_gen_paused_for_playback(user_visible) ||
                (!user_visible && album_thumbnail_lazy_work_pending())) {
                if (user_visible) atomic_fetch_add(&album_thumb_gen_error_count, 1);
                else atomic_store(&album_thumb_gen_retry_pending, true);
                goto done;
            }

            song_row_t song;
            if (!metadata_db_get_song_by_id(rows[i].first_song_id, &song)) {
                missing++;
                if (force_reload) atomic_fetch_add(&album_thumb_gen_error_count, 1);
#ifdef UI_PERF_TRACE
                perf_missing++;
#endif
                atomic_fetch_add(&album_thumb_gen_done_count, 1);
                continue;
            }

            uint64_t album_key = song_album_thumbnail_key(&song);
            if (album_thumb_gen_cached_key(THUMBNAIL_KEY_ALBUM, album_key)) {
                cached++;
#ifdef UI_PERF_TRACE
                perf_skipped++;
#endif
                atomic_fetch_add(&album_thumb_gen_done_count, 1);
                continue;
            }

            albumart_info_t info;
            albumart_info_from_song_row(&song, &info);

            /* Playing with the experimental switch on: only read a 72px cover
             * already generated on the card (thumbnail priority, which the
             * coordinator admits during playback). Anything needing real
             * extraction is left for the pass after playback stops. */
            if (!user_visible && audio_is_playing()) {
                if (album_thumb_gen_ram_filled >= ALBUM_THUMBNAIL_CACHE_SIZE) {
                    /* RAM budget full: further reads would be decoded only
                     * to be freed. End this playback pass now. */
                    atomic_store(&album_thumb_gen_playback_pass_epoch,
                                 atomic_load(&album_thumb_gen_cache_epoch));
                    atomic_store(&album_thumb_gen_retry_pending, true);
                    goto done;
                }
                uint16_t * cached_pixels = NULL;
                char found[PATH_MAX];
                if (albumart_generated_cache_fresh(&info, ALBUM_THUMBNAIL_PX, ALBUM_THUMBNAIL_PX,
                                                   found, sizeof(found))) {
                    uint8_t * data = NULL;
                    uint32_t size = 0;
                    if (albumart_load_file_ex(found, &data, &size, THUMBNAIL_SIDECAR_MAX_BYTES,
                                              ARTWORK_PRIO_THUMBNAIL) == ALBUMART_LOAD_OK) {
                        if (cover_decode_to_rgb565_ex(data, size, ALBUM_THUMBNAIL_PX, ALBUM_THUMBNAIL_PX,
                                ARTWORK_PRIO_THUMBNAIL, album_thumb_gen_cancel_cb,
                                (void *) (intptr_t) my_generation, &cached_pixels) != COVER_DECODE_OK) {
                            free(cached_pixels);
                            cached_pixels = NULL;
                        }
                    }
                    free(data);
                }
                if (!cached_pixels) {
                    atomic_store(&album_thumb_gen_retry_pending, true);
                } else {
                    album_thumb_gen_ram_filled++;
                    album_thumbnail_handoff_t handoff = {
                        .kind = ALBUM_BOOT_RESULT_ALBUM,
                        .cache_epoch = atomic_load(&album_thumb_gen_cache_epoch),
                        .key_kind = THUMBNAIL_KEY_ALBUM,
                        .key = album_key,
                        .song_id = song.id,
                        .pixels = (uint8_t *) cached_pixels,
                    };
                    if (!album_thumb_gen_handoff_result(my_generation, &handoff)) goto done;
                    cached++;
                }
                atomic_fetch_add(&album_thumb_gen_done_count, 1);
                usleep(ALBUM_THUMB_GEN_INTER_ALBUM_US);
                continue;
            }

            time_t source_mtime = album_source_mtime(&song, &info);

            artwork_fail_reason_t fail_reason;
            if (artwork_failure_cache_is_blocked(song.id, source_mtime, &fail_reason)) {
                if (fail_reason == ARTWORK_FAIL_TEMPORARY)
                    atomic_store(&album_thumb_gen_retry_pending, true);
                failed++;
#ifdef UI_PERF_TRACE
                perf_skipped++;
#endif
                atomic_fetch_add(&album_thumb_gen_done_count, 1);
                continue;
            }

            char found[PATH_MAX];
            /* Still enter the decode call below for the first ALBUM_THUMBNAIL_
             * CACHE_SIZE albums even when both persistent caches already exist
             * -- on every boot after the very first one this is nearly always
             * true for the whole library, and skipping straight to `continue`
             * here (as every album past the RAM budget still does) would mean
             * the RAM-preload hand-off below never runs at all on a normal
             * boot. album_thumbnail_load_or_decode_ex() already returns the
             * existing 72px sidecar's pixels immediately in this case without
             * touching the original source file, so this costs one cheap BMP
             * read+decode per album instead of a real cache regeneration. */
            if (album_thumb_gen_ram_filled >= ALBUM_THUMBNAIL_CACHE_SIZE &&
                album_thumbnail_sized_cache_hit(&info, found, sizeof(found)) &&
                album_player_cache_hit(&info, found, sizeof(found))) {
                cached++;
#ifdef UI_PERF_TRACE
                perf_skipped++;
#endif
                atomic_fetch_add(&album_thumb_gen_done_count, 1);
                continue;
            }

            uint16_t * pixels = NULL;
            album_thumbnail_load_or_decode_ex(&song, decode_prio,
                                              album_thumb_gen_cancel_cb, (void *) (intptr_t) my_generation,
                                              &pixels);
            if (!pixels && ((!user_visible && audio_is_playing()) || album_thumb_gen_should_cancel(my_generation) ||
                (artwork_failure_cache_is_blocked(song.id, source_mtime, &fail_reason) &&
                 fail_reason == ARTWORK_FAIL_TEMPORARY)))
                atomic_store(&album_thumb_gen_retry_pending, true);
            if (pixels) { artwork_failure_cache_note_success(song.id); generated++; } else { failed++; }
            if (force_reload && pixels && !album_thumbnail_reload_persisted(&song)) {
                /* Decoded but not saved (e.g. card full): the next boot
                 * would lose it, so report the reload as incomplete. */
                atomic_fetch_add(&album_thumb_gen_error_count, 1);
            }
            if (force_reload && !pixels) {
                artwork_fail_reason_t current_reason = ARTWORK_FAIL_NONE;
                bool blocked = artwork_failure_cache_is_blocked(song.id, source_mtime, &current_reason);
                if (!blocked || current_reason == ARTWORK_FAIL_TEMPORARY)
                    atomic_fetch_add(&album_thumb_gen_error_count, 1);
            }
            if (!pixels) {
                artwork_fail_reason_t diag_fail_reason = ARTWORK_FAIL_NONE;
                bool diag_blocked = artwork_failure_cache_is_blocked(song.id, source_mtime, &diag_fail_reason);
                DB_LOG("ART_CACHE", "album_failed song=%lld path=%s reason=%s",
                       (long long) song.id, song.path,
                       !diag_blocked ? "unrecorded" :
                       (diag_fail_reason == ARTWORK_FAIL_PERMANENT ? "PERMANENT" : "TEMPORARY"));
            }

#ifdef UI_PERF_TRACE
            if (pixels) perf_generated++; else perf_failed++;
#endif
            bool keep_in_ram = album_thumb_gen_ram_filled < ALBUM_THUMBNAIL_CACHE_SIZE;
            if (keep_in_ram) album_thumb_gen_ram_filled++;
            if (keep_in_ram || (!user_visible && album_thumbnail_lazy_work_pending())) {
                if (album_thumb_gen_should_cancel(my_generation)) {
                    free(pixels);
                    goto done;
                }
                album_thumbnail_handoff_t handoff = {
                    .kind = ALBUM_BOOT_RESULT_ALBUM,
                    .cache_epoch = atomic_load(&album_thumb_gen_cache_epoch),
                    .key_kind = THUMBNAIL_KEY_ALBUM,
                    .key = album_key,
                    .song_id = song.id,
                    .pixels = (uint8_t *) pixels,
                    .have_source_mtime = pixels == NULL,
                    .source_mtime = source_mtime
                };
                if (!album_thumb_gen_handoff_result(my_generation, &handoff)) goto done;
            } else {
                free(pixels);
            }
                atomic_fetch_add(&album_thumb_gen_done_count, 1);

            if (!user_visible && album_thumbnail_lazy_work_pending()) {
                atomic_store(&album_thumb_gen_retry_pending, true);
                goto done;
            }

            int diag_done = atomic_load(&album_thumb_gen_done_count);
            if ((diag_done % 50) == 0) {
                DB_LOG("ART_CACHE", "progress done=%d total=%d generated=%d cached=%d missing=%d failed=%d elapsed_ms=%llu rss_kb=%ld",
                       diag_done, atomic_load(&album_thumb_gen_total_count), generated, cached, missing, failed,
                       (unsigned long long) (db_log_now_ms() - started_ms), db_log_rss_kb());
            }

            if (!user_visible) usleep(ALBUM_THUMB_GEN_INTER_ALBUM_US);
        }
        offset += n;
        if (single_album || n < ALBUM_THUMB_GEN_BATCH) break;

        /* Pause between batches to give CPU/SD bus complete rest */
        if (!user_visible)
            for (int p = 0; p < 10; p++) {
                if (album_thumb_gen_should_cancel(my_generation)) goto done;
                if (album_thumbnail_lazy_work_pending()) {
                    atomic_store(&album_thumb_gen_retry_pending, true);
                    goto done;
                }
                usleep(ALBUM_THUMB_GEN_INTER_BATCH_US / 10);
            }
    }
    if (!user_visible && !single_album && audio_is_playing()) {
        /* Playback-time pass (experimental switch) done for this cache
         * epoch; the artist pass and any skipped extraction wait for
         * playback to stop. */
        atomic_store(&album_thumb_gen_playback_pass_epoch, atomic_load(&album_thumb_gen_cache_epoch));
        atomic_store(&album_thumb_gen_retry_pending, true);
        goto done;
    }
    if (!user_visible && !single_album && !album_thumb_gen_should_cancel(my_generation) &&
        !album_thumb_gen_warm_artist_aliases(my_generation))
        goto done;
done:
    DB_LOG("ART_CACHE", "worker_end generation=%d done=%d total=%d generated=%d cached=%d missing=%d failed=%d cancelled=%d elapsed_ms=%llu rss_kb=%ld",
           my_generation, atomic_load(&album_thumb_gen_done_count), atomic_load(&album_thumb_gen_total_count),
           generated, cached, missing, failed, (int) album_thumb_gen_should_cancel(my_generation),
           (unsigned long long) (db_log_now_ms() - started_ms), db_log_rss_kb());
#ifdef UI_PERF_TRACE
    printf("PERF album_thumb_gen done=%d generated=%d skipped=%d missing=%d failed=%d us=%llu cancelled=%d\n",
           atomic_load(&album_thumb_gen_done_count), perf_generated, perf_skipped, perf_missing, perf_failed,
           (unsigned long long) (ui_perf_now_us() - perf_start_us),
           (int) album_thumb_gen_should_cancel(my_generation));
#endif
    atomic_store(&album_thumb_gen_active, false);
    return NULL;
}

/* Called once, right after Update Music Database's own scan thread is
 * joined and the database is already fully committed (see this function's
 * own call site) -- never blocks the caller waiting for generation itself
 * to finish, only (briefly) for a STILL-RUNNING previous pass to notice
 * it's been superseded and exit, which happens within roughly one album's
 * worth of decode work given the cancellation check at the top of every
 * iteration. */
/* 4MB stack size configured for thumbnail generation thread to accommodate
 * stack usage of JPEG and PNG cover decoders. */
#define ALBUM_COVER_DECODE_THREAD_STACK_SIZE (4 * 1024 * 1024)

static int prepare_album_thumbnail_generation(bool user_visible, bool force_reload,
                                             bool single_album, int64_t target_song_id) {
    cancel_album_thumbnail_generation();
    reap_album_thumbnail_generation();
    (void) album_thumbnail_commit_boot_preload();
    album_thumb_gen_user_visible = user_visible;
    album_thumb_gen_force_reload = force_reload;
    album_thumb_gen_single_album = single_album;
    album_thumb_gen_target_song_id = target_song_id;
    album_thumb_gen_altstack_already_installed = false;
    atomic_store(&album_thumb_gen_error_count, 0);
    atomic_store(&album_thumb_gen_retry_pending, false);
    album_thumb_gen_ram_filled = 0;
    album_thumb_gen_cached_key_count = 0;
    for (int i = 0; i < ALBUM_THUMBNAIL_CACHE_SIZE; i++) {
        album_thumbnail_cache_entry_t * entry = &album_thumbnail_cache[i];
        if (!entry->known || !entry->pixels) continue;
        if (album_thumb_gen_cached_key_count >= ALBUM_THUMBNAIL_CACHE_SIZE) break;
        album_thumb_gen_cached_keys[album_thumb_gen_cached_key_count++] = entry->key;
        album_thumb_gen_cached_key_kinds[album_thumb_gen_cached_key_count - 1] = entry->key_kind;
        if (entry->key_kind == THUMBNAIL_KEY_ALBUM) album_thumb_gen_ram_filled++;
    }
    album_thumb_gen_retry_tick = lv_tick_get();

    int artist_count = 0, album_artist_count = 0, album_count = 0;
    metadata_db_get_group_counts(&artist_count, &album_artist_count, &album_count);
    (void) artist_thumbnail_alias_cache_resize(artist_count, album_artist_count);
    free(album_thumb_gen_known_aliases);
    album_thumb_gen_known_aliases = NULL;
    album_thumb_gen_known_alias_count = 0;
    if (artist_thumbnail_alias_cache_capacity)
        album_thumb_gen_known_aliases = malloc(artist_thumbnail_alias_cache_capacity *
                                               sizeof(*album_thumb_gen_known_aliases));
    for (size_t i = 0; album_thumb_gen_known_aliases && i < artist_thumbnail_alias_cache_capacity; i++)
        if (artist_thumbnail_alias_cache[i].known)
            album_thumb_gen_known_aliases[album_thumb_gen_known_alias_count++] =
                (album_thumb_gen_alias_key_t) { artist_thumbnail_alias_cache[i].artist_key,
                                                artist_thumbnail_alias_cache[i].artist_group_kind };
    atomic_store(&album_thumb_gen_done_count, 0);
    int total = single_album ? 1 : album_count;
    if (!user_visible && !single_album) total += artist_count + album_artist_count;
    atomic_store(&album_thumb_gen_total_count, total);
    atomic_store(&album_thumb_gen_cancel, false);
    atomic_store(&album_thumb_gen_cache_epoch, atomic_load(&album_thumbnail_cache_epoch));
    int generation = atomic_fetch_add(&album_thumb_gen_generation, 1) + 1;
    atomic_store(&album_thumb_gen_active, true);
    /* Only start_next_album_thumbnail()'s own two trigger points used to
     * need this timer, and both already resume it. The RAM-preload hand-off
     * below now also depends on it ticking to drain album_boot_preload_
     * pending -- without this, a cold boot's first generation pass would
     * stage exactly one result and then block forever with nothing left to
     * consume it. */
    if (album_thumbnail_poll_timer) lv_timer_resume(album_thumbnail_poll_timer);
    DB_LOG("ART_CACHE", "start generation=%d albums=%d rss_kb=%ld",
           generation, single_album ? 1 : album_count, db_log_rss_kb());
    return generation;
}

static bool launch_album_thumbnail_generation(int generation) {
    pthread_attr_t attr;
    pthread_attr_t * attr_ptr = NULL;
    bool attr_initialized = pthread_attr_init(&attr) == 0;
    if (attr_initialized) {
        if (pthread_attr_setstacksize(&attr, ALBUM_COVER_DECODE_THREAD_STACK_SIZE) == 0) attr_ptr = &attr;
    }
    bool created = pthread_create(&album_thumb_gen_thread, attr_ptr, album_thumb_gen_thread_func,
                                   (void *) (intptr_t) generation) == 0;
    if (attr_initialized) pthread_attr_destroy(&attr);
    if (!created) {
        atomic_store(&album_thumb_gen_active, false);
        DB_LOG("ART_CACHE", "thread_create_failed generation=%d rss_kb=%ld", generation, db_log_rss_kb());
    } else {
        album_thumb_gen_thread_joinable = true;
    }
    return created;
}

static void start_album_thumbnail_generation(void) {
    int generation = prepare_album_thumbnail_generation(false, false, false, 0);
    (void) launch_album_thumbnail_generation(generation);
}

static void start_next_album_thumbnail(void) {
    if (library_rescan_active || !album_thumbnail_active_list ||
        !album_thumbnail_list_is_visible(album_thumbnail_active_list) ||
        album_thumbnail_scrolling || atomic_load(&album_thumbnail_active) ||
        atomic_load(&album_thumbnail_queue_count) <= 0) return;
    /* Never run two full cover decoders at once on the 56 MiB target. The
     * post-load warmer can still be finishing its current album when a row
     * queues work. Keep this poll timer alive and give the visible-row job
     * priority as soon as that worker yields instead of doubling peak decode
     * memory. */
    if (atomic_load(&album_thumb_gen_active)) {
        if (album_thumbnail_poll_timer) lv_timer_resume(album_thumbnail_poll_timer);
        return;
    }
    album_thumbnail_request_t * req = malloc(sizeof(*req));
    if (!req) return;
    atomic_store(&album_thumbnail_active, true);
    *req = album_thumbnail_queue[0];
    memmove(&album_thumbnail_queue[0], &album_thumbnail_queue[1],
            sizeof(album_thumbnail_queue[0]) * (size_t) (--album_thumbnail_queue_count));
    album_thumbnail_active_key = req->key;
    album_thumbnail_active_key_kind = req->key_kind;
    album_thumbnail_active_artist_group_kind = req->artist_group_kind;
    album_thumbnail_active_cache_epoch = req->cache_epoch;
    album_thumbnail_active_view_generation = req->view_generation;
    album_thumbnail_active_logical_index = req->logical_index;
    album_thumbnail_active_key_valid = true;
    album_thumbnail_done = false;
    album_lazy_job_started_ms = db_log_enabled() ? db_log_now_ms() : 0;
    /* See start_album_thumbnail_generation()'s own ALBUM_COVER_DECODE_THREAD_
     * STACK_SIZE comment -- same full JPEG/PNG cover decode, same fix. */
    pthread_attr_t attr;
    pthread_attr_t * attr_ptr = NULL;
    if (pthread_attr_init(&attr) == 0) {
        if (pthread_attr_setstacksize(&attr, ALBUM_COVER_DECODE_THREAD_STACK_SIZE) == 0) attr_ptr = &attr;
    }
    bool created = pthread_create(&album_thumbnail_thread, attr_ptr, album_thumbnail_thread_func, req) == 0;
    if (attr_ptr) pthread_attr_destroy(&attr);
    if (!created) {
        atomic_store(&album_thumbnail_active, false);
        album_thumbnail_active_key_valid = false;
        free(req);
        return;
    }
    if (album_thumbnail_poll_timer) lv_timer_resume(album_thumbnail_poll_timer);
}

static void queue_thumbnail(lv_obj_t * list, int logical_index, thumbnail_key_kind_t key_kind,
                            uint64_t key, int64_t representative_song_id,
                            metadata_db_group_kind_t artist_group_kind, const char * artist_name) {
    if (library_rescan_active || representative_song_id <= 0 || key == 0 || album_thumbnail_scrolling ||
        list != album_thumbnail_active_list || !album_thumbnail_list_is_visible(list) ||
        album_thumbnail_cache_find(key_kind, key)) return;
    int view_generation = atomic_load(&album_thumbnail_view_generation);
    int cache_epoch = atomic_load(&album_thumbnail_cache_epoch);
    if (atomic_load(&album_thumbnail_active) && album_thumbnail_active_key_valid &&
        album_thumbnail_active_cache_epoch == cache_epoch &&
        album_thumbnail_active_key_kind == key_kind && album_thumbnail_active_key == key &&
        (key_kind != THUMBNAIL_KEY_ARTIST || album_thumbnail_active_artist_group_kind == artist_group_kind)) {
        /* A visible request for the same key can move to a newer list view
         * while this cache decode remains useful. Retarget its repaint to
         * that current request without changing codec cancellation state. */
        album_thumbnail_active_view_generation = view_generation;
        album_thumbnail_active_logical_index = logical_index;
        return;
    }
    for (int i = 0; i < album_thumbnail_queue_count; i++)
        if (album_thumbnail_queue[i].key_kind == key_kind && album_thumbnail_queue[i].key == key &&
            (key_kind != THUMBNAIL_KEY_ARTIST || album_thumbnail_queue[i].artist_group_kind == artist_group_kind))
            return;
    if (album_thumbnail_queue_count >= ALBUM_THUMBNAIL_QUEUE_SIZE) return;
    album_thumbnail_queue[album_thumbnail_queue_count++] = (album_thumbnail_request_t) {
        .song_id = representative_song_id,
        .key = key,
        .key_kind = key_kind,
        .artist_group_kind = artist_group_kind,
        .view_generation = view_generation,
        .cache_epoch = cache_epoch,
        .logical_index = logical_index,
        .list = list
    };
    if (key_kind == THUMBNAIL_KEY_ARTIST)
        snprintf(album_thumbnail_queue[album_thumbnail_queue_count - 1].artist_name,
                 sizeof(album_thumbnail_queue[album_thumbnail_queue_count - 1].artist_name),
                 "%s", artist_name ? artist_name : "");
    album_lazy_queued++;
    if (key_kind == THUMBNAIL_KEY_ALBUM)
        DB_LOG("ART_LAZY", "queued song=%lld row=%d queue_depth=%d view_generation=%d cache_epoch=%d",
               (long long) representative_song_id, logical_index,
               atomic_load(&album_thumbnail_queue_count), view_generation, cache_epoch);
    else
        DB_LOG("ART_ARTIST", "queued song=%lld row=%d key=%016llx queue_depth=%d view_generation=%d cache_epoch=%d",
               (long long) representative_song_id, logical_index, (unsigned long long) key,
               atomic_load(&album_thumbnail_queue_count), view_generation, cache_epoch);
    start_next_album_thumbnail();
}

static const thumbnail_decorator_context_t * thumbnail_context_for_list(lv_obj_t * list) {
    if (list == artists_list) return &artist_thumbnail_context;
    if (list == album_artist_list) return &album_artist_thumbnail_context;
    if (list == albums_list || list == artist_albums_list || list == all_songs_list ||
        list == recently_added_list || list == files_search_list || list == group_songs_list)
        return &album_thumbnail_context;
    return NULL;
}

static void refresh_thumbnail_list_visible(lv_obj_t * list) {
    if (!list) return;
    if (list == group_songs_list) refresh_group_song_thumbnails();
    else compact_list_refresh_visible(list);
}

static void album_thumbnail_scroll_cb(lv_event_t * e) {
    lv_obj_t * list = lv_event_get_target(e);
    if (list != album_thumbnail_active_list) return;
    if (lv_event_get_code(e) == LV_EVENT_SCROLL_BEGIN) {
        album_thumbnail_scrolling = true;
        album_thumbnail_queue_count = 0;
        album_thumbnail_view_generation++;
    } else if (lv_event_get_code(e) == LV_EVENT_SCROLL_END) {
        album_thumbnail_scrolling = false;
        refresh_thumbnail_list_visible(list); /* queues the newly settled visible window */
    }
}

static void thumbnail_begin_screen(lv_obj_t * list) {
    if (!thumbnail_context_for_list(list)) return;
    album_thumbnail_view_generation++;
    album_thumbnail_active_list = list;
    album_thumbnail_scrolling = false;
    album_thumbnail_queue_count = 0;
    album_lazy_queued = album_lazy_completed = album_lazy_with_art = album_lazy_stale = 0;
    if (list == albums_list || list == artist_albums_list) {
        const char * diag_page = list == albums_list ? "albums" : "artist_albums";
        DB_LOG("ALBUMS_PAGE", "loaded page=%s request_to_loaded_ms=%llu rss_kb=%ld", diag_page,
               albums_page_open_requested_ms ? (unsigned long long) (db_log_now_ms() - albums_page_open_requested_ms) : 0ULL,
               db_log_rss_kb());
    } else {
        const char * page = list == artists_list ? "artists" :
                            list == album_artist_list ? "album_artists" :
                            list == all_songs_list ? "all_songs" :
                            list == recently_added_list ? "recently_added" :
                            list == group_songs_list ? "group_songs" : "files_search";
        DB_LOG("ART_LIST_PAGE", "loaded page=%s rss_kb=%ld", page, db_log_rss_kb());
    }
    albums_page_open_requested_ms = 0;
    if (list == group_songs_list) refresh_group_song_thumbnails();
    else if (list) compact_list_refresh_visible(list);
}

static void thumbnail_end_screen(lv_obj_t * list) {
    if (album_thumbnail_active_list != list) return;
    album_thumbnail_active_list = NULL;
    album_thumbnail_scrolling = false;
    album_thumbnail_queue_count = 0;
    if (list == albums_list || list == artist_albums_list) {
        DB_LOG("ALBUMS_PAGE", "unloaded page=%s queued=%u completed=%u with_art=%u stale=%u rss_kb=%ld",
               list == albums_list ? "albums" : "artist_albums", album_lazy_queued, album_lazy_completed,
               album_lazy_with_art, album_lazy_stale, db_log_rss_kb());
    } else {
        const char * page = list == artists_list ? "artists" :
                            list == album_artist_list ? "album_artists" :
                            list == all_songs_list ? "all_songs" :
                            list == recently_added_list ? "recently_added" :
                            list == group_songs_list ? "group_songs" : "files_search";
        DB_LOG("ART_LIST_PAGE", "unloaded page=%s queued=%u completed=%u with_art=%u stale=%u rss_kb=%ld",
               page, album_lazy_queued, album_lazy_completed, album_lazy_with_art, album_lazy_stale,
               db_log_rss_kb());
    }
    album_thumbnail_view_generation++;
}

static void album_thumbnail_poll_cb(lv_timer_t * timer) {
    /* Same reasoning as compact_list_poll_fetch_cb()'s own comment
     * (screen_builders.c): this timer's row-image/cache-eviction work
     * landing in the same tick as a live gesture's frame-present call is
     * exactly the kind of variable-cost interference that produces uneven
     * frame pacing mid-animation. Deferring costs nothing -- a landed
     * decode just sits ready a tick or two longer, and this timer keeps
     * rescheduling itself regardless. */
    if (gui_navigation_transition_in_progress()) return;
    if (album_thumbnail_scrolling) return;
    (void) album_thumbnail_commit_boot_preload();
    if (!atomic_load(&album_thumbnail_active)) {
        start_next_album_thumbnail();
        if (!atomic_load(&album_thumbnail_active) && !atomic_load(&album_thumb_gen_active)) lv_timer_pause(timer);
        return;
    }
    if (!atomic_load(&album_thumbnail_done)) return;
    pthread_join(album_thumbnail_thread, NULL);
    /* Keep the slot busy until all result fields are consumed. Row refreshes
     * below can synchronously queue work and try to start the next worker. */

    bool cache_epoch_matches = album_thumbnail_result_cache_epoch ==
                               atomic_load(&album_thumbnail_cache_epoch);
    bool result_committed = false;
    bool result_had_art = album_thumbnail_result_pixels != NULL;
    if (cache_epoch_matches) {
        if (album_thumbnail_result_request_key_kind == THUMBNAIL_KEY_ARTIST) {
            if (album_thumbnail_result_has_alias || album_thumbnail_result_artist_negative_confirmed) {
                artist_thumbnail_alias_cache_commit(album_thumbnail_result_artist_key,
                    album_thumbnail_result_artist_group_kind,
                    album_thumbnail_result_has_alias, album_thumbnail_result_alias_album_key,
                    album_thumbnail_result_alias_song_id);
                if (album_thumbnail_result_has_alias)
                    album_thumbnail_cache_remove(THUMBNAIL_KEY_ARTIST,
                                                 album_thumbnail_result_artist_key);
                album_thumbnail_queue_remove_artist(album_thumbnail_result_artist_key,
                                                    album_thumbnail_result_artist_group_kind);
                result_committed = true;
            }
            if (album_thumbnail_result_pixels) {
                if (album_thumbnail_result_key_kind == THUMBNAIL_KEY_ARTIST)
                    artist_thumbnail_alias_cache_remove(album_thumbnail_result_artist_key,
                                                        album_thumbnail_result_artist_group_kind);
                album_thumbnail_cache_commit(album_thumbnail_result_key_kind, album_thumbnail_result_key,
                    album_thumbnail_result_song_id, album_thumbnail_result_pixels, false, 0, false);
                album_thumbnail_result_pixels = NULL;
                result_committed = true;
            }
        } else {
            album_thumbnail_cache_commit(THUMBNAIL_KEY_ALBUM, album_thumbnail_result_key,
                album_thumbnail_result_song_id, album_thumbnail_result_pixels,
                album_thumbnail_result_have_source_mtime, album_thumbnail_result_source_mtime,
                album_thumbnail_result_negative_confirmed);
            album_thumbnail_result_pixels = NULL;
            result_committed = true;
        }
    }
    bool same_view = album_thumbnail_active_view_generation == atomic_load(&album_thumbnail_view_generation);
    bool repaint_result = cache_epoch_matches &&
        ((same_view && album_thumbnail_active_list && album_thumbnail_list_is_visible(album_thumbnail_active_list)) ||
         (album_thumbnail_result_request_key_kind == THUMBNAIL_KEY_ARTIST &&
          thumbnail_active_list_shows(THUMBNAIL_KEY_ARTIST, album_thumbnail_result_artist_key)) ||
         /* An artist result may carry an album's pixels too (key_kind ALBUM). */
         (result_had_art &&
          thumbnail_active_list_shows(album_thumbnail_result_key_kind, album_thumbnail_result_key)) ||
         (album_thumbnail_result_request_key_kind == THUMBNAIL_KEY_ALBUM &&
          thumbnail_active_list_shows(THUMBNAIL_KEY_ALBUM, album_thumbnail_result_key)));
    album_lazy_completed++;
    if (result_had_art) album_lazy_with_art++;
    if (!cache_epoch_matches) album_lazy_stale++;
    if (album_thumbnail_result_request_key_kind == THUMBNAIL_KEY_ALBUM)
        DB_LOG("ART_LAZY", "result song=%lld row=%d committed=%d repaint=%d art=%d queue_depth=%d ui_wait_ms=%llu",
               (long long) album_thumbnail_result_song_id, album_thumbnail_active_logical_index,
               result_committed, repaint_result, result_had_art, atomic_load(&album_thumbnail_queue_count),
               album_lazy_job_started_ms ? (unsigned long long) (db_log_now_ms() - album_lazy_job_started_ms) : 0ULL);
    else
        DB_LOG("ART_ARTIST", "result song=%lld row=%d committed=%d repaint=%d art=%d alias=%d album=%016llx queue_depth=%d",
               (long long) album_thumbnail_result_song_id, album_thumbnail_active_logical_index,
               result_committed, repaint_result, result_had_art, album_thumbnail_result_has_alias,
               (unsigned long long) album_thumbnail_result_alias_album_key,
               atomic_load(&album_thumbnail_queue_count));
    free(album_thumbnail_result_pixels);
    album_thumbnail_result_pixels = NULL;
    if (repaint_result) {
        /* The request's row index is only meaningful in the view it came from. */
        if (same_view && album_thumbnail_result_key_kind == THUMBNAIL_KEY_ALBUM &&
            (album_thumbnail_active_list == albums_list ||
             album_thumbnail_active_list == artist_albums_list))
            compact_list_refresh_item(album_thumbnail_active_list, album_thumbnail_active_logical_index);
        else
            refresh_thumbnail_list_visible(album_thumbnail_active_list);
    }
    atomic_store(&album_thumbnail_active, false);
    album_thumbnail_active_key_valid = false;
    start_next_album_thumbnail();
    if (!atomic_load(&album_thumbnail_active)) lv_timer_pause(timer);
}

/* UI reloads and database replacement both invalidate every list pointer
 * carried by the lazy artwork request. Stop the warmer first, then join the
 * single visible-row decoder before any screen or DB generation is torn
 * down. Both paths are cooperatively bounded by the artwork timeout. */
static void quiesce_album_artwork_workers(void) {
    album_thumbnail_cache_epoch_advance();
    album_thumbnail_view_generation++;
    cancel_album_thumbnail_generation();
    reap_album_thumbnail_generation();
    album_thumbnail_discard_boot_preload();
    atomic_store(&album_thumb_gen_retry_pending, false);

    album_thumbnail_queue_count = 0;
    album_thumbnail_active_list = NULL;
    album_thumbnail_active_key_valid = false;

    if (atomic_load(&album_thumbnail_active)) {
        pthread_join(album_thumbnail_thread, NULL);
        atomic_store(&album_thumbnail_active, false);
    }
    atomic_store(&album_thumbnail_done, false);
    free(album_thumbnail_result_pixels);
    album_thumbnail_result_pixels = NULL;
    if (album_thumbnail_poll_timer) lv_timer_pause(album_thumbnail_poll_timer);
}

static void thumbnail_row_decorator(lv_obj_t * list, lv_obj_t * row, lv_obj_t * image,
                                    int logical_index, int pool_slot, int64_t representative_song_id,
                                    uint64_t artwork_key, const char * artwork_name, void * ctx) {
    (void) pool_slot;
    const thumbnail_decorator_context_t * context = ctx ? ctx : &album_thumbnail_context;
    /* 14px card inset + 72px cover + 14px breathing room before text. */
    lv_obj_set_style_pad_left(row, 100, 0);
    if (representative_song_id <= 0 || artwork_key == 0) {
        lv_image_set_src(image, asset_path("touch_list/list_default_album.png"));
        lv_obj_add_flag(image, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    album_thumbnail_cache_entry_t * cached = album_thumbnail_cache_find(context->key_kind, artwork_key);
    artist_thumbnail_alias_entry_t * alias = NULL;
    if ((!cached || !cached->pixels) && context->key_kind == THUMBNAIL_KEY_ARTIST)
        alias = artist_thumbnail_alias_find(artwork_key, context->artist_group_kind);
    if (alias && alias->has_album) {
        cached = album_thumbnail_cache_find(alias->album_key_kind, alias->album_key);
        if (cached && cached->pixels) {
            lv_image_set_src(image, &cached->dsc);
            lv_image_set_scale(image, LV_SCALE_NONE);
            lv_obj_remove_flag(image, LV_OBJ_FLAG_HIDDEN);
            return;
        }
    }
    if (cached && cached->pixels) {
        lv_image_set_src(image, &cached->dsc);
        lv_image_set_scale(image, LV_SCALE_NONE);
        lv_obj_remove_flag(image, LV_OBJ_FLAG_HIDDEN);
    } else {
        const void * src = asset_path("touch_list/list_default_album.png");
        lv_image_set_src(image, src);
        /* Scale from the asset's own width. A hardcoded /72 assumed the PNG
         * was already 72px; LV_IMAGE_ALIGN_STRETCH before set_src divided by
         * img->w=0 and killed the process during build_albums_screen(). */
        lv_image_header_t header;
        int32_t src_w = 72;
        if (lv_image_decoder_get_info(src, &header) == LV_RESULT_OK && header.w > 0)
            src_w = header.w;
        lv_image_set_scale(image, (ALBUM_THUMBNAIL_PX * LV_SCALE_NONE) / src_w);
        lv_obj_remove_flag(image, LV_OBJ_FLAG_HIDDEN);
        int32_t scroll_y = lv_obj_get_scroll_y(list);
        int32_t row_y = lv_obj_get_y(row);
        bool in_viewport = row_y + lv_obj_get_height(row) >= scroll_y &&
                           row_y <= scroll_y + lv_obj_get_height(list);
        if (!in_viewport) return;
        if (alias) {
            if (alias->has_album && !cached)
                queue_thumbnail(list, logical_index, alias->album_key_kind, alias->album_key,
                                alias->representative_song_id, METADATA_DB_GROUP_ARTIST, NULL);
        } else if (!cached) {
            queue_thumbnail(list, logical_index, context->key_kind, artwork_key,
                            representative_song_id, context->artist_group_kind, artwork_name);
        }
    }
}

static void group_song_row_text_offset(lv_obj_t * row, bool with_thumbnail) {
    if (!row || lv_obj_get_child_count(row) < 2) return;
    int32_t target_x = with_thumbnail ? 100 : GUI_TEXT_INSET;
    int32_t delta = target_x - lv_obj_get_x(lv_obj_get_child(row, 0));
    if (delta == 0) return;
    for (int child_index = 0; child_index < 2; child_index++) {
        lv_obj_t * label = lv_obj_get_child(row, child_index);
        int32_t width = lv_obj_get_width(label) - delta;
        if (width < 40) width = 40;
        lv_obj_set_x(label, target_x);
        lv_obj_set_width(label, width);
    }
}

static void refresh_group_song_thumbnails(void) {
    if (!group_songs_list) return;
    lv_obj_update_layout(group_songs_list);
    int page_end = group_songs_page_start + GROUP_SONGS_PAGE_SIZE;
    if (page_end > group_songs_count) page_end = group_songs_count;
    int32_t scroll_y = lv_obj_get_scroll_y(group_songs_list);
    int32_t view_bottom = scroll_y + lv_obj_get_height(group_songs_list);
    for (int index = group_songs_page_start; index < page_end; index++) {
        int slot = index - group_songs_page_start;
        lv_obj_t * row = group_songs_visible_rows[slot];
        if (!row) continue;
        int32_t row_y = lv_obj_get_y(row);
        bool in_viewport = row_y + lv_obj_get_height(row) >= scroll_y && row_y <= view_bottom;
        if (!in_viewport || group_songs_entries[index].song_id <= 0 ||
            group_songs_entries[index].artwork_key == 0) {
            if (group_songs_thumbnail_images[slot]) {
                lv_obj_delete(group_songs_thumbnail_images[slot]);
                group_songs_thumbnail_images[slot] = NULL;
                group_song_row_text_offset(row, false);
            }
            continue;
        }
        if (!group_songs_thumbnail_images[slot]) {
            group_songs_thumbnail_images[slot] = lv_image_create(row);
            lv_obj_remove_flag(group_songs_thumbnail_images[slot], LV_OBJ_FLAG_CLICKABLE);
            group_song_row_text_offset(row, true);
        }
        thumbnail_row_decorator(group_songs_list, row, group_songs_thumbnail_images[slot],
                                index, slot, group_songs_entries[index].song_id,
                                group_songs_entries[index].artwork_key,
                                NULL,
                                (void *) &album_thumbnail_context);
        /* The shared decorator pads compact-list rows by 100px for the cover;
         * these rows position their labels absolutely instead (see
         * group_song_row_text_offset()), so padding would shift the text twice. */
        lv_obj_set_style_pad_left(row, 0, 0);
        int32_t pad_left = lv_obj_get_style_pad_left(row, LV_PART_MAIN);
        int32_t pad_top = lv_obj_get_style_pad_top(row, LV_PART_MAIN);
        int32_t pad_bottom = lv_obj_get_style_pad_bottom(row, LV_PART_MAIN);
        lv_obj_align(group_songs_thumbnail_images[slot], LV_ALIGN_LEFT_MID,
                     14 - pad_left, (pad_top - pad_bottom) / -2);
    }
}

/* Before freeing one cached entry's pixels: detach only the image widgets
 * that borrow its descriptor (pointer compare across the recycled pools).
 * Returns true when the list on screen lost one; the caller re-decorates it
 * once the slot is cleared so it re-queues what it needs. Hidden lists
 * re-decorate on their own SCREEN_LOADED refresh. */
static bool release_thumbnail_dsc(const lv_image_dsc_t * dsc) {
    lv_obj_t * lists[] = { albums_list, artist_albums_list, artists_list, album_artist_list,
                           all_songs_list, recently_added_list, files_search_list };
    bool active_detached = false;
    for (size_t i = 0; i < sizeof(lists) / sizeof(lists[0]); i++)
        if (lists[i] && compact_list_detach_image_src(lists[i], dsc) &&
            lists[i] == album_thumbnail_active_list)
            active_detached = true;
    for (int slot = 0; slot < GROUP_SONGS_PAGE_SIZE; slot++) {
        lv_obj_t * image = group_songs_thumbnail_images[slot];
        if (image && lv_image_get_src(image) == dsc) {
            lv_image_set_src(image, NULL);
            lv_obj_add_flag(image, LV_OBJ_FLAG_HIDDEN);
            if (album_thumbnail_active_list == group_songs_list) active_detached = true;
        }
    }
    return active_detached;
}

/* The five rebuildable library lists die with their screens. Clear every
 * pointer to them right after the delete: the rebuild below constructs them
 * one at a time, and build_albums_screen()'s thumbnail cache clear walks all
 * cover lists, which reached the not-yet-rebuilt (freed) Album Artists and
 * Recently Added lists and crashed on device after a library update. */
static void forget_deleted_library_lists(void) {
    lv_obj_t * dead[] = { all_songs_list, artists_list, albums_list, album_artist_list, recently_added_list };
    bool active_list_deleted = false;
    for (size_t i = 0; i < sizeof(dead) / sizeof(dead[0]); i++) {
        if (dead[i] && album_thumbnail_active_list == dead[i]) {
            album_thumbnail_active_list = NULL;
            active_list_deleted = true;
        }
    }
    for (int i = 0; i < atomic_load(&album_thumbnail_queue_count);) {
        bool list_deleted = false;
        for (size_t j = 0; j < sizeof(dead) / sizeof(dead[0]); j++)
            if (dead[j] && album_thumbnail_queue[i].list == dead[j]) list_deleted = true;
        if (!list_deleted) {
            i++;
            continue;
        }
        int count = atomic_load(&album_thumbnail_queue_count);
        memmove(&album_thumbnail_queue[i], &album_thumbnail_queue[i + 1],
                sizeof(album_thumbnail_queue[0]) * (size_t) (count - i - 1));
        atomic_fetch_sub(&album_thumbnail_queue_count, 1);
    }
    if (active_list_deleted) album_thumbnail_view_generation++;
    all_songs_list = artists_list = albums_list = album_artist_list = recently_added_list = NULL;
}

static void refresh_all_thumbnail_lists(void) {
    lv_obj_t * lists[] = { albums_list, artist_albums_list, artists_list, album_artist_list,
                           all_songs_list, recently_added_list, files_search_list };
    for (size_t i = 0; i < sizeof(lists) / sizeof(lists[0]); i++)
        if (lists[i]) compact_list_refresh_visible(lists[i]);
    refresh_group_song_thumbnails();
}

/* Unfiltered (NULL filter, "every album in the library") -- uses metadata_
 * db_get_albums_page_filtered() rather than metadata_db_get_groups_page(
 * METADATA_DB_GROUP_ALBUM, ...), since the latter groups by album name
 * alone (the exact bug this session fixed) and this screen must show the
 * corrected (album, album_artist) grouping. */
static int albums_fetch_page(void * ctx, int offset, int count, compact_list_page_row_t out_rows[]) {
    (void) ctx;
    group_row_t * rows = malloc(sizeof(group_row_t) * (size_t) count);
    int n = rows ? metadata_db_get_albums_page_filtered(NULL, offset, count, rows) : 0;
    for (int i = 0; i < n; i++) {
        snprintf(out_rows[i].label, sizeof(out_rows[i].label), "%s", rows[i].name);
        snprintf(out_rows[i].subtitle, sizeof(out_rows[i].subtitle), "%s", rows[i].album_artist);
        out_rows[i].identity = rows[i].first_song_id;
        if (rows[i].album_artist[0]) {
            albumart_info_t info = {0};
            snprintf(info.albumartist, sizeof(info.albumartist), "%s", rows[i].album_artist);
            snprintf(info.album, sizeof(info.album), "%s", rows[i].name);
            out_rows[i].artwork_key = albumart_thumbnail_key(&info);
        } else {
            song_row_t representative;
            if (metadata_db_get_song_by_id(rows[i].first_song_id, &representative))
                out_rows[i].artwork_key = song_album_thumbnail_key(&representative);
        }
        snprintf(out_rows[i].trailing_asset, sizeof(out_rows[i].trailing_asset),
                 "%s", "playing_plane/ic_more.png");
    }
    free(rows);
    return n;
}

static void artist_row_click_cb(int index) {
    if (library_rescan_active) return;
    index = search_remap_index(SEARCH_BINDING_ARTISTS, index);
    group_row_t group;
    if (metadata_db_get_groups_page(METADATA_DB_GROUP_ARTIST, index, 1, &group) != 1) return;
    show_artist_albums(group.name, METADATA_DB_GROUP_ARTIST);
}

/* artist_filter scopes an album to one credited artist, for the Artists
 * drill-down where a track tagged "A;B" puts the album under both. The album
 * group's own song_count covers the whole album, so a filtered listing has to
 * count its own rows or it would promise tracks it will not show. Album
 * Artists and the plain Albums list pass NULL and keep the whole album. */
static group_song_entry_t * load_album_entries_filtered(const char * name, const char * album_artist,
                                                         int song_count, const char * artist_filter,
                                                         int * out_count) {
    *out_count = 0;
    if (artist_filter && artist_filter[0]) {
        int64_t filtered = metadata_db_count_songs_filtered(NULL, artist_filter, album_artist, name, NULL);
        song_count = filtered > 0 && filtered <= INT_MAX ? (int) filtered : 0;
    }
    if (song_count <= 0) return NULL;
    song_row_t * songs = calloc((size_t) song_count, sizeof(*songs));
    group_song_entry_t * entries = calloc((size_t) song_count, sizeof(*entries));
    if (!songs || !entries) { free(songs); free(entries); return NULL; }
    int n = 0;
    while (n < song_count) {
        int want = song_count - n;
        if (want > 64) want = 64;
        int got = (artist_filter && artist_filter[0])
                      ? metadata_db_get_songs_filtered_page(NULL, artist_filter, album_artist, name, NULL,
                                                            n, want, songs + n)
                      : metadata_db_get_album_songs(name, album_artist, n, songs + n, want);
        if (got <= 0) break;
        n += got;
        if (got < want) break;
    }
    if (n <= 0) {
        free(songs);
        free_group_song_entries(entries, song_count);
        return NULL;
    }
    qsort(songs, (size_t)n, sizeof(*songs), cmp_song_disc_track);
    bool multi_disc = album_songs_are_multi_disc(songs, n);
    for (int i = 0; i < n; i++) {
        char title[384];
        format_music_submenu_identity(&songs[i], title, sizeof(title));
        entries[i].path = strdup(songs[i].path);
        entries[i].title = strdup(title);
        entries[i].song_id = songs[i].id;
        entries[i].artwork_key = song_album_thumbnail_key(&songs[i]);
        if (!entries[i].path || !entries[i].title) {
            free(songs);
            free_group_song_entries(entries, song_count);
            return NULL;
        }
    }
    set_album_disc_headers(songs, n, multi_disc, entries);
    free(songs);
    *out_count = n;
    return entries;
}

/* The artist whose album list is on screen, when that list is a plain artist
 * rather than an album artist. NULL elsewhere, so only the Artists path
 * scopes an album to one contributor. */
static const char * artist_albums_song_filter(void);

static bool show_album_group_filtered(const group_row_t * group, const char * artist_filter) {
    int count = 0;
    group_song_entry_t * entries = load_album_entries_filtered(group->name, group->album_artist,
                                                                group->song_count, artist_filter, &count);
    if (!entries) return false;
    show_music_group_songs(group->name, group->album_artist, artist_filter, entries, count);
    free_group_song_entries(entries, count);
    return true;
}

static bool show_album_group(const group_row_t * group) {
    return show_album_group_filtered(group, NULL);
}

static void album_row_click_cb(int index) {
    if (library_rescan_active) return;
    index = search_remap_index(SEARCH_BINDING_ALBUMS, index);

    /* Resolve this specific (album, album_artist) pair at its current
     * display position via a single-row offset lookup (same pattern as All
     * Songs' own row-click resolution) -- disambiguated by album_artist too,
     * so two different artists sharing an album title never collide. */
    group_row_t group;
    if (metadata_db_get_albums_page_filtered(NULL, index, 1, &group) != 1) return;
    (void) show_album_group(&group);
}

/* Builds the Artists screen using a paged provider
 * (compact_list_set_paged_provider()) to load artist groups incrementally. */
static lv_obj_t * build_artists_screen(void) {
    lv_obj_t * scr = build_compact_list_screen("Artists", generic_back_cb, NULL, 0, artist_row_click_cb,
                                                artist_collection_long_press_cb, &artists_list, NULL,
                                                LIST_ROW_WIDTH_WIDE, true, accent_lv_color());
    compact_list_set_row_height(artists_list, MUSIC_LIST_ROW_HEIGHT);
    int artist_count = 0, album_artist_count = 0, album_count = 0;
    metadata_db_get_group_counts(&artist_count, &album_artist_count, &album_count);
    (void) artist_thumbnail_alias_cache_resize(artist_count, album_artist_count);
    compact_list_set_paged_provider(artists_list, artists_fetch_page, NULL, artist_count);
    compact_list_set_row_decorator(artists_list, thumbnail_row_decorator,
                                    (void *) &artist_thumbnail_context);
    compact_list_set_trailing_click(artists_list, artist_collection_more_click_cb);
    lv_obj_add_event_cb(scr, album_thumbnail_screen_loaded_cb, LV_EVENT_SCREEN_LOADED, artists_list);
    lv_obj_add_event_cb(scr, album_thumbnail_screen_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, artists_list);
    lv_obj_add_event_cb(artists_list, album_thumbnail_scroll_cb, LV_EVENT_SCROLL_BEGIN, NULL);
    lv_obj_add_event_cb(artists_list, album_thumbnail_scroll_cb, LV_EVENT_SCROLL_END, NULL);
    finalize_screen_navigation(scr);
    return scr;
}

static lv_obj_t * build_albums_screen(void) {
    lv_obj_t * scr = build_compact_list_screen("Albums", generic_back_cb, NULL, 0, album_row_click_cb,
                                                album_row_long_press_cb, &albums_list, NULL,
                                                LIST_ROW_WIDTH_WIDE, true, accent_lv_color());
    compact_list_set_row_height(albums_list, MUSIC_LIST_ROW_HEIGHT);
    int artist_count = 0, album_artist_count = 0, album_count = 0;
    metadata_db_get_group_counts(&artist_count, &album_artist_count, &album_count);
    /* Cache invalidation belongs to the operation that replaces the library
     * (refresh_library_screens_after_reload(), SD reinsertion, UI reload),
     * done once there. Clearing here repeated it and, mid-rebuild, redrew
     * lists that were not rebuilt yet. */
    album_thumbnail_view_generation++;
    if (!album_thumbnail_poll_timer) {
        album_thumbnail_poll_timer = lv_timer_create(album_thumbnail_poll_cb, 50, NULL);
        lv_timer_pause(album_thumbnail_poll_timer);
    }
    /* Decorator before the paged provider so the first (now synchronous)
     * window fill already has pad_left=100 and the 72px cover slot; names
     * then lay out to the right of the art instead of under it. */
    compact_list_set_row_decorator(albums_list, thumbnail_row_decorator,
                                    (void *) &album_thumbnail_context);
    compact_list_set_trailing_click(albums_list, album_more_click_cb);
    compact_list_set_paged_provider(albums_list, albums_fetch_page, NULL, album_count);
    lv_obj_add_event_cb(scr, album_thumbnail_screen_loaded_cb, LV_EVENT_SCREEN_LOADED, albums_list);
    lv_obj_add_event_cb(scr, album_thumbnail_screen_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, albums_list);
    lv_obj_add_event_cb(albums_list, album_thumbnail_scroll_cb, LV_EVENT_SCROLL_BEGIN, NULL);
    lv_obj_add_event_cb(albums_list, album_thumbnail_scroll_cb, LV_EVENT_SCROLL_END, NULL);
    finalize_screen_navigation(scr);
    return scr;
}

static void album_artist_row_click_cb(int index) {
    if (library_rescan_active) return;
    index = search_remap_index(SEARCH_BINDING_ALBUM_ARTIST, index);
    group_row_t group;
    if (metadata_db_get_groups_page(METADATA_DB_GROUP_ALBUM_ARTIST, index, 1, &group) != 1) return;
    show_artist_albums(group.name, METADATA_DB_GROUP_ALBUM_ARTIST);
}

static lv_obj_t * build_album_artist_screen(void) {
    lv_obj_t * scr = build_compact_list_screen("Album Artist", generic_back_cb, NULL, 0, album_artist_row_click_cb,
                                                album_artist_collection_long_press_cb, &album_artist_list, NULL,
                                                LIST_ROW_WIDTH_WIDE, true, accent_lv_color());
    compact_list_set_row_height(album_artist_list, MUSIC_LIST_ROW_HEIGHT);
    int artist_count = 0, album_artist_count = 0, album_count = 0;
    metadata_db_get_group_counts(&artist_count, &album_artist_count, &album_count);
    (void) artist_thumbnail_alias_cache_resize(artist_count, album_artist_count);
    compact_list_set_paged_provider(album_artist_list, album_artists_fetch_page, NULL, album_artist_count);
    compact_list_set_row_decorator(album_artist_list, thumbnail_row_decorator,
                                    (void *) &album_artist_thumbnail_context);
    compact_list_set_trailing_click(album_artist_list, album_artist_collection_more_click_cb);
    lv_obj_add_event_cb(scr, album_thumbnail_screen_loaded_cb, LV_EVENT_SCREEN_LOADED, album_artist_list);
    lv_obj_add_event_cb(scr, album_thumbnail_screen_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, album_artist_list);
    lv_obj_add_event_cb(album_artist_list, album_thumbnail_scroll_cb, LV_EVENT_SCROLL_BEGIN, NULL);
    lv_obj_add_event_cb(album_artist_list, album_thumbnail_scroll_cb, LV_EVENT_SCROLL_END, NULL);
    finalize_screen_navigation(scr);
    return scr;
}

/* ---- A-Z browse index (Artists/Album Artist/All Songs) --
 *
 * A draggable vertical letter strip (touch_list/a_z.png, stock HiBy asset)
 * overlaid on the right edge of each of these three screens, jumping the
 * underlying build_compact_list_screen() list straight to the first
 * entry starting with the touched letter -- the standard iOS Music/
 * Contacts pattern. touch_list/a_z_result_bg.png is the big popup letter
 * bubble shown while dragging, same convention.
 *
 * Drag tracking is polled from its own dedicated timer (poll_az_index_drag,
 * registered in gui_init() next to poll_quick_drawer_drag) rather than any
 * LVGL touch event, for the exact same reason poll_quick_drawer_drag()
 * does -- see its doc comment: LV_EVENT_PRESSING never fires at all on
 * this LVGL version, only PRESSED/RELEASED/CLICKED/LONG_PRESSED do,
 * regardless of which object was hit. A separate timer rather than folding
 * this into poll_quick_drawer_drag keeps that already-large function
 * scoped to its own three existing gestures. */

typedef const char * (*az_index_name_of_t)(int display_index);

typedef struct {
    lv_obj_t * screen;
    lv_obj_t * list;
    lv_obj_t * strip;       /* the a_z.png touch strip */
    lv_obj_t * popup;       /* the a_z_result_bg.png bubble, hidden except while dragging */
    lv_obj_t * popup_label; /* big current-letter text inside popup */
    metadata_db_az_kind_t db_kind; /* DB-backed jump table for this screen -- see poll_az_index_drag() */
} az_index_binding_t;

#define AZ_INDEX_BINDING_COUNT 4
static az_index_binding_t az_index_bindings[AZ_INDEX_BINDING_COUNT];
static int az_index_registered_count = 0;
static lv_timer_t * az_index_visibility_timer;
static az_index_binding_t * az_index_visibility_binding;
static bool az_index_dragging;
/* Whether this press's origin has already been judged against the strip --
 * see poll_az_index_drag()'s own comment on deciding ownership once. */
static bool az_index_press_judged;

#define AZ_INDEX_HIDE_DELAY_MS 900

static void az_index_visibility_timeout_cb(lv_timer_t * timer) {
    (void) timer;
    if (az_index_dragging) {
        lv_timer_reset(az_index_visibility_timer);
        return;
    }
    if (az_index_visibility_binding && az_index_visibility_binding->strip)
        lv_obj_add_flag(az_index_visibility_binding->strip, LV_OBJ_FLAG_HIDDEN);
    az_index_visibility_binding = NULL;
    lv_timer_pause(az_index_visibility_timer);
}

static void az_index_scroll_visibility_cb(lv_event_t * e) {
    lv_obj_t * list = lv_event_get_target(e);
    az_index_binding_t * binding = NULL;
    for (int i = 0; i < az_index_registered_count; ++i) {
        if (az_index_bindings[i].list == list) { binding = &az_index_bindings[i]; break; }
    }
    if (!binding) return;
    if (az_index_visibility_binding && az_index_visibility_binding != binding)
        lv_obj_add_flag(az_index_visibility_binding->strip, LV_OBJ_FLAG_HIDDEN);
    az_index_visibility_binding = binding;
    lv_obj_remove_flag(binding->strip, LV_OBJ_FLAG_HIDDEN);
    if (!az_index_visibility_timer) {
        az_index_visibility_timer = lv_timer_create(az_index_visibility_timeout_cb,
                                                     AZ_INDEX_HIDE_DELAY_MS, NULL);
    }
    lv_timer_set_period(az_index_visibility_timer, AZ_INDEX_HIDE_DELAY_MS);
    lv_timer_reset(az_index_visibility_timer);
    lv_timer_resume(az_index_visibility_timer);
}

static void reset_az_index_bindings(void) {
    if (az_index_visibility_timer) {
        lv_timer_delete(az_index_visibility_timer);
        az_index_visibility_timer = NULL;
    }
    az_index_visibility_binding = NULL;
    az_index_registered_count = 0;
}


/* Called once per screen right after that screen (and its list) is built,
 * at both gui_init()'s boot-time build and the post-library-rescan rebuild
 * -- az_index_registered_count is reset to 0 before the latter, since the
 * old screen/list/strip/popup pointers this table holds become dangling
 * the moment lv_obj_delete() runs on the old screens there. */
static void register_az_index(lv_obj_t * screen, lv_obj_t * list, metadata_db_az_kind_t db_kind) {
    /* Not touch_list/a_z.png -- confirmed via pixel sampling that stock
     * asset is a fully opaque solid-black rectangle with the letters
     * painted in (alpha=255 everywhere, RGB (0,0,0) in the "empty" areas),
     * not a real transparent PNG -- invisible only against HiBy's own
     * pure-black screens, never designed to overlap anything lighter. It
     * visibly blacks out the right edge of these lists' (28,28,30) row
     * backgrounds, which a widget-level bg_opa/border style can't fix
     * since that opaque black is baked into the image's own pixels, not
     * the object's separate background layer. A plain label (genuinely
     * transparent -- only its glyph pixels paint anything) sidesteps this
     * entirely and needs no chroma-key support this LVGL build doesn't have. */
    lv_obj_t * strip = lv_label_create(screen);
    lv_label_set_text(strip, "A\nB\nC\nD\nE\nF\nG\nH\nI\nJ\nK\nL\nM\nN\nO\nP\nQ\nR\nS\nT\nU\nV\nW\nX\nY\nZ\n#");
    int32_t top = STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT;
    int32_t display_h = lv_display_get_vertical_resolution(lv_display_get_default());
    int32_t available_h = display_h - top - HOME_INDICATOR_BAND_HEIGHT;
    const lv_font_t * strip_font = &lv_font_montserrat_20;
    int32_t line_h = lv_font_get_line_height(strip_font);
    lv_obj_set_style_text_font(strip, strip_font, 0);
    lv_obj_set_width(strip, BOARD_SCALE_PX(30));
    lv_obj_set_style_pad_right(strip, BOARD_SCALE_PX(4), 0);
    lv_obj_add_style(strip, &style_theme_text_primary, 0);
    lv_obj_set_style_text_align(strip, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_bg_opa(strip, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(strip, 0, 0);
    lv_obj_remove_flag(strip, LV_OBJ_FLAG_SCROLLABLE);
    /* Stretched with extra line spacing to span close to the full list
     * height (27 lines * app_font_16's own line height) rather than
     * sitting bunched up near the top. */
    int32_t line_space = (available_h - line_h * 27) / 26;
    /* Negative line spacing is intentional on shorter panels: retaining
     * readable 20px glyphs is preferable to falling back to the old tiny
     * font, and the evenly compressed 27-line column still maps touches by
     * its final measured bounds. */
    if (line_space < -6) line_space = -6;
    if (line_space > 3) line_space = 3;
    lv_obj_set_style_text_line_space(strip, line_space, 0);
    /* Top edge (the "A") lines up with the list's own top edge; the
     * stretched height above lands the bottom ("#") close to the screen's
     * bottom corner, matching the list's own bottom edge -- the list
     * itself starts at exactly STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT and
     * runs flush to the screen bottom (see build_compact_list_screen()). */
    lv_obj_align(strip, LV_ALIGN_TOP_RIGHT, 0, top);
    lv_obj_add_flag(strip, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t * popup = lv_image_create(screen);
    lv_image_set_src(popup, asset_path("touch_list/a_z_result_bg.png"));
    lv_obj_center(popup);
    lv_obj_add_flag(popup, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t * popup_label = lv_label_create(popup);
    lv_obj_set_style_text_font(popup_label, &app_font_28, 0);
    lv_obj_add_style(popup_label, &style_theme_text_primary, 0);
    lv_obj_center(popup_label);

    az_index_bindings[az_index_registered_count++] = (az_index_binding_t){ screen, list, strip, popup, popup_label, db_kind };
    lv_obj_add_event_cb(list, az_index_scroll_visibility_cb, LV_EVENT_SCROLL_BEGIN, NULL);
    lv_obj_add_event_cb(list, az_index_scroll_visibility_cb, LV_EVENT_SCROLL, NULL);
    lv_obj_add_event_cb(list, az_index_scroll_visibility_cb, LV_EVENT_SCROLL_END, NULL);
}

static az_index_binding_t * find_az_binding_for_screen(lv_obj_t * screen) {
    for (int i = 0; i < az_index_registered_count; i++) {
        if (az_index_bindings[i].screen == screen) return &az_index_bindings[i];
    }
    return NULL;
}

/* Same pause/resume treatment as quick_drawer_drag_timer above, and for the
 * same reason -- see its own comment. */
lv_timer_t * az_index_drag_timer = NULL;
static az_index_binding_t * az_index_active_binding = NULL;
static int az_index_jump_table[27];

void poll_az_index_drag(lv_timer_t * timer) {
    lv_indev_t * indev = find_pointer_indev();
    if (!indev) return;

    bool pressed = lv_indev_get_state(indev) == LV_INDEV_STATE_PRESSED;
    lv_point_t p;
    lv_indev_get_point(indev, &p);

    if (!az_index_dragging) {
        /* Only paused here (truly idle -- resumed on the next press-down by
         * resume_fast_gesture_timers_cb(), see this timer's own handle
         * comment) and in the just-released branch below. */
        if (!pressed) {
            az_index_press_judged = false;
            lv_timer_pause(timer);
            return;
        }
        /* Ownership is decided ONCE, at press-down: the strip only takes a
         * drag that actually started on it.
         *
         * This used to re-test the live touch point on every tick, on
         * purpose, so that a press begun before the strip appeared could
         * still slide into it. The cost of that was GitHub #84: an ordinary
         * upward list swipe that drifts rightward lands in the strip near
         * the end of its travel, silently changes owner, and jumps the list
         * to a letter. Deciding at press-down is the same latching
         * drag_adjust_press_owned (gui_shell.c) already uses for sliders,
         * and it is what the report itself asked for. The press-begun-
         * before-the-strip-appeared case is given up deliberately: the
         * strip is not on screen to aim at then anyway, so nothing is lost
         * that the user could have been aiming for. */
        if (az_index_press_judged) return;
        az_index_press_judged = true;

        az_index_binding_t * b = find_az_binding_for_screen(lv_screen_active());
        if (!b) return;
        if (lv_obj_has_flag(b->strip, LV_OBJ_FLAG_HIDDEN)) return;

        lv_area_t area;
        lv_obj_get_coords(b->strip, &area);
        if (p.x < area.x1 || p.x > area.x2 || p.y < area.y1 || p.y > area.y2) return;

        az_index_dragging = true;
        az_index_active_binding = b;
        /* One entry per letter A-Z plus '#' (index 26) for anything not
         * starting with a letter -- the display offset of the first
         * matching entry, or the nearest one after it if that exact letter
         * has no entries ("jump forward to nearest match"). Computed fresh
         * on every touch-down from the already-sorted in-memory index
         * (metadata_db_get_az_table()). */
        metadata_db_get_az_table(b->db_kind, az_index_jump_table);
        lv_obj_remove_flag(b->popup, LV_OBJ_FLAG_HIDDEN);
    } else if (!pressed) {
        lv_obj_add_flag(az_index_active_binding->popup, LV_OBJ_FLAG_HIDDEN);
        az_index_dragging = false;
        az_index_active_binding = NULL;
        az_index_press_judged = false;
        lv_timer_pause(timer);
        return;
    }

    az_index_binding_t * b = az_index_active_binding;
    lv_area_t area;
    lv_obj_get_coords(b->strip, &area);
    int32_t rel_y = p.y - area.y1;
    int32_t h = area.y2 - area.y1;
    if (rel_y < 0) rel_y = 0;
    if (rel_y >= h) rel_y = h - 1;
    int letter_idx = (int) ((int64_t) rel_y * 27 / (h > 0 ? h : 1));
    if (letter_idx > 26) letter_idx = 26;

    int target = az_index_jump_table[letter_idx];
    if (target >= 0) compact_list_scroll_to_index(b->list, target);

    char letter = (letter_idx < 26) ? (char) ('A' + letter_idx) : '#';
    lv_label_set_text_fmt(b->popup_label, "%c", letter);
}

/* ---- Live search (Artists/Albums/Album Artist/All Songs/Files) --
 *
 * A search icon in the title row opens an inline search bar + the
 * existing T9 keypad at the bottom of the SAME screen (reparented via
 * t9_keypad_attach(), see its own doc comment) -- unlike this app's
 * existing modal show_text_entry() flow, the list stays visible and
 * narrows live between them as each character is typed, rather than
 * being hidden behind a full-screen keyboard. The search bar (bg_search.png)
 * is drawn as a full-title-row-width container created AFTER (so on top
 * of) the screen's own back button and title -- covering both rather than
 * needing direct references to hide them, and doubling as a tap-absorbing
 * surface so the covered back button can't be hit through it. The only
 * way out of search while it's open is the close ("x") button; the
 * screen's own back button becomes reachable again once that's tapped. */

#define SEARCH_BAR_Y STATUS_BAR_CLEARANCE
#define SEARCH_BAR_HEIGHT 80

typedef struct {
    lv_obj_t * screen;
    lv_obj_t * list;
    lv_obj_t * search_btn;
    lv_obj_t * search_bar;
    az_index_name_of_t name_of; /* "label for display index i" -- in-memory fallback, only actually used when db_backed is false (Files/Subsonic) */
    const int * count_ptr;      /* likewise -- only read when db_backed is false */
    bool active;
    int * filtered_indices; /* display index -> real index; NULL when not filtering (full list shown) */
    int filtered_count;     /* only meaningful while filtered_indices != NULL */
    bool is_overlay_list;   /* true only for Files -- `list` is a search-results overlay layered on top of
                              * file_browser.c's own folder-browsing UI (opaque, its own screen-colored background),
                              * hidden except while search is active, rather than the screen's one-and-only list
                              * the other four bindings resize in place. */
    bool db_backed; /* true for Artists/Albums/Album Artist/All Songs/Files -- search_apply_filter() below queries
                      * metadata_db_search_names() directly instead of scanning name_of/count_ptr. false only for
                      * the two Subsonic bindings (their own remote-fetched arrays, not this DB). */
    metadata_db_az_kind_t db_kind;              /* only meaningful when db_backed */
    compact_list_fetch_page_cb_t restore_fetch_page; /* re-applied on search_close() to switch the list back to paged
                                                        * mode -- the exact same provider build_*_screen() set up,
                                                        * matching its own compact_list_set_paged_provider() call.
                                                        * Only meaningful when db_backed. */
    char (*filtered_labels)[128]; /* owned label storage backing filtered compact_list_item_t.label pointers, since
                                    * a DB query's result strings (unlike name_of()'s pointers into persistent
                                    * in-memory arrays) have no other long-lived home -- see compact_list_set_items()'s
                                    * own doc comment on why the strings must outlive that call. Only used when
                                    * db_backed; NULL otherwise. */
} search_binding_t;

static search_binding_t search_bindings[SEARCH_BINDING_COUNT];

static search_binding_t * find_search_binding_for_screen(lv_obj_t * screen) {
    for (int i = 0; i < SEARCH_BINDING_COUNT; i++) {
        if (search_bindings[i].screen == screen) return &search_bindings[i];
    }
    return NULL;
}

int search_remap_index(search_binding_id_t binding_id, int display_index) {
    search_binding_t * b = &search_bindings[binding_id];
    if (b->filtered_indices) return b->filtered_indices[display_index];
    return display_index;
}

/* Plain case-insensitive substring match -- not strcasestr(), which needs
 * _GNU_SOURCE defined before <string.h> that this file doesn't set (and
 * musl guards the prototype on it -- confirmed via musl's own string.h).
 * Empty needle matches everything (the "search box is empty" case). */
static bool search_matches(const char * haystack, const char * needle) {
    if (!haystack) return false;
    if (!needle || !needle[0]) return true;
    size_t needle_len = strlen(needle);
    for (const char * h = haystack; *h; h++) {
        size_t i = 0;
        while (i < needle_len && h[i] != '\0' && tolower((unsigned char) h[i]) == tolower((unsigned char) needle[i])) i++;
        if (i == needle_len) return true;
    }
    return false;
}

/* Real, live results a search box could plausibly need to show at once --
 * matches this app's own bounded-search-cache convention (metadata_db_
 * search_songs()'s own cap, remote_control.c's LIBRARY_JSON_MAX_LIMIT).
 * Without this, typing a common single letter against a 30k+-song library
 * linearly scans the whole array and mallocs/copies a match-sized buffer on
 * every keystroke. compact_list_set_items() itself stays cheap at any
 * item_count (only ~20 real row widgets ever exist regardless, unlike
 * populate_indexed_list()'s one-widget-per-row cost), so this isn't a
 * crash risk -- but scrolling through thousands of results nobody will
 * ever reach the end of is a wasted allocation and a bad live-search feel
 * on every single character typed. */
#define SEARCH_RESULTS_MAX 200

/* ---- Live search: async DB query for db_backed bindings -------------
 * Debounces keystrokes via search_debounce_timer and offloads the database
 * search query to a worker pthread, keeping the UI responsive. In-memory
 * bindings search synchronously in search_apply_filter(). */
#define SEARCH_DEBOUNCE_MS 200

static lv_timer_t * search_debounce_timer;

typedef struct {
    metadata_db_az_kind_t db_kind;
    char query[256];
} search_job_request_t;

static pthread_t search_job_thread;
static bool search_job_active = false;
static atomic_bool search_job_done_flag = false;
static uint64_t search_catalog_epoch;
static uint64_t search_job_catalog_epoch;
static search_binding_t * search_job_for_binding;
static int search_job_result_count;
static metadata_db_search_hit_t search_job_result_hits[SEARCH_RESULTS_MAX];
static int64_t search_job_result_identity[SEARCH_RESULTS_MAX];
static uint64_t search_job_result_artwork_key[SEARCH_RESULTS_MAX];

static bool search_job_pending_valid = false;
static search_job_request_t search_job_pending_request;
static search_binding_t * search_job_pending_binding;

static void * search_job_thread_func(void * arg) {
    search_job_request_t * req = (search_job_request_t *) arg;
    search_job_result_count = metadata_db_search_names(req->db_kind, req->query, SEARCH_RESULTS_MAX, search_job_result_hits);
    for (int i = 0; i < search_job_result_count; i++) {
        search_job_result_identity[i] = 0;
        search_job_result_artwork_key[i] = 0;
        if (req->db_kind == METADATA_DB_AZ_ARTIST || req->db_kind == METADATA_DB_AZ_ALBUM_ARTIST) {
            group_row_t group;
            metadata_db_group_kind_t kind = req->db_kind == METADATA_DB_AZ_ARTIST
                                                ? METADATA_DB_GROUP_ARTIST
                                                : METADATA_DB_GROUP_ALBUM_ARTIST;
            if (metadata_db_get_groups_page(kind, search_job_result_hits[i].offset, 1, &group) == 1) {
                search_job_result_identity[i] = group.first_song_id;
                search_job_result_artwork_key[i] = albumart_artist_thumbnail_key(group.name);
            }
        } else if (req->db_kind == METADATA_DB_AZ_ALBUM) {
            group_row_t group;
            if (metadata_db_get_albums_page_filtered(NULL, search_job_result_hits[i].offset, 1, &group) == 1) {
                song_row_t song;
                if (metadata_db_get_song_by_id(group.first_song_id, &song)) {
                    search_job_result_identity[i] = song.id;
                    search_job_result_artwork_key[i] = song_album_thumbnail_key(&song);
                }
            }
        } else {
            song_row_t song;
            if (metadata_db_get_songs_filtered_page(NULL, NULL, NULL, NULL, NULL,
                    search_job_result_hits[i].offset, 1, &song) == 1) {
                search_job_result_identity[i] = song.id;
                search_job_result_artwork_key[i] = song_album_thumbnail_key(&song);
            }
        }
    }
    free(req);
    atomic_store_explicit(&search_job_done_flag, true, memory_order_release); /* written last -- poll_search_job() only checks this flag */
    return NULL;
}

/* At most one job in flight -- a debounce fire arriving while the previous
 * one is still running replaces the pending request rather than queuing,
 * same "only the latest ever matters" shape as launch_cover_decode_req()'s
 * cover_decode_pending. */
static void launch_search_job(search_binding_t * b, const char * query) {
    if (search_job_active) {
        search_job_pending_binding = b;
        search_job_pending_request.db_kind = b->db_kind;
        snprintf(search_job_pending_request.query, sizeof(search_job_pending_request.query), "%s", query);
        search_job_pending_valid = true;
        return;
    }

    search_job_request_t * req = malloc(sizeof(*req));
    if (!req) return;
    req->db_kind = b->db_kind;
    snprintf(req->query, sizeof(req->query), "%s", query);

    search_job_for_binding = b;
    search_job_catalog_epoch = search_catalog_epoch;
    atomic_store_explicit(&search_job_done_flag, false, memory_order_relaxed);
    search_job_active = true;
    if (pthread_create(&search_job_thread, NULL, search_job_thread_func, req) != 0) {
        free(req);
        search_job_active = false;
    }
}

/* Builds compact_list_item_t/filtered_indices/filtered_labels from a
 * finished search job's hits and applies them to b->list. */
static void search_apply_results_to_list(search_binding_t * b, const metadata_db_search_hit_t * hits,
                                         const int64_t * identities, const uint64_t * artwork_keys,
                                         int matched) {
    compact_list_item_t * items = malloc(sizeof(compact_list_item_t) * (size_t) (matched > 0 ? matched : 1));
    int * indices = malloc(sizeof(int) * (size_t) (matched > 0 ? matched : 1));
    char(*labels)[128] = malloc(sizeof(*labels) * (size_t) (matched > 0 ? matched : 1));
    if (!items || !indices || !labels) {
        free(items);
        free(indices);
        free(labels);
        return;
    }
    for (int i = 0; i < matched; i++) {
        snprintf(labels[i], sizeof(labels[i]), "%.127s", hits[i].label);
        items[i] = (compact_list_item_t) {
            .label = labels[i],
            .identity = identities[i],
            .artwork_key = artwork_keys[i],
            .artwork_name = (b->db_kind == METADATA_DB_AZ_ARTIST ||
                             b->db_kind == METADATA_DB_AZ_ALBUM_ARTIST) ? labels[i] : NULL
        };
        indices[i] = hits[i].offset;
    }

    compact_list_set_items(b->list, items, matched);
    free(items);

    free(b->filtered_indices);
    free(b->filtered_labels);
    b->filtered_indices = indices;
    b->filtered_labels = labels;
    b->filtered_count = matched;
}

/* Called every tick from update_timer_cb, same as poll_cover_decode()/poll_
 * lyrics_load(). Discards the result (never applies it) if the binding it
 * was for is no longer the active search -- the user may have closed
 * search, or switched to a different search-active screen, while the query
 * was in flight. */
void poll_search_job(void) {
    if (!search_job_active || !atomic_load_explicit(&search_job_done_flag, memory_order_acquire)) return;
    search_job_active = false;
    pthread_join(search_job_thread, NULL);

    if (search_job_catalog_epoch == search_catalog_epoch && search_job_for_binding->active &&
        find_search_binding_for_screen(lv_screen_active()) == search_job_for_binding) {
        search_apply_results_to_list(search_job_for_binding, search_job_result_hits,
                                     search_job_result_identity, search_job_result_artwork_key,
                                     search_job_result_count);
    }

    if (search_job_pending_valid) {
        search_job_pending_valid = false;
        launch_search_job(search_job_pending_binding, search_job_pending_request.query);
    }
}

/* Rebuilds binding->list contents to entries matching `query` via
 * compact_list_set_items(). An empty query clears the list.
 * `filtered_indices` maps display rows back to unfiltered indices for
 * search_remap_index(), capped at SEARCH_RESULTS_MAX matches.
 *
 * db_backed bindings query metadata_db_search_names() via a background
 * worker (launch_search_job()). In-memory bindings are filtered synchronously. */
static void search_apply_filter(search_binding_t * b, const char * query) {
    bool have_query = query && query[0];

    if (b->db_backed) {
        if (!have_query) {
            compact_list_set_items(b->list, NULL, 0);
            free(b->filtered_indices);
            b->filtered_indices = NULL;
            b->filtered_count = 0;
            free(b->filtered_labels);
            b->filtered_labels = NULL;
            return;
        }
        /* Deliberately does NOT clear filtered_indices/filtered_labels
         * here -- the previous result set stays on screen until the new
         * one actually lands (search_apply_results_to_list(), above),
         * rather than flashing to empty for however long the background
         * query takes. */
        launch_search_job(b, query);
        return;
    }

    free(b->filtered_indices);
    b->filtered_indices = NULL;
    b->filtered_count = 0;
    free(b->filtered_labels);
    b->filtered_labels = NULL;

    int count = have_query ? *b->count_ptr : 0;
    int cap = count < SEARCH_RESULTS_MAX ? count : SEARCH_RESULTS_MAX;
    compact_list_item_t * items = malloc(sizeof(compact_list_item_t) * (size_t) (cap > 0 ? cap : 1));
    int * indices = malloc(sizeof(int) * (size_t) (cap > 0 ? cap : 1));
    int matched = 0;

    for (int i = 0; i < count && matched < cap; i++) {
        const char * name = b->name_of(i);
        if (!search_matches(name, query)) continue;
        items[matched] = (compact_list_item_t){ name };
        indices[matched] = i;
        matched++;
    }

    compact_list_set_items(b->list, items, matched);
    free(items);

    if (have_query) {
        b->filtered_indices = indices;
        b->filtered_count = matched;
    } else {
        /* Unfiltered -- display index already equals the real index, no
         * remap table needed (also covers count == 0 cleanly). */
        free(indices);
    }
}

/* Fires once SEARCH_DEBOUNCE_MS after the last keystroke -- re-checks
 * everything fresh (not captured at keystroke time) since by definition no
 * newer keystroke has arrived without cancelling and recreating this timer
 * first (see search_textarea_value_changed_cb() below). */
static void search_debounce_timer_cb(lv_timer_t * timer) {
    (void) timer;
    search_debounce_timer = NULL;
    if (!t9_keypad_is_inline_active()) return;
    search_binding_t * b = find_search_binding_for_screen(lv_screen_active());
    if (!b) return;
    search_apply_filter(b, t9_keypad_get_text());
}

void search_textarea_value_changed_cb(lv_event_t * e) {
    (void) e;
    if (!t9_keypad_is_inline_active()) return;
    if (search_debounce_timer) {
        lv_timer_delete(search_debounce_timer);
        search_debounce_timer = NULL;
    }
    search_debounce_timer = lv_timer_create(search_debounce_timer_cb, SEARCH_DEBOUNCE_MS, NULL);
    lv_timer_set_repeat_count(search_debounce_timer, 1);
}

/* Restores a binding's list to its normal full-height, bottom-anchored
 * layout -- the exact geometry build_compact_list_screen() itself uses. */
static void search_restore_list_geometry(lv_obj_t * list) {
    lv_obj_set_size(list, lv_pct(100),
                     lv_display_get_vertical_resolution(lv_display_get_default()) - STATUS_BAR_CLEARANCE - TITLE_ROW_HEIGHT);
    lv_obj_align(list, LV_ALIGN_BOTTOM_MID, 0, 0);
}

/* Enter's meaning while a search binding owns the keypad: hide just the
 * keypad (search bar, typed query, and the current filter all stay) and
 * give the list back the vertical space the keypad occupied -- matches a
 * normal search box where dismissing the on-screen keyboard doesn't clear
 * what you searched for. text_entry_textarea is deliberately NOT
 * reparented back here (unlike the full t9_keypad_release()) so the typed
 * query stays visible in the search bar. */


static void search_open(search_binding_t * b) {
    /* db_backed bindings (Artists/Albums/Album Artist/All Songs) query
     * metadata_db_search_names() directly -- see search_apply_filter()'s own
     * comment -- so opening search on any of them never needs this. Only
     * Files/Subsonic still scan the in-memory name_of/count_ptr arrays. */
    lv_obj_add_flag(b->search_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(b->search_bar, LV_OBJ_FLAG_HIDDEN);

    if (b->is_overlay_list) lv_obj_remove_flag(b->list, LV_OBJ_FLAG_HIDDEN);

    az_index_binding_t * az = find_az_binding_for_screen(b->screen);
    if (az) lv_obj_add_flag(az->strip, LV_OBJ_FLAG_HIDDEN);

    /* Room for the search bar above AND the keypad below -- the keypad
     * attaches at its own fixed TEXT_ENTRY_GRID_X/Y position regardless of
     * which screen it's on, so that's the exact number to stop above. */
    lv_obj_set_size(b->list, lv_pct(100), t9_keypad_get_grid_y() - (SEARCH_BAR_Y + SEARCH_BAR_HEIGHT));
    lv_obj_align(b->list, LV_ALIGN_TOP_MID, 0, SEARCH_BAR_Y + SEARCH_BAR_HEIGHT);

    /* Spans from near the bar's left edge to just before close_btn (at
     * 440-51-8=381). */
    t9_keypad_attach(b->screen, b->search_bar, 8, 14, 365);

    b->active = true;
    if (b->is_overlay_list && b->list == files_search_list) thumbnail_begin_screen(b->list);
    search_apply_filter(b, ""); /* start unfiltered -- also (re)establishes filtered_indices == NULL */
}

static void search_close_internal(search_binding_t * b, bool restore_provider) {
    t9_keypad_release();
    if (b->is_overlay_list && b->list == files_search_list) thumbnail_end_screen(b->list);

    /* Not strictly required for correctness -- poll_search_job()/search_
     * debounce_timer_cb() both already re-check b->active/find_search_
     * binding_for_screen() at fire time and no-op harmlessly if this
     * binding is no longer the active search -- but cancelling outright
     * avoids a pointless wait for a query result nobody will ever see. */
    if (search_debounce_timer) {
        lv_timer_delete(search_debounce_timer);
        search_debounce_timer = NULL;
    }

    lv_obj_add_flag(b->search_bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(b->search_btn, LV_OBJ_FLAG_HIDDEN);

    az_index_binding_t * az = find_az_binding_for_screen(b->screen);
    if (az) lv_obj_remove_flag(az->strip, LV_OBJ_FLAG_HIDDEN);

    if (b->is_overlay_list) {
        lv_obj_add_flag(b->list, LV_OBJ_FLAG_HIDDEN); /* reveals file_browser.c's own folder-browsing UI underneath again */
    } else {
        search_restore_list_geometry(b->list);
    }

    /* Eager search rows can point into filtered_labels. Drop those rows
     * before releasing the labels, including for a hidden overlay list. */
    compact_list_set_items(b->list, NULL, 0);
    free(b->filtered_indices);
    b->filtered_indices = NULL;
    b->filtered_count = 0;
    free(b->filtered_labels);
    b->filtered_labels = NULL;

    if (b->db_backed) {
        if (restore_provider) {
            /* Switches the list back to paged mode instead of rebuilding a
             * static items[] array. */
            int artist_count = 0, album_artist_count = 0, album_count = 0;
            metadata_db_get_group_counts(&artist_count, &album_artist_count, &album_count);
            int total = 0;
            switch (b->db_kind) {
                case METADATA_DB_AZ_ARTIST: total = artist_count; break;
                case METADATA_DB_AZ_ALBUM_ARTIST: total = album_artist_count; break;
                case METADATA_DB_AZ_ALBUM: total = album_count; break;
                case METADATA_DB_AZ_ALL_SONGS: total = (int) metadata_db_get_song_count(); break;
            }
            compact_list_set_paged_provider(b->list, b->restore_fetch_page, NULL, total);
        }
    } else {
        compact_list_item_t * items = NULL;
        int count = *b->count_ptr;
        if (count > 0) {
            items = malloc(sizeof(compact_list_item_t) * (size_t) count);
            for (int i = 0; i < count; i++) items[i] = (compact_list_item_t){ b->name_of(i) };
        }
        compact_list_set_items(b->list, items, count);
        free(items);
    }

    b->active = false;
}

static void search_close(search_binding_t * b) {
    search_close_internal(b, true);
}

/* screen_gesture_event_cb()'s back-swipe hook -- see its own forward
 * declaration for why. */
bool search_close_if_active_for_screen(lv_obj_t * screen) {
    search_binding_t * b = find_search_binding_for_screen(screen);
    if (!b || !b->active) return false;
    search_close(b);
    return true;
}

static void invalidate_search_catalog_results(void) {
    search_catalog_epoch++;
    search_job_pending_valid = false;
}

static void invalidate_library_search_bindings(void) {
    if (search_debounce_timer) {
        lv_timer_delete(search_debounce_timer);
        search_debounce_timer = NULL;
    }
    for (int i = 0; i < SEARCH_BINDING_COUNT; i++) {
        search_binding_t * b = &search_bindings[i];
        if (!b->db_backed || !b->list) continue;
        if (b->active) search_close_internal(b, false);
        else if (b->filtered_labels || b->filtered_indices) {
            /* Files results can be hidden while their eager items still
             * point into filtered_labels, so clear the list before freeing.
             * A binding without results is left alone: clearing a paged
             * library list here reset its scroll before rearm_library_list()
             * could keep it. */
            compact_list_set_items(b->list, NULL, 0);
            free(b->filtered_indices);
            b->filtered_indices = NULL;
            b->filtered_count = 0;
            free(b->filtered_labels);
            b->filtered_labels = NULL;
        }
    }
}

/* screen_gesture_event_cb()'s back-swipe hook for the Files screen: steps up
 * one directory instead of popping the screen, unless already at root. */
bool file_browser_back_if_not_root_for_screen(lv_obj_t * screen) {
    if (screen != files_screen || file_browser_at_root()) return false;
    file_browser_go_up();
    return true;
}

static void search_btn_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    search_binding_id_t id = (search_binding_id_t) (intptr_t) lv_event_get_user_data(e);
    search_open(&search_bindings[id]);
}

static void search_close_btn_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    search_binding_id_t id = (search_binding_id_t) (intptr_t) lv_event_get_user_data(e);
    search_close(&search_bindings[id]);
}

/* Called once per screen right after that screen (and its list) is built,
 * same two call sites (boot-time gui_init() + post-rescan rebuild) the
 * A-Z index already registers at. Builds the initial search icon (top-
 * right of the title row) and the search bar (hidden until search_btn is
 * tapped). */
void register_search(search_binding_id_t id, lv_obj_t * screen, lv_obj_t * list, az_index_name_of_t name_of,
                             const int * count_ptr, bool is_overlay_list, bool db_backed, metadata_db_az_kind_t db_kind,
                             compact_list_fetch_page_cb_t restore_fetch_page) {
    search_binding_t * b = &search_bindings[id];
    free(b->filtered_indices); /* re-registering (post-rescan rebuild) over a binding left mid-filter would otherwise leak this */
    free(b->filtered_labels);

    lv_obj_t * search_btn = build_top_right_icon_button(screen, asset_path("sub_back/btn_search.png"), NULL);
    lv_obj_add_event_cb(search_btn, search_btn_click_cb, LV_EVENT_CLICKED, (void *) (intptr_t) id);
    lv_obj_add_flag(search_btn, LV_OBJ_FLAG_GESTURE_BUBBLE); /* added after finalize_screen_navigation()'s one-time pass, needs this set explicitly -- see screen_gesture_event_cb()'s own comment */

    /* Sized to bg_search.png native dimensions (440x80) and aligned to
     * cover the title row. Includes close button and text input. */
    lv_obj_t * bar = lv_obj_create(screen);
    lv_obj_set_size(bar, 440, SEARCH_BAR_HEIGHT);
    lv_obj_set_pos(bar, 0, SEARCH_BAR_Y);
    lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_image_src(bar, asset_path("sub_back/bg_search.png"), 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    /* lv_obj_create() pulls in the default theme's "card" style, which sets
     * pad_all to a nonzero default -- every lv_obj_set_pos() below for this
     * bar's children is relative to its CONTENT area (inside that padding),
     * so left uncleared, close_btn (and the reparented textarea) land
     * further right/down than intended. Same root cause already documented
     * on volume_popup's own construction. */
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(bar, LV_OBJ_FLAG_CLICKABLE); /* absorbs taps so the covered back button/title beneath can't be hit through it */
    lv_obj_add_flag(bar, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t * close_btn = lv_image_create(bar);
    lv_image_set_src(close_btn, asset_path("sub_back/close.png"));
    lv_obj_set_pos(close_btn, 440 - 51 - 8, 14);
    lv_obj_add_flag(close_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(close_btn, search_close_btn_click_cb, LV_EVENT_CLICKED, (void *) (intptr_t) id);

    /* Same reasoning as search_btn above -- bar and its children were all
     * added after finalize_screen_navigation()'s one-time recursive pass,
     * so a swipe starting on any of them (the bar covers the whole title
     * row) needs this set explicitly to bubble up to
     * screen_gesture_event_cb() at all. */
    lv_obj_add_flag(bar, LV_OBJ_FLAG_GESTURE_BUBBLE);
    enable_gesture_bubble_recursive(bar);

    *b = (search_binding_t){ screen,     list,    search_btn,        bar,      name_of, count_ptr,
                              false,      NULL,    0,                 is_overlay_list, db_backed, db_kind,
                              restore_fetch_page, NULL };
}

/* Files' search (SEARCH_BINDING_FILES) is the one binding with no per-
 * screen row-click callback of its own to add a search_remap_index() line
 * to (see the other four's own callbacks) -- it plays the tapped result
 * directly, building its own playlist fresh in exactly the order currently
 * displayed (the filtered subset if one's active, matching
 * all_songs_row_click_cb's own "build in display order, pass display_index
 * straight through" shape), so display_index never needs remapping here. */
static void files_search_row_click_cb(int display_index) {
    if (library_rescan_active) return;
    search_binding_t * b = &search_bindings[SEARCH_BINDING_FILES];
    int all_songs_display_index = b->filtered_indices ? b->filtered_indices[display_index] : display_index;
    set_player_source_all_songs(all_songs_display_index);
    on_file_selected_lazy_all_songs(all_songs_display_index);
}

/* ---- Playlists (Music submenu) --
 *
 * Favorites and Most Played (top 20 by play count, see metadata_db's
 * song_play_count table) are resolved fresh from the DB on every tap and
 * shown through show_group_songs() -- the exact same drill-down screen
 * Artists/Albums/Album Artist already use -- rather than a screen of their
 * own. Membership can change between visits (unlike Artists/Albums/Album
 * Artist, fixed at scan time), so these two are recomputed fresh every
 * time their row is tapped rather than cached. show_group_songs() takes an
 * owned copy of the small result set, so the DB-returned path array can be
 * released immediately.
 *
 * User-created .m3u playlists (Player screen's "Add to Playlist", or
 * dropped into MUSIC_ROOT_DIR/Playlists by hand) are listed below those
 * from that folder only. ---- */

#define MOST_PLAYED_LIMIT 20
#define RECENTLY_ADDED_LIMIT 50

/* Resolves each of paths[0..count) to a display title via a single-row DB
 * lookup (metadata_db_get_song_by_path()) -- falls back to the raw
 * basename if a path somehow isn't in the library anymore (a stale
 * favorite/playlist entry that outlived a rescan). Caller owns the
 * returned array (free() it); paths[] itself is untouched. Shared by
 * show_favorites()/show_most_played()/show_m3u_playlist() below -- these
 * lists are small (a hand-curated favorites list, MOST_PLAYED_LIMIT, one
 * playlist), so a DB lookup per song costs nothing next to loading the
 * whole library just to resolve a title. */
static group_song_entry_t * build_group_song_entries_from_paths(char ** paths, int count) {
    group_song_entry_t * entries = calloc((size_t) (count > 0 ? count : 1), sizeof(*entries));
    if (!entries) return NULL;
    for (int i = 0; i < count; i++) {
        entries[i].path = strdup(paths[i]);
        song_row_t row = {0};
        snprintf(row.path, sizeof(row.path), "%s", paths[i]);
        char title[384];
        if (metadata_db_get_song_by_path(paths[i], &row)) {
            format_song_identity(&row, title, sizeof(title));
        } else {
            snprintf(title, sizeof(title), "%s", basename_of(paths[i]));
        }
        entries[i].title = strdup(title);
        entries[i].song_id = row.id;
        entries[i].artwork_key = song_album_thumbnail_key(&row);
        if (!entries[i].path || !entries[i].title) {
            free_group_song_entries(entries, count);
            return NULL;
        }
    }
    return entries;
}

/* Plays a song selected via the remote-control web UI within its intended
 * scope (album, playlist, or artist). Falls back to full-library queue if
 * no scope is provided or resolved. */
void play_remote_control_song(const char * song_path, const char * playlist_name, const char * artist_filter,
                                      const char * album_artist_filter, const char * album_filter) {
    if (!song_path || !song_path[0]) return;
    group_song_entry_t * scoped_entries = NULL;
    int scoped_count = 0;
    char scoped_title[128] = "";

    if (playlist_name[0] != '\0') {
        char ** paths = NULL;
        int count = 0;
        bool loaded = false;
        if (strcmp(playlist_name, "@favorites") == 0 || strcmp(playlist_name, "Favorites") == 0) {
            metadata_db_load_favorite_songs(&paths, &count);
            loaded = true;
            snprintf(scoped_title, sizeof(scoped_title), "Favorites");
        } else if (strcmp(playlist_name, "@most_played") == 0 || strcmp(playlist_name, "Most Played") == 0) {
            metadata_db_load_top_played_songs(MOST_PLAYED_LIMIT, &paths, &count);
            loaded = true;
            snprintf(scoped_title, sizeof(scoped_title), "Most Played");
        } else if (strcmp(playlist_name, "@recently_added") == 0 || strcmp(playlist_name, "Recently Added") == 0) {
            metadata_db_load_recently_added_songs(RECENTLY_ADDED_LIMIT, &paths, &count);
            loaded = true;
            snprintf(scoped_title, sizeof(scoped_title), "Recently Added");
        } else {
            char m3u_path[512];
            snprintf(m3u_path, sizeof(m3u_path), "%s/%s.m3u", PLAYLISTS_DIR, playlist_name);
            loaded = file_browser_build_playlist_from_m3u(m3u_path, &paths, &count);
        }
        if (loaded && count > 0) {
            scoped_entries = build_group_song_entries_from_paths(paths, count);
            scoped_count = scoped_entries ? count : 0;
            for (int i = 0; i < count; i++) free(paths[i]);
            free(paths);
            if (scoped_title[0] == '\0') snprintf(scoped_title, sizeof(scoped_title), "%s", playlist_name);
        }
    } else if (album_filter[0] != '\0' && (artist_filter[0] != '\0' || album_artist_filter[0] != '\0')) {
        int64_t count64 = metadata_db_count_songs_filtered(NULL, artist_filter, album_artist_filter, album_filter, NULL);
        if (count64 > 0 && count64 <= INT_MAX) {
            scoped_entries = calloc((size_t) count64, sizeof(*scoped_entries));
            song_row_t rows[64];
            while (scoped_entries && scoped_count < count64) {
                int want = (int) (count64 - scoped_count);
                if (want > 64) want = 64;
                int got = metadata_db_get_songs_filtered_page(NULL, artist_filter, album_artist_filter, album_filter, NULL,
                                                               scoped_count, want, rows);
                if (got <= 0) break;
                for (int i = 0; i < got; i++) {
                    char title[128];
                    metadata_db_song_display_title(&rows[i], title, sizeof(title));
                    scoped_entries[scoped_count + i].path = strdup(rows[i].path);
                    scoped_entries[scoped_count + i].title = strdup(title);
                    scoped_entries[scoped_count + i].song_id = rows[i].id;
                    scoped_entries[scoped_count + i].artwork_key = song_album_thumbnail_key(&rows[i]);
                    if (!scoped_entries[scoped_count + i].path || !scoped_entries[scoped_count + i].title) {
                        free_group_song_entries(scoped_entries, (int) count64);
                        scoped_entries = NULL;
                        scoped_count = 0;
                        break;
                    }
                }
                if (!scoped_entries) break;
                scoped_count += got;
                if (got < want) break;
            }
            snprintf(scoped_title, sizeof(scoped_title), "%s", album_filter);
        }
    }

    if (scoped_entries && scoped_count > 0) {
        int pos = -1;
        for (int i = 0; i < scoped_count; i++) {
            if (strcmp(scoped_entries[i].path, song_path) == 0) {
                pos = i;
                break;
            }
        }
        if (pos >= 0) {
            char ** playlist_copy = malloc(sizeof(char *) * (size_t) scoped_count);
            for (int i = 0; i < scoped_count; i++) playlist_copy[i] = strdup(scoped_entries[i].path);
            set_player_source_group_songs_direct(scoped_entries, scoped_count, scoped_title, pos);
            on_file_selected(playlist_copy, scoped_count, pos);
            free_group_song_entries(scoped_entries, scoped_count);
            return;
        }
    }
    free_group_song_entries(scoped_entries, scoped_count);

    int64_t offset = metadata_db_get_song_title_offset(song_path);
    if (offset >= 0 && offset <= INT_MAX) {
        set_player_source_all_songs((int) offset);
        on_file_selected_lazy_all_songs((int) offset);
    }
}

/* Called from apply_track_metadata_to_ui() right after now_playing_path is
 * updated -- the single dispatch point that pushes it out to every now-
 * playing-aware list. Resolves the playing path's own tags via a single DB
 * lookup (metadata_db_get_song_by_path()), then each of Artists/Albums/
 * Album Artist/All Songs gets its own display offset via metadata_db_get_
 * group_offset()/metadata_db_get_song_title_offset() -- Artists/Albums/
 * Album Artist match by name (a whole group's songs share one row, so the
 * indicator lights up whichever artist/album/album-artist the CURRENT
 * TRACK belongs to, not just an exact-song match); All Songs matches by
 * exact song identity. Skips (rather than asserts on) any list that's
 * NULL -- Artists/Albums/Album Artist/All Songs are all built once at
 * startup so in practice this only ever matters before gui_init() finishes
 * building them, but there's no reason to depend on call-order here when a
 * simple guard covers it. */
void refresh_now_playing_indicators(void) {
    int artist_row = -1, album_row = -1, album_artist_row = -1, all_songs_row = -1, recently_added_row = -1;

    song_row_t row;
    if (now_playing_path[0] && metadata_db_get_song_by_path(now_playing_path, &row)) {
        /* The index files a split tag under each artist, so the raw combined
         * string is not a group -- resolve which row it was filed under. Done
         * in one call so the name cannot be split under one delimiter set and
         * looked up in an index built from another, and it gives up rather
         * than waiting on a rebuild that would stall this refresh. */
        int64_t v = 0;
        if (metadata_db_try_artist_row(row.tags.artist, &v) && v >= 0 && v <= INT_MAX) artist_row = (int) v;
        v = metadata_db_get_group_offset(METADATA_DB_GROUP_ALBUM, row.tags.album, row.tags.album_artist);
        if (v >= 0 && v <= INT_MAX) album_row = (int) v;
        v = metadata_db_get_group_offset(METADATA_DB_GROUP_ALBUM_ARTIST, row.tags.album_artist, NULL);
        if (v >= 0 && v <= INT_MAX) album_artist_row = (int) v;
        v = metadata_db_get_song_title_offset(now_playing_path);
        if (v >= 0 && v <= INT_MAX) all_songs_row = (int) v;
        v = metadata_db_get_song_recency_offset(now_playing_path);
        if (v >= 0 && v <= INT_MAX) recently_added_row = (int) v;
    }

    if (artists_list) compact_list_set_now_playing(artists_list, artist_row);
    if (albums_list) compact_list_set_now_playing(albums_list, album_row);
    if (album_artist_list) compact_list_set_now_playing(album_artist_list, album_artist_row);
    if (all_songs_list) compact_list_set_now_playing(all_songs_list, all_songs_row);
    if (recently_added_list) compact_list_set_now_playing(recently_added_list, recently_added_row >= 0 ? recently_added_row + 1 : -1);

    refresh_group_songs_now_playing_indicator(); /* group_songs isn't compact_list-based -- see its own comment */
    refresh_artist_albums_now_playing_indicator(); /* Artists/Album Artist's shared compact album drill-down */
}

static void show_favorites(void) {
    char ** paths;
    int count;
    metadata_db_load_favorite_songs(&paths, &count);

    group_song_entry_t * entries = build_group_song_entries_from_paths(paths, count);
    if (entries) show_group_songs("Favorites", entries, count);
    free_group_song_entries(entries, count);
    for (int i = 0; i < count; i++) free(paths[i]);
    free(paths);
}

static void show_most_played(void) {
    char ** paths;
    int count;
    metadata_db_load_top_played_songs(MOST_PLAYED_LIMIT, &paths, &count);

    group_song_entry_t * entries = build_group_song_entries_from_paths(paths, count);
    if (entries) show_group_songs("Most Played", entries, count);
    free_group_song_entries(entries, count);
    for (int i = 0; i < count; i++) free(paths[i]);
    free(paths);
}

/* Unlike Favorites/Most Played above, Recently Added is not built through
 * show_group_songs() -- it's the whole library just reordered, potentially
 * far larger than either of those two's own small deliberately-capped
 * result sets, so it needs recently_added_screen's own paged compact_list
 * (build_recently_added_screen()) instead. Same "prebuilt screen, just
 * nav_push it" shape as open_queue_screen(). */
static void open_recently_added_screen(void) {
    compact_list_set_paged_provider(recently_added_list, recently_added_fetch_page, NULL,
        (int) metadata_db_get_song_count() + 1);
    nav_push(recently_added_screen);
}

static char ** playlists_m3u_paths = NULL;
static int playlists_m3u_count = 0;

static char playlist_m3u_name[128];

/* The currently open M3U's title survives an in-place row removal. Song
 * paths and titles themselves live in group_songs_entries, independent of
 * the full-library arrays. */
/* Shows an .m3u's contents the same way Favorites/Most Played do -- a
 * tappable song list (show_group_songs_editable()), not straight into
 * playback -- rather than the old behavior of jumping directly to track 0
 * the moment the playlist row itself was tapped, matching every other entry
 * point into a group of songs elsewhere in this screen (Artists/Albums/
 * Album Artist all list first too). m3u_path is passed through as the
 * editable-playlist marker so the Edit/remove UI (group_songs_edit_m3u_path,
 * see its own comment) only ever shows up here, not for Favorites/Most
 * Played/Artists/Albums. */
static void show_m3u_playlist(const char * name, const char * m3u_path, char ** paths, int count) {
    group_song_entry_t * entries = build_group_song_entries_from_paths(paths, count);
    if (count && !entries) return;
    snprintf(playlist_m3u_name, sizeof(playlist_m3u_name), "%s", name);
    group_songs_refresh_source = GROUP_SONGS_REFRESH_M3U;
    snprintf(group_songs_refresh_m3u_path, sizeof(group_songs_refresh_m3u_path), "%s", m3u_path ? m3u_path : "");
    show_group_songs_editable(playlist_m3u_name, entries, count, m3u_path, false);
    free_group_song_entries(entries, count);
}

/* Remove-icon handler for an editable playlist's rows (see
 * populate_group_songs_rows() -- forward-declared near
 * group_songs_edit_m3u_path). Rewrites the underlying .m3u file, then
 * re-reads it back and redraws in place (no nav_push -- this stays on the
 * same group_songs_screen instance, just with the file's new contents)
 * rather than trusting the in-memory indices array to still match the file
 * after the rewrite. */
static void reload_edited_playlist(void) {
    char ** songs = NULL;
    int count = 0;
    if (!playlist_files_read(group_songs_edit_m3u_path, &songs, &count)) {
        show_error_toast("Cannot read playlist"); return;
    }
    group_song_entry_t * entries = count ? build_group_song_entries_from_paths(songs, count) : NULL;
    for (int i = 0; i < count; i++) free(songs[i]);
    free(songs);
    if (count && !entries) return;
    group_songs_file_stat_valid = stat(group_songs_edit_m3u_path, &group_songs_file_stat) == 0;
    set_group_songs_entries(entries, count);
    free_group_song_entries(entries, count);
    populate_group_songs_rows();
}

static void group_song_move_row_cb(lv_event_t * e) {
    int value = (int) (intptr_t) lv_event_get_user_data(e);
    int pos = value / 2;
    int to = pos + (value % 2 ? 1 : -1);
    if (!group_songs_edit_m3u_path || to < 0 || to >= group_songs_count) return;
    if (!group_playlist_unchanged()) { reload_edited_playlist(); show_info_toast("Playlist changed. Try again."); return; }
    if (!playlist_files_edit_entry(group_songs_edit_m3u_path, pos, to)) {
        show_error_toast("Cannot reorder playlist"); return;
    }
    reload_edited_playlist();
}

static void group_song_remove_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int pos = (int) (intptr_t) lv_event_get_user_data(e);
    if (!group_songs_edit_m3u_path || pos < 0 || pos >= group_songs_count) return;
    if (!group_playlist_unchanged()) { reload_edited_playlist(); show_info_toast("Playlist changed. Try again."); return; }
    if (!playlist_files_edit_entry(group_songs_edit_m3u_path, pos, -1)) {
        show_error_toast("Cannot remove entry"); return;
    }
    reload_edited_playlist();
    show_info_toast("Removed from playlist");
}

/* Suppresses the follow-up LV_EVENT_CLICKED event when a long press has
 * already opened the playlist context menu on a user-playlist row. */
static bool playlist_row_long_press_fired = false;

static void playlist_row_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (playlist_row_long_press_fired) { playlist_row_long_press_fired = false; return; }
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    if (index == 0) { show_favorites(); return; }
    if (index == 1) { show_most_played(); return; }
    if (index == 2) { open_queue_screen(); return; }
    if (index == 3) { open_recently_added_screen(); return; }
    int i = index - 4;
    if (i < 0 || i >= playlists_m3u_count) return;
    char ** songs = NULL;
    int count = 0;
    if (!playlist_files_read(playlists_m3u_paths[i], &songs, &count)) {
        show_error_toast("Playlist unavailable or unreadable"); return;
    }
    show_m3u_playlist(basename_of(playlists_m3u_paths[i]), playlists_m3u_paths[i], songs, count);
    for (int j = 0; j < count; j++) free(songs[j]);
    free(songs);
}

/* Playlists' own row shape -- plain rounded rect (LIST_ROW_* look, same as
 * Artists/Albums/All Songs/Files) at LIST_ROW_WIDTH_WIDE rather than
 * add_pill_row_base()'s shared touch_list/item_bg.png pill: that PNG draws
 * at its native size regardless of the row's own width (see LIST_ROW_WIDTH's
 * own comment), so widening a pill row would've just left dead space around
 * an unchanged-size graphic instead of an actually-bigger tap target.
 * add_pill_row_base() itself stays untouched -- it's shared with a dozen
 * unrelated settings screens (Bluetooth DAC, Codec, Font Size, USB Mode,
 * ...) not part of this request. */
static lv_obj_t * add_playlist_row_base(lv_obj_t * parent, const char * label_text) {
    lv_obj_t * row = lv_obj_create(parent);
    lv_obj_set_size(row, LIST_ROW_WIDTH_WIDE, MUSIC_LIST_ROW_HEIGHT);
    lv_obj_add_style(row, &pill_row_bg_style, 0);
    lv_obj_add_style(row, &list_row_pressed_style, LV_STATE_PRESSED);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * label = lv_label_create(row);
    lv_label_set_text(label, label_text);
    lv_obj_add_style(label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(label, &LIST_ROW_FONT, 0);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, LIST_ROW_LABEL_INSET, 0);
    configure_scrolling_row_label(label, LIST_ROW_WIDTH_WIDE - LIST_ROW_LABEL_INSET - BOARD_SCALE_PX(60));
    if (parent == playlists_list) {
        lv_obj_t * chevron = lv_label_create(row);
        lv_label_set_text(chevron, ">");
        lv_obj_add_style(chevron, &style_theme_text_muted, 0);
        lv_obj_set_style_text_font(chevron, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
        lv_obj_align(chevron, LV_ALIGN_RIGHT_MID, -GUI_TEXT_INSET, 0);
    }
    return row;
}

/* Long-press context menu (Rename/Delete) for a user-created .m3u row --
 * never wired onto the Favorites/Most Played/Queue/Recently Added rows (see
 * populate_playlists_screen() below), since none is backed by a real file
 * playlist_files_delete()/playlist_files_rename() could act on. */
static gui_popup_t playlist_delete_popup;
static lv_obj_t * playlist_context_menu_popup, * playlist_context_menu_backdrop;
static char playlist_action_path[PATH_MAX];

static void playlist_delete_cancel_cb(lv_event_t * e) {
    (void) e;
    gui_popup_hide(&playlist_delete_popup);
}

static void playlist_delete_confirm_cb(lv_event_t * e) {
    playlist_delete_cancel_cb(e);
    if (!playlist_files_delete(playlist_action_path)) {
        show_error_toast("Cannot delete playlist"); return;
    }
    metadata_db_playlist_delete_one(playlist_action_path);
    populate_playlists_screen();
    show_info_toast("Playlist deleted");
}

static void playlist_rename_done_cb(const char * name, void * data) {
    (void) data;
    char dest[PATH_MAX];
    if (!playlist_files_rename(playlist_action_path, name, dest, sizeof(dest))) {
        show_error_toast("Cannot rename: invalid name or file exists"); return;
    }
    metadata_db_playlist_delete_one(playlist_action_path);
    metadata_db_playlist_insert_one(dest);
    populate_playlists_screen();
    show_info_toast("Playlist renamed");
}

static void hide_playlist_context_menu_popup(void) {
    if (playlist_context_menu_popup) lv_obj_add_flag(playlist_context_menu_popup, LV_OBJ_FLAG_HIDDEN);
    if (playlist_context_menu_backdrop) lv_obj_add_flag(playlist_context_menu_backdrop, LV_OBJ_FLAG_HIDDEN);
}

static void playlist_context_menu_backdrop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_playlist_context_menu_popup();
}

static void playlist_context_menu_rename_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_playlist_context_menu_popup();
    show_text_entry("Rename Playlist", basename_of(playlist_action_path), false, false, playlist_rename_done_cb, NULL);
}

static void playlist_context_menu_delete_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_playlist_context_menu_popup();
    if (!playlist_delete_popup.popup)
        playlist_delete_popup.popup = build_confirm_popup("Delete playlist?", LV_LABEL_LONG_WRAP, NULL,
            "The playlist file will be deleted. Music files are kept.", "Delete",
            accent_lv_color(), playlist_delete_confirm_cb, NULL, "Cancel", accent_lv_color(),
            playlist_delete_cancel_cb, NULL, playlist_delete_cancel_cb, &playlist_delete_popup.backdrop);
    gui_popup_show(&playlist_delete_popup);
}

static void playlist_context_menu_cancel_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_playlist_context_menu_popup();
}

static void build_playlist_context_menu_popup(void) {
    static const menu_popup_row_t rows[] = {
        { "Rename Playlist", playlist_context_menu_rename_cb, false },
        { "Delete Playlist", playlist_context_menu_delete_cb, true },
        { "Cancel", playlist_context_menu_cancel_cb, false },
    };
    playlist_context_menu_popup = build_menu_popup(rows, (int) (sizeof(rows) / sizeof(rows[0])),
                                                    playlist_context_menu_backdrop_cb,
                                                    &playlist_context_menu_backdrop);
}

static void playlist_row_long_press_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_LONG_PRESSED) return;
    playlist_row_long_press_fired = true;
    int i = (int) (intptr_t) lv_event_get_user_data(e);
    if (i < 0 || i >= playlists_m3u_count) return;
    snprintf(playlist_action_path, sizeof(playlist_action_path), "%s", playlists_m3u_paths[i]);
    lv_obj_remove_flag(playlist_context_menu_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(playlist_context_menu_popup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(playlist_context_menu_backdrop);
    lv_obj_move_foreground(playlist_context_menu_popup);
}

static void populate_playlists_screen(void) {
    lv_obj_clean(playlists_list);

    for (int i = 0; i < playlists_m3u_count; i++) free(playlists_m3u_paths[i]);
    free(playlists_m3u_paths);
    playlists_m3u_paths = NULL;
    playlists_m3u_count = 0;
    /* Persistent cache of PLAYLISTS_DIR only -- see rescan_playlists(). */
    metadata_db_load_all_playlists(&playlists_m3u_paths, &playlists_m3u_count);

    lv_obj_t * create = add_playlist_row_base(playlists_list, "+ New Playlist");
    lv_obj_add_style(create, &style_theme_card_bg, 0);
    lv_obj_add_flag(create, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(create, new_playlist_row_cb, LV_EVENT_CLICKED, (void *) 1);
    build_list_section(playlists_list, "System playlists");

    /* Favorites/Most Played/Queue/Recently Added are never deletable/
     * renamable (none is a real .m3u file -- see playlist_row_click_cb()'s
     * index==0..3 special cases), so they get no long-press handler. */
    lv_obj_t * favorites_row = add_playlist_row_base(playlists_list, "Favorites");
    lv_obj_add_flag(favorites_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(favorites_row, playlist_row_click_cb, LV_EVENT_CLICKED, (void *) (intptr_t) 0);

    lv_obj_t * most_played_row = add_playlist_row_base(playlists_list, "Most Played");
    lv_obj_add_flag(most_played_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(most_played_row, playlist_row_click_cb, LV_EVENT_CLICKED, (void *) (intptr_t) 1);

    lv_obj_t * queue_row = add_playlist_row_base(playlists_list, "Queue");
    lv_obj_add_flag(queue_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(queue_row, playlist_row_click_cb, LV_EVENT_CLICKED, (void *) (intptr_t) 2);

    lv_obj_t * recently_added_row = add_playlist_row_base(playlists_list, "Recently Added");
    lv_obj_add_flag(recently_added_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(recently_added_row, playlist_row_click_cb, LV_EVENT_CLICKED, (void *) (intptr_t) 3);

    build_list_section(playlists_list, "User playlists");
    if (!playlists_m3u_count)
        build_list_message(playlists_list, "No user playlists", "Create a playlist above or copy one to the SD card's Playlists folder.");
    for (int i = 0; i < playlists_m3u_count; i++) {
        const char * display = playlists_m3u_paths[i];
        if (strncmp(display, PLAYLISTS_DIR "/", strlen(PLAYLISTS_DIR) + 1) == 0) display += strlen(PLAYLISTS_DIR) + 1;
        lv_obj_t * row = add_playlist_row_base(playlists_list, display);
        lv_obj_set_width(lv_obj_get_child(row, 0), LIST_ROW_WIDTH_WIDE - LIST_ROW_LABEL_INSET - BOARD_SCALE_PX(60));
        lv_label_set_long_mode(lv_obj_get_child(row, 0), LV_LABEL_LONG_DOT);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, playlist_row_click_cb, LV_EVENT_CLICKED, (void *) (intptr_t) (4 + i));
        lv_obj_add_event_cb(row, playlist_row_long_press_cb, LV_EVENT_LONG_PRESSED, (void *) (intptr_t) i);
    }
}

/* A missing Playlists folder is recreated so the user always has one to use;
 * a read-only card simply fails the mkdir. Callers check the card is mounted
 * so an empty mount point never gets one. */
static void ensure_playlists_dir(void) {
    if (mkdir(PLAYLISTS_DIR, 0755) != 0 && errno != EEXIST)
        DB_LOG("DB", "playlists_mkdir_failed errno=%d", errno);
}

/* Rescans only the Playlists folder; gui_library_poll_playlists() reports
 * the result and repopulates the list. */
static void playlists_refresh_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || playlists_manual_refresh) return;
#ifndef HOST_BUILD
    if (!sd_card_root_is_mounted()) {
        show_error_toast("SD card not available");
        return;
    }
#endif
    ensure_playlists_dir();
    playlists_manual_refresh = true;
    set_header_refresh_action_busy(playlists_refresh_icon, true);
    playlist_files_refresh_async(PLAYLISTS_DIR);
}

static lv_obj_t * build_playlists_screen(void) {
    lv_obj_t * title_label;
    lv_obj_t * scr = build_subsonic_list_screen("Playlists", &title_label, &playlists_list);
    playlists_refresh_icon = build_header_refresh_action(scr, playlists_refresh_cb);
    set_header_refresh_action_busy(playlists_refresh_icon, playlists_manual_refresh);

    /* Explicit cross-axis centering scoped to this screen also keeps rows
     * correct if it is ever hosted in a parent narrower than the display. */
    lv_obj_set_flex_align(playlists_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);

    return scr;
}

/* ---- CUE sheet track list (File Browser -> tap a .cue) ----
 * Allows browsing and jumping straight to any track within a single
 * audio rip (e.g. album.flac + album.cue). Tapping a track seeks the
 * shared audio file to that track's INDEX 01 offset (on_file_selected_at())
 * and begins playback from there. */
static cue_sheet_t current_cue_sheet;
static bool current_cue_sheet_valid = false;
static char current_cue_source_dir[PATH_MAX];
static int current_cue_source_row = -1;

static void cue_track_row_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    if (!current_cue_sheet_valid || index < 0 || index >= current_cue_sheet.track_count) return;

    int count = current_cue_sheet.track_count;
    char ** paths = malloc(sizeof(char *) * (size_t) count);
    if (!paths) return;
    int copied = 0;
    for (; copied < count; copied++) {
        paths[copied] = strdup(current_cue_sheet.audio_path);
        if (!paths[copied]) break;
    }
    if (copied != count) {
        for (int i = 0; i < copied; i++) free(paths[i]);
        free(paths);
        show_error_toast("Not enough memory to load CUE tracks");
        return;
    }
    set_player_source_file_browser(current_cue_source_dir, current_cue_source_row);
    on_file_selected_at(paths, count, index, current_cue_sheet.tracks[index].start_seconds);
}

static void populate_cue_tracks_screen(void) {
    lv_obj_clean(cue_tracks_list);
    for (int i = 0; i < current_cue_sheet.track_count; i++) {
        cue_track_t * t = &current_cue_sheet.tracks[i];
        char label[160];
        /* Falls back to the plain track number when a sheet doesn't set
         * TITLE for a track (rare but real -- some auto-generated sheets
         * only carry INDEX times) rather than showing an empty row. */
        if (t->title[0]) {
            snprintf(label, sizeof(label), "%d. %s", t->number, t->title);
        } else {
            snprintf(label, sizeof(label), "Track %d", t->number);
        }
        lv_obj_t * row = add_playlist_row_base(cue_tracks_list, label);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, cue_track_row_click_cb, LV_EVENT_CLICKED, (void *) (intptr_t) i);
    }
}

static lv_obj_t * build_cue_tracks_screen(void) {
    lv_obj_t * scr = build_subsonic_list_screen("Tracks", &cue_tracks_title_label, &cue_tracks_list);
    lv_obj_set_flex_align(cue_tracks_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    return scr;
}

/* file_browser.h's on_cue_select callback -- see file_browser_init()'s own
 * comment. Parses fresh on every tap (a .cue sheet is tiny, no reason to
 * cache) and replaces whatever sheet this screen was last showing. */
void on_cue_file_selected(const char * cue_path) {
    cue_sheet_free(&current_cue_sheet);
    current_cue_sheet_valid = cue_parse_file(cue_path, &current_cue_sheet);
    if (!current_cue_sheet_valid) {
        show_error_toast("Couldn't read this .cue file");
        return;
    }
    snprintf(current_cue_source_dir, sizeof(current_cue_source_dir), "%s", file_browser_get_last_selected_dir());
    current_cue_source_row = file_browser_get_last_selected_row();
    lv_label_set_text(cue_tracks_title_label, basename_of(current_cue_sheet.audio_path));
    populate_cue_tracks_screen();
    nav_push(cue_tracks_screen);
}

/* Settings > Update Music Database: reruns library_scan_once() (a full
 * rescan + tag re-read of MUSIC_ROOT_DIR, same as gui_init's startup call)
 * on a background thread -- a real library's tag reads are too slow to do
 * on the UI thread, same reasoning as every other pthread_create() in this
 * file -- then rebuilds the four prebuilt library screens (All Songs,
 * Artists, Albums, Album Artist), since unlike group_songs_screen/
 * artist_albums_screen (which rebuild their rows on every visit) these are
 * only ever built once at startup: each one's compact_list_set_paged_
 * provider() call captured a total_count at that build time, which a
 * rescan can make stale (more/fewer songs, artists, albums), so the whole
 * screen is rebuilt to pick up a fresh count rather than trying to patch
 * one in place. Playlists isn't one of these four -- see its own build_
 * playlists_screen()/populate_playlists_screen() comment for why it
 * doesn't need rebuilding here. */
typedef enum {
    LIBRARY_RESCAN_NORMAL,
    LIBRARY_RESCAN_FORCE_METADATA,
    LIBRARY_RESCAN_TARGETED,
    LIBRARY_RESCAN_REFRESH_COVERS_ALL,
    LIBRARY_RESCAN_REFRESH_COVERS_ALBUM
} library_rescan_mode_t;

typedef enum {
    LIBRARY_REFRESH_SCOPE_NONE,
    LIBRARY_REFRESH_SCOPE_PATHS,
    LIBRARY_REFRESH_SCOPE_ALBUM,
    LIBRARY_REFRESH_SCOPE_ARTIST,
    LIBRARY_REFRESH_SCOPE_ALBUM_ARTIST
} library_refresh_scope_t;

static pthread_t library_rescan_thread;
bool library_rescan_active = false;
static atomic_bool library_rescan_done_flag = false;
static library_rescan_mode_t library_rescan_mode;
static char ** library_rescan_target_paths;
static int library_rescan_target_count;
static library_refresh_scope_t library_rescan_scope;
static char library_rescan_scope_name[METADATA_DB_TEXT_MAX];
static char library_rescan_scope_album_artist[METADATA_DB_TEXT_MAX];
static int library_rescan_refreshed_count;
static int library_rescan_changed_count;
/* Forced re-reads that failed (parser timeout/crash); reported even when the
 * refresh otherwise changed nothing and skipped publication. */
static int library_rescan_failed_count;
/* Forced reads whose path already had a row. Paths are unique, so when this
 * equals the live song count every existing row was found on disk. */
static int library_rescan_matched_existing;
static bool library_rescan_identity_changed;
static bool library_rescan_published;
static gui_popup_t refresh_all_metadata_popup;
static gui_popup_t refresh_all_covers_popup;

/* See scan_one_song_into_db()'s own comment on why this exists. Non-static
 * so crash_diag_handler() (main.c) can read it directly from a signal
 * handler -- no getter/mutex, a diagnostic breadcrumb doesn't need either,
 * and calling a function from a signal handler than just returns a global
 * would be no safer than reading the global itself. */
char g_scan_last_path[PATH_MAX] = "";
/* Published to the UI by library_rescan_done_flag's release/acquire pair. */
static bool library_rescan_succeeded;
/* Set when the walk could not read part of the tree, so the toast does not
 * claim a full update. Rows whose file is missing are still removed. */
static bool library_rescan_incomplete;
static bool library_rescan_allow_rebuild;
static bool library_rescan_load_blocked;
static bool library_rescan_saved;
static bool library_rescan_migrating;
static bool library_rescan_migration_cleanup;
static const char library_recovered_message[] = "Library recovered. Use Settings > Update Music Database to save";

static void library_scan_once_with_options(bool force_all_metadata);
static void library_scan_targeted_once(void);
static bool library_scan_open_database(bool allow_rebuild, bool allow_pending_migration,
                                       metadata_db_load_outcome_t * out_open_outcome,
                                       bool * out_preserve_rebuild);
static int compare_scan_target_paths(const void * a, const void * b);
static void reset_library_scan_progress(void);
static void hide_collection_menu(void);
static void refresh_targeted_nested_views(void);
static void invalidate_library_search_bindings(void);
static void invalidate_search_catalog_results(void);

/* Retain the highest-priority outcome observed during an operation. */
static metadata_db_load_outcome_t combine_load_outcomes(metadata_db_load_outcome_t a, metadata_db_load_outcome_t b) {
    if (a == METADATA_DB_LOAD_FAILED || b == METADATA_DB_LOAD_FAILED) return METADATA_DB_LOAD_FAILED;
    if (a == METADATA_DB_LOAD_UNMOUNTED || b == METADATA_DB_LOAD_UNMOUNTED) return METADATA_DB_LOAD_UNMOUNTED;
    if (a == METADATA_DB_LOAD_SUCCESS_RECOVERED || b == METADATA_DB_LOAD_SUCCESS_RECOVERED) return METADATA_DB_LOAD_SUCCESS_RECOVERED;
    if (a == METADATA_DB_LOAD_SUCCESS_NORMAL || b == METADATA_DB_LOAD_SUCCESS_NORMAL) return METADATA_DB_LOAD_SUCCESS_NORMAL;
    return METADATA_DB_LOAD_SUCCESS_FRESH;
}

/* Cache reports are owned by the UI thread. */
static metadata_db_load_outcome_t library_cache_load_outcome;
static bool library_cache_load_pending;
/* "Library loaded" is shown only for an SD hot swap, not at boot. */
static bool library_cache_load_announce_loaded;
static uint32_t library_cache_load_card_generation;
static uint32_t library_cache_load_generation;
static uint32_t library_prompt_card_generation;
static uint32_t library_boot_ready_tick;
static bool library_boot_ready_tick_set;
static bool boot_warmup_requested;
static bool boot_warmup_pending;

typedef enum {
    LIBRARY_PROMPT_NONE,
    LIBRARY_PROMPT_NO_DATABASE,
    LIBRARY_PROMPT_LOAD_FAILED,
    LIBRARY_PROMPT_FILES_CHANGED,
} library_prompt_reason_t;

typedef enum {
    LIBRARY_PROMPT_REQUEST_NONE,
    LIBRARY_PROMPT_REQUEST_PENDING,
    LIBRARY_PROMPT_REQUEST_VISIBLE,
} library_prompt_request_state_t;

typedef struct {
    library_prompt_reason_t reason;
    library_prompt_request_state_t state;
    uint32_t card_generation;
    uint32_t load_generation;
} library_prompt_request_t;

typedef enum {
    LIBRARY_PROMPT_CARD_CURRENT,
    LIBRARY_PROMPT_CARD_CHANGED,
    LIBRARY_PROMPT_CARD_UNKNOWN,
} library_prompt_card_status_t;

static library_prompt_request_t library_prompt_request;
static library_prompt_request_t library_prompt_queued_request;
static gui_popup_t library_update_prompt_popup;
static lv_obj_t * library_update_prompt_title;
static lv_obj_t * library_update_prompt_body;
static lv_obj_t * library_update_prompt_confirm_label;

static library_prompt_card_status_t library_prompt_card_status(void);
static void library_prompt_prepare_cache_load_card(void);
static void library_update_prompt_confirm_cb(lv_event_t * e);
static void library_update_prompt_later_cb(lv_event_t * e);
static void library_update_prompt_backdrop_cb(lv_event_t * e);
static void library_prompt_requeue_visible(void);

static bool library_prompt_request_is_current(const library_prompt_request_t * request) {
    if (!request || request->state == LIBRARY_PROMPT_REQUEST_NONE ||
        request->card_generation != library_prompt_card_generation) return false;
    if (request->reason == LIBRARY_PROMPT_FILES_CHANGED) return true;
    if (request->load_generation != library_cache_load_generation) return false;
    if (request->reason == LIBRARY_PROMPT_NO_DATABASE)
        return library_cache_load_outcome == METADATA_DB_LOAD_SUCCESS_FRESH;
    if (request->reason == LIBRARY_PROMPT_LOAD_FAILED)
        return library_cache_load_outcome == METADATA_DB_LOAD_FAILED;
    return false;
}

static void library_prompt_clear_request(library_prompt_request_t * request) {
    memset(request, 0, sizeof(*request));
}

static void library_prompt_activate_queued_request(void) {
    if (library_prompt_request.state != LIBRARY_PROMPT_REQUEST_NONE ||
        library_prompt_queued_request.state == LIBRARY_PROMPT_REQUEST_NONE) return;
    library_prompt_request = library_prompt_queued_request;
    library_prompt_clear_request(&library_prompt_queued_request);
}

static void library_prompt_queue_request(const library_prompt_request_t * request) {
    if (library_prompt_queued_request.reason == LIBRARY_PROMPT_FILES_CHANGED &&
        request->reason != LIBRARY_PROMPT_FILES_CHANGED) return;
    library_prompt_queued_request = *request;
}

static void library_prompt_enqueue(library_prompt_reason_t reason, uint32_t card_generation) {
    library_prompt_request_t request = {
        .reason = reason,
        .state = LIBRARY_PROMPT_REQUEST_PENDING,
        .card_generation = card_generation,
        .load_generation = library_cache_load_generation,
    };
    if (library_prompt_request.state == LIBRARY_PROMPT_REQUEST_VISIBLE) {
        if (library_prompt_request.reason == LIBRARY_PROMPT_FILES_CHANGED &&
            reason == LIBRARY_PROMPT_FILES_CHANGED) return;
        library_prompt_queue_request(&request);
    } else if (library_prompt_request.state == LIBRARY_PROMPT_REQUEST_PENDING &&
               library_prompt_request.reason == LIBRARY_PROMPT_FILES_CHANGED &&
               reason != LIBRARY_PROMPT_FILES_CHANGED) {
        library_prompt_queue_request(&request);
    } else {
        library_prompt_request = request;
        library_prompt_clear_request(&library_prompt_queued_request);
    }
}

static void library_prompt_drop_all(void) {
    gui_popup_hide(&library_update_prompt_popup);
    library_prompt_clear_request(&library_prompt_request);
    library_prompt_clear_request(&library_prompt_queued_request);
}

void gui_library_invalidate_boot_prompt(void) {
    library_prompt_drop_all();
}

void gui_library_suspend_boot_prompt(void) {
    library_prompt_requeue_visible();
}

static void library_prompt_card_changed(void) {
    library_prompt_card_generation++;
    boot_warmup_pending = false;
    library_prompt_drop_all();
}

void gui_library_request_files_changed_prompt(void) {
    library_prompt_card_status_t card_status = library_prompt_card_status();
    if (card_status == LIBRARY_PROMPT_CARD_CHANGED) {
        library_prompt_card_changed();
        return;
    }
    library_prompt_enqueue(LIBRARY_PROMPT_FILES_CHANGED, library_prompt_card_generation);
}

static bool library_prompt_has_manual_overlay(void) {
    return az_index_dragging ||
           (az_index_active_binding && az_index_active_binding->popup &&
            !lv_obj_has_flag(az_index_active_binding->popup, LV_OBJ_FLAG_HIDDEN)) ||
           (playlist_start_popup && !lv_obj_has_flag(playlist_start_popup, LV_OBJ_FLAG_HIDDEN)) ||
           (playlist_start_backdrop && !lv_obj_has_flag(playlist_start_backdrop, LV_OBJ_FLAG_HIDDEN)) ||
           (playlist_context_menu_popup &&
            !lv_obj_has_flag(playlist_context_menu_popup, LV_OBJ_FLAG_HIDDEN)) ||
           (playlist_context_menu_backdrop &&
            !lv_obj_has_flag(playlist_context_menu_backdrop, LV_OBJ_FLAG_HIDDEN)) ||
           (collection_menu_popup && !lv_obj_has_flag(collection_menu_popup, LV_OBJ_FLAG_HIDDEN)) ||
           (collection_menu_backdrop && !lv_obj_has_flag(collection_menu_backdrop, LV_OBJ_FLAG_HIDDEN)) ||
           (album_collection_menu_popup && !lv_obj_has_flag(album_collection_menu_popup, LV_OBJ_FLAG_HIDDEN)) ||
           (album_collection_menu_backdrop &&
            !lv_obj_has_flag(album_collection_menu_backdrop, LV_OBJ_FLAG_HIDDEN));
}

#define BOOT_SETTLE_MS 8000
static bool library_prompt_environment_idle(void) {
    if (!library_boot_ready_tick_set || lv_tick_elaps(library_boot_ready_tick) < BOOT_SETTLE_MS ||
        !backlight_screen_is_on() || gui_navigation_transition_in_progress() ||
        lv_screen_active() == gui_busy_get_screen() || gui_library_navigation_blocked() ||
        gui_network_boot_prompt_blocked() || gui_shell_boot_prompt_blocked() ||
        gui_lock_screen_is_showing() || gui_player_boot_prompt_blocked() ||
        gui_queue_boot_prompt_blocked() || library_prompt_has_manual_overlay() ||
        gui_notifications_toast_visible()) return false;

    lv_obj_t * active = lv_screen_active();
    if (t9_keypad_is_inline_active() ||
        (gui_text_input_get_screen() && active == gui_text_input_get_screen()) ||
        (gui_network_get_import_wifi_screen() && active == gui_network_get_import_wifi_screen())) return false;

    return gui_popup_visible_count() == 0;
}

/* A visible prompt yields only to something that takes over the screen.
 * Touches on the prompt itself latch the shell's gesture candidates, and a
 * toast can appear under it, so the settle checks above do not apply. */
static bool library_prompt_taken_over(void) {
    return !backlight_screen_is_on() || lv_screen_active() == gui_busy_get_screen() ||
           gui_lock_screen_is_showing() || gui_network_boot_prompt_blocked() ||
           gui_popup_visible_count() != 1;
}

static void library_prompt_update_labels(library_prompt_reason_t reason) {
    switch (reason) {
        case LIBRARY_PROMPT_NO_DATABASE:
            lv_label_set_text(library_update_prompt_title, "No music database");
            lv_label_set_text(library_update_prompt_body,
                              "Build it now? Large libraries can take several minutes.");
            lv_label_set_text(library_update_prompt_confirm_label, "Build");
            break;
        case LIBRARY_PROMPT_LOAD_FAILED:
            lv_label_set_text(library_update_prompt_title, "Music database unavailable");
            lv_label_set_text(library_update_prompt_body, "It could not be loaded. Rebuild it now?");
            lv_label_set_text(library_update_prompt_confirm_label, "Rebuild");
            break;
        case LIBRARY_PROMPT_FILES_CHANGED:
            lv_label_set_text(library_update_prompt_title, "Update music database?");
            lv_label_set_text(library_update_prompt_body, "Files on the card may have changed.");
            lv_label_set_text(library_update_prompt_confirm_label, "Update");
            break;
        case LIBRARY_PROMPT_NONE:
            break;
    }
}

static void library_update_prompt_close(bool show_settings_toast) {
    gui_popup_hide(&library_update_prompt_popup);
    library_prompt_clear_request(&library_prompt_request);
    library_prompt_activate_queued_request();

    if (!show_settings_toast) return;
    show_info_toast("Use Settings > Update Music Database");
}

/* The settle checks only decide when to show the prompt. Once the user taps
 * the action, revalidate what makes the scan unsafe or stale; the tap's own
 * press still has the shell's gesture candidates latched at this point. */
static bool library_prompt_can_run_action(const library_prompt_request_t * request) {
    return library_prompt_request_is_current(request) &&
           library_prompt_card_status() == LIBRARY_PROMPT_CARD_CURRENT &&
           !gui_library_navigation_blocked() && !gui_network_usb_prompt_invalidated();
}

static void library_update_prompt_confirm_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED ||
        !gui_popup_is_visible(&library_update_prompt_popup)) return;
    library_prompt_request_t handled = library_prompt_request;
    gui_popup_hide(&library_update_prompt_popup);
    library_prompt_clear_request(&library_prompt_request);
    if (!library_prompt_can_run_action(&handled)) {
        library_prompt_card_status_t card_status = library_prompt_card_status();
        if (card_status == LIBRARY_PROMPT_CARD_CHANGED) library_prompt_card_changed();
        else if (gui_library_navigation_blocked())
            library_prompt_drop_all();
        else if (library_prompt_request_is_current(&handled)) {
            handled.state = LIBRARY_PROMPT_REQUEST_PENDING;
            library_prompt_request = handled;
        }
        return;
    }
    if (handled.reason == LIBRARY_PROMPT_LOAD_FAILED) start_library_rescan();
    else start_library_auto_rescan();
}

static void library_update_prompt_later_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    library_update_prompt_close(true);
}

static void library_update_prompt_backdrop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    library_update_prompt_close(true);
}

static void build_library_update_prompt_popup(void) {
    library_update_prompt_popup.popup = build_confirm_popup_with_labels(
        "", LV_LABEL_LONG_WRAP, &library_update_prompt_title, "", &library_update_prompt_body,
        "", &library_update_prompt_confirm_label, accent_lv_color(), library_update_prompt_confirm_cb, NULL,
        "Later", lv_color_white(), library_update_prompt_later_cb, NULL,
        library_update_prompt_backdrop_cb, &library_update_prompt_popup.backdrop);
}

static void library_prompt_requeue_visible(void) {
    if (!gui_popup_is_visible(&library_update_prompt_popup)) return;
    gui_popup_hide(&library_update_prompt_popup);
    if (library_prompt_request_is_current(&library_prompt_request))
        library_prompt_request.state = LIBRARY_PROMPT_REQUEST_PENDING;
    else
        library_prompt_clear_request(&library_prompt_request);
}

void gui_library_boot_ready(uint32_t tick) {
    if (library_boot_ready_tick_set) return;
    library_boot_ready_tick = tick;
    library_boot_ready_tick_set = true;
}

void gui_library_poll_boot_prompt(void) {
    bool has_request = library_prompt_request.state != LIBRARY_PROMPT_REQUEST_NONE ||
                       library_prompt_queued_request.state != LIBRARY_PROMPT_REQUEST_NONE;
    library_prompt_card_status_t card_status = LIBRARY_PROMPT_CARD_CURRENT;
    if (has_request || boot_warmup_pending) card_status = library_prompt_card_status();
    if ((has_request || boot_warmup_pending) && card_status == LIBRARY_PROMPT_CARD_CHANGED) {
        library_prompt_card_changed();
        return;
    }
    if (card_status == LIBRARY_PROMPT_CARD_UNKNOWN) {
        library_prompt_requeue_visible();
        return;
    }
    if (has_request && gui_network_usb_prompt_invalidated()) {
        gui_library_suspend_boot_prompt();
        return;
    }
    if (has_request && gui_library_navigation_blocked()) {
        library_prompt_drop_all();
        return;
    }

    if (library_prompt_request.state != LIBRARY_PROMPT_REQUEST_NONE &&
        !library_prompt_request_is_current(&library_prompt_request)) {
        gui_popup_hide(&library_update_prompt_popup);
        library_prompt_clear_request(&library_prompt_request);
    }
    if (library_prompt_queued_request.state != LIBRARY_PROMPT_REQUEST_NONE &&
        !library_prompt_request_is_current(&library_prompt_queued_request))
        library_prompt_clear_request(&library_prompt_queued_request);
    library_prompt_activate_queued_request();

    if (boot_warmup_pending && library_prompt_environment_idle()) {
        int64_t song_count = metadata_db_get_song_count();
        bool warmer_active = atomic_load(&album_thumb_gen_active);
        if (song_count <= 0 || warmer_active) {
            boot_warmup_pending = false;
        } else if (!atomic_load(&album_thumbnail_active)) {
            boot_warmup_pending = false;
            start_album_thumbnail_generation();
        }
    }

    if (library_prompt_request.state == LIBRARY_PROMPT_REQUEST_VISIBLE) {
        if (!gui_popup_is_visible(&library_update_prompt_popup) ||
            library_prompt_taken_over()) library_prompt_requeue_visible();
        return;
    }
    if (library_prompt_request.state != LIBRARY_PROMPT_REQUEST_PENDING ||
        !library_update_prompt_popup.popup || !library_prompt_environment_idle()) return;

    library_prompt_update_labels(library_prompt_request.reason);
    library_prompt_request.state = LIBRARY_PROMPT_REQUEST_VISIBLE;
    gui_popup_show(&library_update_prompt_popup);
}

/* Initialized before pthread_create and published by library_rescan_done_flag. */
static metadata_db_load_outcome_t library_operation_outcome;
static metadata_db_load_outcome_t reload_library_cache(void);

static void report_library_cache_load(void) {
    if (!library_cache_load_pending) return;
    library_cache_load_pending = false;
    if (metadata_db_migration_needed() &&
        library_cache_load_outcome != METADATA_DB_LOAD_FAILED &&
        library_cache_load_outcome != METADATA_DB_LOAD_UNMOUNTED) {
        show_info_toast("Library migration pending. Favourites and play history will be kept");
        return;
    }
    switch (library_cache_load_outcome) {
        case METADATA_DB_LOAD_FAILED:
            library_prompt_enqueue(LIBRARY_PROMPT_LOAD_FAILED, library_cache_load_card_generation);
            break;
        case METADATA_DB_LOAD_UNMOUNTED: show_error_toast("No SD card"); break;
        case METADATA_DB_LOAD_SUCCESS_RECOVERED: show_info_toast(library_recovered_message); break;
        case METADATA_DB_LOAD_SUCCESS_FRESH:
            library_prompt_enqueue(LIBRARY_PROMPT_NO_DATABASE, library_cache_load_card_generation);
            break;
        case METADATA_DB_LOAD_SUCCESS_NORMAL:
            if (library_cache_load_announce_loaded) show_info_toast("Library loaded");
            break;
    }
}

static void * library_rescan_thread_func(void * arg) {
    install_thread_crash_altstack(); /* see its own comment (main.c) */
#ifdef __linux__
    struct sched_param sp = { 0 };
    sched_setscheduler(0, SCHED_BATCH, &sp);
    setpriority(PRIO_PROCESS, 0, 5);
#endif
    if (library_rescan_mode == LIBRARY_RESCAN_REFRESH_COVERS_ALL ||
        library_rescan_mode == LIBRARY_RESCAN_REFRESH_COVERS_ALBUM) {
        album_thumb_gen_altstack_already_installed = true;
        (void) album_thumb_gen_thread_func(arg);
        library_rescan_succeeded = atomic_load(&album_thumb_gen_error_count) == 0 &&
                                   !atomic_load(&album_thumb_gen_cancel);
        library_rescan_incomplete = atomic_load(&album_thumb_gen_error_count) > 0;
        atomic_store_explicit(&library_rescan_done_flag, true, memory_order_release);
        return NULL;
    }
    uint64_t started_ms = db_log_enabled() ? db_log_now_ms() : 0;
    DB_LOG("DB", "rescan_thread_begin rss_kb=%ld", db_log_rss_kb());
    if (library_rescan_mode == LIBRARY_RESCAN_FORCE_METADATA)
        library_scan_once_with_options(true);
    else if (library_rescan_mode == LIBRARY_RESCAN_TARGETED)
        library_scan_targeted_once();
    else
        library_scan_once();
    metadata_db_migration_cancel();
    DB_LOG("DB", "rescan_thread_end songs=%lld elapsed_ms=%llu rss_kb=%ld",
           (long long) metadata_db_get_song_count(), (unsigned long long) (db_log_now_ms() - started_ms),
           db_log_rss_kb());
    atomic_store_explicit(&library_rescan_done_flag, true, memory_order_release); /* written last -- update_timer_cb only checks this flag */
    return NULL;
}

/* Controls automatic library rescanning on boot / SD mount. */
#define GUI_LIBRARY_AUTO_RESCAN_ENABLED true
bool gui_library_auto_rescan_enabled(void) {
    return GUI_LIBRARY_AUTO_RESCAN_ENABLED;
}

/* 4MB stack size configured for library rescan thread to accommodate deep
 * call chains through metadata parsers and tagcache indexing. */
#define LIBRARY_RESCAN_THREAD_STACK_SIZE (4 * 1024 * 1024)

static void library_rescan_free_target_paths(void) {
    for (int i = 0; i < library_rescan_target_count; i++) free(library_rescan_target_paths[i]);
    free(library_rescan_target_paths);
    library_rescan_target_paths = NULL;
    library_rescan_target_count = 0;
    library_rescan_scope = LIBRARY_REFRESH_SCOPE_NONE;
    library_rescan_scope_name[0] = '\0';
    library_rescan_scope_album_artist[0] = '\0';
}

static bool start_library_rescan_job(bool allow_rebuild, library_rescan_mode_t mode,
                                     char ** target_paths, int target_count,
                                     library_refresh_scope_t scope, const char * scope_name,
                                     const char * scope_album_artist) {
    gui_library_invalidate_boot_prompt();
    boot_warmup_pending = false;
    /* Ignore request if a rescan is already running. */
    if (library_rescan_active) {
        for (int i = 0; i < target_count; i++) free(target_paths[i]);
        free(target_paths);
        return false;
    }
    DB_LOG("DB", "rescan_requested existing_songs=%lld rss_kb=%ld",
           (long long) metadata_db_get_song_count(), db_log_rss_kb());
    /* A metadata parser child is already a meaningful peak on this 56 MiB
     * target. Do not overlap it with a previous warmer or lazy cover decode;
     * cancellation is cooperative and bounded by the artwork timeout. */
    quiesce_album_artwork_workers();
    library_rescan_mode = mode;
    library_rescan_target_paths = target_paths;
    library_rescan_target_count = target_count;
    library_rescan_scope = scope;
    snprintf(library_rescan_scope_name, sizeof(library_rescan_scope_name), "%s", scope_name ? scope_name : "");
    snprintf(library_rescan_scope_album_artist, sizeof(library_rescan_scope_album_artist), "%s",
             scope_album_artist ? scope_album_artist : "");
    library_rescan_refreshed_count = 0;
    library_rescan_changed_count = 0;
    library_rescan_failed_count = 0;
    library_rescan_matched_existing = 0;
    library_rescan_identity_changed = false;
    library_rescan_published = false;
    library_rescan_allow_rebuild = allow_rebuild;
    atomic_store_explicit(&library_scan_cancel_requested, false, memory_order_release);
    library_operation_outcome = !allow_rebuild && library_cache_load_pending ? library_cache_load_outcome
                                                                           : METADATA_DB_LOAD_SUCCESS_NORMAL;
    atomic_store_explicit(&library_rescan_done_flag, false, memory_order_relaxed);
    library_rescan_active = true;
    const char * busy_title = metadata_db_migration_needed() ? "Migrating\nmusic database..."
        : mode == LIBRARY_RESCAN_FORCE_METADATA ? "Refreshing\nall metadata..."
        : mode == LIBRARY_RESCAN_TARGETED ? "Refreshing\nmetadata..."
        : "Updating\nmusic database...";
    library_rescan_token = gui_busy_show(busy_title, "");
    gui_busy_set_progress(library_rescan_token, 0);

    pthread_attr_t attr;
    pthread_attr_t * attr_ptr = NULL;
    bool attr_initialized = pthread_attr_init(&attr) == 0;
    if (attr_initialized) {
        if (pthread_attr_setstacksize(&attr, LIBRARY_RESCAN_THREAD_STACK_SIZE) == 0) attr_ptr = &attr;
    }
    bool created = pthread_create(&library_rescan_thread, attr_ptr, library_rescan_thread_func, NULL) == 0;
    if (attr_initialized) pthread_attr_destroy(&attr);
    if (created) library_cache_load_pending = false;
    if (!created) {
        library_operation_outcome = METADATA_DB_LOAD_FAILED;
        library_rescan_active = false;
        gui_busy_hide(library_rescan_token);
        library_rescan_free_target_paths();
        library_rescan_mode = LIBRARY_RESCAN_NORMAL;
        show_error_toast("Thread launch failed");
    }
    return created;
}

static void start_library_rescan_with_repair(bool allow_rebuild) {
    (void) start_library_rescan_job(allow_rebuild, LIBRARY_RESCAN_NORMAL, NULL, 0,
                                   LIBRARY_REFRESH_SCOPE_NONE, NULL, NULL);
}

void start_library_rescan(void) {
    start_library_rescan_with_repair(true);
}

void start_library_auto_rescan(void) {
    start_library_rescan_with_repair(false);
}

static int copy_scan_target_paths(const char * const * paths, int count, char *** out_paths) {
    return metadata_refresh_copy_unique_paths(paths, count, out_paths);
}

void start_library_metadata_refresh_paths(const char * const * paths, int count) {
    char ** copied = NULL;
    int unique_count = copy_scan_target_paths(paths, count, &copied);
    if (unique_count <= 0) {
        show_error_toast("No songs to refresh");
        return;
    }
    if (!start_library_rescan_job(true, LIBRARY_RESCAN_TARGETED, copied, unique_count,
                                  LIBRARY_REFRESH_SCOPE_PATHS, NULL, NULL))
        show_info_toast("Library is busy");
}

void start_library_metadata_refresh_album(const char * album, const char * album_artist) {
    if (!album || !album[0] || !album_artist) {
        show_info_toast("No songs to refresh");
        return;
    }
    if (!start_library_rescan_job(true, LIBRARY_RESCAN_TARGETED, NULL, 0,
                                  LIBRARY_REFRESH_SCOPE_ALBUM, album, album_artist))
        show_info_toast("Library is busy");
}

void start_library_metadata_refresh_artist(metadata_db_group_kind_t kind, const char * name) {
    if (!name || !name[0] || (kind != METADATA_DB_GROUP_ARTIST &&
                              kind != METADATA_DB_GROUP_ALBUM_ARTIST)) {
        show_info_toast("No songs to refresh");
        return;
    }
    library_refresh_scope_t scope = kind == METADATA_DB_GROUP_ALBUM_ARTIST
        ? LIBRARY_REFRESH_SCOPE_ALBUM_ARTIST : LIBRARY_REFRESH_SCOPE_ARTIST;
    if (!start_library_rescan_job(true, LIBRARY_RESCAN_TARGETED, NULL, 0, scope, name, NULL))
        show_info_toast("Library is busy");
}

static void refresh_all_metadata_confirm_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    gui_popup_hide(&refresh_all_metadata_popup);
    (void) start_library_rescan_job(true, LIBRARY_RESCAN_FORCE_METADATA, NULL, 0,
                                   LIBRARY_REFRESH_SCOPE_NONE, NULL, NULL);
}

static void refresh_all_metadata_cancel_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) gui_popup_hide(&refresh_all_metadata_popup);
}

static void refresh_all_metadata_backdrop_cb(lv_event_t * e) {
    refresh_all_metadata_cancel_cb(e);
}

static void artwork_failure_cache_clear_album(const char * album, const char * album_artist) {
    song_row_t songs[64];
    int offset = 0;
    for (;;) {
        int count = metadata_db_get_album_songs(album, album_artist, offset, songs,
                                                 (int) (sizeof(songs) / sizeof(songs[0])));
        if (count <= 0) break;
        for (int i = 0; i < count; i++) artwork_failure_cache_note_success(songs[i].id);
        offset += count;
        if (count < (int) (sizeof(songs) / sizeof(songs[0]))) break;
    }
}

static bool library_cover_reload_storage_available(void) {
    if (!metadata_db_storage_current()) return false;
#ifndef HOST_BUILD
    if (!sd_card_root_is_mounted()) return false;
#endif
    return true;
}

static bool start_library_cover_reload_job(bool all_albums, const song_row_t * target_song,
                                           const char * album, const char * album_artist_key) {
    gui_library_invalidate_boot_prompt();
    boot_warmup_pending = false;
    if (library_rescan_active) {
        show_info_toast("Library is busy");
        return false;
    }
    if (!all_albums && (!target_song || !album || !album_artist_key)) {
        show_error_toast("Album unavailable");
        return false;
    }
    if (!library_cover_reload_storage_available()) {
        show_error_toast("No SD card");
        return false;
    }

    uint64_t target_key = 0;
    int64_t target_song_id = 0;
    if (!all_albums) {
        target_key = song_album_thumbnail_key(target_song);
        target_song_id = target_song->id;
    }

    quiesce_album_artwork_workers();
    gui_player_begin_cover_reload();

    library_rescan_mode = all_albums ? LIBRARY_RESCAN_REFRESH_COVERS_ALL
                                     : LIBRARY_RESCAN_REFRESH_COVERS_ALBUM;
    library_rescan_scope = all_albums ? LIBRARY_REFRESH_SCOPE_NONE : LIBRARY_REFRESH_SCOPE_ALBUM;
    snprintf(library_rescan_scope_name, sizeof(library_rescan_scope_name), "%s",
             all_albums ? "" : album);
    snprintf(library_rescan_scope_album_artist, sizeof(library_rescan_scope_album_artist), "%s",
             all_albums ? "" : album_artist_key);
    library_rescan_target_paths = NULL;
    library_rescan_target_count = 0;
    library_rescan_succeeded = false;
    library_rescan_incomplete = false;
    atomic_store_explicit(&library_scan_cancel_requested, false, memory_order_release);
    atomic_store_explicit(&library_rescan_done_flag, false, memory_order_relaxed);
    library_rescan_active = true;

    if (all_albums) {
        album_thumbnail_cache_clear();
    } else {
        /* Any artist may alias this album or hold a "no art" entry that it
         * could now resolve; those records are cheap to re-derive. */
        album_thumbnail_cache_remove(THUMBNAIL_KEY_ALBUM, target_key);
        album_thumbnail_cache_remove_kind(THUMBNAIL_KEY_ARTIST);
        artist_thumbnail_alias_cache_clear();
        artwork_failure_cache_clear_album(album, album_artist_key);
    }
    gui_player_invalidate_cover_negatives();

    int generation = prepare_album_thumbnail_generation(true, true, !all_albums, target_song_id);
    const char * busy_title = all_albums ? "Refreshing\nall covers..." : "Reloading\ncover...";
    library_rescan_token = gui_busy_show(busy_title, "");
    gui_busy_set_progress(library_rescan_token, 0);

    pthread_attr_t attr;
    pthread_attr_t * attr_ptr = NULL;
    bool attr_initialized = pthread_attr_init(&attr) == 0;
    if (attr_initialized && pthread_attr_setstacksize(&attr, LIBRARY_RESCAN_THREAD_STACK_SIZE) == 0)
        attr_ptr = &attr;
    bool created = pthread_create(&library_rescan_thread, attr_ptr, library_rescan_thread_func,
                                  (void *) (intptr_t) generation) == 0;
    if (attr_initialized) pthread_attr_destroy(&attr);
    if (!created) {
        atomic_store(&album_thumb_gen_active, false);
        atomic_store(&album_thumb_gen_cancel, true);
        library_rescan_active = false;
        gui_player_finish_cover_reload(); /* releases any held player request */
        gui_busy_hide(library_rescan_token);
        library_rescan_mode = LIBRARY_RESCAN_NORMAL;
        show_error_toast("Thread launch failed");
    }
    return created;
}

static void start_library_cover_reload_album(const char * album, const char * album_artist) {
    if (!album || !album[0] || !album_artist) {
        show_error_toast("Album unavailable");
        return;
    }
    song_row_t target_song;
    if (metadata_db_get_album_songs(album, album_artist, 0, &target_song, 1) != 1) {
        show_error_toast("Album unavailable");
        return;
    }
    (void) start_library_cover_reload_job(false, &target_song, album, album_artist);
}

static void start_library_cover_reload_all(void) {
    (void) start_library_cover_reload_job(true, NULL, NULL, NULL);
}

static void refresh_all_covers_confirm_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    gui_popup_hide(&refresh_all_covers_popup);
    start_library_cover_reload_all();
}

static void refresh_all_covers_cancel_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) gui_popup_hide(&refresh_all_covers_popup);
}

static void refresh_all_covers_backdrop_cb(lv_event_t * e) {
    refresh_all_covers_cancel_cb(e);
}

void show_library_refresh_all_covers_prompt(void) {
    if (!refresh_all_covers_popup.popup) {
        refresh_all_covers_popup.popup = build_confirm_popup(
            "Refresh all covers?", LV_LABEL_LONG_WRAP, NULL,
            "Removes saved covers and extracts them again. This may take a while.",
            "Refresh", accent_lv_color(), refresh_all_covers_confirm_cb, NULL,
            "Cancel", accent_lv_color(), refresh_all_covers_cancel_cb, NULL,
            refresh_all_covers_backdrop_cb, &refresh_all_covers_popup.backdrop);
    }
    gui_popup_show(&refresh_all_covers_popup);
}

void show_library_refresh_all_metadata_prompt(void) {
    if (!refresh_all_metadata_popup.popup) {
        refresh_all_metadata_popup.popup = build_confirm_popup(
            "Refresh all metadata?", LV_LABEL_LONG_WRAP, NULL,
            "Reads the tags of every song again. This may take a while.",
            "Refresh", accent_lv_color(), refresh_all_metadata_confirm_cb, NULL,
            "Cancel", accent_lv_color(), refresh_all_metadata_cancel_cb, NULL,
            refresh_all_metadata_backdrop_cb, &refresh_all_metadata_popup.backdrop);
    }
    gui_popup_show(&refresh_all_metadata_popup);
}

/* Rebuilds the five prebuilt library screens (All Songs, Artists, Albums,
 * Album Artist, Recently Added) so each one's compact_list_set_paged_
 * provider() call recaptures a fresh total_count against the just-reloaded
 * library --
 * unlike group_songs_screen/artist_albums_screen (which rebuild their rows
 * on every visit) these are only ever built once at startup, so a stale
 * cached count would otherwise persist after ANY reload of the underlying
 * data, not just a full rescan. Shared by poll_library_rescan()'s
 * background-scan-completion path below and reload_library_on_sd_reinsert()'s
 * fast-cache-load path further down, both of which replace that data via a
 * different route (library_scan_once() vs library_load_from_cache_only())
 * but need the exact same screen-side cleanup afterward. playlists_screen
 * deliberately NOT rebuilt here -- see poll_library_rescan()'s own former
 * comment on why (still applies verbatim): its content is recomputed fresh
 * on every visit already, never stale to begin with. */
/* Targeted metadata refresh: the list screens stay alive and only re-read
 * their DB-backed pages. Deleting and rebuilding them here (the full-scan
 * path) is unsafe for a refresh started from inside the library: the busy
 * screen pops back into a library screen, and a back swipe during the save
 * can leave one of the replaced screens active, so a later redraw touched a
 * freed list. Scroll positions are kept; search state is closed before its
 * list is rebound to the current catalog. */
static void rearm_library_list(lv_obj_t * list, compact_list_fetch_page_cb_t fetch_page, int total_count) {
    if (!list) return;
    for (int i = 0; i < SEARCH_BINDING_COUNT; i++)
        if (search_bindings[i].list == list && search_bindings[i].active) return;
    int32_t scroll_y = lv_obj_get_scroll_y(list);
    compact_list_set_paged_provider(list, fetch_page, NULL, total_count);
    if (scroll_y > 0) {
        lv_obj_update_layout(list);
        lv_obj_scroll_to_y(list, scroll_y, LV_ANIM_OFF);
        compact_list_refresh_visible(list);
    }
}

static void refresh_library_lists_in_place(void) {
    int artist_count = 0, album_artist_count = 0, album_count = 0;
    metadata_db_get_group_counts(&artist_count, &album_artist_count, &album_count);
    (void) artist_thumbnail_alias_cache_resize(artist_count, album_artist_count);
    int song_count = (int) metadata_db_get_song_count();
    rearm_library_list(all_songs_list, all_songs_fetch_page, song_count);
    rearm_library_list(artists_list, artists_fetch_page, artist_count);
    rearm_library_list(albums_list, albums_fetch_page, album_count);
    rearm_library_list(album_artist_list, album_artists_fetch_page, album_artist_count);
    rearm_library_list(recently_added_list, recently_added_fetch_page, song_count + 1);
    refresh_targeted_nested_views();
}

static void invalidate_library_views_after_publication(library_rescan_mode_t completed_mode,
                                                       bool identity_changed) {
    invalidate_search_catalog_results();
    invalidate_library_search_bindings();
    hide_collection_menu();
    if (playlist_start_popup) lv_obj_add_flag(playlist_start_popup, LV_OBJ_FLAG_HIDDEN);
    if (playlist_start_backdrop) lv_obj_add_flag(playlist_start_backdrop, LV_OBJ_FLAG_HIDDEN);
    album_thumbnail_cache_epoch_advance();
    album_thumbnail_view_generation++;
    album_thumbnail_queue_count = 0;
    album_thumbnail_active_key_valid = false;
    /* A song that joined or left an album can stale its cached cover, so an
     * identity change drops the whole cache. Full publications can compact
     * song IDs and change artist representatives, so also refresh artist
     * covers, aliases, and failures after those commits. Targeted refreshes
     * keep song IDs stable and retain those records. */
    gui_player_invalidate_cover_negatives();
    if (identity_changed) {
        album_thumbnail_cache_clear();
    } else if (completed_mode != LIBRARY_RESCAN_TARGETED) {
        album_thumbnail_cache_remove_kind(THUMBNAIL_KEY_ARTIST);
        artist_thumbnail_alias_cache_clear();
        artwork_failure_cache_clear();
    }
}

static void refresh_library_screens_after_reload(bool clear_thumbnail_cache) {
    /* A hot-insert cache reload can arrive while one of these screens is
     * the active LVGL screen. Deleting an active screen leaves the display's
     * act_scr pointer dangling; the next refresh then crashes in
     * lv_obj_update_layout(). A user-triggered scan has its non-library
     * progress screen active, so the old screens can be replaced safely. */
    /* Every deletion path (scan publication, SD removal, repair, reinsert)
     * comes through here. An active search owns the shared keypad and
     * textarea on the screen about to be deleted, so close all searches and
     * reject pending results first. Both helpers are idempotent. */
    invalidate_search_catalog_results();
    invalidate_library_search_bindings();
    if (clear_thumbnail_cache) album_thumbnail_cache_clear();
    if (lv_screen_active() != gui_busy_get_screen()) {
        nav_reset_to_home();
    }
    /* Remove replaced screens from navigation and the back-target cache. */
    lv_obj_t * being_replaced[] = { all_songs_screen, artists_screen, albums_screen, album_artist_screen,
                                     recently_added_screen };
    gui_navigation_remove_screen_instances(being_replaced, (int)(sizeof(being_replaced) / sizeof(being_replaced[0])));

    lv_obj_delete(all_songs_screen);
    lv_obj_delete(artists_screen);
    lv_obj_delete(albums_screen);
    lv_obj_delete(album_artist_screen);
    lv_obj_delete(recently_added_screen);
    forget_deleted_library_lists();
    /* Each build_*_screen() below activates its own paged provider against
     * the current (fresh, post-reload) library internally -- no separate
     * populate step needed here, and no whole-library load either: drill-
     * down, search, and the A-Z index are all DB-backed end to end now, so
     * a rescan/SD-reinsert reload costs exactly what a fresh boot does,
     * nothing more. */
    all_songs_screen = build_all_songs_screen();
    artists_screen = build_artists_screen();
    albums_screen = build_albums_screen();
    album_artist_screen = build_album_artist_screen();
    recently_added_screen = build_recently_added_screen();

    /* The four old screens' A-Z index bindings (strip/popup/list pointers)
     * just went dangling along with the lv_obj_delete()s above -- re-register
     * against the freshly rebuilt screens/lists before anything can poll them. */
    reset_az_index_bindings();
    register_az_index(artists_screen, artists_list, METADATA_DB_AZ_ARTIST);
    register_az_index(albums_screen, albums_list, METADATA_DB_AZ_ALBUM);
    register_az_index(album_artist_screen, album_artist_list, METADATA_DB_AZ_ALBUM_ARTIST);
    register_az_index(all_songs_screen, all_songs_list, METADATA_DB_AZ_ALL_SONGS);

    register_search(SEARCH_BINDING_ARTISTS, artists_screen, artists_list, NULL, NULL, false,
                     true, METADATA_DB_AZ_ARTIST, artists_fetch_page);
    register_search(SEARCH_BINDING_ALBUMS, albums_screen, albums_list, NULL, NULL, false,
                     true, METADATA_DB_AZ_ALBUM, albums_fetch_page);
    register_search(SEARCH_BINDING_ALBUM_ARTIST, album_artist_screen, album_artist_list, NULL,
                     NULL, false, true, METADATA_DB_AZ_ALBUM_ARTIST, album_artists_fetch_page);
    register_search(SEARCH_BINDING_ALL_SONGS, all_songs_screen, all_songs_list, NULL, NULL, false,
                     true, METADATA_DB_AZ_ALL_SONGS, all_songs_fetch_page);
}

/* SD reinsertion loads an existing saved database and queue without walking
 * the whole tree again. A card with no database still needs its first scan;
 * users can explicitly update a saved database after changing files. */
static void reload_library_on_sd_reinsert(bool announce_loaded) {
    quiesce_album_artwork_workers();
    album_thumbnail_cache_clear();
    playlist_files_refresh_async(PLAYLISTS_DIR);
    invalidate_search_catalog_results();
    invalidate_library_search_bindings();
    library_load_from_cache_only();
    library_cache_load_announce_loaded = announce_loaded;
    refresh_library_screens_after_reload(false); /* cache already cleared above, before the reload */
    report_library_cache_load();
    if (library_cache_load_outcome != METADATA_DB_LOAD_SUCCESS_FRESH &&
        metadata_db_get_song_count() > 0) {
        boot_warmup_pending = false;
        start_album_thumbnail_generation();
    }
}

/* How long the "Library updated" success message stays up once a rescan
 * finishes, before falling back to Home -- purely so the user gets a
 * moment to actually read it, not a wait for anything real. */
#define LIBRARY_RESCAN_SUCCESS_MS 1500
bool library_rescan_success_pending = false;
static uint32_t library_rescan_success_since_tick = 0;

static void finish_library_cover_reload(bool cancelled) {
    library_rescan_mode_t completed_mode = library_rescan_mode;
    bool all_albums = completed_mode == LIBRARY_RESCAN_REFRESH_COVERS_ALL;
    int error_count = atomic_load(&album_thumb_gen_error_count);
    library_rescan_active = false;
    pthread_join(library_rescan_thread, NULL);
    (void) album_thumbnail_commit_boot_preload();
    refresh_all_thumbnail_lists();
    gui_player_finish_cover_reload();
    /* Pops back to the screen the reload was started from. */
    gui_busy_hide(library_rescan_token);
    library_rescan_free_target_paths();
    library_rescan_mode = LIBRARY_RESCAN_NORMAL;
    if (cancelled) return;
    if (error_count > 0) {
        if (all_albums) show_error_toast("Some covers could not be refreshed");
        else show_error_toast("Could not reload cover");
    } else if (all_albums) {
        show_info_toast("Covers refreshed");
    } else {
        show_info_toast("Cover reloaded");
    }
}

void poll_library_rescan(void) {
    /* Cache-only SD reinsertion also starts a generation pass, but has no
     * progress phase of its own. Reap that naturally completed worker here
     * instead of retaining one joinable thread until some future scan. */
    if (album_thumb_gen_thread_joinable && !atomic_load(&album_thumb_gen_active))
        reap_album_thumbnail_generation();

    /* Deferred work is not an active worker/wakelock. Retry after the
     * failure-cache backoff and once visible-row lazy work drains. */
    if (atomic_load(&album_thumb_gen_retry_pending) &&
        !atomic_load(&album_thumb_gen_active) && !atomic_load(&album_thumbnail_active) &&
        !album_thumbnail_lazy_work_pending() && !library_rescan_active &&
        !sd_format_active &&
        (!audio_is_playing() ||
         (atomic_load(&album_thumb_gen_covers_during_playback) &&
          atomic_load(&album_thumb_gen_playback_pass_epoch) != atomic_load(&album_thumbnail_cache_epoch))) &&
        lv_tick_elaps(album_thumb_gen_retry_tick) >= 15000 &&
#ifndef HOST_BUILD
        sd_card_root_is_mounted() &&
#endif
        artwork_check_memory_admission(ARTWORK_PRIO_WARMER, ALBUM_ART_METADATA_START_BYTES))
        start_album_thumbnail_generation();

    /* Keep the screen awake for the rescan progress display so the auto
     * screen-timeout does not trigger mid-rescan. */
    if (library_rescan_active || library_rescan_success_pending)
        lv_display_trigger_activity(NULL);

    if (library_rescan_success_pending) {
        if (lv_tick_elaps(library_rescan_success_since_tick) >= LIBRARY_RESCAN_SUCCESS_MS) {
            library_rescan_success_pending = false;
            nav_reset_to_home(); /* leaves the busy screen and discards any stale deeper screen */
        }
        return;
    }

    if (!library_rescan_active) {
        report_library_cache_load();
        return;
    }

    if (!atomic_load_explicit(&library_rescan_done_flag, memory_order_acquire)) {
        if (library_rescan_mode == LIBRARY_RESCAN_REFRESH_COVERS_ALL ||
            library_rescan_mode == LIBRARY_RESCAN_REFRESH_COVERS_ALBUM) {
            int total = atomic_load(&album_thumb_gen_total_count);
            int done = atomic_load(&album_thumb_gen_done_count);
            if (total > 0) {
                if (done > total) done = total;
                int progress = (int) ((int64_t) done * 100 / total);
                gui_busy_set_progress(library_rescan_token, progress);
                char detail[96];
                snprintf(detail, sizeof(detail),
                         library_rescan_mode == LIBRARY_RESCAN_REFRESH_COVERS_ALL
                             ? "Refreshing covers\n%d of %d (%d%%)"
                             : "Reloading cover\n%d of %d (%d%%)",
                         done, total, progress);
                gui_busy_set_detail(library_rescan_token, detail);
            } else {
                gui_busy_set_detail(library_rescan_token,
                    library_rescan_mode == LIBRARY_RESCAN_REFRESH_COVERS_ALL
                        ? "Refreshing covers" : "Reloading cover");
            }
            return;
        }
        /* Still scanning -- total stays 0 until the initial file walk
         * finishes (see library_scan_once()), so there's nothing
         * meaningful to show yet in that window; the label just keeps
         * reading "Updating music database..." until then. */
        int total = library_scan_progress_total;
        int done = library_scan_progress_done;
        char detail[96];
        int phase = library_scan_phase;
        if (phase == LIBRARY_SCAN_PHASE_WALK) {
            int items = library_scan_walk_items;
            if (library_rescan_mode == LIBRARY_RESCAN_TARGETED)
                snprintf(detail, sizeof(detail), "Preparing metadata refresh");
            else if (items > 0) snprintf(detail, sizeof(detail), "Looking for music files\n%d items checked", items);
            else snprintf(detail, sizeof(detail), "Looking for music files");
        } else if (phase == LIBRARY_SCAN_PHASE_TAGS && total > 0) {
            if (done > total) done = total;
            gui_busy_set_progress(library_rescan_token, (int32_t) ((int64_t) done * 100 / total));
            snprintf(detail, sizeof(detail), "Reading tags\n%d of %d songs (%d%%)", done, total,
                     (int) ((int64_t) done * 100 / total));
        } else {
            gui_busy_set_progress(library_rescan_token, 100);
            if (library_rescan_mode == LIBRARY_RESCAN_TARGETED)
                snprintf(detail, sizeof(detail), "Saving music database");
            else
                snprintf(detail, sizeof(detail), "Saving music database\nThis can take a few minutes on large libraries");
        }
        gui_busy_set_detail(library_rescan_token, detail);
        return;
    }

    library_rescan_mode_t completed_mode = library_rescan_mode;
    if (completed_mode == LIBRARY_RESCAN_REFRESH_COVERS_ALL ||
        completed_mode == LIBRARY_RESCAN_REFRESH_COVERS_ALBUM) {
        bool scan_cancelled = atomic_load_explicit(&library_scan_cancel_requested, memory_order_acquire);
        finish_library_cover_reload(scan_cancelled || atomic_load(&album_thumb_gen_cancel));
        return;
    }
    library_rescan_active = false;
    bool scan_cancelled = atomic_load_explicit(&library_scan_cancel_requested, memory_order_acquire);
    pthread_join(library_rescan_thread, NULL);

    bool identity_changed = library_rescan_identity_changed;
    bool published = library_rescan_published;
    if (published) invalidate_library_views_after_publication(completed_mode, identity_changed);
    if (scan_cancelled) {
        if (published) {
            if (completed_mode == LIBRARY_RESCAN_TARGETED) refresh_library_lists_in_place();
            else refresh_library_screens_after_reload(completed_mode == LIBRARY_RESCAN_NORMAL);
        }
        gui_busy_hide(library_rescan_token);
        nav_reset_to_home();
        library_rescan_free_target_paths();
        library_rescan_mode = LIBRARY_RESCAN_NORMAL;
        return;
    }

    bool overall_success = library_rescan_succeeded &&
                           library_operation_outcome != METADATA_DB_LOAD_FAILED &&
                           library_operation_outcome != METADATA_DB_LOAD_UNMOUNTED;
    if (published) {
        if (completed_mode == LIBRARY_RESCAN_TARGETED) refresh_library_lists_in_place();
        else refresh_library_screens_after_reload(completed_mode == LIBRARY_RESCAN_NORMAL);
    }

    /* Thumbnail generation is optional cache warming. It must not become a
     * second user-visible phase after a database scan -- no progress screen,
     * no toast. The worker yields when visible rows queue lazy work so those
     * decodes stay first. */
    if (!overall_success) {
        gui_busy_hide(library_rescan_token);
        nav_reset_to_home();
        if (library_rescan_migrating && !library_rescan_migration_cleanup) {
            show_error_toast("Library migration failed. Old library intact. Use Settings > Update Music Database to retry");
        } else if (library_operation_outcome == METADATA_DB_LOAD_UNMOUNTED) {
            show_error_toast("No SD card");
        } else if (library_rescan_load_blocked) {
            if (library_operation_outcome == METADATA_DB_LOAD_SUCCESS_RECOVERED)
                show_info_toast(library_recovered_message);
            else
                show_error_toast("Library unavailable. Use Settings > Update Music Database to rebuild");
        } else {
            show_error_toast("Library update failed. Check SD card and retry");
        }
        library_rescan_free_target_paths();
        library_rescan_mode = LIBRARY_RESCAN_NORMAL;
        return;
    }
    if (published && completed_mode != LIBRARY_RESCAN_TARGETED) start_album_thumbnail_generation();
    gui_busy_hide(library_rescan_token);

    if (completed_mode != LIBRARY_RESCAN_NORMAL) {
        /* Still set from the finished job; reset only when the next starts. */
        /* Parser failures, non-ENOENT stat errors and skipped folders all
         * mean some songs were not re-read. */
        if (library_rescan_failed_count > 0 || library_rescan_incomplete)
            show_error_toast("Some songs could not be read");
        else show_info_toast("Metadata refreshed");
    } else if (library_rescan_migrating) {
        show_info_toast(metadata_db_migration_cleanup_pending()
                            ? "Library migrated. Old database cleanup will retry"
                            : "Library migrated. Favourites and play history kept");
    } else if (library_operation_outcome == METADATA_DB_LOAD_SUCCESS_RECOVERED) {
        show_info_toast(library_rescan_saved
                            ? (library_rescan_incomplete ? "Library recovered and saved, some folders could not be read"
                                                         : "Library recovered and saved")
                            : library_recovered_message);
    } else {
        show_info_toast(library_rescan_incomplete ? "Library updated, some folders could not be read"
                                                  : "Library updated");
    }
    library_rescan_free_target_paths();
    library_rescan_mode = LIBRARY_RESCAN_NORMAL;
    /* A targeted refresh returns to the list it was started from, refreshed
     * in place; only full scans rebuild the library and reset to Home. */
    if (completed_mode == LIBRARY_RESCAN_TARGETED) return;
    library_rescan_success_pending = true;
    library_rescan_success_since_tick = lv_tick_get();
}

/* SD mount-failure detection + Format SD Card -- see poll_sd_card_hotplug()
 * below for the detection/debounce logic and sd_format_card_worker() further
 * down for the actual format sequence. These three are declared here
 * (rather than alongside their real definitions further down) so that both
 * halves of the feature -- the gated detection logic below, and the
 * ungated popups/background thread that come after it in this file -- can
 * see them regardless of which one is defined first. */
static void show_sd_mount_failed_popup(void); /* defined below, alongside its popup */
static bool sd_mount_fail_notified = false;
static bool sd_handoff_pending = false;

#ifndef HOST_BUILD
/* Periodic polling for SD card mount state changes.
 *
 * When an unmounted card is detected, retries mounting. On transition to
 * mounted, reloads the database cache from the card. On transition to
 * unmounted, closes the database, clears library screens, and resets
 * the file browser to root. */
#define SD_CARD_MOUNT_POLL_SECONDS 5

/* Consecutive failed poll cycles (SD_CARD_MOUNT_POLL_SECONDS apart) before
 * the mount-failure popup shows -- 6 * 5s = 30s of genuinely stuck retries,
 * matching this file's own LIBRARY_SCAN_WALK_STALL_TIMEOUT_MS in spirit
 * (long enough that the kernel's own brief card-enumeration window right
 * after physical insertion can't false-positive, short enough the user
 * isn't left staring at an empty library wondering what's wrong). */
#define SD_MOUNT_FAIL_STREAK_THRESHOLD 6

static dev_t last_mounted_device;
static bool have_last_mounted_device;
static char last_card_cid[64];
static bool have_last_card_identity;
static bool prompt_card_change_seen;

static bool sd_card_mount_attached(dev_t * mounted_device) {
    struct stat parent_st, root_st;
    if (stat("/data/mnt", &parent_st) != 0) return false;
    if (stat(MUSIC_ROOT_DIR, &root_st) != 0) return false;
    if (parent_st.st_dev == root_st.st_dev) return false;
    if (mounted_device) *mounted_device = root_st.st_dev;
    return true;
}

static sd_probe_result_t sd_kernel_device_probe(dev_t mounted_device, const char * node) {
    char path[96];
    snprintf(path, sizeof(path), "/sys/class/block/%s/dev", node);
    errno = 0;
    FILE * file = fopen(path, "r");
    if (!file) return errno == ENOENT ? SD_PROBE_ABSENT : SD_PROBE_UNKNOWN;
    unsigned major_number, minor_number;
    int parsed = fscanf(file, "%u:%u", &major_number, &minor_number);
    fclose(file);
    if (parsed != 2) return SD_PROBE_UNKNOWN;
    return mounted_device == makedev(major_number, minor_number) ? SD_PROBE_MATCH : SD_PROBE_DIFFERENT;
}

static bool sd_card_root_is_mounted_observed(bool * attached_observed) {
    dev_t mounted_device;
    bool attached = sd_card_mount_attached(&mounted_device);
    if (attached_observed) *attached_observed = attached;
    if (!attached) return false;
    /* ntfs-3g uses a FUSE filesystem device rather than the underlying MMC
     * device. Its identity is checked by the card CID in the hotplug poll;
     * presence in sysfs still catches a removed card. */
    if (major(mounted_device) != 179) {
        struct stat st;
        if (stat("/sys/class/block/mmcblk0", &st) == 0) return true;
        /* A failed stat that is not "node missing" is not a pulled card. */
        return errno != ENOENT;
    }
    /* The kernel can leave an exFAT mount attached after the card is pulled.
     * /dev/mmcblk0p1 can also be stale if mdev missed the replacement event.
     * Compare the mount's actual device with sysfs, which follows the newly
     * inserted card even when its /dev node has not been recreated. A sysfs
     * read error is not that comparison and must not look like removal. */
    return !sd_card_probes_confirm_removed(
        sd_kernel_device_probe(mounted_device, "mmcblk0p1"),
        sd_kernel_device_probe(mounted_device, "mmcblk0"));
}

bool sd_card_root_is_mounted(void) {
    return sd_card_root_is_mounted_observed(NULL);
}

static int sd_card_sysfs_node_stat(const char * node, int * err) {
    char path[96];
    struct stat st;
    snprintf(path, sizeof(path), "/sys/class/block/%s", node);
    if (stat(path, &st) == 0) {
        *err = 0;
        return 0;
    }
    *err = errno;
    return -1;
}

static bool sd_card_device_node_present(void) {
    int ignored = 0;
    /* Query sysfs rather than /dev: mdev may have left stale nodes behind.
     * A partition-less card has only the whole-disk entry. */
    return sd_card_sysfs_node_stat("mmcblk0p1", &ignored) == 0 ||
           sd_card_sysfs_node_stat("mmcblk0", &ignored) == 0;
}

/* Both nodes missing. A read error on either one is not confirmation. */
static bool sd_card_device_nodes_confirmed_absent(void) {
    int partition_errno = 0, disk_errno = 0;
    int partition_rc = sd_card_sysfs_node_stat("mmcblk0p1", &partition_errno);
    int disk_rc = sd_card_sysfs_node_stat("mmcblk0", &disk_errno);
    return sd_card_sysfs_stats_confirm_absent(partition_rc, partition_errno, disk_rc, disk_errno);
}

/* Whole-disk sysfs entry -- present whenever a card is physically inserted
 * regardless of whether it has a partition table at all, so this is what
 * distinguishes "no card" from "card inserted but can't be mounted" for the
 * mount-failure/Format SD Card flow below (mount_sd_card_if_needed() itself
 * -- see main.c -- already tries every filesystem type this platform
 * actually supports, so a card that's physically present but still never
 * mounts genuinely needs a repartition/reformat, not just a different -t). */
static bool sd_card_base_device_present(void) {
    struct stat st;
    return stat("/sys/class/block/mmcblk0", &st) == 0;
}

static void sd_card_read_cid(char out[64]) {
    out[0] = '\0';
    FILE * file = fopen("/sys/class/block/mmcblk0/device/cid", "r");
    if (!file) return;
    if (fgets(out, 64, file)) out[strcspn(out, "\r\n")] = '\0';
    fclose(file);
}

static library_prompt_card_status_t library_prompt_card_status(void) {
    if (!sd_card_root_is_mounted()) return LIBRARY_PROMPT_CARD_UNKNOWN;
    bool have_current_device = false;
    dev_t current_device = 0;
    have_current_device = sd_card_mount_attached(&current_device);
    if (!have_current_device) return LIBRARY_PROMPT_CARD_UNKNOWN;
    char current_cid[64];
    sd_card_read_cid(current_cid);
    if (sd_card_identity_replaced(true, true, have_last_card_identity,
                                  have_last_mounted_device, last_mounted_device,
                                  have_current_device, current_device,
                                  last_card_cid, current_cid))
        return LIBRARY_PROMPT_CARD_CHANGED;
    if (have_last_card_identity && !current_cid[0]) return LIBRARY_PROMPT_CARD_UNKNOWN;
    return LIBRARY_PROMPT_CARD_CURRENT;
}

static void library_prompt_prepare_cache_load_card(void) {
    if (!sd_card_root_is_mounted()) return;
    dev_t current_device = 0;
    bool have_current_device = sd_card_mount_attached(&current_device);
    char current_cid[64];
    sd_card_read_cid(current_cid);
    bool replaced = sd_card_identity_replaced(true, true, have_last_card_identity,
                                              have_last_mounted_device, last_mounted_device,
                                              have_current_device, current_device,
                                              last_card_cid, current_cid);
    if (replaced) library_prompt_card_changed();
}

static void clear_removed_sd_library(void) {
    quiesce_album_artwork_workers();
    gui_player_handle_sd_unmount();
    metadata_db_close();
    refresh_library_screens_after_reload(true);
    file_browser_reset_to_root();
}

static void show_sd_repair_note(void) {
    const char * message = NULL;
    switch (sd_repair_take_note()) {
    case SD_REPAIR_NOTE_STARTED: message = "Repairing the SD card"; break;
    case SD_REPAIR_NOTE_REPAIRED: message = "SD card repaired"; break;
    case SD_REPAIR_NOTE_STILL_READONLY: message = "SD card is still read-only"; break;
    case SD_REPAIR_NOTE_FAILED: message = "Could not repair the SD card"; break;
    case SD_REPAIR_NOTE_NONE: break;
    }
    if (message) show_error_toast(message);
}

/* Drop file handles so umount can succeed after the kernel has remounted
 * a damaged card read-only underneath an open library. */
static void release_sd_handles_for_repair(void) {
    if (library_rescan_active) gui_library_cancel_scan();
    cancel_album_thumbnail_generation();
    atomic_store(&album_thumb_gen_retry_pending, false);
    gui_player_notify_sd_unmounted_immediate();
    clear_removed_sd_library();
}

void poll_sd_card_hotplug(void) {
    /* Assume already mounted initially since mount_sd_card_if_needed() ran
     * at boot. */
    static bool was_mounted = true;
    /* See its own comment further down, where it's checked -- separate
     * one-shot guard for the boot-time-mount-race case was_mounted's own
     * confirmed-transition logic can silently miss. */
    static bool boot_library_recheck_done = false;
    static time_t last_check = 0;
    static int mount_fail_streak = 0;
    /* Require unmounted state across consecutive polls
     * (SD_UNMOUNT_CONFIRM_STREAK_THRESHOLD) before tearing down library
     * state to debounce brief transient unmounts. */
    static int unmount_confirm_streak = 0;
#define SD_UNMOUNT_CONFIRM_STREAK_THRESHOLD 2

    time_t now = time(NULL);
    if (last_check != 0 && now - last_check < SD_CARD_MOUNT_POLL_SECONDS) return;
    last_check = now;
    show_sd_repair_note();

    bool mount_observed = false;
    bool mounted = sd_card_root_is_mounted_observed(&mount_observed);
    if (mounted && !sd_repair_in_progress()) {
        sd_repair_kick_result_t repair = sd_readonly_repair_kick(release_sd_handles_for_repair);
        if (repair.started || !sd_card_root_is_mounted()) {
            mounted = false;
            mount_observed = false;
            was_mounted = false;
            sd_handoff_pending = true;
            have_last_card_identity = false;
            have_last_mounted_device = false;
            last_card_cid[0] = '\0';
        } else if (repair.released_handles) {
            /* The check could not unmount. Reload the library that was
             * closed to make the unmount attempt. */
            was_mounted = false;
            sd_handoff_pending = true;
        }
    }
    show_sd_repair_note();
    /* Only an attached mount that the kernel probes proved stale may bypass
     * the normal two-poll absence debounce. If the initial mount stat was
     * inconclusive, a successful retry is not evidence of a stale mount. */
    bool stale_mount = sd_card_stale_mount_confirmed(mount_observed, mounted);
    dev_t current_mounted_device = 0;
    /* The mount check above already stat'd this card. This reread can fail
     * on its own; a leftover 0 is not a different device number. */
    bool have_current_device = mounted && sd_card_mount_attached(&current_mounted_device);
    char current_card_cid[64];
    sd_card_read_cid(current_card_cid);
    bool replaced_between_polls = sd_card_identity_replaced(
        mounted, was_mounted, have_last_card_identity, have_last_mounted_device, last_mounted_device,
        have_current_device, current_mounted_device, last_card_cid, current_card_cid);
    if (replaced_between_polls) {
        if (!prompt_card_change_seen) library_prompt_card_changed();
        prompt_card_change_seen = true;
    } else if (!mounted) {
        library_prompt_requeue_visible();
    }
    /* Unconfirmed absence still cancels a scan and, below, blocks writes.
     * It does not latch sd_handoff_pending; that waits for confirmation. */
    if (sd_card_sample_cancels_scan(mounted, replaced_between_polls, library_rescan_active))
        gui_library_cancel_scan();
    if (replaced_between_polls && !library_rescan_active && current_card_cid[0]) {
        /* An old card may have been unmounted and a new one mounted wholly
         * between polls. The mount is valid, but the in-memory library and
         * queue still belong to the previous card. */
        cancel_album_thumbnail_generation();
        atomic_store(&album_thumb_gen_retry_pending, false);
        gui_player_notify_sd_unmounted_immediate();
        clear_removed_sd_library();
        was_mounted = false;
    }
    /* Node-missing is decided before umount changes `mounted`. A sysfs
     * read error is not node-missing; only ENOENT on both nodes is. */
    bool forced_removal = mounted && sd_card_device_nodes_confirmed_absent();
    if (stale_mount || forced_removal) {
        /* -l (lazy): the node's already gone, so there's no real device
         * left to flush to, and a plain umount would fail with EBUSY if
         * this app (or anything else) still has an fd open on a file under
         * MUSIC_ROOT_DIR from right before the card was pulled -- the same
         * reasoning mass_storage_removing.sh already applies to external
         * USB media on this firmware (see its own "umount -l" call). */
        char * argv[] = { (char *) "umount", (char *) "-l", (char *) MUSIC_ROOT_DIR, NULL };
        subprocess_run(argv, NULL, 0);
        mounted = false;
        if (library_rescan_active) gui_library_cancel_scan();
        if (!prompt_card_change_seen) library_prompt_card_changed();
        prompt_card_change_seen = true;
    }

    group_probe_storage_available = mounted;
    if (!mounted) {
        if (group_probe_job && group_probe_job_generation == group_probe_generation)
            cancel_group_song_probes();
        gui_player_notify_sd_unmounted_immediate();
        bool removal_confirmed = sd_card_absence_confirmed(
            stale_mount || forced_removal, 0, SD_UNMOUNT_CONFIRM_STREAK_THRESHOLD);
        if (was_mounted) {
            /* Stop post-scan artwork reads/writes as soon as removal is
             * observed. Joining is deliberately deferred to the next
             * generator start/reap poll so this hotplug callback never
             * blocks the UI on an in-progress image decode. */
            album_thumbnail_cache_epoch_advance();
            cancel_album_thumbnail_generation();
            atomic_store(&album_thumb_gen_retry_pending, false);
            unmount_confirm_streak++;
            removal_confirmed = sd_card_absence_confirmed(
                stale_mount || forced_removal, unmount_confirm_streak,
                SD_UNMOUNT_CONFIRM_STREAK_THRESHOLD);
            if (removal_confirmed && !prompt_card_change_seen) {
                library_prompt_card_changed();
                prompt_card_change_seen = true;
            }
            if (removal_confirmed && !library_rescan_active) {
                /* Close old handles before loading the next card. */
                clear_removed_sd_library();
                was_mounted = false;
                unmount_confirm_streak = 0;
                have_last_card_identity = false;
                have_last_mounted_device = false;
                last_card_cid[0] = '\0';
            }
            /* Else: not yet confirmed (or a rescan from something else,
             * e.g. Settings > Update Music Database, is already running)
             * -- was_mounted deliberately stays true, so a flicker that
             * remounts before reaching the threshold is not a handoff.
             * The latch below follows removal_confirmed, which was sampled
             * before the streak reset, so a confirmed removal stays pending
             * while the scan still holds the database. */
        }
        sd_handoff_pending = sd_card_handoff_after_sample(
            sd_handoff_pending, removal_confirmed, replaced_between_polls);

        /* Tear down state tied to the old card before mounting a replacement.
         * Otherwise a rapid swap can hide the unmounted edge from the next
         * poll and leave the previous card's library and queue in memory. */
        mount_sd_card_if_needed();
        if (sd_card_root_is_mounted()) {
            storage_migrate_legacy_data();
            mount_fail_streak = 0;
            sd_mount_fail_notified = false;
            return;
        }
        /* A read-only card is unmounted while fsck runs. That is not a
         * missing or unmountable card, so it must not offer Format. */
        if (sd_repair_in_progress()) {
            mount_fail_streak = 0;
            sd_mount_fail_notified = false;
            return;
        }

        /* Card is physically there (the whole-disk node exists) but still
         * won't mount after repeated retries. mount_sd_card_if_needed()
         * (main.c) already tries every supported filesystem type against
         * both the first-partition node and, as a fallback, the whole-disk
         * node itself (covering a "superfloppy" card with no partition
         * table/MBR at all) -- so reaching here means either the partition
         * exists but holds something none of those attempts could mount
         * (corruption, or a filesystem this platform genuinely doesn't
         * support), or the raw disk itself does too. Both need the same fix
         * -- reformat -- so this doesn't try to tell them apart. Debounced
         * (SD_MOUNT_FAIL_STREAK_THRESHOLD
         * consecutive failed polls, SD_CARD_MOUNT_POLL_SECONDS apart) so
         * the kernel's own brief card-enumeration window right after
         * physical insertion -- where the node legitimately isn't there
         * *yet* -- doesn't fire a false alarm; not re-shown on every poll
         * once shown once (sd_mount_fail_notified), so the user isn't
         * nagged again until the card actually changes (removed, or a
         * format attempt runs -- both reset it below/in poll_sd_format()). */
        if (sd_card_base_device_present()) {
            mount_fail_streak++;
            if (mount_fail_streak >= SD_MOUNT_FAIL_STREAK_THRESHOLD && !sd_mount_fail_notified && !sd_format_active) {
                sd_mount_fail_notified = true;
                show_sd_mount_failed_popup();
            }
        } else {
            mount_fail_streak = 0;
            sd_mount_fail_notified = false;
        }
        return;
    }

    mount_fail_streak = 0;
    sd_mount_fail_notified = false;
    unmount_confirm_streak = 0; /* seeing "mounted" again cancels any not-yet-confirmed removal */
    /* Same card again does not drop a latch. Only confirmation or a real
     * replacement sets one; passing a cleared streak here leaves a real
     * pending handoff untouched. */
    sd_handoff_pending = sd_card_handoff_after_sample(
        sd_handoff_pending,
        sd_card_absence_confirmed(false, 0, SD_UNMOUNT_CONFIRM_STREAK_THRESHOLD),
        replaced_between_polls);
    if (have_last_card_identity && !current_card_cid[0]) {
        /* A transient CID read failure is an uncertain identity, never a
         * license to restore the old card's queue or cache. */
        gui_player_notify_sd_unmounted_immediate();
        return;
    }
    if (library_rescan_active && (sd_handoff_pending || !was_mounted || replaced_between_polls)) {
        /* Keep the old identity until the scan worker releases its database
         * handles; the next poll must still perform the card handoff. */
        gui_player_notify_sd_unmounted_immediate();
        return;
    }
    /* Every poll that observes the card mounted un-sticks the checkpoint
    * target, even for a transient blip that never reached the confirmed-
     * removal streak above. */
    gui_player_notify_sd_mounted();
    /* Reset file browser and reload fallback fonts before triggering the
     * rescan screen switch. */
    if (sd_handoff_pending && !library_rescan_active) {
        if (was_mounted) clear_removed_sd_library();
        was_mounted = false;
        file_browser_reset_to_root();
        fallback_font_on_sd_mounted();
        reload_library_on_sd_reinsert(true);
        gui_player_restore_sd_queue(false);
        sd_handoff_pending = false;
    } else if (!boot_library_recheck_done) {
        boot_library_recheck_done = true;
        fallback_font_on_sd_mounted();
        if (metadata_db_get_load_outcome() == METADATA_DB_LOAD_UNMOUNTED && !library_rescan_active) {
            file_browser_reset_to_root();
            reload_library_on_sd_reinsert(false);
        }
        /* Covers the boot-time mount race this whole branch exists for: the
         * card genuinely was NOT YET mounted when gui_init() ran its own
         * SD-restore attempt (both use the same sd_card_root_is_mounted()
         * check), but became mounted by this first poll. This is the only
         * other place a "the card actually showed up right after boot"
         * restore can happen --
         * boot_library_recheck_done's one-shot guard means this fires at
         * most once per run. */
        gui_player_restore_sd_queue(true);
    }
    /* Handoff above may have cleared was_mounted. A missed reread must not
     * be stored as device 0, or the next poll looks like a different card. */
    have_last_mounted_device = sd_card_remember_mounted_device(
        have_current_device, current_mounted_device, replaced_between_polls, was_mounted,
        &last_mounted_device, have_last_mounted_device);
    was_mounted = true;
    if (current_card_cid[0]) snprintf(last_card_cid, sizeof(last_card_cid), "%s", current_card_cid);
    /* An empty CID after a handoff must not resurrect the previous card's
     * CID. A known device number remains a valid identity baseline, so a
     * later device change is still detected while CID reads recover. */
    have_last_card_identity = sd_card_identity_is_known(have_last_mounted_device, current_card_cid);
    prompt_card_change_seen = false;
}
#else
bool sd_card_root_is_mounted(void) {
    struct stat st;
    return stat(MUSIC_ROOT_DIR, &st) == 0 && S_ISDIR(st.st_mode);
}

static library_prompt_card_status_t library_prompt_card_status(void) {
    return sd_card_root_is_mounted() ? LIBRARY_PROMPT_CARD_CURRENT : LIBRARY_PROMPT_CARD_UNKNOWN;
}

static void library_prompt_prepare_cache_load_card(void) {
}

void poll_sd_card_hotplug(void) {
}
#endif

/* Actual "Format SD Card" sequence -- runs on a background thread (see
 * start_sd_format()/poll_sd_format() below), since fdisk/mkdosfs can take
 * real time on a slow card and this must never block the UI thread.
 *
 * UNVERIFIED ON REAL HARDWARE. Every command here was chosen from static
 * analysis of the real firmware's own busybox binary (`strings` confirmed
 * mkdosfs/fdisk/dd/umount/partprobe are all compiled-in applets, including
 * fdisk's own interactive prompt text, proving a standard util-linux-style
 * command menu), not from an actual run -- this is a MIPS binary and can't
 * be executed on the dev host, and no live device was available while this
 * was written. Test with a spare/non-critical card before trusting it with
 * a card that matters.
 *
 * Sequence: best-effort unmount, zero the first sector (so fdisk always
 * sees a blank disk regardless of whatever partition table, or lack of
 * one, the card came in with -- avoids needing to conditionally script
 * fdisk's own delete-partition flow for "already has a stale/foreign
 * table"), script fdisk to create one primary partition spanning the whole
 * disk typed W95 FAT32 (LBA), partprobe to force the kernel to notice the
 * new partition, wait for its device node to appear, mkdosfs it, then hand
 * off to the normal mount path and confirm it actually mounted. */
#ifndef HOST_BUILD
static bool sd_format_card_worker(void) {
    char * umount_argv[] = { (char *) "umount", (char *) "-l", (char *) MUSIC_ROOT_DIR, NULL };
    subprocess_run(umount_argv, NULL, 0);

    if (!sd_card_base_device_present()) return false; /* card pulled before/during format */

    char * dd_argv[] = { (char *) "dd", (char *) "if=/dev/zero", (char *) "of=/dev/mmcblk0",
                          (char *) "bs=512", (char *) "count=1", NULL };
    if (!subprocess_run_timeout(dd_argv, NULL, 0, 15000)) return false;

    pid_t fdisk_pid;
    int fdisk_write_fd;
    char * fdisk_argv[] = { (char *) "fdisk", (char *) "/dev/mmcblk0", NULL };
    if (!subprocess_popen_stdin(fdisk_argv, &fdisk_pid, &fdisk_write_fd)) return false;
    /* n(ew) -> p(rimary) -> partition 1 -> default first sector [Enter] ->
     * default last sector [Enter] -> t(ype) -> c (W95 FAT32, LBA) -> w(rite)+exit. */
    const char * fdisk_cmds = "n\np\n1\n\n\nt\nc\nw\n";
    ssize_t ignored = write(fdisk_write_fd, fdisk_cmds, strlen(fdisk_cmds));
    (void) ignored;
    close(fdisk_write_fd);
    subprocess_terminate(fdisk_pid); /* reaps it -- `w` alone isn't guaranteed to have taken effect yet */

    char * partprobe_argv[] = { (char *) "partprobe", (char *) "/dev/mmcblk0", NULL };
    subprocess_run_timeout(partprobe_argv, NULL, 0, 10000);

    bool got_partition = false;
    for (int i = 0; i < 20 && !got_partition; i++) {
        if (sd_card_device_node_present()) { got_partition = true; break; }
        usleep(250000);
    }
    if (!got_partition) return false;

    char * mkdosfs_argv[] = { (char *) "mkdosfs", (char *) "-F", (char *) "32", (char *) "-n",
                               (char *) "MUSIC", (char *) "/dev/mmcblk0p1", NULL };
    if (!subprocess_run_timeout(mkdosfs_argv, NULL, 0, 120000)) return false;

    mount_sd_card_if_needed();
    if (sd_card_root_is_mounted()) storage_migrate_legacy_data();
    return sd_card_root_is_mounted();
}
#else
static bool sd_format_card_worker(void) {
    return false;
}
#endif

static pthread_t sd_format_thread;
static atomic_bool sd_format_done_flag = false;
static volatile bool sd_format_succeeded = false;

static void * sd_format_thread_func(void * arg) {
    (void) arg;
    sd_format_succeeded = sd_format_card_worker();
    atomic_store_explicit(&sd_format_done_flag, true, memory_order_release); /* written last -- poll_sd_format() only checks this flag */
    return NULL;
}

static void start_sd_format(void) {
    if (sd_repair_in_progress()) {
        show_error_toast("SD card repair is still running");
        return;
    }
    gui_library_invalidate_boot_prompt();
    boot_warmup_pending = false;
    atomic_store_explicit(&sd_format_done_flag, false, memory_order_relaxed);
    sd_format_active = true;
    sd_format_token = gui_busy_show("Formatting\nSD Card...", "");
        if (pthread_create(&sd_format_thread, NULL, sd_format_thread_func, NULL) != 0) {
        sd_format_active = false;
        gui_busy_hide(sd_format_token);
        show_error_toast("Thread launch failed");
    }
}

void poll_sd_format(void) {
    if (!sd_format_active || !atomic_load_explicit(&sd_format_done_flag, memory_order_acquire)) return;

    sd_format_active = false;
    pthread_join(sd_format_thread, NULL);
    gui_busy_hide(sd_format_token);

    if (sd_format_succeeded) {
        show_error_toast("SD card formatted");
        sd_mount_fail_notified = false; /* give the freshly-formatted card a clean slate */
        if (!library_rescan_active) {
            start_library_auto_rescan();
            file_browser_reset_to_root();
        }
    } else {
        show_error_toast("SD card format failed");
    }
}

static gui_popup_t sd_mount_failed_popup;
static gui_popup_t sd_format_confirm_popup;

static void hide_sd_mount_failed_popup(void) {
    gui_popup_hide(&sd_mount_failed_popup);
}

static void hide_sd_format_confirm_popup(void) {
    gui_popup_hide(&sd_format_confirm_popup);
}

static void sd_mount_failed_popup_backdrop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_sd_mount_failed_popup();
}

static void sd_mount_failed_dismiss_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_sd_mount_failed_popup();
}

static void sd_format_confirm_popup_backdrop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_sd_format_confirm_popup();
}

static void sd_format_cancel_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_sd_format_confirm_popup();
}

static void sd_mount_failed_format_btn_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_sd_mount_failed_popup();
    gui_popup_show(&sd_format_confirm_popup);
}

static void sd_format_confirm_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_sd_format_confirm_popup();
    start_sd_format();
}

/* Displayed when persistent SD card mount failure is detected. */
static void show_sd_mount_failed_popup(void) {
    gui_popup_show(&sd_mount_failed_popup);
}

static void build_sd_mount_failed_popup(void) {
    sd_mount_failed_popup.popup = build_confirm_popup(
        "SD card couldn't be read", LV_LABEL_LONG_WRAP, NULL,
        "It may have no partition table or a file system this player can't use. "
        "Formatting will erase it and set it up for this player.",
        "Format SD Card", lv_color_make(255, 120, 120), sd_mount_failed_format_btn_cb, NULL, "Dismiss",
        accent_lv_color(), sd_mount_failed_dismiss_cb, NULL, sd_mount_failed_popup_backdrop_cb,
        &sd_mount_failed_popup.backdrop);
}

static void build_sd_format_confirm_popup(void) {
    sd_format_confirm_popup.popup = build_confirm_popup(
        "Erase and format SD card?", LV_LABEL_LONG_WRAP, NULL,
        "This permanently deletes everything on the card. This cannot be undone.", "Format",
        lv_color_make(255, 120, 120), sd_format_confirm_cb, NULL, "Cancel", accent_lv_color(), sd_format_cancel_cb,
        NULL, sd_format_confirm_popup_backdrop_cb, &sd_format_confirm_popup.backdrop);
}

/* Power action overlay -- shown when hw_buttons_consume_power_long_press()
 * fires (see update_timer_cb()'s own consumer block). Same lv_layer_top()
 * overlay shape as sd_mount_failed_popup/sd_format_confirm_popup above,
 * built once and shown/hidden by flag rather than nav_push()'d, so it can
 * appear over whatever screen is currently active. Unlike the countdown
 * this replaced, there is no timer and no implicit action: Power Off and
 * Reboot both fire immediately on tap (idle_shutdown_now()/
 * idle_shutdown_reboot_now(), neither of which returns on a real device),
 * and tapping anywhere else dismisses with no action taken. */
static gui_popup_t power_action_overlay;
static lv_obj_t * power_action_blur_img;
static uint8_t * power_action_blur_bytes;
static lv_image_dsc_t power_action_blur_dsc;

#define POWER_OVERLAY_BLUR_WORK_WIDTH 60
#define POWER_OVERLAY_BLUR_WORK_HEIGHT 100
#define POWER_OVERLAY_BLUR_RADIUS 5
#define POWER_OVERLAY_BLUR_PASSES 3
#define POWER_OVERLAY_DARKEN_NUM 3
#define POWER_OVERLAY_DARKEN_DEN 5

static void power_action_backdrop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    gui_popup_hide(&power_action_overlay);
}

static void power_action_power_off_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    idle_shutdown_now(); /* does not return on a real device */
}

static void power_action_reboot_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    idle_shutdown_reboot_now(); /* does not return on a real device */
}

void start_power_off_countdown(void) {
    /* Snapshot+blur whatever screen is currently behind the overlay fresh
     * on every show -- it can be a different screen each time, and this is
     * cheap enough (tiny work buffer) to do synchronously right before
     * gui_popup_show() rather than showing a stale or blank backdrop for a
     * frame while an async job catches up. */
    uint8_t * bytes = frosted_glass_blur_screen_rgb565(lv_screen_active(),
        POWER_OVERLAY_BLUR_WORK_WIDTH, POWER_OVERLAY_BLUR_WORK_HEIGHT,
        POWER_OVERLAY_BLUR_RADIUS, POWER_OVERLAY_BLUR_PASSES,
        POWER_OVERLAY_DARKEN_NUM, POWER_OVERLAY_DARKEN_DEN,
        BOARD_SCREEN_WIDTH, BOARD_SCREEN_HEIGHT);
    if (bytes) {
        free(power_action_blur_bytes);
        power_action_blur_bytes = bytes;
        memset(&power_action_blur_dsc, 0, sizeof(power_action_blur_dsc));
        power_action_blur_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        power_action_blur_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
        power_action_blur_dsc.header.w = BOARD_SCREEN_WIDTH;
        power_action_blur_dsc.header.h = BOARD_SCREEN_HEIGHT;
        power_action_blur_dsc.header.stride = BOARD_SCREEN_WIDTH * 2;
        power_action_blur_dsc.data = power_action_blur_bytes;
        power_action_blur_dsc.data_size = (uint32_t) BOARD_SCREEN_WIDTH * BOARD_SCREEN_HEIGHT * 2;
        if (power_action_blur_img) {
            lv_image_set_src(power_action_blur_img, &power_action_blur_dsc);
            lv_obj_remove_flag(power_action_blur_img, LV_OBJ_FLAG_HIDDEN);
        }
    } else if (power_action_blur_img) {
        /* Snapshot or allocation failed -- hide rather than show a stale
         * frame from a previous, unrelated screen. The backdrop's own
         * black tint still shows through, so the overlay is not blank. */
        lv_obj_add_flag(power_action_blur_img, LV_OBJ_FLAG_HIDDEN);
    }
    gui_popup_show(&power_action_overlay);
}

#define POWER_ACTION_ICON_PX 150

/* True if icon_path actually resolves to a real file (override or stock),
 * not just a path string asset_path() is always willing to hand back --
 * skips the "S:" LVGL POSIX-driver prefix asset_path() adds before the
 * access() check. These two icons ship as separate PNG assets outside the
 * normal firmware pack (see the .png files' own commit); a build or device
 * that never received them must still show a usable, clearly-labeled
 * button instead of a blank image with a tiny caption under it. */
static bool power_action_icon_exists(const char * icon_path) {
    const char * resolved = asset_path(icon_path);
    if (resolved[0] == 'S' && resolved[1] == ':') resolved += 2;
    return access(resolved, R_OK) == 0;
}

static lv_obj_t * add_power_action_button(lv_obj_t * zone, const char * icon_path,
                                          const char * label_text, lv_event_cb_t cb) {
    bool have_icon = power_action_icon_exists(icon_path);

    /* Taller when the icon is present -- room for the caption label below
     * it (icon + gap + one text line), same idea as the fallback card's
     * own sizing just below. */
    lv_obj_t * btn = lv_obj_create(zone);
    lv_obj_set_size(btn, BOARD_SCALE_PX(POWER_ACTION_ICON_PX),
                    BOARD_SCALE_PX(have_icon ? POWER_ACTION_ICON_PX + 50 : 60));
    lv_obj_center(btn);
    lv_obj_set_style_bg_opa(btn, 0, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    if (have_icon) {
        lv_obj_t * icon = lv_image_create(btn);
        lv_image_set_src(icon, asset_path(icon_path));
        lv_image_set_inner_align(icon, LV_IMAGE_ALIGN_CENTER);
        /* Explicit size alongside scale, not scale alone -- same pairing
         * layout_lucide_topbar_image() (gui_shell.c) already relies on;
         * scale-only left the widget's own layout box at its pre-scale
         * footprint, so it only ever painted a thin sliver of the source
         * image instead of the intended ~150px badge. */
        lv_image_set_scale(icon, (BOARD_SCALE_PX(POWER_ACTION_ICON_PX) * LV_SCALE_NONE + 256) / 512);
        lv_obj_set_size(icon, BOARD_SCALE_PX(POWER_ACTION_ICON_PX), BOARD_SCALE_PX(POWER_ACTION_ICON_PX));
        lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 0);

        lv_obj_t * label = lv_label_create(btn);
        lv_obj_add_style(label, &style_theme_text_primary, 0);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
        lv_label_set_text(label, label_text);
        lv_obj_align_to(label, icon, LV_ALIGN_OUT_BOTTOM_MID, 0, BOARD_SCALE_PX(10));
    } else {
        /* No icon asset available -- the label alone is the whole button,
         * so it needs to read as tappable on its own: bigger text, plus a
         * simple pill background instead of relying on an icon to carry
         * the visual weight. */
        lv_obj_t * label = lv_label_create(btn);
        lv_obj_add_style(label, &style_theme_text_primary, 0);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(label, gui_theme_font(GUI_FONT_ROLE_TITLE), 0);
        lv_label_set_text(label, label_text);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_add_style(btn, &style_theme_card_bg, 0);
        lv_obj_set_style_radius(btn, BOARD_SCALE_PX(28), 0);
        lv_obj_center(label);
    }

    return btn;
}

void build_power_off_countdown_popup(void) {
    /* lv_layer_sys(), not lv_layer_top() -- the status bar band lives on
     * lv_layer_top() and several places in gui_shell.c explicitly
     * re-foreground it above every other lv_layer_top() sibling (quick
     * drawer, drag motion, etc: "the status bar stays above THAT"), which
     * fought back against this popup's own one-time move-to-foreground and
     * left the status bar rendering uncovered/unblurred as a strip across
     * the top. lv_layer_sys() is documented as always above both the
     * normal screen AND lv_layer_top(), so this needs no z-order fight at
     * all -- already the pattern used elsewhere in this codebase (see
     * gui_lyrics.c/gui_network.c/gui_settings.c's own mask objects) for
     * anything that must cover literally everything. */
    lv_obj_t * top = lv_layer_sys();

    power_action_overlay.backdrop = lv_obj_create(top);
    lv_obj_set_size(power_action_overlay.backdrop, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(power_action_overlay.backdrop, lv_color_black(), 0);
    /* Light residual tint over the frosted-glass backdrop image below, for
     * legibility -- the blur itself already darkens the snapshot (see
     * POWER_OVERLAY_DARKEN_NUM/DEN), so this is deliberately much lower
     * than a flat, image-less overlay would need. */
    lv_obj_set_style_bg_opa(power_action_overlay.backdrop, 76, 0); /* 30% -- LV_OPA_* presets only cover multiples of 10 */
    lv_obj_set_style_border_width(power_action_overlay.backdrop, 0, 0);
    /* Default theme padding otherwise insets LV_ALIGN_TOP_LEFT-positioned
     * children (power_action_blur_img below) from the content area, not
     * the true outer edge -- left the blurred backdrop image short of the
     * real top-left corner while its fixed BOARD_SCREEN_WIDTH/HEIGHT size
     * pushed it past the opposite edges instead of just resizing. */
    lv_obj_set_style_pad_all(power_action_overlay.backdrop, 0, 0);
    lv_obj_remove_flag(power_action_overlay.backdrop, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(power_action_overlay.backdrop, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(power_action_overlay.backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(power_action_overlay.backdrop, power_action_backdrop_cb, LV_EVENT_CLICKED, NULL);

    /* Bottom-most child (created first, so later zones/buttons draw on top
     * of it) -- filled in fresh on every start_power_off_countdown() call
     * with a blurred snapshot of whichever screen is behind the overlay at
     * that moment. Hidden until the first real snapshot lands. */
    power_action_blur_img = lv_image_create(power_action_overlay.backdrop);
    lv_obj_set_size(power_action_blur_img, BOARD_SCREEN_WIDTH, BOARD_SCREEN_HEIGHT);
    lv_obj_align(power_action_blur_img, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_remove_flag(power_action_blur_img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(power_action_blur_img, LV_OBJ_FLAG_HIDDEN);

    /* One half-height, full-width, non-clickable zone per button --
     * centering the button within it via lv_obj_center() guarantees an
     * exact upper/lower half split regardless of screen resolution. Not
     * clickable itself so a tap anywhere in a zone outside the button falls
     * through to the backdrop behind it (LVGL skips non-clickable objects
     * during hit-testing), giving the same "tap the background to dismiss"
     * behavior as every other popup here without needing a second explicit
     * hit-test region. */
    lv_obj_t * upper_zone = lv_obj_create(power_action_overlay.backdrop);
    lv_obj_set_size(upper_zone, lv_pct(100), lv_pct(50));
    lv_obj_align(upper_zone, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(upper_zone, 0, 0);
    lv_obj_set_style_border_width(upper_zone, 0, 0);
    lv_obj_remove_flag(upper_zone, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(upper_zone, LV_OBJ_FLAG_CLICKABLE);
    add_power_action_button(upper_zone, "power_action/power.png", "Power Off", power_action_power_off_cb);

    lv_obj_t * lower_zone = lv_obj_create(power_action_overlay.backdrop);
    lv_obj_set_size(lower_zone, lv_pct(100), lv_pct(50));
    lv_obj_align(lower_zone, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(lower_zone, 0, 0);
    lv_obj_set_style_border_width(lower_zone, 0, 0);
    lv_obj_remove_flag(lower_zone, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(lower_zone, LV_OBJ_FLAG_CLICKABLE);
    add_power_action_button(lower_zone, "power_action/reboot.png", "Reboot", power_action_reboot_cb);
}

void update_music_database_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    start_library_rescan();
}

/* No whole-library load anywhere in this app anymore -- all paged/DB-
 * backed now, see build_artists_screen()'s own comment. Drilling into an
 * artist/album-artist's own albums (artist_row_click_cb()/album_artist_
 * row_click_cb()), an album's own songs (album_row_click_cb()), search,
 * and the A-Z index are all direct, targeted DB queries. */
static void artists_tile_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(artists_screen);
}

static void albums_tile_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    /* metadata_db_get_group_counts() is a real DB query, worth skipping
     * outright (not just leaving DB_LOG to no-op on the result) since this
     * runs on every ordinary tap into Albums, not just during a scan. */
    if (db_log_enabled()) {
        int artist_count = 0, album_artist_count = 0, album_count = 0;
        metadata_db_get_group_counts(&artist_count, &album_artist_count, &album_count);
        albums_page_open_requested_ms = db_log_now_ms();
        DB_LOG("ALBUMS_PAGE", "open_requested albums=%d cache_worker_active=%d lazy_active=%d rss_kb=%ld",
               album_count, (int) atomic_load(&album_thumb_gen_active),
               (int) atomic_load(&album_thumbnail_active),
               db_log_rss_kb());
    }
    nav_push(albums_screen);
}

static void album_artist_tile_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(album_artist_screen);
}

static void playlists_tile_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    populate_playlists_screen();
    nav_push(playlists_screen);
}


static void music_files_tile_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(files_screen);
}

static void music_screen_playback_settings_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(gui_settings_get_music_screen());
}

static lv_obj_t * build_music_screen(void) {
    const icon_grid_item_t items[] = {
        { "submenu/files.png", NULL, "Files", music_files_tile_cb, NULL },
        { "submenu/artists.png", NULL, "Artists", artists_tile_cb, NULL },
        { "submenu/albums.png", NULL, "Albums", albums_tile_cb, NULL },
        { "submenu/album_artist.png", NULL, "Album Artist", album_artist_tile_cb, NULL },
        { "submenu/all_songs.png", NULL, "All Songs", all_songs_tile_cb, NULL },
        { "submenu/playlists.png", NULL, "Playlists", playlists_tile_cb, NULL },
    };
    lv_obj_t * scr = build_category_menu_screen("Music", generic_back_cb, items, 6,
                                                &launcher_layout_config.music);
    /* Same real stock-firmware gear icon as the Queue screen's own Options
     * button (sub_back/set.png) -- build_top_right_icon_button() guarantees
     * it lands at exactly the same visual level as this screen's own back
     * arrow, on the opposite corner. Shortcuts straight to Music Settings
     * rather than the full Settings > Music Settings drill-down. */
    build_top_right_icon_button(scr, asset_path("sub_back/set.png"), music_screen_playback_settings_cb);
    finalize_screen_navigation(scr);
    return scr;
}

void gui_library_refresh_music_screen(void) {
    lv_obj_t * old = music_screen;
    lv_obj_t * fresh = build_music_screen();
    if (!fresh) return;
    music_screen = fresh;
    gui_navigation_replace_static_screen(1, old, fresh);
    if (old) lv_obj_delete(old);
}


/* Shared click handler for every plugin-registered Stream Media tile below
 * -- user_data is the tile's index into plugin_manager's own
 * plugin_stream_tiles[] (not an LVGL object), same index-not-object shape
 * plugin_list_row_click_cb() already uses. */
void plugin_stream_tile_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    plugin_manager_stream_tile_clicked(index);
}

/* Subsonic is the one built-in, real, working option here -- Qobuz/Tidal
 * (paid regional subscriptions + a real API integration this project's
 * author couldn't personally test or verify against) and Net Radio (never
 * wired up past a placeholder) were removed rather than left as dead/
 * stalled tiles. Its icon (stream_media/subsonic.png/_s.png) isn't from
 * the stock theme pack at all (Subsonic isn't a stock HiBy feature) --
 * see assets.c's THEME_OVERRIDE_ROOT for how this app adds its own new
 * asset on top of the stock resource pack despite that living on
 * read-only storage on a real device. */

/* Drill-down target for Artists and Album Artists: tapping either shows the
 * artist's own albums (regrouping just their songs by album, via
 * build_groups_by_indices) rather than dumping straight into a flat song
 * list -- tapping an album from there lands on show_group_songs() same as
 * every other terminal list. One persistent screen shared by both callers,
 * same rebuild-in-place approach as group_songs_screen. */
static int artist_albums_group_count;
/* The compact-list page provider may run on its fetch worker while a rapid
 * drill-down replaces the screen's current artist. Snapshot both fields as
 * one pair so an abandoned fetch never observes a torn query. */
static pthread_mutex_t artist_albums_state_mutex = PTHREAD_MUTEX_INITIALIZER;
/* Name/kind of the artist or album artist this screen is currently showing
 * -- set by show_artist_albums(), read back by artist_album_row_click_cb()'s
 * own "All Songs" row (index 0, see that function's own comment) to fetch
 * every song credited to them, flattened across all their albums. */
static char artist_albums_current_name[128];
static metadata_db_group_kind_t artist_albums_current_kind;

static const char * artist_albums_song_filter(void) {
    return artist_albums_current_kind == METADATA_DB_GROUP_ARTIST ? artist_albums_current_name : NULL;
}

/* Defined with the shared bounded artwork cache below. */
static void thumbnail_row_decorator(lv_obj_t * list, lv_obj_t * row, lv_obj_t * image,
                                    int logical_index, int pool_slot, int64_t representative_song_id,
                                    uint64_t artwork_key, const char * artwork_name, void * ctx);

/* Now-playing indicator bar -- same "recreated fresh every populate call"
 * lifecycle as group_songs_now_playing_bar (see its own comment):
 * populate_indexed_list()'s lv_obj_clean(artist_albums_list) destroys
 * whatever was here before, same as every row. */
/* This drill-down is a compact list too: fixed row count regardless of how
 * many albums one artist owns, and therefore able to share the exact album
 * artwork decorator/cache used by Music -> Albums. */
/* Sort scratch for artist_albums_show_all_songs() below -- carries the same
 * owned path/title an eventual group_song_entry_t needs, plus just enough
 * of each song's own tags to order the flattened list the same way a real
 * album listing would (see cmp_artist_song_sort_entry()'s own comment).
 * Freed after the final group_song_entry_t[] is built; its path/title
 * pointers are MOVED there, not copied again. */
typedef struct {
    char * path;
    char * title;
    int64_t song_id;
    uint64_t artwork_key;
    char album[128];
    char album_artist[128];
    int32_t disc_number;
    int32_t track_number;
    bool show_disc_header;
} artist_song_sort_entry_t;

/* Sorts songs by album, then album artist, then disc number, track number,
 * and file path. Grouping by both album and album artist prevents collision
 * between same-titled albums from different artists, while sorting by disc
 * and track preserves listening order within each album. */
static int cmp_artist_song_sort_entry(const void * a, const void * b) {
    const artist_song_sort_entry_t * ea = (const artist_song_sort_entry_t *) a;
    const artist_song_sort_entry_t * eb = (const artist_song_sort_entry_t *) b;
    int c = strcasecmp(ea->album, eb->album);
    if (c) return c;
    c = strcasecmp(ea->album_artist, eb->album_artist);
    if (c) return c;
    int32_t da = ea->disc_number > 0 ? ea->disc_number : 1;
    int32_t db = eb->disc_number > 0 ? eb->disc_number : 1;
    if (da != db) return da < db ? -1 : 1;
    int32_t ta = ea->track_number, tb = eb->track_number;
    if (ta > 0 || tb > 0) {
        if (ta <= 0) return 1;
        if (tb <= 0) return -1;
        if (ta != tb) return ta < tb ? -1 : 1;
    }
    return strcasecmp(ea->path, eb->path);
}

/* Fetches every song credited to artist_albums_current_name (flattened
 * across all its albums) and hands it to show_group_songs_take_ownership()
 * -- the "All Songs" row prepended at index 0 by show_artist_albums().
 *
 * Exact track count is queried via metadata_db_get_group_offset() /
 * metadata_db_get_groups_page() so buffer sizing matches accurately. */
#define ARTIST_ALBUMS_ALL_SONGS_CAP 4000

static bool artist_albums_show_all_songs_internal(bool push_screen, const char * artist_name,
                                                    metadata_db_group_kind_t artist_kind) {
    int64_t offset = metadata_db_get_group_offset(artist_kind, artist_name, NULL);
    group_row_t artist_row;
    int real_total = (offset >= 0 && metadata_db_get_groups_page(artist_kind, (int) offset, 1,
                                                                  &artist_row) == 1)
                          ? artist_row.song_count
                          : 0;
    if (real_total <= 0) return false;
    int total = real_total > ARTIST_ALBUMS_ALL_SONGS_CAP ? ARTIST_ALBUMS_ALL_SONGS_CAP : real_total;

    /* calloc, not malloc -- a failure partway through the fetch loop below
     * frees every one of these `total` slots unconditionally (see that
     * branch's own comment), relying on the not-yet-reached ones staying
     * NULL from this zero-init rather than holding garbage. */
    artist_song_sort_entry_t * sort_entries = calloc((size_t) total, sizeof(*sort_entries));
    if (!sort_entries) return false;

    int n = 0;
    bool failed = false;
    song_row_t page[64];
    while (n < total) {
        int want = total - n;
        if (want > 64) want = 64;
        int got = artist_kind == METADATA_DB_GROUP_ALBUM_ARTIST
                      ? metadata_db_get_album_artist_songs(artist_name, n, page, want)
                      : metadata_db_get_artist_songs(artist_name, n, page, want);
        if (got <= 0) break;
        for (int i = 0; i < got; i++) {
            char title[192];
            format_music_submenu_identity(&page[i], title, sizeof(title));
            artist_song_sort_entry_t * dst = &sort_entries[n + i];
            dst->path = strdup(page[i].path);
            dst->title = strdup(title);
            dst->song_id = page[i].id;
            dst->artwork_key = song_album_thumbnail_key(&page[i]);
            snprintf(dst->album, sizeof(dst->album), "%s", page[i].tags.album);
            snprintf(dst->album_artist, sizeof(dst->album_artist), "%s", page[i].tags.album_artist);
            dst->disc_number = page[i].tags.disc_number;
            dst->track_number = page[i].tags.track_number;
            if (!dst->path || !dst->title) {
                failed = true;
                break;
            }
        }
        if (failed) break;
        n += got;
        if (got < want) break;
    }

    if (failed || n <= 0) {
        /* `total`, not `n` -- covers the current, partially-filled batch
         * (whatever this failure landed on) plus every never-reached slot,
         * all still NULL/zeroed from the calloc above. free(NULL) is a
         * no-op, so this is safe regardless of exactly how far the loop
         * got. */
        for (int i = 0; i < total; i++) {
            free(sort_entries[i].path);
            free(sort_entries[i].title);
        }
        free(sort_entries);
        return false;
    }

    /* Sort songs in disc and track order within each album across the artist's catalog. */
    qsort(sort_entries, (size_t) n, sizeof(*sort_entries), cmp_artist_song_sort_entry);

    /* Mark disc boundaries independently for each contiguous album identity.
     * Untagged tracks belong to disc 1, matching the single-album path. */
    for (int start = 0; start < n; ) {
        int end = start + 1;
        while (end < n && strcasecmp(sort_entries[end].album, sort_entries[start].album) == 0 &&
               strcasecmp(sort_entries[end].album_artist, sort_entries[start].album_artist) == 0)
            end++;
        bool multi_disc = false;
        int first_disc = sort_entries[start].disc_number > 0 ? sort_entries[start].disc_number : 1;
        for (int i = start + 1; i < end; i++) {
            int disc = sort_entries[i].disc_number > 0 ? sort_entries[i].disc_number : 1;
            if (disc != first_disc) { multi_disc = true; break; }
        }
        int previous_disc = first_disc;
        for (int i = start; i < end; i++) {
            int disc = sort_entries[i].disc_number > 0 ? sort_entries[i].disc_number : 1;
            sort_entries[i].show_disc_header = multi_disc && (i == start || disc != previous_disc);
            previous_disc = disc;
        }
        start = end;
    }

    /* Transfer ownership of path and title strings into a group_song_entry_t
     * array for show_group_songs_take_ownership() to avoid duplicate allocations. */
    group_song_entry_t * entries = malloc(sizeof(*entries) * (size_t) n);
    if (!entries) {
        for (int i = 0; i < n; i++) {
            free(sort_entries[i].path);
            free(sort_entries[i].title);
        }
        free(sort_entries);
        return false;
    }
    for (int i = 0; i < n; i++) {
        entries[i].path = sort_entries[i].path;
        entries[i].title = sort_entries[i].title;
        entries[i].song_id = sort_entries[i].song_id;
        entries[i].artwork_key = sort_entries[i].artwork_key;
        entries[i].disc_number = sort_entries[i].disc_number > 0 ? sort_entries[i].disc_number : 1;
        entries[i].show_disc_header = sort_entries[i].show_disc_header;
    }
    free(sort_entries);

    /* Disclose truncation via a toast if the songs exceed the display cap. */
    if (real_total > ARTIST_ALBUMS_ALL_SONGS_CAP) {
        char msg[96];
        snprintf(msg, sizeof(msg), "Showing first %d of %d songs", n, real_total);
        show_info_toast(msg);
    }

    group_songs_refresh_source = GROUP_SONGS_REFRESH_ARTIST;
    if (artist_name != group_songs_refresh_artist_name)
        snprintf(group_songs_refresh_artist_name, sizeof(group_songs_refresh_artist_name), "%s",
                 artist_name);
    group_songs_refresh_artist_kind = artist_kind;
    show_group_songs_take_ownership(artist_name, entries, n, push_screen);
    /* entries is now owned by group_songs_entries -- do not free here. */
    return true;
}

static bool artist_albums_show_all_songs(void) {
    return artist_albums_show_all_songs_internal(true, artist_albums_current_name,
                                                 artist_albums_current_kind);
}

static void artist_album_row_click_cb(int index) {
    if (library_rescan_active) return;
    if (index == 0) {
        (void) artist_albums_show_all_songs();
        return;
    }
    int group_index = index - 1;
    if (group_index < 0 || group_index >= artist_albums_group_count) return;
    group_row_t group;
    if (metadata_db_get_albums_for_group(artist_albums_current_kind, artist_albums_current_name,
                                         group_index, 1, &group) != 1) return;
    int n = 0;
    group_song_entry_t * entries = load_album_entries_filtered(group.name, group.album_artist,
                                                                group.song_count,
                                                                artist_albums_song_filter(), &n);
    if (!entries) return;
    show_music_group_songs(group.name, group.album_artist, artist_albums_song_filter(), entries, n);
    free_group_song_entries(entries, n);
}

static void hide_collection_menu(void) {
    if (collection_menu_popup) lv_obj_add_flag(collection_menu_popup, LV_OBJ_FLAG_HIDDEN);
    if (collection_menu_backdrop) lv_obj_add_flag(collection_menu_backdrop, LV_OBJ_FLAG_HIDDEN);
    if (album_collection_menu_popup) lv_obj_add_flag(album_collection_menu_popup, LV_OBJ_FLAG_HIDDEN);
    if (album_collection_menu_backdrop) lv_obj_add_flag(album_collection_menu_backdrop, LV_OBJ_FLAG_HIDDEN);
}

static void collection_menu_backdrop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) hide_collection_menu();
}

static void show_collection_menu(bool album) {
    if (library_rescan_active) return;
    lv_obj_t * popup = album ? album_collection_menu_popup : collection_menu_popup;
    lv_obj_t * backdrop = album ? album_collection_menu_backdrop : collection_menu_backdrop;
    if (!popup || !backdrop) return;
    lv_obj_remove_flag(backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(popup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(backdrop);
    lv_obj_move_foreground(popup);
}

static bool play_current_group(play_mode_t mode) {
    if (group_songs_count <= 0) return false;
    char ** paths = calloc((size_t) group_songs_count, sizeof(*paths));
    if (!paths) return false;
    for (int i = 0; i < group_songs_count; i++) {
        paths[i] = strdup(group_songs_entries[i].path);
        if (!paths[i]) {
            for (int j = 0; j < group_songs_count; j++) free(paths[j]);
            free(paths);
            return false;
        }
    }
    int start = mode == PLAY_MODE_SHUFFLE ? rand() % group_songs_count : 0;
    gui_player_set_play_mode(mode);
    set_player_source_group_songs(start);
    on_file_selected(paths, group_songs_count, start);
    return true;
}

static bool load_collection_for_playback(void) {
    if (!collection_menu_is_album) {
        /* The artist menu now also opens from the Artists and Album Artists
         * lists, so play the artist the menu was opened for, not whichever
         * artist's album list was visited last. */
        return artist_albums_show_all_songs_internal(true, collection_menu_name,
                                                     collection_menu_artist_kind);
    }
    group_row_t group = {0};
    snprintf(group.name, sizeof(group.name), "%s", collection_menu_name);
    snprintf(group.album_artist, sizeof(group.album_artist), "%s", collection_menu_album_artist);
    group.song_count = collection_menu_song_count;
    return show_album_group_filtered(&group, collection_menu_artist[0] ? collection_menu_artist : NULL);
}

static void collection_play_shuffled_cb(lv_event_t * e) {
    if (library_rescan_active || lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_collection_menu();
    if (load_collection_for_playback()) play_current_group(PLAY_MODE_SHUFFLE);
}

static void collection_play_sequential_cb(lv_event_t * e) {
    if (library_rescan_active || lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_collection_menu();
    if (load_collection_for_playback()) play_current_group(PLAY_MODE_SEQUENTIAL);
}

static bool collection_song_at(int offset, song_row_t * song) {
    if (collection_menu_is_album) {
        /* collection_menu_song_count is the filtered total when the menu was
         * opened from an Artists drill-down, so the rows must be filtered the
         * same way or the offset indexes a different list. */
        if (collection_menu_artist[0])
            return metadata_db_get_songs_filtered_page(NULL, collection_menu_artist,
                                                       collection_menu_album_artist,
                                                       collection_menu_name, NULL, offset, 1, song) == 1;
        return metadata_db_get_album_songs(collection_menu_name, collection_menu_album_artist,
                                            offset, song, 1) == 1;
    }
    return (collection_menu_artist_kind == METADATA_DB_GROUP_ALBUM_ARTIST
                ? metadata_db_get_album_artist_songs(collection_menu_name, offset, song, 1)
                : metadata_db_get_artist_songs(collection_menu_name, offset, song, 1)) == 1;
}

static void collection_add_random_cb(lv_event_t * e) {
    if (library_rescan_active || lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_collection_menu();
    if (collection_menu_song_count <= 0) return;
    song_row_t song;
    if (collection_song_at(rand() % collection_menu_song_count, &song))
        gui_player_queue_add(song.path);
}

static void collection_add_album_cb(lv_event_t * e) {
    if (library_rescan_active || lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_collection_menu();
    int count = 0;
    group_song_entry_t * entries = load_album_entries_filtered(collection_menu_name,
                                                                collection_menu_album_artist,
                                                                collection_menu_song_count,
                                                                collection_menu_artist[0] ? collection_menu_artist : NULL,
                                                                &count);
    if (!entries) return;
    const char ** paths = malloc(sizeof(*paths) * (size_t) count);
    if (paths) {
        for (int i = 0; i < count; i++) paths[i] = entries[i].path;
        gui_player_queue_add_many(paths, count);
        free(paths);
    }
    free_group_song_entries(entries, count);
}

static void collection_refresh_metadata_cb(lv_event_t * e) {
    if (library_rescan_active || lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_collection_menu();
    if (collection_menu_is_album)
        start_library_metadata_refresh_album(collection_menu_name, collection_menu_album_artist);
    else
        start_library_metadata_refresh_artist(collection_menu_artist_kind, collection_menu_name);
}

static void collection_reload_cover_cb(lv_event_t * e) {
    if (library_rescan_active || lv_event_get_code(e) != LV_EVENT_CLICKED || !collection_menu_is_album)
        return;
    hide_collection_menu();
    start_library_cover_reload_album(collection_menu_name, collection_menu_album_artist);
}

static void open_album_collection_menu(const group_row_t * group, const char * artist_filter) {
    collection_menu_is_album = true;
    snprintf(collection_menu_artist, sizeof(collection_menu_artist), "%s", artist_filter ? artist_filter : "");
    /* Match the row's own listing, so "add album" never queues tracks the
     * screen does not show. */
    if (collection_menu_artist[0]) {
        int64_t filtered = metadata_db_count_songs_filtered(NULL, collection_menu_artist,
                                                            group->album_artist, group->name, NULL);
        collection_menu_song_count = filtered > 0 && filtered <= INT_MAX ? (int) filtered : 0;
    } else {
        collection_menu_song_count = group->song_count;
    }
    snprintf(collection_menu_name, sizeof(collection_menu_name), "%s", group->name);
    snprintf(collection_menu_album_artist, sizeof(collection_menu_album_artist), "%s", group->album_artist);
    show_collection_menu(true);
}

static void open_artist_collection_menu(const group_row_t * group, metadata_db_group_kind_t kind) {
    collection_menu_is_album = false;
    collection_menu_artist_kind = kind;
    collection_menu_song_count = group->song_count;
    snprintf(collection_menu_name, sizeof(collection_menu_name), "%s", group->name);
    collection_menu_album_artist[0] = '\0';
    collection_menu_artist[0] = '\0';
    show_collection_menu(false);
}

static void open_artist_collection_menu_at(metadata_db_group_kind_t kind,
                                           search_binding_id_t binding_id, int index) {
    index = search_remap_index(binding_id, index);
    group_row_t group;
    if (metadata_db_get_groups_page(kind, index, 1, &group) == 1)
        open_artist_collection_menu(&group, kind);
}

static void artist_collection_more_click_cb(int index) {
    if (library_rescan_active) return;
    open_artist_collection_menu_at(METADATA_DB_GROUP_ARTIST, SEARCH_BINDING_ARTISTS, index);
}

static void artist_collection_long_press_cb(int index) {
    if (library_rescan_active) return;
    open_artist_collection_menu_at(METADATA_DB_GROUP_ARTIST, SEARCH_BINDING_ARTISTS, index);
}

static void album_artist_collection_more_click_cb(int index) {
    if (library_rescan_active) return;
    open_artist_collection_menu_at(METADATA_DB_GROUP_ALBUM_ARTIST, SEARCH_BINDING_ALBUM_ARTIST, index);
}

static void album_artist_collection_long_press_cb(int index) {
    if (library_rescan_active) return;
    open_artist_collection_menu_at(METADATA_DB_GROUP_ALBUM_ARTIST, SEARCH_BINDING_ALBUM_ARTIST, index);
}

static void open_album_collection_menu_at(int index) {
    index = search_remap_index(SEARCH_BINDING_ALBUMS, index);
    group_row_t group;
    if (metadata_db_get_albums_page_filtered(NULL, index, 1, &group) == 1)
        open_album_collection_menu(&group, NULL);
}

static void album_more_click_cb(int index) {
    if (library_rescan_active) return;
    open_album_collection_menu_at(index);
}

static void album_row_long_press_cb(int index) {
    if (library_rescan_active) return;
    open_album_collection_menu_at(index);
}

static void artist_album_more_click_cb(int index) {
    if (library_rescan_active) return;
    if (index > 0) {
        int group_index = index - 1;
        group_row_t group;
        if (group_index >= 0 && group_index < artist_albums_group_count &&
            metadata_db_get_albums_for_group(artist_albums_current_kind, artist_albums_current_name,
                                              group_index, 1, &group) == 1)
            open_album_collection_menu(&group, artist_albums_song_filter());
        return;
    }
    int64_t offset = metadata_db_get_group_offset(artist_albums_current_kind,
                                                   artist_albums_current_name, NULL);
    group_row_t group;
    if (offset < 0 || metadata_db_get_groups_page(artist_albums_current_kind,
                                                   (int) offset, 1, &group) != 1) return;
    open_artist_collection_menu(&group, artist_albums_current_kind);
}

static void build_collection_menus(void) {
    const menu_popup_row_t rows[] = {
        { "Play all shuffled", collection_play_shuffled_cb, false },
        { "Play sequentially", collection_play_sequential_cb, false },
        { "Add a random song to queue", collection_add_random_cb, false },
        { "Refresh metadata", collection_refresh_metadata_cb, false },
    };
    const menu_popup_row_t album_rows[] = {
        { "Play all shuffled", collection_play_shuffled_cb, false },
        { "Play sequentially", collection_play_sequential_cb, false },
        { "Add a random song to queue", collection_add_random_cb, false },
        { "Add album to queue", collection_add_album_cb, false },
        { "Reload cover", collection_reload_cover_cb, false },
        { "Refresh metadata", collection_refresh_metadata_cb, false },
    };
    collection_menu_popup = build_menu_popup(rows, (int) (sizeof(rows) / sizeof(rows[0])),
                                               collection_menu_backdrop_cb,
                                               &collection_menu_backdrop);
    album_collection_menu_popup = build_menu_popup(album_rows,
                                                     (int) (sizeof(album_rows) / sizeof(album_rows[0])),
                                                     collection_menu_backdrop_cb,
                                                     &album_collection_menu_backdrop);
}

/* A direct lv_image_dsc_t points into the bounded LRU's pixel allocation.
 * Refresh on every screen load so a list that was hidden while the other
 * album screen populated/evicted cache entries never redraws an old row
 * against an evicted descriptor. This also immediately picks up artwork
 * decoded while the list was off-screen. */
static void album_thumbnail_screen_loaded_cb(lv_event_t * e) {
    lv_obj_t * list = (lv_obj_t *) lv_event_get_user_data(e);
    thumbnail_begin_screen(list);
}

static void album_thumbnail_screen_unloaded_cb(lv_event_t * e) {
    lv_obj_t * list = (lv_obj_t *) lv_event_get_user_data(e);
    thumbnail_end_screen(list);
}

/* Resolve the matching album's sorted result offset directly. This keeps the
 * now-playing refresh bounded even for an artist with a large catalog, while
 * preserving the database's canonical/truncated group names and ordering.
 * Same standalone-refresh reasoning as refresh_group_songs_now_playing_
 * indicator() -- callable without rebuilding rows, e.g. on a live playback
 * change while this screen stays open. */
void refresh_artist_albums_now_playing_indicator(void) {
    if (!artist_albums_list) return;

    int match = -1;
    song_row_t playing;
    if (now_playing_path[0] && metadata_db_get_song_by_path(now_playing_path, &playing)) {
        int64_t offset = metadata_db_get_album_for_group_offset(artist_albums_current_kind,
                                                                 artist_albums_current_name,
                                                                 playing.tags.album,
                                                                 playing.tags.album_artist);
        if (offset >= 0 && offset < artist_albums_group_count)
            /* +1 -- show_artist_albums() prepends an "All Songs" row at
             * index 0, ahead of every fetched album row. */
            match = (int) offset + 1;
    }

    compact_list_set_now_playing(artist_albums_list, match);
}

/* Paged artist-album rows keep the complete result set out of memory. Row
 * zero is the synthetic All Songs action; database offset zero starts at
 * display index one. */
static int artist_albums_fetch_page(void * ctx, int offset, int count, compact_list_page_row_t out_rows[]) {
    (void) ctx;
    char current_name[sizeof(artist_albums_current_name)];
    metadata_db_group_kind_t current_kind;
    pthread_mutex_lock(&artist_albums_state_mutex);
    snprintf(current_name, sizeof(current_name), "%s", artist_albums_current_name);
    current_kind = artist_albums_current_kind;
    pthread_mutex_unlock(&artist_albums_state_mutex);

    int prefix = 0;
    if (offset == 0 && count > 0) {
        memset(&out_rows[0], 0, sizeof(out_rows[0]));
        snprintf(out_rows[0].label, sizeof(out_rows[0].label), "%s", "All Songs");
        snprintf(out_rows[0].trailing_asset, sizeof(out_rows[0].trailing_asset), "%s", "playing_plane/ic_more.png");
        prefix = 1;
        count--;
    } else {
        offset--;
    }
    if (count <= 0) return prefix;
    group_row_t * rows = malloc(sizeof(*rows) * (size_t) count);
    int n = rows ? metadata_db_get_albums_for_group(current_kind, current_name, offset, count, rows) : 0;
    for (int i = 0; i < n; i++) {
        snprintf(out_rows[prefix + i].label, sizeof(out_rows[prefix + i].label), "%s", rows[i].name);
        out_rows[prefix + i].identity = rows[i].first_song_id;
        if (rows[i].album_artist[0]) {
            albumart_info_t info = {0};
            snprintf(info.albumartist, sizeof(info.albumartist), "%s", rows[i].album_artist);
            snprintf(info.album, sizeof(info.album), "%s", rows[i].name);
            out_rows[prefix + i].artwork_key = albumart_thumbnail_key(&info);
        } else {
            song_row_t representative;
            if (metadata_db_get_song_by_id(rows[i].first_song_id, &representative))
                out_rows[prefix + i].artwork_key = song_album_thumbnail_key(&representative);
        }
        snprintf(out_rows[prefix + i].trailing_asset, sizeof(out_rows[prefix + i].trailing_asset),
                 "%s", "playing_plane/ic_more.png");
    }
    free(rows);
    return n + prefix;
}

static void populate_artist_albums_view(const char * name, metadata_db_group_kind_t kind) {
    pthread_mutex_lock(&artist_albums_state_mutex);
    snprintf(artist_albums_current_name, sizeof(artist_albums_current_name), "%s", name);
    artist_albums_current_kind = kind;
    pthread_mutex_unlock(&artist_albums_state_mutex);

    int64_t count64 = metadata_db_count_albums_for_group(kind, name);
    artist_albums_group_count = count64 > 0 && count64 <= INT_MAX ? (int) count64 : 0;

    lv_label_set_text(artist_albums_title_label, name);
    compact_list_set_paged_provider(artist_albums_list, artist_albums_fetch_page, NULL,
                                    artist_albums_group_count + 1);
    compact_list_set_row_decorator(artist_albums_list, thumbnail_row_decorator,
                                    (void *) &album_thumbnail_context);
    compact_list_set_trailing_click(artist_albums_list, artist_album_more_click_cb);
    refresh_artist_albums_now_playing_indicator();
}

void show_artist_albums(const char * name, metadata_db_group_kind_t kind) {
    populate_artist_albums_view(name, kind);
    nav_push(artist_albums_screen);
}

static bool library_screen_in_navigation(lv_obj_t * screen) {
    if (!screen || lv_screen_active() == screen) return screen != NULL;
    int depth = gui_navigation_get_depth();
    for (int i = 0; i < depth; i++)
        if (gui_navigation_get_screen_at(i) == screen) return true;
    return false;
}

static void refresh_group_songs_paths_in_place(void) {
    group_song_entry_t * entries = calloc((size_t)(group_songs_count > 0 ? group_songs_count : 1),
                                          sizeof(*entries));
    if (!entries) {
        set_group_songs_entries_owned(NULL, 0);
        populate_group_songs_rows();
        return;
    }
    int count = 0;
    for (int i = 0; i < group_songs_count; i++) {
        song_row_t song;
        if (!metadata_db_get_song_by_path(group_songs_entries[i].path, &song)) continue;
        char title[384];
        if (group_songs_music_submenu) format_music_submenu_identity(&song, title, sizeof(title));
        else format_song_identity(&song, title, sizeof(title));
        entries[count].path = strdup(song.path);
        entries[count].title = strdup(title);
        entries[count].song_id = song.id;
        entries[count].artwork_key = song_album_thumbnail_key(&song);
        entries[count].disc_number = group_songs_entries[i].disc_number;
        entries[count].show_disc_header = group_songs_entries[i].show_disc_header;
        if (!entries[count].path || !entries[count].title) {
            free_group_song_entries(entries, count + 1);
            set_group_songs_entries_owned(NULL, 0);
            populate_group_songs_rows();
            return;
        }
        count++;
    }
    set_group_songs_entries_owned(entries, count);
    populate_group_songs_rows();
}

static void refresh_group_songs_from_database(void) {
    group_songs_page_start = 0;
    switch (group_songs_refresh_source) {
        case GROUP_SONGS_REFRESH_FAVORITES: {
            char ** paths = NULL;
            int count = 0;
            metadata_db_load_favorite_songs(&paths, &count);
            group_song_entry_t * entries = build_group_song_entries_from_paths(paths, count);
            set_group_songs_entries(entries, entries ? count : 0);
            free_group_song_entries(entries, count);
            for (int i = 0; i < count; i++) free(paths[i]);
            free(paths);
            populate_group_songs_rows();
            break;
        }
        case GROUP_SONGS_REFRESH_MOST_PLAYED: {
            char ** paths = NULL;
            int count = 0;
            metadata_db_load_top_played_songs(MOST_PLAYED_LIMIT, &paths, &count);
            group_song_entry_t * entries = build_group_song_entries_from_paths(paths, count);
            set_group_songs_entries(entries, entries ? count : 0);
            free_group_song_entries(entries, count);
            for (int i = 0; i < count; i++) free(paths[i]);
            free(paths);
            populate_group_songs_rows();
            break;
        }
        case GROUP_SONGS_REFRESH_M3U: {
            char ** paths = NULL;
            int count = 0;
            bool loaded = group_songs_refresh_m3u_path[0] &&
                          playlist_files_read(group_songs_refresh_m3u_path, &paths, &count);
            group_song_entry_t * entries = loaded && count ? build_group_song_entries_from_paths(paths, count) : NULL;
            set_group_songs_entries(entries, entries ? count : 0);
            free_group_song_entries(entries, count);
            for (int i = 0; i < count; i++) free(paths[i]);
            free(paths);
            group_songs_file_stat_valid = loaded &&
                stat(group_songs_refresh_m3u_path, &group_songs_file_stat) == 0;
            populate_group_songs_rows();
            break;
        }
        case GROUP_SONGS_REFRESH_ALBUM: {
            int64_t count64 = 0;
            if (group_songs_refresh_artist_filter[0]) {
                count64 = metadata_db_count_songs_filtered(NULL, group_songs_refresh_artist_filter,
                    group_songs_refresh_album_artist, group_songs_refresh_album, NULL);
            } else {
                int64_t offset = metadata_db_get_group_offset(METADATA_DB_GROUP_ALBUM,
                    group_songs_refresh_album, group_songs_refresh_album_artist);
                group_row_t group;
                if (offset >= 0 && metadata_db_get_groups_page(METADATA_DB_GROUP_ALBUM,
                        (int) offset, 1, &group) == 1) count64 = group.song_count;
            }
            int count = count64 > 0 && count64 <= INT_MAX ? (int) count64 : 0;
            int loaded_count = 0;
            group_song_entry_t * entries = load_album_entries_filtered(group_songs_refresh_album,
                group_songs_refresh_album_artist, count, group_songs_refresh_artist_filter, &loaded_count);
            set_group_songs_entries(entries, entries ? loaded_count : 0);
            free_group_song_entries(entries, loaded_count);
            populate_group_songs_rows();
            break;
        }
        case GROUP_SONGS_REFRESH_ARTIST:
            if (!artist_albums_show_all_songs_internal(false, group_songs_refresh_artist_name,
                                                       group_songs_refresh_artist_kind)) {
                set_group_songs_entries(NULL, 0);
                populate_group_songs_rows();
            }
            break;
        case GROUP_SONGS_REFRESH_NONE:
        default:
            refresh_group_songs_paths_in_place();
            break;
    }
}

static void refresh_targeted_nested_views(void) {
    hide_collection_menu();
    if (library_screen_in_navigation(artist_albums_screen)) {
        char name[sizeof(artist_albums_current_name)];
        metadata_db_group_kind_t kind;
        pthread_mutex_lock(&artist_albums_state_mutex);
        snprintf(name, sizeof(name), "%s", artist_albums_current_name);
        kind = artist_albums_current_kind;
        pthread_mutex_unlock(&artist_albums_state_mutex);
        if (name[0]) populate_artist_albums_view(name, kind);
    }
    if (library_screen_in_navigation(group_songs_screen)) refresh_group_songs_from_database();
}

/* Paged Artists/Album Artist -- see build_artists_screen()'s own comment.
 * offset is a position in the DB's own name-sorted order (ORDER BY artist/
 * album_artist COLLATE NOCASE) -- a row tapped here (artist_row_click_cb()/
 * album_artist_row_click_cb() below) re-resolves that same offset via
 * metadata_db_get_groups_page() itself to get the tapped name, rather than
 * caching anything from this page fetch. */
static int artists_fetch_page(void * ctx, int offset, int count, compact_list_page_row_t out_rows[]) {
    (void) ctx;
    group_row_t * rows = malloc(sizeof(group_row_t) * (size_t) count);
    int n = rows ? metadata_db_get_groups_page(METADATA_DB_GROUP_ARTIST, offset, count, rows) : 0;
    for (int i = 0; i < n; i++) {
        snprintf(out_rows[i].label, sizeof(out_rows[i].label), "%s", rows[i].name);
        out_rows[i].identity = rows[i].first_song_id;
        out_rows[i].artwork_key = albumart_artist_thumbnail_key(rows[i].name);
        snprintf(out_rows[i].artwork_name, sizeof(out_rows[i].artwork_name), "%s", rows[i].name);
        snprintf(out_rows[i].trailing_asset, sizeof(out_rows[i].trailing_asset),
                 "%s", "playing_plane/ic_more.png");
    }
    free(rows);
    return n;
}

static int album_artists_fetch_page(void * ctx, int offset, int count, compact_list_page_row_t out_rows[]) {
    (void) ctx;
    group_row_t * rows = malloc(sizeof(group_row_t) * (size_t) count);
    int n = rows ? metadata_db_get_groups_page(METADATA_DB_GROUP_ALBUM_ARTIST, offset, count, rows) : 0;
    for (int i = 0; i < n; i++) {
        snprintf(out_rows[i].label, sizeof(out_rows[i].label), "%s", rows[i].name);
        out_rows[i].identity = rows[i].first_song_id;
        out_rows[i].artwork_key = albumart_artist_thumbnail_key(rows[i].name);
        snprintf(out_rows[i].artwork_name, sizeof(out_rows[i].artwork_name), "%s", rows[i].name);
        snprintf(out_rows[i].trailing_asset, sizeof(out_rows[i].trailing_asset),
                 "%s", "playing_plane/ic_more.png");
    }
    free(rows);
    return n;
}



/* Diagnostic logging helper for teardown and initialization steps. */
static void library_teardown_diag(const char * step);

void gui_library_init(void) {
    library_teardown_diag("az_index_drag_timer before");
    if (!az_index_drag_timer) az_index_drag_timer = lv_timer_create(poll_az_index_drag, LV_DEF_REFR_PERIOD, NULL);
    library_teardown_diag("build_files_screen before");
    files_screen = build_files_screen();
    library_teardown_diag("build_all_songs_screen before");
    all_songs_screen = build_all_songs_screen();
    library_teardown_diag("build_recently_added_screen before");
    recently_added_screen = build_recently_added_screen();
    library_teardown_diag("build_artists_screen before");
    artists_screen = build_artists_screen();
    library_teardown_diag("build_albums_screen before");
    albums_screen = build_albums_screen();
    library_teardown_diag("build_album_artist_screen before");
    album_artist_screen = build_album_artist_screen();
    library_teardown_diag("build_playlists_screen before");
    playlists_screen = build_playlists_screen();
    build_playlist_context_menu_popup();
    library_teardown_diag("build_cue_tracks_screen before");
    cue_tracks_screen = build_cue_tracks_screen();
    library_teardown_diag("build_group_songs_screen before");
    group_songs_screen = build_group_songs_screen();

    library_teardown_diag("build_compact_list_screen(Albums) before");
    artist_albums_screen = build_compact_list_screen("Albums", generic_back_cb, NULL, 0,
                                                      artist_album_row_click_cb, NULL,
                                                      &artist_albums_list, &artist_albums_title_label,
                                                      LIST_ROW_WIDTH_WIDE, true, accent_lv_color());
    library_teardown_diag("compact_list_set_row_height before");
    compact_list_set_row_height(artist_albums_list, MUSIC_LIST_ROW_HEIGHT);
    compact_list_set_row_decorator(artist_albums_list, thumbnail_row_decorator,
                                    (void *) &album_thumbnail_context);
    library_teardown_diag("artist_albums event cbs before");
    lv_obj_add_event_cb(artist_albums_screen, album_thumbnail_screen_loaded_cb,
                        LV_EVENT_SCREEN_LOADED, artist_albums_list);
    lv_obj_add_event_cb(artist_albums_screen, album_thumbnail_screen_unloaded_cb,
                        LV_EVENT_SCREEN_UNLOADED, artist_albums_list);
    lv_obj_add_event_cb(artist_albums_list, album_thumbnail_scroll_cb, LV_EVENT_SCROLL_BEGIN, NULL);
    lv_obj_add_event_cb(artist_albums_list, album_thumbnail_scroll_cb, LV_EVENT_SCROLL_END, NULL);
    library_teardown_diag("finalize_screen_navigation(artist_albums) before");
    finalize_screen_navigation(artist_albums_screen);
    library_teardown_diag("build_add_to_playlist_screen before");
    add_to_playlist_screen = build_add_to_playlist_screen();
    library_teardown_diag("build_music_screen before");
    music_screen = build_music_screen();

    /* Register A-Z index & search */
    library_teardown_diag("register_az_index(artists) before");
    register_az_index(artists_screen, artists_list, METADATA_DB_AZ_ARTIST);
    library_teardown_diag("register_az_index(albums) before");
    register_az_index(albums_screen, albums_list, METADATA_DB_AZ_ALBUM);
    library_teardown_diag("register_az_index(album_artist) before");
    register_az_index(album_artist_screen, album_artist_list, METADATA_DB_AZ_ALBUM_ARTIST);
    library_teardown_diag("register_az_index(all_songs) before");
    register_az_index(all_songs_screen, all_songs_list, METADATA_DB_AZ_ALL_SONGS);

    library_teardown_diag("register_search(artists) before");
    register_search(SEARCH_BINDING_ARTISTS, artists_screen, artists_list, NULL, NULL, false,
                     true, METADATA_DB_AZ_ARTIST, artists_fetch_page);
    library_teardown_diag("register_search(albums) before");
    register_search(SEARCH_BINDING_ALBUMS, albums_screen, albums_list, NULL, NULL, false,
                     true, METADATA_DB_AZ_ALBUM, albums_fetch_page);
    library_teardown_diag("register_search(album_artist) before");
    register_search(SEARCH_BINDING_ALBUM_ARTIST, album_artist_screen, album_artist_list, NULL,
                     NULL, false, true, METADATA_DB_AZ_ALBUM_ARTIST, album_artists_fetch_page);
    library_teardown_diag("register_search(all_songs) before");
    register_search(SEARCH_BINDING_ALL_SONGS, all_songs_screen, all_songs_list, NULL, NULL, false,
                     true, METADATA_DB_AZ_ALL_SONGS, all_songs_fetch_page);

    library_teardown_diag("build_compact_list_widget(files_search) before");
    files_search_list = build_compact_list_widget(files_screen, NULL, 0, files_search_row_click_cb, NULL, LIST_ROW_WIDTH_WIDE, false, lv_color_black());
    lv_obj_set_style_bg_opa(files_search_list, LV_OPA_COVER, 0);
    lv_obj_add_style(files_search_list, &style_theme_screen_bg, 0);
    lv_obj_add_flag(files_search_list, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(files_search_list, LV_OBJ_FLAG_GESTURE_BUBBLE);
    compact_list_set_row_decorator(files_search_list, thumbnail_row_decorator,
                                    (void *) &album_thumbnail_context);
    lv_obj_add_event_cb(files_search_list, album_thumbnail_scroll_cb, LV_EVENT_SCROLL_BEGIN, NULL);
    lv_obj_add_event_cb(files_search_list, album_thumbnail_scroll_cb, LV_EVENT_SCROLL_END, NULL);
    library_teardown_diag("enable_gesture_bubble_recursive(files_search_list) before");
    enable_gesture_bubble_recursive(files_search_list);

    library_teardown_diag("register_search(files) before");
    register_search(SEARCH_BINDING_FILES, files_screen, files_search_list, NULL, NULL, true,
                     true, METADATA_DB_AZ_ALL_SONGS, all_songs_fetch_page);

    library_teardown_diag("build_sd_mount_failed_popup before");
    build_sd_mount_failed_popup();
    library_teardown_diag("build_sd_format_confirm_popup before");
    build_sd_format_confirm_popup();
    library_teardown_diag("build_collection_menus before");
    build_collection_menus();
    library_teardown_diag("build_library_update_prompt_popup before");
    build_library_update_prompt_popup();
    library_teardown_diag("gui_library_init done");
}

/* For gui_reload.c's in-process UI reload -- deletes every screen this
 * module owns so gui_library_init() can rebuild them from a clean slate
 * without leaking the old objects. Deliberately does NOT touch
 * az_index_drag_timer (already guarded/reused correctly by gui_library_
 * init() itself) or the search_bindings[]/az-index registries -- register_
 * search()/register_az_index() already free and reassign their own prior
 * state on re-registration. sd_mount_failed_popup/sd_format_confirm_popup
 * and their backdrops are built directly on lv_layer_top() (see
 * build_confirm_popup()'s own comment), not as children of any of these
 * screens, so they need their own explicit deletion. */
/* Diagnostic logging helper writing teardown and init steps to reload_diag.log
 * with fsync per line to pinpoint failures during theme reload. */
static void library_teardown_diag(const char * step) {
    if (!db_log_enabled()) return;
    (void) mkdir("/data/mnt/sd_0/.logs", 0755);
    int fd = open("/data/mnt/sd_0/.logs/reload_diag.log", O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0) return;
    char line[96];
    int len = snprintf(line, sizeof(line), "[pid=%ld]   gui_library_teardown: %s\n", (long) getpid(), step);
    if (len > 0) {
        if (len >= (int) sizeof(line)) len = (int) sizeof(line) - 1;
        write(fd, line, (size_t) len);
        fsync(fd);
    }
    close(fd);
}

void gui_library_teardown(void) {
    cancel_group_song_probes();
    reset_az_index_bindings();
    library_prompt_requeue_visible();
    gui_popup_teardown(&library_update_prompt_popup);
    gui_popup_teardown(&refresh_all_metadata_popup);
    gui_popup_teardown(&refresh_all_covers_popup);
    library_update_prompt_title = NULL;
    library_update_prompt_body = NULL;
    library_update_prompt_confirm_label = NULL;
    if (playlist_start_popup) { lv_obj_delete(playlist_start_popup); playlist_start_popup = NULL; }
    if (playlist_start_backdrop) { lv_obj_delete(playlist_start_backdrop); playlist_start_backdrop = NULL; }
    gui_popup_teardown(&playlist_delete_popup);
    if (playlist_context_menu_popup) { lv_obj_delete(playlist_context_menu_popup); playlist_context_menu_popup = NULL; }
    if (playlist_context_menu_backdrop) { lv_obj_delete(playlist_context_menu_backdrop); playlist_context_menu_backdrop = NULL; }
    library_teardown_diag("power_action_overlay before");
    gui_popup_teardown(&power_action_overlay);
    power_action_blur_img = NULL; /* child of backdrop -- already deleted by the teardown above */
    free(power_action_blur_bytes);
    power_action_blur_bytes = NULL;
    library_teardown_diag("sd_mount_failed_popup before");
    gui_popup_teardown(&sd_mount_failed_popup);
    library_teardown_diag("sd_format_confirm_popup before");
    gui_popup_teardown(&sd_format_confirm_popup);
    library_teardown_diag("collection_menu_popup before");
    if (collection_menu_popup) { lv_obj_delete(collection_menu_popup); collection_menu_popup = NULL; }
    library_teardown_diag("collection_menu_backdrop before");
    if (collection_menu_backdrop) { lv_obj_delete(collection_menu_backdrop); collection_menu_backdrop = NULL; }
    library_teardown_diag("album_collection_menu_popup before");
    if (album_collection_menu_popup) { lv_obj_delete(album_collection_menu_popup); album_collection_menu_popup = NULL; }
    library_teardown_diag("album_collection_menu_backdrop before");
    if (album_collection_menu_backdrop) {
        lv_obj_delete(album_collection_menu_backdrop);
        album_collection_menu_backdrop = NULL;
    }
    library_teardown_diag("music_screen before");
    if (music_screen) { lv_obj_delete(music_screen); music_screen = NULL; }
    library_teardown_diag("files_screen before");
    if (files_screen) { lv_obj_delete(files_screen); files_screen = NULL; }
    library_teardown_diag("all_songs_screen before");
    if (all_songs_screen) { lv_obj_delete(all_songs_screen); all_songs_screen = NULL; }
    library_teardown_diag("recently_added_screen before");
    if (recently_added_screen) { lv_obj_delete(recently_added_screen); recently_added_screen = NULL; }
    library_teardown_diag("artists_screen before");
    if (artists_screen) { lv_obj_delete(artists_screen); artists_screen = NULL; }
    library_teardown_diag("albums_screen before");
    if (albums_screen) { lv_obj_delete(albums_screen); albums_screen = NULL; }
    library_teardown_diag("album_artist_screen before");
    if (album_artist_screen) { lv_obj_delete(album_artist_screen); album_artist_screen = NULL; }
    library_teardown_diag("group_songs_screen before");
    if (group_songs_screen) { lv_obj_delete(group_songs_screen); group_songs_screen = NULL; }
    library_teardown_diag("artist_albums_screen before");
    if (artist_albums_screen) { lv_obj_delete(artist_albums_screen); artist_albums_screen = NULL; }
    library_teardown_diag("playlists_screen before");
    if (playlists_screen) { lv_obj_delete(playlists_screen); playlists_screen = NULL; }
    playlists_refresh_icon = NULL;
    library_teardown_diag("cue_tracks_screen before");
    if (cue_tracks_screen) { lv_obj_delete(cue_tracks_screen); cue_tracks_screen = NULL; }
    library_teardown_diag("add_to_playlist_screen before");
    if (add_to_playlist_screen) { lv_obj_delete(add_to_playlist_screen); add_to_playlist_screen = NULL; }
    /* The screens owned these children; clear the borrowed pointers with
     * their parents so non-NULL remains a valid liveness check. */
    files_search_list = NULL;
    all_songs_list = NULL;
    recently_added_list = NULL;
    artists_list = NULL;
    albums_list = NULL;
    album_artist_list = NULL;
    group_songs_list = NULL;
    /* Borrowed children of the deleted group songs list; a later thumbnail
     * refresh after the UI rebuild must not reach them. */
    memset(group_songs_visible_rows, 0, sizeof(group_songs_visible_rows));
    memset(group_songs_thumbnail_images, 0, sizeof(group_songs_thumbnail_images));
    group_songs_title_label = NULL;
    group_songs_edit_btn = NULL;
    group_songs_now_playing_bar = NULL;
    artist_albums_list = NULL;
    artist_albums_title_label = NULL;
    playlists_list = NULL;
    cue_tracks_list = NULL;
    cue_tracks_title_label = NULL;
    add_to_playlist_list = NULL;
    album_thumbnail_active_list = NULL;
    /* build_power_off_countdown_popup() -- called separately from gui_init()
     * (not from gui_library_init()), but its lv_layer_top() objects are
     * still this file's own statics to own and delete. Left out, a visible
     * (if currently hidden) old overlay would linger above the rebuilt UI
     * forever, invisible only until the next power-button long-press
     * actually shows it. */
    library_teardown_diag("power_action_overlay before");
    gui_popup_teardown(&power_action_overlay);
    power_action_blur_img = NULL; /* child of backdrop -- already deleted by the teardown above */
    free(power_action_blur_bytes);
    power_action_blur_bytes = NULL;
    library_teardown_diag("done");
}

void gui_library_resume_fast_timers(void) {
    if (az_index_drag_timer && lv_timer_get_paused(az_index_drag_timer)) {
        lv_timer_resume(az_index_drag_timer);
        lv_timer_ready(az_index_drag_timer);
    }
}

void gui_library_reset_drag_state(void) {
    if (az_index_dragging && az_index_active_binding) {
        lv_obj_add_flag(az_index_active_binding->popup, LV_OBJ_FLAG_HIDDEN);
        az_index_active_binding = NULL;
    }
    az_index_dragging = false;
    /* Safe to clear even mid-press: this pauses the timer below, and it is
     * only ever resumed on a raw press-DOWN edge (gui_shell.c's indev
     * hooks), so the remainder of an in-flight press cannot be re-judged at
     * a drifted position. Leaving it set would instead disable the strip
     * for the whole of the NEXT press. */
    az_index_press_judged = false;
    if (az_index_drag_timer) {
        lv_timer_pause(az_index_drag_timer);
    }
}

typedef struct {
    char root[600];
    int root_fd;
    int spool_fd;
    atomic_int refs;
    atomic_bool cancel;
    atomic_bool done;
    bool ok;
    int count;
    int skipped; /* entries the walk could not read and stepped over */
    atomic_int progress;
} scan_walk_work_t;

static void scan_walk_release(scan_walk_work_t * w) {
    if (atomic_fetch_sub_explicit(&w->refs, 1, memory_order_acq_rel) == 1) {
        close(w->root_fd);
        close(w->spool_fd);
        free(w);
    }
}

typedef struct {
    FILE *spool;
    int root_fd;
    atomic_bool *cancel;
} scan_spool_context_t;

void gui_library_cancel_scan(void) {
    atomic_store_explicit(&library_scan_cancel_requested, true, memory_order_release);
    if (library_rescan_mode == LIBRARY_RESCAN_REFRESH_COVERS_ALL ||
        library_rescan_mode == LIBRARY_RESCAN_REFRESH_COVERS_ALBUM)
        cancel_album_thumbnail_generation();
}

/* Binary length-prefixed spool: paths can contain whitespace and, unlike a
 * newline-delimited temporary file, this remains correct even for odd names.
 * The spool lives on the SD card, so discovery storage scales with library
 * size without consuming the device's RAM. */
static bool scan_spool_visit_cb(const char * path, void * user) {
    scan_spool_context_t * ctx = user;
    FILE * f = ctx->spool;
    if (atomic_load_explicit(ctx->cancel, memory_order_acquire)) return false;
    char logical[PATH_MAX];
    char prefix[64];
    snprintf(prefix, sizeof(prefix), "/proc/self/fd/%d", ctx->root_fd);
    const char * p = path;
    if (strncmp(path, prefix, strlen(prefix)) == 0) {
        snprintf(logical, sizeof(logical), "%s%s", MUSIC_ROOT_DIR, path + strlen(prefix));
        p = logical;
    }
    size_t n = strlen(p);
    if (n > UINT32_MAX) return false;
    uint32_t len = (uint32_t) n;
    return fwrite(&len, sizeof(len), 1, f) == 1 && fwrite(p, 1, n, f) == n;
}


#define LIBRARY_SCAN_FILE_TIMEOUT_MS 5000
#define LIBRARY_SCAN_WALK_STALL_TIMEOUT_MS 30000

static void * scan_walk_worker(void * arg) {
    scan_walk_work_t * w = (scan_walk_work_t *) arg;
    int writer_fd = dup(w->spool_fd);
    FILE * spool = writer_fd >= 0 ? fdopen(writer_fd, "wb") : NULL;
    if (!spool) {
        if (writer_fd >= 0) close(writer_fd);
        w->ok = false;
        w->done = true;
        atomic_store_explicit(&w->done, true, memory_order_release);
        scan_walk_release(w);
        return NULL;
    }
    scan_spool_context_t context = { spool, w->root_fd, &w->cancel };
    w->ok = file_browser_walk_all_songs_excluding_top_level(
        w->root, AUDIOBOOKS_LIBRARY_DIR_NAME, scan_spool_visit_cb, &context, &w->count, &w->progress,
        &w->skipped);
    if (fclose(spool) != 0) w->ok = false;
    atomic_store_explicit(&w->done, true, memory_order_release);
    scan_walk_release(w);
    return NULL;
}

#define SCAN_WALK_THREAD_STACK_SIZE (1024 * 1024)
static bool scan_all_songs_with_timeout(const char * root, int * out_spool_fd,
                                        scan_walk_work_t ** out_owner,
                                        int * out_count, int * out_skipped) {
    scan_walk_work_t * w = calloc(1, sizeof(*w));
    if (!w) return false;
    w->root_fd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (w->root_fd < 0) { free(w); return false; }
    snprintf(w->root, sizeof(w->root), "/proc/self/fd/%d", w->root_fd);
    w->spool_fd = openat(w->root_fd, ".compas_scan.tmp", O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (w->spool_fd < 0) { close(w->root_fd); free(w); return false; }
    unlinkat(w->root_fd, ".compas_scan.tmp", 0);
    atomic_store(&w->refs, 2);
    atomic_store(&w->cancel, false);

    pthread_attr_t attr;
    pthread_attr_t * attr_ptr = NULL;
    if (pthread_attr_init(&attr) == 0) {
        if (pthread_attr_setstacksize(&attr, SCAN_WALK_THREAD_STACK_SIZE) == 0) attr_ptr = &attr;
    }

    pthread_t thread;
    bool created = pthread_create(&thread, attr_ptr, scan_walk_worker, w) == 0;
    if (attr_ptr) pthread_attr_destroy(&attr);
    if (!created) {
        close(w->spool_fd); close(w->root_fd);
        free(w);
        return false;
    }
    pthread_detach(thread);

    int last_seen_progress = 0;
    int stalled_ms = 0;
    for (;;) {
        if (atomic_load_explicit(&w->done, memory_order_acquire)) {
            bool ok = w->ok;
            if (ok) {
                *out_spool_fd = w->spool_fd;
                *out_owner = w;
                *out_count = w->count;
                if (out_skipped) *out_skipped = w->skipped;
            } else {
                /* The worker has already released its owner on done. */
            }
            if (!ok) scan_walk_release(w);
            return ok;
        }
        if (atomic_load_explicit(&library_scan_cancel_requested, memory_order_acquire))
            atomic_store_explicit(&w->cancel, true, memory_order_release);
        int progress = w->progress;
        library_scan_walk_items = progress;
        if (progress != last_seen_progress) {
            last_seen_progress = progress;
            stalled_ms = 0;
        } else {
            stalled_ms += 20;
            if (stalled_ms >= LIBRARY_SCAN_WALK_STALL_TIMEOUT_MS) break;
        }
        usleep(20000);
    }

    /* The worker may be in uninterruptible I/O. Do not touch/free its state.
     * Its uniquely named spool can be orphaned safely and removed on a later
     * maintenance pass; critically, it owns no tagcache lock or GUI memory. */
    fprintf(stderr, "Warning: scan of %s stalled with no progress for %ds (possible filesystem corruption)\n",
            root, LIBRARY_SCAN_WALK_STALL_TIMEOUT_MS / 1000);
    atomic_store_explicit(&w->cancel, true, memory_order_release);
    scan_walk_release(w);
    return false;
}

static bool scan_spool_read_path(FILE * f, char * path, size_t path_size) {
    uint32_t len = 0;
    if (fread(&len, sizeof(len), 1, f) != 1) return false;
    if (len == 0 || len >= path_size) return false;
    if (fread(path, 1, len, f) != len) return false;
    if (memchr(path, '\0', len) != NULL) return false;
    path[len] = '\0';
    return true;
}

/* Prove that discovery still describes the complete unchanged library before
 * entering the staging path. Any malformed or ambiguous spool data falls
 * through to the normal update so the existing recovery semantics remain. */
static bool library_scan_unchanged_fast_path(int spool_fd, int root_fd, int discovered_count,
                                              bool force_metadata) {
    if (force_metadata || lseek(spool_fd, 0, SEEK_SET) < 0) return false;
    int read_fd = dup(spool_fd);
    FILE * spool = read_fd >= 0 ? fdopen(read_fd, "rb") : NULL;
    if (!spool && read_fd >= 0) close(read_fd);
    if (!spool) return false;

    bool unchanged = true;
    char path[PATH_MAX];
    for (int i = 0; i < discovered_count; i++) {
        struct stat st;
        cached_tags_t cached;
        char actual[PATH_MAX];
        size_t root_len = strlen(MUSIC_ROOT_DIR);
        if (!scan_spool_read_path(spool, path, sizeof(path)) || path[0] == '\0' ||
            strncmp(path, MUSIC_ROOT_DIR, root_len) != 0 ||
            (path[root_len] != '/' && path[root_len] != '\0') ||
            snprintf(actual, sizeof(actual), "/proc/self/fd/%d%s", root_fd, path + root_len) >= (int) sizeof(actual) ||
            stat(actual, &st) != 0 || !metadata_db_storage_current() ||
            atomic_load_explicit(&library_scan_cancel_requested, memory_order_acquire) ||
            !metadata_db_get(path, (int64_t) st.st_mtime, (int64_t) st.st_size, &cached)) {
            unchanged = false;
            break;
        }
    }

    /* The count must describe the whole spool, including no trailing record
     * or read error, before it can authorize skipping the update. */
    if (unchanged && (fgetc(spool) != EOF || ferror(spool))) unchanged = false;
    if (fclose(spool) != 0) unchanged = false;
    if (!unchanged) return false;
    library_scan_progress_done = discovered_count;
    library_scan_phase = LIBRARY_SCAN_PHASE_SAVE;
    library_rescan_succeeded = true;
    library_rescan_incomplete = false;
    library_rescan_saved = false;
    DB_LOG("DB", "update_unchanged files=%d rss_kb=%ld", discovered_count, db_log_rss_kb());
    return true;
}

typedef enum {
    SCAN_SONG_CACHED,   /* metadata_db_get() hit -- no parse attempted */
    SCAN_SONG_PARSED,   /* isolated read + DB upsert succeeded */
    SCAN_SONG_FAILED,   /* metadata_read_isolated() failed -- isolated child timeout/crash/malformed input */
    SCAN_SONG_NO_STAT,  /* stat() failed -- metadata was parsed but never upserted (no mtime/size to key the row by) */
} scan_song_result_t;

static int compare_scan_target_paths(const void * a, const void * b) {
    return strcmp(*(char * const *) a, *(char * const *) b);
}

static int library_refresh_scope_song_page(int offset, song_row_t * rows, int count) {
    switch (library_rescan_scope) {
        case LIBRARY_REFRESH_SCOPE_ALBUM:
            return metadata_db_get_album_songs(library_rescan_scope_name,
                library_rescan_scope_album_artist, offset, rows, count);
        case LIBRARY_REFRESH_SCOPE_ARTIST:
            return metadata_db_get_artist_songs(library_rescan_scope_name, offset, rows, count);
        case LIBRARY_REFRESH_SCOPE_ALBUM_ARTIST:
            return metadata_db_get_album_artist_songs(library_rescan_scope_name, offset, rows, count);
        case LIBRARY_REFRESH_SCOPE_NONE:
        case LIBRARY_REFRESH_SCOPE_PATHS:
            return 0;
    }
    return 0;
}

static bool library_scan_collect_refresh_scope(void) {
    enum { PAGE_SIZE = 64 };
    song_row_t rows[PAGE_SIZE];
    char ** paths = NULL;
    int count = 0, capacity = 0;
    for (int offset = 0; ; ) {
        int got = library_refresh_scope_song_page(offset, rows, PAGE_SIZE);
        if (got <= 0) break;
        if (count > INT_MAX - got) goto fail;
        int needed = count + got;
        if (needed > capacity) {
            int grown_capacity = capacity ? capacity : PAGE_SIZE;
            while (grown_capacity < needed) {
                if (grown_capacity > INT_MAX / 2) { grown_capacity = needed; break; }
                grown_capacity *= 2;
            }
            char ** grown = realloc(paths, sizeof(*paths) * (size_t) grown_capacity);
            if (!grown) goto fail;
            paths = grown;
            capacity = grown_capacity;
        }
        for (int i = 0; i < got; i++) {
            paths[count] = strdup(rows[i].path);
            if (!paths[count]) goto fail;
            count++;
        }
        if (got < PAGE_SIZE) break;
        if (offset > INT_MAX - got) goto fail;
        offset += got;
    }
    if (count > 1) qsort(paths, (size_t) count, sizeof(*paths), compare_scan_target_paths);
    int unique = 0;
    for (int i = 0; i < count; i++) {
        if (unique > 0 && strcmp(paths[unique - 1], paths[i]) == 0) {
            free(paths[i]);
            continue;
        }
        paths[unique++] = paths[i];
    }
    library_rescan_target_paths = paths;
    library_rescan_target_count = unique;
    return true;
fail:
    for (int i = 0; i < count; i++) free(paths[i]);
    free(paths);
    return false;
}

static bool cached_song_tags_equal(const cached_tags_t * a, const cached_tags_t * b) {
    return strcmp(a->title, b->title) == 0 && strcmp(a->artist, b->artist) == 0 &&
           strcmp(a->album, b->album) == 0 && strcmp(a->album_artist, b->album_artist) == 0 &&
           strcmp(a->genre, b->genre) == 0 && a->track_number == b->track_number &&
           a->disc_number == b->disc_number;
}

static scan_song_result_t scan_song_stat_into_db(const char * path, const char * db_path,
                                                   const struct stat * source_stat,
                                                   bool force_metadata, bool * out_changed,
                                                   bool * out_identity_changed, bool * out_inserted) {
    if (out_changed) *out_changed = false;
    if (out_identity_changed) *out_identity_changed = false;
    if (out_inserted) *out_inserted = false;
    bool have_stat = source_stat != NULL;
    int64_t mtime = have_stat ? (int64_t) source_stat->st_mtime : 0;
    int64_t size = have_stat ? (int64_t) source_stat->st_size : 0;

    song_row_t previous = {0};
    bool had_previous = force_metadata && metadata_db_get_song_by_path(db_path, &previous);
    /* Only a path stat() just found counts as present on the card. */
    if (had_previous && have_stat) library_rescan_matched_existing++;

    /* Record breadcrumb path prior to reading metadata so crash diagnostics
     * can report the specific file being parsed if an unhandled signal occurs. */
    snprintf(g_scan_last_path, sizeof(g_scan_last_path), "%s", path);

    track_metadata_t meta;
    if (!metadata_read_isolated(path, &meta, LIBRARY_SCAN_FILE_TIMEOUT_MS)) return SCAN_SONG_FAILED;

    cached_tags_t fresh;
    memset(&fresh, 0, sizeof(fresh));
    fresh.track_number = meta.has_track_number ? meta.track_number : -1;
    fresh.disc_number = meta.has_disc_number ? meta.disc_number : -1;
    snprintf(fresh.title, sizeof(fresh.title), "%s", meta.has_title ? meta.title : "");
    snprintf(fresh.artist, sizeof(fresh.artist), "%s", meta.has_artist ? meta.artist : "Unknown Artist");
    snprintf(fresh.album, sizeof(fresh.album), "%s", meta.has_album ? meta.album : "Unknown Album");
    const char * album_artist_value = meta.has_album_artist ? meta.album_artist
                                     : (meta.has_artist ? meta.artist : "Unknown Artist");
    snprintf(fresh.album_artist, sizeof(fresh.album_artist), "%s", album_artist_value);
    snprintf(fresh.genre, sizeof(fresh.genre), "%s", meta.has_genre ? meta.genre : "Unknown Genre");
    free(meta.picture_data);
    free(meta.lyrics); /* always NULL here (metadata_read_isolated() itself already frees/NULLs it before the pipe write), freeing defensively for symmetry */

    if (!have_stat) return SCAN_SONG_NO_STAT;
    /* A forced refresh only writes rows whose tags differ. Leaving same-tag
     * rows untouched also leaves a changed mtime/size stale; the next ordinary
     * scan misses that old cache key and reads the file once. A parse that
     * differs only in a spelling tagcache interns back to the stored one is
     * written but reported unchanged by tagcache, so it cannot force a commit. */
    bool exact_match = force_metadata && had_previous && cached_song_tags_equal(&previous.tags, &fresh);
    bool stored_changed = !exact_match && metadata_db_put(db_path, mtime, size, &fresh);
    if (force_metadata) {
        if (out_changed) *out_changed = had_previous && stored_changed;
        if (out_inserted) *out_inserted = !had_previous;
        if (out_identity_changed) {
            /* Judged from the parse: a spelling tagcache interns back can only
             * over-report here, and only for a row that changed anyway. */
            song_row_t refreshed = {0};
            size_t path_len = strlen(db_path);
            if (path_len >= sizeof(refreshed.path)) path_len = sizeof(refreshed.path) - 1;
            memcpy(refreshed.path, db_path, path_len);
            refreshed.path[path_len] = '\0';
            refreshed.tags = fresh;
            *out_identity_changed = !had_previous ||
                (stored_changed && (strcmp(previous.tags.artist, fresh.artist) != 0 ||
                                    song_album_thumbnail_key(&previous) != song_album_thumbnail_key(&refreshed)));
        }
    }
    return SCAN_SONG_PARSED;
}

static scan_song_result_t scan_one_song_into_db(const char * path, const char * db_path,
                                                 bool force_metadata, bool * out_changed,
                                                 bool * out_identity_changed, bool * out_inserted) {
    if (out_changed) *out_changed = false;
    if (out_identity_changed) *out_identity_changed = false;
    if (out_inserted) *out_inserted = false;
    struct stat st;
    bool have_stat = stat(path, &st) == 0;
    int64_t mtime = have_stat ? (int64_t) st.st_mtime : 0;
    int64_t size = have_stat ? (int64_t) st.st_size : 0;
    cached_tags_t cached;
    if (!force_metadata && have_stat && metadata_db_get(db_path, mtime, size, &cached)) return SCAN_SONG_CACHED;
    return scan_song_stat_into_db(path, db_path, have_stat ? &st : NULL, force_metadata,
                                  out_changed, out_identity_changed, out_inserted);
}

static bool library_scan_target_actual_path(const char * path, int root_fd,
                                            char * actual_path, size_t actual_path_size) {
    size_t root_len = strlen(MUSIC_ROOT_DIR);
    if (!path || strncmp(path, MUSIC_ROOT_DIR, root_len) != 0 || path[root_len] != '/') return false;
    const char * relative = path + root_len + 1;
    for (const char * component = relative; ; ) {
        const char * slash = strchr(component, '/');
        size_t length = slash ? (size_t) (slash - component) : strlen(component);
        if (length == 0 || (length == 1 && component[0] == '.') ||
            (length == 2 && component[0] == '.' && component[1] == '.')) return false;
        if (!slash) break;
        component = slash + 1;
    }
    int n = snprintf(actual_path, actual_path_size, "/proc/self/fd/%d/%s", root_fd, relative);
    return n > 0 && (size_t) n < actual_path_size;
}

static void library_scan_targeted_once(void) {
    library_rescan_refreshed_count = 0;
    library_rescan_changed_count = 0;
    library_rescan_failed_count = 0;
    library_rescan_matched_existing = 0;
    library_rescan_identity_changed = false;
    library_rescan_succeeded = false;
    library_rescan_incomplete = false;
    library_rescan_load_blocked = false;
    library_rescan_saved = false;
    library_rescan_published = false;
    library_rescan_migrating = metadata_db_migration_needed();
    library_rescan_migration_cleanup = metadata_db_migration_cleanup_pending();
    library_scan_progress_done = 0;
    library_scan_progress_total = library_rescan_scope == LIBRARY_REFRESH_SCOPE_PATHS
                                      ? library_rescan_target_count : 0;
    library_scan_walk_items = 0;
    library_scan_phase = library_rescan_scope == LIBRARY_REFRESH_SCOPE_PATHS
                             ? LIBRARY_SCAN_PHASE_TAGS : LIBRARY_SCAN_PHASE_WALK;
    bool db_logging = db_log_enabled();
    uint64_t scan_started_ms = db_logging ? db_log_now_ms() : 0;

    if (atomic_load_explicit(&library_scan_cancel_requested, memory_order_acquire)) return;

    DB_LOG("DB", "targeted_refresh_begin targets=%d rss_kb=%ld",
           library_rescan_target_count, db_log_rss_kb());
    bool preserve_rebuild = false;
    if (!library_scan_open_database(library_rescan_allow_rebuild, false, NULL,
                                    &preserve_rebuild)) return;
    if (preserve_rebuild) {
        /* A preserved rebuild is a provisional empty cache. A partial update
         * must never publish only the selected rows as a replacement library. */
        library_rescan_load_blocked = true;
        library_operation_outcome = combine_load_outcomes(library_operation_outcome, METADATA_DB_LOAD_FAILED);
        return;
    }
    if (!sd_card_root_is_mounted() || !metadata_db_storage_current()) {
        library_operation_outcome = combine_load_outcomes(library_operation_outcome, METADATA_DB_LOAD_UNMOUNTED);
        return;
    }
    if (library_rescan_scope != LIBRARY_REFRESH_SCOPE_NONE &&
        library_rescan_scope != LIBRARY_REFRESH_SCOPE_PATHS &&
        !library_scan_collect_refresh_scope()) {
        library_operation_outcome = combine_load_outcomes(library_operation_outcome, METADATA_DB_LOAD_FAILED);
        return;
    }

    int target_count = library_rescan_target_count;
    library_scan_progress_total = target_count;
    library_scan_progress_done = 0;
    library_scan_phase = LIBRARY_SCAN_PHASE_TAGS;
    if (target_count <= 0) {
        library_rescan_succeeded = true;
        return;
    }

    int root_fd = open(MUSIC_ROOT_DIR, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (root_fd < 0) {
        library_operation_outcome = combine_load_outcomes(library_operation_outcome,
            sd_card_root_is_mounted() ? METADATA_DB_LOAD_FAILED : METADATA_DB_LOAD_UNMOUNTED);
        return;
    }

    metadata_db_begin_targeted_update();
    bool update_ok = true;
    bool storage_lost = false;
    bool target_deleted = false;
    bool row_inserted = false;
    char actual_path[PATH_MAX];
    for (int done = 0; done < target_count; done++) {
        if (atomic_load_explicit(&library_scan_cancel_requested, memory_order_acquire)) break;
        if (!sd_card_root_is_mounted() || !metadata_db_storage_current()) {
            update_ok = false;
            storage_lost = true;
            break;
        }
        const char * path = library_rescan_target_paths[done];
        if (!library_scan_target_actual_path(path, root_fd, actual_path, sizeof(actual_path))) {
            update_ok = false;
            break;
        }

        struct stat st;
        if (stat(actual_path, &st) != 0) {
            if (errno == ENOENT) {
                song_row_t removed_song;
                bool had_previous = metadata_db_get_song_by_path(path, &removed_song);
                if (!metadata_db_delete_target_path(path)) {
                    update_ok = false;
                    break;
                }
                if (had_previous) {
                    target_deleted = true;
                    library_rescan_identity_changed = true;
                }
            } else {
                library_rescan_incomplete = true;
            }
        } else {
            uint64_t file_started_ms = db_logging ? db_log_now_ms() : 0;
            bool tags_changed = false, identity_changed = false, inserted = false;
            scan_song_result_t result = scan_song_stat_into_db(actual_path, path, &st, true,
                                                               &tags_changed, &identity_changed, &inserted);
            if (result == SCAN_SONG_PARSED) {
                library_rescan_refreshed_count++;
                if (tags_changed) library_rescan_changed_count++;
                if (inserted) row_inserted = true;
                if (identity_changed) library_rescan_identity_changed = true;
            } else if (result == SCAN_SONG_FAILED) {
                library_rescan_incomplete = true;
                library_rescan_failed_count++;
            }
            if (db_logging) {
                uint64_t file_ms = db_log_now_ms() - file_started_ms;
                static const char * const scan_song_result_name[] = { "cached", "parsed", "failed", "no_stat" };
                DB_LOG("DB_FILE", "target_end index=%d result=%s elapsed_ms=%llu path=%s", done,
                       scan_song_result_name[result], (unsigned long long) file_ms, path);
            }
        }
        library_scan_progress_done = done + 1;
    }
    close(root_fd);

    if (atomic_load_explicit(&library_scan_cancel_requested, memory_order_acquire)) {
        metadata_db_abort_update();
        return;
    }
    if (!update_ok || storage_lost || !sd_card_root_is_mounted() || !metadata_db_storage_current()) {
        metadata_db_abort_update();
        library_operation_outcome = combine_load_outcomes(library_operation_outcome,
            storage_lost || !sd_card_root_is_mounted() || !metadata_db_storage_current()
                ? METADATA_DB_LOAD_UNMOUNTED : METADATA_DB_LOAD_FAILED);
        return;
    }

    if (!library_rescan_migrating &&
        metadata_refresh_should_skip_commit(library_rescan_changed_count, target_deleted, row_inserted)) {
        int32_t generation_before = tagcache_generation();
        metadata_db_abort_update();
        library_rescan_published = tagcache_generation() != generation_before;
        library_rescan_saved = false;
        library_rescan_succeeded = true;
        library_operation_outcome = combine_load_outcomes(library_operation_outcome,
                                                           metadata_db_get_load_outcome());
        reset_library_scan_progress();
        DB_LOG("DB", "targeted_refresh_end refreshed=%d changed=%d ok=1 published=%d no_op=1 elapsed_ms=%llu songs=%lld rss_kb=%ld",
               library_rescan_refreshed_count, library_rescan_changed_count, library_rescan_published,
               (unsigned long long) (db_log_now_ms() - scan_started_ms),
               (long long) metadata_db_get_song_count(), db_log_rss_kb());
        return;
    }

    library_scan_phase = LIBRARY_SCAN_PHASE_SAVE;
    int32_t generation_before = tagcache_generation();
    bool committed = metadata_db_end_update();
    if (committed && library_rescan_migrating && metadata_db_storage_current() &&
        !atomic_load_explicit(&library_scan_cancel_requested, memory_order_acquire))
        committed = metadata_db_migration_finish();
    library_rescan_published = tagcache_generation() != generation_before;
    if (committed && library_rescan_migrating) library_rescan_migration_cleanup = true;
    library_rescan_saved = committed && !preserve_rebuild;
    library_operation_outcome = combine_load_outcomes(library_operation_outcome, metadata_db_get_load_outcome());
    if (!committed)
        library_operation_outcome = combine_load_outcomes(library_operation_outcome, METADATA_DB_LOAD_FAILED);
    library_rescan_succeeded = committed;

    reset_library_scan_progress();
    metadata_db_load_outcome_t reload_outcome = metadata_db_storage_current()
        ? metadata_db_get_load_outcome() : METADATA_DB_LOAD_UNMOUNTED;
    library_rescan_saved = library_rescan_saved && reload_outcome == METADATA_DB_LOAD_SUCCESS_NORMAL;
    library_operation_outcome = combine_load_outcomes(library_operation_outcome, reload_outcome);
    DB_LOG("DB", "targeted_refresh_end refreshed=%d changed=%d ok=%d elapsed_ms=%llu songs=%lld rss_kb=%ld",
           library_rescan_refreshed_count, library_rescan_changed_count, committed,
           (unsigned long long) (db_log_now_ms() - scan_started_ms),
           (long long) metadata_db_get_song_count(), db_log_rss_kb());
}

/* Overall scan progress, polled by update_timer_cb while library_rescan_active
 * is true to drive the "Updating music database..." screen's progress bar
 * (Settings > Update Music Database) -- purely cosmetic, for the user's
 * peace of mind that a rescan is actually moving rather than stuck, since
 * the underlying incremental scan (metadata_db.c's mtime/size cache) is
 * already fast on an unchanged library. _total is set once discovered_count
 * is known (library_scan_once(), before its spool-reading loop starts);
 * _done is advanced by that same loop, one file at a time (see
 * scan_one_song_into_db()'s own caller in library_scan_once()). */


/* Refreshes the persistent playlist cache (metadata_db.c) from
 * PLAYLISTS_DIR only (the SD card's Playlists folder), not a walk of the
 * whole music tree. Folded into library_scan_once() so it runs on Update
 * Music Database. Ordinary in-app create/delete update the cache with a
 * single insert/delete instead of calling this. */
void gui_library_poll_playlists(void) {
    static lv_obj_t * last_screen;
    static uint32_t last_check;
    static bool startup = true, was_storage;
    lv_obj_t * screen = lv_screen_active();
    bool visible = screen && (screen == playlists_screen || screen == add_to_playlist_screen);
    bool entered = visible && screen != last_screen;
    last_screen = screen;
    bool refresh_ok = false;
    if (playlist_files_refresh_poll(&refresh_ok)) {
        /* A failed scan can still have pruned deleted playlists. */
        if (visible) {
            lv_obj_t * list = screen == playlists_screen ? playlists_list : add_to_playlist_list;
            int32_t scroll = lv_obj_get_scroll_y(list);
            if (screen == playlists_screen) populate_playlists_screen();
            else populate_add_to_playlist_screen();
            lv_obj_scroll_to_y(list, scroll, LV_ANIM_OFF);
        }
        if (playlists_manual_refresh) {
            playlists_manual_refresh = false;
            set_header_refresh_action_busy(playlists_refresh_icon, false);
            if (refresh_ok) show_info_toast("Playlists refreshed");
            else show_error_toast("Some playlists could not be read");
        }
    }
    if (!backlight_screen_is_on()) return;
#ifndef HOST_BUILD
    if (!sd_card_root_is_mounted()) return;
#endif
    uint32_t now = lv_tick_get();
    if (!startup && !entered && now - last_check < 5000) return;
    last_check = now;
    usb_mode_t mode;
    bool storage = gui_network_usb_storage_session_active() &&
                   usb_mode_control_detect_current(&mode) && mode == USB_MODE_STORAGE;
    bool returned = was_storage && !storage;
    was_storage = storage;
    if (storage) return;
    if (screen == group_songs_screen && group_songs_edit_m3u_path && !group_playlist_unchanged()) {
        struct stat st;
        if (stat(group_songs_edit_m3u_path, &st) == 0) {
            int32_t scroll = lv_obj_get_scroll_y(group_songs_list);
            reload_edited_playlist();
            lv_obj_scroll_to_y(group_songs_list, scroll, LV_ANIM_OFF);
        }
    }
    if (startup || visible || returned) playlist_files_refresh_async(PLAYLISTS_DIR);
    startup = false;
}

static void rescan_playlists(void) {
    /* Runs before library_scan_once()'s own mount check: never prune or
     * create folders against an unmounted, empty mount point. */
    bool mounted = sd_card_root_is_mounted();
    if (mounted) ensure_playlists_dir();
    playlist_files_reconcile(PLAYLISTS_DIR, mounted);
}

static bool library_scan_open_database(bool allow_rebuild, bool allow_pending_migration,
                                       metadata_db_load_outcome_t * out_open_outcome,
                                       bool * out_preserve_rebuild) {
    metadata_db_load_outcome_t open_outcome = metadata_db_open();
    bool preserve_rebuild = false;
    if (open_outcome == METADATA_DB_LOAD_FAILED && allow_rebuild) {
        bool prepared = metadata_db_prepare_rebuild();
        open_outcome = metadata_db_get_load_outcome();
        preserve_rebuild = prepared && open_outcome == METADATA_DB_LOAD_FAILED;
        if (preserve_rebuild)
            open_outcome = METADATA_DB_LOAD_SUCCESS_NORMAL;
    }
    if (out_open_outcome) *out_open_outcome = open_outcome;
    if (out_preserve_rebuild) *out_preserve_rebuild = preserve_rebuild;
    library_operation_outcome = combine_load_outcomes(library_operation_outcome, open_outcome);
    if (library_operation_outcome == METADATA_DB_LOAD_FAILED ||
        library_operation_outcome == METADATA_DB_LOAD_UNMOUNTED) {
        library_rescan_load_blocked = !allow_rebuild;
        return false;
    }
    if (!allow_rebuild && open_outcome == METADATA_DB_LOAD_SUCCESS_RECOVERED) {
        library_rescan_load_blocked = true;
        return false;
    }
    library_rescan_migrating = metadata_db_migration_needed();
    library_rescan_migration_cleanup = metadata_db_migration_cleanup_pending();
    if (!allow_pending_migration && library_rescan_migrating && !library_rescan_migration_cleanup) {
        /* Legacy rating replay needs every song marked seen by a full scan. */
        library_operation_outcome = combine_load_outcomes(library_operation_outcome, METADATA_DB_LOAD_FAILED);
        return false;
    }
    if (!metadata_db_migration_prepare()) {
        library_operation_outcome = combine_load_outcomes(library_operation_outcome, METADATA_DB_LOAD_FAILED);
        return false;
    }
    return true;
}

static void library_scan_once_with_options(bool force_all_metadata) {
    library_rescan_published = false;
    library_rescan_refreshed_count = 0;
    library_rescan_changed_count = 0;
    library_rescan_failed_count = 0;
    library_rescan_matched_existing = 0;
    library_rescan_identity_changed = false;
    library_rescan_succeeded = false;
    library_rescan_incomplete = false;
    library_rescan_load_blocked = false;
    library_rescan_saved = false;
    library_rescan_migrating = metadata_db_migration_needed();
    library_rescan_migration_cleanup = metadata_db_migration_cleanup_pending();
    bool db_logging = db_log_enabled();
    uint64_t scan_started_ms = db_logging ? db_log_now_ms() : 0;
    uint64_t phase_started_ms = scan_started_ms;
    library_scan_progress_done = 0;
    library_scan_progress_total = 0;
    library_scan_walk_items = 0;
    library_scan_phase = LIBRARY_SCAN_PHASE_WALK;

    if (atomic_load_explicit(&library_scan_cancel_requested, memory_order_acquire)) return;

    DB_LOG("DB", "scan_begin root=%s rss_kb=%ld", MUSIC_ROOT_DIR, db_log_rss_kb());
    metadata_db_load_outcome_t open_outcome;
    bool preserve_rebuild = false;
    if (!library_scan_open_database(library_rescan_allow_rebuild, true,
                                    &open_outcome, &preserve_rebuild)) return;
    int64_t song_count_before_scan = metadata_db_get_song_count();
    DB_LOG("DB", "db_open elapsed_ms=%llu songs=%lld rss_kb=%ld",
           (unsigned long long) (db_log_now_ms() - phase_started_ms), (long long) song_count_before_scan,
           db_log_rss_kb());
    phase_started_ms = db_logging ? db_log_now_ms() : 0;
    gui_books_rescan();
    if (atomic_load_explicit(&library_scan_cancel_requested, memory_order_acquire)) return;
    DB_LOG("DB", "books_scan_end elapsed_ms=%llu rss_kb=%ld",
           (unsigned long long) (db_log_now_ms() - phase_started_ms), db_log_rss_kb());
    phase_started_ms = db_logging ? db_log_now_ms() : 0;
    rescan_playlists();
    if (atomic_load_explicit(&library_scan_cancel_requested, memory_order_acquire)) return;
    DB_LOG("DB", "playlists_scan_end elapsed_ms=%llu rss_kb=%ld",
           (unsigned long long) (db_log_now_ms() - phase_started_ms), db_log_rss_kb());

    /* An unmounted card leaves the mount point readable and empty, which walks
     * cleanly to zero files. */
    if (!sd_card_root_is_mounted()) {
        DB_LOG("DB", "discover_skipped reason=not_mounted rss_kb=%ld", db_log_rss_kb());
        library_operation_outcome = combine_load_outcomes(library_operation_outcome, METADATA_DB_LOAD_UNMOUNTED);
        return;
    }

    int spool_fd = -1;
    scan_walk_work_t * spool_owner = NULL;
    int discovered_count = 0;
    int skipped_count = 0;
    phase_started_ms = db_logging ? db_log_now_ms() : 0;
    if (!scan_all_songs_with_timeout(MUSIC_ROOT_DIR, &spool_fd, &spool_owner, &discovered_count,
                                     &skipped_count)) {
        DB_LOG("DB", "discover_failed elapsed_ms=%llu rss_kb=%ld",
               (unsigned long long) (db_log_now_ms() - phase_started_ms), db_log_rss_kb());
        library_operation_outcome = combine_load_outcomes(library_operation_outcome, METADATA_DB_LOAD_FAILED);
        return;
    }
    if (atomic_load_explicit(&library_scan_cancel_requested, memory_order_acquire)) {
        scan_walk_release(spool_owner);
        metadata_db_abort_update();
        return;
    }
    DB_LOG("DB", "discover_end files=%d skipped=%d elapsed_ms=%llu spool=%s rss_kb=%ld",
           discovered_count, skipped_count, (unsigned long long) (db_log_now_ms() - phase_started_ms),
           "fd-backed", db_log_rss_kb());

    if (preserve_rebuild && discovered_count == 0) {
        DB_LOG("DB", "rebuild_rejected reason=no_files");
        metadata_db_abort_update();
        library_operation_outcome = combine_load_outcomes(library_operation_outcome, METADATA_DB_LOAD_FAILED);
        scan_walk_release(spool_owner);
        return;
    }

    library_scan_progress_total = discovered_count;
    library_scan_phase = LIBRARY_SCAN_PHASE_TAGS;
    bool force_metadata_anywhere = force_all_metadata;
    if (open_outcome == METADATA_DB_LOAD_SUCCESS_NORMAL && !preserve_rebuild &&
        !library_rescan_migrating && skipped_count == 0 && discovered_count >= 0 &&
        metadata_db_get_song_count() == (int64_t) discovered_count &&
        library_scan_unchanged_fast_path(spool_fd, spool_owner->root_fd, discovered_count,
                                         force_metadata_anywhere)) {
        scan_walk_release(spool_owner);
        return;
    }

    phase_started_ms = db_logging ? db_log_now_ms() : 0;
    metadata_db_begin_update();
    DB_LOG("DB", "update_begin files=%d rss_kb=%ld", discovered_count, db_log_rss_kb());

    if (lseek(spool_fd, 0, SEEK_SET) < 0) {
        metadata_db_abort_update();
        scan_walk_release(spool_owner);
        return;
    }
    int spool_read_fd = dup(spool_fd);
    FILE * spool = spool_read_fd >= 0 ? fdopen(spool_read_fd, "rb") : NULL;
    if (!spool) {
        if (spool_read_fd >= 0) close(spool_read_fd);
        DB_LOG("DB", "spool_open_failed rss_kb=%ld", db_log_rss_kb());
        metadata_db_abort_update();
        library_operation_outcome = combine_load_outcomes(library_operation_outcome, metadata_db_get_load_outcome());
        library_operation_outcome = combine_load_outcomes(library_operation_outcome, METADATA_DB_LOAD_FAILED);
        scan_walk_release(spool_owner);
        return;
    }

    bool complete = true;
    bool row_inserted = false;
    char path[PATH_MAX];
    int done = 0;
    /* Cached once, not read fresh every song: db_log_now_ms() is a real
     * clock_gettime() syscall on this target (no vDSO), and DB_LOG's own
     * internal enabled-check can't save that cost since its arguments (the
     * elapsed-time expressions below) are already evaluated before the call
     * is even made. Skipping db_log_now_ms()/DB_LOG entirely when off is the
     * only way a 10,000+ song scan doesn't pay two syscalls per file even
     * with logging disabled (the default). */
    while (done < discovered_count) {
        if (atomic_load_explicit(&library_scan_cancel_requested, memory_order_acquire)) {
            complete = false;
            break;
        }
        if (!scan_spool_read_path(spool, path, sizeof(path))) {
            complete = false;
            break;
        }
        if (path[0] == '\0') {
            complete = false;
            break;
        }
        if (path[0] != '\0') {
            if (!metadata_db_storage_current() || atomic_load_explicit(&library_scan_cancel_requested, memory_order_acquire)) {
                complete = false;
                break;
            }
            char actual_path[PATH_MAX];
            size_t root_len = strlen(MUSIC_ROOT_DIR);
            if (strncmp(path, MUSIC_ROOT_DIR, root_len) != 0 ||
                (path[root_len] != '/' && path[root_len] != '\0') ||
                snprintf(actual_path, sizeof(actual_path), "/proc/self/fd/%d%s", spool_owner->root_fd,
                         path + root_len) >= (int) sizeof(actual_path)) {
                complete = false;
                break;
            }
            uint64_t file_started_ms = db_logging ? db_log_now_ms() : 0;
            /* No rss_kb here -- see db_log_rss_kb()'s own comment; RSS is
             * only worth the /proc read at phase boundaries, progress
             * intervals, slow files, and failures, not on every song. Plain
             * buffered logging, no per-file flush -- crash forensics for the
             * parent are already covered by g_scan_last_path (set inside
             * scan_one_song_into_db() itself, see its own comment) plus
             * main.c's crash handler, which fsyncs reload_diag.log; the
             * actual metadata parse also runs in an isolated child process
             * (metadata_read_isolated()), so a malformed file yields
             * SCAN_SONG_FAILED below rather than crashing this process. */
            if (db_logging) DB_LOG("DB_FILE", "file_begin index=%d total=%d path=%s", done, discovered_count, path);
            bool tags_changed = false, identity_changed = false, inserted = false;
            scan_song_result_t result = scan_one_song_into_db(actual_path, path, force_all_metadata,
                                                               force_all_metadata ? &tags_changed : NULL,
                                                               force_all_metadata ? &identity_changed : NULL,
                                                               force_all_metadata ? &inserted : NULL);
            /* NO_STAT: the file was parsed but its tags were discarded because
             * stat() failed, so the song was not refreshed either. */
            if (force_all_metadata && (result == SCAN_SONG_FAILED || result == SCAN_SONG_NO_STAT))
                library_rescan_failed_count++;
            if (force_all_metadata && result == SCAN_SONG_PARSED) {
                library_rescan_refreshed_count++;
                if (tags_changed) library_rescan_changed_count++;
                if (inserted) row_inserted = true;
                if (identity_changed) library_rescan_identity_changed = true;
            }
            if (db_logging) {
                uint64_t file_ms = db_log_now_ms() - file_started_ms;
                static const char * const scan_song_result_name[] = { "cached", "parsed", "failed", "no_stat" };
                DB_LOG("DB_FILE", "file_end index=%d result=%s elapsed_ms=%llu path=%s", done,
                       scan_song_result_name[result], (unsigned long long) file_ms, path);
                if (file_ms >= 250)
                    DB_LOG("DB", "slow_file index=%d result=%s elapsed_ms=%llu rss_kb=%ld path=%s", done,
                           scan_song_result_name[result], (unsigned long long) file_ms, db_log_rss_kb(), path);
            }
        }
        library_scan_progress_done = ++done;
        if ((done % 1000) == 0 || done == discovered_count)
            DB_LOG("DB", "tag_progress done=%d total=%d elapsed_ms=%llu rss_kb=%ld",
                   done, discovered_count, (unsigned long long) (db_log_now_ms() - phase_started_ms),
                   db_log_rss_kb());
    }
    if (complete && (fgetc(spool) != EOF || ferror(spool))) complete = false;
    fclose(spool);
    scan_walk_release(spool_owner);

    if (!metadata_db_storage_current() || atomic_load_explicit(&library_scan_cancel_requested, memory_order_acquire))
        complete = false;

    if (!complete || (library_rescan_migrating && !library_rescan_migration_cleanup && skipped_count > 0)) {
        DB_LOG("DB", "spool_read_incomplete done=%d total=%d rss_kb=%ld", done, discovered_count, db_log_rss_kb());
        metadata_db_abort_update();
        library_operation_outcome = combine_load_outcomes(library_operation_outcome, metadata_db_get_load_outcome());
        library_operation_outcome = combine_load_outcomes(library_operation_outcome, METADATA_DB_LOAD_FAILED);
        return;
    }

    DB_LOG("DB", "update_end skipped=%d", skipped_count);
    if (!metadata_db_storage_current() || atomic_load_explicit(&library_scan_cancel_requested, memory_order_acquire)) {
        metadata_db_abort_update();
        return;
    }
    /* The walk already stat()ed every file on the card. With nothing
     * inserted and no folder skipped, a row is missing exactly when fewer
     * found files matched a row than the (guarded) live count; otherwise the
     * mismatch over-reports. A missing row may be deleted at commit, taking
     * a song (and maybe its cover source) out of an album, so the publication
     * treats it as an identity change. */
    bool missing_paths = force_all_metadata &&
                         (int64_t) library_rescan_matched_existing != metadata_db_get_song_count();
    if (missing_paths) library_rescan_identity_changed = true;
    if (force_all_metadata && !library_rescan_migrating && !preserve_rebuild && skipped_count == 0 &&
        metadata_refresh_should_skip_commit(library_rescan_changed_count, false, row_inserted)) {
        if (atomic_load_explicit(&library_scan_cancel_requested, memory_order_acquire) ||
            !sd_card_root_is_mounted() || !metadata_db_storage_current()) {
            metadata_db_abort_update();
            if (!sd_card_root_is_mounted() || !metadata_db_storage_current())
                library_operation_outcome = combine_load_outcomes(library_operation_outcome, METADATA_DB_LOAD_UNMOUNTED);
            return;
        }
        if (!missing_paths) {
            int32_t generation_before = tagcache_generation();
            metadata_db_abort_update();
            library_rescan_published = tagcache_generation() != generation_before;
            library_rescan_saved = false;
            library_rescan_succeeded = true;
            library_rescan_incomplete = false;
            library_operation_outcome = combine_load_outcomes(library_operation_outcome,
                                                               metadata_db_get_load_outcome());
            reset_library_scan_progress();
            DB_LOG("DB", "refresh_all_end refreshed=%d changed=%d ok=1 published=%d no_op=1 elapsed_ms=%llu songs=%lld rss_kb=%ld",
                   library_rescan_refreshed_count, library_rescan_changed_count, library_rescan_published,
                   (unsigned long long) (db_log_now_ms() - scan_started_ms),
                   (long long) metadata_db_get_song_count(), db_log_rss_kb());
            return;
        }
    }

    library_scan_phase = LIBRARY_SCAN_PHASE_SAVE;
    int32_t generation_before = tagcache_generation();
    bool committed = metadata_db_end_update();
    if (committed && library_rescan_migrating && metadata_db_storage_current() &&
        !atomic_load_explicit(&library_scan_cancel_requested, memory_order_acquire))
        committed = metadata_db_migration_finish();
    library_rescan_published = tagcache_generation() != generation_before;
    if (committed && library_rescan_migrating) library_rescan_migration_cleanup = true;
    metadata_db_load_outcome_t commit_outcome = metadata_db_get_load_outcome();
    library_rescan_saved = committed && !preserve_rebuild;
    library_operation_outcome = combine_load_outcomes(library_operation_outcome, commit_outcome);
    if (!committed)
        library_operation_outcome = combine_load_outcomes(library_operation_outcome, METADATA_DB_LOAD_FAILED);
    library_rescan_succeeded = committed;
    library_rescan_incomplete = committed && skipped_count > 0;
    if (!committed)
        fprintf(stderr, "Warning: music database commit failed -- keeping last on-disk library\n");
    DB_LOG("DB", "commit_end ok=%d files=%d previous_songs=%lld elapsed_ms=%llu songs=%lld rss_kb=%ld", committed,
           done, (long long) song_count_before_scan, (unsigned long long) (db_log_now_ms() - phase_started_ms),
           (long long) metadata_db_get_song_count(), db_log_rss_kb());

    phase_started_ms = db_logging ? db_log_now_ms() : 0;
    reset_library_scan_progress();
    metadata_db_load_outcome_t reload_outcome = metadata_db_storage_current()
        ? metadata_db_get_load_outcome() : METADATA_DB_LOAD_UNMOUNTED;
    library_rescan_saved = library_rescan_saved && reload_outcome == METADATA_DB_LOAD_SUCCESS_NORMAL;
    library_operation_outcome = combine_load_outcomes(library_operation_outcome, reload_outcome);
    DB_LOG("DB", "reload_end songs=%lld elapsed_ms=%llu total_ms=%llu rss_kb=%ld",
           (long long) metadata_db_get_song_count(), (unsigned long long) (db_log_now_ms() - phase_started_ms),
           (unsigned long long) (db_log_now_ms() - scan_started_ms), db_log_rss_kb());
}

void library_scan_once(void) {
    library_scan_once_with_options(false);
}

static void reset_library_scan_progress(void) {
    library_scan_progress_done = 0;
    library_scan_progress_total = 0;
    library_scan_walk_items = 0;
}

static metadata_db_load_outcome_t reload_library_cache(void) {
    reset_library_scan_progress();
    return metadata_db_reload();
}

/* Loads saved metadata without scanning and queues one UI report. */
void library_load_from_cache_only(void) {
    library_prompt_prepare_cache_load_card();
    library_cache_load_card_generation = library_prompt_card_generation;
    library_cache_load_announce_loaded = false;
    library_cache_load_outcome = reload_library_cache();
    library_cache_load_generation++;
    library_cache_load_pending = true;
}

/* Mark the boot warmer for the idle tick after the screen settles. */
void gui_library_start_boot_thumbnail_warmup(void) {
    if (boot_warmup_requested) return;
    boot_warmup_requested = true;
    boot_warmup_pending = true;
}

bool gui_library_has_background_work(void) {
    return library_rescan_active || library_rescan_success_pending ||
           atomic_load(&album_thumbnail_active) ||
           atomic_load(&album_thumb_gen_active) || sd_format_active || search_job_active ||
           (group_probe_job && !track_probe_job_done(group_probe_job));
}

bool gui_library_navigation_blocked(void) {
    /* These operations use a modal progress/completion screen. Thumbnail
     * decoding, post-scan cache warming, and search are deliberately absent:
     * they are cancellable/optional workers and do not own the active screen. */
    return library_rescan_active || library_rescan_success_pending || sd_format_active;
}

void gui_library_prepare_for_ui_reload(void) {
    cancel_group_song_probes();
    bool cover_reload = library_rescan_active &&
        (library_rescan_mode == LIBRARY_RESCAN_REFRESH_COVERS_ALL ||
         library_rescan_mode == LIBRARY_RESCAN_REFRESH_COVERS_ALBUM);
    if (cover_reload) {
        gui_library_cancel_scan();
        /* The cover operation runs its warmer inside library_rescan_thread,
         * which is separate from the ordinary album warmer thread. Join it
         * before teardown deletes lists or rebuilds the UI state it reads. */
        pthread_join(library_rescan_thread, NULL);
        library_rescan_active = false;
        gui_player_cancel_cover_reload();
        gui_busy_hide(library_rescan_token);
        library_rescan_free_target_paths();
        library_rescan_mode = LIBRARY_RESCAN_NORMAL;
        atomic_store_explicit(&library_rescan_done_flag, false, memory_order_relaxed);
    }
    quiesce_album_artwork_workers();
    album_thumbnail_cache_clear();

    if (search_debounce_timer) lv_timer_pause(search_debounce_timer);
    search_job_pending_valid = false;
    if (search_job_active) {
        pthread_join(search_job_thread, NULL);
        search_job_active = false;
    }
    atomic_store_explicit(&search_job_done_flag, false, memory_order_relaxed);
    search_job_for_binding = NULL;
    search_job_pending_binding = NULL;
}

void gui_library_cancel_background_work(void) {
    cancel_group_song_probes();
    if (group_probe_job) {
        track_probe_job_cancel_and_destroy(group_probe_job);
        group_probe_job = NULL;
    }
    quiesce_album_artwork_workers();
    if (library_rescan_active) {
        bool cover_reload = library_rescan_mode == LIBRARY_RESCAN_REFRESH_COVERS_ALL ||
                            library_rescan_mode == LIBRARY_RESCAN_REFRESH_COVERS_ALBUM;
        if (cover_reload) gui_library_cancel_scan();
        pthread_join(library_rescan_thread, NULL);
        library_rescan_active = false;
        gui_busy_hide(library_rescan_token);
        if (cover_reload) {
            gui_player_cancel_cover_reload();
            library_rescan_mode = LIBRARY_RESCAN_NORMAL;
        }
    }
    if (sd_format_active) {
        pthread_join(sd_format_thread, NULL);
        sd_format_active = false;
        gui_busy_hide(sd_format_token);
    }
    if (search_job_active) {
        pthread_join(search_job_thread, NULL);
        search_job_active = false;
    }
}


lv_obj_t * gui_library_get_music_screen(void) { return music_screen; }
lv_obj_t * gui_library_get_files_screen(void) { return files_screen; }
lv_obj_t * gui_library_get_all_songs_screen(void) { return all_songs_screen; }
lv_obj_t * gui_library_get_recently_added_screen(void) { return recently_added_screen; }
lv_obj_t * gui_library_get_artists_screen(void) { return artists_screen; }
lv_obj_t * gui_library_get_albums_screen(void) { return albums_screen; }
lv_obj_t * gui_library_get_album_artist_screen(void) { return album_artist_screen; }
lv_obj_t * gui_library_get_group_songs_screen(void) { return group_songs_screen; }
lv_obj_t * gui_library_get_playlists_screen(void) { return playlists_screen; }
