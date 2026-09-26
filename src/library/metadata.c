#include "metadata.h"
#include "library_endian.h"

#include "dr_flac.h"
#include "dr_wav.h"
#include "ogg_demux.h"
#include "stb_vorbis.h"
#include "mbedtls/base64.h"

#include "fallback_font.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include "artwork_coordinator.h"
#include <time.h>
#include <unistd.h>

/* Validates that adding size and pad to chunk_start will not overflow a
 * signed long. Returns true and writes the next seek offset to *out_next on
 * success, or false if size exceeds safe bounds. */
static bool safe_chunk_advance(long chunk_start, uint64_t size, long pad, long * out_next) {
    if (chunk_start < 0 || pad < 0) return false;
    if (size > (uint64_t) (LONG_MAX - pad)) return false;
    long size_l = (long) size; /* provably in-range now -- no implementation-defined truncation */
    if (chunk_start > LONG_MAX - size_l - pad) return false; /* would overflow the addition below */
    *out_next = chunk_start + size_l + pad;
    return true;
}

/* Same compressed-byte cap as m4a_read_cover_art() / gui.c's sidecar load.
 * Decode also rejects sources with post-scale RGB888 above 1200x1200 (JPEG
 * allows native up to 4096 if scaled <= 1200; PNG/BMP reject native > 1200);
 * this stops the malloc of a 10 MiB APIC before that decoder runs. Also used
 * as a hard cap on ID3 tag bodies, USLT/lyrics payloads, and base64
 * METADATA_BLOCK_PICTURE decodes so a lying size field cannot malloc tens of
 * MiB in-process. */
#define METADATA_BLOB_MAX_BYTES (4U * 1024U * 1024U)
#define EMBEDDED_COVER_MAX_BYTES METADATA_BLOB_MAX_BYTES
#define ID3_TEXT_FRAME_MAX_BYTES (64U * 1024U)

/* Enabled only in the disposable artwork helper, never in playback readers. */
static bool artwork_only;
/* Per-thread mode used by the lyrics worker: retain lyrics while skipping
 * caller-visible artwork extraction.  TLS keeps concurrent readers isolated. */
static _Thread_local bool lyrics_only;

static bool store_picture_bytes(track_metadata_t * out, const uint8_t * src, uint32_t n) {
    if (!out || out->picture_data != NULL || !src || n == 0) return false;
    if (n > EMBEDDED_COVER_MAX_BYTES) {
        out->picture_too_large = true;
        return false;
    }
    uint8_t * copy = malloc(n);
    if (!copy) return false;
    memcpy(copy, src, n);
    out->picture_data = copy;
    out->picture_size = n;
    return true;
}

static void copy_bounded(char * dst, size_t dst_size, const char * src, size_t src_len) {
    utf8_truncate_safe_bounded(dst, dst_size, src, src_len);
}

static bool parse_sequence_number(const char * text, size_t len, int * out_value) {
    size_t i = 0;
    while (i < len && (text[i] == ' ' || text[i] == '\t')) i++;
    unsigned value = 0;
    bool have_digit = false;
    while (i < len && text[i] >= '0' && text[i] <= '9') {
        have_digit = true;
        unsigned digit = (unsigned) (text[i++] - '0');
        if (value > 9999U) return false;
        value = value * 10U + digit;
    }
    if (!have_digit || value == 0 || value > 99999U) return false;
    *out_value = (int) value;
    return true;
}

static void apply_sequence_text(track_metadata_t * out, bool disc, const char * value, size_t value_len) {
    int number;
    if (!parse_sequence_number(value, value_len, &number)) return;
    if (disc) {
        out->disc_number = number;
        out->has_disc_number = true;
    } else {
        out->track_number = number;
        out->has_track_number = true;
    }
}

/* strtod() wrapper for REPLAYGAIN_* fields. Reports success only when at least
 * one digit was consumed and the parsed float is finite (rejecting NaN and Infinity). */
static bool parse_finite_double(const char * str, double * out) {
    char * end = NULL;
    double v = strtod(str, &end);
    if (end == str || !isfinite(v)) return false;
    *out = v;
    return true;
}

/* REPLAYGAIN_TRACK_GAIN/_PEAK/_ALBUM_GAIN/_ALBUM_PEAK key matching, shared
 * by any format that hands over an already-split key/value pair: Vorbis
 * comments (apply_vorbis_comment_field() below, after splitting on '='),
 * and APEv2 items (read_ape_metadata() further down -- already split on
 * disk, no '=' to strip, but real-world APEv2 taggers write these same four
 * keys too, so this tag family isn't actually Vorbis-specific despite the
 * name). value is bounded-copied into a small stack buffer first since
 * neither caller's value buffer is guaranteed NUL-terminated. */
/* Bounds ReplayGain values to sane limits (+-100 dB gain, 0..100 peak ratio)
 * to prevent floating-point overflow during linear gain conversion. */
#define REPLAYGAIN_GAIN_DB_LIMIT 100.0
#define REPLAYGAIN_PEAK_LIMIT 100.0

static bool parse_replaygain_gain(const char * str, double * out) {
    double v;
    if (!parse_finite_double(str, &v) || v < -REPLAYGAIN_GAIN_DB_LIMIT || v > REPLAYGAIN_GAIN_DB_LIMIT) return false;
    *out = v;
    return true;
}

static bool parse_replaygain_peak(const char * str, double * out) {
    double v;
    if (!parse_finite_double(str, &v) || v <= 0.0 || v > REPLAYGAIN_PEAK_LIMIT) return false;
    *out = v;
    return true;
}

static void apply_replaygain_field(track_metadata_t * out, const char * key, size_t key_len, const char * value,
                                    size_t value_len) {
    if (key_len != 21) return; /* every one of the four keys below is exactly 21 characters */
    char value_buf[32];
    copy_bounded(value_buf, sizeof(value_buf), value, value_len);
    double v;
    if (strncasecmp(key, "REPLAYGAIN_TRACK_GAIN", 21) == 0) {
        if (parse_replaygain_gain(value_buf, &v)) {
            out->replaygain_gain_db = v;
            out->has_replaygain = true;
        }
    } else if (strncasecmp(key, "REPLAYGAIN_TRACK_PEAK", 21) == 0) {
        if (parse_replaygain_peak(value_buf, &v)) {
            out->replaygain_peak = v;
            out->has_replaygain_peak = true;
        }
    } else if (strncasecmp(key, "REPLAYGAIN_ALBUM_GAIN", 21) == 0) {
        if (parse_replaygain_gain(value_buf, &v)) {
            out->replaygain_album_gain_db = v;
            out->has_replaygain_album = true;
        }
    } else if (strncasecmp(key, "REPLAYGAIN_ALBUM_PEAK", 21) == 0) {
        if (parse_replaygain_peak(value_buf, &v)) {
            out->replaygain_album_peak = v;
            out->has_replaygain_album_peak = true;
        }
    }
}

/* Generic Vorbis-Comment "KEY=VALUE" field matching -- shared by FLAC's
 * VORBIS_COMMENT block (below, via dr_flac's own iterator) and Opus's
 * OpusTags (read_opus_metadata(), same comment-list layout per RFC 7845
 * 5.2). Only the iteration mechanism differs between formats; this matching
 * logic has no dr_flac-specific coupling. */
static void apply_vorbis_comment_field(track_metadata_t * out, const char * comment, size_t comment_len,
                                       bool include_blobs) {
    const char * eq = memchr(comment, '=', comment_len);
    if (!eq) return;

    size_t key_len = (size_t) (eq - comment);
    const char * value = eq + 1;
    size_t value_len = comment_len - key_len - 1;

    if (key_len == 5 && strncasecmp(comment, "TITLE", 5) == 0) {
        copy_bounded(out->title, sizeof(out->title), value, value_len);
        out->has_title = true;
    } else if (key_len == 6 && strncasecmp(comment, "ARTIST", 6) == 0) {
        copy_bounded(out->artist, sizeof(out->artist), value, value_len);
        out->has_artist = true;
    } else if (key_len == 5 && strncasecmp(comment, "ALBUM", 5) == 0) {
        copy_bounded(out->album, sizeof(out->album), value, value_len);
        out->has_album = true;
    } else if (key_len == 11 && strncasecmp(comment, "ALBUMARTIST", 11) == 0) {
        copy_bounded(out->album_artist, sizeof(out->album_artist), value, value_len);
        out->has_album_artist = true;
    } else if (key_len == 5 && strncasecmp(comment, "GENRE", 5) == 0) {
        copy_bounded(out->genre, sizeof(out->genre), value, value_len);
        out->has_genre = true;
    } else if (key_len == 11 && strncasecmp(comment, "TRACKNUMBER", 11) == 0) {
        apply_sequence_text(out, false, value, value_len);
    } else if ((key_len == 10 && strncasecmp(comment, "DISCNUMBER", 10) == 0) ||
               (key_len == 10 && strncasecmp(comment, "DISKNUMBER", 10) == 0)) {
        apply_sequence_text(out, true, value, value_len);
    } else if (key_len == 21 && strncasecmp(comment, "REPLAYGAIN_", 11) == 0) {
        /* strtod() stops at a trailing " dB" on gain fields, no need to
         * strip it -- apply_replaygain_field() re-checks the full key
         * against each of the four exact REPLAYGAIN_* names itself. */
        apply_replaygain_field(out, comment, key_len, value, value_len);
    } else if (include_blobs && !artwork_only && out->lyrics == NULL && value_len > 0 &&
               ((key_len == 6 && strncasecmp(comment, "LYRICS", 6) == 0) ||
                (key_len == 14 && strncasecmp(comment, "UNSYNCEDLYRICS", 14) == 0))) {
        /* Plain UTF-8 text, no ID3v2-style encoding byte to strip -- unlike
         * decode_id3v2_uslt_frame()'s own malloc'd buffer, this is just a
         * straight copy. Keeps the first of either key found, same
         * first-wins precedent as picture_data. May or may not contain
         * [mm:ss.xx] LRC timestamps -- see track_metadata_t's own lyrics
         * field comment. */
        if (value_len > METADATA_BLOB_MAX_BYTES) return;
        char * lyrics = malloc(value_len + 1);
        if (lyrics) {
            memcpy(lyrics, value, value_len);
            lyrics[value_len] = '\0';
            out->lyrics = lyrics;
        }
    }
}

/* Parses a METADATA_BLOCK_PICTURE structure (https://xiph.org/flac/format.html#metadata_block_picture,
 * also used verbatim -- just base64-wrapped as an Ogg/Opus comment value --
 * by Xiph's own "PICTURE" tag convention for Vorbis-Comment-based formats):
 * a fixed run of big-endian fields (type, then length-prefixed MIME string,
 * length-prefixed description, width/height/depth/color-count) followed by
 * the length-prefixed image bytes themselves. dr_flac parses this same
 * layout internally for native FLAC picture blocks (exposed directly via
 * meta->data.picture in flac_meta_cb() above), but that parsing isn't
 * reusable source code -- this is a from-scratch equivalent for the
 * base64-decoded bytes the Opus path hands it, keeping only the final
 * image bytes since track_metadata_t has no fields for the others. */
static void parse_flac_picture_block(const uint8_t * data, size_t size, track_metadata_t * out) {
    if (out->picture_data != NULL) return; /* keep the first picture found, same as FLAC/MP3/M4A */

    size_t pos = 0;
    if (pos + 4 > size) return;
    pos += 4; /* picture type -- not discriminated on, matching FLAC/MP3/M4A's "first picture wins" */

    if (pos + 4 > size) return;
    uint32_t mime_len = ((uint32_t) data[pos] << 24) | ((uint32_t) data[pos + 1] << 16) | ((uint32_t) data[pos + 2] << 8) | data[pos + 3];
    pos += 4;
    if (pos + mime_len > size) return;
    pos += mime_len;

    if (pos + 4 > size) return;
    uint32_t desc_len = ((uint32_t) data[pos] << 24) | ((uint32_t) data[pos + 1] << 16) | ((uint32_t) data[pos + 2] << 8) | data[pos + 3];
    pos += 4;
    if (pos + desc_len > size) return;
    pos += desc_len;

    if (pos + 16 > size) return; /* width, height, depth, color-count -- 4 bytes each */
    pos += 16;

    if (pos + 4 > size) return;
    uint32_t pic_data_len = ((uint32_t) data[pos] << 24) | ((uint32_t) data[pos + 1] << 16) | ((uint32_t) data[pos + 2] << 8) | data[pos + 3];
    pos += 4;
    if (pic_data_len == 0 || pos + pic_data_len > size) return;

    store_picture_bytes(out, data + pos, pic_data_len);
}

/* ---- FLAC: VORBIS_COMMENT block ---- */

typedef struct {
    track_metadata_t * out;
    bool include_blobs;
} flac_metadata_ctx_t;

