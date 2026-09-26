/* Host regression tests against the real BlueALSA 5 implementation. Unused
 * device code is discarded by gc-sections. */
#define _GNU_SOURCE
#include <stdio.h>
/* Fixture for the LDAC decoder check; no library by default. */
static const char * test_ldac_decoder_path = "/nonexistent/libldacdec.so.1";
#define BT_LDAC_DECODER_PATH test_ldac_decoder_path
#include "bluetooth_control.c"
#include <assert.h>
#include <errno.h>

static const char * process_cmdline;
static size_t process_cmdline_size;
static bool process_present, check_lock;
static bool retain_process_after_kill;
static unsigned proc_index, kill_calls, spawn_calls, soft_volume_calls;
static unsigned soft_volume_checked_calls;
static bool soft_volume_fail;
static bool source_pcm_present;
static enum { BLUEZ_CLI_MODERN, BLUEZ_CLI_LEGACY, BLUEZ_CLI_EMPTY_HELP,
              BLUEZ_CLI_TRUNCATED_HELP } bluez_cli_mode;
static unsigned bluez_help_calls, bluez_help_failures, bluez_paired_calls;
static char soft_volume_state[8];
static char soft_volume_path[256];
static char discovered_source_path[256];
static char spawned[10][128];
static size_t spawned_argc;
static struct dirent proc_entry;

static void expect_locked(void) {
    if (check_lock) assert(pthread_mutex_trylock(&bt_daemon_respawn_mutex) == EBUSY);
}

DIR * __wrap_opendir(const char * path) {
    assert(strcmp(path, "/proc") == 0);
    expect_locked();
    proc_index = 0;
    return (DIR *)&proc_entry;
}

struct dirent * __wrap_readdir(DIR * dir) {
    assert(dir == (DIR *)&proc_entry);
    if (proc_index++ || !process_present) return NULL;
    strcpy(proc_entry.d_name, "12345");
    return &proc_entry;
}

int __wrap_closedir(DIR * dir) {
    assert(dir == (DIR *)&proc_entry);
    return 0;
}

static void capture_argv(char * const argv[]) {
    expect_locked();
    spawn_calls++;
    spawned_argc = 0;
    while (argv[spawned_argc]) {
        assert(spawned_argc < 10);
        assert(strlen(argv[spawned_argc]) < sizeof(spawned[0]));
        strcpy(spawned[spawned_argc], argv[spawned_argc]);
        spawned_argc++;
    }
}

void subprocess_kill_all_matching(const char * needle) {
    expect_locked();
    kill_calls++;
    if (strcmp(needle, "bluealsad") == 0 && !retain_process_after_kill)
        process_present = false;
}

bool subprocess_spawn_daemon(char * const argv[]) {
    capture_argv(argv);
    return true;
}

bool subprocess_spawn_daemon_logged(char * const argv[], const char * log_path) {
    (void)log_path;
    capture_argv(argv);
    /* Stop apply_output_settings after constructing the actual daemon argv. */
    return false;
}

bool subprocess_run_checked(char * const argv[], char * out, size_t size,
                            int timeout_ms, int * out_exit_code) {
    expect_locked();
    assert(strcmp(argv[0], "/usr/bin/bluealsactl") == 0);
    assert(strcmp(argv[1], "soft-volume") == 0);
    assert(out == NULL && size == 0 && timeout_ms == 5000);
    assert(out_exit_code);
    soft_volume_checked_calls++;
    snprintf(soft_volume_path, sizeof(soft_volume_path), "%s", argv[2]);
    snprintf(soft_volume_state, sizeof(soft_volume_state), "%s", argv[3]);
    *out_exit_code = soft_volume_fail ? 1 : 0;
    return true; /* Exercise callers that must inspect the exit status. */
}

/* Same fixture as subprocess_run(): the production code niced this call
 * down, which changes the child's scheduling priority, nothing the test
 * fixture models. */
bool subprocess_run_low_priority(char * const argv[], char * out, size_t size) {
    return subprocess_run(argv, out, size);
}

