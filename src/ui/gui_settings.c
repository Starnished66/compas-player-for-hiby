#include "gui.h"
#include "gui_settings.h"
#include "hw_buttons.h"
#include "gui_library.h"
#include "bluetooth_control.h"
#include "gui_shell.h"
#include "gui_text_input.h"
#include "app_clock.h"
#include "app_version.h"
#include <math.h>
#include <sys/stat.h>
#include "gui_subsonic.h"
#include "gui_books.h"
#include "gui_network.h"
#include "subprocess.h"
#include "timezone_apply.h"
#include "gui_lyrics.h"
#include "screen_builders.h"
#include "settings.h"
#include "tagcache.h"
#include "metadata_db.h"
#include "assets.h"
#include "metadata.h"
#include "audio.h"
#include "peq.h"
#include "device_config.h"
#include "usb_mode_control.h"
#include "timezone_data.h"
#include "firmware_update.h"
#include "plugin_manager.h"
#include "gui_plugin_manage.h"
#include "fallback_font.h"
#include "gui_navigation.h"
#include "db_log.h"
#include "usb_dac_bridge.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "backlight.h"
#ifdef HOST_BUILD
  #define MUSIC_ROOT_DIR "./music"
#else
  #define MUSIC_ROOT_DIR "/data/mnt/sd_0"
#endif
#define PEQ_PROFILES_DIR MUSIC_ROOT_DIR "/PEQ_Profiles"
#include <stdatomic.h>
#include <pthread.h>

/* Extern references to screen pointers owned by this module (defined here) */
static lv_obj_t * settings_crossfade_toggle_img = NULL;
static lv_obj_t * settings_gapless_toggle_img = NULL;
static lv_obj_t * settings_screen;
static lv_obj_t * settings_music_screen;
static lv_obj_t * music_playback_screen;
static lv_obj_t * music_audio_screen;
static lv_obj_t * music_controls_screen;
static lv_obj_t * music_timers_screen;
static lv_obj_t * music_database_screen;
static lv_obj_t * music_library_screen;
static lv_obj_t * settings_display_screen;
static lv_obj_t * settings_power_screen;
static lv_obj_t * settings_system_screen;
static lv_obj_t * about_screen;
static lv_obj_t * buy_me_a_coffee_screen;
static lv_obj_t * dev_options_screen;
static lv_obj_t * accent_color_screen;
static lv_obj_t * custom_font_screen;
static lv_obj_t * screen_timeout_screen;
static lv_obj_t * screen_dimming_screen;
static lv_obj_t * screen_dimming_switch;
static lv_obj_t * screen_dimming_slider_card;
static lv_obj_t * screen_dimming_slider;
static lv_obj_t * screen_dimming_value_label;
static lv_obj_t * startup_volume_screen;
static lv_obj_t * sleep_timer_screen;
static lv_obj_t * idle_shutdown_screen;
static lv_obj_t * clock_screen;
static lv_obj_t * clock_set_time_screen;
static lv_obj_t * clock_hour_roller;
static lv_obj_t * clock_minute_roller;
static lv_obj_t * clock_ampm_roller;
static lv_obj_t * clock_set_time_row;
static lv_obj_t * clock_timezone_row;
static lv_obj_t * clock_timezone_value_label;
static lv_obj_t * eq_screen;
static lv_obj_t * eq_profiles_screen;

/* Externs to gui.c functions and state this module needs */
extern lv_obj_t * gui_library_get_music_screen();
extern lv_obj_t * stream_media_screen;
extern lv_obj_t * gui_network_get_wireless_screen();
extern lv_obj_t * gui_network_get_usb_dac_overlay();
extern lv_obj_t * gui_shell_get_dac_home_screen();
extern player_settings_t current_settings;
extern void nav_push(lv_obj_t * screen);
extern void nav_pop(void);
extern void finalize_screen_navigation(lv_obj_t * screen);
extern void show_error_toast(const char * msg);
extern void show_info_toast(const char * msg);
extern lv_color_t accent_lv_color(void);
extern lv_obj_t * add_pill_chevron_row(lv_obj_t * list, const char * text, lv_event_cb_t cb);
extern lv_obj_t * add_pill_toggle_row(lv_obj_t * parent, const char * label_text, bool checked, lv_event_cb_t on_click);
extern lv_obj_t * add_pill_row_base(lv_obj_t * list, const char * text);
extern const lv_font_t * gui_theme_font(gui_font_role_t role);
extern void reserve_title_width_before(lv_obj_t * title, lv_obj_t * right_icon);
extern void generic_back_cb(lv_event_t * e);
extern void start_library_rescan(void);
extern void show_library_refresh_all_metadata_prompt(void);
extern void show_library_refresh_all_covers_prompt(void);

static lv_obj_t * screen_timeout_switch;
static lv_obj_t * screen_timeout_slider_card;
static lv_obj_t * screen_timeout_value_label;
static lv_obj_t * screen_timeout_slider;
static lv_obj_t * startup_volume_switch;
static lv_obj_t * startup_volume_slider_card;
static lv_obj_t * startup_volume_value_label;
static lv_obj_t * startup_volume_slider;
static lv_obj_t * sleep_timer_switch;
static lv_obj_t * sleep_timer_slider_card;
static lv_obj_t * sleep_timer_remaining_btn;
static lv_obj_t * sleep_timer_value_label;
static lv_obj_t * sleep_timer_slider;
static lv_obj_t * idle_shutdown_switch;
static lv_obj_t * idle_shutdown_slider_card;
static lv_obj_t * idle_shutdown_value_label;
static lv_obj_t * idle_shutdown_slider;
static lv_obj_t * idle_action_section;
static lv_obj_t * idle_action_poweroff_row;
static lv_obj_t * idle_action_suspend_row;
static lv_obj_t * eq_bypass_switch;
static lv_obj_t * eq_band_dropdown;
static lv_obj_t * eq_band_enabled_switch;
static lv_obj_t * eq_preamp_slider;
static lv_obj_t * eq_preamp_value_label;
static lv_obj_t * eq_freq_slider;
static lv_obj_t * eq_gain_slider;
static lv_obj_t * eq_q_slider;
static lv_obj_t * eq_type_dropdown;
static lv_obj_t * eq_freq_value_label;
static lv_obj_t * eq_gain_value_label;
static lv_obj_t * eq_q_value_label;
static int current_eq_band = 0;

void gui_settings_refresh_font_geometry(void) {
    int32_t width = 170;
    if (current_settings.font_size_tier == 1) width = 200;
    else if (current_settings.font_size_tier == 2) width = 240;
    if (eq_band_dropdown) lv_obj_set_width(eq_band_dropdown, width);
    if (eq_type_dropdown) lv_obj_set_width(eq_type_dropdown, width);
    if (eq_screen) lv_obj_update_layout(eq_screen);
}

#define EQ_FREQ_MIN_HZ 20.0
#define EQ_FREQ_MAX_HZ 20000.0
#define EQ_FREQ_SLIDER_MAX 1000

static int32_t freq_to_slider(double freq_hz) {
    if (freq_hz < EQ_FREQ_MIN_HZ) freq_hz = EQ_FREQ_MIN_HZ;
    if (freq_hz > EQ_FREQ_MAX_HZ) freq_hz = EQ_FREQ_MAX_HZ;
    double ratio = log(freq_hz / EQ_FREQ_MIN_HZ) / log(EQ_FREQ_MAX_HZ / EQ_FREQ_MIN_HZ);
    return (int32_t) (ratio * EQ_FREQ_SLIDER_MAX);
}

static double slider_to_freq(int32_t slider_val) {
    double ratio = (double) slider_val / (double) EQ_FREQ_SLIDER_MAX;
    return EQ_FREQ_MIN_HZ * pow(EQ_FREQ_MAX_HZ / EQ_FREQ_MIN_HZ, ratio);
}

/* Which numeric field a tap-to-edit label represents -- passed through
 * show_text_entry()'s user_data so one shared done-callback can update the
 * right slider/label/peq setter instead of needing four near-identical
 * callbacks. */
typedef enum {
    EQ_FIELD_PREAMP,
    EQ_FIELD_FREQ,
    EQ_FIELD_GAIN,
    EQ_FIELD_Q,
} eq_field_t;

static void eq_numeric_entry_done_cb(const char * text, void * user_data) {
    eq_field_t field = (eq_field_t) (intptr_t) user_data;
    double val = atof(text);
    const peq_band_t * band;

    switch (field) {
        case EQ_FIELD_PREAMP:
            if (val < -12.0) val = -12.0;
            if (val > 12.0) val = 12.0;
            peq_set_preamp_db(val);
            lv_slider_set_value(eq_preamp_slider, (int32_t) (val * 10.0), LV_ANIM_OFF);
            lv_label_set_text_fmt(eq_preamp_value_label, "Pre-Amp: %+.2f dB", val);
            peq_save();
            break;
        case EQ_FIELD_FREQ:
            band = peq_get_band(current_eq_band);
            if (!band) break;
            if (val < EQ_FREQ_MIN_HZ) val = EQ_FREQ_MIN_HZ;
            if (val > EQ_FREQ_MAX_HZ) val = EQ_FREQ_MAX_HZ;
            peq_set_band(current_eq_band, val, band->gain_db, band->q);
            lv_slider_set_value(eq_freq_slider, freq_to_slider(val), LV_ANIM_OFF);
            lv_label_set_text_fmt(eq_freq_value_label, "Frequency: %.0f Hz", val);
            peq_save();
            break;
        case EQ_FIELD_GAIN:
            band = peq_get_band(current_eq_band);
            if (!band) break;
            if (val < -12.0) val = -12.0;
            if (val > 12.0) val = 12.0;
            peq_set_band(current_eq_band, band->freq_hz, val, band->q);
            lv_slider_set_value(eq_gain_slider, (int32_t) (val * 10.0), LV_ANIM_OFF);
            lv_label_set_text_fmt(eq_gain_value_label, "Gain: %+.2f dB", val);
            peq_save();
            break;
        case EQ_FIELD_Q:
            band = peq_get_band(current_eq_band);
            if (!band) break;
            if (val < 0.1) val = 0.1;
            if (val > 10.0) val = 10.0;
            peq_set_band(current_eq_band, band->freq_hz, band->gain_db, val);
            lv_slider_set_value(eq_q_slider, (int32_t) (val * 10.0), LV_ANIM_OFF);
            lv_label_set_text_fmt(eq_q_value_label, "Q: %.2f", val);
            peq_save();
            break;
    }
}

/* Tap-to-edit: attached to each slider card's value label (see
 * create_eq_slider_card()) -- opens the shared numeric keypad pre-filled
 * with the field's current exact value, for typing a precise number
 * instead of only dragging a slider. */
static void eq_field_label_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    eq_field_t field = (eq_field_t) (intptr_t) lv_event_get_user_data(e);
    const peq_band_t * band;
    char buf[32];
    const char * title;

    switch (field) {
        case EQ_FIELD_PREAMP:
            title = "Pre-Amp (dB, -12 to 12)";
            snprintf(buf, sizeof(buf), "%.2f", peq_get_preamp_db());
            break;
        case EQ_FIELD_FREQ:
            title = "Frequency (Hz, 20 to 20000)";
            band = peq_get_band(current_eq_band);
            snprintf(buf, sizeof(buf), "%.0f", band ? band->freq_hz : 0.0);
            break;
        case EQ_FIELD_GAIN:
            title = "Gain (dB, -12 to 12)";
            band = peq_get_band(current_eq_band);
            snprintf(buf, sizeof(buf), "%.2f", band ? band->gain_db : 0.0);
            break;
        case EQ_FIELD_Q:
        default:
            title = "Q (0.1 to 10)";
            band = peq_get_band(current_eq_band);
            snprintf(buf, sizeof(buf), "%.2f", band ? band->q : 0.0);
            break;
    }

    show_text_entry(title, buf, false, true, eq_numeric_entry_done_cb, (void *) (intptr_t) field);
}

static void eq_screen_btn_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(eq_screen);
}

static void eq_bypass_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    /* Switch shows "EQ Enabled" -- checked means NOT bypassed. */
    peq_set_bypass(!lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
    peq_save();
}

static void refresh_eq_band_widgets(void) {
    const peq_band_t * band = peq_get_band(current_eq_band);
    if (!band) return;

    lv_dropdown_set_selected(eq_band_dropdown, (uint32_t) current_eq_band);

    if (band->enabled) lv_obj_add_state(eq_band_enabled_switch, LV_STATE_CHECKED);
    else lv_obj_clear_state(eq_band_enabled_switch, LV_STATE_CHECKED);

    lv_dropdown_set_selected(eq_type_dropdown, (uint32_t) band->type);

    lv_slider_set_value(eq_freq_slider, freq_to_slider(band->freq_hz), LV_ANIM_OFF);
    lv_slider_set_value(eq_gain_slider, (int32_t) (band->gain_db * 10.0), LV_ANIM_OFF);
    lv_slider_set_value(eq_q_slider, (int32_t) (band->q * 10.0), LV_ANIM_OFF);

    lv_label_set_text_fmt(eq_freq_value_label, "Frequency: %.0f Hz", band->freq_hz);
    lv_label_set_text_fmt(eq_gain_value_label, "Gain: %+.2f dB", band->gain_db);
    lv_label_set_text_fmt(eq_q_value_label, "Q: %.2f", band->q);
}

static void eq_band_dropdown_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    current_eq_band = (int) lv_dropdown_get_selected(lv_event_get_target(e));
    refresh_eq_band_widgets();
}

static void eq_type_dropdown_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    peq_set_band_type(current_eq_band, (peq_band_type_t) lv_dropdown_get_selected(lv_event_get_target(e)));
    peq_save();
}

static void eq_preamp_slider_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    double db = (double) lv_slider_get_value(lv_event_get_target(e)) / 10.0;

    if (code == LV_EVENT_VALUE_CHANGED) {
        peq_set_preamp_db(db);
        lv_label_set_text_fmt(eq_preamp_value_label, "Pre-Amp: %+.2f dB", db);
    } else if (code == LV_EVENT_RELEASED) {
        peq_save();
    }
}

static void eq_band_enabled_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    peq_set_band_enabled(current_eq_band, lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
    peq_save();
}

static void eq_freq_slider_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    const peq_band_t * band = peq_get_band(current_eq_band);
    if (!band) return;
    double freq = slider_to_freq(lv_slider_get_value(lv_event_get_target(e)));

    /* Format string with label prefix consistently during active dragging
     * and when released. */
    if (code == LV_EVENT_VALUE_CHANGED) {
        peq_set_band(current_eq_band, freq, band->gain_db, band->q);
        lv_label_set_text_fmt(eq_freq_value_label, "Frequency: %.0f Hz", freq);
    } else if (code == LV_EVENT_RELEASED) {
        peq_save();
    }
}

static void eq_gain_slider_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    const peq_band_t * band = peq_get_band(current_eq_band);
    if (!band) return;
    double gain = (double) lv_slider_get_value(lv_event_get_target(e)) / 10.0;

    if (code == LV_EVENT_VALUE_CHANGED) {
        peq_set_band(current_eq_band, band->freq_hz, gain, band->q);
        lv_label_set_text_fmt(eq_gain_value_label, "Gain: %+.2f dB", gain);
    } else if (code == LV_EVENT_RELEASED) {
        peq_save();
    }
}

static void eq_q_slider_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    const peq_band_t * band = peq_get_band(current_eq_band);
    if (!band) return;
    double q = (double) lv_slider_get_value(lv_event_get_target(e)) / 10.0;

    if (code == LV_EVENT_VALUE_CHANGED) {
        peq_set_band(current_eq_band, band->freq_hz, band->gain_db, q);
        lv_label_set_text_fmt(eq_q_value_label, "Q: %.2f", q);
    } else if (code == LV_EVENT_RELEASED) {
        peq_save();
    }
}

static gui_popup_t firmware_update_popup;
static lv_obj_t * firmware_update_popup_title;

static void hide_firmware_update_popup(void) {
    gui_popup_hide(&firmware_update_popup);
}

static void firmware_update_popup_backdrop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_firmware_update_popup();
}

static void firmware_update_cancel_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_firmware_update_popup();
}

static void firmware_update_confirm_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_firmware_update_popup();
    firmware_update_enter_recovery();
}

static void build_firmware_update_popup(void) {
    firmware_update_popup.popup = build_confirm_popup(
        "", LV_LABEL_LONG_WRAP, &firmware_update_popup_title, NULL, "Update & Reboot", lv_color_make(255, 120, 120),
        firmware_update_confirm_cb, NULL, "Cancel", accent_lv_color(), firmware_update_cancel_cb, NULL,
        firmware_update_popup_backdrop_cb, &firmware_update_popup.backdrop);
}

void firmware_update_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    char path[512];
    if (!firmware_update_scan(path, sizeof(path))) {
        show_error_toast("No .upt firmware file found on SD card");
        return;
    }

    const char * filename = strrchr(path, '/');
    filename = filename ? filename + 1 : path;
    lv_label_set_text_fmt(firmware_update_popup_title, "Update using %s?\nDevice will reboot into recovery mode.",
                           filename);

    gui_popup_show(&firmware_update_popup);
}

static lv_obj_t * adb_switch = NULL;

/* The Developer Options screen is built once and cached, so its ADB row would
 * otherwise keep showing whatever the state was at build time. Called when the
 * screen opens, and again by poll_usb_mode_switch() once the gadget settles so
 * a switch that failed does not leave the toggle promising ADB. */
void gui_settings_sync_adb_toggle(void) {
    if (!adb_switch) return;
    if (gui_network_adb_active()) lv_obj_add_state(adb_switch, LV_STATE_CHECKED);
    else lv_obj_clear_state(adb_switch, LV_STATE_CHECKED);
}

static void adb_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    /* Turning ADB off falls back to Storage: the stock default, and the safer
     * of the other two since DAC takes over the whole screen with its own
     * overlay rather than just quietly changing what the USB port does. */
    bool want_adb = !gui_network_adb_active();
    if (!start_usb_mode_switch(want_adb ? USB_MODE_ADB : USB_MODE_STORAGE)) {
        /* Request dropped because a switch is already in flight. Put the
         * switch back rather than leaving it showing a state nothing is
         * working toward. */
        gui_settings_sync_adb_toggle();
        return;
    }
    /* Accepted: the switch already shows the requested state, and applying it
     * takes seconds. Leave it optimistically flipped; poll_usb_mode_switch()
     * confirms or rolls it back. */
}

static void dev_options_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    gui_settings_sync_adb_toggle();
    nav_push(dev_options_screen);
}

static const char PAYPAL_DONATION_URL[] =
    "https://www.paypal.com/cgi-bin/webscr?cmd=_donations&business=josegarita%40protonmail.com&currency_code=USD";

static void buy_me_a_coffee_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(buy_me_a_coffee_screen);
}