static void flac_meta_cb(void * user_data, drflac_metadata * meta) {
    flac_metadata_ctx_t * ctx = (flac_metadata_ctx_t *) user_data;
    track_metadata_t * out = ctx->out;

    if (meta->type == DRFLAC_METADATA_BLOCK_TYPE_PICTURE) {
        /* dr_flac frees its own copy of pPictureData right after this
         * callback returns, so it has to be copied here, not just
         * pointed to. */
        if (ctx->include_blobs && !lyrics_only && out->picture_data == NULL && meta->data.picture.pictureDataSize > 0 &&
            meta->data.picture.pPictureData != NULL) {
            store_picture_bytes(out, meta->data.picture.pPictureData, meta->data.picture.pictureDataSize);
        }
        return;
    }

    if (meta->type != DRFLAC_METADATA_BLOCK_TYPE_VORBIS_COMMENT) return;

    drflac_vorbis_comment_iterator iter;
    drflac_init_vorbis_comment_iterator(&iter, meta->data.vorbis_comment.commentCount, meta->data.vorbis_comment.pComments);

    drflac_uint32 comment_len;
    const char * comment;
    while ((comment = drflac_next_vorbis_comment(&iter, &comment_len)) != NULL) {
        /* Scan-only reads do not need lyrics. Avoid allocating a potentially
         * multi-megabyte comment that the database immediately discards. */
        if (!ctx->include_blobs) {
            const char * eq = memchr(comment, '=', comment_len);
            size_t key_len = eq ? (size_t) (eq - comment) : 0;
            if ((key_len == 6 && strncasecmp(comment, "LYRICS", 6) == 0) ||
                (key_len == 14 && strncasecmp(comment, "UNSYNCEDLYRICS", 14) == 0))
                continue;
        }
        apply_vorbis_comment_field(out, comment, comment_len, ctx->include_blobs);
    }
}

static uint32_t read_be32(const uint8_t * b, bool synchsafe); /* defined below, in the ID3v2 section */

static void read_flac_text_metadata(const char * path, track_metadata_t * out, bool include_blobs) {
        FILE * f = fopen(path, "rb");
        if (!f) return;

        /* Some taggers prepend a plain ID3v2 tag before "fLaC" -- dr_flac's
         * own drflac_init() skips exactly this (see dr_flac.h's "Skip over
         * any ID3 tags" loop) before falling through to the full-parse
         * path, so this lightweight walker has to do the same or an
         * ID3-prefixed FLAC loses ALL of its text tags in no-artwork/
         * lyrics-only mode, not just the album art. */
        uint8_t sig[4];
        for (;;) {
            if (fread(sig, 1, 4, f) != 4) { fclose(f); return; }
            if (memcmp(sig, "ID3", 3) != 0) break;
            uint8_t rest[6];
            if (fread(rest, 1, sizeof(rest), f) != sizeof(rest)) { fclose(f); return; }
            uint32_t tag_size = read_be32(&rest[2], true);
            if (rest[1] & 0x10) tag_size += 10; /* footer present */
            if (fseek(f, (long) tag_size, SEEK_CUR) != 0) { fclose(f); return; }
        }
        if (memcmp(sig, "fLaC", 4) != 0) {
            fclose(f);
            return;
        }
        bool last = false;
        while (!last) {
            uint8_t h[4];
            if (fread(h, 1, 4, f) != 4) break;
            last = (h[0] & 0x80) != 0;
            uint32_t n = ((uint32_t)(h[1] << 16) | ((uint32_t)h[2] << 8) | h[3]);
            if ((h[0] & 0x7f) != 4) {
                if (fseek(f, (long)n, SEEK_CUR) != 0) break;
                continue;
            }
            if (n > METADATA_BLOB_MAX_BYTES) break;
            uint8_t * block = malloc(n ? n : 1);
            if (!block || fread(block, 1, n, f) != n) { free(block); break; }
            if (n >= 8) {
                uint32_t pos = 4;
                uint32_t vendor = (uint32_t)block[0] | ((uint32_t)block[1] << 8) |
                                  ((uint32_t)block[2] << 16) | ((uint32_t)block[3] << 24);
                if (vendor <= n - pos - 4) {
                    pos += vendor;
                    uint32_t count = (uint32_t)block[pos] | ((uint32_t)block[pos+1] << 8) |
                                     ((uint32_t)block[pos+2] << 16) | ((uint32_t)block[pos+3] << 24);
                    pos += 4;
                    for (uint32_t i = 0; i < count && pos + 4 <= n; i++) {
                        uint32_t len = (uint32_t)block[pos] | ((uint32_t)block[pos+1] << 8) |
                                       ((uint32_t)block[pos+2] << 16) | ((uint32_t)block[pos+3] << 24);
                        pos += 4;
                        if (len > n - pos) break;
                        const char * field = (const char *)&block[pos];
                        size_t eq = 0;
                        while (eq < len && field[eq] != '=') eq++;
                        bool lyric = (eq == 6 && strncasecmp(field, "LYRICS", 6) == 0) ||
                                     (eq == 14 && strncasecmp(field, "UNSYNCEDLYRICS", 14) == 0);
                        if (include_blobs || !lyric) apply_vorbis_comment_field(out, field, len, include_blobs);
                        pos += len;
                    }
                }
            }
            free(block);
        }
        fclose(f);
        return;
    }

static void read_flac_metadata(const char * path, track_metadata_t * out, bool include_blobs) {
    if (lyrics_only || !include_blobs) {
        read_flac_text_metadata(path, out, include_blobs);
        return;
    }
    flac_metadata_ctx_t ctx = { out, include_blobs };
    drflac * flac = drflac_open_file_with_metadata(path, flac_meta_cb, &ctx, NULL);
    if (flac) drflac_close(flac);
}

/* ---- WAV: RIFF LIST/INFO chunk (dr_wav parses this natively) ---- */

static void read_wav_metadata(const char * path, track_metadata_t * out) {
    drwav wav;
    if (!drwav_init_file_with_metadata(&wav, path, drwav_metadata_type_list_all_info_strings, NULL)) return;

    for (drwav_uint32 i = 0; i < wav.metadataCount; i++) {
        drwav_metadata * meta = &wav.pMetadata[i];
        switch (meta->type) {
            case drwav_metadata_type_list_info_title:
                copy_bounded(out->title, sizeof(out->title), meta->data.infoText.pString, meta->data.infoText.stringLength);
                out->has_title = true;
                break;
            case drwav_metadata_type_list_info_artist:
                copy_bounded(out->artist, sizeof(out->artist), meta->data.infoText.pString, meta->data.infoText.stringLength);
                out->has_artist = true;
                break;
            case drwav_metadata_type_list_info_album:
                copy_bounded(out->album, sizeof(out->album), meta->data.infoText.pString, meta->data.infoText.stringLength);
                out->has_album = true;
                break;
            case drwav_metadata_type_list_info_tracknumber:
                apply_sequence_text(out, false, meta->data.infoText.pString, meta->data.infoText.stringLength);
                break;
            default:
                break;
        }
    }

    drwav_uninit(&wav);
}

/* ---- MP3: ID3v2 (falling back to ID3v1) -- dr_mp3 has no tag support ---- */

static uint32_t read_be32(const uint8_t * b, bool synchsafe) {
    if (synchsafe) {
        return ((uint32_t) (b[0] & 0x7F) << 21) | ((uint32_t) (b[1] & 0x7F) << 14) |
               ((uint32_t) (b[2] & 0x7F) << 7) | (uint32_t) (b[3] & 0x7F);
    }
    return ((uint32_t) b[0] << 24) | ((uint32_t) b[1] << 16) | ((uint32_t) b[2] << 8) | (uint32_t) b[3];
}

/* Decodes an ID3v2 text frame body (1 encoding byte + text) into UTF-8.
 * Handles ISO-8859-1/UTF-8 (copied as-is) and UTF-16/UTF-16BE (converted,
 * including surrogate pairs) -- the encodings actually used in practice.
 * Unsynchronization is not undone; this covers the common case (tags
 * written by mainstream taggers), not every ID3v2 edge case. */
static void decode_id3v2_text_frame(const uint8_t * data, uint32_t size, char * out, size_t out_size) {
    out[0] = '\0';
    if (size == 0) return;

    uint8_t encoding = data[0];
    const uint8_t * text = data + 1;
    uint32_t text_len = size - 1;

    if (encoding == 0x00 || encoding == 0x03) {
        copy_bounded(out, out_size, (const char *) text, text_len);
        return;
    }

    /* UTF-16 with BOM (0x01) or UTF-16BE without BOM (0x02) */
    bool big_endian = (encoding == 0x02);
    size_t src = 0;
    if (encoding == 0x01 && text_len >= 2) {
        if (text[0] == 0xFF && text[1] == 0xFE) { big_endian = false; src = 2; }
        else if (text[0] == 0xFE && text[1] == 0xFF) { big_endian = true; src = 2; }
    }

    size_t out_pos = 0;
    while (src + 1 < text_len && out_pos + 4 < out_size) {
        uint16_t unit = big_endian ? (uint16_t) ((text[src] << 8) | text[src + 1])
                                   : (uint16_t) ((text[src + 1] << 8) | text[src]);
        src += 2;
        if (unit == 0) break;

        uint32_t codepoint = unit;
        if (unit >= 0xD800 && unit <= 0xDBFF && src + 1 < text_len) {
            uint16_t unit2 = big_endian ? (uint16_t) ((text[src] << 8) | text[src + 1])
                                        : (uint16_t) ((text[src + 1] << 8) | text[src]);
            if (unit2 >= 0xDC00 && unit2 <= 0xDFFF) {
                codepoint = 0x10000 + (((uint32_t) unit - 0xD800) << 10) + (unit2 - 0xDC00);
                src += 2;
            }
        }

        if (codepoint < 0x80) {
            out[out_pos++] = (char) codepoint;
        } else if (codepoint < 0x800) {
            out[out_pos++] = (char) (0xC0 | (codepoint >> 6));
            out[out_pos++] = (char) (0x80 | (codepoint & 0x3F));
        } else if (codepoint < 0x10000) {
            out[out_pos++] = (char) (0xE0 | (codepoint >> 12));
            out[out_pos++] = (char) (0x80 | ((codepoint >> 6) & 0x3F));
            out[out_pos++] = (char) (0x80 | (codepoint & 0x3F));
        } else {
            out[out_pos++] = (char) (0xF0 | (codepoint >> 18));
            out[out_pos++] = (char) (0x80 | ((codepoint >> 12) & 0x3F));
            out[out_pos++] = (char) (0x80 | ((codepoint >> 6) & 0x3F));
            out[out_pos++] = (char) (0x80 | (codepoint & 0x3F));
        }
    }
    out[out_pos] = '\0';
}

/* Scans forward from `start` for the null terminator appropriate to an
 * ID3v2 text encoding byte (1-byte null for Latin-1/UTF-8, 2-byte null for
 * UTF-16/UTF-16BE) and returns the position right after it, or 0 if none
 * is found before `size` (0 is never a valid return otherwise, since
 * start is always >= 1 in every caller). */
static uint32_t id3v2_terminated_field_end(const uint8_t * data, uint32_t size, uint32_t start, uint8_t encoding) {
    bool wide_terminator = (encoding == 0x01 || encoding == 0x02);
    uint32_t pos = start;
    while (pos < size) {
        if (!wide_terminator && data[pos] == '\0') return pos + 1;
        if (wide_terminator && pos + 1 < size && data[pos] == '\0' && data[pos + 1] == '\0') return pos + 2;
        pos += wide_terminator ? 2 : 1;
    }
    return 0;
}

/* APIC frame body: 1 byte text encoding (applies to the description field
 * only -- MIME type is always Latin-1 regardless), null-terminated MIME
 * type string, 1 byte picture type, a description string terminated per
 * the encoding byte (1-byte null for Latin-1/UTF-8, 2-byte null for
 * UTF-16), then the raw picture bytes to the end of the frame. */
static void decode_id3v2_apic_frame(const uint8_t * data, uint32_t size, track_metadata_t * out) {
    if (out->picture_data != NULL || size < 2) return; /* keep the first picture found, same as FLAC */

    uint8_t encoding = data[0];
    uint32_t pos = 1;

    const uint8_t * mime_end = memchr(data + pos, '\0', size - pos);
    if (!mime_end) return;
    pos = (uint32_t) (mime_end - data) + 1;

    if (pos >= size) return;
    pos += 1; /* picture type byte */

    uint32_t desc_start = pos;
    pos = id3v2_terminated_field_end(data, size, desc_start, encoding);
    if (pos == 0 || pos <= desc_start || pos > size) return; /* malformed -- no terminator found */

    uint32_t picture_size = size - pos;
    if (picture_size == 0) return;

    store_picture_bytes(out, data + pos, picture_size);
}

/* TXXX frame body: 1 byte text encoding, a description string terminated
 * per the encoding byte, then a value string (encoded the same way) running
 * to the end of the frame with no terminator required. Taggers that write
 * ReplayGain into ID3v2 (rather than APEv2 or the binary RVA2 frame) use
 * this with descriptions "REPLAYGAIN_TRACK_GAIN"/"REPLAYGAIN_TRACK_PEAK", or
 * the ALBUM_ variants for whole-album gain/peak. */
static void decode_id3v2_txxx_frame(const uint8_t * data, uint32_t size, track_metadata_t * out) {
    if (size < 2) return;

    uint8_t encoding = data[0];
    uint32_t desc_start = 1;
    uint32_t value_start = id3v2_terminated_field_end(data, size, desc_start, encoding);
    if (value_start == 0 || value_start > size) return;

    /* decode_id3v2_text_frame() expects data[0] to be the encoding byte,
     * so re-prefix each field into a small stack buffer to reuse it for
     * both the description and the value. */
    uint8_t desc_buf[64];
    uint32_t desc_len = value_start - desc_start;
    if (desc_len > sizeof(desc_buf) - 1) desc_len = sizeof(desc_buf) - 1;
    desc_buf[0] = encoding;
    memcpy(desc_buf + 1, data + desc_start, desc_len);

    char description[64];
    decode_id3v2_text_frame(desc_buf, desc_len + 1, description, sizeof(description));

    uint32_t value_len = size - value_start;
    uint8_t value_buf[40];
    uint32_t copy_len = value_len;
    if (copy_len > sizeof(value_buf) - 1) copy_len = sizeof(value_buf) - 1;
    value_buf[0] = encoding;
    memcpy(value_buf + 1, data + value_start, copy_len);

    char value_str[40];
    decode_id3v2_text_frame(value_buf, copy_len + 1, value_str, sizeof(value_str));

    /* description/value_str are both already fully decoded, NUL-terminated
     * C strings at this point -- apply_replaygain_field() (metadata.c, near
     * apply_vorbis_comment_field()) does its own exact-name matching and
     * finite-number validation, same as it does for Vorbis/APEv2, so this
     * is a plain delegation rather than re-implementing that matching here
     * a third time. A no-op for any TXXX description that isn't one of the
     * four REPLAYGAIN_* names. */
    apply_replaygain_field(out, description, strlen(description), value_str, strlen(value_str));
}