bool subprocess_run(char * const argv[], char * out, size_t size) {
    expect_locked();
    if (strcmp(argv[0], "bluetoothctl") == 0) {
        assert(out && size);
        if (strcmp(argv[1], "--help") == 0) {
            assert(argv[2] == NULL);
            bluez_help_calls++;
            if (bluez_help_failures) {
                bluez_help_failures--;
                return false;
            }
            if (bluez_cli_mode == BLUEZ_CLI_EMPTY_HELP) {
                out[0] = '\0';
            } else if (bluez_cli_mode == BLUEZ_CLI_TRUNCATED_HELP) {
                snprintf(out, size, "Commands:\n\tdevices\t\tList available");
            } else {
                snprintf(out, size, "%s", bluez_cli_mode == BLUEZ_CLI_MODERN ?
                         "Commands:\n"
                         "\tlist\t\tList available controllers\n"
                         "\tshow\t\tController information\n"
                         "\tselect\t\tSelect default controller\n"
                         "\tdevices\t\tList available devices, with an optional property as the filter\n" :
                         "Commands:\n"
                         "\tlist\t\tList available controllers\n"
                         "\tshow\t\tController information\n"
                         "\tpaired-devices\tList paired devices\n");
                if (bluez_cli_mode == BLUEZ_CLI_MODERN) {
                    /* The real 5.87 --help is 9151 bytes; keep this test
                     * from accidentally validating an undersized capture. */
                    assert(size > 9151);
                    size_t len = strlen(out);
                    memset(out + len, ' ', 9150 - len);
                    out[9150] = '\n';
                    out[9151] = '\0';
                }
            }
            return true;
        }
        if (strcmp(argv[1], "devices") == 0) {
            assert(bluez_cli_mode == BLUEZ_CLI_MODERN);
            assert(strcmp(argv[2], "Paired") == 0 && argv[3] == NULL);
        } else if (strcmp(argv[1], "paired-devices") == 0) {
            assert(bluez_cli_mode == BLUEZ_CLI_LEGACY);
            assert(argv[2] == NULL);
        } else {
            assert(strcmp(argv[1], "info") == 0);
            assert(argv[2] && argv[3] == NULL);
            snprintf(out, size, "Device %s\n\tPaired: yes\n\tConnected: %s\n",
                     argv[2], strstr(argv[2], "02") ? "yes" : "no");
            return true;
        }
        bluez_paired_calls++;
        snprintf(out, size,
                 "Device AA:BB:CC:DD:EE:01 Headphones\n"
                 "Device AA:BB:CC:DD:EE:02 Backup Headphones\n");
        return true;
    }
    if (strcmp(argv[0], "/usr/bin/bluealsactl") == 0) {
        if (strcmp(argv[1], "list-pcms") == 0) {
            assert(out && size);
            snprintf(out, size, "%s", source_pcm_present ?
                     "/org/bluealsa/hci0/dev_source/a2dpsrc/sink\n" : "");
            return true;
        }
        assert(strcmp(argv[1], "soft-volume") == 0);
        assert(strcmp(argv[3], "on") == 0 || strcmp(argv[3], "off") == 0);
        soft_volume_calls++;
        snprintf(soft_volume_path, sizeof(soft_volume_path), "%s", argv[2]);
        snprintf(soft_volume_state, sizeof(soft_volume_state), "%s", argv[3]);
        return !soft_volume_fail;
    }
    assert(strcmp(argv[0], "ps") == 0);
    assert(out && size);
    if (process_present)
        snprintf(out, size, "12345 %s\n", process_cmdline ? process_cmdline :
                 "bluealsad");
    else out[0] = '\0';
    return true;
}

bool subprocess_popen(char * const argv[], pid_t * pid, int * fd) {
    (void)argv; (void)pid; (void)fd;
    assert(!"unexpected monitor process");
    return false;
}

void subprocess_terminate(pid_t pid) {
    (void)pid;
    assert(!"unexpected process termination");
}

int __wrap_pthread_create(pthread_t * thread, const pthread_attr_t * attr,
                          void * (*start)(void *), void * arg) {
    (void)thread; (void)attr; (void)start; (void)arg;
    assert(!"unexpected monitor thread");
    return EAGAIN;
}

FILE * __real_fopen(const char * path, const char * mode);

FILE * __wrap_fopen(const char * path, const char * mode) {
    if (strncmp(path, "/proc/", 6) == 0) {
        assert(strcmp(path, "/proc/12345/cmdline") == 0);
        assert(strcmp(mode, "r") == 0);
        expect_locked();
        return fmemopen((void *)process_cmdline, process_cmdline_size, "r");
    }
    return __real_fopen(path, mode);
}

/* Automatic source: every codec, LDAC at high quality with adaptive bitrate. */
#define AUTO_ARGS "--all-codecs\0--ldac-abr\0--ldac-quality\0high\0"
static void assert_auto_codec_args(size_t * n) {
    assert(strcmp(spawned[(*n)++], "--all-codecs") == 0);
    assert(strcmp(spawned[(*n)++], "--ldac-abr") == 0);
    assert(strcmp(spawned[(*n)++], "--ldac-quality") == 0 && strcmp(spawned[(*n)++], "high") == 0);
}

