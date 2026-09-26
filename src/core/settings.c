#include "settings.h"
#include "debug_log.h"
player_settings_t current_settings;
#include "subprocess.h"
#include "subsonic_saved_servers.h"
#include "storage_paths.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#ifndef HOST_BUILD
#include <sys/reboot.h>
#endif
#include <sys/stat.h>

#ifdef HOST_BUILD
  #define SETTINGS_DIR_PATH "."
#else
  /* /usr/data is the device's persistent ubifs partition (survives reboots,
   * unlike /tmp) -- the same place the stock firmware keeps its own small
   * settings files (theme_id, region, etc). */
  #define SETTINGS_DIR_PATH "/usr/data"
#endif

#define SETTINGS_TMP_FILE_PATH SETTINGS_FILE_PATH ".tmp"

const int SCREEN_TIMEOUT_STEPS[SCREEN_TIMEOUT_STEP_COUNT] = { 15, 30, 60, 120, 300, 600, 1800 };
const int SCREEN_DIM_DELAY_STEPS[SCREEN_DIM_DELAY_STEP_COUNT] = { 5, 10, 15, 30, 60, 120, 300 };
const int IDLE_SHUTDOWN_STEPS[IDLE_SHUTDOWN_STEP_COUNT] = { 10, 15, 30, 60, 120 };
const int SLEEP_TIMER_STEPS[SLEEP_TIMER_STEP_COUNT] = { 5, 10, 15, 20, 30, 45, 60, 90, 120, 180 };

/* Snap hand-edited and legacy values onto the same presets used by the UI. */
static int nearest_step(int value, const int * steps, int count) {
    int best = steps[0];
    int best_diff = abs(value - best);
    for (int i = 1; i < count; i++) {
        int diff = abs(value - steps[i]);
        if (diff < best_diff) {
            best_diff = diff;
            best = steps[i];
        }
    }
    return best;
}

static bool valid_bt_output_mac(const char * value) {
    if (!value || strlen(value) != 17) return false;
    for (int i = 0; i < 17; i++) {
        if (i == 2 || i == 5 || i == 8 || i == 11 || i == 14) {
            if (value[i] != ':') return false;
        } else {
            const char c = value[i];
            const bool hex = (c >= '0' && c <= '9') ||
                             (c >= 'a' && c <= 'f') ||
                             (c >= 'A' && c <= 'F');
            if (!hex) return false;
        }
    }
    return true;
}

static bool valid_remote_control_pin(const char * value) {
    if (!value) return false;
    size_t length = strlen(value);
    if (length < 4 || length > REMOTE_CONTROL_PIN_MAX_LENGTH) return false;
    for (size_t i = 0; i < length; i++) {
        if (value[i] < '0' || value[i] > '9') return false;
    }
    return true;
}

static void set_defaults(player_settings_t * out) {
    out->volume = 1.0f;
    out->last_track[0] = '\0';
    out->last_position = 0.0;
    out->last_source_kind = 0;
    out->last_source_name[0] = '\0';
    out->resume_mode = 0;
    out->play_pause_button_mode = 0;
    out->accent_color = 0x2196F3; /* matches the app's existing default blue */
    out->crossfade_enabled = false;
    out->gapless_enabled = true; /* on by default -- see settings.h */
    out->replaygain_mode = 1; /* Per Track -- preserves the old replaygain_enabled=true default */
    out->car_mode_enabled = false;
    out->inline_remote_enabled = true;
    out->lyrics_enabled = true;
    out->subsonic_url[0] = '\0';
    out->subsonic_username[0] = '\0';
    out->subsonic_password[0] = '\0';
    out->subsonic_verify_tls = true;
    memset(out->subsonic_saved, 0, sizeof(out->subsonic_saved));
    out->subsonic_saved_count = 0;
    out->bt_volume_sync_enabled = true;
    out->bt_dac_mode_enabled = false;
    out->bt_speexrate_enabled = true;
    out->bt_sample_rate = 0;
    out->bt_device_rate_count = 0;
    memset(out->bt_device_rates, 0, sizeof(out->bt_device_rates));
    snprintf(out->bt_codec, sizeof(out->bt_codec), "auto");
    out->bt_last_output_mac[0] = '\0';
    out->bt_hide_unnamed_devices = true;
    out->wifi_dac_mode_enabled = false;
    out->dlna_renderer_enabled = false;
    out->remote_control_enabled = false;
    out->remote_control_pin[0] = '\0';
    out->screen_timeout_enabled = true;
    out->screen_timeout_seconds = 30;
    out->screen_dimming_enabled = true;
    out->screen_dim_delay_seconds = 10;
    out->hide_player_topbar = false;
    out->led_indicator_enabled = true;
    out->db_logging_enabled = false; /* opt-in developer diagnostic, off by default */
    out->screenshot_combo_enabled = false;
    out->dev_bt_dac_all_codecs = false;
    out->dev_covers_during_playback = false;
    out->charge_limiter_enabled = false; /* opt-in -- caps max charge voltage to 4.2V, a real behavior change the user should choose, not a default surprise */
    out->safe_charging_enabled = false; /* off means leave the PMIC charge-current setting untouched */
    out->show_battery_percent = true; /* on by default -- matches every previous version's always-on behavior */
    /* Defaults on: a device left screen-off with idle_shutdown_enabled=false
     * sits at full power indefinitely. Suspend-to-RAM (not a full poweroff)
     * avoids resetting the music queue. 10 minutes (IDLE_SHUTDOWN_STEPS min)
     * to reliably catch devices left idle. */
    out->idle_shutdown_enabled = true;
    out->idle_shutdown_minutes = 10;
    out->idle_suspend_enabled = true;
    out->idle_suspend_default_migrated = false; /* settings_load()'s own one-time migration sets this true */
    out->usb_mode = 0; /* USB_MODE_STORAGE -- see settings.h's own comment on why this is a plain int */
    out->play_mode = 0; /* PLAY_MODE_SEQUENTIAL */
    out->swipe_up_home_enabled = true;
    out->startup_volume_fixed_enabled = true; /* matches stock's own default */
    out->startup_volume_fixed_percent = 20;
    out->sleep_timer_minutes = 15;
    out->timezone[0] = '\0';
    out->hostname[0] = '\0'; /* empty -- stock's own /usr/resource/hostname stays in effect */
    out->font_size_tier = 0;
    out->lyrics_font_size_tier = 2; /* Large -- see settings.h's own comment */
    out->brightness_percent = 80;
    out->clock_24h = true; /* matches the app's original, only-ever clock format -- existing installs see no change */
    out->clock_automatic = true;
    out->clock_manual_epoch = 0;
    out->clock_system_reference = 0;
    out->custom_font[0] = '\0';
}