/* v2.2 PIC frame body: 1 byte text encoding, a FIXED 3-character image
 * format code (not a null-terminated MIME string like APIC's -- e.g.
 * "JPG"/"PNG", always exactly 3 bytes, no terminator), 1 byte picture
 * type, a description string terminated per the encoding byte, then the
 * raw picture bytes to the end of the frame. */
static void decode_id3v2_pic_frame(const uint8_t * data, uint32_t size, track_metadata_t * out) {
    if (out->picture_data != NULL || size < 5) return; /* keep the first picture found, same as APIC */

    uint8_t encoding = data[0];
    uint32_t pos = 1 + 3; /* encoding byte + fixed 3-char image format code */
    pos += 1; /* picture type byte */
    if (pos >= size) return;

    uint32_t desc_start = pos;
    pos = id3v2_terminated_field_end(data, size, desc_start, encoding);
    if (pos == 0 || pos <= desc_start || pos > size) return; /* malformed -- no terminator found */

    uint32_t picture_size = size - pos;
    if (picture_size == 0) return;

    store_picture_bytes(out, data + pos, picture_size);
}

/* USLT (Unsynchronised lyrics/text transcription) frame body: 1 byte text
 * encoding, a FIXED 3-byte language code (not null-terminated -- e.g. "eng",
 * always exactly 3 bytes), a short content-descriptor string terminated per
 * the encoding byte (almost always empty in practice), then the actual
 * lyrics text (encoded the same way as the description) running to the end
 * of the frame with no terminator required -- same overall shape as TXXX's
 * own description+value pair above, just with a fixed-width field first
 * instead of another terminated one. Despite the "Unsynchronised" name, some
 * taggers embed real [mm:ss.xx]-tagged LRC text here anyway -- the caller
 * (lyrics_load_thread_func(), gui.c) tries lrc parsing first and falls back
 * to a plain unsynced display otherwise, same treatment an external .lrc
 * sidecar's own content would need if it turned out to have no timestamps. */
static void decode_id3v2_uslt_frame(const uint8_t * data, uint32_t size, track_metadata_t * out) {
    if (out->lyrics != NULL || size < 5) return; /* keep the first lyrics frame found, same as APIC's picture */

    uint8_t encoding = data[0];
    uint32_t desc_start = 1 + 3; /* encoding byte + fixed 3-byte language code */

    uint32_t text_start = id3v2_terminated_field_end(data, size, desc_start, encoding);
    if (text_start == 0 || text_start > size) return; /* malformed -- no terminator found */

    uint32_t text_len = size - text_start;
    if (text_len == 0 || text_len > METADATA_BLOB_MAX_BYTES) return;

    /* decode_id3v2_text_frame() expects data[0] to be the encoding byte, so
     * re-prefix the text into a malloc'd buffer the same way decode_id3v2_
     * txxx_frame() does for its own (much smaller) fields -- lyrics can run
     * several KB, so this can't reuse a small stack buffer. text_len*2+16 is
     * a generous worst-case UTF-16-source-to-UTF-8 expansion bound (see
     * decode_id3v2_text_frame()'s own 4-bytes-out-per-2-bytes-in surrogate
     * pair handling), plenty for Latin-1/UTF-8 source (1 byte in, at most 1
     * byte out) too. */
    uint8_t * prefixed = malloc((size_t) text_len + 1);
    if (!prefixed) return;
    prefixed[0] = encoding;
    memcpy(prefixed + 1, data + text_start, text_len);

    size_t out_size = (size_t) text_len * 2 + 16;
    char * lyrics = malloc(out_size);
    if (lyrics) {
        decode_id3v2_text_frame(prefixed, text_len + 1, lyrics, out_size);
        if (lyrics[0] != '\0') out->lyrics = lyrics;
        else free(lyrics);
    }
    free(prefixed);
}

static uint32_t id3_deunsync_in_place(uint8_t * data, uint32_t size) {
    uint32_t write_pos = 0;
    for (uint32_t read_pos = 0; read_pos < size; read_pos++) {
        uint8_t b = data[read_pos];
        data[write_pos++] = b;
        if (b == 0xFF && read_pos + 1 < size && data[read_pos + 1] == 0x00) read_pos++;
    }
    return write_pos;
}

typedef struct {
    FILE * f;
    uint32_t raw_remaining;
    bool tag_unsync;
    bool previous_ff;
} id3_stream_t;

static bool id3_stream_read(id3_stream_t * s, uint8_t * out, uint32_t count) {
    uint32_t written = 0;
    while (written < count && s->raw_remaining > 0) {
        int c = fgetc(s->f);
        if (c == EOF) return false;
        s->raw_remaining--;
        if (s->tag_unsync && s->previous_ff && c == 0x00) {
            s->previous_ff = false;
            continue;
        }
        out[written++] = (uint8_t) c;
        s->previous_ff = c == 0xFF;
    }
    return written == count;
}

static bool id3_stream_skip(id3_stream_t * s, uint32_t count) {
    if (!s->tag_unsync) {
        if (count > s->raw_remaining || fseek(s->f, (long) count, SEEK_CUR) != 0) return false;
        s->raw_remaining -= count;
        s->previous_ff = false;
        return true;
    }
    uint8_t scratch[512];
    while (count > 0) {
        uint32_t n = count < sizeof(scratch) ? count : (uint32_t) sizeof(scratch);
        if (!id3_stream_read(s, scratch, n)) return false;
        count -= n;
    }
    return true;
}

/* Oversized tags are usually small text frames plus one very large APIC.
 * Walk frame headers directly from the file so that APIC can be skipped
 * without discarding title/artist/album. The file is bounded by tag_end on
 * every read and seek; no allocation is based on the overall tag size. */
static bool read_id3v2_streaming(FILE * f, track_metadata_t * out, uint8_t major_version,
                                 uint8_t tag_flags, uint32_t tag_size, bool include_blobs) {
    long body_start = ftell(f);
    if (body_start < 0 || tag_size > (uint32_t) LONG_MAX || body_start > LONG_MAX - (long) tag_size) return false;
    long tag_end = body_start + (long) tag_size;
    bool found_any = false;
    id3_stream_t stream = { f, tag_size, (tag_flags & 0x80) != 0, false };

    if (major_version >= 3 && (tag_flags & 0x40)) {
        uint8_t ext[4];
        if (!id3_stream_read(&stream, ext, 4)) return false;
        uint32_t ext_size = read_be32(ext, major_version >= 4);
        uint64_t ext_total = major_version >= 4 ? ext_size : (uint64_t) ext_size + 4U;
        if (ext_total < 4 || ext_total > tag_size || !id3_stream_skip(&stream, (uint32_t) ext_total - 4U)) return false;
    }

    for (;;) {
        uint32_t header_size = major_version <= 2 ? 6U : 10U;
        uint8_t hdr[10];
        if (!id3_stream_read(&stream, hdr, header_size)) break;
        if (hdr[0] == 0) break;

        char id[5] = {0};
        uint32_t frame_size;
        uint8_t flags2 = 0;
        if (major_version <= 2) {
            memcpy(id, hdr, 3);
            frame_size = ((uint32_t) hdr[3] << 16) | ((uint32_t) hdr[4] << 8) | hdr[5];
        } else {
            memcpy(id, hdr, 4);
            frame_size = read_be32(hdr + 4, major_version >= 4);
            flags2 = hdr[9];
        }
        bool is_text = strcmp(id, major_version <= 2 ? "TT2" : "TIT2") == 0 ||
                       strcmp(id, major_version <= 2 ? "TP1" : "TPE1") == 0 ||
                       strcmp(id, major_version <= 2 ? "TAL" : "TALB") == 0 ||
                       strcmp(id, major_version <= 2 ? "TP2" : "TPE2") == 0 ||
                       strcmp(id, major_version <= 2 ? "TCO" : "TCON") == 0 ||
                       strcmp(id, major_version <= 2 ? "TRK" : "TRCK") == 0 ||
                       strcmp(id, major_version <= 2 ? "TPA" : "TPOS") == 0;
        bool is_picture = strcmp(id, major_version <= 2 ? "PIC" : "APIC") == 0;
        bool is_txxx = major_version >= 3 && strcmp(id, "TXXX") == 0;
        bool is_lyrics = major_version >= 3 && strcmp(id, "USLT") == 0;
        uint32_t limit = (is_text || is_txxx) ? ID3_TEXT_FRAME_MAX_BYTES : METADATA_BLOB_MAX_BYTES;

        bool compressed = false, encrypted = false, grouped = false, frame_unsync = false, data_len = false;
        if (major_version >= 4) {
            compressed = (flags2 & 0x08) != 0; encrypted = (flags2 & 0x04) != 0;
            grouped = (flags2 & 0x40) != 0; frame_unsync = (flags2 & 0x02) != 0;
            data_len = (flags2 & 0x01) != 0;
        } else if (major_version == 3) {
            compressed = (flags2 & 0x80) != 0; encrypted = (flags2 & 0x40) != 0;
            grouped = (flags2 & 0x20) != 0; data_len = compressed;
        }

        bool wanted = is_text || is_txxx ||
                      (include_blobs && !lyrics_only && is_picture) ||
                      (include_blobs && is_lyrics && !artwork_only);
        if (wanted && !compressed && !encrypted && frame_size <= limit) {
            uint8_t * data = malloc(frame_size ? frame_size : 1);
            bool read_ok = data && id3_stream_read(&stream, data, frame_size);
            if (read_ok) {
                uint32_t n = frame_size;
                if (frame_unsync) n = id3_deunsync_in_place(data, n);
                uint32_t prefix = (grouped ? 1U : 0U) + (data_len ? 4U : 0U);
                if (prefix <= n) {
                    uint8_t * payload = data + prefix;
                    n -= prefix;
                    if (strcmp(id, major_version <= 2 ? "TT2" : "TIT2") == 0) {
                        decode_id3v2_text_frame(payload, n, out->title, sizeof(out->title)); out->has_title = out->title[0] != 0;
                    } else if (strcmp(id, major_version <= 2 ? "TP1" : "TPE1") == 0) {
                        decode_id3v2_text_frame(payload, n, out->artist, sizeof(out->artist)); out->has_artist = out->artist[0] != 0;
                    } else if (strcmp(id, major_version <= 2 ? "TAL" : "TALB") == 0) {
                        decode_id3v2_text_frame(payload, n, out->album, sizeof(out->album)); out->has_album = out->album[0] != 0;
                    } else if (strcmp(id, major_version <= 2 ? "TP2" : "TPE2") == 0) {
                        decode_id3v2_text_frame(payload, n, out->album_artist, sizeof(out->album_artist)); out->has_album_artist = out->album_artist[0] != 0;
                    } else if (strcmp(id, major_version <= 2 ? "TCO" : "TCON") == 0) {
                        decode_id3v2_text_frame(payload, n, out->genre, sizeof(out->genre)); out->has_genre = out->genre[0] != 0;
                    } else if (strcmp(id, major_version <= 2 ? "TRK" : "TRCK") == 0 ||
                               strcmp(id, major_version <= 2 ? "TPA" : "TPOS") == 0) {
                        char sequence[32];
                        decode_id3v2_text_frame(payload, n, sequence, sizeof(sequence));
                        apply_sequence_text(out, id[1] == 'P', sequence, strlen(sequence));
                    } else if (is_picture) {
                        if (major_version <= 2) decode_id3v2_pic_frame(payload, n, out); else decode_id3v2_apic_frame(payload, n, out);
                    } else if (is_txxx) decode_id3v2_txxx_frame(payload, n, out);
                    else if (is_lyrics) decode_id3v2_uslt_frame(payload, n, out);
                    found_any = true;
                }
            }
            free(data);
            if (!read_ok) break;
        } else {
            /* A large APIC/PIC body is intentionally skipped instead of
             * allocated. Preserve that fact for artwork clients so an
             * oversized source is not reported as permanently missing. */
            if (include_blobs && !lyrics_only && is_picture && frame_size > EMBEDDED_COVER_MAX_BYTES)
                out->picture_too_large = true;
            if (!id3_stream_skip(&stream, frame_size)) break;
        }
    }
    fseek(f, tag_end, SEEK_SET);
    return found_any;
}