/* DAC mode always offers aptX and aptX-HD on top of BlueALSA's defaults. */
static void assert_dac_stable_codecs(size_t * n) {
    assert(strcmp(spawned[(*n)++], "-c") == 0 && strcmp(spawned[(*n)++], "aptX") == 0);
    assert(strcmp(spawned[(*n)++], "-c") == 0 && strcmp(spawned[(*n)++], "aptX-HD") == 0);
}

static void test_daemon_argv(void) {
    static const char source[] = "bluealsad\0-p\0a2dp-source\0" AUTO_ARGS;
    static const char sink[] = "bluealsad\0-p\0a2dp-sink\0";
    static const char other[] = "/usr/bin/bluealsa-aplay\0-p\0a2dp-source\0";
    const struct { const char * args; size_t size; bool present; } cases[] = {
        {source, sizeof(source), true},
        {sink, sizeof(sink), true},
        {other, sizeof(other), true},
        {source, sizeof(source), false},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        process_cmdline = cases[i].args;
        process_cmdline_size = cases[i].size;
        process_present = cases[i].present;
        kill_calls = spawn_calls = 0;
        check_lock = true;
        ensure_bluealsa_running();
        check_lock = false;
        assert(kill_calls == 0);
        assert(spawn_calls == (unsigned)(!cases[i].present || cases[i].args == other));
        if (spawn_calls) {
            assert(spawned_argc == 7);
            assert(strcmp(spawned[0], "/usr/bin/bluealsad") == 0);
            assert(strcmp(spawned[1], "-p") == 0);
            assert(strcmp(spawned[2], "a2dp-source") == 0);
            size_t n = 3;
            assert_auto_codec_args(&n);
        }
        assert(pthread_mutex_trylock(&bt_daemon_respawn_mutex) == 0);
        pthread_mutex_unlock(&bt_daemon_respawn_mutex);
    }

    for (int dac = 0; dac < 2; dac++) for (int volume = 0; volume < 2; volume++) {
        kill_calls = spawn_calls = 0;
        check_lock = true;
        assert(!bt_control_apply_output_settings(dac, volume));
        check_lock = false;
        assert(spawn_calls == 1 && kill_calls == 3);
        assert(strcmp(spawned[0], "/usr/bin/bluealsad") == 0);
        assert(strcmp(spawned[1], "-p") == 0);
        assert(strcmp(spawned[2], dac ? "a2dp-sink" : "a2dp-source") == 0);
        size_t n = 3;
        if (!dac) assert_auto_codec_args(&n);
        else assert_dac_stable_codecs(&n);
        assert(spawned_argc == n);
        assert(pthread_mutex_trylock(&bt_daemon_respawn_mutex) == 0);
        pthread_mutex_unlock(&bt_daemon_respawn_mutex);
    }
}