void settings_subsonic_server_upsert(player_settings_t * settings, const char * url, const char * username,
                                      const char * password, bool verify_tls) {
    if (!settings || !url || !url[0]) return;
    int found = -1;
    for (int i = 0; i < settings->subsonic_saved_count; i++) {
        if (strcmp(settings->subsonic_saved[i].url, url) == 0) {
            found = i;
            break;
        }
    }
    if (found < 0) {
        if (settings->subsonic_saved_count >= SETTINGS_SUBSONIC_SAVED_MAX) return;
        found = settings->subsonic_saved_count++;
        memset(&settings->subsonic_saved[found], 0, sizeof(settings->subsonic_saved[found]));
    }
    snprintf(settings->subsonic_saved[found].url, sizeof(settings->subsonic_saved[found].url), "%s", url);
    snprintf(settings->subsonic_saved[found].username, sizeof(settings->subsonic_saved[found].username), "%s",
             username ? username : "");
    snprintf(settings->subsonic_saved[found].password, sizeof(settings->subsonic_saved[found].password), "%s",
             password ? password : "");
    settings->subsonic_saved[found].verify_tls = verify_tls;
}

static void import_legacy_subsonic_list_file(player_settings_t * out, const char * path) {
    FILE * f = fopen(path, "r");
    if (!f) return;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
        char * url = line;
        char * user = strchr(url, '\t');
        if (!user) continue;
        *user++ = '\0';
        char * pass = strchr(user, '\t');
        if (!pass) continue;
        *pass++ = '\0';
        char * tls = strchr(pass, '\t');
        if (!tls) continue;
        *tls++ = '\0';
        settings_subsonic_server_upsert(out, url, user, pass, tls[0] != '0');
    }
    fclose(f);
}

static void import_legacy_subsonic_servers(player_settings_t * out) {
    if (out->subsonic_saved_count == 0) {
        import_legacy_subsonic_list_file(out,
#ifdef HOST_BUILD
            "./open_hiby_player_subsonic.txt"
#else
            "/usr/data/open_hiby_player_subsonic.txt"
#endif
        );
#ifndef HOST_BUILD
        import_legacy_subsonic_list_file(out, "/data/mnt/sd_0/.compas/subsonic.list");
        import_legacy_subsonic_list_file(out, "/data/mnt/sd_0/.open_hiby_player/subsonic.list");
#endif
#ifdef HOST_BUILD
        import_legacy_subsonic_list_file(out, "./.compas/subsonic.list");
        import_legacy_subsonic_list_file(out, "./.open_hiby_player/subsonic.list");
#endif
    }
    if (out->subsonic_saved_count == 0 && out->subsonic_url[0]) {
        settings_subsonic_server_upsert(out, out->subsonic_url, out->subsonic_username, out->subsonic_password,
                                        out->subsonic_verify_tls);
    }
}

/* Sidecar is what gui.c reads (via metadata_db_load_subsonic_servers()).
 * Union settings.subsonic_saved into it (so a settings-only list from this
 * tree isn't dropped by an SD TSV left by the other copy), then mirror up
 * to SETTINGS_SUBSONIC_SAVED_MAX sidecar rows back into settings so
 * settings_save() still keeps a copy on /usr/data. Identical rows are not
 * rewritten -- upsert itself skips a no-op save. */