static lv_obj_t * build_buy_me_a_coffee_screen(void) {
    lv_obj_t * title_label;
    lv_obj_t * list;
    lv_obj_t * scr = build_subsonic_list_screen("Buy Me a Coffee", &title_label, &list);

    lv_obj_t * qrcode = lv_qrcode_create(list);
    lv_qrcode_set_size(qrcode, BOARD_SCALE_PX(320));
    lv_qrcode_set_dark_color(qrcode, lv_color_black());
    lv_qrcode_set_light_color(qrcode, lv_color_white());
    lv_qrcode_set_quiet_zone(qrcode, true);
    lv_obj_set_style_border_width(qrcode, 4, 0);
    lv_obj_set_style_border_color(qrcode, lv_color_white(), 0);
    lv_qrcode_update(qrcode, PAYPAL_DONATION_URL, strlen(PAYPAL_DONATION_URL));

    lv_obj_t * caption = lv_label_create(list);
    lv_label_set_text(caption, "Scan with your phone to support Compás Player on PayPal");
    lv_obj_set_width(caption, lv_pct(90));
    lv_label_set_long_mode(caption, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(caption, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_style(caption, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(caption, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_obj_set_style_pad_top(caption, BOARD_SCALE_PX(12), 0);
    lv_obj_set_style_pad_bottom(caption, BOARD_SCALE_PX(20), 0);

    /* The QR and its caption fit within both 800px and 720px panels. Center
     * the pair as a group so the shorter R3 Pro II screen keeps equal space
     * above and below it without requiring a scroll. */
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return scr;
}

static lv_obj_t * build_about_screen(void) {
    static pill_list_item_t items[5];
    items[0] = (pill_list_item_t){ "Compás Player", PILL_ACCESSORY_NONE, false, NULL, NULL, NULL };
    items[1] = (pill_list_item_t){ app_version_label(), PILL_ACCESSORY_NONE, false, NULL, NULL, NULL };
    items[2] = (pill_list_item_t){ "Buy Me a Coffee", PILL_ACCESSORY_CHEVRON, false,
                                    buy_me_a_coffee_row_cb, NULL, NULL };
    items[3] =
        (pill_list_item_t){ "Firmware Update", PILL_ACCESSORY_CHEVRON, false, firmware_update_row_cb, NULL, NULL };
    items[4] =
        (pill_list_item_t){ "Developer Options", PILL_ACCESSORY_CHEVRON, false, dev_options_row_cb, NULL, NULL };
    lv_obj_t * scr = build_pill_list_screen("About", generic_back_cb, items, 5, gui_theme_accent_style(), GUI_ROW_GAP, 100);
    finalize_screen_navigation(scr);
    return scr;
}

static void db_logging_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    current_settings.db_logging_enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    settings_save(&current_settings);
    db_log_set_enabled(current_settings.db_logging_enabled);
    usb_dac_bridge_set_debug_log_enabled(current_settings.db_logging_enabled);
}

/* Writes a detailed timestamped log of library database scans and album art
 * cache jobs to .logs/database_artwork.log, and USB DAC bridge diagnostics to
 * .logs/usb_dac_bridge.log on the SD card -- see db_log.h and usb_dac_bridge.h. */
static void screenshot_combo_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    current_settings.screenshot_combo_enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    settings_save(&current_settings);
    hw_buttons_set_screenshot_combo_enabled(current_settings.screenshot_combo_enabled);
}

static void dev_bt_dac_all_codecs_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    current_settings.dev_bt_dac_all_codecs = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    settings_save(&current_settings);
    bt_control_set_dac_all_codecs(current_settings.dev_bt_dac_all_codecs);
}

static void dev_covers_during_playback_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    current_settings.dev_covers_during_playback = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    settings_save(&current_settings);
    gui_library_set_covers_during_playback(current_settings.dev_covers_during_playback);
}

static lv_obj_t * build_dev_options_screen(void) {
    static pill_list_item_t items[5];
    /* ADB lives here rather than on the USB Mode screen: it overrides
     * Storage/DAC while on and persists across a reboot, so it sits behind
     * Developer Options as the explicit opt-in that makes re-applying it on
     * startup reasonable. The USB Mode screen still dims the other modes
     * while it owns the port. */
    items[0] = (pill_list_item_t){ "ADB", PILL_ACCESSORY_TOGGLE, gui_network_adb_active(),
                                    NULL, adb_switch_event_cb, NULL, &adb_switch };
    items[1] = (pill_list_item_t){ "Enable debug logging", PILL_ACCESSORY_TOGGLE,
                                    current_settings.db_logging_enabled, NULL, db_logging_switch_event_cb, NULL };
    items[2] = (pill_list_item_t){ "Screenshots (Power + Vol Down)", PILL_ACCESSORY_TOGGLE,
                                    current_settings.screenshot_combo_enabled, NULL,
                                    screenshot_combo_switch_event_cb, NULL };
    /* Experimental: off by default, see settings.h. The DAC codec switch
     * applies the next time DAC mode starts. */
    items[3] = (pill_list_item_t){ "LDAC in DAC mode (Experimental)", PILL_ACCESSORY_TOGGLE,
                                    current_settings.dev_bt_dac_all_codecs, NULL,
                                    dev_bt_dac_all_codecs_switch_event_cb, NULL };
    items[4] = (pill_list_item_t){ "Load covers during playback (Experimental)", PILL_ACCESSORY_TOGGLE,
                                    current_settings.dev_covers_during_playback, NULL,
                                    dev_covers_during_playback_switch_event_cb, NULL };
    lv_obj_t * scr = build_pill_list_screen("Developer Options", generic_back_cb, items, 5, gui_theme_accent_style(), GUI_ROW_GAP, 100);
    finalize_screen_navigation(scr);
    return scr;
}