static void test_sbc_xq_lifecycle(void) {
    static const char modern_auto[] = "bluealsad\0-p\0a2dp-source\0";
    static const char modern_xq[] = "bluealsad\0-p\0a2dp-source\0--sbc-quality=xq\0";
    static const char modern_xq_split[] = "bluealsad\0--profile\0a2dp-source\0--sbc-quality\0xq\0";
    static const char modern_sink_profile[] = "bluealsad\0--profile=a2dp-sink\0";
    static const char modern_sink[] = "bluealsad\0-p\0a2dp-sink\0";

    bt_control_restore_codec_preference("sbc_xq");
    assert(strcmp(bt_control_get_playback_pcm(), "bluealsa:CODEC=SBC") == 0);
    /* Source reconciliation restarts stale quality settings and is stable
     * when the already-running source is configured correctly. */
    process_cmdline = modern_auto; process_cmdline_size = sizeof(modern_auto);
    process_present = true; retain_process_after_kill = true; kill_calls = spawn_calls = 0;
    ensure_bluealsa_running();
    assert(kill_calls == 1 && spawn_calls == 1 && spawned_argc == 4);
    assert(strcmp(spawned[0], "/usr/bin/bluealsad") == 0);
    assert(strcmp(spawned[3], "--sbc-quality=xq") == 0);
    retain_process_after_kill = false;
    process_cmdline = modern_xq; process_cmdline_size = sizeof(modern_xq);
    process_present = true; kill_calls = spawn_calls = 0;
    ensure_bluealsa_running();
    assert(kill_calls == 0 && spawn_calls == 0);
    process_cmdline = modern_xq_split; process_cmdline_size = sizeof(modern_xq_split);
    process_present = true; kill_calls = spawn_calls = 0;
    ensure_bluealsa_running();
    assert(kill_calls == 0 && spawn_calls == 0);
    process_cmdline = modern_sink_profile; process_cmdline_size = sizeof(modern_sink_profile);
    process_present = true; kill_calls = spawn_calls = 0;
    ensure_bluealsa_running();
    assert(kill_calls == 0 && spawn_calls == 0);

    /* Modern profile changes use the modern daemon and still keep XQ off the
     * receiver/sink profile. */
    kill_calls = spawn_calls = 0;
    assert(!bt_control_apply_output_settings(false, false));
    assert(spawn_calls == 1 && kill_calls == 3 && spawned_argc == 4);
    assert(strcmp(spawned[0], "/usr/bin/bluealsad") == 0);
    assert(strcmp(spawned[3], "--sbc-quality=xq") == 0);
    kill_calls = spawn_calls = 0;
    assert(!bt_control_apply_output_settings(true, false));
    assert(spawn_calls == 1 && kill_calls == 3 && spawned_argc == 7);
    assert(strcmp(spawned[0], "/usr/bin/bluealsad") == 0);
    size_t n = 3;
    assert_dac_stable_codecs(&n);
    /* The experimental switch adds LDAC, but only with the rebuilt decoder:
     * none is installed here, so the sink stays at the stable set. */
    bt_control_set_dac_all_codecs(true);
    kill_calls = spawn_calls = 0;
    assert(!bt_control_apply_output_settings(true, false));
    assert(spawn_calls == 1 && spawned_argc == 7);
    /* With the rebuilt decoder installed, LDAC is offered too. The marker
     * straddles the reader's 4096-byte chunk to cover the carried tail. */
    char decoder_path[] = "/tmp/ldacdec_fixture_XXXXXX";
    int decoder_fd = mkstemp(decoder_path);
    assert(decoder_fd >= 0);
    FILE * decoder = fdopen(decoder_fd, "wb");
    assert(decoder);
    for (int k = 0; k < 4090; k++) fputc(k & 1 ? '\0' : 'x', decoder);
    fputs("ldacDecode_type_len", decoder);
    fclose(decoder);
    test_ldac_decoder_path = decoder_path;
    kill_calls = spawn_calls = 0;
    assert(!bt_control_apply_output_settings(true, false));
    assert(spawn_calls == 1 && spawned_argc == 9);
    assert(strcmp(spawned[7], "-c") == 0 && strcmp(spawned[8], "LDAC") == 0);
    /* Switch off: no LDAC even with the rebuilt decoder installed. */
    bt_control_set_dac_all_codecs(false);
    kill_calls = spawn_calls = 0;
    assert(!bt_control_apply_output_settings(true, false));
    assert(spawn_calls == 1 && spawned_argc == 7);
    bt_control_set_dac_all_codecs(true);
    decoder = fopen(decoder_path, "wb"); /* stock decoder: no marker */
    assert(decoder);
    fputs("ldacDecode_type", decoder);
    fclose(decoder);
    kill_calls = spawn_calls = 0;
    assert(!bt_control_apply_output_settings(true, false));
    assert(spawn_calls == 1 && spawned_argc == 7);
    unlink(decoder_path);
    test_ldac_decoder_path = "/nonexistent/libldacdec.so.1";
    bt_control_set_dac_all_codecs(false);
    process_cmdline = modern_sink; process_cmdline_size = sizeof(modern_sink);
    process_present = true; kill_calls = spawn_calls = 0;
    ensure_bluealsa_running();
    assert(kill_calls == 0 && spawn_calls == 0);

    assert(bt_control_set_codec("sbc_xq"));
    assert(strcmp(bt_control_get_playback_pcm(), "bluealsa:CODEC=SBC") == 0);
    assert(!bt_control_set_codec("not-a-codec"));
    assert(strcmp(bt_control_get_playback_pcm(), "bluealsa:CODEC=SBC") == 0);
    bt_control_restore_codec_preference("auto");
    assert(strcmp(bt_control_get_playback_pcm(), "bluealsa") == 0);
    bt_control_restore_codec_preference("aptx");
    assert(strcmp(bt_control_get_playback_pcm(), "bluealsa:CODEC=aptX") == 0);
    bt_control_restore_codec_preference("aac");
    assert(strcmp(bt_control_get_playback_pcm(), "bluealsa:CODEC=AAC") == 0);
    bt_control_restore_codec_preference("ldac_hq");
    assert(strcmp(bt_control_get_playback_pcm(), "bluealsa:CODEC=LDAC") == 0);
    bt_control_restore_codec_preference("auto");
}

