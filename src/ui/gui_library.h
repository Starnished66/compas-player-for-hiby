#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <lvgl/lvgl.h>
#include "metadata_db.h"
#include "artwork_coordinator.h"

typedef struct group_song_entry_s {
    char * path;
    char * title;
    int32_t disc_number;
    bool show_disc_header;
    int64_t song_id;
    uint64_t artwork_key;
} group_song_entry_t;

void free_group_song_entries(group_song_entry_t * entries, int count);
bool copy_group_song_entries(group_song_entry_t ** out, const group_song_entry_t * entries, int count);
void show_group_songs(const char * title, const group_song_entry_t * entries, int count);

/* Screens accessors */
lv_obj_t * gui_library_get_music_screen(void);
lv_obj_t * gui_library_get_files_screen(void);
lv_obj_t * gui_library_get_all_songs_screen(void);
lv_obj_t * gui_library_get_recently_added_screen(void);
lv_obj_t * gui_library_get_artists_screen(void);
lv_obj_t * gui_library_get_albums_screen(void);
lv_obj_t * gui_library_get_album_artist_screen(void);
lv_obj_t * gui_library_get_group_songs_screen(void);
lv_obj_t * gui_library_get_playlists_screen(void);

void gui_library_init(void);
void gui_library_start_boot_thumbnail_warmup(void);
void gui_library_boot_ready(uint32_t tick);
void gui_library_poll_boot_prompt(void);
void gui_library_request_files_changed_prompt(void);
void gui_library_invalidate_boot_prompt(void);
void gui_library_suspend_boot_prompt(void);
/* Deletes every screen this module owns so gui_reload.c's in-process UI
 * reload can call gui_library_init() again from a clean slate. */
void gui_library_teardown(void);
void gui_library_refresh_music_screen(void);

void start_library_rescan(void);
void start_library_auto_rescan(void);
void start_library_metadata_refresh_paths(const char * const * paths, int count);
void start_library_metadata_refresh_album(const char * album, const char * album_artist);
void start_library_metadata_refresh_artist(metadata_db_group_kind_t kind, const char * name);
void show_library_refresh_all_metadata_prompt(void);
/* Controls boot migration and background import/storage follow-up behavior.
 * Explicit Settings and plugin refresh requests remain available. */
bool gui_library_auto_rescan_enabled(void);
void poll_library_rescan(void);
void poll_sd_format(void);
void album_thumbnail_generation_poll(void);

/* On-demand fallback for the Playing Now page when the background warmer
 * has not produced the player-sized (ALBUMART_PLAYER_CACHE_SIZE) cache for
 * this track yet: finds the original art (external sidecar file, then
 * embedded picture via the same bounded/isolated extraction the warmer
 * uses), decodes and persists the cache exactly as the warmer would, and
 * returns COVER_ART_WIDTH x COVER_ART_HEIGHT pixels for immediate display.
 * One-time cost per track -- once the cache file exists, the caller's own
 * fast path never reaches this again for it.
 *
 * *out_no_art_confirmed (may be NULL) is set true only on a false return
 * that means "this track genuinely has no usable art anywhere", safe for
 * the caller to remember and stop retrying; left false on every transient
 * failure (coordinator busy, isolated-helper fork/pipe/timeout,
 * cancellation), which a caller must retry rather than treat as final. */
bool gui_library_generate_player_cover(const char * track_path, const char * artist,
                                       const char * album, const char * album_artist,
                                       artwork_cancel_fn cancel_cb, void * user_data,
                                       uint16_t ** out_pixels, bool * out_no_art_confirmed);

void open_add_to_playlist_for(const char * path);
void gui_library_poll_playlists(void);
/* Nonblocking duration/codec completion polling; updates only the current page. */
void gui_library_poll_track_probes(void);
void on_cue_file_selected(const char * cue_path);
void show_artist_albums(const char * name, metadata_db_group_kind_t kind);
void refresh_library_screens_after_rescan(void);

void poll_az_index_drag(lv_timer_t * timer);
void plugin_stream_tile_click_cb(lv_event_t * e);
void set_player_source_group_songs_direct(const group_song_entry_t * entries, int count, const char * title, int selected_index);

bool search_close_if_active_for_screen(lv_obj_t * screen);
bool file_browser_back_if_not_root_for_screen(lv_obj_t * screen);
void refresh_now_playing_indicators(void);

/* Shared song identity used by library and queue rows: display title on the
 * first line, then Artist · Album on the metadata line. */
void gui_library_format_song_identity(const song_row_t * row,
                                      char * title, size_t title_size,
                                      char * subtitle, size_t subtitle_size);

void poll_search_job(void);
void play_remote_control_song(const char * song_path, const char * playlist_name, const char * artist_filter,
                              const char * album_artist_filter, const char * album_filter);
void start_power_off_countdown(void);
void build_power_off_countdown_popup(void);
void poll_sd_card_hotplug(void);
bool sd_card_root_is_mounted(void);

void gui_library_resume_fast_timers(void);
void gui_library_reset_drag_state(void);

bool gui_library_has_background_work(void);
/* Experimental (Developer Options): let the cover warmer keep reading already
 * generated thumbnails while audio plays. */
void gui_library_set_covers_during_playback(bool enabled);
/* True only while a modal library operation owns navigation. Optional
 * workers such as album-art warming remain background work for shutdown
 * coordination, but must not disable the drawer or screen gestures. */
bool gui_library_navigation_blocked(void);
/* Joins UI-pointer-bearing artwork/search jobs before a soft screen rebuild. */
void gui_library_prepare_for_ui_reload(void);
void gui_library_cancel_background_work(void);
void gui_library_cancel_scan(void);