static void sync_subsonic_saved_sidecar(player_settings_t * out) {
    subsonic_saved_server_t * rows = NULL;
    int n = 0;
    subsonic_saved_servers_load(&rows, &n);

    bool wrote = false;
    for (int i = 0; i < out->subsonic_saved_count; i++) {
        if (!out->subsonic_saved[i].url[0]) continue;
        int found = -1;
        for (int j = 0; j < n; j++) {
            if (strcmp(rows[j].url, out->subsonic_saved[i].url) == 0) {
                found = j;
                break;
            }
        }
        if (found >= 0 && strcmp(rows[found].username, out->subsonic_saved[i].username) == 0 &&
            strcmp(rows[found].password, out->subsonic_saved[i].password) == 0 &&
            rows[found].verify_tls == out->subsonic_saved[i].verify_tls) {
            continue;
        }
        subsonic_saved_servers_upsert(out->subsonic_saved[i].url, out->subsonic_saved[i].username,
                                      out->subsonic_saved[i].password, out->subsonic_saved[i].verify_tls);
        wrote = true;
    }

    if (wrote) {
        free(rows);
        rows = NULL;
        n = 0;
        subsonic_saved_servers_load(&rows, &n);
    }

    if (n > 0) {
        int copy = n < SETTINGS_SUBSONIC_SAVED_MAX ? n : SETTINGS_SUBSONIC_SAVED_MAX;
        memset(out->subsonic_saved, 0, sizeof(out->subsonic_saved));
        out->subsonic_saved_count = copy;
        for (int i = 0; i < copy; i++) {
            snprintf(out->subsonic_saved[i].url, sizeof(out->subsonic_saved[i].url), "%s", rows[i].url);
            snprintf(out->subsonic_saved[i].username, sizeof(out->subsonic_saved[i].username), "%s", rows[i].username);
            snprintf(out->subsonic_saved[i].password, sizeof(out->subsonic_saved[i].password), "%s", rows[i].password);
            out->subsonic_saved[i].verify_tls = rows[i].verify_tls;
        }
    }
    free(rows);
}