static void test_modern_argv(void) {
    static const char source[] = "bluealsad\0-p\0a2dp-source\0";
    process_cmdline = source;
    process_cmdline_size = sizeof(source);
    process_present = false;
    kill_calls = spawn_calls = 0;
    ensure_bluealsa_running();
    assert(kill_calls == 0 && spawn_calls == 1);
    assert(spawned_argc == 7);
    assert(strcmp(spawned[0], "/usr/bin/bluealsad") == 0);
    assert(strcmp(spawned[1], "-p") == 0);
    assert(strcmp(spawned[2], "a2dp-source") == 0);
    {
        size_t n = 3;
        assert_auto_codec_args(&n);
    }

    soft_volume_calls = 0;
    soft_volume_checked_calls = 0;
    atomic_store(&modern_soft_volume_requested, true);
    bluealsa_apply_soft_volume("/org/bluealsa/hci0/dev_old/a2dpsnk/source");
    assert(soft_volume_checked_calls == 1);
    assert(soft_volume_calls == 0);
    assert(strcmp(soft_volume_state, "on") == 0);
    bluealsa_apply_soft_volume("/org/bluealsa/hci0/dev_old/a2dpsnk/source");
    assert(soft_volume_checked_calls == 1);
    bluealsa_apply_soft_volume("/org/bluealsa/hci0/dev_new/a2dpsnk/source");
    assert(soft_volume_checked_calls == 2);
    bluealsa_clear_soft_volume_path("/org/bluealsa/hci0/dev_old/a2dpsnk/source");
    bluealsa_apply_soft_volume("/org/bluealsa/hci0/dev_new/a2dpsnk/source");
    assert(soft_volume_checked_calls == 2); /* stale PCMRemoved must not evict new PCM */
    atomic_store(&modern_soft_volume_requested, false);
    bluealsa_clear_soft_volume_path(NULL);
    bluealsa_apply_soft_volume("/org/bluealsa/hci0/dev_new/a2dpsnk/source");
    assert(soft_volume_checked_calls == 3 && strcmp(soft_volume_state, "off") == 0);
    soft_volume_fail = true;
    bluealsa_clear_soft_volume_path(NULL);
    bluealsa_apply_soft_volume("/org/bluealsa/hci0/dev_new/a2dpsnk/source");
    assert(soft_volume_checked_calls == 4);
    soft_volume_fail = false;
    bluealsa_apply_soft_volume("/org/bluealsa/hci0/dev_new/a2dpsnk/source");
    assert(soft_volume_checked_calls == 5); /* nonzero exit must be retried */
    bluealsa_apply_soft_volume("/org/bluealsa/hci0/dev_receiver/a2dpsrc/sink");
    assert(soft_volume_checked_calls == 6 && strcmp(soft_volume_state, "off") == 0);
    source_pcm_present = true;
    bluealsa_clear_soft_volume_path(NULL);
    assert(find_source_pcm_path(discovered_source_path, sizeof(discovered_source_path)));
    assert(strcmp(discovered_source_path, "/org/bluealsa/hci0/dev_source/a2dpsrc/sink") == 0);
    assert(soft_volume_checked_calls == 7); /* initial discovery applies SoftVolume */

    assert(!bt_control_apply_output_settings(false, true));
    assert(!atomic_load(&modern_soft_volume_requested));
    kill_calls = spawn_calls = 0;
    assert(!bt_control_apply_output_settings(false, false));
    assert(spawn_calls == 1 && kill_calls == 3);
    assert(atomic_load(&modern_soft_volume_requested));
    assert(strcmp(spawned[0], "/usr/bin/bluealsad") == 0);
    assert(strcmp(spawned[1], "-p") == 0);
    assert(strcmp(spawned[2], "a2dp-source") == 0);
    assert(!bt_control_apply_output_settings(true, true));
    assert(atomic_load(&modern_soft_volume_requested)); /* DAC mode: phone volume via software */
}

static void test_bluez_paired_devices(void) {
    bt_device_t devices[2] = {0};

    /* A failed, empty, or truncated help probe must not poison the cache or
     * fall back blindly to a command the current bluetoothctl may not implement. */
    bt_bluetoothctl_devices_capability = BT_BLUETOOTHCTL_DEVICES_UNKNOWN;
    bluez_cli_mode = BLUEZ_CLI_EMPTY_HELP;
    bluez_help_calls = bluez_help_failures = bluez_paired_calls = 0;
    assert(bt_control_list_paired_states(devices, 2) == -1);
    assert(bluez_help_calls == 1 && bluez_paired_calls == 0);
    bluez_cli_mode = BLUEZ_CLI_TRUNCATED_HELP;
    assert(bt_control_list_paired_states(devices, 2) == -1);
    assert(bluez_help_calls == 2 && bluez_paired_calls == 0);
    bluez_cli_mode = BLUEZ_CLI_LEGACY;
    bluez_help_failures = 1;
    assert(bt_control_list_paired_states(devices, 2) == -1);
    assert(bluez_help_calls == 3 && bluez_paired_calls == 0);
    assert(bt_control_list_paired_states(devices, 2) == 2);
    assert(bluez_help_calls == 4 && bluez_paired_calls == 1);
    assert(strcmp(devices[0].mac, "AA:BB:CC:DD:EE:01") == 0);
    assert(strcmp(devices[0].name, "Headphones") == 0);
    assert(devices[0].paired && !devices[0].connected);
    assert(bt_control_is_connected());
    assert(bluez_help_calls == 4 && bluez_paired_calls == 2);

    /* 5.87's help marker selects `devices Paired`, and the successful
     * capability remains cached across both paired-list callers. */
    bt_bluetoothctl_devices_capability = BT_BLUETOOTHCTL_DEVICES_UNKNOWN;
    bluez_cli_mode = BLUEZ_CLI_MODERN;
    bluez_help_calls = bluez_paired_calls = 0;
    assert(bt_control_list_paired_states(devices, 2) == 2);
    assert(bluez_help_calls == 1 && bluez_paired_calls == 1);
    assert(strcmp(devices[1].name, "Backup Headphones") == 0);
    assert(devices[1].paired && devices[1].connected);
    assert(bt_control_list_paired_states(devices, 2) == 2);
    assert(bluez_help_calls == 1 && bluez_paired_calls == 2);
}