static bool read_id3v2(FILE * f, track_metadata_t * out, bool include_blobs) {
    uint8_t header[10];
    if (fread(header, 1, sizeof(header), f) != sizeof(header)) return false;
    if (memcmp(header, "ID3", 3) != 0) return false;

    uint8_t major_version = header[3];
    uint8_t flags = header[5];
    if (major_version < 2 || major_version > 4) return false;
    uint32_t tag_size = read_be32(&header[6], true); /* overall tag size is always synchsafe */
    if (tag_size == 0) return false;
    /* A database scan only needs bounded text frames. Always stream in that
     * mode so a large APIC/USLT body is skipped without allocating the
     * complete ID3 tag first. */
    if (artwork_only || lyrics_only || !include_blobs || tag_size > METADATA_BLOB_MAX_BYTES)
        return read_id3v2_streaming(f, out, major_version, flags, tag_size, include_blobs);

    uint8_t * tag_data = malloc(tag_size);
    if (!tag_data) return false;
    if (fread(tag_data, 1, tag_size, f) != tag_size) {
        free(tag_data);
        return false;
    }

    /* De-unsynchronize tag body in place if the header unsynchronization flag
     * (bit 0x80) is set, stripping inserted 0x00 bytes following 0xFF bytes. */
    if (flags & 0x80) {
        tag_size = id3_deunsync_in_place(tag_data, tag_size);
    }

    uint32_t body_start = 0;

    /* Skip the optional extended header if present (flags bit 0x40).
     * In ID3v2.4 the size includes itself; in ID3v2.3 it excludes the 4-byte size field. */
    if (major_version >= 3 && (flags & 0x40) && tag_size >= 4) {
        uint32_t ext_size = read_be32(tag_data, major_version >= 4);
        uint32_t ext_total = (major_version >= 4) ? ext_size : (ext_size + 4);
        if (ext_total <= tag_size) body_start = ext_total;
    }

    bool found_any = false;

    if (major_version <= 2) {
        /* ID3v2.2 uses 3-character frame IDs and 6-byte frame headers (3-char ID
         * plus 3-byte size without flags). PIC picture frames use a distinct layout. */
        uint32_t pos = body_start;
        while (pos + 6 <= tag_size) {
            if (tag_data[pos] == '\0') break;

            char frame_id[4];
            memcpy(frame_id, tag_data + pos, 3);
            frame_id[3] = '\0';
            uint32_t frame_size =
                ((uint32_t) tag_data[pos + 3] << 16) | ((uint32_t) tag_data[pos + 4] << 8) | tag_data[pos + 5];
            pos += 6;
            if (pos + frame_size > tag_size) break;

            if (strcmp(frame_id, "TT2") == 0) {
                decode_id3v2_text_frame(tag_data + pos, frame_size, out->title, sizeof(out->title));
                out->has_title = out->title[0] != '\0';
                found_any = true;
            } else if (strcmp(frame_id, "TP1") == 0) {
                decode_id3v2_text_frame(tag_data + pos, frame_size, out->artist, sizeof(out->artist));
                out->has_artist = out->artist[0] != '\0';
                found_any = true;
            } else if (strcmp(frame_id, "TAL") == 0) {
                decode_id3v2_text_frame(tag_data + pos, frame_size, out->album, sizeof(out->album));
                out->has_album = out->album[0] != '\0';
                found_any = true;
            } else if (strcmp(frame_id, "TP2") == 0) {
                decode_id3v2_text_frame(tag_data + pos, frame_size, out->album_artist, sizeof(out->album_artist));
                out->has_album_artist = out->album_artist[0] != '\0';
                found_any = true;
            } else if (strcmp(frame_id, "TCO") == 0) {
                decode_id3v2_text_frame(tag_data + pos, frame_size, out->genre, sizeof(out->genre));
                out->has_genre = out->genre[0] != '\0';
                found_any = true;
            } else if (strcmp(frame_id, "TRK") == 0 || strcmp(frame_id, "TPA") == 0) {
                char sequence[32];
                decode_id3v2_text_frame(tag_data + pos, frame_size, sequence, sizeof(sequence));
                apply_sequence_text(out, frame_id[1] == 'P', sequence, strlen(sequence));
                found_any = true;
            } else if (strcmp(frame_id, "PIC") == 0) {
                decode_id3v2_pic_frame(tag_data + pos, frame_size, out);
                found_any = true;
            }

            pos += frame_size;
        }

        free(tag_data);
        return found_any;
    }

    bool frame_size_synchsafe = (major_version >= 4);
    uint32_t pos = body_start;

    while (pos + 10 <= tag_size) {
        if (tag_data[pos] == '\0') break; /* start of padding */

        char frame_id[5];
        memcpy(frame_id, tag_data + pos, 4);
        frame_id[4] = '\0';
        uint32_t frame_size = read_be32(tag_data + pos + 4, frame_size_synchsafe);
        uint8_t frame_flags2 = tag_data[pos + 9];
        pos += 10;
        /* Bound frame_size against remaining tag size using subtraction to avoid 32-bit overflow. */
        if (frame_size > tag_size - pos) break;
        uint32_t next_pos = pos + frame_size; /* safe now -- frame_size <= tag_size - pos, just proven above; may still be adjusted below */

        /* Interpret per-frame flag bytes for v2.3 and v2.4 (per-frame unsync,
         * grouping identity, data-length indicators, and compression/encryption).
         * Unsupported compressed or encrypted frames are skipped. */
        bool frame_compressed, frame_encrypted, frame_grouped, frame_unsync, frame_has_data_len;
        if (major_version >= 4) {
            frame_compressed = (frame_flags2 & 0x08) != 0;
            frame_encrypted = (frame_flags2 & 0x04) != 0;
            frame_grouped = (frame_flags2 & 0x40) != 0;
            frame_unsync = (frame_flags2 & 0x02) != 0;
            frame_has_data_len = (frame_flags2 & 0x01) != 0;
        } else {
            frame_compressed = (frame_flags2 & 0x80) != 0;
            frame_encrypted = (frame_flags2 & 0x40) != 0;
            frame_grouped = (frame_flags2 & 0x20) != 0;
            frame_unsync = false;
            frame_has_data_len = frame_compressed;
        }

        if (!frame_compressed && !frame_encrypted) {
            uint8_t * frame_data = tag_data + pos;
            uint32_t frame_data_size = frame_size;

            if (frame_grouped && frame_data_size >= 1) {
                frame_data += 1;
                frame_data_size -= 1;
            }
            if (frame_has_data_len && frame_data_size >= 4) {
                frame_data += 4;
                frame_data_size -= 4;
            }

            if (frame_unsync && frame_data_size > 0) {
                uint32_t write_pos = 0;
                for (uint32_t read_pos = 0; read_pos < frame_data_size; read_pos++) {
                    uint8_t b = frame_data[read_pos];
                    frame_data[write_pos++] = b;
                    if (b == 0xFF && read_pos + 1 < frame_data_size && frame_data[read_pos + 1] == 0x00) {
                        read_pos++;
                    }
                }
                frame_data_size = write_pos;
            }

            if (strcmp(frame_id, "TIT2") == 0) {
                decode_id3v2_text_frame(frame_data, frame_data_size, out->title, sizeof(out->title));
                out->has_title = out->title[0] != '\0';
                found_any = true;
            } else if (strcmp(frame_id, "TPE1") == 0) {
                decode_id3v2_text_frame(frame_data, frame_data_size, out->artist, sizeof(out->artist));
                out->has_artist = out->artist[0] != '\0';
                found_any = true;
            } else if (strcmp(frame_id, "TALB") == 0) {
                decode_id3v2_text_frame(frame_data, frame_data_size, out->album, sizeof(out->album));
                out->has_album = out->album[0] != '\0';
                found_any = true;
            } else if (strcmp(frame_id, "TPE2") == 0) {
                decode_id3v2_text_frame(frame_data, frame_data_size, out->album_artist, sizeof(out->album_artist));
                out->has_album_artist = out->album_artist[0] != '\0';
                found_any = true;
            } else if (strcmp(frame_id, "TCON") == 0) {
                decode_id3v2_text_frame(frame_data, frame_data_size, out->genre, sizeof(out->genre));
                out->has_genre = out->genre[0] != '\0';
                found_any = true;
            } else if (strcmp(frame_id, "TRCK") == 0 || strcmp(frame_id, "TPOS") == 0) {
                char sequence[32];
                decode_id3v2_text_frame(frame_data, frame_data_size, sequence, sizeof(sequence));
                apply_sequence_text(out, frame_id[1] == 'P', sequence, strlen(sequence));
                found_any = true;
            } else if (strcmp(frame_id, "APIC") == 0) {
                decode_id3v2_apic_frame(frame_data, frame_data_size, out);
                found_any = true;
            } else if (strcmp(frame_id, "TXXX") == 0) {
                decode_id3v2_txxx_frame(frame_data, frame_data_size, out);
                found_any = true;
            } else if (strcmp(frame_id, "USLT") == 0) {
                decode_id3v2_uslt_frame(frame_data, frame_data_size, out);
                found_any = true;
            }
        }

        pos = next_pos;
    }

    free(tag_data);
    return found_any;
}

static void copy_id3v1_field(char * dst, size_t dst_size, const char * src, size_t src_len) {
    while (src_len > 0 && (src[src_len - 1] == ' ' || src[src_len - 1] == '\0')) src_len--;
    copy_bounded(dst, dst_size, src, src_len);
}

static bool read_id3v1(FILE * f, track_metadata_t * out) {
    if (fseek(f, -128, SEEK_END) != 0) return false;

    uint8_t tag[128];
    if (fread(tag, 1, sizeof(tag), f) != sizeof(tag)) return false;
    if (memcmp(tag, "TAG", 3) != 0) return false;

    copy_id3v1_field(out->title, sizeof(out->title), (const char *) tag + 3, 30);
    copy_id3v1_field(out->artist, sizeof(out->artist), (const char *) tag + 33, 30);
    copy_id3v1_field(out->album, sizeof(out->album), (const char *) tag + 63, 30);
    out->has_title = out->title[0] != '\0';
    out->has_artist = out->artist[0] != '\0';
    out->has_album = out->album[0] != '\0';
    if (tag[125] == 0 && tag[126] != 0) {
        out->track_number = tag[126];
        out->has_track_number = true;
    }
    return true;
}

static void read_mp3_metadata(const char * path, track_metadata_t * out, bool include_blobs) {
    FILE * f = fopen(path, "rb");
    if (!f) return;

    if (!read_id3v2(f, out, include_blobs)) {
        read_id3v1(f, out);
    }

    fclose(f);
}

/* Raw AAC (.aac, ADTS/ADIF) metadata parser. Parses prepended ID3v2 or trailing
 * ID3v1 tags commonly placed around raw ADTS streams. */
static void read_aac_metadata(const char * path, track_metadata_t * out, bool include_blobs) {
    FILE * f = fopen(path, "rb");
    if (!f) return;

    if (!read_id3v2(f, out, include_blobs)) {
        read_id3v1(f, out);
    }

    fclose(f);
}

/* ---- M4A (ALAC or AAC container): moov/udta/meta/ilst iTunes-style atoms ----
 *
 * This is a separate, minimal box walker rather than a reuse of
 * mp4_demux.c's (audio.c's demuxer only tracks the moov/trak/.../stbl path
 * needed to locate compressed audio samples, has no reason to know about
 * udta/meta/ilst, and its helpers are file-local statics anyway). */

typedef struct {
    char type[5];
    uint64_t size;
    long header_size;
    long data_start;
} m4a_box_t;

static uint32_t m4a_read_u32be(const uint8_t * b) {
    return ((uint32_t) b[0] << 24) | ((uint32_t) b[1] << 16) | ((uint32_t) b[2] << 8) | (uint32_t) b[3];
}

static bool m4a_read_box_header(FILE * f, m4a_box_t * out) {
    long start = ftell(f);
    if (start < 0) return false;
    uint8_t hdr[8];
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) return false;

    uint32_t size32 = m4a_read_u32be(hdr);
    memcpy(out->type, hdr + 4, 4);
    out->type[4] = '\0';

    if (size32 == 1) {
        uint8_t ext[8];
        if (fread(ext, 1, sizeof(ext), f) != sizeof(ext)) return false;
        uint64_t size64 = 0;
        for (int i = 0; i < 8; i++) size64 = (size64 << 8) | ext[i];
        out->size = size64;
        out->header_size = 16;
    } else if (size32 == 0) {
        long cur = ftell(f);
        if (cur < 0 || fseek(f, 0, SEEK_END) != 0) return false;
        long end = ftell(f);
        if (end < start || fseek(f, cur, SEEK_SET) != 0) return false;
        out->size = (uint64_t) (end - start);
        out->header_size = 8;
    } else {
        out->size = size32;
        out->header_size = 8;
    }

    /* Reject malformed boxes smaller than their own header. */
    if (out->size < (uint64_t) out->header_size) return false;

    /* FILE/fseek use a 32-bit long on the target. Validate the still-
     * unsigned box size before either casting or adding it, otherwise a
     * corrupt extended-size atom can wrap the next seek backwards and keep
     * the scanner walking the same bytes indefinitely. */
    long box_end;
    if (!safe_chunk_advance(start, out->size, 0, &box_end)) return false;

    out->data_start = start + out->header_size;
    return true;
}

static bool m4a_box_end_within(const m4a_box_t * box, long container_end, long * out_end) {
    if (!box || box->data_start < box->header_size || container_end < 0) return false;
    long box_start = box->data_start - box->header_size;
    long box_end;
    if (!safe_chunk_advance(box_start, box->size, 0, &box_end)) return false;
    if (box_end < box->data_start || box_end > container_end) return false;
    if (out_end) *out_end = box_end;
    return true;
}

static bool m4a_find_child(FILE * f, long container_start, uint64_t container_size, const char * type, m4a_box_t * out) {
    long end;
    if (!safe_chunk_advance(container_start, container_size, 0, &end) ||
        fseek(f, container_start, SEEK_SET) != 0)
        return false;

    for (;;) {
        long pos = ftell(f);
        if (pos < 0 || pos >= end) break;
        m4a_box_t box;
        if (!m4a_read_box_header(f, &box)) return false;
        long next;
        if (!m4a_box_end_within(&box, end, &next)) return false;
        if (strcmp(box.type, type) == 0) {
            *out = box;
            return true;
        }
        if (fseek(f, next, SEEK_SET) != 0) return false;
    }
    return false;
}