bool settings_load(player_settings_t * out) {
    set_defaults(out);

    FILE * f = fopen(SETTINGS_FILE_PATH, "r");
    if (!f) f = fopen(SETTINGS_LEGACY_FILE_PATH, "r");
    if (!f) {
        import_legacy_subsonic_servers(out);
        sync_subsonic_saved_sidecar(out);
        return false;
    }

    char line[600];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = '\0';

        char * eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        const char * key = line;
        const char * value = eq + 1;

        if (strcmp(key, "volume") == 0) {
            out->volume = (float) atof(value);
        } else if (strcmp(key, "last_track") == 0) {
            snprintf(out->last_track, sizeof(out->last_track), "%s", value);
        } else if (strcmp(key, "last_position") == 0) {
            out->last_position = atof(value);
        } else if (strcmp(key, "last_source_kind") == 0) {
            out->last_source_kind = atoi(value);
        } else if (strcmp(key, "last_source_name") == 0) {
            snprintf(out->last_source_name, sizeof(out->last_source_name), "%s", value);
        } else if (strcmp(key, "resume_mode") == 0) {
            out->resume_mode = atoi(value);
        } else if (strcmp(key, "play_pause_button_mode") == 0) {
            out->play_pause_button_mode = atoi(value);
        } else if (strcmp(key, "accent_color") == 0) {
            out->accent_color = (uint32_t) strtoul(value, NULL, 16);
        } else if (strcmp(key, "crossfade") == 0) {
            out->crossfade_enabled = (strcmp(value, "1") == 0);
        } else if (strcmp(key, "gapless") == 0) {
            out->gapless_enabled = (strcmp(value, "1") == 0);
        } else if (strcmp(key, "replaygain_enabled") == 0) {
            /* Legacy on/off key, from before the Off/Per Track/Per Album
             * mode selector -- settings_save() only ever writes the new
             * "replaygain_mode" key going forward, so an old config file has
             * this one but never both; if both somehow appear, whichever
             * comes later in the file wins (plain sequential key parsing),
             * same as every other key here. */
            out->replaygain_mode = (strcmp(value, "1") == 0) ? 1 : 0;
        } else if (strcmp(key, "replaygain_mode") == 0) {
            out->replaygain_mode = atoi(value);
        } else if (strcmp(key, "car_mode_enabled") == 0) {
            out->car_mode_enabled = (strcmp(value, "1") == 0);
        } else if (strcmp(key, "inline_remote_enabled") == 0) {
            out->inline_remote_enabled = (strcmp(value, "1") == 0);
        } else if (strcmp(key, "lyrics_enabled") == 0) {
            out->lyrics_enabled = (strcmp(value, "1") == 0);
        } else if (strcmp(key, "subsonic_url") == 0) {
            snprintf(out->subsonic_url, sizeof(out->subsonic_url), "%s", value);
        } else if (strcmp(key, "subsonic_username") == 0) {
            snprintf(out->subsonic_username, sizeof(out->subsonic_username), "%s", value);
        } else if (strcmp(key, "subsonic_password") == 0) {
            snprintf(out->subsonic_password, sizeof(out->subsonic_password), "%s", value);
        } else if (strcmp(key, "subsonic_verify_tls") == 0) {
            out->subsonic_verify_tls = (strcmp(value, "1") == 0);
        } else if (strncmp(key, "subsonic_saved_", 15) == 0) {
            int idx = -1;
            char field[32];
            if (sscanf(key, "subsonic_saved_%d_%31s", &idx, field) == 2 && idx >= 0 &&
                idx < SETTINGS_SUBSONIC_SAVED_MAX) {
                if (idx + 1 > out->subsonic_saved_count) out->subsonic_saved_count = idx + 1;
                if (strcmp(field, "url") == 0) {
                    snprintf(out->subsonic_saved[idx].url, sizeof(out->subsonic_saved[idx].url), "%s", value);
                } else if (strcmp(field, "username") == 0) {
                    snprintf(out->subsonic_saved[idx].username, sizeof(out->subsonic_saved[idx].username), "%s", value);
                } else if (strcmp(field, "password") == 0) {
                    snprintf(out->subsonic_saved[idx].password, sizeof(out->subsonic_saved[idx].password), "%s", value);
                } else if (strcmp(field, "verify_tls") == 0) {
                    out->subsonic_saved[idx].verify_tls = (strcmp(value, "1") == 0);
                }
            }
        } else if (strcmp(key, "bt_volume_sync") == 0) {
            out->bt_volume_sync_enabled = (strcmp(value, "1") == 0);
        } else if (strcmp(key, "bt_device_rate") == 0) {
            /* "AA:BB:CC:DD:EE:FF,44100" */
            char * comma = strchr(value, ',');
            char * rate_end = NULL;
            unsigned long parsed = comma ? strtoul(comma + 1, &rate_end, 10) : 0;
            /* A malformed line is dropped rather than stored: a bad rate here
             * would be pushed straight at the Bluetooth daemon. */
            if (comma && out->bt_device_rate_count < BT_DEVICE_RATE_MAX &&
                    (size_t) (comma - value) == 17 && valid_bt_output_mac(value) &&
                    rate_end && rate_end != comma + 1 && *rate_end == '\0' &&
                    (parsed == 0 || (parsed >= 8000 && parsed <= 192000))) {
                int i = out->bt_device_rate_count++;
                memcpy(out->bt_device_rates[i].mac, value, 17);
                out->bt_device_rates[i].mac[17] = '\0';
                out->bt_device_rates[i].rate = (unsigned int) parsed;
            }
        } else if (strcmp(key, "bt_sample_rate") == 0) {
            out->bt_sample_rate = (unsigned int) strtoul(value, NULL, 10);
        } else if (strcmp(key, "bt_speexrate") == 0) {
            out->bt_speexrate_enabled = (strcmp(value, "1") == 0);
        } else if (strcmp(key, "bt_codec") == 0) {
            snprintf(out->bt_codec, sizeof(out->bt_codec), "%s", value);
        } else if (strcmp(key, "bt_last_output_mac") == 0) {
            if (valid_bt_output_mac(value)) {
                snprintf(out->bt_last_output_mac, sizeof(out->bt_last_output_mac), "%s", value);
            } else {
                out->bt_last_output_mac[0] = '\0';
            }
        } else if (strcmp(key, "bt_hide_unnamed_devices") == 0) {
            out->bt_hide_unnamed_devices = (strcmp(value, "1") == 0);
        } else if (strcmp(key, "wifi_dac_mode") == 0) {
            out->wifi_dac_mode_enabled = (strcmp(value, "1") == 0);
        } else if (strcmp(key, "dlna_renderer_enabled") == 0) {
            out->dlna_renderer_enabled = (strcmp(value, "1") == 0);
        } else if (strcmp(key, "remote_control_enabled") == 0) {
            out->remote_control_enabled = (strcmp(value, "1") == 0);
        } else if (strcmp(key, "remote_control_pin") == 0) {
            if (valid_remote_control_pin(value))
                snprintf(out->remote_control_pin, sizeof(out->remote_control_pin), "%s", value);
        } else if (strcmp(key, "screen_timeout_enabled") == 0) {
            out->screen_timeout_enabled = (strcmp(value, "1") == 0);
        } else if (strcmp(key, "screen_timeout_seconds") == 0) {
            out->screen_timeout_seconds = atoi(value);
        } else if (strcmp(key, "screen_dimming_enabled") == 0) {
            out->screen_dimming_enabled = (strcmp(value, "1") == 0);
        } else if (strcmp(key, "screen_dim_delay_seconds") == 0) {
            out->screen_dim_delay_seconds = atoi(value);
        } else if (strcmp(key, "hide_player_topbar") == 0) {
            out->hide_player_topbar = (strcmp(value, "1") == 0);
        } else if (strcmp(key, "led_indicator_enabled") == 0) {
            out->led_indicator_enabled = (strcmp(value, "1") == 0);
        } else if (strcmp(key, "db_logging_enabled") == 0) {
            out->db_logging_enabled = (strcmp(value, "1") == 0);
        } else if (strcmp(key, "screenshot_combo_enabled") == 0) {
            out->screenshot_combo_enabled = (strcmp(value, "1") == 0);
        } else if (strcmp(key, "dev_bt_dac_all_codecs") == 0) {
            out->dev_bt_dac_all_codecs = (strcmp(value, "1") == 0);
        } else if (strcmp(key, "dev_covers_during_playback") == 0) {
            out->dev_covers_during_playback = (strcmp(value, "1") == 0);
        } else if (strcmp(key, "charge_limiter_enabled") == 0) {
            out->charge_limiter_enabled = (strcmp(value, "1") == 0);
        } else if (strcmp(key, "safe_charging_enabled") == 0) {
            out->safe_charging_enabled = (strcmp(value, "1") == 0);
        } else if (strcmp(key, "show_battery_percent") == 0) {
            out->show_battery_percent = (strcmp(value, "1") == 0);
        } else if (strcmp(key, "idle_shutdown_enabled") == 0) {
            out->idle_shutdown_enabled = (strcmp(value, "1") == 0);
        } else if (strcmp(key, "idle_shutdown_minutes") == 0) {
            out->idle_shutdown_minutes = atoi(value);
        } else if (strcmp(key, "idle_suspend_enabled") == 0) {
            out->idle_suspend_enabled = (strcmp(value, "1") == 0);
        } else if (strcmp(key, "idle_suspend_default_migrated") == 0) {
            out->idle_suspend_default_migrated = (strcmp(value, "1") == 0);
        } else if (strcmp(key, "usb_mode") == 0) {
            out->usb_mode = atoi(value);
        } else if (strcmp(key, "play_mode") == 0) {
            out->play_mode = atoi(value);
        } else if (strcmp(key, "swipe_up_home_enabled") == 0) {
            out->swipe_up_home_enabled = (strcmp(value, "1") == 0);
        } else if (strcmp(key, "startup_volume_fixed_enabled") == 0) {
            out->startup_volume_fixed_enabled = (strcmp(value, "1") == 0);
        } else if (strcmp(key, "startup_volume_fixed_percent") == 0) {
            out->startup_volume_fixed_percent = atoi(value);
        } else if (strcmp(key, "sleep_timer_minutes") == 0) {
            out->sleep_timer_minutes = atoi(value);
        } else if (strcmp(key, "timezone") == 0) {
            snprintf(out->timezone, sizeof(out->timezone), "%s", value);
        } else if (strcmp(key, "hostname") == 0) {
            snprintf(out->hostname, sizeof(out->hostname), "%s", value);
        } else if (strcmp(key, "font_size_tier") == 0) {
            out->font_size_tier = atoi(value);
        } else if (strcmp(key, "lyrics_font_size_tier") == 0) {
            out->lyrics_font_size_tier = atoi(value);
        } else if (strcmp(key, "brightness_percent") == 0) {
            out->brightness_percent = atoi(value);
        } else if (strcmp(key, "clock_24h") == 0) {
            out->clock_24h = (strcmp(value, "1") == 0);
        } else if (strcmp(key, "clock_automatic") == 0) {
            out->clock_automatic = (strcmp(value, "1") == 0);
        } else if (strcmp(key, "clock_manual_epoch") == 0) {
            out->clock_manual_epoch = (int64_t) strtoll(value, NULL, 10);
        } else if (strcmp(key, "clock_system_reference") == 0) {
            out->clock_system_reference = (int64_t) strtoll(value, NULL, 10);
        } else if (strcmp(key, "custom_font") == 0) {
            if (!strchr(value, '/') && !strchr(value, '\\') && !strstr(value, "..")) {
                snprintf(out->custom_font, sizeof(out->custom_font), "%s", value);
            } else {
                out->custom_font[0] = '\0';
            }
        }
    }

    if (out->usb_mode < 0 || out->usb_mode > 2) out->usb_mode = 0; /* defensive re-clamp, same reasoning as screen_timeout_seconds -- the settings file is plaintext and could be hand-edited out of range */
    if (out->play_mode < 0 || out->play_mode > 3) out->play_mode = 0;
    if (out->font_size_tier < 0 || out->font_size_tier > 2) out->font_size_tier = 0;
    if (out->lyrics_font_size_tier != 1 && out->lyrics_font_size_tier != 2) out->lyrics_font_size_tier = 2; /* Medium/Large only, see settings.h */
    if (out->replaygain_mode < 0 || out->replaygain_mode > 2) out->replaygain_mode = 1; /* Off/Per Track/Per Album only, see settings.h */
    if (out->startup_volume_fixed_percent < 0 || out->startup_volume_fixed_percent > 100) out->startup_volume_fixed_percent = 20;
    if (out->brightness_percent < 0 || out->brightness_percent > 100) out->brightness_percent = 80;
    if (out->resume_mode < 0 || out->resume_mode > 2) out->resume_mode = 0;
    if (out->play_pause_button_mode < 0 || out->play_pause_button_mode > 2) out->play_pause_button_mode = 0;

    /* One-time forced migration for installs that already have a
     * settings.txt predating idle_suspend_enabled's default flip (see
     * set_defaults() above) -- settings_save() always writes every field,
     * so an existing file already pins idle_shutdown_enabled/
     * idle_suspend_enabled/idle_shutdown_minutes back to their old
     * false/false/30 values, silently defeating the new default for every
     * existing user rather than just new installs. Same
     * marker-key-in-the-file idiom as playlist_files.c's own
     * PLAYLIST_MIGRATION_MARKER_NAME, just inline here instead of a
     * separate marker file since settings.txt is already a single
     * key=value blob rewritten in full on every save. Runs once: after
     * this, idle_suspend_default_migrated=1 is itself persisted on the next
     * settings_save(), so a user who deliberately turns any of these back
     * off afterward stays off. */
    if (!out->idle_suspend_default_migrated) {
        out->idle_shutdown_enabled = true;
        out->idle_shutdown_minutes = 10;
        out->idle_suspend_enabled = true;
        out->idle_suspend_default_migrated = true;
    }

    out->screen_timeout_seconds = nearest_step(out->screen_timeout_seconds, SCREEN_TIMEOUT_STEPS, SCREEN_TIMEOUT_STEP_COUNT);
    out->screen_dim_delay_seconds = nearest_step(out->screen_dim_delay_seconds, SCREEN_DIM_DELAY_STEPS, SCREEN_DIM_DELAY_STEP_COUNT);
    out->idle_shutdown_minutes = nearest_step(out->idle_shutdown_minutes, IDLE_SHUTDOWN_STEPS, IDLE_SHUTDOWN_STEP_COUNT);
    out->sleep_timer_minutes = nearest_step(out->sleep_timer_minutes, SLEEP_TIMER_STEPS, SLEEP_TIMER_STEP_COUNT);

    import_legacy_subsonic_servers(out);
    sync_subsonic_saved_sidecar(out);

    fclose(f);
    return true;
}