/* 44.1 kHz is the one transport rate the daemon can be told to negotiate, and
 * it is the default, so the argument has to reach the source daemon and stay
 * off the sink. */
static void test_force_audio_cd_argv(void) {
    bt_control_restore_codec_preference("auto");
    bt_control_set_sample_rate(44100);

    process_cmdline = NULL; process_cmdline_size = 0;
    process_present = false; kill_calls = spawn_calls = 0;
    ensure_bluealsa_running();
    assert(spawn_calls == 1 && spawned_argc == 8);
    assert(strcmp(spawned[2], "a2dp-source") == 0);
    assert(strcmp(spawned[3], "--a2dp-force-audio-cd") == 0);
    {
        size_t n = 4;
        assert_auto_codec_args(&n);
    }

    /* A daemon already carrying the argument is left alone. */
    static const char forced[] = "/usr/bin/bluealsad\0-p\0a2dp-source\0--a2dp-force-audio-cd\0" AUTO_ARGS;
    process_cmdline = forced; process_cmdline_size = sizeof(forced);
    process_present = true; kill_calls = spawn_calls = 0;
    ensure_bluealsa_running();
    assert(kill_calls == 0 && spawn_calls == 0);

    /* Automatic resolves to 44.1 kHz, so it keeps the argument. */
    bt_control_set_sample_rate(0);
    source_pcm_present = false;
    process_cmdline = forced; process_cmdline_size = sizeof(forced);
    process_present = true; kill_calls = spawn_calls = 0;
    ensure_bluealsa_running();
    assert(kill_calls == 0 && spawn_calls == 0);

    /* An explicit non-44.1 rate drops it, which is a real argv mismatch.
     * Nothing is connected, so correcting it may restart the daemon. */
    bt_control_set_sample_rate(48000);
    source_pcm_present = false;
    process_cmdline = forced; process_cmdline_size = sizeof(forced);
    process_present = true; retain_process_after_kill = true; kill_calls = spawn_calls = 0;
    ensure_bluealsa_running();
    assert(kill_calls == 1 && spawn_calls == 1 && spawned_argc == 7);
    {
        size_t n = 3;
        assert_auto_codec_args(&n);
    }
    retain_process_after_kill = false;

    /* Same mismatch with an accessory connected must NOT kill the daemon:
     * that would drop a live A2DP link just to correct an argument. */
    source_pcm_present = true;
    process_cmdline = forced; process_cmdline_size = sizeof(forced);
    process_present = true; kill_calls = spawn_calls = 0;
    ensure_bluealsa_running();
    assert(kill_calls == 0 && spawn_calls == 0);
    source_pcm_present = false;

    /* DAC mode never forces the rate: the phone picks it. */
    bt_control_set_sample_rate(0); /* automatic still means 44.1 for the source */
    kill_calls = spawn_calls = 0;
    assert(!bt_control_apply_output_settings(true, false));
    assert(spawn_calls == 1 && spawned_argc == 7);
    assert(strcmp(spawned[2], "a2dp-sink") == 0);
    bt_control_set_sample_rate(48000);
}

/* Capability decoding, against dbus-send output captured verbatim from a
 * real headset (Galaxy Buds2 Pro, AAC endpoint). Both byte-array spellings
 * are covered: requiring either one alone silently produced an empty rate
 * list on hardware that used the other. */