/* Every ilst item (©nam/©ART/©alb/covr/...) wraps its actual value in a
 * nested "data" box: a FullBox-shaped header (type indicator(4) + locale(4)
 * -- always seen as zeroed/type 1 or 13/14 in practice, not otherwise
 * inspected here) followed immediately by the raw value bytes. */
static bool m4a_find_data_payload(FILE * f, m4a_box_t parent, long * out_offset, uint32_t * out_size) {
    if (parent.size < (uint64_t) parent.header_size) return false;
    m4a_box_t data_box;
    if (!m4a_find_child(f, parent.data_start, parent.size - (uint64_t) parent.header_size, "data", &data_box)) return false;
    if (data_box.size < (uint64_t) data_box.header_size + 8) return false;
    uint64_t payload_size = data_box.size - (uint64_t) data_box.header_size - 8;
    if (payload_size > UINT32_MAX || data_box.data_start > LONG_MAX - 8) return false;
    *out_offset = data_box.data_start + 8;
    *out_size = (uint32_t) payload_size;
    return true;
}

static void m4a_read_text_tag(FILE * f, m4a_box_t item, char * dst, size_t dst_size, bool * has_flag) {
    long payload_offset;
    uint32_t payload_size;
    if (!m4a_find_data_payload(f, item, &payload_offset, &payload_size) || payload_size == 0) return;

    uint32_t n = (payload_size > dst_size - 1) ? (uint32_t) (dst_size - 1) : payload_size;
    fseek(f, payload_offset, SEEK_SET);
    if (fread(dst, 1, n, f) != n) return;
    dst[n] = '\0';
    *has_flag = true;
}

static void m4a_read_sequence_tag(FILE * f, m4a_box_t item, bool disc, track_metadata_t * out) {
    long payload_offset;
    uint32_t payload_size;
    if (!m4a_find_data_payload(f, item, &payload_offset, &payload_size) || payload_size < 4) return;
    uint8_t data[8] = {0};
    uint32_t n = payload_size < sizeof(data) ? payload_size : (uint32_t) sizeof(data);
    if (fseek(f, payload_offset, SEEK_SET) != 0 || fread(data, 1, n, f) != n) return;
    /* iTunes trkn/disk payload: two reserved bytes followed by the
     * big-endian current track/disc number (then total count). */
    int value = ((int) data[2] << 8) | data[3];
    if (value <= 0) return;
    if (disc) {
        out->disc_number = value;
        out->has_disc_number = true;
    } else {
        out->track_number = value;
        out->has_track_number = true;
    }
}

static void m4a_read_cover_art(FILE * f, m4a_box_t item, track_metadata_t * out) {
    if (out->picture_data != NULL) return; /* keep the first picture found, same as FLAC/MP3 */

    long payload_offset;
    uint32_t payload_size;
    if (!m4a_find_data_payload(f, item, &payload_offset, &payload_size) || payload_size == 0) return;
    /* Audiobook/M4B covers are often multi-megabyte JPEGs. Unbounded
     * malloc here races the decoder's own RAM on the 56 MiB target. */
    if (payload_size > EMBEDDED_COVER_MAX_BYTES) {
        out->picture_too_large = true;
        return;
    }

    uint8_t * data = malloc(payload_size);
    if (!data) return;
    fseek(f, payload_offset, SEEK_SET);
    if (fread(data, 1, payload_size, f) == payload_size) {
        out->picture_data = data;
        out->picture_size = payload_size;
    } else {
        free(data);
    }
}

/* "\xA9lyr" atom -- same nested "data" box shape as every other ilst item,
 * plain UTF-8 text (no encoding byte, unlike ID3v2's USLT) -- same malloc'd-
 * buffer-plus-NUL-terminator shape as m4a_read_cover_art() just above, text
 * instead of binary. May or may not contain [mm:ss.xx] LRC timestamps -- see
 * track_metadata_t's own lyrics field comment. */
static void m4a_read_lyrics_tag(FILE * f, m4a_box_t item, track_metadata_t * out) {
    if (out->lyrics != NULL) return; /* keep the first found, same as cover art */

    long payload_offset;
    uint32_t payload_size;
    if (!m4a_find_data_payload(f, item, &payload_offset, &payload_size) || payload_size == 0) return;
    if (payload_size > METADATA_BLOB_MAX_BYTES) return;

    char * data = malloc((size_t) payload_size + 1);
    if (!data) return;
    fseek(f, payload_offset, SEEK_SET);
    if (fread(data, 1, payload_size, f) == payload_size) {
        data[payload_size] = '\0';
        out->lyrics = data;
    } else {
        free(data);
    }
}

static void read_m4a_metadata(const char * path, track_metadata_t * out, bool include_blobs) {
    FILE * f = fopen(path, "rb");
    if (!f) return;

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return; }
    long file_size = ftell(f);
    if (file_size < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return; }

    m4a_box_t moov;
    bool found_moov = false;
    for (;;) {
        long pos = ftell(f);
        if (pos < 0 || pos >= file_size) break;
        m4a_box_t box;
        if (!m4a_read_box_header(f, &box)) break;
        long next;
        if (!m4a_box_end_within(&box, file_size, &next)) break;
        if (strcmp(box.type, "moov") == 0) { moov = box; found_moov = true; break; }
        if (fseek(f, next, SEEK_SET) != 0) break;
    }
    if (!found_moov) { fclose(f); return; }

    m4a_box_t udta, meta, ilst;
    if (!m4a_find_child(f, moov.data_start, moov.size - (uint64_t) moov.header_size, "udta", &udta)) { fclose(f); return; }
    if (!m4a_find_child(f, udta.data_start, udta.size - (uint64_t) udta.header_size, "meta", &meta)) { fclose(f); return; }

    /* Unlike every other container walked here, top-level "meta" is itself a
     * FullBox -- its children start 4 bytes (version+flags) further in
     * than a plain container's would. */
    if (meta.size < (uint64_t) meta.header_size + 4 || meta.data_start > LONG_MAX - 4) {
        fclose(f);
        return;
    }
    long meta_children_start = meta.data_start + 4;
    uint64_t meta_children_size = meta.size - (uint64_t) meta.header_size - 4;
    if (!m4a_find_child(f, meta_children_start, meta_children_size, "ilst", &ilst)) { fclose(f); return; }

    long ilst_end;
    if (ilst.size < (uint64_t) ilst.header_size ||
        !safe_chunk_advance(ilst.data_start, ilst.size - (uint64_t) ilst.header_size, 0, &ilst_end) ||
        fseek(f, ilst.data_start, SEEK_SET) != 0) {
        fclose(f);
        return;
    }
    for (;;) {
        long pos = ftell(f);
        if (pos < 0 || pos >= ilst_end) break;
        m4a_box_t item;
        if (!m4a_read_box_header(f, &item)) break;
        long next;
        if (!m4a_box_end_within(&item, ilst_end, &next)) break;

        if (strcmp(item.type, "\xA9" "nam") == 0) {
            m4a_read_text_tag(f, item, out->title, sizeof(out->title), &out->has_title);
        } else if (strcmp(item.type, "\xA9" "ART") == 0) {
            m4a_read_text_tag(f, item, out->artist, sizeof(out->artist), &out->has_artist);
        } else if (strcmp(item.type, "\xA9" "alb") == 0) {
            m4a_read_text_tag(f, item, out->album, sizeof(out->album), &out->has_album);
        } else if (strcmp(item.type, "aART") == 0) {
            m4a_read_text_tag(f, item, out->album_artist, sizeof(out->album_artist), &out->has_album_artist);
        } else if (strcmp(item.type, "\xA9" "gen") == 0) {
            m4a_read_text_tag(f, item, out->genre, sizeof(out->genre), &out->has_genre);
        } else if (strcmp(item.type, "trkn") == 0) {
            m4a_read_sequence_tag(f, item, false, out);
        } else if (strcmp(item.type, "disk") == 0) {
            m4a_read_sequence_tag(f, item, true, out);
        } else if (include_blobs && !lyrics_only && strcmp(item.type, "covr") == 0) {
            m4a_read_cover_art(f, item, out);
        } else if (include_blobs && !artwork_only && strcmp(item.type, "\xA9" "lyr") == 0) {
            m4a_read_lyrics_tag(f, item, out);
        }

        if (fseek(f, next, SEEK_SET) != 0) break;
    }

    fclose(f);
}

/* ---- Opus: OpusTags (Ogg comment header, RFC 7845 5.2) ---- */

static void read_opus_metadata(const char * path, track_metadata_t * out, bool include_blobs) {
    ogg_demux_t * demux = include_blobs ? ogg_demux_open(path) : ogg_demux_open_metadata(path, true);
    if (!demux) return;

    unsigned int count = ogg_demux_get_comment_count(demux);
    for (unsigned int i = 0; i < count; i++) {
        uint32_t comment_len;
        const char * comment = ogg_demux_get_comment(demux, i, &comment_len);
        if (!comment) continue;

        /* METADATA_BLOCK_PICTURE isn't a plain KEY=VALUE text field --
         * apply_vorbis_comment_field() would just fail its '='-split match
         * on the base64 payload harmlessly, but handling it explicitly
         * here (rather than teaching that shared helper about base64/
         * binary decoding) keeps it a pure text-field matcher. */
        static const char picture_key[] = "METADATA_BLOCK_PICTURE=";
        size_t picture_key_len = sizeof(picture_key) - 1;
        if (include_blobs && !lyrics_only && comment_len > picture_key_len && strncasecmp(comment, picture_key, picture_key_len) == 0) {
            const char * b64 = comment + picture_key_len;
            size_t b64_len = comment_len - picture_key_len;

            size_t decoded_len = 0;
            int size_err = mbedtls_base64_decode(NULL, 0, &decoded_len, (const unsigned char *) b64, b64_len);
            if ((size_err == 0 || size_err == MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) &&
                decoded_len > METADATA_BLOB_MAX_BYTES)
                out->picture_too_large = true;
            if ((size_err == 0 || size_err == MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) && decoded_len > 0 &&
                decoded_len <= METADATA_BLOB_MAX_BYTES) {
                uint8_t * decoded = malloc(decoded_len);
                if (decoded) {
                    size_t actual_len = 0;
                    if (mbedtls_base64_decode(decoded, decoded_len, &actual_len, (const unsigned char *) b64, b64_len) == 0) {
                        parse_flac_picture_block(decoded, actual_len, out);
                    }
                    free(decoded);
                }
            }
            continue;
        }

        apply_vorbis_comment_field(out, comment, comment_len, include_blobs);
    }

    ogg_demux_close(demux);
}

/* ---- Ogg Vorbis (.ogg): comment header, same RFC-ish "vendor string +
 * KEY=VALUE list" layout as Opus's own OpusTags above (Vorbis comments are
 * in fact where that layout originated -- RFC 7845 5.2 explicitly reuses
 * the Vorbis I spec's own comment format). Reuses vorbis_decoder.h's stb_
 * vorbis wrapper's underlying library directly (stb_vorbis_get_comment())
 * rather than ogg_demux.h, which is hardcoded to Opus's own OpusHead/
 * OpusTags packet names (see its own header comment) -- stb_vorbis already
 * does its own complete Ogg demux internally, so there's no reason to
 * duplicate that here. Heavier than ogg_demux's lightweight header-only
 * scan (this opens a full decoder, not just a comment reader), same
 * pragmatic tradeoff class as other formats in this file that reuse a full
 * decoder-open for a metadata-only read. */
static void read_ogg_vorbis_metadata(const char * path, track_metadata_t * out, bool include_blobs) {
    int error = 0;
    stb_vorbis * f = stb_vorbis_open_filename(path, &error, NULL);
    if (!f) return;

    stb_vorbis_comment comment = stb_vorbis_get_comment(f);
    for (int i = 0; i < comment.comment_list_length; i++) {
        const char * field = comment.comment_list[i];
        if (!field) continue;
        size_t field_len = strlen(field);

        if (!include_blobs &&
            ((field_len >= 7 && strncasecmp(field, "LYRICS=", 7) == 0) ||
             (field_len >= 15 && strncasecmp(field, "UNSYNCEDLYRICS=", 15) == 0) ||
             (field_len >= 23 && strncasecmp(field, "METADATA_BLOCK_PICTURE=", 23) == 0)))
            continue;

        /* Same METADATA_BLOCK_PICTURE special case as read_opus_metadata()
         * above -- see its own comment for why this stays out of
         * apply_vorbis_comment_field() itself. */
        static const char picture_key[] = "METADATA_BLOCK_PICTURE=";
        size_t picture_key_len = sizeof(picture_key) - 1;
        if (include_blobs && !lyrics_only && field_len > picture_key_len && strncasecmp(field, picture_key, picture_key_len) == 0) {
            const char * b64 = field + picture_key_len;
            size_t b64_len = field_len - picture_key_len;

            size_t decoded_len = 0;
            int size_err = mbedtls_base64_decode(NULL, 0, &decoded_len, (const unsigned char *) b64, b64_len);
            if ((size_err == 0 || size_err == MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) &&
                decoded_len > METADATA_BLOB_MAX_BYTES)
                out->picture_too_large = true;
            if ((size_err == 0 || size_err == MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) && decoded_len > 0 &&
                decoded_len <= METADATA_BLOB_MAX_BYTES) {
                uint8_t * decoded = malloc(decoded_len);
                if (decoded) {
                    size_t actual_len = 0;
                    if (mbedtls_base64_decode(decoded, decoded_len, &actual_len, (const unsigned char *) b64, b64_len) == 0) {
                        parse_flac_picture_block(decoded, actual_len, out);
                    }
                    free(decoded);
                }
            }
            continue;
        }

        apply_vorbis_comment_field(out, field, field_len, include_blobs);
    }

    stb_vorbis_close(f);
}