/* Flushes directory metadata changes to flash so the rename survives an unclean
 * shutdown (atomic durable replace: write tmp -> fsync tmp -> rename -> fsync dir). */
static void fsync_settings_dir(void) {
    int dir_fd = open(INTERNAL_COMPAS_DIR, O_RDONLY | O_DIRECTORY);
    if (dir_fd < 0) return;
    fsync(dir_fd);
    close(dir_fd);
}

static void settings_write_file(const player_settings_t * settings) {
    DBG_LOG("settings_save: called (idle_suspend_enabled=%d)\n", settings->idle_suspend_enabled ? 1 : 0);
    (void) mkdir(INTERNAL_COMPAS_DIR, 0755);
    /* This file contains the RC PIN and Subsonic credentials. Create or
     * rewrite the atomic temp file as owner-only even when an older temp
     * file exists with broader permissions. */
    int settings_fd = open(SETTINGS_TMP_FILE_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (settings_fd < 0) return;
    if (fchmod(settings_fd, 0600) != 0) {
        close(settings_fd);
        return;
    }
    FILE * f = fdopen(settings_fd, "w");
    if (!f) {
        close(settings_fd);
        return;
    }

    fprintf(f, "volume=%.3f\n", (double) settings->volume);
    fprintf(f, "last_track=%s\n", settings->last_track);
    fprintf(f, "last_position=%.3f\n", settings->last_position);
    fprintf(f, "last_source_kind=%d\n", settings->last_source_kind);
    fprintf(f, "last_source_name=%s\n", settings->last_source_name);
    fprintf(f, "resume_mode=%d\n", settings->resume_mode);
    fprintf(f, "play_pause_button_mode=%d\n", settings->play_pause_button_mode);
    fprintf(f, "accent_color=%06X\n", (unsigned int) (settings->accent_color & 0xFFFFFF));
    fprintf(f, "crossfade=%d\n", settings->crossfade_enabled ? 1 : 0);
    fprintf(f, "gapless=%d\n", settings->gapless_enabled ? 1 : 0);
    fprintf(f, "replaygain_mode=%d\n", settings->replaygain_mode);
    fprintf(f, "car_mode_enabled=%d\n", settings->car_mode_enabled ? 1 : 0);
    fprintf(f, "inline_remote_enabled=%d\n", settings->inline_remote_enabled ? 1 : 0);
    fprintf(f, "lyrics_enabled=%d\n", settings->lyrics_enabled ? 1 : 0);
    fprintf(f, "subsonic_url=%s\n", settings->subsonic_url);
    fprintf(f, "subsonic_username=%s\n", settings->subsonic_username);
    fprintf(f, "subsonic_password=%s\n", settings->subsonic_password);
    fprintf(f, "subsonic_verify_tls=%d\n", settings->subsonic_verify_tls ? 1 : 0);
    for (int i = 0; i < settings->subsonic_saved_count && i < SETTINGS_SUBSONIC_SAVED_MAX; i++) {
        fprintf(f, "subsonic_saved_%d_url=%s\n", i, settings->subsonic_saved[i].url);
        fprintf(f, "subsonic_saved_%d_username=%s\n", i, settings->subsonic_saved[i].username);
        fprintf(f, "subsonic_saved_%d_password=%s\n", i, settings->subsonic_saved[i].password);
        fprintf(f, "subsonic_saved_%d_verify_tls=%d\n", i, settings->subsonic_saved[i].verify_tls ? 1 : 0);
    }
    fprintf(f, "bt_volume_sync=%d\n", settings->bt_volume_sync_enabled ? 1 : 0);
    fprintf(f, "bt_speexrate=%d\n", settings->bt_speexrate_enabled ? 1 : 0);
    fprintf(f, "bt_sample_rate=%u\n", settings->bt_sample_rate);
    for (int i = 0; i < settings->bt_device_rate_count && i < BT_DEVICE_RATE_MAX; i++)
        fprintf(f, "bt_device_rate=%s,%u\n", settings->bt_device_rates[i].mac,
                settings->bt_device_rates[i].rate);
    fprintf(f, "bt_codec=%s\n", settings->bt_codec);
    fprintf(f, "bt_last_output_mac=%s\n", settings->bt_last_output_mac);
    fprintf(f, "bt_hide_unnamed_devices=%d\n", settings->bt_hide_unnamed_devices ? 1 : 0);
    fprintf(f, "wifi_dac_mode=%d\n", settings->wifi_dac_mode_enabled ? 1 : 0);
    fprintf(f, "dlna_renderer_enabled=%d\n", settings->dlna_renderer_enabled ? 1 : 0);
    fprintf(f, "remote_control_enabled=%d\n", settings->remote_control_enabled ? 1 : 0);
    fprintf(f, "remote_control_pin=%s\n", settings->remote_control_pin);
    fprintf(f, "screen_timeout_enabled=%d\n", settings->screen_timeout_enabled ? 1 : 0);
    fprintf(f, "screen_timeout_seconds=%d\n", settings->screen_timeout_seconds);
    fprintf(f, "screen_dimming_enabled=%d\n", settings->screen_dimming_enabled ? 1 : 0);
    fprintf(f, "screen_dim_delay_seconds=%d\n", settings->screen_dim_delay_seconds);
    fprintf(f, "hide_player_topbar=%d\n", settings->hide_player_topbar ? 1 : 0);
    fprintf(f, "led_indicator_enabled=%d\n", settings->led_indicator_enabled ? 1 : 0);
    fprintf(f, "db_logging_enabled=%d\n", settings->db_logging_enabled ? 1 : 0);
    fprintf(f, "screenshot_combo_enabled=%d\n", settings->screenshot_combo_enabled ? 1 : 0);
    fprintf(f, "dev_bt_dac_all_codecs=%d\n", settings->dev_bt_dac_all_codecs ? 1 : 0);
    fprintf(f, "dev_covers_during_playback=%d\n", settings->dev_covers_during_playback ? 1 : 0);
    fprintf(f, "charge_limiter_enabled=%d\n", settings->charge_limiter_enabled ? 1 : 0);
    fprintf(f, "safe_charging_enabled=%d\n", settings->safe_charging_enabled ? 1 : 0);
    fprintf(f, "show_battery_percent=%d\n", settings->show_battery_percent ? 1 : 0);
    fprintf(f, "idle_shutdown_enabled=%d\n", settings->idle_shutdown_enabled ? 1 : 0);
    fprintf(f, "idle_shutdown_minutes=%d\n", settings->idle_shutdown_minutes);
    fprintf(f, "idle_suspend_enabled=%d\n", settings->idle_suspend_enabled ? 1 : 0);
    fprintf(f, "idle_suspend_default_migrated=%d\n", settings->idle_suspend_default_migrated ? 1 : 0);
    fprintf(f, "usb_mode=%d\n", settings->usb_mode);
    fprintf(f, "play_mode=%d\n", settings->play_mode);
    fprintf(f, "swipe_up_home_enabled=%d\n", settings->swipe_up_home_enabled ? 1 : 0);
    fprintf(f, "startup_volume_fixed_enabled=%d\n", settings->startup_volume_fixed_enabled ? 1 : 0);
    fprintf(f, "startup_volume_fixed_percent=%d\n", settings->startup_volume_fixed_percent);
    fprintf(f, "sleep_timer_minutes=%d\n", settings->sleep_timer_minutes);
    fprintf(f, "timezone=%s\n", settings->timezone);
    fprintf(f, "hostname=%s\n", settings->hostname);
    fprintf(f, "font_size_tier=%d\n", settings->font_size_tier);
    fprintf(f, "lyrics_font_size_tier=%d\n", settings->lyrics_font_size_tier);
    fprintf(f, "brightness_percent=%d\n", settings->brightness_percent);
    fprintf(f, "clock_24h=%d\n", settings->clock_24h ? 1 : 0);
    fprintf(f, "clock_automatic=%d\n", settings->clock_automatic ? 1 : 0);
    fprintf(f, "clock_manual_epoch=%lld\n", (long long) settings->clock_manual_epoch);
    fprintf(f, "clock_system_reference=%lld\n", (long long) settings->clock_system_reference);
    fprintf(f, "custom_font=%s\n", settings->custom_font);

    fflush(f);
    fsync(fileno(f));
    fclose(f);
    if (rename(SETTINGS_TMP_FILE_PATH, SETTINGS_FILE_PATH) == 0) chmod(SETTINGS_FILE_PATH, 0600);
    fsync_settings_dir();
}

/* Both synchronous callers and the coalescing worker share the same tmp
 * file, so writes must be serialized. The generation prevents an older
 * queued snapshot from landing after a newer synchronous save. */
static pthread_mutex_t settings_write_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t settings_queue_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t settings_queue_cond = PTHREAD_COND_INITIALIZER;
static pthread_once_t settings_worker_once = PTHREAD_ONCE_INIT;
static player_settings_t settings_queued_snapshot;
static uint64_t settings_save_generation = 0;
static uint64_t settings_queued_generation = 0;
static bool settings_save_pending = false;
static bool settings_worker_ready = false;

/* Rate remembered for one accessory, falling back to the global choice when
 * that accessory has never been given one. */
unsigned int settings_bt_rate_for(const player_settings_t * settings, const char * mac) {
    if (settings && mac && mac[0])
        for (int i = 0; i < settings->bt_device_rate_count && i < BT_DEVICE_RATE_MAX; i++)
            if (strcasecmp(settings->bt_device_rates[i].mac, mac) == 0)
                return settings->bt_device_rates[i].rate;
    return settings ? settings->bt_sample_rate : 0;
}

void settings_bt_set_rate_for(player_settings_t * settings, const char * mac, unsigned int rate) {
    if (!settings || !mac || !mac[0]) return;
    for (int i = 0; i < settings->bt_device_rate_count && i < BT_DEVICE_RATE_MAX; i++)
        if (strcasecmp(settings->bt_device_rates[i].mac, mac) == 0) {
            settings->bt_device_rates[i].rate = rate;
            return;
        }
    if (settings->bt_device_rate_count < BT_DEVICE_RATE_MAX) {
        int i = settings->bt_device_rate_count++;
        snprintf(settings->bt_device_rates[i].mac, sizeof(settings->bt_device_rates[i].mac), "%s", mac);
        settings->bt_device_rates[i].rate = rate;
        return;
    }
    /* Full: drop the oldest so a newly used accessory is still remembered. */
    memmove(&settings->bt_device_rates[0], &settings->bt_device_rates[1],
            sizeof(settings->bt_device_rates[0]) * (BT_DEVICE_RATE_MAX - 1));
    int last = BT_DEVICE_RATE_MAX - 1;
    snprintf(settings->bt_device_rates[last].mac, sizeof(settings->bt_device_rates[last].mac), "%s", mac);
    settings->bt_device_rates[last].rate = rate;
}

void settings_save(const player_settings_t * settings) {
    if (!settings) return;
    pthread_mutex_lock(&settings_write_mutex);
    pthread_mutex_lock(&settings_queue_mutex);
    settings_save_generation++;
    pthread_mutex_unlock(&settings_queue_mutex);
    settings_write_file(settings);
    pthread_mutex_unlock(&settings_write_mutex);
}

static void * settings_worker_main(void * unused) {
    (void) unused;
    for (;;) {
        pthread_mutex_lock(&settings_queue_mutex);
        while (!settings_save_pending)
            pthread_cond_wait(&settings_queue_cond, &settings_queue_mutex);
        player_settings_t snapshot = settings_queued_snapshot;
        uint64_t generation = settings_queued_generation;
        settings_save_pending = false;
        pthread_mutex_unlock(&settings_queue_mutex);

        pthread_mutex_lock(&settings_write_mutex);
        pthread_mutex_lock(&settings_queue_mutex);
        bool current = generation == settings_save_generation;
        pthread_mutex_unlock(&settings_queue_mutex);
        if (current) settings_write_file(&snapshot);
        pthread_mutex_unlock(&settings_write_mutex);
    }
    return NULL;
}

static void settings_start_worker(void) {
    pthread_t thread;
    if (pthread_create(&thread, NULL, settings_worker_main, NULL) == 0) {
        pthread_detach(thread);
        settings_worker_ready = true;
    }
}

void settings_save_async(const player_settings_t * settings) {
    if (!settings) return;
    pthread_once(&settings_worker_once, settings_start_worker);
    if (!settings_worker_ready) {
        settings_save(settings);
        return;
    }

    pthread_mutex_lock(&settings_queue_mutex);
    settings_queued_snapshot = *settings;
    settings_queued_generation = ++settings_save_generation;
    settings_save_pending = true;
    pthread_cond_signal(&settings_queue_cond);
    pthread_mutex_unlock(&settings_queue_mutex);
}

void settings_factory_reset(void) {
    DBG_LOG("settings_factory_reset: called\n");
#ifndef HOST_BUILD
    /* Wipes every direct child of /usr/data except "mnt", preserving the physical
     * SD card mount (/usr/data/mnt/sd_0) while clearing all other configuration files. */
    DIR * dir = opendir(SETTINGS_DIR_PATH);
    if (dir) {
        struct dirent * entry;
        while ((entry = readdir(dir)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0 ||
                strcmp(entry->d_name, "mnt") == 0) {
                continue;
            }
            char path[600];
            snprintf(path, sizeof(path), "%s/%s", SETTINGS_DIR_PATH, entry->d_name);
            DBG_LOG("settings_factory_reset: rm -rf %s\n", path);
            char * rm_argv[] = { (char *) "/bin/rm", (char *) "-rf", path, NULL };
            subprocess_run(rm_argv, NULL, 0);
        }
        closedir(dir);
    } else {
        DBG_LOG("settings_factory_reset: opendir(%s) failed, errno=%d\n", SETTINGS_DIR_PATH, errno);
    }
#endif
    /* Belt-and-suspenders: the loop above already covers these two (both
     * direct children of SETTINGS_DIR_PATH), but keeping the explicit
     * removes means this function still does SOMETHING sane on a host
     * build (SETTINGS_DIR_PATH is "." there, deliberately not wiped
     * wholesale -- see #ifndef HOST_BUILD above) and if opendir() above
     * ever failed. */
    remove(SETTINGS_FILE_PATH);
    remove(SETTINGS_TMP_FILE_PATH); /* stray leftover from an interrupted settings_save(), if any -- harmless to attempt even when it doesn't exist */

#ifndef HOST_BUILD
    /* Not execl("/sbin/reboot", ...): compas_bootloader's
     * run_player_supervised() (src/bootloader/main.c) treats ANY clean
     * (status 0) exit of this exact supervised PID as "player exited
     * cleanly -- power off", regardless of which command replaced this
     * process's image. /sbin/reboot typically exits 0 once it hands off to
     * init, well before the actual kernel restart completes, so the
     * bootloader's poweroff-on-clean-exit races ahead of and wins over the
     * real reboot -- this was the reported "factory reset just shuts down
     * instead of rebooting" bug. Calling reboot(2) directly, same as
     * main.c's own reboot_device() already does for every other abnormal-
     * exit reboot, restarts the kernel immediately from within this
     * process instead of handing off through an external command whose
     * exit status the supervisor can misread. */
    sync();
    reboot(RB_AUTOBOOT);
    for (;;) pause();
#endif
}