static void test_a2dp_capability_rates(void) {
    static const char bare[] =
        "   array [\n"
        "      dict entry(\n"
        "         string \"UUID\"\n"
        "         variant             string \"0000110b-0000-1000-8000-00805f9b34fb\"\n"
        "      )\n"
        "      dict entry(\n"
        "         string \"Codec\"\n"
        "         variant             byte 2\n"
        "      )\n"
        "      dict entry(\n"
        "         string \"Capabilities\"\n"
        "         variant             array of bytes [\n"
        "               80 01 8c 83 e8 00\n"
        "            ]\n"
        "      )\n"
        "   ]\n";
    static const char prefixed[] =
        "         string \"Capabilities\"\n"
        "         variant             array of bytes [\n"
        "               byte 0x80, byte 0x01, byte 0x8c, byte 0x83, byte 0xe8, byte 0x00\n"
        "            ]\n";

    uint8_t caps[32];
    size_t caps_len = 0;
    assert(dbus_prop_byte_array(bare, "Capabilities", caps, sizeof(caps), &caps_len));
    assert(caps_len == 6 && caps[0] == 0x80 && caps[1] == 0x01 && caps[2] == 0x8c && caps[5] == 0x00);

    uint8_t codec_id = 0;
    assert(dbus_prop_byte(bare, "Codec", &codec_id) && codec_id == 0x02);
    assert(a2dp_caps_match_codec(caps, caps_len, codec_id, "AAC"));
    assert(!a2dp_caps_match_codec(caps, caps_len, codec_id, "SBC"));

    /* 0x018: bit 4 is 44.1 kHz and bit 3 is 48 kHz, listed low to high. */
    unsigned int rates[12];
    int n = a2dp_caps_rates(caps, caps_len, "AAC", rates, 12);
    assert(n == 2 && rates[0] == 44100 && rates[1] == 48000);

    size_t prefixed_len = 0;
    uint8_t prefixed_caps[32];
    assert(dbus_prop_byte_array(prefixed, "Capabilities", prefixed_caps, sizeof(prefixed_caps), &prefixed_len));
    assert(prefixed_len == 6 && memcmp(prefixed_caps, caps, 6) == 0);

    /* Wire layouts for the other codecs this player offers. */
    static const uint8_t sbc[] = { 0x3f, 0xff, 2, 53 };
    n = a2dp_caps_rates(sbc, sizeof(sbc), "SBC", rates, 12);
    assert(n == 2 && rates[0] == 44100 && rates[1] == 48000);

    static const uint8_t ldac[] = { 0x2d, 0x01, 0x00, 0x00, 0xaa, 0x00, 0x3c, 0x07 };
    n = a2dp_caps_rates(ldac, sizeof(ldac), "LDAC", rates, 12);
    assert(n == 4 && rates[0] == 44100 && rates[1] == 48000 && rates[2] == 88200 && rates[3] == 96000);
    assert(a2dp_caps_match_codec(ldac, sizeof(ldac), 0xff, "LDAC"));
    assert(!a2dp_caps_match_codec(ldac, sizeof(ldac), 0xff, "aptX"));

    /* A truncated or unparsable array must yield nothing rather than junk.
     * Checked against a fresh buffer: reusing the one above would still hold
     * the decoded AAC blob and pass even if a parse failure wrote garbage. */
    uint8_t empty_caps[32];
    size_t empty_len = 1;
    memset(empty_caps, 0xAA, sizeof(empty_caps));
    assert(!dbus_prop_byte_array("string \"Capabilities\" variant array of bytes [ ]",
                                 "Capabilities", empty_caps, sizeof(empty_caps), &empty_len));
    assert(!dbus_prop_byte_array("string \"Capabilities\" variant array of bytes [ 80 01",
                                 "Capabilities", empty_caps, sizeof(empty_caps), &empty_len));
    assert(a2dp_caps_rates(empty_caps, 2, "AAC", rates, 12) == 0); /* AAC needs 3 bytes */

    /* aptX shares SBC's nibble layout but sits after the 6-byte vendor header. */
    static const uint8_t aptx[] = { 0x4f, 0x00, 0x00, 0x00, 0x01, 0x00, 0x32 };
    n = a2dp_caps_rates(aptx, sizeof(aptx), "aptX", rates, 12);
    assert(n == 2 && rates[0] == 44100 && rates[1] == 48000);
    assert(a2dp_caps_match_codec(aptx, sizeof(aptx), 0xff, "aptX"));
    assert(!a2dp_caps_match_codec(aptx, sizeof(aptx), 0xff, "aptX-HD"));
}