/* AIFF/AIFC metadata parser. Reads embedded "ID3 " IFF chunks, which are often
 * positioned after the SSND chunk. */
static void read_aiff_metadata(const char * path, track_metadata_t * out, bool include_blobs) {
    FILE * f = fopen(path, "rb");
    if (!f) return;

    uint8_t header[12];
    if (fread(header, 1, sizeof(header), f) != sizeof(header) || memcmp(header, "FORM", 4) != 0 ||
        (memcmp(header + 8, "AIFF", 4) != 0 && memcmp(header + 8, "AIFC", 4) != 0)) {
        fclose(f);
        return;
    }

    while (1) {
        long chunk_start = ftell(f); /* before this iteration's own header read -- see safe_chunk_advance()'s own comment */
        uint8_t chunk_header[8];
        if (fread(chunk_header, 1, sizeof(chunk_header), f) != sizeof(chunk_header)) break;

        char chunk_id[5] = { 0 };
        memcpy(chunk_id, chunk_header, 4);
        uint32_t chunk_size = read_be32(chunk_header + 4, false);
        long chunk_data_start = ftell(f);

        if (strcmp(chunk_id, "ID3 ") == 0) {
            read_id3v2(f, out, include_blobs);
            break;
        }

        /* Chunks are 2-byte aligned; ensure forward progress to prevent infinite loops on corrupted files. */
        long next;
        if (!safe_chunk_advance(chunk_data_start, chunk_size, (long) (chunk_size & 1), &next) ||
            next <= chunk_start || fseek(f, next, SEEK_SET) != 0) break;
    }

    fclose(f);
}

/* WAV ID3 fallback parser. Scans for embedded "id3 " / "ID3 " RIFF chunks
 * (often placed after audio data) when standard RIFF LIST/INFO chunks are absent. */
static void read_wav_id3_fallback(const char * path, track_metadata_t * out, bool include_blobs) {
    FILE * f = fopen(path, "rb");
    if (!f) return;

    uint8_t header[12];
    if (fread(header, 1, sizeof(header), f) != sizeof(header) || memcmp(header, "RIFF", 4) != 0 ||
        memcmp(header + 8, "WAVE", 4) != 0) {
        fclose(f);
        return;
    }

    while (1) {
        long chunk_start = ftell(f); /* before this iteration's own header read -- see the forward-progress comment below */
        uint8_t chunk_header[8];
        if (fread(chunk_header, 1, sizeof(chunk_header), f) != sizeof(chunk_header)) break;

        char chunk_id[5] = { 0 };
        memcpy(chunk_id, chunk_header, 4);
        uint32_t chunk_size = (uint32_t) chunk_header[4] | ((uint32_t) chunk_header[5] << 8) |
                               ((uint32_t) chunk_header[6] << 16) | ((uint32_t) chunk_header[7] << 24);
        long chunk_data_start = ftell(f);

        if (strcasecmp(chunk_id, "id3 ") == 0) {
            read_id3v2(f, out, include_blobs);
            break;
        }

        /* Chunks are 2-byte aligned; validate size and ensure forward progress to prevent infinite loops. */
        long next;
        if (!safe_chunk_advance(chunk_data_start, chunk_size, (long) (chunk_size & 1), &next) ||
            next <= chunk_start || fseek(f, next, SEEK_SET) != 0) break;
    }

    fclose(f);
}

static uint64_t read_u64le(const uint8_t * b) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | b[i];
    return v;
}

/* DSDIFF (.dff) is a big-endian IFF-derived format, unlike the little-endian
 * DSF/ASF readers above -- its chunk sizes must be read big-endian, matching
 * dsd_decoder.c's own audio_read_u64be() for the same FRM8/chunk headers. */
static uint64_t read_u64be(const uint8_t * b) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = (v << 8) | b[i];
    return v;
}

/* DSF metadata parser. Reads the ID3v2 metadataPointer offset from the "DSD "
 * master chunk and parses the ID3v2 tag. */
static void read_dsf_metadata(const char * path, track_metadata_t * out, bool include_blobs) {
    FILE * f = fopen(path, "rb");
    if (!f) return;

    uint8_t dsd_chunk[28];
    if (fread(dsd_chunk, 1, sizeof(dsd_chunk), f) != sizeof(dsd_chunk) || memcmp(dsd_chunk, "DSD ", 4) != 0) {
        fclose(f);
        return;
    }

    uint64_t metadata_pointer = read_u64le(&dsd_chunk[20]);
    if (metadata_pointer != 0 && fseek(f, (long) metadata_pointer, SEEK_SET) == 0) {
        read_id3v2(f, out, include_blobs);
    }

    fclose(f);
}

/* Same key -> field matching apply_vorbis_comment_field() does, factored
 * out since both APEv2 items and Vorbis comments are plain case-
 * insensitive "Title"/"Artist"/"Album" text fields -- only how each
 * format's key/value pair is framed on disk differs. */
static void apply_title_artist_album_field(track_metadata_t * out, const char * key, size_t key_len,
                                            const char * value, size_t value_len) {
    if (key_len == 5 && strncasecmp(key, "Title", 5) == 0) {
        copy_bounded(out->title, sizeof(out->title), value, value_len);
        out->has_title = true;
    } else if (key_len == 6 && strncasecmp(key, "Artist", 6) == 0) {
        copy_bounded(out->artist, sizeof(out->artist), value, value_len);
        out->has_artist = true;
    } else if (key_len == 5 && strncasecmp(key, "Album", 5) == 0) {
        copy_bounded(out->album, sizeof(out->album), value, value_len);
        out->has_album = true;
    } else if ((key_len == 5 && strncasecmp(key, "Track", 5) == 0) ||
               (key_len == 11 && strncasecmp(key, "TrackNumber", 11) == 0)) {
        apply_sequence_text(out, false, value, value_len);
    } else if ((key_len == 4 && strncasecmp(key, "Disc", 4) == 0) ||
               (key_len == 10 && strncasecmp(key, "DiscNumber", 10) == 0)) {
        apply_sequence_text(out, true, value, value_len);
    }
}

/* ---- APE (Monkey's Audio): APEv2 footer tag -- no existing helper to
 * reuse, ape_demux.c only ever parses the file-start descriptor/header for
 * decoding, never anything at the file's end. Spec layout: a 32-byte
 * footer (magic "APETAGEX" + version u32LE + tag_size u32LE, which counts
 * the items plus this footer itself but never the file's actual end if a
 * legacy trailing ID3v1 tag also follows + item_count u32LE + flags u32LE
 * + 8 reserved bytes), preceded by item_count items of
 * { value_length u32LE, item_flags u32LE, null-terminated key, raw value }.
 * Only text items (item_flags bits 1-2 == 0) are read as UTF-8 text. */
static void read_ape_metadata(const char * path, track_metadata_t * out) {
    FILE * f = fopen(path, "rb");
    if (!f) return;

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return;
    }
    long file_end = ftell(f);
    if (file_end < 32) {
        fclose(f);
        return;
    }

    /* Some files carry a legacy 128-byte ID3v1 tag appended AFTER the
     * APEv2 tag -- skip past it if present so the footer search below
     * looks at the actual end of the APEv2 tag, not the ID3v1 trailer. */
    long effective_end = file_end;
    if (file_end >= 128) {
        uint8_t id3v1_magic[3];
        if (fseek(f, file_end - 128, SEEK_SET) == 0 && fread(id3v1_magic, 1, 3, f) == 3 &&
            memcmp(id3v1_magic, "TAG", 3) == 0) {
            effective_end = file_end - 128;
        }
    }
    if (effective_end < 32) {
        fclose(f);
        return;
    }

    uint8_t footer[32];
    if (fseek(f, effective_end - 32, SEEK_SET) != 0 || fread(footer, 1, sizeof(footer), f) != sizeof(footer) ||
        memcmp(footer, "APETAGEX", 8) != 0) {
        fclose(f);
        return;
    }

    uint32_t tag_size = library_read_u32le(&footer[12]);
    uint32_t item_count = library_read_u32le(&footer[16]);
    if (tag_size < 32 || (long) tag_size > effective_end) {
        fclose(f);
        return;
    }

    long items_start = effective_end - (long) tag_size;
    if (fseek(f, items_start, SEEK_SET) != 0) {
        fclose(f);
        return;
    }

    for (uint32_t i = 0; i < item_count; i++) {
        uint8_t item_header[8];
        if (fread(item_header, 1, sizeof(item_header), f) != sizeof(item_header)) break;
        uint32_t value_length = library_read_u32le(&item_header[0]);
        uint32_t item_flags = library_read_u32le(&item_header[4]);

        char key[64];
        size_t key_len = 0;
        int c;
        while (key_len + 1 < sizeof(key) && (c = fgetc(f)) != EOF && c != '\0') key[key_len++] = (char) c;
        key[key_len] = '\0';
        if (c == EOF) break;
        /* A key longer than this buffer (no real-world tag uses one) still
         * needs its remaining bytes drained before the value, or every
         * subsequent item in the tag would be misaligned. */
        while (c != '\0') {
            c = fgetc(f);
            if (c == EOF) break;
        }

        if (value_length > 0 && value_length < (1u << 20)) { /* defensive cap -- no real tag value is anywhere near 1MB */
            char * value = malloc(value_length);
            if (value && fread(value, 1, value_length, f) == value_length) {
                if ((item_flags & 0x6) == 0) { /* bits 1-2 of item_flags: 0 = UTF-8 text */
                    apply_title_artist_album_field(out, key, key_len, value, value_length);
                    /* APEv2 stores ReplayGain keys using the same naming as Vorbis comments. */
                    apply_replaygain_field(out, key, key_len, value, value_length);
                }
            }
            free(value);
        } else if (fseek(f, (long) value_length, SEEK_CUR) != 0) {
            break;
        }
    }

    fclose(f);
}

/* ---- DFF (DSDIFF): real-world files use two different, incompatible
 * tagging conventions depending on which tool wrote them -- many modern
 * taggers (dBpoweramp, foobar2000, ...) embed a non-standard top-level
 * "ID3 " chunk, exactly AIFF's own convention above, even though it isn't
 * part of the official Philips DSDIFF spec; the spec's own native
 * mechanism is a "DIIN" chunk containing "DIAR" (artist)/"DITI" (title)
 * sub-chunks (plain strings, no ID3-style text-encoding byte). Tries the
 * ID3 chunk first (richer -- covers title/artist/album in one read_id3v2()
 * call), falling back to DIIN/DIAR/DITI only if no "ID3 " chunk was found
 * anywhere in the file. Independent chunk walk from dsd_decoder.c's own
 * open_dff() (big-endian 64-bit chunk sizes, unlike AIFF's 32-bit) --
 * that one stops once channels/sample_rate/data are all found, which
 * happens before any trailing "ID3 "/"DIIN" chunk would be reached. */
static bool dff_read_chunk_header(FILE * f, char id_out[5], uint64_t * size_out) {
    uint8_t header[12];
    if (fread(header, 1, sizeof(header), f) != sizeof(header)) return false;
    memcpy(id_out, header, 4);
    id_out[4] = '\0';
    *size_out = read_u64be(header + 4); /* DSDIFF chunk sizes are big-endian */
    return true;
}

static void read_dff_metadata(const char * path, track_metadata_t * out, bool include_blobs) {
    FILE * f = fopen(path, "rb");
    if (!f) return;

    char frm8_id[5];
    uint64_t frm8_size;
    uint8_t form_type[4];
    if (!dff_read_chunk_header(f, frm8_id, &frm8_size) || strcmp(frm8_id, "FRM8") != 0 ||
        fread(form_type, 1, sizeof(form_type), f) != sizeof(form_type) || memcmp(form_type, "DSD ", 4) != 0) {
        fclose(f);
        return;
    }

    long diin_data_start = -1;
    long diin_end = -1;

    while (1) {
        char id[5];
        uint64_t size;
        long chunk_start = ftell(f); /* position BEFORE this iteration's own header read -- next must clear this, or a corrupted size could seek back here and loop forever */
        long chunk_data_start = ftell(f);
        if (!dff_read_chunk_header(f, id, &size)) break;
        chunk_data_start = ftell(f);

        if (strcmp(id, "ID3 ") == 0) {
            read_id3v2(f, out, include_blobs);
            fclose(f);
            return;
        }
        if (strcmp(id, "DIIN") == 0 && diin_data_start < 0) {
            /* Safely compute diin_end without overflow. */
            long candidate_end;
            if (safe_chunk_advance(chunk_data_start, size, 0, &candidate_end)) {
                diin_data_start = chunk_data_start;
                diin_end = candidate_end;
            }
        }

        /* Chunks are 2-byte aligned; validate size and ensure forward progress to prevent infinite loops. */
        long next;
        if (!safe_chunk_advance(chunk_data_start, size, (long) (size & 1), &next) ||
            next <= chunk_start || fseek(f, next, SEEK_SET) != 0) break;
    }

    /* No "ID3 " chunk anywhere -- fall back to the spec-native DIIN
     * sub-chunks, if a DIIN chunk was seen during the walk above. */
    if (diin_data_start >= 0 && fseek(f, diin_data_start, SEEK_SET) == 0) {
        while (ftell(f) < diin_end) {
            char sub_id[5];
            uint64_t sub_size;
            long sub_chunk_start = ftell(f); /* see the outer chunk walk's own forward-progress comment above */
            long sub_data_start = ftell(f);
            if (!dff_read_chunk_header(f, sub_id, &sub_size)) break;
            sub_data_start = ftell(f);

            if ((strcmp(sub_id, "DIAR") == 0 || strcmp(sub_id, "DITI") == 0) && sub_size > 0 &&
                sub_size < sizeof(out->title)) {
                char text[128];
                if (fread(text, 1, (size_t) sub_size, f) == sub_size) {
                    if (strcmp(sub_id, "DIAR") == 0) {
                        copy_bounded(out->artist, sizeof(out->artist), text, (size_t) sub_size);
                        out->has_artist = true;
                    } else {
                        copy_bounded(out->title, sizeof(out->title), text, (size_t) sub_size);
                        out->has_title = true;
                    }
                }
            }

            /* Validate subchunk size and ensure forward progress. */
            long next;
            if (!safe_chunk_advance(sub_data_start, sub_size, (long) (sub_size & 1), &next) ||
                next <= sub_chunk_start || fseek(f, next, SEEK_SET) != 0) break;
        }
    }

    fclose(f);
}