static lv_obj_t * build_accent_color_screen(void) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_add_style(scr, &style_theme_screen_bg, 0);

    build_screen_header(scr, "Accent Color", generic_back_cb, NULL, NULL);

    /* 16-swatch color palette displayed in a wrapping flex container with
     * generous touch targets and spacing. */
    lv_obj_t * swatch_row = lv_obj_create(scr);
    lv_obj_set_size(swatch_row, lv_pct(92), LV_SIZE_CONTENT);
    lv_obj_align(swatch_row, LV_ALIGN_TOP_MID, 0, STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT + 20);
    lv_obj_set_style_bg_opa(swatch_row, 0, 0);
    lv_obj_set_style_border_width(swatch_row, 0, 0);
    lv_obj_remove_flag(swatch_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(swatch_row, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(swatch_row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(swatch_row, 20, 0);
    lv_obj_set_style_pad_column(swatch_row, 16, 0);

    for (size_t i = 0; i < ACCENT_PALETTE_COUNT; i++) {
        lv_obj_t * swatch = lv_obj_create(swatch_row);
        lv_obj_set_size(swatch, BOARD_SCALE_PX(64), BOARD_SCALE_PX(64));
        lv_obj_set_style_radius(swatch, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(swatch, lv_color_hex(accent_palette[i]), 0);
        lv_obj_set_style_border_width(swatch, current_settings.accent_color == accent_palette[i] ? 4 : 0, 0);
        lv_obj_set_style_border_color(swatch, lv_color_make(255, 255, 255), 0);
        lv_obj_add_flag(swatch, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(swatch, accent_swatch_event_cb, LV_EVENT_CLICKED, (void *) (intptr_t) accent_palette[i]);
        gui_theme_register_accent_swatch(i, swatch);
    }

    finalize_screen_navigation(scr);
    return scr;
}

static void accent_color_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(accent_color_screen);
}

static lv_obj_t * custom_font_list = NULL;
static lv_obj_t * custom_font_preview_latin = NULL;
static lv_obj_t * custom_font_preview_note = NULL;
static char discovered_custom_fonts[MAX_CUSTOM_FONTS_DISCOVERED][64];
static int discovered_custom_font_count = 0;

static void custom_font_option_cb(lv_event_t * e);
static void custom_font_apply_timer_cb(lv_timer_t * timer);

static void populate_custom_font_screen(void) {
    if (!custom_font_list) return;
    lv_obj_clean(custom_font_list);

    discovered_custom_font_count = fallback_font_discover_custom(discovered_custom_fonts, MAX_CUSTOM_FONTS_DISCOVERED);

    const char * active_name = fallback_font_get_custom_name();

    /* 1. Default (built-in Montserrat) option */
    bool default_selected = (strcmp(active_name, "Default") == 0 || !current_settings.custom_font[0]);
    add_pill_option_row(custom_font_list, "Default (Built-in)",
                        default_selected, custom_font_option_cb, (void *) (intptr_t) -1);

    /* 2. Discovered fonts from <SD>/Fonts */
    for (int i = 0; i < discovered_custom_font_count; i++) {
        bool selected = (strcmp(active_name, discovered_custom_fonts[i]) == 0);
        add_pill_option_row(custom_font_list, discovered_custom_fonts[i],
                            selected, custom_font_option_cb, (void *) (intptr_t) i);
    }

    if (discovered_custom_font_count == 0) {
        lv_obj_t * empty_note = lv_label_create(custom_font_list);
        lv_label_set_text(empty_note, "No .ttf fonts found in /Fonts");
        lv_obj_add_style(empty_note, &style_theme_text_muted, 0);
        lv_obj_set_style_text_font(empty_note, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
        lv_obj_set_style_pad_top(empty_note, 12, 0);
    }
}

static void custom_font_apply_timer_cb(lv_timer_t * timer) {
    lv_obj_t * mask = (lv_obj_t *)lv_timer_get_user_data(timer);
    int index = (int)(intptr_t)lv_obj_get_user_data(mask) - 2;
    lv_timer_delete(timer);

    if (index < -1 || index >= discovered_custom_font_count) {
        lv_obj_delete(mask);
        show_error_toast("Font selection is no longer available");
        return;
    }
    const char * target = index < 0 ? "Default" : discovered_custom_fonts[index];
    if (!fallback_font_apply_custom(target)) {
        lv_obj_delete(mask);
        show_error_toast("Failed to load font. Check format & memory.");
        return;
    }

    /* The stable app_font_* addresses now contain the candidate descriptors.
     * Perform the same bounded, one-shot refresh as live Font Size while the
     * black input mask is still covering the display.  Custom Font also
     * changes the Latin face of app_font_lyrics (but not its independent
     * size), so its existing layout gets one explicit refresh here. */
    gui_navigation_invalidate_font_snapshots();
    snprintf(current_settings.custom_font, sizeof(current_settings.custom_font), "%s",
             index < 0 ? "" : target);
    settings_save(&current_settings);
    lv_obj_report_style_change(NULL);
    /* Same full-display walk as font_size_apply_timer_cb() -- Custom Font
     * also rewrites app_font_* metrics in place, so unvisited Settings
     * submenus need their USER_3 label boxes recomputed too. */
    screen_builders_refresh_all_font_geometry();
    gui_settings_refresh_font_geometry();
    compact_list_refresh_all();
    gui_lyrics_refresh_layout();
    quick_drawer_mark_snapshot_dirty();
    nav_reset_to_home();
    screen_builders_refresh_font_geometry(lv_screen_active());
    lv_obj_invalidate(lv_screen_active());
    lv_obj_invalidate(lv_layer_top());
    lv_obj_invalidate(lv_layer_sys());

    lv_obj_delete(mask);
}

static void custom_font_option_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    const char * target = (index < 0) ? "Default" : discovered_custom_fonts[index];

    /* Match Font Size's true no-op: do not copy/validate/rebuild the active
     * font, flash the display, save settings, or reset navigation. */
    const char * active = fallback_font_get_custom_name();
    if ((index < 0 && strcmp(active, "Default") == 0) ||
        (index >= 0 && strcmp(active, target) == 0)) return;

    lv_obj_t * mask = lv_obj_create(lv_layer_sys());
    /* Encode -1 (Default) as 1 and discovered indices as 2..N+1.  The
     * discovered-name table is static and the input-blocking mask prevents
     * it from being repopulated before the one-shot callback consumes it. */
    lv_obj_set_user_data(mask, (void *)(intptr_t)(index + 2));
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

    /* As with Font Size, let the regular refresh timer paint black before
     * any SD I/O, TTF validation or cache construction begins. */
    lv_timer_create(custom_font_apply_timer_cb, 35, mask);
}

static void custom_font_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    populate_custom_font_screen();
    nav_push(custom_font_screen);
}

static lv_obj_t * build_custom_font_screen(void) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_add_style(scr, &style_theme_screen_bg, 0);

    build_screen_header(scr, "Font", generic_back_cb, NULL, NULL);

    /* Preview card pinned at top */
    lv_obj_t * preview_card = lv_obj_create(scr);
    lv_obj_set_size(preview_card, lv_pct(90), BOARD_SCALE_PX(120));
    lv_obj_align(preview_card, LV_ALIGN_TOP_MID, 0, STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT + 8);
    lv_obj_add_style(preview_card, &style_theme_card_bg, 0);
    lv_obj_set_style_border_width(preview_card, 0, 0);
    lv_obj_set_style_radius(preview_card, 10, 0);
    lv_obj_remove_flag(preview_card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * preview_title = lv_label_create(preview_card);
    lv_label_set_text(preview_title, "Preview");
    lv_obj_add_style(preview_title, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(preview_title, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_align(preview_title, LV_ALIGN_TOP_LEFT, 12, 6);

    custom_font_preview_latin = lv_label_create(preview_card);
    lv_label_set_text(custom_font_preview_latin, "The quick brown fox jumps 123");
    lv_obj_add_style(custom_font_preview_latin, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(custom_font_preview_latin, gui_theme_font(GUI_FONT_ROLE_ROW), 0);
    lv_obj_align(custom_font_preview_latin, LV_ALIGN_TOP_LEFT, 12, 32);

    /* Custom fonts intentionally replace only the Latin face.  Rendering a
     * sample spanning every file-backed fallback here made this screen take
     * seconds to enter and leave on slow flash, while not previewing anything
     * the selected custom font can change. */
    custom_font_preview_note = lv_label_create(preview_card);
    lv_label_set_text(custom_font_preview_note, "Custom fonts affect Latin text only.");
    lv_obj_add_style(custom_font_preview_note, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(custom_font_preview_note, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_align(custom_font_preview_note, LV_ALIGN_TOP_LEFT, 12, 62);

    lv_obj_t * hint = lv_label_create(preview_card);
    lv_label_set_text(hint, "Place .ttf fonts in SD /Fonts folder.");
    lv_obj_add_style(hint, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(hint, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_align(hint, LV_ALIGN_TOP_LEFT, 12, 92);

    /* Scrollable font list */
    custom_font_list = lv_obj_create(scr);
    int32_t top_offset = STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT + 136;
    lv_obj_set_size(custom_font_list, lv_pct(100),
                    lv_display_get_vertical_resolution(lv_display_get_default()) - top_offset);
    lv_obj_align(custom_font_list, LV_ALIGN_TOP_MID, 0, top_offset);
    lv_obj_set_style_bg_opa(custom_font_list, 0, 0);
    lv_obj_set_style_border_width(custom_font_list, 0, 0);
    lv_obj_set_style_pad_all(custom_font_list, 0, 0);
    lv_obj_set_flex_flow(custom_font_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(custom_font_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(custom_font_list, 6, 0);

    finalize_screen_navigation(scr);
    return scr;
}

/* "45s" below 1 minute, "1m"/"2m" on an exact minute, "1m 30s" otherwise --
 * matches the 30s-10min range (SCREEN_TIMEOUT_MIN/MAX_SECONDS) without ever
 * needing more than whole minutes+seconds. */
static void format_screen_timeout(char * buf, size_t buf_size, int seconds) {
    if (seconds < 60) {
        snprintf(buf, buf_size, "%ds", seconds);
        return;
    }
    int minutes = seconds / 60;
    int remainder = seconds % 60;
    if (remainder == 0) snprintf(buf, buf_size, "%dm", minutes);
    else snprintf(buf, buf_size, "%dm %ds", minutes, remainder);
}

/* Index of the closest shared timeout preset. */
static int screen_timeout_seconds_to_step_index(int seconds) {
    return find_nearest_step_index(SCREEN_TIMEOUT_STEPS, SCREEN_TIMEOUT_STEP_COUNT, seconds);
}

static void screen_timeout_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    current_settings.screen_timeout_enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    settings_save(&current_settings);

    if (current_settings.screen_timeout_enabled) {
        lv_obj_remove_flag(screen_timeout_slider_card, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(screen_timeout_slider_card, LV_OBJ_FLAG_HIDDEN);
    }
}

/* Same VALUE_CHANGED-updates-live/RELEASED-persists split as the EQ sliders
 * (eq_preamp_slider_event_cb etc.) -- writing settings.c's file on every
 * drag tick would be needless disk I/O for a value that only matters once
 * the user lets go. The slider itself moves over step indices (0..
 * SCREEN_TIMEOUT_STEP_COUNT-1), mapped onto the shared preset table. */
static void screen_timeout_slider_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    int32_t index = lv_slider_get_value(lv_event_get_target(e));
    int seconds = SCREEN_TIMEOUT_STEPS[index];

    if (code == LV_EVENT_VALUE_CHANGED) {
        current_settings.screen_timeout_seconds = seconds;
        char buf[32];
        format_screen_timeout(buf, sizeof(buf), seconds);
        lv_label_set_text(screen_timeout_value_label, buf);
    } else if (code == LV_EVENT_RELEASED) {
        settings_save(&current_settings);
    }
}

static lv_obj_t * build_screen_timeout_screen(void) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_add_style(scr, &style_theme_screen_bg, 0);

    build_screen_header(scr, "Screen Timeout", generic_back_cb, NULL, NULL);

    /* Enable row configured with flex layout (SPACE_BETWEEN and flex-grow)
     * so the text label wraps cleanly across font sizes without overlapping
     * the switch. */
    lv_obj_t * enable_row = lv_obj_create(scr);
    lv_obj_set_width(enable_row, lv_pct(90));
    lv_obj_set_height(enable_row, LV_SIZE_CONTENT);
    lv_obj_align(enable_row, LV_ALIGN_TOP_MID, 0, STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT + 20);
    lv_obj_set_style_bg_opa(enable_row, 0, 0);
    lv_obj_set_style_border_width(enable_row, 0, 0);
    lv_obj_remove_flag(enable_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(enable_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(enable_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(enable_row, 12, 0);

    lv_obj_t * enable_label = lv_label_create(enable_row);
    lv_label_set_text(enable_label, "Turn off screen automatically");
    lv_obj_add_style(enable_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(enable_label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_label_set_long_mode(enable_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_flex_grow(enable_label, 1);

    screen_timeout_switch = lv_switch_create(enable_row);
    lv_obj_add_style(screen_timeout_switch, gui_theme_accent_style(), LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (current_settings.screen_timeout_enabled) lv_obj_add_state(screen_timeout_switch, LV_STATE_CHECKED);
    lv_obj_add_event_cb(screen_timeout_switch, screen_timeout_switch_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* Rounded slider card with vertical clearance below the track for the
     * knob diameter and centered value label. */
    screen_timeout_slider_card = build_setting_slider_card(scr, enable_row, BOARD_SCALE_PX(170), BOARD_SCALE_PX(18),
        0, SCREEN_TIMEOUT_STEP_COUNT - 1,
        screen_timeout_seconds_to_step_index(current_settings.screen_timeout_seconds),
        screen_timeout_slider_event_cb, &screen_timeout_slider, &screen_timeout_value_label);
    if (!current_settings.screen_timeout_enabled) lv_obj_add_flag(screen_timeout_slider_card, LV_OBJ_FLAG_HIDDEN);
    char initial_buf[32];
    format_screen_timeout(initial_buf, sizeof(initial_buf), current_settings.screen_timeout_seconds);
    lv_label_set_text(screen_timeout_value_label, initial_buf);

    finalize_screen_navigation(scr);
    return scr;
}

static void screen_timeout_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(screen_timeout_screen);
}

static int screen_dim_delay_seconds_to_step_index(int seconds) {
    return find_nearest_step_index(SCREEN_DIM_DELAY_STEPS, SCREEN_DIM_DELAY_STEP_COUNT, seconds);
}

static void screen_dimming_ui_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    current_settings.screen_dimming_enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    if (!current_settings.screen_dimming_enabled) {
        backlight_set_dimmed(false);
    }
    settings_save(&current_settings);

    if (current_settings.screen_dimming_enabled) {
        lv_obj_remove_flag(screen_dimming_slider_card, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(screen_dimming_slider_card, LV_OBJ_FLAG_HIDDEN);
    }
}

static void screen_dim_delay_slider_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    int32_t index = lv_slider_get_value(lv_event_get_target(e));
    int seconds = SCREEN_DIM_DELAY_STEPS[index];

    if (code == LV_EVENT_VALUE_CHANGED) {
        current_settings.screen_dim_delay_seconds = seconds;
        char buf[32];
        format_screen_timeout(buf, sizeof(buf), seconds);
        lv_label_set_text(screen_dimming_value_label, buf);
    } else if (code == LV_EVENT_RELEASED) {
        settings_save(&current_settings);
    }
}

static void screen_dimming_screen_loaded_cb(lv_event_t * e) {
    int effective_max_index = SCREEN_DIM_DELAY_STEP_COUNT - 1;
    if (current_settings.screen_timeout_enabled) {
        effective_max_index = -1;
        for (int i = SCREEN_DIM_DELAY_STEP_COUNT - 1; i >= 0; i--) {
            if (SCREEN_DIM_DELAY_STEPS[i] < current_settings.screen_timeout_seconds) {
                effective_max_index = i;
                break;
            }
        }
        if (effective_max_index == -1) {
            effective_max_index = 0;
        }
    }

    lv_slider_set_range(screen_dimming_slider, 0, effective_max_index);

    int current_index = screen_dim_delay_seconds_to_step_index(current_settings.screen_dim_delay_seconds);
    if (current_index > effective_max_index) {
        current_settings.screen_dim_delay_seconds = SCREEN_DIM_DELAY_STEPS[effective_max_index];
        lv_slider_set_value(screen_dimming_slider, effective_max_index, LV_ANIM_OFF);
        char buf[32];
        format_screen_timeout(buf, sizeof(buf), current_settings.screen_dim_delay_seconds);
        lv_label_set_text(screen_dimming_value_label, buf);
        settings_save(&current_settings);
    }
}

static lv_obj_t * build_screen_dimming_screen(void) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_add_event_cb(scr, screen_dimming_screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
    lv_obj_add_style(scr, &style_theme_screen_bg, 0);

    build_screen_header(scr, "Screen Dimming", generic_back_cb, NULL, NULL);

    lv_obj_t * enable_row = lv_obj_create(scr);
    lv_obj_set_width(enable_row, lv_pct(90));
    lv_obj_set_height(enable_row, LV_SIZE_CONTENT);
    lv_obj_align(enable_row, LV_ALIGN_TOP_MID, 0, STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT + 20);
    lv_obj_set_style_bg_opa(enable_row, 0, 0);
    lv_obj_set_style_border_width(enable_row, 0, 0);
    lv_obj_remove_flag(enable_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(enable_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(enable_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(enable_row, 12, 0);

    lv_obj_t * enable_label = lv_label_create(enable_row);
    lv_label_set_text(enable_label, "Dim screen before timeout");
    lv_obj_add_style(enable_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(enable_label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_label_set_long_mode(enable_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_flex_grow(enable_label, 1);

    screen_dimming_switch = lv_switch_create(enable_row);
    lv_obj_add_style(screen_dimming_switch, gui_theme_accent_style(), LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (current_settings.screen_dimming_enabled) lv_obj_add_state(screen_dimming_switch, LV_STATE_CHECKED);
    lv_obj_add_event_cb(screen_dimming_switch, screen_dimming_ui_switch_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    screen_dimming_slider_card = build_setting_slider_card(scr, enable_row, BOARD_SCALE_PX(170), BOARD_SCALE_PX(18),
        0, SCREEN_DIM_DELAY_STEP_COUNT - 1,
        screen_dim_delay_seconds_to_step_index(current_settings.screen_dim_delay_seconds),
        screen_dim_delay_slider_event_cb, &screen_dimming_slider, &screen_dimming_value_label);
    if (!current_settings.screen_dimming_enabled) lv_obj_add_flag(screen_dimming_slider_card, LV_OBJ_FLAG_HIDDEN);
    char initial_buf[32];
    format_screen_timeout(initial_buf, sizeof(initial_buf), current_settings.screen_dim_delay_seconds);
    lv_label_set_text(screen_dimming_value_label, initial_buf);

    finalize_screen_navigation(scr);
    return scr;
}

static void screen_dimming_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(screen_dimming_screen);
}

static void startup_volume_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    current_settings.startup_volume_fixed_enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    settings_save(&current_settings);

    if (current_settings.startup_volume_fixed_enabled) {
        lv_obj_remove_flag(startup_volume_slider_card, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(startup_volume_slider_card, LV_OBJ_FLAG_HIDDEN);
    }
}

/* Same VALUE_CHANGED-updates-live/RELEASED-persists split as
 * screen_timeout_slider_event_cb -- this only sets what the NEXT launch
 * starts at, not the current session's live volume, so there's no reason
 * to call audio_set_volume() here at all. */
static void startup_volume_slider_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    int32_t percent = lv_slider_get_value(lv_event_get_target(e));

    if (code == LV_EVENT_VALUE_CHANGED) {
        current_settings.startup_volume_fixed_percent = (int) percent;
        lv_label_set_text_fmt(startup_volume_value_label, "%d%%", (int) percent);
    } else if (code == LV_EVENT_RELEASED) {
        settings_save(&current_settings);
    }
}

/* Mirrors build_screen_timeout_screen()'s own layout (toggle row + a
 * dark card holding a live-readout slider, shown/hidden with the toggle) --
 * see that function's comments for the reasoning behind the specific
 * measurements reused here. */
static lv_obj_t * build_startup_volume_screen(void) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_add_style(scr, &style_theme_screen_bg, 0);

    build_screen_header(scr, "Startup Volume", generic_back_cb, NULL, NULL);

    /* Flex row with SPACE_BETWEEN layout to prevent text/switch overlap. */
    lv_obj_t * enable_row = lv_obj_create(scr);
    lv_obj_set_width(enable_row, lv_pct(90));
    lv_obj_set_height(enable_row, LV_SIZE_CONTENT);
    lv_obj_align(enable_row, LV_ALIGN_TOP_MID, 0, STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT + 20);
    lv_obj_set_style_bg_opa(enable_row, 0, 0);
    lv_obj_set_style_border_width(enable_row, 0, 0);
    lv_obj_remove_flag(enable_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(enable_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(enable_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(enable_row, 12, 0);

    lv_obj_t * enable_label = lv_label_create(enable_row);
    lv_label_set_text(enable_label, "Launch at a fixed volume");
    lv_obj_add_style(enable_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(enable_label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_label_set_long_mode(enable_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_flex_grow(enable_label, 1);

    startup_volume_switch = lv_switch_create(enable_row);
    lv_obj_add_style(startup_volume_switch, gui_theme_accent_style(), LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (current_settings.startup_volume_fixed_enabled) lv_obj_add_state(startup_volume_switch, LV_STATE_CHECKED);
    lv_obj_add_event_cb(startup_volume_switch, startup_volume_switch_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    startup_volume_slider_card = build_setting_slider_card(scr, enable_row, BOARD_SCALE_PX(170), BOARD_SCALE_PX(18),
        0, 100, current_settings.startup_volume_fixed_percent,
        startup_volume_slider_event_cb, &startup_volume_slider, &startup_volume_value_label);
    if (!current_settings.startup_volume_fixed_enabled) lv_obj_add_flag(startup_volume_slider_card, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text_fmt(startup_volume_value_label, "%d%%", current_settings.startup_volume_fixed_percent);

    finalize_screen_navigation(scr);
    return scr;
}

static void startup_volume_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(startup_volume_screen);
}

/* Index into SLEEP_TIMER_STEPS closest to `minutes' -- same reasoning as
 * screen_timeout_seconds_to_step_index() above. */
static int sleep_timer_minutes_to_step_index(int minutes) {
    return find_nearest_step_index(SLEEP_TIMER_STEPS, SLEEP_TIMER_STEP_COUNT, minutes);
}

/* Extracted from build_sleep_timer_screen() so it can be shared with its
 * own format_duration(), needed now that SLEEP_TIMER_STEPS reaches 180
 * (settings.c) and a bare "%d min" would read as "180 min" instead of the
 * much more readable "3 hr". */
static void format_sleep_timer_duration(char * buf, size_t buf_size, int minutes) {
    int hours = minutes / 60;
    int remainder = minutes % 60;
    if (hours == 0) snprintf(buf, buf_size, "%d min", remainder);
    else if (remainder == 0) snprintf(buf, buf_size, "%d hr", hours);
    else snprintf(buf, buf_size, "%d hr %d min", hours, remainder);
}

/* Arms or disarms the sleep timer directly, synchronizing state with the
 * quick drawer sleep timer toggle. */
static void sleep_timer_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    bool active = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    quick_drawer_sleep_timer_set_active(active);
    if (active) {
        lv_obj_remove_flag(sleep_timer_slider_card, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(sleep_timer_remaining_btn, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(sleep_timer_slider_card, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(sleep_timer_remaining_btn, LV_OBJ_FLAG_HIDDEN);
    }
}

/* This only sets the duration the quick-drawer sleep icon/enable toggle
 * above arms next time (or restarts the CURRENT countdown at, if already
 * armed -- same real-time-restart behavior ExtendedSleepTimer.lua's own
 * duration slider has, matching what a user dragging this while counting
 * down would expect: the countdown they're looking at should reflect the
 * duration they just picked, not the one that was armed before). */
static void sleep_timer_slider_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    int32_t index = lv_slider_get_value(lv_event_get_target(e));
    int minutes = SLEEP_TIMER_STEPS[index];

    if (code == LV_EVENT_VALUE_CHANGED) {
        current_settings.sleep_timer_minutes = minutes;
        char duration_buf[32];
        format_sleep_timer_duration(duration_buf, sizeof(duration_buf), minutes);
        lv_label_set_text(sleep_timer_value_label, duration_buf);
        if (quick_drawer_sleep_timer_is_active()) quick_drawer_sleep_timer_set_active(true);
    } else if (code == LV_EVENT_RELEASED) {
        settings_save(&current_settings);
    }
}

/* "Show Time Remaining" button -- same info ExtendedSleepTimer.lua's own
 * "Show Time Remaining" settings row surfaces via a toast, in the same
 * H:MM:SS (or M:SS under an hour) shape as that plugin's format_remaining().
 * Hidden whenever the timer is disarmed (sleep_timer_switch_event_cb /
 * gui_settings_sync_sleep_timer_toggle), so this is only ever reachable
 * while armed -- no "timer is off" fallback message needed here. */
static void sleep_timer_show_remaining_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int total_seconds = quick_drawer_sleep_timer_remaining_seconds();
    int hours = total_seconds / 3600;
    int minutes = (total_seconds / 60) % 60;
    int secs = total_seconds % 60;
    char buf[48];
    if (hours > 0) snprintf(buf, sizeof(buf), "Time remaining: %d:%02d:%02d", hours, minutes, secs);
    else snprintf(buf, sizeof(buf), "Time remaining: %d:%02d", minutes, secs);
    show_info_toast(buf);
}

/* Mirrors build_screen_timeout_screen()'s layout (enable row + slider
 * card), plus one more row: a "Show Time Remaining" button neither that
 * screen nor this one needed before ExtendedSleepTimer.lua's own version
 * of this same feature demonstrated why one's genuinely useful here (the
 * status bar has nowhere to show a running countdown, unlike the quick
 * drawer's own quick_drawer_sleep_label). */
static lv_obj_t * build_sleep_timer_screen(void) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_add_style(scr, &style_theme_screen_bg, 0);

    build_screen_header(scr, "Sleep Timer", generic_back_cb, NULL, NULL);

    /* Same flex-row/SPACE_BETWEEN/flex_grow-label shape as build_screen_
     * timeout_screen()'s own enable_row -- see its comment for the real-
     * device overlap bug this avoids at bigger system text sizes. */
    lv_obj_t * enable_row = lv_obj_create(scr);
    lv_obj_set_width(enable_row, lv_pct(90));
    lv_obj_set_height(enable_row, LV_SIZE_CONTENT);
    lv_obj_align(enable_row, LV_ALIGN_TOP_MID, 0, STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT + 20);
    lv_obj_set_style_bg_opa(enable_row, 0, 0);
    lv_obj_set_style_border_width(enable_row, 0, 0);
    lv_obj_remove_flag(enable_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(enable_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(enable_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(enable_row, 12, 0);

    lv_obj_t * enable_label = lv_label_create(enable_row);
    lv_label_set_text(enable_label, "Enable Sleep Timer");
    lv_obj_add_style(enable_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(enable_label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_label_set_long_mode(enable_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_flex_grow(enable_label, 1);

    sleep_timer_switch = lv_switch_create(enable_row);
    lv_obj_add_style(sleep_timer_switch, gui_theme_accent_style(), LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (quick_drawer_sleep_timer_is_active()) lv_obj_add_state(sleep_timer_switch, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sleep_timer_switch, sleep_timer_switch_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    sleep_timer_slider_card = build_setting_slider_card(scr, enable_row, BOARD_SCALE_PX(170), BOARD_SCALE_PX(18),
        0, SLEEP_TIMER_STEP_COUNT - 1,
        sleep_timer_minutes_to_step_index(current_settings.sleep_timer_minutes),
        sleep_timer_slider_event_cb, &sleep_timer_slider, &sleep_timer_value_label);
    if (!quick_drawer_sleep_timer_is_active()) lv_obj_add_flag(sleep_timer_slider_card, LV_OBJ_FLAG_HIDDEN);
    char duration_buf[32];
    format_sleep_timer_duration(duration_buf, sizeof(duration_buf), current_settings.sleep_timer_minutes);
    lv_label_set_text(sleep_timer_value_label, duration_buf);

    sleep_timer_remaining_btn = lv_obj_create(scr);
    lv_obj_set_size(sleep_timer_remaining_btn, lv_pct(90), BOARD_SCALE_PX(70));
    lv_obj_align_to(sleep_timer_remaining_btn, sleep_timer_slider_card, LV_ALIGN_OUT_BOTTOM_MID, 0, 20);
    lv_obj_add_style(sleep_timer_remaining_btn, &style_theme_card_bg, 0);
    lv_obj_set_style_border_width(sleep_timer_remaining_btn, 0, 0);
    lv_obj_set_style_radius(sleep_timer_remaining_btn, 10, 0);
    lv_obj_remove_flag(sleep_timer_remaining_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(sleep_timer_remaining_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(sleep_timer_remaining_btn, sleep_timer_show_remaining_cb, LV_EVENT_CLICKED, NULL);
    if (!quick_drawer_sleep_timer_is_active()) lv_obj_add_flag(sleep_timer_remaining_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t * remaining_label = lv_label_create(sleep_timer_remaining_btn);
    lv_label_set_text(remaining_label, "Show Time Remaining");
    lv_obj_add_style(remaining_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(remaining_label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_obj_center(remaining_label);

    finalize_screen_navigation(scr);
    return scr;
}

static void sleep_timer_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(sleep_timer_screen);
}

/* All IDLE_SHUTDOWN_STEPS values are whole minutes, so unlike
 * format_screen_timeout() this never needs a seconds remainder -- "10m" /
 * "1h" / "2h", not "1h 30m", since 90 isn't one of the steps. */
static void format_idle_shutdown(char * buf, size_t buf_size, int minutes) {
    if (minutes < 60) {
        snprintf(buf, buf_size, "%dm", minutes);
        return;
    }
    snprintf(buf, buf_size, "%dh", minutes / 60);
}

static int idle_shutdown_minutes_to_step_index(int minutes) {
    return find_nearest_step_index(IDLE_SHUTDOWN_STEPS, IDLE_SHUTDOWN_STEP_COUNT, minutes);
}

static void idle_shutdown_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    current_settings.idle_shutdown_enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    settings_save(&current_settings);

    if (current_settings.idle_shutdown_enabled) {
        lv_obj_remove_flag(idle_action_section, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(idle_shutdown_slider_card, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(idle_action_section, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(idle_shutdown_slider_card, LV_OBJ_FLAG_HIDDEN);
    }
}

/* Mutually-exclusive idle-action choice (Power Off vs Suspend to RAM) --
 * replaces the old single "Suspend instead of power off" switch, which
 * modeled this same 2-way choice as a toggle acting on an implicit
 * opposite (plain poweroff being "off"). Same accent-border-highlight
 * pill-row selection language as populate_usb_mode_screen()'s own Storage/
 * USB DAC rows (add_pill_row_base()), but inline on this same screen
 * rather than a separate list screen, since it stays right above the
 * slider it's a sibling setting of. */
static void idle_action_choice_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    bool suspend = (bool) (intptr_t) lv_event_get_user_data(e);
    current_settings.idle_suspend_enabled = suspend;
    settings_save(&current_settings);
    lv_obj_set_style_border_width(idle_action_poweroff_row, suspend ? 0 : 3, 0);
    lv_obj_set_style_border_width(idle_action_suspend_row, suspend ? 3 : 0, 0);
}

static void idle_shutdown_slider_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    int32_t index = lv_slider_get_value(lv_event_get_target(e));
    int minutes = IDLE_SHUTDOWN_STEPS[index];

    if (code == LV_EVENT_VALUE_CHANGED) {
        current_settings.idle_shutdown_minutes = minutes;
        char buf[32];
        format_idle_shutdown(buf, sizeof(buf), minutes);
        lv_label_set_text(idle_shutdown_value_label, buf);
    } else if (code == LV_EVENT_RELEASED) {
        settings_save(&current_settings);
    }
}

static lv_obj_t * build_idle_shutdown_screen(void) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_add_style(scr, &style_theme_screen_bg, 0);

    build_screen_header(scr, "Idle Shutdown", generic_back_cb, NULL, NULL);

    /* Uses content-based sizing so wrapped label text does not overlap. */
    lv_obj_t * enable_row = lv_obj_create(scr);
    lv_obj_set_width(enable_row, lv_pct(90));
    lv_obj_set_height(enable_row, LV_SIZE_CONTENT);
    lv_obj_align(enable_row, LV_ALIGN_TOP_MID, 0, STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT + 20);
    lv_obj_set_style_bg_opa(enable_row, 0, 0);
    lv_obj_set_style_border_width(enable_row, 0, 0);
    lv_obj_remove_flag(enable_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(enable_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(enable_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(enable_row, 12, 0);

    lv_obj_t * enable_label = lv_label_create(enable_row);
    lv_label_set_text(enable_label, "Automatically go idle");
    lv_obj_add_style(enable_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(enable_label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_label_set_long_mode(enable_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_flex_grow(enable_label, 1);

    idle_shutdown_switch = lv_switch_create(enable_row);
    lv_obj_add_style(idle_shutdown_switch, gui_theme_accent_style(), LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (current_settings.idle_shutdown_enabled) lv_obj_add_state(idle_shutdown_switch, LV_STATE_CHECKED);
    lv_obj_add_event_cb(idle_shutdown_switch, idle_shutdown_switch_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* Mutually-exclusive idle-action choice (Power Off or Suspend to RAM).
     * Shown/hidden together with the slider card in
     * idle_shutdown_switch_event_cb(). */
    /* Section height and the two pill rows' Y offsets below were all
     * hardcoded (292 / 34 / 34+124+10) at some medium-tier row height --
     * add_pill_row_base()'s own row height already scales with
     * GUI_FONT_ROLE_BODY's line height, but these hand-rolled offsets
     * didn't, so at the largest ("BlindMF") font tier the two rows grew
     * taller than the fixed gaps between them and overflowed the fixed
     * section height into the slider card below (GitHub issue #91).
     * Derived from the same font metrics add_pill_row_base() itself uses,
     * so this stays correct at every tier without needing another manual
     * retune. */
    int32_t idle_action_explain_h = lv_font_get_line_height(gui_theme_font(GUI_FONT_ROLE_SUBTEXT));
    int32_t idle_action_row_h = lv_font_get_line_height(gui_theme_font(GUI_FONT_ROLE_BODY)) + 32;
    if (idle_action_row_h < GUI_SETTINGS_ROW_HEIGHT) idle_action_row_h = GUI_SETTINGS_ROW_HEIGHT;
    int32_t idle_action_row1_y = idle_action_explain_h + 14;
    int32_t idle_action_row_gap = 10;
    int32_t idle_action_row2_y = idle_action_row1_y + idle_action_row_h + idle_action_row_gap;
    int32_t idle_action_section_h = idle_action_row2_y + idle_action_row_h;

    /* 100% width accommodates display-width pill rows without clipping.
     * pad_top and pad_bottom are zeroed so the child rows and label fit within
     * the section height (now font-tier-derived above) without vertical overflow. */
    idle_action_section = lv_obj_create(scr);
    lv_obj_set_size(idle_action_section, lv_pct(100), idle_action_section_h);
    /* Positioned relative to enable_row's bottom edge so wrapped label text
     * does not cause overlapping. */
    lv_obj_align_to(idle_action_section, enable_row, LV_ALIGN_OUT_BOTTOM_MID, 0, 20);
    lv_obj_set_style_bg_opa(idle_action_section, 0, 0);
    lv_obj_set_style_border_width(idle_action_section, 0, 0);
    lv_obj_set_style_pad_top(idle_action_section, 0, 0);
    lv_obj_set_style_pad_bottom(idle_action_section, 0, 0);
    lv_obj_remove_flag(idle_action_section, LV_OBJ_FLAG_SCROLLABLE);
    if (!current_settings.idle_shutdown_enabled) lv_obj_add_flag(idle_action_section, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t * idle_action_explain_label = lv_label_create(idle_action_section);
    lv_label_set_text(idle_action_explain_label, "Choose what happens when idle:");
    lv_obj_add_style(idle_action_explain_label, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(idle_action_explain_label, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_align(idle_action_explain_label, LV_ALIGN_TOP_LEFT, 0, 0);

    idle_action_poweroff_row = add_pill_row_base(idle_action_section, "Power Off");
    lv_obj_align(idle_action_poweroff_row, LV_ALIGN_TOP_MID, 0, idle_action_row1_y);
    lv_obj_set_style_border_color(idle_action_poweroff_row, accent_lv_color(), 0);
    lv_obj_set_style_border_width(idle_action_poweroff_row, current_settings.idle_suspend_enabled ? 0 : 3, 0);
    lv_obj_add_flag(idle_action_poweroff_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(idle_action_poweroff_row, idle_action_choice_cb, LV_EVENT_CLICKED, (void *) (intptr_t) false);

    idle_action_suspend_row = add_pill_row_base(idle_action_section, "Suspend to RAM");
    lv_obj_align(idle_action_suspend_row, LV_ALIGN_TOP_MID, 0, idle_action_row2_y);
    lv_obj_set_style_border_color(idle_action_suspend_row, accent_lv_color(), 0);
    lv_obj_set_style_border_width(idle_action_suspend_row, current_settings.idle_suspend_enabled ? 3 : 0, 0);
    lv_obj_add_flag(idle_action_suspend_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(idle_action_suspend_row, idle_action_choice_cb, LV_EVENT_CLICKED, (void *) (intptr_t) true);

    /* Slider card positioned below idle_action_section. Sized at 200px height
     * to accommodate the explanatory caption above the slider. */
    idle_shutdown_slider_card = build_setting_slider_card(scr, idle_action_section, BOARD_SCALE_PX(200), BOARD_SCALE_PX(48),
        0, IDLE_SHUTDOWN_STEP_COUNT - 1,
        idle_shutdown_minutes_to_step_index(current_settings.idle_shutdown_minutes),
        idle_shutdown_slider_event_cb, &idle_shutdown_slider, &idle_shutdown_value_label);
    if (!current_settings.idle_shutdown_enabled) lv_obj_add_flag(idle_shutdown_slider_card, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t * idle_shutdown_slider_caption = lv_label_create(idle_shutdown_slider_card);
    lv_label_set_text(idle_shutdown_slider_caption, "Idle timeout:");
    lv_obj_add_style(idle_shutdown_slider_caption, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(idle_shutdown_slider_caption, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_align(idle_shutdown_slider_caption, LV_ALIGN_TOP_LEFT, 24, 14);
    char initial_buf[32];
    format_idle_shutdown(initial_buf, sizeof(initial_buf), current_settings.idle_shutdown_minutes);
    lv_label_set_text(idle_shutdown_value_label, initial_buf);

    finalize_screen_navigation(scr);
    return scr;
}

static void idle_shutdown_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(idle_shutdown_screen);
}

/* ---- Time Zone picker ----------------------------------------------------
 * Region -> City selection. TIMEZONE_TABLE (timezone_data.h) is sorted
 * by UTC offset then city name. Region names are the top-level IANA region
 * components present in TIMEZONE_TABLE. */
static const char * const TIMEZONE_REGIONS[] = {
    "Africa", "America", "Antarctica", "Arctic", "Asia", "Atlantic", "Australia", "Europe", "Indian", "Pacific"
};
#define TIMEZONE_REGION_COUNT (sizeof(TIMEZONE_REGIONS) / sizeof(TIMEZONE_REGIONS[0]))

static lv_obj_t * timezone_region_screen;
static lv_obj_t * timezone_city_screen;
/* Maps a City-screen row index (as passed to timezone_city_row_click_cb)
 * back to its real TIMEZONE_TABLE index -- the City screen only ever shows
 * one region's subset, so row index != table index. */
static int timezone_city_indices[TIMEZONE_TABLE_COUNT];
static int timezone_city_count;

static void clock_timezone_indicator_update(void) {
    if (!clock_timezone_value_label) return;
    const char * zone = current_settings.timezone[0]
        ? current_settings.timezone : "Not selected (UTC)";
    for (int i = 0; current_settings.timezone[0] && i < TIMEZONE_TABLE_COUNT; i++) {
        if (strcmp(current_settings.timezone, TIMEZONE_TABLE[i].iana_id) == 0) {
            zone = TIMEZONE_TABLE[i].display_name;
            break;
        }
    }
    lv_label_set_text(clock_timezone_value_label, zone);
}

static void timezone_city_row_click_cb(int row_index) {
    if (row_index < 0 || row_index >= timezone_city_count) return;
    const timezone_entry_t * entry = &TIMEZONE_TABLE[timezone_city_indices[row_index]];
    if (!timezone_apply(entry->iana_id)) {
        show_error_toast("Failed to apply time zone");
        return;
    }
    snprintf(current_settings.timezone, sizeof(current_settings.timezone), "%s", entry->iana_id);
    settings_save(&current_settings);
    refresh_clock_label(); /* topbar clock reflects the new zone immediately, not just on the next periodic refresh */
    clock_timezone_indicator_update();
    /* Skip the intermediate Region entry before starting navigation. Two
     * immediate nav_pop() calls race their slide transitions: the first
     * can finish later and restore Region over the intended Clock screen. */
    int depth = gui_navigation_get_depth();
    if (depth >= 3) nav_remove_stack_slot(depth - 2);
    nav_pop(); /* City -> Clock -- picking a leaf city completes the choice */
}

/* Rebuilt (delete + rebuild, same idiom as gui_library_get_all_songs_screen() after a library
 * rescan -- see poll_library_rescan()) every time a region is opened,
 * filtered down to just that region's entries, rather than built once --
 * needed both because the region changes on every open and so the
 * "(current)" marker always reflects current_settings.timezone as of the
 * most recent selection. Cheap even at TIMEZONE_TABLE_COUNT entries since
 * build_compact_list_screen is virtualized (screen_builders.h) -- only a
 * small pool of row widgets actually gets created, not one per entry.
 * `labels` is this function's own long-lived backing array for the
 * "(current)"-suffixed copy build_compact_list_screen's own doc comment
 * requires (label pointers must outlive the screen, not just this call) --
 * previous generation's entries are freed up to prev_count (the region
 * filter means the count varies per open, unlike a fixed-size table) right
 * before this rebuild, once the previous screen holding those pointers has
 * already been deleted by the caller. */
static void build_timezone_city_screen_items(compact_list_item_t * items, const char * region) {
    static char * labels[TIMEZONE_TABLE_COUNT];
    static int prev_count = 0;
    for (int i = 0; i < prev_count; i++) {
        free(labels[i]);
        labels[i] = NULL;
    }

    size_t region_len = strlen(region);
    timezone_city_count = 0;
    for (int i = 0; i < TIMEZONE_TABLE_COUNT; i++) {
        const char * id = TIMEZONE_TABLE[i].iana_id;
        if (strncmp(id, region, region_len) != 0 || id[region_len] != '/') continue;

        int row = timezone_city_count;
        bool is_current = strcmp(current_settings.timezone, id) == 0;
        if (is_current) {
            size_t len = strlen(TIMEZONE_TABLE[i].display_name) + 16;
            labels[row] = malloc(len);
            snprintf(labels[row], len, "%s (current)", TIMEZONE_TABLE[i].display_name);
        } else {
            labels[row] = strdup(TIMEZONE_TABLE[i].display_name);
        }
        timezone_city_indices[row] = i;
        items[row] = (compact_list_item_t){ labels[row] };
        timezone_city_count++;
    }
    prev_count = timezone_city_count;
}

static lv_obj_t * build_timezone_city_screen(const char * region) {
    compact_list_item_t * items = malloc(sizeof(compact_list_item_t) * (size_t) TIMEZONE_TABLE_COUNT);
    build_timezone_city_screen_items(items, region);
    lv_obj_t * scr = build_compact_list_screen(region, generic_back_cb, items, timezone_city_count, timezone_city_row_click_cb, NULL, NULL, NULL, LIST_ROW_WIDTH, false, lv_color_black());
    free(items);
    finalize_screen_navigation(scr);
    return scr;
}

static void open_timezone_city_screen(const char * region) {
    if (timezone_city_screen) lv_obj_delete(timezone_city_screen);
    timezone_city_screen = build_timezone_city_screen(region);
    nav_push(timezone_city_screen);
}

static void timezone_region_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    open_timezone_city_screen(TIMEZONE_REGIONS[index]);
}

/* Small (10 rows) and fixed -- built once at startup like every other
 * screen, unlike the per-region City screen above. */
static lv_obj_t * build_timezone_region_screen(void) {
    static pill_list_item_t items[TIMEZONE_REGION_COUNT];
    for (size_t i = 0; i < TIMEZONE_REGION_COUNT; i++) {
        items[i] = (pill_list_item_t){ TIMEZONE_REGIONS[i], PILL_ACCESSORY_CHEVRON, false, timezone_region_row_cb, NULL,
                                        (void *) (intptr_t) i };
    }
    lv_obj_t * scr = build_pill_list_screen("Time Zone", generic_back_cb, items, (int) TIMEZONE_REGION_COUNT, gui_theme_accent_style(), GUI_ROW_GAP, 100);
    finalize_screen_navigation(scr);
    return scr;
}

static void timezone_settings_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(timezone_region_screen);
}

/* Defined later, alongside its confirmation popup (build_factory_reset_popup(),
 * right after build_eq_reset_popup() -- same hand-built top-layer overlay
 * shape). */
static void factory_reset_btn_cb(lv_event_t * e);

/* ---- Settings category sub-screens -- grouped into category screens
 * (Display, Power & Sleep, Device & Storage, Date & Time, About). ---- */

/* ---- Music Settings sub-screens -- grouped into category screens
 * (Playback, Audio, Controls & Interface, Timers, Library, Plugins). ---- */


/* Shared click handler for every plugin-registered Playback list row below
 * -- same index-not-object shape plugin_display_list_item_click_cb() already
 * uses. */
static void plugin_playback_list_item_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    plugin_manager_playback_list_item_clicked(index);
}

static void plugin_music_audio_list_item_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    plugin_manager_music_audio_list_item_clicked(index);
}

static void plugin_music_controls_list_item_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    plugin_manager_music_controls_list_item_clicked(index);
}

static void plugin_music_timers_list_item_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    plugin_manager_music_timers_list_item_clicked(index);
}

static void plugin_music_library_list_item_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    plugin_manager_music_library_list_item_clicked(index);
}

static lv_obj_t * build_music_playback_screen(void) {
    static pill_list_item_t items[4 + PLUGIN_MAX_PLAYBACK_LIST_ITEMS];
    items[0] = (pill_list_item_t){ "Resume Last Track", PILL_ACCESSORY_CHEVRON, false, resume_mode_settings_row_cb, NULL, NULL };
    items[1] = (pill_list_item_t){ "ReplayGain", PILL_ACCESSORY_CHEVRON, false, replaygain_mode_settings_row_cb, NULL, NULL };
    items[2] = (pill_list_item_t){ "Crossfade", PILL_ACCESSORY_TOGGLE,
                                    current_settings.crossfade_enabled, NULL, crossfade_switch_event_cb, NULL,
                                    &settings_crossfade_toggle_img };
    /* Directly below Crossfade: the two are coupled (crossfade needs an
     * armed next track, which only gapless provides), so seeing them
     * together is what makes that relationship legible when one flips the
     * other -- see gui_player_set_gapless_enabled(). */
    items[3] = (pill_list_item_t){ "Gapless", PILL_ACCESSORY_TOGGLE,
                                    current_settings.gapless_enabled, NULL, gapless_switch_event_cb, NULL,
                                    &settings_gapless_toggle_img };

    int count = 4;
    count = append_plugin_list_rows(items, count, PLUGIN_MAX_PLAYBACK_LIST_ITEMS,
                                    plugin_manager_get_playback_list_item_count,
                                    plugin_manager_get_playback_list_item_label,
                                    plugin_manager_get_playback_list_item_options,
                                    plugin_playback_list_item_click_cb);

    lv_obj_t * scr = build_pill_list_screen("Playback", generic_back_cb, items, count, gui_theme_accent_style(), GUI_ROW_GAP, 100);
    finalize_screen_navigation(scr);
    return scr;
}

static lv_obj_t * build_music_audio_screen(void) {
    static pill_list_item_t items[2 + PLUGIN_MAX_MUSIC_AUDIO_LIST_ITEMS];
    items[0] = (pill_list_item_t){ "Equalizer", PILL_ACCESSORY_CHEVRON, false, eq_screen_btn_event_cb, NULL, NULL };
    items[1] = (pill_list_item_t){ "Startup Volume", PILL_ACCESSORY_CHEVRON, false, startup_volume_row_cb, NULL, NULL };

    int count = 2;
    count = append_plugin_list_rows(items, count, PLUGIN_MAX_MUSIC_AUDIO_LIST_ITEMS,
                                    plugin_manager_get_music_audio_list_item_count,
                                    plugin_manager_get_music_audio_list_item_label,
                                    plugin_manager_get_music_audio_list_item_options,
                                    plugin_music_audio_list_item_click_cb);

    lv_obj_t * scr = build_pill_list_screen("Audio", generic_back_cb, items, count, gui_theme_accent_style(), GUI_ROW_GAP, 100);
    finalize_screen_navigation(scr);
    return scr;
}

static lv_obj_t * build_music_controls_screen(void) {
    static pill_list_item_t items[4 + PLUGIN_MAX_MUSIC_CONTROLS_LIST_ITEMS];
    items[0] = (pill_list_item_t){ "Play/Pause Button", PILL_ACCESSORY_CHEVRON, false, play_pause_button_mode_settings_row_cb, NULL, NULL };
    items[1] = (pill_list_item_t){ "Car Mode", PILL_ACCESSORY_TOGGLE,
                                    current_settings.car_mode_enabled, NULL, car_mode_switch_event_cb, NULL };
    items[2] = (pill_list_item_t){ "In-line Remote", PILL_ACCESSORY_TOGGLE,
                                    current_settings.inline_remote_enabled, NULL, inline_remote_switch_event_cb, NULL };
    items[3] = (pill_list_item_t){ "Lyrics", PILL_ACCESSORY_TOGGLE,
                                    current_settings.lyrics_enabled, NULL, lyrics_switch_event_cb, NULL };

    int count = 4;
    count = append_plugin_list_rows(items, count, PLUGIN_MAX_MUSIC_CONTROLS_LIST_ITEMS,
                                    plugin_manager_get_music_controls_list_item_count,
                                    plugin_manager_get_music_controls_list_item_label,
                                    plugin_manager_get_music_controls_list_item_options,
                                    plugin_music_controls_list_item_click_cb);

    lv_obj_t * scr = build_pill_list_screen("Controls & Interface", generic_back_cb, items, count, gui_theme_accent_style(), GUI_ROW_GAP, 100);
    finalize_screen_navigation(scr);
    return scr;
}

static lv_obj_t * build_music_timers_screen(void) {
    static pill_list_item_t items[1 + PLUGIN_MAX_MUSIC_TIMERS_LIST_ITEMS];
    items[0] = (pill_list_item_t){ "Sleep Timer", PILL_ACCESSORY_CHEVRON, false, sleep_timer_row_cb, NULL, NULL };

    int count = 1;
    count = append_plugin_list_rows(items, count, PLUGIN_MAX_MUSIC_TIMERS_LIST_ITEMS,
                                    plugin_manager_get_music_timers_list_item_count,
                                    plugin_manager_get_music_timers_list_item_label,
                                    plugin_manager_get_music_timers_list_item_options,
                                    plugin_music_timers_list_item_click_cb);

    lv_obj_t * scr = build_pill_list_screen("Timers", generic_back_cb, items, count, gui_theme_accent_style(), GUI_ROW_GAP, 100);
    finalize_screen_navigation(scr);
    return scr;
}

static lv_obj_t * build_music_library_screen(void) {
    static pill_list_item_t items[PLUGIN_MAX_MUSIC_LIBRARY_LIST_ITEMS];
    int count = append_plugin_list_rows(items, 0, PLUGIN_MAX_MUSIC_LIBRARY_LIST_ITEMS,
                                        plugin_manager_get_music_library_list_item_count,
                                        plugin_manager_get_music_library_list_item_label,
                                        plugin_manager_get_music_library_list_item_options,
                                        plugin_music_library_list_item_click_cb);
    if (count <= 0) return NULL;

    lv_obj_t * scr = build_pill_list_screen("Library", generic_back_cb, items, count, gui_theme_accent_style(), GUI_ROW_GAP, 100);
    finalize_screen_navigation(scr);
    return scr;
}

static void music_category_playback_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(music_playback_screen);
}
static void music_category_audio_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(music_audio_screen);
}
static void music_category_controls_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(music_controls_screen);
}
static void music_category_timers_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(music_timers_screen);
}
static void music_category_library_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || !music_library_screen) return;
    nav_push(music_library_screen);
}

void refresh_all_metadata_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) show_library_refresh_all_metadata_prompt();
}

static void refresh_all_covers_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) show_library_refresh_all_covers_prompt();
}

static void music_category_database_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(music_database_screen);
}

/* Update checks for added, removed and changed files; Refresh re-reads the
 * tags of every song even when a file looks unchanged. Distinct icons keep
 * the two from reading as the same action. */
static lv_obj_t * build_music_database_screen(void) {
    static pill_list_item_t items[3];
    lv_obj_t * native_rows[3] = { NULL };
    items[0] = (pill_list_item_t){
        .label = "Update Music Database",
        .accessory = PILL_ACCESSORY_NONE,
        .on_click = update_music_database_row_cb,
        .out_row = &native_rows[0],
    };
    items[1] = (pill_list_item_t){
        .label = "Refresh All Metadata",
        .accessory = PILL_ACCESSORY_NONE,
        .on_click = refresh_all_metadata_row_cb,
        .out_row = &native_rows[1],
    };
    items[2] = (pill_list_item_t){
        .label = "Refresh All Covers",
        .accessory = PILL_ACCESSORY_NONE,
        .on_click = refresh_all_covers_row_cb,
        .out_row = &native_rows[2],
    };
    lv_obj_t * scr = build_pill_list_screen("Music Database", generic_back_cb, items, 3,
                                            gui_theme_accent_style(), GUI_ROW_GAP, 100);
    decorate_category_row(native_rows[0], "submenu/update_database.png", NULL);
    decorate_category_row(native_rows[1], "submenu/refresh_metadata.png", NULL);
    decorate_category_row(native_rows[2], "submenu/refresh_covers.png", NULL);
    finalize_screen_navigation(scr);
    return scr;
}

static lv_obj_t * build_music_settings_screen(void) {
    static pill_list_item_t items[6];
    lv_obj_t * native_rows[6] = { NULL };
    items[0] = (pill_list_item_t){
        .label = "Music Database",
        .accessory = PILL_ACCESSORY_CHEVRON,
        .on_click = music_category_database_cb,
        .out_row = &native_rows[0],
    };
    items[1] = (pill_list_item_t){ "Playback", PILL_ACCESSORY_CHEVRON, false, music_category_playback_cb, NULL, NULL };
    items[2] = (pill_list_item_t){ "Audio", PILL_ACCESSORY_CHEVRON, false, music_category_audio_cb, NULL, NULL };
    items[3] = (pill_list_item_t){ "Controls & Interface", PILL_ACCESSORY_CHEVRON, false, music_category_controls_cb, NULL, NULL };
    items[4] = (pill_list_item_t){ "Timers", PILL_ACCESSORY_CHEVRON, false, music_category_timers_cb, NULL, NULL };

    int count = 5;
    if (plugin_manager_get_music_library_list_item_count() > 0) {
        items[count++] = (pill_list_item_t){ "Library", PILL_ACCESSORY_CHEVRON, false,
                                             music_category_library_cb, NULL, NULL };
    }

    lv_obj_t * scr = build_pill_list_screen("Music Settings", generic_back_cb, items, count,
                                            gui_theme_accent_style(), GUI_ROW_GAP, 100);
    decorate_category_row(native_rows[0], "submenu/music_database.png", NULL);
    finalize_screen_navigation(scr);
    return scr;
}

/* Shared click handler for every plugin-registered Display list row below --
 * same index-not-object shape plugin_settings_list_item_click_cb() (defined
 * further down, alongside build_settings_screen()) already uses. Forward-
 * declared static since build_settings_display_screen() is defined earlier
 * in the file than that sibling. */
static void plugin_display_list_item_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    plugin_manager_display_list_item_clicked(index);
}

static lv_obj_t * build_settings_display_screen(void) {
    static pill_list_item_t items[8 + PLUGIN_MAX_DISPLAY_LIST_ITEMS];
    items[0] = (pill_list_item_t){ "Accent Color", PILL_ACCESSORY_CHEVRON, false, accent_color_row_cb, NULL, NULL };
    items[1] = (pill_list_item_t){ "Font", PILL_ACCESSORY_CHEVRON, false, custom_font_row_cb, NULL, NULL };
    items[2] = (pill_list_item_t){ "Font Size", PILL_ACCESSORY_CHEVRON, false, font_size_settings_row_cb, NULL, NULL };
    items[3] = (pill_list_item_t){ "Lyrics Text Size", PILL_ACCESSORY_CHEVRON, false, lyrics_font_size_settings_row_cb, NULL, NULL };
    items[4] = (pill_list_item_t){ "Screen Timeout", PILL_ACCESSORY_CHEVRON, false, screen_timeout_row_cb, NULL, NULL };
    items[5] = (pill_list_item_t){ "Screen Dimming", PILL_ACCESSORY_CHEVRON, false, screen_dimming_row_cb, NULL, NULL };
    items[6] = (pill_list_item_t){ "Swipe Up for Home", PILL_ACCESSORY_TOGGLE,
                                    current_settings.swipe_up_home_enabled, NULL, swipe_up_home_switch_event_cb, NULL };
    items[7] = (pill_list_item_t){ "Hide Player/Lyrics Top Bar", PILL_ACCESSORY_TOGGLE,
                                    current_settings.hide_player_topbar, NULL,
                                    hide_player_topbar_switch_event_cb, NULL };

    int count = 8;
    count = append_plugin_list_rows(items, count, PLUGIN_MAX_DISPLAY_LIST_ITEMS,
                                    plugin_manager_get_display_list_item_count,
                                    plugin_manager_get_display_list_item_label,
                                    plugin_manager_get_display_list_item_options,
                                    plugin_display_list_item_click_cb);

    lv_obj_t * scr = build_pill_list_screen("Display", generic_back_cb, items, count, gui_theme_accent_style(), GUI_ROW_GAP, 100);
    finalize_screen_navigation(scr);
    return scr;
}

/* Shared click handler for every plugin-registered Power list row below --
 * same index-not-object shape plugin_display_list_item_click_cb() already
 * uses. */
static void plugin_power_list_item_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    plugin_manager_power_list_item_clicked(index);
}

static lv_obj_t * build_settings_power_screen(void) {
    static pill_list_item_t items[5 + PLUGIN_MAX_POWER_LIST_ITEMS];
    items[0] = (pill_list_item_t){ "Charge Limiter (Around 85%)", PILL_ACCESSORY_TOGGLE,
                                    current_settings.charge_limiter_enabled, NULL, charge_limiter_switch_event_cb, NULL };
    items[1] = (pill_list_item_t){ "Safe Charging (500mA)", PILL_ACCESSORY_TOGGLE,
                                    current_settings.safe_charging_enabled, NULL, safe_charging_switch_event_cb, NULL };
    items[2] = (pill_list_item_t){ "Idle Shutdown", PILL_ACCESSORY_CHEVRON, false, idle_shutdown_row_cb, NULL, NULL };
    items[3] = (pill_list_item_t){ "LED charge indicator", PILL_ACCESSORY_TOGGLE,
                                    current_settings.led_indicator_enabled, NULL, led_indicator_switch_event_cb, NULL };
    items[4] = (pill_list_item_t){ "Battery Percentage", PILL_ACCESSORY_TOGGLE,
                                    current_settings.show_battery_percent, NULL, battery_percent_switch_event_cb, NULL };

    int count = 5;
    count = append_plugin_list_rows(items, count, PLUGIN_MAX_POWER_LIST_ITEMS,
                                    plugin_manager_get_power_list_item_count,
                                    plugin_manager_get_power_list_item_label,
                                    plugin_manager_get_power_list_item_options,
                                    plugin_power_list_item_click_cb);

    lv_obj_t * scr = build_pill_list_screen("Power", generic_back_cb, items, count, gui_theme_accent_style(), GUI_ROW_GAP, 100);
    finalize_screen_navigation(scr);
    return scr;
}

/* Shared click handler for every plugin-registered System list row below --
 * same index-not-object shape plugin_display_list_item_click_cb() already
 * uses. */
static void plugin_system_list_item_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    plugin_manager_system_list_item_clicked(index);
}

/* Defined later, alongside the reboot-confirm popup this opens into (see
 * hostname_entry_done_cb()) -- needed here first, for build_settings_
 * system_screen()'s own row list below. */
static void hostname_row_cb(lv_event_t * e);

static void clock_persist(void) {
    app_clock_get_persistence(&current_settings.clock_manual_epoch,
                              &current_settings.clock_system_reference);
    settings_save(&current_settings);
}

static void clock_update_set_time_enabled(void) {
    if (!clock_set_time_row) return;
    if (current_settings.clock_automatic) {
        lv_obj_clear_flag(clock_set_time_row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_opa(clock_set_time_row, LV_OPA_40, 0);
    } else {
        lv_obj_add_flag(clock_set_time_row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_opa(clock_set_time_row, LV_OPA_COVER, 0);
    }
}

static void clock_automatic_changed_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    bool automatic = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    app_clock_set_automatic(automatic);
    current_settings.clock_automatic = automatic;
    clock_update_set_time_enabled();
    clock_persist();
    refresh_clock_label();
}

static void clock_set_time_save_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int hour = (int) lv_roller_get_selected(clock_hour_roller);
    int minute = (int) lv_roller_get_selected(clock_minute_roller);
    if (!current_settings.clock_24h) {
        hour = (hour + 1) % 12;
        if (lv_roller_get_selected(clock_ampm_roller) == 1) hour += 12;
    }
    app_clock_set_local_time(hour, minute);
    current_settings.clock_automatic = false;
    clock_persist();
    refresh_clock_label();
    nav_pop();
}

static void clock_set_time_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (current_settings.clock_automatic) {
        show_info_toast("Turn off Automatic to set the clock");
        return;
    }
    struct tm local;
    app_clock_localtime(&local);
    if (current_settings.clock_24h) {
        lv_roller_set_options(clock_hour_roller,
            "00\n01\n02\n03\n04\n05\n06\n07\n08\n09\n10\n11\n12\n13\n14\n15\n16\n17\n18\n19\n20\n21\n22\n23",
            LV_ROLLER_MODE_NORMAL);
        lv_roller_set_selected(clock_hour_roller, (uint32_t) local.tm_hour, LV_ANIM_OFF);
        lv_obj_add_flag(clock_ampm_roller, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_roller_set_options(clock_hour_roller, "01\n02\n03\n04\n05\n06\n07\n08\n09\n10\n11\n12",
                              LV_ROLLER_MODE_NORMAL);
        lv_roller_set_selected(clock_hour_roller, (uint32_t) ((local.tm_hour + 11) % 12), LV_ANIM_OFF);
        lv_roller_set_selected(clock_ampm_roller, local.tm_hour >= 12 ? 1 : 0, LV_ANIM_OFF);
        lv_obj_remove_flag(clock_ampm_roller, LV_OBJ_FLAG_HIDDEN);
    }
    lv_roller_set_selected(clock_minute_roller, (uint32_t) local.tm_min, LV_ANIM_OFF);
    nav_push(clock_set_time_screen);
}

static void clock_settings_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(clock_screen);
}

static lv_obj_t * build_clock_screen(void) {
    pill_list_item_t items[] = {
        { "Automatic", PILL_ACCESSORY_TOGGLE, current_settings.clock_automatic,
          NULL, clock_automatic_changed_cb, NULL },
        { .label = "Set Time", .accessory = PILL_ACCESSORY_CHEVRON,
          .on_click = clock_set_time_row_cb, .out_row = &clock_set_time_row },
        { "24-Hour Clock", PILL_ACCESSORY_TOGGLE, current_settings.clock_24h,
          NULL, clock_24h_switch_event_cb, NULL },
        { .label = "Time Zone", .accessory = PILL_ACCESSORY_CHEVRON,
          .on_click = timezone_settings_row_cb, .out_row = &clock_timezone_row },
    };
    lv_obj_t * scr = build_pill_list_screen("Clock", generic_back_cb, items, 4,
gui_theme_accent_style(), GUI_ROW_GAP, 100);
    if (clock_timezone_row) {
        lv_obj_t * title = lv_obj_get_child(clock_timezone_row, 0);
        if (title) lv_obj_align(title, LV_ALIGN_LEFT_MID, 24, -18);
        clock_timezone_value_label = lv_label_create(clock_timezone_row);
        lv_obj_add_style(clock_timezone_value_label, &style_theme_text_muted, 0);
        lv_obj_set_style_text_font(clock_timezone_value_label,
                                   gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
        configure_scrolling_row_label(clock_timezone_value_label, 370);
        lv_obj_align(clock_timezone_value_label, LV_ALIGN_LEFT_MID, 24, 23);
        clock_timezone_indicator_update();
    }
    clock_update_set_time_enabled();
    finalize_screen_navigation(scr);
    return scr;
}

static lv_obj_t * build_clock_set_time_screen(void) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_add_style(scr, &style_theme_screen_bg, 0);
    build_screen_header(scr, "Set Time", generic_back_cb, NULL, NULL);

    lv_obj_t * row = lv_obj_create(scr);
    lv_obj_set_size(row, lv_pct(92), BOARD_SCALE_PX(360));
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT + 18);
    lv_obj_add_style(row, &style_theme_card_bg, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    clock_hour_roller = lv_roller_create(row);
    clock_minute_roller = lv_roller_create(row);
    clock_ampm_roller = lv_roller_create(row);
    lv_roller_set_options(clock_minute_roller,
        "00\n01\n02\n03\n04\n05\n06\n07\n08\n09\n10\n11\n12\n13\n14\n15\n16\n17\n18\n19\n20\n21\n22\n23\n24\n25\n26\n27\n28\n29\n30\n31\n32\n33\n34\n35\n36\n37\n38\n39\n40\n41\n42\n43\n44\n45\n46\n47\n48\n49\n50\n51\n52\n53\n54\n55\n56\n57\n58\n59",
        LV_ROLLER_MODE_NORMAL);
    lv_roller_set_options(clock_ampm_roller, "AM\nPM", LV_ROLLER_MODE_NORMAL);
    lv_obj_t * rollers[] = { clock_hour_roller, clock_minute_roller, clock_ampm_roller };
    for (int i = 0; i < 3; i++) {
        lv_obj_set_size(rollers[i], i == 2 ? BOARD_SCALE_PX(105) : BOARD_SCALE_PX(120), BOARD_SCALE_PX(300));
        lv_obj_set_style_text_font(rollers[i], gui_theme_font(GUI_FONT_ROLE_TITLE), 0);
        lv_obj_add_style(rollers[i], gui_theme_accent_style(), LV_PART_SELECTED);
        /* style_accent deliberately sets both background and text to the
         * accent color (needed by several non-roller users), which makes a
         * roller's selected value disappear into its selection band. Keep
         * the accent background but explicitly restore themed primary text
         * for the selected part; adding this after the accent style gives
         * it precedence and still follows live theme color changes. */
        lv_obj_add_style(rollers[i], &style_theme_text_primary, LV_PART_SELECTED);
    }

    lv_obj_t * save = lv_button_create(scr);
    lv_obj_set_size(save, BOARD_SCALE_PX(220), BOARD_SCALE_PX(78));
    lv_obj_align_to(save, row, LV_ALIGN_OUT_BOTTOM_MID, 0, 28);
    lv_obj_add_style(save, gui_theme_accent_style(), 0);
    lv_obj_add_event_cb(save, clock_set_time_save_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * save_label = lv_label_create(save);
    lv_label_set_text(save_label, "Save");
    lv_obj_add_style(save_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(save_label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_obj_center(save_label);
    finalize_screen_navigation(scr);
    return scr;
}

static lv_obj_t * build_settings_system_screen(void) {
    static pill_list_item_t items[5 + PLUGIN_MAX_SYSTEM_LIST_ITEMS];
    items[0] = (pill_list_item_t){ "Clock", PILL_ACCESSORY_CHEVRON, false,
                                    clock_settings_row_cb, NULL, NULL };
    items[1] = (pill_list_item_t){ "USB Mode", PILL_ACCESSORY_CHEVRON, false, usb_mode_settings_row_cb, NULL, NULL };
    items[2] = (pill_list_item_t){ "Hostname", PILL_ACCESSORY_CHEVRON, false, hostname_row_cb, NULL, NULL };
    items[3] = (pill_list_item_t){ "Plugin Manager", PILL_ACCESSORY_CHEVRON, false, gui_plugin_manage_row_cb, NULL, NULL };
    items[4] = (pill_list_item_t){ "Factory Reset", PILL_ACCESSORY_NONE, false, factory_reset_btn_cb, NULL, NULL };

    int count = 5;
    count = append_plugin_list_rows(items, count, PLUGIN_MAX_SYSTEM_LIST_ITEMS,
                                    plugin_manager_get_system_list_item_count,
                                    plugin_manager_get_system_list_item_label,
                                    plugin_manager_get_system_list_item_options,
                                    plugin_system_list_item_click_cb);

    lv_obj_t * scr = build_pill_list_screen("System", generic_back_cb, items, count, gui_theme_accent_style(), GUI_ROW_GAP, 100);
    finalize_screen_navigation(scr);
    return scr;
}

static void settings_category_music_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(settings_music_screen);
}

static void settings_category_display_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(settings_display_screen);
}

static void settings_category_power_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(settings_power_screen);
}

static void settings_category_system_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(settings_system_screen);
}

static void settings_about_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(about_screen);
}

/* Auto-resume on launch has no settings row while AUTO_RESUME_ON_LAUNCH_ENABLED
 * is disabled at compile time. current_settings.auto_resume_enabled and
 * auto_resume_switch_event_cb remain available for when re-enabled. */
/* Shared click handler for every plugin-registered Settings list row below
 * -- same index-not-object shape plugin_books_list_item_click_cb() already
 * uses for the Books screen. */
static void plugin_settings_list_item_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    plugin_manager_settings_list_item_clicked(index);
}

static lv_obj_t * build_settings_screen(void) {
    static pill_list_item_t items[5 + PLUGIN_MAX_SETTINGS_LIST_ITEMS];
    lv_obj_t * category_rows[5] = { NULL };
    items[0] = (pill_list_item_t){ "Music Settings", PILL_ACCESSORY_CHEVRON, false, settings_category_music_cb, NULL, NULL };
    items[1] = (pill_list_item_t){ "Display", PILL_ACCESSORY_CHEVRON, false, settings_category_display_cb, NULL, NULL };
    items[2] = (pill_list_item_t){ "Power", PILL_ACCESSORY_CHEVRON, false, settings_category_power_cb, NULL, NULL };
    items[3] = (pill_list_item_t){ "System", PILL_ACCESSORY_CHEVRON, false, settings_category_system_cb, NULL, NULL };
    items[4] = (pill_list_item_t){ "About", PILL_ACCESSORY_CHEVRON, false, settings_about_row_cb, NULL, NULL };
    for (unsigned i = 0; i < 5; ++i) items[i].out_row = &category_rows[i];

    int count = 5;
    count = append_plugin_list_rows(items, count, PLUGIN_MAX_SETTINGS_LIST_ITEMS,
                                    plugin_manager_get_settings_list_item_count,
                                    plugin_manager_get_settings_list_item_label,
                                    plugin_manager_get_settings_list_item_options,
                                    plugin_settings_list_item_click_cb);

    lv_obj_t * scr = build_pill_list_screen("Settings", generic_back_cb, items, count, gui_theme_accent_style(), GUI_ROW_GAP, 100);
    static const char * names[] = { "music", "display", "power", "system", "about" };
    for (unsigned i = 0; i < 5; ++i) {
        char icon[64];
        snprintf(icon, sizeof(icon), "settings/%s.png", names[i]);
        if (category_rows[i]) decorate_category_row(category_rows[i], icon, NULL);
        items[i].out_row = NULL;
    }
    finalize_screen_navigation(scr);
    return scr;
}

static void music_tile_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(gui_library_get_music_screen());
}

static void stream_media_tile_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(stream_media_screen);
}

static void wireless_tile_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(gui_network_get_wireless_screen());
}



static void settings_tile_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(settings_screen);
}

/* ---- DAC home screen -- direct shortcut to enabling USB DAC or Bluetooth
 * DAC without going through Settings > System / Wireless > Bluetooth first.
 * Reuses the exact same underlying entry points those screens already use
 * (start_usb_mode_switch()/open_bt_dac_screen()) rather than duplicating
 * any of that state machine. Replaced the home screen's old "About" tile --
 * About is still reachable, just moved into Settings (see
 * settings_about_row_cb()). ---- */

static void dac_home_usb_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    /* If USB DAC is already active in hardware, show its overlay directly. */
    usb_mode_t detected;
    bool have_detected = usb_mode_control_detect_current(&detected);
    if (have_detected && detected == USB_MODE_DAC) {
        current_settings.usb_mode = (int) USB_MODE_DAC;
        nav_push(gui_network_get_usb_dac_overlay());
        return;
    }
    /* Require explicit disable of ADB mode before switching to USB DAC. */
    if (have_detected && detected == USB_MODE_ADB) {
        show_info_toast("Turn off ADB first (Settings > System > USB Mode), then enable USB DAC from here.");
        return;
    }
    start_usb_mode_switch(USB_MODE_DAC);
}

lv_obj_t * build_dac_home_screen(void) {
    const icon_grid_item_t items[] = {
        { "submenu/usb.png", NULL, "USB DAC", dac_home_usb_row_cb, NULL },
        { "submenu/bluetooth.png", NULL, "Bluetooth DAC", bt_dac_settings_row_cb, NULL },
    };
    lv_obj_t * scr = build_category_menu_screen("DAC", generic_back_cb, items, 2, NULL);
    finalize_screen_navigation(scr);
    return scr;
}

static void dac_home_tile_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(gui_shell_get_dac_home_screen());
}


/* Published order for plugin.set_home_layout()'s `key` field (home_layout.h)
 * -- the one and only place this order is spelled out; plugin_manager.c
 * validates a Lua `key` string against this same array rather than keeping
 * its own copy that could drift out of sync. */
const char * const home_layout_tile_keys[HOME_LAYOUT_TILE_COUNT] = {
    "music", "stream_media", "wireless", "books", "settings", "dac", "subsonic",
};

/* Native per-tile metadata, same fixed order as home_layout_tile_keys[]
 * above -- shared by both branches of build_home_screen() below so the
 * label/icon/click-target triple is written exactly once regardless of
 * which mode (tile or list) actually renders it. */
typedef struct {
    const char * icon_asset;
    const char * icon_asset_selected;
    const char * label;
    lv_event_cb_t on_click;
    uint32_t glow_color; /* 0 leaves the icon's background transparent. */
} home_native_tile_t;

static const home_native_tile_t home_native_tiles[HOME_LAYOUT_TILE_COUNT] = {
    { "launcher/music.png", "launcher/music_s.png", "Music", music_tile_cb, 0xF5B457 },
    { "launcher/stream_media.png", "launcher/stream_media_s.png", "Stream Media", stream_media_tile_cb, 0x48ADE5 },
    { "launcher/wireless.png", "launcher/wireless_s.png", "Wireless", wireless_tile_cb, 0x39BD95 },
    { "launcher/book.png", "launcher/book_s.png", "Books", gui_books_home_tile_cb, 0xF5B457 },
    { "launcher/sys_set.png", "launcher/sys_set_s.png", "Settings", settings_tile_cb, 0x909EB5 },
    { "launcher/dac.png", "launcher/dac_s.png", "DAC", dac_home_tile_cb, 0xA36BE4 },
    { "stream_media/subsonic.png", "stream_media/subsonic_s.png", "Subsonic", subsonic_tile_cb, 0 },
};

/* One resolved entry (native tile or plugin-registered tile), independent
 * of tile-mode/list-mode -- resolve_home_tiles() below fills this once, and
 * both build_home_screen() branches read from it, so the native-vs-plugin
 * resolution and style-override lookup are each written exactly once. */
typedef struct {
    const char * icon_asset;
    const char * icon_asset_selected;
    const char * label;
    lv_event_cb_t on_click;
    void * user_data;
    const home_tile_override_t * override; /* NULL = never restyled, every native default applies */
    uint32_t glow_color; /* 0 for plugin tiles */
} resolved_home_tile_t;

static void plugin_home_tile_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    plugin_manager_home_tile_clicked(index);
}

/* home_layout_config.tiles[]/tile_count is a flat, unordered list keyed by
 * name (native key or plugin tile id) -- see home_layout.h's own comment.
 * Returns NULL, not a zeroed override, when `key` was never restyled, so
 * callers can tell "no override" apart from "an override that happens to
 * leave everything at its default" -- in practice both resolve the same
 * (every has_* false), so this distinction currently only matters for
 * clarity, not behavior. */
static const home_tile_override_t * find_home_tile_override(const char * key) {
    for (int i = 0; i < home_layout_config.tile_count; i++) {
        if (strcmp(home_layout_config.tiles[i].key, key) == 0) return &home_layout_config.tiles[i].override;
    }
    return NULL;
}

/* Resolves home_layout_config.order[] (or, when unconfigured, today's fixed
 * 6-native-tile order) into `out`, which must have room for
 * HOME_LAYOUT_MAX_TILES entries. Returns how many entries were actually
 * filled -- can be less than the order length when an entry names neither a
 * native key nor a currently-registered plugin tile id (that plugin failed
 * to load, or hasn't loaded yet -- see set_home_layout()'s own comment on
 * why this isn't rejected earlier, at Lua-call time); such an entry is
 * skipped and logged here, not treated as an error. This is also why this
 * resolution must happen here rather than in l_plugin_set_home_layout()
 * itself -- this always runs after plugin_manager_init() has finished
 * loading every plugin (gui_reload.c's own reload sequence runs plugin_
 * manager_init() before rebuilding any screen), so a plugin tile referenced
 * by an earlier-loading theme is reliably resolvable by the time Home is
 * actually built. */
static int resolve_home_tiles(resolved_home_tile_t * out) {
    int n = home_layout_config.order_count > 0 ? home_layout_config.order_count : HOME_LAYOUT_DEFAULT_TILE_COUNT;
    int count = 0;
    for (int i = 0; i < n; i++) {
        const char * key = home_layout_config.order_count > 0 ? home_layout_config.order[i] : home_layout_tile_keys[i];

        int native_idx = -1;
        for (int k = 0; k < HOME_LAYOUT_TILE_COUNT; k++) {
            if (strcmp(key, home_layout_tile_keys[k]) == 0) { native_idx = k; break; }
        }

        resolved_home_tile_t * r = &out[count];
        if (native_idx >= 0) {
            const home_native_tile_t * native = &home_native_tiles[native_idx];
            r->icon_asset = native->icon_asset;
            r->icon_asset_selected = native->icon_asset_selected;
            r->label = native->label;
            r->on_click = native->on_click;
            r->user_data = NULL;
            r->glow_color = native->glow_color;
        } else {
            int plugin_idx = plugin_manager_find_home_tile_by_id(key);
            if (plugin_idx < 0) {
                fprintf(stderr, "[home] order entry '%s' is not a native tile or a currently-registered "
                                "plugin.register_home_tile() id -- skipping\n", key);
                continue;
            }
            r->icon_asset = plugin_manager_get_home_tile_icon(plugin_idx);
            r->icon_asset_selected = plugin_manager_get_home_tile_icon_selected(plugin_idx);
            r->label = plugin_manager_get_home_tile_label(plugin_idx);
            r->on_click = plugin_home_tile_click_cb;
            r->user_data = (void *) (intptr_t) plugin_idx;
            r->glow_color = 0;
        }
        r->override = find_home_tile_override(key);
        count++;
    }
    return count;
}

/* options.background_image (PLUGINS.md, plugin.set_home_layout()) -- sets
 * Home's OWN root object's bg_image_src directly, not style_theme_screen_bg
 * (shared by every screen in the app, mutated by plugin.set_background_
 * color("screen", ...)). scr's grid/tile children are already transparent
 * (build_icon_grid_screen()'s own bg_opa=0 on both, screen_builders.c)
 * unless a per-tile bg_color override says otherwise, so this shows through
 * cleanly behind them with no other change needed. Called from both
 * build_home_screen() branches below, right after each builds its own scr,
 * so a plugin-set background applies whether Home is tile or list mode. */
static void apply_home_background_image(lv_obj_t * scr) {
    if (!home_layout_config.configured || !home_layout_config.has_background_image) return;
    lv_obj_set_style_bg_image_src(scr, asset_path(home_layout_config.background_image), 0);
}

lv_obj_t * build_home_screen(void) {
    static resolved_home_tile_t resolved[HOME_LAYOUT_MAX_TILES];
    int count = resolve_home_tiles(resolved);
    const home_tile_override_t zero_override = { 0 };

    /* plugin.set_home_layout()'s list-mode path -- a pill-list screen built
     * from the resolved tiles above instead of the icon grid, with each
     * row's style pulled from its own override. See home_layout.h's own
     * comment for why this only ever reflects whatever was configured at
     * THIS boot's plugin-load time (or the most recent plugin.refresh_
     * theme()/reload_ui()), never a live mid-session change outside that. */
    if (home_layout_config.configured && home_layout_config.list_mode) {
        static icon_grid_item_t items[HOME_LAYOUT_MAX_TILES];
        for (int i = 0; i < count; i++) {
            const home_tile_override_t * ov = resolved[i].override ? resolved[i].override : &zero_override;
            items[i] = (icon_grid_item_t){
                .icon_asset = resolved[i].icon_asset,
                .label = resolved[i].label,
                .on_click = resolved[i].on_click,
                .user_data = resolved[i].user_data,
                .has_bg_color = ov->has_bg_color, .bg_color = ov->bg_color,
                .has_text_color = ov->has_text_color, .text_color = ov->text_color,
                .has_radius = ov->has_radius, .radius = ov->radius,
                /* Every field below is a per-tile LIST-MODE override
                 * (icon_grid_item_t's own doc comment, screen_builders.h) --
                 * always supplied here (has_* = true) since Home's per-tile
                 * defaults (no accessory/icon unless explicitly overridden)
                 * differ from build_launcher_menu_screen()'s own layout-level
                 * defaults, so there is no shared `layout` value worth
                 * falling back to. */
                .has_row_height = true, .row_height = ov->height,
                .has_row_width = true, .row_width = ov->width,
                .has_accessory = true, .accessory = ov->has_accessory && ov->accessory,
                .text_size = ov->text_size[0] ? ov->text_size : NULL,
                .text_align = ov->align[0] ? ov->align : NULL,
                .has_icon = true, .icon = ov->has_icon && ov->icon,
            };
        }

        /* No per-screen style here (that lives entirely in each item's own
         * override above) -- this layout only carries what's genuinely
         * shared across every tile: list mode itself and the row gap. */
        launcher_menu_layout_t home_list_layout = {
            .list_mode = true,
            .row_gap = home_layout_config.row_gap > 0 ? home_layout_config.row_gap : 6,
        };
        lv_obj_t * scr = build_launcher_menu_screen(NULL, NULL, items, count, BOARD_SCALE_PX(100), false, &home_list_layout);
        apply_home_background_image(scr);
        finalize_screen_navigation(scr);
        return scr;
    }

    static icon_grid_item_t items[HOME_LAYOUT_MAX_TILES];
    for (int i = 0; i < count; i++) {
        const home_tile_override_t * ov = resolved[i].override ? resolved[i].override : &zero_override;
        items[i] = (icon_grid_item_t){
            .icon_asset = resolved[i].icon_asset,
            .icon_asset_selected = resolved[i].icon_asset_selected,
            .label = resolved[i].label,
            .on_click = resolved[i].on_click,
            .user_data = resolved[i].user_data,
            .has_bg_color = ov->has_bg_color, .bg_color = ov->bg_color,
            .has_text_color = ov->has_text_color, .text_color = ov->text_color,
            .has_radius = ov->has_radius, .radius = ov->radius,
            /* Explicit plugin surfaces take precedence over native glows. */
            .icon_glow_color = ov->has_bg_color ? 0 : resolved[i].glow_color,
        };
    }
    /* No back_btn_cb -- this is the true root, nothing to go back to. No
     * title either -- matches the real stock launcher, which has no header
     * text above its icon grid. tile_gap stays 0 (today's exact flush-cell
     * look) unless a plugin configured one. l_plugin_set_home_layout()
     * already rejects a tile-mode `order` past 6 entries, so `count` here
     * never exceeds what build_icon_grid_screen()'s own row math expects. */
    lv_obj_t * scr = build_icon_grid_screen(NULL, NULL, items, count, BOARD_SCALE_PX(100), false,
                                             home_layout_config.configured ? home_layout_config.tile_gap : 0);
    apply_home_background_image(scr);
    finalize_screen_navigation(scr);
    return scr;
}

/* EQ slider card with value label on top and slider track below.
 * Card uses flex-column flow with content-based sizing so card height
 * dynamically adjusts to the active font tier without overlapping. */
static lv_obj_t * create_eq_slider_card(lv_obj_t * parent, eq_field_t field, lv_obj_t ** out_value_label,
                                        lv_obj_t ** out_slider, int32_t range_min, int32_t range_max) {
    lv_obj_t * card = lv_obj_create(parent);
    lv_obj_set_width(card, lv_pct(96));
    /* Flex column with content height accommodates varying font heights. */
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_add_style(card, &style_theme_card_bg, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_pad_all(card, 14, 0);
    lv_obj_set_style_pad_row(card, 10, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);

    /* Clickable (tap-to-edit, see eq_field_label_click_cb()) -- the whole
     * label area is the tap target, not just the text glyphs, via
     * lv_obj_set_ext_click_area() below. */
    lv_obj_t * value_label = lv_label_create(card);
    lv_obj_add_style(value_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(value_label, gui_theme_font(GUI_FONT_ROLE_ROW), 0);
    lv_obj_add_flag(value_label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(value_label, 16);
    lv_obj_add_event_cb(value_label, eq_field_label_click_cb, LV_EVENT_CLICKED, (void *) (intptr_t) field);

    /* Inset track width so the knob radius stays within the card at slider extremes. */
    lv_obj_t * slider = lv_slider_create(card);
    lv_obj_set_width(slider, lv_pct(88));
    lv_obj_set_height(slider, SLIDER_TRACK_HEIGHT);
    lv_slider_set_range(slider, range_min, range_max);
    lv_obj_add_style(slider, gui_theme_accent_style(), LV_PART_INDICATOR);
    lv_obj_add_style(slider, gui_theme_accent_knob_style(), LV_PART_KNOB);
    lv_obj_set_style_width(slider, SLIDER_KNOB_SIZE, LV_PART_KNOB);
    lv_obj_set_style_height(slider, SLIDER_KNOB_SIZE, LV_PART_KNOB);
    lv_obj_set_ext_click_area(slider, 20);

    *out_value_label = value_label;
    *out_slider = slider;
    return card;
}

/* ---- PEQ named profiles (save/load to/from the SD card) ----
 *
 * peq.c's own peq_load()/peq_save() always target one fixed, always-current
 * file (loaded at startup, saved on every change) -- these profiles are a
 * separate, explicit "keep this exact setup around under a name" action,
 * living in their own SD card folder rather than mixed in with music. */
static lv_obj_t * eq_profiles_list;
static lv_obj_t * eq_profiles_title_label;
/* Name of the named profile whose values are currently loaded.  The PEQ
 * engine deliberately knows only about values and its always-current
 * autosave file, so the UI owns this bit of presentation state.  Saving
 * with this unchanged overwrites that profile; editing it creates a new
 * profile and makes the new name current. */
static char eq_current_profile_name[256];

static void eq_set_current_profile_from_path(const char * path) {
    const char * name = basename_of(path);
    snprintf(eq_current_profile_name, sizeof(eq_current_profile_name), "%s", name ? name : "");
    char * dot = strrchr(eq_current_profile_name, '.');
    if (dot && strcmp(dot, ".peq") == 0) *dot = '\0';
}

/* Refreshes every widget on the EQ screen from peq.c's current state --
 * shared by the initial build and by "Load Profile" (which changes
 * everything at once, unlike the individual per-field setters that only
 * ever touch one widget). */
static void refresh_all_eq_widgets(void) {
    if (peq_get_bypass()) lv_obj_clear_state(eq_bypass_switch, LV_STATE_CHECKED);
    else lv_obj_add_state(eq_bypass_switch, LV_STATE_CHECKED);
    lv_slider_set_value(eq_preamp_slider, (int32_t) (peq_get_preamp_db() * 10.0), LV_ANIM_OFF);
    lv_label_set_text_fmt(eq_preamp_value_label, "Pre-Amp: %+.2f dB", peq_get_preamp_db());
    refresh_eq_band_widgets();
}

/* ---- Reset PEQ to defaults, with a confirmation popup -- same hand-built
 * top-layer overlay shape as bt_dac_leave_popup (this codebase doesn't use
 * LVGL's lv_msgbox anywhere), since a factory reset of every band's
 * freq/gain/Q/type plus the preamp is not something a stray tap should be
 * able to trigger by accident. ---- */
static gui_popup_t eq_reset_popup;

static void hide_eq_reset_popup(void) {
    gui_popup_hide(&eq_reset_popup);
}

static void eq_reset_popup_backdrop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_eq_reset_popup();
}

static void eq_reset_cancel_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_eq_reset_popup();
}

static void eq_reset_confirm_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_eq_reset_popup();
    peq_reset_to_defaults();
    eq_current_profile_name[0] = '\0';
    refresh_all_eq_widgets();
    peq_save();
    show_error_toast("PEQ reset to defaults");
}

static void eq_reset_btn_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    gui_popup_show(&eq_reset_popup);
}

static void build_eq_reset_popup(void) {
    eq_reset_popup.popup = build_confirm_popup("Reset PEQ to defaults?", LV_LABEL_LONG_WRAP, NULL, NULL, "Reset",
                                                lv_color_make(255, 120, 120), eq_reset_confirm_cb, NULL, "Cancel",
                                                accent_lv_color(), eq_reset_cancel_cb, NULL, eq_reset_popup_backdrop_cb,
                                                &eq_reset_popup.backdrop);
}

/* ---- Factory Reset, with a confirmation popup -- same hand-built
 * top-layer overlay shape as eq_reset_popup right above (this codebase
 * doesn't use LVGL's lv_msgbox anywhere). Wiping every app setting is
 * exactly the kind of thing a stray tap must never be able to trigger.
 * Reboots immediately on confirm (settings_factory_reset() deletes the
 * settings file and returns -- see its own comment in settings.h for why
 * nothing here tries to hot-apply the reset settings instead of just
 * rebooting into them fresh). ---- */
static gui_popup_t factory_reset_popup;

static void hide_factory_reset_popup(void) {
    gui_popup_hide(&factory_reset_popup);
}

static void factory_reset_popup_backdrop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_factory_reset_popup();
}

static void factory_reset_cancel_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_factory_reset_popup();
}

static void factory_reset_confirm_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_factory_reset_popup();
    settings_factory_reset(); /* deletes the settings file and reboots -- see its own comment in settings.h/.c */
}

static void factory_reset_btn_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    gui_popup_show(&factory_reset_popup);
}

static void build_factory_reset_popup(void) {
    factory_reset_popup.popup = build_confirm_popup(
        "Reset all settings and reboot?", LV_LABEL_LONG_WRAP, NULL, NULL, "Reset", lv_color_make(255, 120, 120),
        factory_reset_confirm_cb, NULL, "Cancel", accent_lv_color(), factory_reset_cancel_cb, NULL,
        factory_reset_popup_backdrop_cb, &factory_reset_popup.backdrop);
}

/* Settings -> System -> Hostname uses the shared confirmation-popup helper;
 * see hostname_apply()'s own comment for why a reboot is genuinely required
 * here (wifi_on.sh/bt_init each only read their file once, on demand). */
static gui_popup_t hostname_reboot_popup;

static void hide_hostname_reboot_popup(void) {
    gui_popup_hide(&hostname_reboot_popup);
}

static void hostname_reboot_popup_backdrop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_hostname_reboot_popup();
}

static void hostname_reboot_later_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_hostname_reboot_popup();
}

static void hostname_reboot_now_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_hostname_reboot_popup();
    char * reboot_argv[] = { (char *) "/sbin/reboot", NULL };
    subprocess_run(reboot_argv, NULL, 0);
}

static void show_hostname_reboot_popup(void) {
    gui_popup_show(&hostname_reboot_popup);
}

static void build_hostname_reboot_popup(void) {
    hostname_reboot_popup.popup = build_confirm_popup(
        "Restart now to apply the new hostname?", LV_LABEL_LONG_WRAP, NULL, NULL, "Restart Now", accent_lv_color(),
        hostname_reboot_now_cb, NULL, "Later", lv_color_make(160, 160, 160), hostname_reboot_later_cb, NULL,
        hostname_reboot_popup_backdrop_cb, &hostname_reboot_popup.backdrop);
}

/* RFC 952/1123 hostname-label charset -- letters/digits/hyphen only, no
 * leading/trailing hyphen. Real bug caught in review: nothing validated
 * this before it was written straight to /usr/data/hostname_override.txt
 * and handed to sethostname() (see hostname_apply.c) -- a space or
 * punctuation typed on the on-screen keyboard would produce an invalid
 * WiFi/BT broadcast name, or corrupt whatever naive parsing a downstream
 * consumer of those two bind-mounted files does. Empty is exempted --
 * that's hostname_entry_done_cb()'s own "reset to stock" sentinel below,
 * not a real hostname. */
static bool hostname_is_valid(const char * text) {
    size_t len = strlen(text);
    if (len == 0) return true;
    if (len > 63) return false;
    if (text[0] == '-' || text[len - 1] == '-') return false;
    for (size_t i = 0; i < len; i++) {
        char c = text[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-')) return false;
    }
    return true;
}

static void hostname_entry_done_cb(const char * text, void * user_data) {
    (void) user_data;
    if (!hostname_is_valid(text)) {
        show_error_toast("Hostname can only use letters, numbers, and hyphens");
        return;
    }
    /* Empty submission means "reset to the stock name" -- hostname_apply()
     * itself treats an empty string as a no-op (leaves whatever's already
     * in effect from a previous boot alone), so resetting to stock also
     * needs a reboot back to the un-overridden squashfs file, same as
     * setting a new one does. */
    snprintf(current_settings.hostname, sizeof(current_settings.hostname), "%s", text);
    settings_save(&current_settings);
    show_hostname_reboot_popup();
}

static void hostname_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    show_text_entry("Hostname", current_settings.hostname, false, false, hostname_entry_done_cb, NULL);
}

static char ** eq_profile_paths = NULL;
static int eq_profile_count = 0;
static bool eq_profiles_edit_mode = false;
/* When true, populate_eq_profiles_screen() wires rows to overwrite the
 * tapped profile with the CURRENT PEQ values instead of loading it --
 * entered only via the Save flow's "Replace Existing" choice (see
 * eq_open_save_choice_popup()), never a standalone navigation target. */
static bool eq_profiles_replace_mode = false;
static lv_obj_t * eq_profiles_edit_btn = NULL;
static char eq_profile_pending_path[512];
static gui_popup_t eq_profile_delete_popup;

static void eq_profiles_free_paths(void) {
    for (int i = 0; i < eq_profile_count; i++) free(eq_profile_paths[i]);
    free(eq_profile_paths);
    eq_profile_paths = NULL;
    eq_profile_count = 0;
}

static bool eq_profile_name_is_valid(const char * text) {
    if (!text || text[0] == '\0') return false;
    if (strchr(text, '/') || strchr(text, '\\')) return false;
    return true;
}

static void eq_profile_path_from_name(char * out, size_t out_size, const char * name) {
    snprintf(out, out_size, "%s/%s.peq", PEQ_PROFILES_DIR, name);
}

static bool eq_save_named_profile(const char * name) {
    if (!eq_profile_name_is_valid(name)) {
        show_error_toast("Invalid profile name");
        return false;
    }
    mkdir(PEQ_PROFILES_DIR, 0755);
    char path[512];
    eq_profile_path_from_name(path, sizeof(path), name);
    if (!peq_save_to_path(path)) {
        show_error_toast("Failed to save profile");
        return false;
    }
    snprintf(eq_current_profile_name, sizeof(eq_current_profile_name), "%s", name);
    show_info_toast("Profile saved");
    return true;
}

static void eq_profile_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    const char * path = (const char *) lv_event_get_user_data(e);
    if (!peq_load_from_path(path)) {
        show_error_toast("Failed to load profile");
        return;
    }
    eq_set_current_profile_from_path(path);
    refresh_all_eq_widgets();
    peq_save();
    nav_pop();
    show_info_toast("Profile loaded");
}

/* Replace-mode row tap: overwrites the tapped profile's file with the
 * CURRENT PEQ values (the opposite direction of eq_profile_row_cb's load) --
 * only reachable via the Save flow's "Replace Existing" choice, so tapping a
 * row here is already the user's one deliberate confirming action, same as
 * a normal in-place Save never asking again.
 * Writes to `path` directly (already a real, scanned, existing file -- no
 * need to re-validate a name or reconstruct it via eq_profile_path_from_name())
 * and only commits eq_current_profile_name/leaves the screen on success --
 * committing identity before the write could succeed would leave a FAILED
 * overwrite (SD full, unmount mid-write) pointing the next in-place Save at
 * the tapped file instead of whatever was actually current before this tap. */
static void eq_profile_replace_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    const char * path = (const char *) lv_event_get_user_data(e);
    if (!peq_save_to_path(path)) {
        show_error_toast("Failed to save profile");
        return;
    }
    eq_set_current_profile_from_path(path);
    show_info_toast("Profile saved");
    nav_pop();
}

static void hide_eq_profile_delete_popup(void) {
    gui_popup_hide(&eq_profile_delete_popup);
}

static void eq_profile_delete_popup_backdrop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_eq_profile_delete_popup();
}

static void eq_profile_delete_cancel_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_eq_profile_delete_popup();
}

static void populate_eq_profiles_screen(void);

static void eq_profile_delete_confirm_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_eq_profile_delete_popup();
    if (eq_profile_pending_path[0] == '\0') return;
    if (unlink(eq_profile_pending_path) != 0) {
        show_error_toast("Failed to delete profile");
        return;
    }
    char stem[256];
    snprintf(stem, sizeof(stem), "%s", eq_current_profile_name);
    eq_set_current_profile_from_path(eq_profile_pending_path);
    if (strcmp(eq_current_profile_name, stem) == 0) eq_current_profile_name[0] = '\0';
    else snprintf(eq_current_profile_name, sizeof(eq_current_profile_name), "%s", stem);
    eq_profile_pending_path[0] = '\0';
    populate_eq_profiles_screen();
    show_info_toast("Profile deleted");
}

static void eq_profile_delete_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    const char * path = (const char *) lv_event_get_user_data(e);
    snprintf(eq_profile_pending_path, sizeof(eq_profile_pending_path), "%s", path);
    gui_popup_show(&eq_profile_delete_popup);
}

static void eq_profile_rename_done_cb(const char * text, void * user_data) {
    const char * old_path = (const char *) user_data;
    if (!eq_profile_name_is_valid(text)) {
        show_error_toast("Invalid profile name");
        return;
    }
    char new_path[512];
    eq_profile_path_from_name(new_path, sizeof(new_path), text);
    if (strcmp(old_path, new_path) == 0) return;
    if (rename(old_path, new_path) != 0) {
        show_error_toast("Failed to rename profile");
        return;
    }
    char previous[256];
    snprintf(previous, sizeof(previous), "%s", eq_current_profile_name);
    eq_set_current_profile_from_path(old_path);
    if (strcmp(eq_current_profile_name, previous) == 0)
        snprintf(eq_current_profile_name, sizeof(eq_current_profile_name), "%s", text);
    else
        snprintf(eq_current_profile_name, sizeof(eq_current_profile_name), "%s", previous);
    populate_eq_profiles_screen();
    show_info_toast("Profile renamed");
}

static void eq_profile_rename_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    const char * path = (const char *) lv_event_get_user_data(e);
    char display[256];
    snprintf(display, sizeof(display), "%s", basename_of(path));
    char * dot = strrchr(display, '.');
    if (dot && strcmp(dot, ".peq") == 0) *dot = '\0';
    show_text_entry("Rename Profile", display, false, false, eq_profile_rename_done_cb, (void *) path);
}

static void populate_eq_profiles_screen(void) {
    lv_obj_clean(eq_profiles_list);
    eq_profiles_free_paths();

    if (eq_profiles_title_label)
        lv_label_set_text(eq_profiles_title_label, eq_profiles_replace_mode ? "Replace Profile" : "Profiles");
    if (eq_profiles_edit_btn) {
        lv_label_set_text(eq_profiles_edit_btn, eq_profiles_edit_mode ? "Done" : "Edit");
        /* Rename/delete don't make sense mid-replace -- hide the entry point
         * entirely rather than let a mode switch there leave replace_mode
         * armed underneath a "Done" tap that returns to it unexpectedly. */
        if (eq_profiles_replace_mode) lv_obj_add_flag(eq_profiles_edit_btn, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(eq_profiles_edit_btn, LV_OBJ_FLAG_HIDDEN);
    }

    if (!peq_scan_profiles(PEQ_PROFILES_DIR, &eq_profile_paths, &eq_profile_count)) {
        lv_obj_t * label = lv_label_create(eq_profiles_list);
        lv_label_set_text(label, "No saved profiles");
        lv_obj_add_style(label, &style_theme_text_muted, 0);
        lv_obj_set_style_pad_left(label, 24, 0);
        return;
    }

    for (int i = 0; i < eq_profile_count; i++) {
        lv_obj_t * row = lv_obj_create(eq_profiles_list);
        lv_obj_set_size(row, LIST_ROW_WIDTH, LIST_ROW_HEIGHT);
        lv_obj_set_style_radius(row, LIST_ROW_RADIUS, 0);
        lv_obj_set_style_bg_color(row, LIST_ROW_BG_COLOR, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        char display[64];
        snprintf(display, sizeof(display), "%s", basename_of(eq_profile_paths[i]));
        char * dot = strrchr(display, '.');
        if (dot) *dot = '\0';

        lv_obj_t * label = lv_label_create(row);
        lv_label_set_text(label, display);
        lv_obj_add_style(label, &style_theme_text_primary, 0);
        lv_obj_set_style_text_font(label, &LIST_ROW_FONT, 0);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, LIST_ROW_LABEL_INSET, 0);

        if (eq_profiles_edit_mode) {
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(row, eq_profile_rename_row_cb, LV_EVENT_CLICKED, eq_profile_paths[i]);
            lv_obj_t * delete_icon = lv_image_create(row);
            lv_image_set_src(delete_icon, asset_path("touch_list/del.png"));
            lv_obj_align(delete_icon, LV_ALIGN_RIGHT_MID, -20, 0);
            lv_obj_add_flag(delete_icon, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(delete_icon, eq_profile_delete_row_cb, LV_EVENT_CLICKED, eq_profile_paths[i]);
        } else if (eq_profiles_replace_mode) {
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(row, eq_profile_replace_row_cb, LV_EVENT_CLICKED, eq_profile_paths[i]);
        } else {
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(row, eq_profile_row_cb, LV_EVENT_CLICKED, eq_profile_paths[i]);
        }
    }
}

/* Catches every way this screen can be left (back button, back-swipe, Home
 * swipe) -- not just eq_profile_replace_row_cb's own success-path nav_pop()
 * -- so replace mode can never stay armed into a later, unrelated visit to
 * this same screen via "Load Profile". */
static void eq_profiles_screen_unloaded_cb(lv_event_t * e) {
    (void) e;
    eq_profiles_replace_mode = false;
}

static void eq_profiles_edit_btn_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    eq_profiles_edit_mode = !eq_profiles_edit_mode;
    populate_eq_profiles_screen();
}

static void build_eq_profile_delete_popup(void) {
    eq_profile_delete_popup.popup = build_confirm_popup("Delete this profile?", LV_LABEL_LONG_WRAP, NULL, NULL, "Delete",
                                                         lv_color_make(255, 120, 120), eq_profile_delete_confirm_cb, NULL,
                                                         "Cancel", accent_lv_color(), eq_profile_delete_cancel_cb, NULL,
                                                         eq_profile_delete_popup_backdrop_cb, &eq_profile_delete_popup.backdrop);
}

static lv_obj_t * build_eq_profiles_screen(void) {
    lv_obj_t * scr = build_subsonic_list_screen("Profiles", &eq_profiles_title_label, &eq_profiles_list);
    lv_obj_add_event_cb(scr, eq_profiles_screen_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
    eq_profiles_edit_btn = lv_label_create(scr);
    lv_label_set_text(eq_profiles_edit_btn, "Edit");
    lv_obj_set_style_text_color(eq_profiles_edit_btn, accent_lv_color(), 0);
    lv_obj_set_style_text_font(eq_profiles_edit_btn, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    align_screen_header_action(eq_profiles_edit_btn, 20);
    lv_obj_add_flag(eq_profiles_edit_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(eq_profiles_edit_btn, eq_profiles_edit_btn_cb, LV_EVENT_CLICKED, NULL);
    if (eq_profiles_title_label) reserve_title_width_before(eq_profiles_title_label, eq_profiles_edit_btn);
    return scr;
}

static void eq_load_profile_btn_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    eq_profiles_edit_mode = false;
    eq_profiles_replace_mode = false;
    populate_eq_profiles_screen();
    nav_push(eq_profiles_screen);
}

/* Entered only from the Save flow's "Replace Existing" choice -- see
 * eq_open_save_choice_popup(). */
static void eq_open_profiles_for_replace(void) {
    eq_profiles_edit_mode = false;
    eq_profiles_replace_mode = true;
    populate_eq_profiles_screen();
    nav_push(eq_profiles_screen);
}

static void eq_save_profile_name_done_cb(const char * text, void * user_data) {
    (void) user_data;
    eq_save_named_profile(text);
}

/* Save-choice popup: offered whenever a Save action would otherwise go
 * straight to a blank/prefilled text entry (no current profile, or a
 * deliberate "Save As" hold) AND at least one profile already exists to
 * replace -- reported gap: users had no way to pick an EXISTING preset to
 * overwrite, only a text box to name a new one. `eq_save_choice_prefill_current`
 * remembers which of those two triggered it, so "New Profile" opens the
 * right text-entry variant once the choice is made. */
static gui_popup_t eq_save_choice_popup;
static bool eq_save_choice_prefill_current = false;

static void eq_hide_save_choice_popup(void) {
    gui_popup_hide(&eq_save_choice_popup);
}

static void eq_save_choice_backdrop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    eq_hide_save_choice_popup();
}

static void eq_save_choice_new_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    eq_hide_save_choice_popup();
    const char * prefill = eq_save_choice_prefill_current ? eq_current_profile_name : "";
    const char * title = eq_save_choice_prefill_current ? "Save Profile As" : "Profile Name";
    show_text_entry(title, prefill, false, false, eq_save_profile_name_done_cb, NULL);
}

static void eq_save_choice_replace_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    eq_hide_save_choice_popup();
    eq_open_profiles_for_replace();
}

static void build_eq_save_choice_popup(void) {
    eq_save_choice_popup.popup = build_confirm_popup(
        "Save Profile", LV_LABEL_LONG_WRAP, NULL,
        "Save as a new profile, or replace one that already exists?",
        "Replace Existing", lv_color_make(255, 120, 120), eq_save_choice_replace_cb, NULL,
        "New Profile", accent_lv_color(), eq_save_choice_new_cb, NULL,
        eq_save_choice_backdrop_cb, &eq_save_choice_popup.backdrop);
}

/* Shows the choice popup when there's at least one existing profile to
 * offer replacing; otherwise (first-ever save) goes straight to the same
 * text-entry a plain "New Profile" choice would have opened, since there's
 * nothing yet to replace. */
static void eq_open_save_choice_popup(bool prefill_current_name) {
    char ** existing_paths = NULL;
    int existing_count = 0;
    bool have_existing = peq_scan_profiles(PEQ_PROFILES_DIR, &existing_paths, &existing_count) && existing_count > 0;
    for (int i = 0; i < existing_count; i++) free(existing_paths[i]);
    free(existing_paths);

    if (!have_existing) {
        const char * prefill = prefill_current_name ? eq_current_profile_name : "";
        const char * title = prefill_current_name ? "Save Profile As" : "Profile Name";
        show_text_entry(title, prefill, false, false, eq_save_profile_name_done_cb, NULL);
        return;
    }

    eq_save_choice_prefill_current = prefill_current_name;
    gui_popup_show(&eq_save_choice_popup);
}

/* Save Profile: a quick tap saves in place; a deliberate hold opens the save
 * choice above (prefilled with the current name if choosing "New Profile").
 * Tracked locally via our own PRESSED/CLICKED timestamps rather than LVGL's
 * global LV_EVENT_LONG_PRESSED (LV_INDEV_DEF_LONG_PRESS_TIME, 400ms, shared
 * by every long-press in the app) -- 400ms was too easy to trip on an
 * ordinary deliberate tap here, especially right after a run of slider
 * drags, so this button alone uses a longer local hold. */
#define EQ_SAVE_PROFILE_HOLD_MS 700
static uint32_t eq_save_profile_press_start_ms = 0;

static void eq_save_profile_pressed_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_PRESSED) return;
    eq_save_profile_press_start_ms = lv_tick_get();
}

static void eq_save_profile_btn_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    bool held = lv_tick_elaps(eq_save_profile_press_start_ms) >= EQ_SAVE_PROFILE_HOLD_MS;
    if (held) {
        eq_open_save_choice_popup(true);
        return;
    }
    if (eq_current_profile_name[0]) {
        eq_save_named_profile(eq_current_profile_name);
        return;
    }
    eq_open_save_choice_popup(false);
}

static lv_obj_t * build_eq_screen(void) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_add_style(scr, &style_theme_screen_bg, 0);

    /* Standard back button + title, matching every other sub-screen
     * (build_subsonic_list_screen et al) instead of the old screen's own
     * one-off green "< Back" button -- the other half of "styling should
     * use the same as all the other screens". */
    build_screen_header(scr, "PEQ", generic_back_cb, NULL, NULL);

    /* "EQ Enabled" switch lives in the title row itself (top-right), same
     * spot other screens put a title-row action (e.g. the Wi-Fi screen's
     * "Rescan"). */
    eq_bypass_switch = lv_switch_create(scr);
    align_screen_header_action(eq_bypass_switch, 16);
    lv_obj_add_style(eq_bypass_switch, gui_theme_accent_style(), LV_PART_INDICATOR | LV_STATE_CHECKED);

    /* Reset to defaults -- positioned relative to the bypass switch.
     * Requires confirmation popup since this resets all band settings and preamp. */
    lv_obj_t * reset_btn = lv_label_create(scr);
    lv_label_set_text(reset_btn, "Reset");
    lv_obj_set_style_text_color(reset_btn, lv_color_make(255, 120, 120), 0);
    lv_obj_set_style_text_font(reset_btn, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_align_to(reset_btn, eq_bypass_switch, LV_ALIGN_OUT_LEFT_MID, -14, 0);
    lv_obj_add_flag(reset_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(reset_btn, 16);
    lv_obj_add_event_cb(reset_btn, eq_reset_btn_cb, LV_EVENT_CLICKED, NULL);

    /* Everything else lives in one scrollable flex-column container below
     * the title row -- pad_gap gives every card the same, generous spacing
     * (the "sliders more spaced out" ask) from one place instead of
     * hand-tuned per-element y-offsets that made it easy for things to end
     * up too close together or overlapping as fields got bigger. */
    lv_obj_t * content = lv_obj_create(scr);
    lv_obj_set_size(content, lv_pct(100),
                    lv_display_get_vertical_resolution(lv_display_get_default()) - STATUS_BAR_CLEARANCE -
                        TITLE_ROW_HEIGHT);
    lv_obj_align(content, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(content, 0, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    /* Zero default container padding so card width calculations align cleanly. */
    lv_obj_set_style_pad_all(content, 0, 0);
    lv_obj_set_scroll_dir(content, LV_DIR_VER);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    /* START main-axis alignment anchors children to top of scrollable area. */
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(content, 16, 0);
    lv_obj_set_style_pad_top(content, 12, 0);
    lv_obj_set_style_pad_bottom(content, 24, 0);

    /* Pre-Amp: an overall gain applied before the per-band filters, so
     * boosted bands can be pulled back down without touching every band's
     * own gain -- same role as the reference DAP's "Pre AMP" control. */
    lv_obj_t * eq_preamp_card = create_eq_slider_card(content, EQ_FIELD_PREAMP, &eq_preamp_value_label, &eq_preamp_slider, -120, 120);

    /* Dropdown width scales with font size tier so options fit without truncation. */
    int32_t eq_dropdown_width = 170;
    if (current_settings.font_size_tier == 1) eq_dropdown_width = 200;
    else if (current_settings.font_size_tier == 2) eq_dropdown_width = 240;

    /* Band selection row with flex layout and content-based height. */
    lv_obj_t * band_row = lv_obj_create(content);
    lv_obj_set_width(band_row, lv_pct(96));
    lv_obj_set_height(band_row, LV_SIZE_CONTENT);
    lv_obj_add_style(band_row, &style_theme_card_bg, 0);
    lv_obj_set_style_border_width(band_row, 0, 0);
    lv_obj_set_style_radius(band_row, 12, 0);
    lv_obj_set_style_pad_all(band_row, 14, 0);
    lv_obj_remove_flag(band_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(band_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(band_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t * band_label = lv_label_create(band_row);
    lv_label_set_text(band_label, "Band");
    lv_obj_add_style(band_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(band_label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);

    eq_band_dropdown = lv_dropdown_create(band_row);
    lv_dropdown_set_options(eq_band_dropdown, "Band0\nBand1\nBand2\nBand3\nBand4\nBand5\nBand6\nBand7\nBand8\nBand9");
    lv_obj_set_style_text_font(eq_band_dropdown, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    /* The closed box and the opened list are two separate LVGL objects
     * (lv_dropdown_create() builds dropdown->list eagerly, as a child of
     * the screen, not of this dropdown) -- styling only the box above left
     * the opened list's text at LVGL's own default (unscaled) font. */
    lv_obj_set_style_text_font(lv_dropdown_get_list(eq_band_dropdown), gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_obj_set_width(eq_band_dropdown, eq_dropdown_width);

    lv_obj_t * eq_freq_card = create_eq_slider_card(content, EQ_FIELD_FREQ, &eq_freq_value_label, &eq_freq_slider, 0, EQ_FREQ_SLIDER_MAX);
    lv_obj_t * eq_gain_card = create_eq_slider_card(content, EQ_FIELD_GAIN, &eq_gain_value_label, &eq_gain_slider, -120, 120);
    lv_obj_t * eq_q_card = create_eq_slider_card(content, EQ_FIELD_Q, &eq_q_value_label, &eq_q_slider, 1, 100);

    lv_obj_t * type_row = lv_obj_create(content);
    lv_obj_set_width(type_row, lv_pct(96));
    lv_obj_set_height(type_row, LV_SIZE_CONTENT);
    lv_obj_add_style(type_row, &style_theme_card_bg, 0);
    lv_obj_set_style_border_width(type_row, 0, 0);
    lv_obj_set_style_radius(type_row, 12, 0);
    lv_obj_set_style_pad_all(type_row, 14, 0);
    lv_obj_remove_flag(type_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(type_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(type_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t * type_label = lv_label_create(type_row);
    lv_label_set_text(type_label, "Type");
    lv_obj_add_style(type_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(type_label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);

    eq_type_dropdown = lv_dropdown_create(type_row);
    lv_dropdown_set_options(eq_type_dropdown, "Peaking\nLow Shelf\nHigh Shelf");
    /* Same gap as eq_band_dropdown just above -- box and opened list both
     * need the tier-aware font explicitly, LVGL's own default isn't. */
    lv_obj_set_style_text_font(eq_type_dropdown, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_obj_set_style_text_font(lv_dropdown_get_list(eq_type_dropdown), gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_obj_set_width(eq_type_dropdown, eq_dropdown_width);

    lv_obj_t * enable_row = lv_obj_create(content);
    lv_obj_set_width(enable_row, lv_pct(96));
    lv_obj_set_height(enable_row, LV_SIZE_CONTENT);
    lv_obj_add_style(enable_row, &style_theme_card_bg, 0);
    lv_obj_set_style_border_width(enable_row, 0, 0);
    lv_obj_set_style_radius(enable_row, 12, 0);
    lv_obj_set_style_pad_all(enable_row, 14, 0);
    lv_obj_remove_flag(enable_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(enable_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(enable_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t * enabled_label = lv_label_create(enable_row);
    lv_label_set_text(enabled_label, "Enable");
    lv_obj_add_style(enabled_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(enabled_label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);

    eq_band_enabled_switch = lv_switch_create(enable_row);
    lv_obj_add_style(eq_band_enabled_switch, gui_theme_accent_style(), LV_PART_INDICATOR | LV_STATE_CHECKED);

    /* Save/Load Profile row with wrapping button labels and content-based height. */
    lv_obj_t * profile_row = lv_obj_create(content);
    lv_obj_set_width(profile_row, lv_pct(96));
    lv_obj_set_height(profile_row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(profile_row, 0, 0);
    lv_obj_set_style_border_width(profile_row, 0, 0);
    lv_obj_remove_flag(profile_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(profile_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(profile_row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(profile_row, 12, 0);

    lv_obj_t * save_btn = lv_obj_create(profile_row);
    lv_obj_set_flex_grow(save_btn, 1);
    lv_obj_set_height(save_btn, LV_SIZE_CONTENT);
    lv_obj_add_style(save_btn, &style_theme_card_bg, 0);
    lv_obj_set_style_border_width(save_btn, 0, 0);
    lv_obj_set_style_radius(save_btn, 12, 0);
    lv_obj_set_style_pad_all(save_btn, 14, 0);
    lv_obj_remove_flag(save_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(save_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(save_btn, eq_save_profile_pressed_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(save_btn, eq_save_profile_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * save_label = lv_label_create(save_btn);
    lv_label_set_text(save_label, "Save Profile");
    lv_obj_set_style_text_font(save_label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_obj_add_style(save_label, gui_theme_accent_style(), 0);
    lv_label_set_long_mode(save_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(save_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(save_label, lv_pct(100));
    lv_obj_center(save_label);

    lv_obj_t * load_btn = lv_obj_create(profile_row);
    lv_obj_set_flex_grow(load_btn, 1);
    lv_obj_set_height(load_btn, LV_SIZE_CONTENT);
    lv_obj_add_style(load_btn, &style_theme_card_bg, 0);
    lv_obj_set_style_border_width(load_btn, 0, 0);
    lv_obj_set_style_radius(load_btn, 12, 0);
    lv_obj_set_style_pad_all(load_btn, 14, 0);
    lv_obj_remove_flag(load_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(load_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(load_btn, eq_load_profile_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * load_label = lv_label_create(load_btn);
    lv_label_set_text(load_label, "Profiles");
    lv_obj_set_style_text_font(load_label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_obj_add_style(load_label, gui_theme_accent_style(), 0);
    lv_label_set_long_mode(load_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(load_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(load_label, lv_pct(100));
    lv_obj_center(load_label);

    /* Establish correct initial visual state for every widget before
     * binding event callbacks to avoid spurious value-changed events during
     * screen construction. */
    refresh_all_eq_widgets();

    lv_obj_add_event_cb(eq_bypass_switch, eq_bypass_switch_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(eq_band_dropdown, eq_band_dropdown_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(eq_preamp_slider, eq_preamp_slider_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(eq_band_enabled_switch, eq_band_enabled_switch_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(eq_type_dropdown, eq_type_dropdown_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(eq_freq_slider, eq_freq_slider_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(eq_gain_slider, eq_gain_slider_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(eq_q_slider, eq_q_slider_event_cb, LV_EVENT_ALL, NULL);

    finalize_screen_navigation(scr);
    /* Prevent dragging in the scrollable content area from bubbling up as an app-wide swipe. */
    lv_obj_remove_flag(content, LV_OBJ_FLAG_GESTURE_BUBBLE);
    /* Register slider cards as dead zones to prevent horizontal slider adjustments
     * from triggering the swipe-to-player transition. */
    lv_obj_remove_flag(eq_preamp_card, LV_OBJ_FLAG_GESTURE_BUBBLE);
    register_swipe_dead_zone(eq_preamp_card);
    lv_obj_remove_flag(eq_freq_card, LV_OBJ_FLAG_GESTURE_BUBBLE);
    register_swipe_dead_zone(eq_freq_card);
    lv_obj_remove_flag(eq_gain_card, LV_OBJ_FLAG_GESTURE_BUBBLE);
    register_swipe_dead_zone(eq_gain_card);
    lv_obj_remove_flag(eq_q_card, LV_OBJ_FLAG_GESTURE_BUBBLE);
    register_swipe_dead_zone(eq_q_card);
    return scr;
}

void gui_settings_init(void) {
    buy_me_a_coffee_screen = build_buy_me_a_coffee_screen();
    about_screen = build_about_screen();
    dev_options_screen = build_dev_options_screen();
    accent_color_screen = build_accent_color_screen();
    custom_font_screen = build_custom_font_screen();
    screen_timeout_screen = build_screen_timeout_screen();
    screen_dimming_screen = build_screen_dimming_screen();
    startup_volume_screen = build_startup_volume_screen();
    sleep_timer_screen = build_sleep_timer_screen();
    idle_shutdown_screen = build_idle_shutdown_screen();
    timezone_region_screen = build_timezone_region_screen();
    clock_set_time_screen = build_clock_set_time_screen();
    clock_screen = build_clock_screen();
    music_playback_screen = build_music_playback_screen();
    music_audio_screen = build_music_audio_screen();
    music_controls_screen = build_music_controls_screen();
    music_timers_screen = build_music_timers_screen();
    music_database_screen = build_music_database_screen();
    music_library_screen = plugin_manager_get_music_library_list_item_count() > 0
                               ? build_music_library_screen() : NULL;
    settings_music_screen = build_music_settings_screen();
    settings_display_screen = build_settings_display_screen();
    settings_power_screen = build_settings_power_screen();
    settings_system_screen = build_settings_system_screen();
    settings_screen = build_settings_screen();
    eq_screen = build_eq_screen();
    eq_profiles_screen = build_eq_profiles_screen();
    build_firmware_update_popup();
    build_eq_reset_popup();
    build_eq_profile_delete_popup();
    build_eq_save_choice_popup();
    build_factory_reset_popup();
    build_hostname_reboot_popup();
}

/* For gui_reload.c's in-process UI reload -- deletes every screen this
 * module owns so gui_settings_init() can rebuild them from a clean slate
 * without leaking the old objects. Does NOT touch build_home_screen()'s
 * result -- that's gui_shell.c's own static (home_screen), not this
 * module's, even though build_home_screen() itself lives here. The four
 * popup-and-backdrop pairs below are built directly on lv_layer_top() (see
 * build_confirm_popup()'s own comment), not as children of any of these
 * screens, so each needs its own explicit deletion. */
void gui_settings_teardown(void) {
    gui_popup_teardown(&firmware_update_popup);
    gui_popup_teardown(&eq_reset_popup);
    gui_popup_teardown(&eq_profile_delete_popup);
    gui_popup_teardown(&eq_save_choice_popup);
    gui_popup_teardown(&factory_reset_popup);
    gui_popup_teardown(&hostname_reboot_popup);

    if (about_screen) { lv_obj_delete(about_screen); about_screen = NULL; }
    if (buy_me_a_coffee_screen) { lv_obj_delete(buy_me_a_coffee_screen); buy_me_a_coffee_screen = NULL; }
    if (dev_options_screen) { lv_obj_delete(dev_options_screen); dev_options_screen = NULL; }
    adb_switch = NULL; /* owned by the screen just deleted; sync runs off the USB poll, not this screen's lifetime */
    if (accent_color_screen) { lv_obj_delete(accent_color_screen); accent_color_screen = NULL; }
    if (custom_font_screen) { lv_obj_delete(custom_font_screen); custom_font_screen = NULL; }
    if (screen_timeout_screen) { lv_obj_delete(screen_timeout_screen); screen_timeout_screen = NULL; }
    if (screen_dimming_screen) { lv_obj_delete(screen_dimming_screen); screen_dimming_screen = NULL; }
    if (startup_volume_screen) { lv_obj_delete(startup_volume_screen); startup_volume_screen = NULL; }
    if (sleep_timer_screen) { lv_obj_delete(sleep_timer_screen); sleep_timer_screen = NULL; }
    if (idle_shutdown_screen) { lv_obj_delete(idle_shutdown_screen); idle_shutdown_screen = NULL; }
    if (timezone_region_screen) { lv_obj_delete(timezone_region_screen); timezone_region_screen = NULL; }
    if (clock_screen) { lv_obj_delete(clock_screen); clock_screen = NULL; }
    if (clock_set_time_screen) { lv_obj_delete(clock_set_time_screen); clock_set_time_screen = NULL; }
    clock_hour_roller = clock_minute_roller = clock_ampm_roller = NULL;
    clock_set_time_row = NULL;
    clock_timezone_row = clock_timezone_value_label = NULL;
    /* Lazily built by open_timezone_city_screen() -- NULL until the user has
     * opened at least one region, same guard shape as every screen above. */
    if (timezone_city_screen) { lv_obj_delete(timezone_city_screen); timezone_city_screen = NULL; }
    if (settings_music_screen) { lv_obj_delete(settings_music_screen); settings_music_screen = NULL; }
    if (music_playback_screen) { lv_obj_delete(music_playback_screen); music_playback_screen = NULL; }
    if (music_audio_screen) { lv_obj_delete(music_audio_screen); music_audio_screen = NULL; }
    if (music_controls_screen) { lv_obj_delete(music_controls_screen); music_controls_screen = NULL; }
    if (music_timers_screen) { lv_obj_delete(music_timers_screen); music_timers_screen = NULL; }
    if (music_database_screen) { lv_obj_delete(music_database_screen); music_database_screen = NULL; }
    if (music_library_screen) { lv_obj_delete(music_library_screen); music_library_screen = NULL; }
    if (settings_display_screen) { lv_obj_delete(settings_display_screen); settings_display_screen = NULL; }
    if (settings_power_screen) { lv_obj_delete(settings_power_screen); settings_power_screen = NULL; }
    if (settings_system_screen) { lv_obj_delete(settings_system_screen); settings_system_screen = NULL; }
    if (settings_screen) { lv_obj_delete(settings_screen); settings_screen = NULL; }
    if (eq_screen) { lv_obj_delete(eq_screen); eq_screen = NULL; }
    if (eq_profiles_screen) { lv_obj_delete(eq_profiles_screen); eq_profiles_screen = NULL; }
    eq_profiles_edit_btn = NULL;
    eq_profiles_title_label = NULL;
    eq_profiles_free_paths();
}

/* Externs for callbacks defined in gui.c */

/* Extern widget var in gui.c */




lv_obj_t * gui_settings_get_screen(void) { return settings_screen; }
lv_obj_t * gui_settings_get_music_screen(void) { return settings_music_screen; }
lv_obj_t * gui_settings_get_display_screen(void) { return settings_display_screen; }
lv_obj_t * gui_settings_get_power_screen(void) { return settings_power_screen; }
lv_obj_t * gui_settings_get_system_screen(void) { return settings_system_screen; }
lv_obj_t * gui_settings_get_about_screen(void) { return about_screen; }
lv_obj_t * gui_settings_get_accent_screen(void) { return accent_color_screen; }
lv_obj_t * gui_settings_get_custom_font_screen(void) { return custom_font_screen; }
lv_obj_t * gui_settings_get_eq_screen(void) { return eq_screen; }



void gui_settings_sync_gapless_toggle(void) {
    if (!settings_gapless_toggle_img) return;
    if (current_settings.gapless_enabled) lv_obj_add_state(settings_gapless_toggle_img, LV_STATE_CHECKED);
    else lv_obj_clear_state(settings_gapless_toggle_img, LV_STATE_CHECKED);
}

void gui_settings_sync_crossfade_toggle(void) {
    if (!settings_crossfade_toggle_img) return;
    /* Real lv_switch now (see PILL_ACCESSORY_TOGGLE in screen_builders.c) --
     * CHECKED state alone drives its visual, no sprite swap needed. */
    if (current_settings.crossfade_enabled) lv_obj_add_state(settings_crossfade_toggle_img, LV_STATE_CHECKED);
    else lv_obj_clear_state(settings_crossfade_toggle_img, LV_STATE_CHECKED);
}

/* Same bidirectional-sync role as gui_settings_sync_crossfade_toggle()
 * above, for the Sleep Timer screen's own enable switch -- called from
 * gui_shell.c whenever the drawer icon arms/disarms the timer, or the
 * countdown expires on its own, so this screen never shows a stale state
 * the next time it's opened. Also shows/hides the duration slider card and
 * the "Show Time Remaining" button, matching sleep_timer_switch_event_cb's
 * own behavior when the switch is toggled from this screen directly. */
void gui_settings_sync_sleep_timer_toggle(void) {
    if (!sleep_timer_switch) return;
    bool active = quick_drawer_sleep_timer_is_active();
    if (active) lv_obj_add_state(sleep_timer_switch, LV_STATE_CHECKED);
    else lv_obj_clear_state(sleep_timer_switch, LV_STATE_CHECKED);
    if (!sleep_timer_slider_card) return;
    if (active) {
        lv_obj_remove_flag(sleep_timer_slider_card, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(sleep_timer_remaining_btn, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(sleep_timer_slider_card, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(sleep_timer_remaining_btn, LV_OBJ_FLAG_HIDDEN);
    }
}