static void test_auto_codec_ranking(void) {
    char best[32];
    assert(bluealsa_best_available_codec("Available codecs: SBC AAC aptX LDAC\n", 0, best, sizeof(best)) &&
           strcmp(best, "LDAC") == 0);
    assert(bluealsa_best_available_codec("Selected codec: SBC\nAvailable codecs: SBC AAC\nRate: 44100\n", 0,
                                         best, sizeof(best)) && strcmp(best, "AAC") == 0);
    assert(bluealsa_best_available_codec("Available codecs: AAC aptX aptX-HD SBC\n", 0, best, sizeof(best)) &&
           strcmp(best, "aptX-HD") == 0);
    assert(bluealsa_best_available_codec("Available codecs: SBC aptX AAC\n", 0, best, sizeof(best)) &&
           strcmp(best, "aptX") == 0);
    /* Codecs outside the ranking are ignored; SBC is always the floor. */
    assert(bluealsa_best_available_codec("Available codecs: FastStream LC3plus SBC\n", 0, best, sizeof(best)) &&
           strcmp(best, "SBC") == 0);
    /* Not known yet: the accessory's endpoints have not been published. */
    assert(!bluealsa_best_available_codec("Available codecs: [ Unknown ]\n", 0, best, sizeof(best)));
    assert(!bluealsa_best_available_codec("Selected codec: AAC\n", 0, best, sizeof(best)));
    /* Codecs Automatic found unusable on a device are skipped. */
    assert(bluealsa_best_available_codec("Available codecs: SBC AAC aptX LDAC\n", 1u << 0, best, sizeof(best)) &&
           strcmp(best, "aptX") == 0);
    assert(bluealsa_best_available_codec("Available codecs: SBC AAC aptX LDAC\n", (1u << 0) | (1u << 2),
                                         best, sizeof(best)) && strcmp(best, "AAC") == 0);

    /* Per-device memory: a failed codec, and one followed by a quick
     * disconnect, are skipped next time; SBC is never rejected. */
    const char * dev_a = "/org/bluealsa/hci0/dev_AA_AA_AA_AA_AA_AA/a2dpsrc/sink";
    const char * dev_b = "/org/bluealsa/hci0/dev_BB_BB_BB_BB_BB_BB/a2dpsrc/sink";
    assert(bt_auto_rejected(dev_a) == 0);
    bt_auto_reject(dev_a, "LDAC");
    assert(bt_auto_rejected(dev_a) == 1u << 0 && bt_auto_rejected(dev_b) == 0);
    bt_auto_note_switch(dev_b, "aptX");
    bt_auto_note_disconnect(dev_b);
    assert(bt_auto_rejected(dev_b) == 1u << 2);
    bt_auto_reject(dev_b, "SBC");
    assert(bt_auto_rejected(dev_b) == 1u << 2);
}

static void test_rate_refusal_memory(void) {
    /* A refused rate survives routine re-application (e.g. on reconnect);
     * only a rate the user chooses clears it. */
    snprintf(bt_rate_refused_path, sizeof(bt_rate_refused_path), "%s", "/org/bluealsa/hci0/dev_X/a2dpsrc/sink");
    snprintf(bt_rate_refused_codec, sizeof(bt_rate_refused_codec), "%s", "AAC");
    bt_rate_refused_rate = 96000;
    bt_control_set_sample_rate(96000);
    bt_control_set_sample_rate(48000);
    assert(bt_rate_refused_rate == 96000 && bt_rate_refused_path[0]);
    bt_control_choose_sample_rate(48000);
    assert(bt_rate_refused_rate == 0 && !bt_rate_refused_path[0] && !bt_rate_refused_codec[0]);
}

int main(void) {
    /* A locking regression must fail promptly rather than hang the target. */
    alarm(10);
    test_auto_codec_ranking();
    test_rate_refusal_memory();

    int monitor_volume = -1;
    assert(parse_monitor_volume("0x7f7f", &monitor_volume) && monitor_volume == 127);
    assert(parse_monitor_volume("127", &monitor_volume) && monitor_volume == 127);
    assert(parse_monitor_volume("127[M]", &monitor_volume) && monitor_volume == 127);
    assert(!parse_monitor_volume("127junk", &monitor_volume));
    assert(!parse_monitor_volume("128", &monitor_volume));
    assert(!parse_monitor_volume("0x7f7fjunk", &monitor_volume));

    assert(bt_control_set_codec("auto"));
    /* The existing argv assertions below cover codec arguments, so the rate
     * is parked on one that adds no argument of its own to keep them
     * positional. Automatic is NOT that: it resolves to 44.1 kHz and so
     * carries --a2dp-force-audio-cd. A dedicated case for it follows. */
    bt_control_set_sample_rate(48000);
    test_daemon_argv();
    test_sbc_xq_lifecycle();
    test_modern_argv();
    test_force_audio_cd_argv();
    test_a2dp_capability_rates();
    test_bluez_paired_devices();
    alarm(0);
    puts("bluetooth-codec-selftest: PASS (BlueALSA 5 daemon/codec behavior, soft volume, BlueZ compatibility)");
    return 0;
}