/* ---- WMA: ASF Content Description + Extended Content Description
 * objects. asf_demux.c already has exactly this shape for decoding
 * (GUID_HEADER et al., asf_object_header_t, read_object_header(), and
 * asf_demux_open()'s own top-level object walk) but everything there is
 * static to that file, so this is an independent walk in the same shape --
 * same reasoning as DFF/AIFF above. GUIDs are stored in ASF on-disk byte
 * order: the first three fields (uint32, uint16, uint16) little-endian, the
 * final 8 bytes as-is -- the same layout asf_demux.c uses for GUID_HEADER et
 * al. and what fread() delivers here. (Earlier these two content GUIDs were
 * written in textual/big-endian order, so they never matched an ASF file and
 * no WMA tag was ever read, even though GUID_ASF_HEADER below happened to be
 * correct; verified against asf_demux.c's own little-endian constants.) */
static const uint8_t GUID_CONTENT_DESCRIPTION[16] = {
    /* {75B22633-668E-11CF-A6D9-00AA0062CE6C} */
    0x33, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11, 0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C
};
static const uint8_t GUID_EXT_CONTENT_DESCRIPTION[16] = {
    /* {D2D0A440-E307-11D2-97F0-00A0C95EA850} */
    0x40, 0xA4, 0xD0, 0xD2, 0x07, 0xE3, 0xD2, 0x11, 0x97, 0xF0, 0x00, 0xA0, 0xC9, 0x5E, 0xA8, 0x50
};
static const uint8_t GUID_ASF_HEADER[16] = {
    /* {75B22630-668E-11CF-A6D9-00AA0062CE6C} */
    0x30, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11, 0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C
};

/* Reuses decode_id3v2_text_frame() for the UTF-16LE -> UTF-8 conversion
 * rather than a second implementation: encoding byte 0x01 ("UTF-16 with
 * optional BOM") already treats absent-BOM input as little-endian by
 * default -- its big_endian flag only flips true on an actual 0xFEFF BOM
 * -- which is exactly ASF's raw no-BOM UTF-16LE convention. Caps at a
 * small stack buffer; no real WMA tag field is anywhere near this long. */
static void decode_asf_utf16le(FILE * f, uint16_t byte_len, char * out, size_t out_size) {
    uint8_t buf[257];
    buf[0] = 0x01;
    if (byte_len > sizeof(buf) - 1) byte_len = sizeof(buf) - 1;
    if (fread(buf + 1, 1, byte_len, f) != byte_len) {
        out[0] = '\0';
        return;
    }
    decode_id3v2_text_frame(buf, (uint32_t) byte_len + 1, out, out_size);
}

static void read_wma_metadata(const char * path, track_metadata_t * out) {
    FILE * f = fopen(path, "rb");
    if (!f) return;

    uint8_t top_guid[16];
    uint8_t size_buf[8];
    if (fread(top_guid, 1, 16, f) != 16 || memcmp(top_guid, GUID_ASF_HEADER, 16) != 0 ||
        fread(size_buf, 1, 8, f) != 8) {
        fclose(f);
        return;
    }

    uint8_t count_buf[6]; /* num_objects u32LE + 2 reserved bytes */
    if (fread(count_buf, 1, sizeof(count_buf), f) != sizeof(count_buf)) {
        fclose(f);
        return;
    }
    uint32_t num_objects = library_read_u32le(count_buf);

    long pos = ftell(f);
    for (uint32_t i = 0; i < num_objects; i++) {
        if (fseek(f, pos, SEEK_SET) != 0) break;

        uint8_t sub_guid[16];
        uint8_t sub_size_buf[8];
        if (fread(sub_guid, 1, 16, f) != 16 || fread(sub_size_buf, 1, 8, f) != 8) break;
        uint64_t sub_size = read_u64le(sub_size_buf);
        long data_start = ftell(f);
        /* Ensure sub_size covers the 24-byte header before subtracting,
         * avoiding underflow and safely advancing to the next object. */
        if (sub_size < 24) break;
        long next;
        if (!safe_chunk_advance(data_start, sub_size - 24, 0, &next)) break;

        if (memcmp(sub_guid, GUID_CONTENT_DESCRIPTION, 16) == 0) {
            uint8_t lens[10]; /* 5 x u16LE lengths: title, author, copyright, description, rating */
            if (fread(lens, 1, sizeof(lens), f) == sizeof(lens)) {
                uint16_t title_len = library_read_u16le(&lens[0]);
                uint16_t author_len = library_read_u16le(&lens[2]);
                if (title_len > 0) {
                    decode_asf_utf16le(f, title_len, out->title, sizeof(out->title));
                    out->has_title = out->title[0] != '\0';
                }
                /* Absolute reposition (not a relative skip) since
                 * decode_asf_utf16le() only ever consumes up to its own
                 * internal cap even if title_len is longer -- this is
                 * correct either way. */
                if (fseek(f, data_start + 10 + title_len, SEEK_SET) == 0 && author_len > 0) {
                    decode_asf_utf16le(f, author_len, out->artist, sizeof(out->artist));
                    out->has_artist = out->artist[0] != '\0';
                }
            }
        } else if (memcmp(sub_guid, GUID_EXT_CONTENT_DESCRIPTION, 16) == 0) {
            uint8_t count_field[2];
            if (fread(count_field, 1, sizeof(count_field), f) == sizeof(count_field)) {
                uint16_t desc_count = library_read_u16le(count_field);
                for (uint16_t d = 0; d < desc_count; d++) {
                    uint8_t name_len_buf[2];
                    if (fread(name_len_buf, 1, 2, f) != 2) break;
                    uint16_t name_len = library_read_u16le(name_len_buf);
                    char name[64];
                    decode_asf_utf16le(f, name_len, name, sizeof(name));
                    /* decode_asf_utf16le() already consumed exactly
                     * min(name_len, sizeof(buf)-1) bytes -- account for
                     * a name longer than that internal cap so the value
                     * fields below don't get misaligned. */
                    if (name_len > 256) fseek(f, (long) name_len - 256, SEEK_CUR);

                    uint8_t type_len_buf[4];
                    if (fread(type_len_buf, 1, 4, f) != 4) break;
                    uint16_t value_type = library_read_u16le(&type_len_buf[0]);
                    uint16_t value_len = library_read_u16le(&type_len_buf[2]);

                    if (value_type == 0 && value_len > 0 && strcasecmp(name, "WM/AlbumTitle") == 0) {
                        decode_asf_utf16le(f, value_len, out->album, sizeof(out->album));
                        out->has_album = out->album[0] != '\0';
                        if (value_len > 256) fseek(f, (long) value_len - 256, SEEK_CUR);
                    } else if (fseek(f, value_len, SEEK_CUR) != 0) {
                        break;
                    }
                }
            }
        }

        if (next <= pos) break; /* forward-progress guard -- see `next`'s own comment above */
        pos = next;
    }

    fclose(f);
}

static void read_wav_all(const char * path, track_metadata_t * out, bool include_blobs) {
    read_wav_metadata(path, out);
    if (!out->has_title) {
        read_wav_id3_fallback(path, out, include_blobs); /* see its own comment */
    } else if (include_blobs) {
        /* LIST/INFO carries only text: a cover or lyrics can live solely in
         * the "id3 " chunk. Take just those blobs from it so the LIST/INFO
         * text tags stay authoritative. */
        track_metadata_t id3;
        memset(&id3, 0, sizeof(id3));
        read_wav_id3_fallback(path, &id3, true);
        out->picture_data = id3.picture_data;
        out->picture_size = id3.picture_size;
        out->picture_too_large = id3.picture_too_large;
        out->lyrics = id3.lyrics;
    }
}

/* One display line per tag: control characters (line breaks from multi-line
 * tags included, CRLF as one) become spaces, so a label never grows into the
 * row below it. A tag with no visible characters counts as absent, so callers
 * apply the same fallback as for a missing tag. Other spacing is kept: it is
 * part of the value the library groups by. */
static void normalize_tag_text(char * text, bool * present) {
    if (!*present) return;
    bool visible = false;
    char * w = text;
    for (const char * r = text; *r; r++) {
        unsigned char c = (unsigned char) *r;
        if (c == '\r' && r[1] == '\n') continue;
        if (c < 0x20 || c == 0x7F) c = ' ';
        if (c != ' ') visible = true;
        *w++ = (char) c;
    }
    *w = '\0';
    if (!visible) {
        text[0] = '\0';
        *present = false;
    }
}

void metadata_normalize_text_tags(track_metadata_t * out) {
    normalize_tag_text(out->title, &out->has_title);
    normalize_tag_text(out->artist, &out->has_artist);
    normalize_tag_text(out->album, &out->has_album);
    normalize_tag_text(out->album_artist, &out->has_album_artist);
    normalize_tag_text(out->genre, &out->has_genre);
}

static void metadata_read_dispatch(const char * path, track_metadata_t * out, bool include_blobs);

static void metadata_read_internal(const char * path, track_metadata_t * out, bool include_blobs) {
    metadata_read_dispatch(path, out, include_blobs);
    metadata_normalize_text_tags(out);
}

static void metadata_read_dispatch(const char * path, track_metadata_t * out, bool include_blobs) {
    memset(out, 0, sizeof(*out));

    const char * ext = strrchr(path, '.');
    if (!ext) return;

    if (strcasecmp(ext, ".flac") == 0) {
        read_flac_metadata(path, out, include_blobs);
    } else if (strcasecmp(ext, ".mp3") == 0) {
        read_mp3_metadata(path, out, include_blobs);
    } else if (strcasecmp(ext, ".wav") == 0) {
        read_wav_all(path, out, include_blobs);
    } else if (strcasecmp(ext, ".aac") == 0) {
        read_aac_metadata(path, out, include_blobs);
    } else if (strcasecmp(ext, ".m4a") == 0 || strcasecmp(ext, ".m4b") == 0) {
        read_m4a_metadata(path, out, include_blobs);
    } else if (strcasecmp(ext, ".opus") == 0) {
        read_opus_metadata(path, out, include_blobs);
    } else if (strcasecmp(ext, ".ogg") == 0) {
        ogg_codec_t codec = ogg_detect_codec(path);
        if (codec == OGG_CODEC_OPUS) read_opus_metadata(path, out, include_blobs);
        else if (codec == OGG_CODEC_VORBIS) read_ogg_vorbis_metadata(path, out, include_blobs);
    } else if (strcasecmp(ext, ".aiff") == 0 || strcasecmp(ext, ".aif") == 0) {
        read_aiff_metadata(path, out, include_blobs);
    } else if (strcasecmp(ext, ".dsf") == 0) {
        read_dsf_metadata(path, out, include_blobs);
    } else if (strcasecmp(ext, ".ape") == 0) {
        read_ape_metadata(path, out);
    } else if (strcasecmp(ext, ".dff") == 0) {
        read_dff_metadata(path, out, include_blobs);
    } else if (strcasecmp(ext, ".wma") == 0) {
        read_wma_metadata(path, out);
    }
}

void metadata_read(const char * path, track_metadata_t * out) {
    metadata_read_internal(path, out, true);
}

void metadata_read_without_artwork(const char * path, track_metadata_t * out) {
    /* The no-blob mode preserves textual tags and ReplayGain while avoiding
     * embedded artwork (lyrics are intentionally omitted as well). */
    metadata_read_internal(path, out, false);
}

void metadata_read_lyrics_without_artwork(const char * path, track_metadata_t * out) {
    bool previous = lyrics_only;
    lyrics_only = true;
    metadata_read_internal(path, out, true);
    lyrics_only = previous;
}

/* Isolates decoder-based metadata parsing in a short-lived child process so
 * timeouts can be cleanly terminated with SIGKILL without leaking threads or
 * risking memory corruption from decoder crashes.
 *
 * MP3 and raw AAC files skip the fork because they use internal bounded ID3
 * readers. Embedded pictures and lyrics are omitted in isolated reads to keep
 * IPC fixed-size. */
static bool isolated_needs_child(const char * path) {
    const char * ext = strrchr(path, '.');
    if (!ext) return false;
    if (strcasecmp(ext, ".mp3") == 0 || strcasecmp(ext, ".aac") == 0) return false;
    return true;
}

/* Children stuck in uninterruptible SD-card I/O cannot be synchronously
 * reaped even after SIGKILL. Keep a small explicit set and retry WNOHANG on
 * later scan calls; never use waitpid(-1), which could steal a child owned
 * by another subsystem. Once the set is full, new isolated reads fail
 * closed instead of creating an unbounded number of unreapable children. */
#define METADATA_ABANDONED_CHILD_MAX 8
typedef struct {
    pthread_mutex_t mutex;
    pid_t children[METADATA_ABANDONED_CHILD_MAX];
    int capacity;
    const char * label;
} metadata_child_pool_t;

static metadata_child_pool_t scan_child_pool = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .capacity = METADATA_ABANDONED_CHILD_MAX,
    .label = "scan",
};
static metadata_child_pool_t artwork_child_pool = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .capacity = 2,
    .label = "artwork",
};
static pthread_mutex_t artwork_isolation_mutex = PTHREAD_MUTEX_INITIALIZER;

static void reap_abandoned_metadata_children(metadata_child_pool_t * pool) {
    pthread_mutex_lock(&pool->mutex);
    for (int i = 0; i < pool->capacity; i++) {
        pid_t pid = pool->children[i];
        if (pid <= 0) continue;
        int status = 0;
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid || (r < 0 && errno == ECHILD)) pool->children[i] = 0;
    }
    pthread_mutex_unlock(&pool->mutex);
}

static bool abandoned_metadata_child_capacity_available(metadata_child_pool_t * pool) {
    bool available = false;
    pthread_mutex_lock(&pool->mutex);
    for (int i = 0; i < pool->capacity; i++) {
        if (pool->children[i] == 0) {
            available = true;
            break;
        }
    }
    pthread_mutex_unlock(&pool->mutex);
    return available;
}

static void remember_abandoned_metadata_child(metadata_child_pool_t * pool, pid_t pid) {
    pthread_mutex_lock(&pool->mutex);
    for (int i = 0; i < pool->capacity; i++) {
        if (pool->children[i] == 0) {
            pool->children[i] = pid;
            pid = 0;
            break;
        }
    }
    pthread_mutex_unlock(&pool->mutex);
    if (pid > 0)
        fprintf(stderr, "metadata %s: abandoned-child set full; pid %ld remains for init to reap\n",
                pool->label, (long) pid);
}

static void metadata_read_scan_tags(const char * path, track_metadata_t * out) {
    metadata_read_internal(path, out, false);
    free(out->picture_data);
    out->picture_data = NULL;
    out->picture_size = 0;
    free(out->lyrics);
    out->lyrics = NULL;
}

static bool reap_scan_parser_child(pid_t pid, track_metadata_t * out, bool ok) {
    /* Bounded reap, same as subprocess_run_timeout()'s own final wait --
     * even after SIGKILL, a child genuinely stuck in an uninterruptible
     * kernel read stays unreapable until that I/O naturally unblocks, so
     * this can't be a plain blocking waitpid() either. */
    /* A successful child has already sent its full result and should exit
     * promptly; the shorter poll avoids a 50 ms delay on each quick read. */
    const int reap_sleep_us = ok ? 1000 : 50000;
    for (int waited_us = 0; waited_us < 1000000; waited_us += reap_sleep_us) {
        int status = 0;
        if (waitpid(pid, &status, WNOHANG) == pid) {
            if (ok && WIFEXITED(status) && WEXITSTATUS(status) == 0) return true;
            memset(out, 0, sizeof(*out));
            return false;
        }
        usleep(reap_sleep_us);
    }
    kill(pid, SIGKILL);
    remember_abandoned_metadata_child(&scan_child_pool, pid);
    return ok;
}

bool metadata_read_isolated(const char * path, track_metadata_t * out, int timeout_ms) {
    memset(out, 0, sizeof(*out));
    if (!path || !path[0]) return false;
    if (!isolated_needs_child(path)) {
        metadata_read_scan_tags(path, out);
        return true;
    }

    reap_abandoned_metadata_children(&scan_child_pool);
    if (!abandoned_metadata_child_capacity_available(&scan_child_pool)) {
        fprintf(stderr, "metadata scan: too many unreaped parser children; skipping %s\n", path);
        return false;
    }

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        fprintf(stderr, "metadata scan: pipe failed for %s: %s\n", path, strerror(errno));
        return false;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        fprintf(stderr, "metadata scan: fork failed for %s: %s\n", path, strerror(errno));
        return false;
    }

    if (pid == 0) {
        close(pipefd[0]);
        track_metadata_t result;
        metadata_read_scan_tags(path, &result);
        size_t written = 0;
        while (written < sizeof(result)) {
            ssize_t n = write(pipefd[1], (const char *) &result + written, sizeof(result) - written);
            if (n <= 0) break;
            written += (size_t) n;
        }
        _exit(0);
    }

    close(pipefd[1]);

    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);

    size_t total = 0;
    bool ok = true;
    while (total < sizeof(*out)) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        int64_t elapsed_ms = (int64_t)(now.tv_sec - start.tv_sec) * 1000 +
                             (int64_t)(now.tv_nsec - start.tv_nsec) / 1000000;
        int remaining_ms = timeout_ms - (int) elapsed_ms;
        if (remaining_ms <= 0) {
            ok = false;
            break;
        }

        struct pollfd pfd = { .fd = pipefd[0], .events = POLLIN };
        int pr = poll(&pfd, 1, remaining_ms);
        if (pr <= 0) { ok = false; break; }
        ssize_t n = read(pipefd[0], (char *) out + total, sizeof(*out) - total);
        if (n <= 0) { ok = false; break; }
        total += (size_t) n;
    }
    close(pipefd[0]);
    if (!ok) {
        memset(out, 0, sizeof(*out));
        kill(pid, SIGKILL);
    }

    return reap_scan_parser_child(pid, out, ok);
}

static bool read_with_deadline(int fd, void * dst, size_t size,
                               const struct timespec * start, int timeout_ms) {
    size_t total = 0;
    while (total < size) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        int64_t elapsed_ms = (int64_t) (now.tv_sec - start->tv_sec) * 1000 +
                             (int64_t) (now.tv_nsec - start->tv_nsec) / 1000000;
        int remaining_ms = timeout_ms - (int) elapsed_ms;
        if (remaining_ms <= 0) return false;

        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int pr;
        do pr = poll(&pfd, 1, remaining_ms); while (pr < 0 && errno == EINTR);
        if (pr <= 0) return false;
        ssize_t n;
        do n = read(fd, (char *) dst + total, size - total); while (n < 0 && errno == EINTR);
        if (n <= 0) return false;
        total += (size_t) n;
    }
    return true;
}

static bool write_all(int fd, const void * src, size_t size) {
    size_t total = 0;
    while (total < size) {
        ssize_t n;
        do n = write(fd, (const char *) src + total, size - total);
        while (n < 0 && errno == EINTR);
        if (n <= 0) return false;
        total += (size_t) n;
    }
    return true;
}

static bool metadata_artwork_limit_memory(artwork_priority_t prio) {
    /* Bound ALL parser allocations, including third-party container readers.
     * The limit is headroom, not an allocation: a small picture only uses
     * its actual bytes. Reserve matches the CALLER's real priority (see
     * artwork_reserve_bytes_for_priority()'s own comment) -- this used to
     * always use the warmer's own 8 MiB reserve regardless of priority,
     * refusing to even attempt an interactive PRIO_PLAYER extraction under
     * the same ~9MB-available threshold that only the background warmer
     * should be held to. */
    size_t available = system_get_mem_available_bytes();
    const size_t reserve = artwork_reserve_bytes_for_priority(prio);
    if (available <= reserve + 1024U * 1024U) return false;
    size_t budget = available - reserve;
    if (budget > 12U * 1024U * 1024U) budget = 12U * 1024U * 1024U;
    FILE * statm = fopen("/proc/self/statm", "r");
    unsigned long pages = 0;
    bool have_size = statm && fscanf(statm, "%lu", &pages) == 1;
    if (statm) fclose(statm);
    long page_size = sysconf(_SC_PAGESIZE);
    struct rlimit limit;
    if (!have_size || page_size <= 0 || getrlimit(RLIMIT_AS, &limit) != 0) return false;
    uint64_t ceiling = (uint64_t) pages * (unsigned long) page_size + budget;
    if (ceiling > SIZE_MAX) return false;
    if (limit.rlim_cur == RLIM_INFINITY || ceiling < limit.rlim_cur)
        limit.rlim_cur = (rlim_t) ceiling;
    return setrlimit(RLIMIT_AS, &limit) == 0;
}

int metadata_artwork_helper_run(const char * path, int output_fd, artwork_priority_t prio) {
    if (!path || !path[0] || output_fd < 0) return 1;
    if (!metadata_artwork_limit_memory(prio)) return 1;
    artwork_only = true;

    /* This process is disposable; under unexpected memory pressure the
     * kernel should discard it instead of the interactive player. */
    int oom_fd = open("/proc/self/oom_score_adj", O_WRONLY);
    if (oom_fd >= 0) {
        const char score[] = "500";
        ssize_t ignored = write(oom_fd, score, sizeof(score) - 1);
        (void) ignored;
        close(oom_fd);
    }

    track_metadata_t result;
    errno = 0;
    metadata_read(path, &result);
    if (!result.picture_data && errno == ENOMEM) {
        free(result.lyrics);
        return 1;
    }
    uint8_t * picture = result.picture_data;
    uint32_t picture_size = result.picture_size;
    char * lyrics = result.lyrics;
    result.picture_data = NULL;
    result.lyrics = NULL;

    bool ok = write_all(output_fd, &result, sizeof(result));
    if (ok && picture_size > 0)
        ok = picture && write_all(output_fd, picture, picture_size);
    free(picture);
    free(lyrics);
    close(output_fd);
    return ok ? 0 : 1;
}

static metadata_artwork_result_t metadata_read_artwork_isolated_impl(const char * path,
                                                                     track_metadata_t * out,
                                                                     int timeout_ms,
                                                                     artwork_priority_t prio) {
    memset(out, 0, sizeof(*out));
    if (!path || !path[0] || timeout_ms <= 0) return METADATA_ARTWORK_TEMPORARY_FAILURE;

    reap_abandoned_metadata_children(&artwork_child_pool);
    if (!abandoned_metadata_child_capacity_available(&artwork_child_pool))
        return METADATA_ARTWORK_TEMPORARY_FAILURE;

    int pipefd[2];
    if (pipe(pipefd) != 0) return METADATA_ARTWORK_TEMPORARY_FAILURE;
    char output_fd_arg[24];
    snprintf(output_fd_arg, sizeof(output_fd_arg), "%d", pipefd[1]);
    /* Formatted here, before fork() -- snprintf() in the freshly-forked
     * (but still address-space-copied, pre-exec) child would run in a
     * process that inherited every lock this multithreaded parent might
     * have held mid-acquire; safer to have both argv strings ready in the
     * parent and just execv() them across. */
    char prio_arg[8];
    snprintf(prio_arg, sizeof(prio_arg), "%d", (int) prio);
    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return METADATA_ARTWORK_TEMPORARY_FAILURE;
    }

    if (pid == 0) {
        close(pipefd[0]);
        char * const argv[] = {
            (char *) "compas_player",
            (char *) "--metadata-artwork-helper",
            output_fd_arg,
            (char *) path,
            prio_arg,
            NULL,
        };
        execv("/proc/self/exe", argv);
        _exit(127);
    }

    close(pipefd[1]);
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);

    track_metadata_t result;
    bool ok = read_with_deadline(pipefd[0], &result, sizeof(result), &start, timeout_ms);
    metadata_artwork_result_t outcome = METADATA_ARTWORK_TEMPORARY_FAILURE;
    uint8_t * picture = NULL;
    if (ok && result.picture_size > EMBEDDED_COVER_MAX_BYTES) {
        outcome = METADATA_ARTWORK_INVALID;
        ok = false;
    }
    if (ok && result.picture_too_large && result.picture_size == 0) {
        outcome = METADATA_ARTWORK_TOO_LARGE;
    } else if (ok && result.picture_size > 0) {
        /* The helper still owns its copy. Charge only the additional copy
         * against current free memory, not a fixed worst-case tag size --
         * against the CALLER's real reserve (`prio`), not always the
         * warmer's own stricter one (see this function's own header
         * comment). */
        picture = artwork_check_memory_admission(prio, result.picture_size)
            ? malloc(result.picture_size) : NULL;
        ok = picture && read_with_deadline(pipefd[0], picture, result.picture_size, &start, timeout_ms);
    }
    close(pipefd[0]);

    if (ok) {
        result.picture_data = picture;
        result.lyrics = NULL;
        *out = result;
        if (result.picture_too_large && !picture) outcome = METADATA_ARTWORK_TOO_LARGE;
        else outcome = picture ? METADATA_ARTWORK_FOUND : METADATA_ARTWORK_NOT_FOUND;
    } else {
        free(picture);
        kill(pid, SIGKILL);
    }

    for (int waited_ms = 0; waited_ms < 1000; waited_ms += 50) {
        int status = 0;
        if (waitpid(pid, &status, WNOHANG) == pid) {
            if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
                free(out->picture_data);
                memset(out, 0, sizeof(*out));
                return outcome == METADATA_ARTWORK_INVALID
                    ? METADATA_ARTWORK_INVALID
                    : outcome == METADATA_ARTWORK_TOO_LARGE
                    ? METADATA_ARTWORK_TOO_LARGE
                    : METADATA_ARTWORK_TEMPORARY_FAILURE;
            }
            return outcome;
        }
        usleep(50000);
    }
    kill(pid, SIGKILL);
    remember_abandoned_metadata_child(&artwork_child_pool, pid);
    return outcome;
}

metadata_artwork_result_t metadata_read_artwork_isolated(const char * path,
                                                         track_metadata_t * out,
                                                         int timeout_ms,
                                                         artwork_priority_t prio) {
    if (!out) return METADATA_ARTWORK_TEMPORARY_FAILURE;
    memset(out, 0, sizeof(*out));
    pthread_mutex_lock(&artwork_isolation_mutex);
    metadata_artwork_result_t result =
        metadata_read_artwork_isolated_impl(path, out, timeout_ms, prio);
    pthread_mutex_unlock(&artwork_isolation_mutex);
    return result;
}
