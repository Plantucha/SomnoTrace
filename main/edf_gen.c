/*
 * SomnoTrace - EDF file generation from session data warehouse
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 *
 * This file is part of SomnoTrace.
 *
 * SomnoTrace is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * SomnoTrace is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
 * A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 *
 * ADDITIONAL TERM (GPLv3 Section 7(b)): Redistributions must preserve the
 * attribution "Based on SomnoTrace, originally created by Ilya Kruchinin
 * (https://github.com/ilyakruchinin)." See the NOTICE file for details.
 */

#include "edf_gen.h"
#include "sd_storage.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "cJSON.h"

static const char *TAG = "edf_gen";

/* ════════════════════════════════════════════════════════════════════
 *  Section 1: CRC functions
 * ════════════════════════════════════════════════════════════════════
 *
 * CRC16-CCITT-FALSE: used for EDF patient ID CRC words and the Crc16
 * signal in every EDF data record.
 * Polynomial 0x1021, init 0xFFFF, no final XOR (CCITT-FALSE variant).
 *
 * CRC-32 (IEEE 802.3): used for Identification.crc file.
 * Polynomial 0xEDB88320 (reflected), init 0xFFFFFFFF, xorout 0xFFFFFFFF.
 */

static uint16_t crc16_ccitt(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; b++) {
            if (crc & 0x8000)
                crc = (crc << 1) ^ 0x1021;
            else
                crc <<= 1;
        }
    }
    return crc;
}

static uint32_t crc32_ieee(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            if (crc & 1)
                crc = (crc >> 1) ^ 0xEDB88320;
            else
                crc >>= 1;
        }
    }
    return crc ^ 0xFFFFFFFF;
}

/* ════════════════════════════════════════════════════════════════════
 *  Section 2: Minimal protobuf wire-format decoder
 * ════════════════════════════════════════════════════════════════════
 *
 * The AS11 spool payloads are protobuf-encoded.  We need a minimal decoder
 * that can extract varint and length-delimited fields without a .proto
 * schema.  The field number and wire type are packed into the first byte(s)
 * of each field tag as a varint: (field_number << 3) | wire_type.
 *
 * Wire types:
 *   0 = varint       (int64, uint64, bool, enum)
 *   1 = 64-bit       (fixed64, sfixed64, double)
 *   2 = length-delimited (string, bytes, embedded message, packed repeated)
 *   5 = 32-bit       (fixed32, sfixed32, float)
 */

typedef struct {
    int field;
    int wire;
    const uint8_t *data;
    size_t len;
} pb_field_t;

/* Decode a varint from buf, advancing *pos.  Returns the value. */
static uint64_t pb_decode_varint(const uint8_t *buf, size_t buf_len,
                                 size_t *pos)
{
    uint64_t val = 0;
    int shift = 0;
    while (*pos < buf_len) {
        uint8_t b = buf[(*pos)++];
        val |= (uint64_t)(b & 0x7F) << shift;
        if (!(b & 0x80)) break;
        shift += 7;
        if (shift >= 64) break;
    }
    return val;
}

/* Iterate over top-level fields in a protobuf message.
 * Calls cb(field, wire, data, len, user_data) for each field.
 * Returns number of fields decoded. */
typedef void (*pb_iter_cb)(const pb_field_t *f, void *user_data);

static int pb_iter(const uint8_t *buf, size_t buf_len, pb_iter_cb cb, void *ud)
{
    size_t pos = 0;
    int count = 0;
    while (pos < buf_len) {
        size_t tag_start = pos;
        uint64_t tag = pb_decode_varint(buf, buf_len, &pos);
        int field = (int)(tag >> 3);
        int wire = (int)(tag & 0x07);
        if (field == 0) break;

        pb_field_t f = { .field = field, .wire = wire };

        switch (wire) {
        case 0: {  /* varint */
            size_t vpos = pos;
            f.data = buf + tag_start;
            f.len = 0;  /* varint value is decoded by caller if needed */
            /* Skip the varint value */
            pb_decode_varint(buf, buf_len, &vpos);
            f.len = vpos - pos;
            f.data = buf + pos;
            pos = vpos;
            break;
        }
        case 1: {  /* 64-bit */
            if (pos + 8 > buf_len) return count;
            f.data = buf + pos;
            f.len = 8;
            pos += 8;
            break;
        }
        case 2: {  /* length-delimited */
            size_t lpos = pos;
            uint64_t flen = pb_decode_varint(buf, buf_len, &lpos);
            pos = lpos;
            if (pos + flen > buf_len) return count;
            f.data = buf + pos;
            f.len = (size_t)flen;
            pos += flen;
            break;
        }
        case 5: {  /* 32-bit */
            if (pos + 4 > buf_len) return count;
            f.data = buf + pos;
            f.len = 4;
            pos += 4;
            break;
        }
        default:
            /* Unknown wire type — stop */
            return count;
        }

        cb(&f, ud);
        count++;
    }
    return count;
}

/* Helper: extract a varint value from a field's data. */
static int64_t pb_varint_val(const pb_field_t *f)
{
    if (!f || !f->data) return 0;
    size_t pos = 0;
    return (int64_t)pb_decode_varint(f->data, f->len > 0 ? f->len : 10, &pos);
}

/* ════════════════════════════════════════════════════════════════════
 *  Section 3: EDF header writer
 * ════════════════════════════════════════════════════════════════════
 *
 * EDF fixed header is 256 bytes, followed by 256 bytes per signal of
 * per-signal metadata.  All text fields are ASCII, left-aligned,
 * space-padded, NOT null-terminated.
 *
 * AS11 EDF patient ID format: "X X X X %04X %04X"
 *   - First CRC16: over fixed header bytes 0x19..0xFF (after patient ID field)
 *   - Second CRC16: over all signal header blocks (0x100..header_bytes-1)
 *
 * AS11 EDF recording ID format:
 *   "Startdate DD-MMM-YYYY X X X SRN=<serial> MID=<mid> VID=<vid>"
 */

/* Write a left-aligned, space-padded text field to a buffer. */
static void edf_write_field(char *buf, int width, const char *text)
{
    memset(buf, ' ', width);
    if (text) {
        int len = strlen(text);
        if (len > width) len = width;
        memcpy(buf, text, len);
    }
}

static bool write_all(FILE *f, const void *data, size_t len)
{
    return fwrite(data, 1, len, f) == len;
}

static FILE *open_atomic_file(const char *path, char *tmp_path, size_t tmp_path_len)
{
    int written = snprintf(tmp_path, tmp_path_len, "%s.tmp", path);
    if (written < 0 || (size_t)written >= tmp_path_len) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    unlink(tmp_path);
    return fopen(tmp_path, "wb");
}

static esp_err_t finalize_atomic_file(FILE *f, const char *tmp_path, const char *path)
{
    int saved_errno = 0;
    if (fflush(f) != 0 || fsync(fileno(f)) != 0 || fclose(f) != 0) {
        saved_errno = errno;
        unlink(tmp_path);
        errno = saved_errno;
        return ESP_FAIL;
    }
    /* FATFS does not support rename-over-existing — remove target first. */
    unlink(path);
    if (rename(tmp_path, path) != 0) {
        saved_errno = errno;
        unlink(tmp_path);
        errno = saved_errno;
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void discard_atomic_file(FILE *f, const char *tmp_path)
{
    if (f) fclose(f);
    if (tmp_path) unlink(tmp_path);
}

/* Signal metadata definition. */
typedef struct {
    const char *label;      /* 16 chars */
    const char *transducer; /* 80 chars */
    const char *unit;       /* 8 chars */
    double phys_min;        /* 8 chars */
    double phys_max;        /* 8 chars */
    int dig_min;            /* 8 chars */
    int dig_max;            /* 8 chars */
    const char *prefilter;  /* 80 chars */
    int samples_per_record; /* 8 chars */
    /* When true, a raw .snt sample equal to the invalid sentinel (-1) is
     * written to the EDF as the invalid marker (-1) directly, bypassing the
     * physical→digital scaling.  AS11 uses -1 to mark "no data" samples
     * (e.g. SpO2/Pulse when no oximeter is connected).  Leave false for
     * signals where -1 is a legitimate measurement (e.g. BRP Flow). */
    bool invalid_passthrough;
} edf_signal_def_t;

/* Write the complete EDF header (fixed + per-signal) to a file.
 * Returns the header byte count on success, or -1 on error.
 *
 * The Crc16 signal is automatically appended as the last signal.
 *
 * Parameters:
 *   f           - open FILE* positioned at start
 *   patient_id  - pre-formatted patient ID string (80 chars max)
 *   recording_id - pre-formatted recording ID string (80 chars max)
 *   start_date  - "dd.mm.yy"
 *   start_time  - "hh.mm.ss"
 *   record_count - number of data records
 *   record_dur  - record duration string e.g. "60.00" or "86400.00"
 *   reserved    - "EDF" or "EDF+D"
 *   signals     - array of signal definitions (NOT including Crc16)
 *   n_signals   - number of signals (NOT including Crc16)
 */
static int edf_write_header(FILE *f, const char *patient_id,
                            const char *recording_id,
                            const char *start_date, const char *start_time,
                            int record_count, const char *record_dur,
                            const char *reserved,
                            const edf_signal_def_t *signals, int n_signals)
{
    /* Total signals including Crc16 */
    int total_signals = n_signals + 1;
    int header_bytes = 256 + 256 * total_signals;

    /* ── Fixed header (256 bytes) ── */
    char hdr[256];
    memset(hdr, ' ', sizeof(hdr));

    edf_write_field(hdr + 0, 8, "0");                    /* version */
    edf_write_field(hdr + 8, 80, patient_id);            /* patient ID */
    edf_write_field(hdr + 88, 80, recording_id);         /* recording ID */
    edf_write_field(hdr + 168, 8, start_date);           /* start date */
    edf_write_field(hdr + 176, 8, start_time);           /* start time */
    char hb[16];
    snprintf(hb, sizeof(hb), "%d", header_bytes);
    edf_write_field(hdr + 184, 8, hb);                   /* header bytes */
    edf_write_field(hdr + 192, 44, reserved);            /* reserved */
    char rc[16];
    snprintf(rc, sizeof(rc), "%d", record_count);
    edf_write_field(hdr + 236, 8, rc);                   /* record count */
    edf_write_field(hdr + 244, 8, record_dur);           /* record duration */
    char ns[16];
    snprintf(ns, sizeof(ns), "%d", total_signals);
    edf_write_field(hdr + 252, 4, ns);                   /* signal count */

    /* ── Per-signal header blocks (256 bytes per signal) ──
     * EDF stores all signal metadata fields in interleaved order:
     * first all labels, then all transducer types, etc. */

    int total = total_signals;
    /* Signal header blocks: 256 bytes per signal.  STR.edf has 78 signals
     * (77 data + Crc16) = 19,968 bytes — too large for stack allocation.
     * Use heap allocation to handle any signal count. */
    size_t sigblock_size = 256 * total;
    char *sigblock = malloc(sigblock_size);
    if (!sigblock) {
        ESP_LOGE(TAG, "edf_write_header: malloc sigblock %u failed",
                 (unsigned)sigblock_size);
        return -1;
    }
    memset(sigblock, ' ', sigblock_size);

    /* For each metadata field, write all signals' values */
    /* Field 1: label (16 chars each) */
    for (int i = 0; i < n_signals; i++)
        edf_write_field(sigblock + i * 16, 16, signals[i].label);
    edf_write_field(sigblock + n_signals * 16, 16, "Crc16");

    /* Field 2: transducer type (80 chars each) */
    int offset = total * 16;
    for (int i = 0; i < n_signals; i++)
        edf_write_field(sigblock + offset + i * 80, 80, signals[i].transducer);
    edf_write_field(sigblock + offset + n_signals * 80, 80, "");

    /* Field 3: physical dimension (8 chars each) */
    offset = total * (16 + 80);
    for (int i = 0; i < n_signals; i++)
        edf_write_field(sigblock + offset + i * 8, 8, signals[i].unit);
    edf_write_field(sigblock + offset + n_signals * 8, 8, "");

    /* Field 4: physical minimum (8 chars each).
     * AS11 formats enum signals (phys==dig, small range) as integers,
     * all others with %.2f. */
    offset = total * (16 + 80 + 8);
    for (int i = 0; i < n_signals; i++) {
        char buf[16];
        if (signals[i].dig_min == 0 && signals[i].dig_max == 16)
            snprintf(buf, sizeof(buf), "%d", signals[i].dig_min);
        else
            snprintf(buf, sizeof(buf), "%.2f", signals[i].phys_min);
        edf_write_field(sigblock + offset + i * 8, 8, buf);
    }
    edf_write_field(sigblock + offset + n_signals * 8, 8, "-32768.0");

    /* Field 5: physical maximum (8 chars each) */
    offset = total * (16 + 80 + 8 + 8);
    for (int i = 0; i < n_signals; i++) {
        char buf[16];
        if (signals[i].dig_min == 0 && signals[i].dig_max == 16)
            snprintf(buf, sizeof(buf), "%d", signals[i].dig_max);
        else
            snprintf(buf, sizeof(buf), "%.2f", signals[i].phys_max);
        edf_write_field(sigblock + offset + i * 8, 8, buf);
    }
    edf_write_field(sigblock + offset + n_signals * 8, 8, "32767.00");

    /* Field 6: digital minimum (8 chars each) */
    offset = total * (16 + 80 + 8 + 8 + 8);
    for (int i = 0; i < n_signals; i++) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", signals[i].dig_min);
        edf_write_field(sigblock + offset + i * 8, 8, buf);
    }
    edf_write_field(sigblock + offset + n_signals * 8, 8, "-32768");

    /* Field 7: digital maximum (8 chars each) */
    offset = total * (16 + 80 + 8 + 8 + 8 + 8);
    for (int i = 0; i < n_signals; i++) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", signals[i].dig_max);
        edf_write_field(sigblock + offset + i * 8, 8, buf);
    }
    edf_write_field(sigblock + offset + n_signals * 8, 8, "32767");

    /* Field 8: prefiltering (80 chars each) */
    offset = total * (16 + 80 + 8 + 8 + 8 + 8 + 8);
    for (int i = 0; i < n_signals; i++)
        edf_write_field(sigblock + offset + i * 80, 80, signals[i].prefilter);
    edf_write_field(sigblock + offset + n_signals * 80, 80, "");

    /* Field 9: samples per data record (8 chars each) */
    offset = total * (16 + 80 + 8 + 8 + 8 + 8 + 8 + 80);
    for (int i = 0; i < n_signals; i++) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", signals[i].samples_per_record);
        edf_write_field(sigblock + offset + i * 8, 8, buf);
    }
    edf_write_field(sigblock + offset + n_signals * 8, 8, "1");

    /* Field 10: reserved (32 chars each) */
    offset = total * (16 + 80 + 8 + 8 + 8 + 8 + 8 + 80 + 8);
    /* Already zeroed (space-padded) */

    /* Compute header CRCs from in-memory buffers BEFORE writing to disk.
     * This avoids seeking back to read the header after data has been
     * written, which causes FATFS per-file cache eviction and data loss.
     * CRC1 = crc16(hdr[0x19..0xFF]), CRC2 = crc16(sigblock[0..end]).
     * The patient ID placeholder ("X X X X 0000 0000") is used for the
     * CRC computation, then replaced with the actual CRC values. */
    uint16_t crc1 = crc16_ccitt((uint8_t *)hdr + 0x19, 256 - 0x19);
    uint16_t crc2 = crc16_ccitt((uint8_t *)sigblock, 256 * total);

    ESP_LOGI(TAG, "edf_write_header: H1=%04X H2=%04X", crc1, crc2);

    /* Write CRC values into the patient ID field */
    char pid[81];
    snprintf(pid, sizeof(pid), "X X X X %04X %04X", crc1, crc2);
    int plen = strlen(pid);
    while (plen < 80) pid[plen++] = ' ';
    pid[80] = '\0';
    memcpy(hdr + 8, pid, 80);

    bool written = write_all(f, hdr, sizeof(hdr)) &&
                   write_all(f, sigblock, 256 * total);

    free(sigblock);
    return written ? header_bytes : -1;
}

/* ════════════════════════════════════════════════════════════════════
 *  Section 4: .snt file format reader
 * ════════════════════════════════════════════════════════════════════ */

#define SNT_MAGIC 0x534E5442u  /* "SNTB" */

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t  version;
    uint8_t  tier;
    uint8_t  n_channels;
    uint8_t  sample_bytes;
    uint16_t sample_hz_x10;
    uint16_t reserved;
    int64_t  start_epoch_ms;
    uint32_t sample_count;
    uint32_t reserved2;
} snt_header_t;

/* Read and validate an .snt file header.  Returns ESP_OK on success. */
static esp_err_t snt_read_header(FILE *f, snt_header_t *hdr)
{
    if (fread(hdr, 1, sizeof(snt_header_t), f) != sizeof(snt_header_t)) {
        return ESP_FAIL;
    }
    if (hdr->magic != SNT_MAGIC) {
        ESP_LOGE(TAG, "bad SNT magic: 0x%08x", hdr->magic);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ════════════════════════════════════════════════════════════════════
 *  Section 5: .snt → EDF conversion for BRP, SA2, PLD
 * ════════════════════════════════════════════════════════════════════
 *
 * Converts .snt interleaved int16 data into EDF data records.
 * Each EDF data record contains 60 seconds of data for all signals,
 * followed by a Crc16 sample (CRC16-CCITT-FALSE of the record's signal
 * data, stored little-endian).
 *
 * The .snt format stores samples as interleaved int16 values:
 *   [ch0_s0, ch1_s0, ch0_s1, ch1_s1, ...]
 * The EDF format stores all samples for signal 0, then all for signal 1, etc.
 */

/* Find the MaskOn event timestamp from events.snt.
 *
 * events.snt contains JSON lines with EventNotification messages.
 * Each event has "reportTime" (ISO 8601 UTC string, AS11 internal clock)
 * and "event" label.  We look for the first "MaskOn" event and return
 * its epoch milliseconds in the AS11 clock domain.
 *
 * Returns the AS11-clock epoch ms, or -1 if MaskOn is not found.
 * The caller converts to NTP by adding clock_drift_ms. */
static int64_t parse_iso8601_utc_ms(const char *iso_str)
{
    if (!iso_str) return -1;
    struct tm ev_tm = {0};
    int ms = 0;
    int n = sscanf(iso_str, "%d-%d-%dT%d:%d:%d.%dZ",
                   &ev_tm.tm_year, &ev_tm.tm_mon, &ev_tm.tm_mday,
                   &ev_tm.tm_hour, &ev_tm.tm_min, &ev_tm.tm_sec, &ms);
    if (n < 6) {
        n = sscanf(iso_str, "%d-%d-%dT%d:%d:%dZ",
                   &ev_tm.tm_year, &ev_tm.tm_mon, &ev_tm.tm_mday,
                   &ev_tm.tm_hour, &ev_tm.tm_min, &ev_tm.tm_sec);
        if (n < 6) return -1;
        ms = 0;
    }
    ev_tm.tm_year -= 1900;
    ev_tm.tm_mon -= 1;
    /* Howard Hinnant's date algorithm (public domain). */
    int y = ev_tm.tm_year + 1900;
    int m = ev_tm.tm_mon + 1;
    int d = ev_tm.tm_mday;
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = (int64_t)era * 146097 + (int)doe - 719468;
    int64_t secs = days * 86400 + ev_tm.tm_hour * 3600 +
                   ev_tm.tm_min * 60 + ev_tm.tm_sec;
    return secs * 1000 + ms;
}

/* Find the first MaskOn event timestamp in events.snt.
 * Returns AS11-clock epoch ms, or -1 if MaskOn is not found.
 * The caller converts to NTP by adding clock_drift_ms. */
static int64_t find_mask_on_time(const char *events_snt_path)
{
    FILE *f = fopen(events_snt_path, "r");
    if (!f) return -1;

    int64_t mask_on_ms = -1;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        cJSON *msg = cJSON_Parse(line);
        if (!msg) continue;
        cJSON *params = cJSON_GetObjectItem(msg, "params");
        if (params) {
            cJSON *events = cJSON_GetObjectItem(params, "events");
            if (events && cJSON_IsArray(events)) {
                int n = cJSON_GetArraySize(events);
                for (int i = 0; i < n; i++) {
                    cJSON *ev = cJSON_GetArrayItem(events, i);
                    if (!ev) continue;
                    cJSON *label = cJSON_GetObjectItem(ev, "event");
                    if (!label || !cJSON_IsString(label)) continue;
                    if (strcmp(label->valuestring, "MaskOn") == 0) {
                        cJSON *rt = cJSON_GetObjectItem(ev, "reportTime");
                        if (rt && cJSON_IsString(rt)) {
                            mask_on_ms = parse_iso8601_utc_ms(rt->valuestring);
                            cJSON_Delete(msg);
                            fclose(f);
                            return mask_on_ms;
                        }
                    }
                }
            }
        }
        cJSON_Delete(msg);
    }
    fclose(f);
    return mask_on_ms;
}

/* Find the last MaskOff event timestamp in events.snt.
 * Returns AS11-clock epoch ms, or -1 if MaskOff is not found.
 * We scan for the last (not first) MaskOff because a session may contain
 * multiple MaskOn/MaskOff pairs (brief mask removals within one session).
 * The caller converts to NTP by adding clock_drift_ms. */
static int64_t find_mask_off_time(const char *events_snt_path)
{
    FILE *f = fopen(events_snt_path, "r");
    if (!f) return -1;

    int64_t mask_off_ms = -1;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        cJSON *msg = cJSON_Parse(line);
        if (!msg) continue;
        cJSON *params = cJSON_GetObjectItem(msg, "params");
        if (params) {
            cJSON *events = cJSON_GetObjectItem(params, "events");
            if (events && cJSON_IsArray(events)) {
                int n = cJSON_GetArraySize(events);
                for (int i = 0; i < n; i++) {
                    cJSON *ev = cJSON_GetArrayItem(events, i);
                    if (!ev) continue;
                    cJSON *label = cJSON_GetObjectItem(ev, "event");
                    if (!label || !cJSON_IsString(label)) continue;
                    if (strcmp(label->valuestring, "MaskOff") == 0) {
                        cJSON *rt = cJSON_GetObjectItem(ev, "reportTime");
                        if (rt && cJSON_IsString(rt)) {
                            mask_off_ms = parse_iso8601_utc_ms(rt->valuestring);
                        }
                    }
                }
            }
        }
        cJSON_Delete(msg);
    }
    fclose(f);
    return mask_off_ms;
}

/* Convert one .snt file to an EDF file.
 *
 * Parameters:
 *   snt_path    - path to .snt file
 *   edf_path    - output EDF path
 *   patient_id  - pre-formatted patient ID (will have CRC filled in)
 *   recording_id - pre-formatted recording ID
 *   start_date  - "dd.mm.yy"
 *   start_time  - "hh.mm.ss"
 *   signals     - signal definitions (without Crc16)
 *   n_signals   - number of EDF signals
 *   record_dur  - "60.00"
 *   channel_map - array mapping EDF signal index → .snt channel index,
 *                 or NULL if n_signals == snt n_channels (1:1 mapping)
 *   skip_samples - number of leading samples to skip (per channel), used
 *                 to align EDF start to MaskOn instead of TherapyStart.
 *                 0 = start from the beginning of the .snt file.
 *   max_samples  - maximum number of samples (per channel) to include
 *                 after skipping, used to truncate the end to MaskOff.
 *                 0 = use all remaining samples (no end truncation).
 */
static esp_err_t convert_snt_to_edf(const char *snt_path, const char *edf_path,
                                    const char *patient_id, const char *recording_id,
                                    const char *start_date, const char *start_time,
                                    const edf_signal_def_t *signals, int n_signals,
                                    const char *record_dur,
                                    const int *channel_map,
                                    uint32_t skip_samples,
                                    uint32_t max_samples)
{
    FILE *snt = fopen(snt_path, "rb");
    if (!snt) {
        ESP_LOGW(TAG, "cannot open %s: %s", snt_path, strerror(errno));
        return ESP_FAIL;
    }

    snt_header_t hdr;
    if (snt_read_header(snt, &hdr) != ESP_OK) {
        fclose(snt);
        return ESP_FAIL;
    }

    /* Validate channel mapping.
     * If channel_map is provided, .snt n_channels must be >= max mapped index+1.
     * If channel_map is NULL, require .snt n_channels == n_signals (1:1). */
    int snt_channels = hdr.n_channels;
    if (channel_map) {
        for (int i = 0; i < n_signals; i++) {
            if (channel_map[i] >= snt_channels) {
                ESP_LOGE(TAG, "%s: channel_map[%d]=%d >= snt n_channels=%d",
                         snt_path, i, channel_map[i], snt_channels);
                fclose(snt);
                return ESP_FAIL;
            }
        }
    } else if (snt_channels != n_signals) {
        ESP_LOGE(TAG, "%s: channel count mismatch: snt=%d edf=%d",
                 snt_path, snt_channels, n_signals);
        fclose(snt);
        return ESP_FAIL;
    }

    /* Calculate samples per record per signal from the .snt capture rate.
     * The .snt rate now matches the EDF rate (SA2 is decimated at capture time). */
    int hz_x10 = hdr.sample_hz_x10;
    int *spr = malloc(n_signals * sizeof(int));
    edf_signal_def_t *sig = malloc(n_signals * sizeof(edf_signal_def_t));
    if (!spr || !sig) {
        ESP_LOGE(TAG, "malloc spr/sig failed");
        free(spr); free(sig);
        fclose(snt);
        return ESP_ERR_NO_MEM;
    }
    for (int i = 0; i < n_signals; i++) {
        spr[i] = (hz_x10 * 60) / 10;
        sig[i] = signals[i];
        sig[i].samples_per_record = spr[i];
    }

    /* Total samples (per channel) after skipping the leading offset.
     *
     * EDF records are 60 seconds each.  Use floor division so the partial
     * last record is dropped, matching AS11 behaviour (AS11 does not
     * zero-pad trailing partial records). */
    uint32_t total_samples = hdr.sample_count;
    if (skip_samples > total_samples) {
        ESP_LOGW(TAG, "%s: skip_samples (%u) > sample_count (%u), clamping",
                 snt_path, skip_samples, total_samples);
        skip_samples = total_samples;
    }
    total_samples -= skip_samples;
    /* Truncate the end to MaskOff if max_samples is specified.
     * AS11 EDF data spans MaskOn→MaskOff, but .snt captures TherapyStart→
     * TherapyStop.  Without this, the EDF includes ~53-57s of post-MaskOff
     * data (mask-off ramp-down before TherapyStop), which can create an
     * extra 60-second record that AS11 doesn't have. */
    if (max_samples > 0 && max_samples < total_samples) {
        ESP_LOGI(TAG, "%s: truncating %u → %u samples (MaskOff end-trim)",
                 snt_path, total_samples, max_samples);
        total_samples = max_samples;
    }
    int total_records = (int)(total_samples / spr[0]);
    if (total_records < 0) total_records = 0;

    /* AS11 writes header-only (0-record) BRP/PLD/SA2 EDF files for sessions
     * shorter than one data record (60 seconds).  Write the header with
     * num_records=0 to match, rather than skipping file creation. */
    if (total_records == 0) {
        ESP_LOGI(TAG, "%s: short session (%u samples < %d spr), writing header-only EDF",
                 snt_path, total_samples, spr[0]);
        char tmp_path[380];
        FILE *edf = open_atomic_file(edf_path, tmp_path, sizeof(tmp_path));
        if (!edf) {
            ESP_LOGE(TAG, "cannot create %s: %s", edf_path, strerror(errno));
            free(spr); free(sig);
            fclose(snt);
            return ESP_FAIL;
        }
        if (edf_write_header(edf, patient_id, recording_id,
                             start_date, start_time,
                             0, record_dur, "EDF", sig, n_signals) < 0 ||
            finalize_atomic_file(edf, tmp_path, edf_path) != ESP_OK) {
            ESP_LOGE(TAG, "cannot write %s: %s", edf_path, strerror(errno));
            free(spr); free(sig);
            fclose(snt);
            return ESP_FAIL;
        }
        free(spr); free(sig);
        fclose(snt);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "converting %s → %s: %u samples (skip %u), %d records, %d ch",
             snt_path, edf_path, total_samples, skip_samples, total_records, n_signals);

    /* Seek past skipped leading samples.
     * .snt data starts at offset sizeof(snt_header_t) and is interleaved
     * as [ch0_s0, ch1_s0, ch0_s1, ch1_s1, ...], so each sample frame is
     * snt_channels × sizeof(int16_t) bytes. */
    if (skip_samples > 0) {
        long skip_bytes = (long)skip_samples * snt_channels * sizeof(int16_t);
        if (fseek(snt, sizeof(snt_header_t) + skip_bytes, SEEK_SET) != 0) {
            ESP_LOGW(TAG, "%s: fseek skip failed, starting from beginning", snt_path);
            fseek(snt, sizeof(snt_header_t), SEEK_SET);
        }
    }

    /* Create EDF file and write header */
    char tmp_path[380];
    FILE *edf = open_atomic_file(edf_path, tmp_path, sizeof(tmp_path));
    if (!edf) {
        ESP_LOGE(TAG, "cannot create %s: %s", edf_path, strerror(errno));
        fclose(snt);
        return ESP_FAIL;
    }

    int header_bytes = edf_write_header(edf, patient_id, recording_id,
                                        start_date, start_time,
                                        total_records, record_dur,
                                        "EDF", sig, n_signals);
    if (header_bytes < 0) {
        ESP_LOGE(TAG, "edf_write_header failed for %s", edf_path);
        free(spr); free(sig);
        discard_atomic_file(edf, tmp_path);
        fclose(snt);
        return ESP_FAIL;
    }

    /* Read .snt data and write EDF records.
     * Each EDF record = 60 seconds of data.
     * .snt format stores interleaved int16: [ch0_s0, ch1_s0, ch0_s1, ch1_s1, ...]
     * EDF format stores all samples for signal 0, then all for signal 1, etc.
     * Then Crc16 (CRC16-CCITT-FALSE of the record's signal data, as int16).
     *
     * Optimisation: allocate raw + de-interleaved buffers once (outside the
     * loop) to avoid per-record malloc/free churn on the heap. */
    int samples_per_record = spr[0];  /* same for all signals in our files */
    int record_data_samples = samples_per_record * n_signals;
    size_t record_bytes = record_data_samples * sizeof(int16_t);

    /* Raw buffer must hold snt_channels per sample (interleaved) */
    size_t raw_record_bytes = samples_per_record * snt_channels * sizeof(int16_t);
    int16_t *raw = malloc(raw_record_bytes);          /* interleaved input */
    /* record_buf is written to the SD card via fwrite.  It MUST live in
     * PSRAM: the ESP32-S3 SDMMC driver mishandles direct multi-sector DMA
     * writes from internal RAM (the full-sector chunk is silently written
     * as zeros), whereas PSRAM source buffers are bounced through an aligned
     * internal buffer and write correctly.  Without this, any EDF data
     * region larger than one sector (e.g. the 6000-byte BRP record) is
     * zeroed on disk except for the trailing partial sector.  Fall back to
     * internal RAM if PSRAM is unavailable. */
    int16_t *record_buf = heap_caps_malloc(record_bytes, MALLOC_CAP_SPIRAM);
    if (!record_buf) {
        record_buf = malloc(record_bytes);
    }
    if (!raw || !record_buf) {
        ESP_LOGE(TAG, "malloc record buffers failed");
        free(raw); free(record_buf); free(spr); free(sig);
        discard_atomic_file(edf, tmp_path);
        fclose(snt);
        return ESP_ERR_NO_MEM;
    }

    for (int rec = 0; rec < total_records; rec++) {
        /* Read one record's worth of interleaved samples from .snt.
         * With floor division the last record is always complete, but
         * the partial-record handling is retained as a safety net. */
        memset(raw, 0, raw_record_bytes);
        size_t avail = raw_record_bytes;
        /* For the last record, only read what's available */
        if (rec == total_records - 1) {
            uint32_t remaining = total_samples - (uint32_t)rec * spr[0];
            if (remaining < (uint32_t)samples_per_record) {
                avail = remaining * snt_channels * sizeof(int16_t);
            }
        }
        if (avail > 0) {
            if (fread(raw, 1, avail, snt) != avail) {
                ESP_LOGW(TAG, "short read at record %d", rec);
                free(raw); free(record_buf); free(spr); free(sig);
                discard_atomic_file(edf, tmp_path);
                fclose(snt);
                return ESP_FAIL;
            }
        }

        /* De-interleave: [ch0_s0, ch1_s0, ...] → [ch0_all, ch1_all, ...]
         * Samples beyond available data remain zero (from memset above).
         * If channel_map is provided, select only the mapped channels.
         *
         * Re-scale from the capture scale (physical × 100) to each signal's
         * EDF digital scale.  The AS11 digital scale differs per signal
         * (dig_max/phys_max), so the raw ×100 value cannot be written as the
         * digital value directly:
         *   dig = dig_min + (phys - phys_min) × (dig_max-dig_min)/(phys_max-phys_min)
         * where phys = stored / 100.
         *
         * The ×100 BLE scale was confirmed by reverse-engineering (2026-07-05):
         * raw .snt int16 / 100.0 matches AS11 EDF physical values for all PLD
         * channels.  Residual differences are BLE quantisation, not a scale
         * error — see the PLD.edf comment above and spec/archive/
         * edf-as11-comparison-20260629.md §3.6. */
        int avail_samples = (int)(avail / (snt_channels * sizeof(int16_t)));
        for (int ch = 0; ch < n_signals; ch++) {
            int snt_ch = channel_map ? channel_map[ch] : ch;
            double pmin = sig[ch].phys_min;
            double pspan = sig[ch].phys_max - sig[ch].phys_min;
            double dmin = sig[ch].dig_min;
            double dspan = sig[ch].dig_max - sig[ch].dig_min;
            double k = (pspan != 0.0) ? (dspan / pspan) : 0.0;
            bool passthrough = sig[ch].invalid_passthrough;
            for (int s = 0; s < samples_per_record; s++) {
                if (s < avail_samples) {
                    int16_t stored = raw[s * snt_channels + snt_ch];
                    /* AS11 marks "no data" samples with the invalid marker
                     * (-1) rather than scaling them.  Pass the sentinel
                     * through verbatim for signals that opt in. */
                    if (passthrough && stored == -1) {
                        record_buf[ch * samples_per_record + s] = -1;
                        continue;
                    }
                    double phys = stored / 100.0;
                    double dig = dmin + (phys - pmin) * k;
                    int idig = (int)(dig < 0 ? dig - 0.5 : dig + 0.5);
                    if (idig > INT16_MAX) idig = INT16_MAX;
                    if (idig < INT16_MIN) idig = INT16_MIN;
                    /* Clamp to the signal's valid digital range when not
                     * in invalid-passthrough mode.  This prevents the -1
                     * BLE sentinel ("no data yet") from leaking through the
                     * scaling for signals whose k factor maps -1/100 back to
                     * -1 (e.g. MaskPress with k=50 → -0.5 → rounds to -1).
                     * Signals that use the -1 sentinel (FlowLim, SA2 SpO2/Pulse)
                     * have invalid_passthrough=true and bypass this clamp. */
                    if (!passthrough) {
                        if (idig > sig[ch].dig_max) idig = sig[ch].dig_max;
                        if (idig < sig[ch].dig_min) idig = sig[ch].dig_min;
                    }
                    record_buf[ch * samples_per_record + s] = (int16_t)idig;
                } else {
                    /* Pad trailing samples of a partial record.  For
                     * invalid-passthrough signals use the -1 marker to match
                     * AS11; otherwise zero-pad. */
                    record_buf[ch * samples_per_record + s] = passthrough ? -1 : 0;
                }
            }
        }

        /* Write de-interleaved signal data to EDF */
        if (!write_all(edf, record_buf, record_bytes)) {
            free(raw); free(record_buf); free(spr); free(sig);
            discard_atomic_file(edf, tmp_path);
            fclose(snt);
            return ESP_FAIL;
        }

        /* Compute CRC16 over the signal data bytes and write as int16 */
        uint16_t crc = crc16_ccitt((uint8_t *)record_buf, record_bytes);
        int16_t crc_val = (int16_t)crc;
        if (!write_all(edf, &crc_val, sizeof(crc_val))) {
            free(raw); free(record_buf); free(spr); free(sig);
            discard_atomic_file(edf, tmp_path);
            fclose(snt);
            return ESP_FAIL;
        }
    }

    free(raw);
    free(record_buf);
    free(spr);
    free(sig);

    if (finalize_atomic_file(edf, tmp_path, edf_path) != ESP_OK) {
        ESP_LOGE(TAG, "cannot finalize %s: %s", edf_path, strerror(errno));
        fclose(snt);
        return ESP_FAIL;
    }
    fclose(snt);
    ESP_LOGI(TAG, "EDF conversion complete: %s", edf_path);
    return ESP_OK;
}

/* Forward declarations — defined in Section 9. */
static cJSON *read_json_file(const char *path);
static uint8_t *read_bin_file(const char *path, size_t *out_len);
/* Forward declaration — defined in Section 10. */
static void noon_day_folder(int64_t epoch_ms, char *out, size_t out_len);

/* ════════════════════════════════════════════════════════════════════
 *  Section 6: STR.edf generation from Summary spool protobuf
 * ════════════════════════════════════════════════════════════════════
 *
 * The Summary spool contains one or more protobuf records (wrapped in
 * field-2 messages).  Each record contains session statistics and settings
 * that map to the 134-field STR.edf format.
 *
 * The STR.edf has 1 data record per 86400 seconds (1 day), containing
 * 134 int16 signal values + 1 Crc16 value.
 *
 * Field mapping is based on the _SUMMARY_FIELDS table in as11_spool.py
 * and the STR signal reference in edf_signals.md.
 */

/* Summary protobuf field numbers (from as11_spool.py _SUMMARY_FIELDS) */
#define SUM_F_PERIOD_START    2
#define SUM_F_PERIOD_END      3
#define SUM_F_TZ_OFFSET       4
#define SUM_F_DURATION_MIN    5
#define SUM_F_SESSION_MODE    6
#define SUM_F_AHI             7
#define SUM_F_AI              8
#define SUM_F_HI              9
#define SUM_F_OAI             10
#define SUM_F_CAI             11
#define SUM_F_UAI             12
#define SUM_F_RIN             13
#define SUM_F_LEAK            14
#define SUM_F_INSP_PRESS      15
#define SUM_F_CSR             16
#define SUM_F_SAU             17
#define SUM_F_SPONT_TRIG      18
#define SUM_F_SPONT_CYC       19
#define SUM_F_EXP_PRESS       20
#define SUM_F_MEAN_MASK_PRESS 21
#define SUM_F_TIDAL_VOL       22
#define SUM_F_MIN_VENT        23
#define SUM_F_TGT_VENT        24
#define SUM_F_RESP_RATE       25
#define SUM_F_INSP_DUR        26
#define SUM_F_IE_RATIO        27
#define SUM_F_SPO2            28
#define SUM_F_AMB_HUMID       29
#define SUM_F_HUM_TEMP        30
#define SUM_F_HTUBE_TEMP      31
#define SUM_F_HUM_POWER       32
#define SUM_F_HTUBE_POWER     33
#define SUM_F_HUM_CONNECTED   34
#define SUM_F_TUBE_CONNECTED  35
#define SUM_F_BLOWER_PRESS    36
#define SUM_F_RESP_FLOW       37
#define SUM_F_BLOWER_FLOW     38
#define SUM_F_SESSION_COUNT   39
#define SUM_F_CLOCK_B         40
#define SUM_F_HEART_RATE      41

/* Sub-field percentile indices for metric submessages.
 * Sub-field 2 = 50th percentile, 3 = 95th (or 70th for Leak), 4 = 100th (or 95th).
 * The exact mapping depends on the field — see _SUMMARY_SUBFIELDS in as11_spool.py. */

/* STR.edf signal count for VID=3 (AutoSet):
 * 77 data signals + 1 Crc16 = 78 total.
 * Non-AutoSet variants would have additional signals (VAuto/Spont/ST/ASV settings,
 * SpontTrig/Cyc, TgtVent/IERatio/Ti stats) — see edf_signals.md variant provenance. */
#define STR_DATA_COUNT    77
#define STR_SIGNAL_COUNT  78   /* includes Crc16 */

/* STR enum export maps — some fields are remapped before writing to EDF.
 * From edf_signals.md "STR enum export maps" section. */
static const int MODE_MAP[] = {3, 1, 2, 4, 10, 16, 8, 6, 7, 5, 9};

/* Context for protobuf iteration: collects all field values from a
 * Summary record into a key-value store. */
typedef struct {
    int64_t scalars[64];       /* varint fields by field number */
    bool has_scalar[64];
    /* For metric submessages (length-delimited), store up to 4 sub-values */
    struct {
        int64_t val[8];
        int n;
    } metrics[64];
    bool has_metric[64];
    /* Session entries from Summary spool field 6 (SessionModeEntries).
     * Each entry has: sub-field 1 = MaskOn timestamp (epoch ms),
     * sub-field 2 = per-session duration in minutes (NOT therapy mode). */
    struct {
        int64_t ts;            /* sub-field 1: MaskOn timestamp (epoch ms) */
        int64_t duration_min;  /* sub-field 2: per-session duration (minutes) */
    } session_entries[20];
    int n_session_entries;
} summary_ctx_t;

/* Callback for pb_iter on a Summary record. */
static void summary_field_cb(const pb_field_t *f, void *ud)
{
    summary_ctx_t *ctx = (summary_ctx_t *)ud;
    if (f->field < 1 || f->field > 63) return;

    if (f->wire == 0) {
        /* Varint field — decode the value */
        ctx->scalars[f->field] = pb_varint_val(f);
        ctx->has_scalar[f->field] = true;
    } else if (f->field == SUM_F_SESSION_MODE && f->wire == 2 && f->data && f->len > 0) {
        /* SessionModeEntries (field 6): repeated wrapper submessages.
         * Each wrapper (sub-field 1, wire 2) contains:
         *   sub-sub-field 1 (varint) = MaskOn timestamp (epoch ms)
         *   sub-sub-field 2 (varint) = per-session duration (minutes) */
        size_t pos = 0;
        while (pos < f->len && ctx->n_session_entries < 20) {
            uint64_t tag = pb_decode_varint(f->data, f->len, &pos);
            int sf = (int)(tag >> 3);
            int sw = (int)(tag & 0x07);
            if (sf == 0) break;
            if (sw == 2 && sf == 1) {
                /* Wrapper submessage for one session entry */
                size_t lpos = pos;
                uint64_t flen = pb_decode_varint(f->data, f->len, &lpos);
                const uint8_t *inner = f->data + lpos;
                size_t inner_len = (size_t)flen;
                pos = lpos + flen;
                int64_t ts = 0, duration_min = 0;
                size_t ip = 0;
                while (ip < inner_len) {
                    uint64_t itag = pb_decode_varint(inner, inner_len, &ip);
                    int isf = (int)(itag >> 3);
                    int isw = (int)(itag & 0x07);
                    if (isf == 0) break;
                    if (isw == 0) {
                        int64_t val = (int64_t)pb_decode_varint(inner, inner_len, &ip);
                        if (isf == 1) ts = val;
                        else if (isf == 2) duration_min = val;
                    } else if (isw == 2) {
                        size_t ilpos = ip;
                        uint64_t ilen = pb_decode_varint(inner, inner_len, &ilpos);
                        ip = ilpos + ilen;
                    } else if (isw == 1) {
                        ip += 8;
                    } else if (isw == 5) {
                        ip += 4;
                    } else {
                        break;
                    }
                }
                ctx->session_entries[ctx->n_session_entries].ts = ts;
                ctx->session_entries[ctx->n_session_entries].duration_min = duration_min;
                ctx->n_session_entries++;
            } else if (sw == 2) {
                size_t lpos = pos;
                uint64_t flen = pb_decode_varint(f->data, f->len, &lpos);
                pos = lpos + flen;
            } else if (sw == 0) {
                (void)pb_decode_varint(f->data, f->len, &pos);
            } else if (sw == 1) {
                pos += 8;
            } else if (sw == 5) {
                pos += 4;
            } else {
                break;
            }
        }
    } else if (f->wire == 2 && f->data && f->len > 0) {
        /* Length-delimited — could be a metric submessage or raw bytes.
         * Try to decode as a protobuf submessage with varint sub-fields. */
        size_t pos = 0;
        int n = 0;
        while (pos < f->len && n < 8) {
            uint64_t tag = pb_decode_varint(f->data, f->len, &pos);
            int sub_field = (int)(tag >> 3);
            int sub_wire = (int)(tag & 0x07);
            if (sub_field == 0) break;
            if (sub_wire == 0) {
                ctx->metrics[f->field].val[sub_field] =
                    (int64_t)pb_decode_varint(f->data, f->len, &pos);
            } else if (sub_wire == 2) {
                size_t lpos = pos;
                uint64_t flen = pb_decode_varint(f->data, f->len, &lpos);
                pos = lpos + flen;
            } else if (sub_wire == 1) {
                pos += 8;
            } else if (sub_wire == 5) {
                pos += 4;
            } else {
                break;
            }
            if (sub_wire == 0) n++;
        }
        ctx->metrics[f->field].n = n;
        ctx->has_metric[f->field] = true;
    }
}

/* Get a metric sub-value (percentile) from the context.
 * sub_idx: 2=50th, 3=95th/70th, 4=100th/95th, 1=5th */
static int16_t get_metric(const summary_ctx_t *ctx, int field, int sub_idx,
                          int16_t default_val)
{
    if (field < 1 || field > 63 || !ctx->has_metric[field]) return default_val;
    if (sub_idx < 0 || sub_idx >= 8) return default_val;
    /* Check if the sub-field was populated (val array is sparse) */
    /* We use a simple heuristic: if n > 0 and sub_idx was seen */
    /* Actually, we need to track which sub-fields were seen.
     * For now, use the val directly — if it's 0 and n is small,
     * it might not have been set.  This is a simplification. */
    return (int16_t)ctx->metrics[field].val[sub_idx];
}

/* Get a scalar value from the context. */
static int16_t get_scalar(const summary_ctx_t *ctx, int field, int16_t default_val)
{
    if (field < 1 || field > 63 || !ctx->has_scalar[field]) return default_val;
    return (int16_t)ctx->scalars[field];
}

/* Map On/Off string from settings.json to AS11 EDF enum value.
 * AS11 EDF uses: Off=1, On=2 for boolean-like settings. */
static int on_off_to_edf(const char *s)
{
    if (!s) return -1;
    if (strcmp(s, "On") == 0) return 2;
    if (strcmp(s, "Off") == 0) return 1;
    return -1;
}

/* Map ActiveTherapyProfile name from settings.json to the MOP enum index
 * used by MODE_MAP.  The AS11 STR.edf Mode field is sourced from the
 * ActiveTherapyProfile (MOP setting), not from SessionModeEntries in the
 * Summary spool (which contain unreliable values).  See edf_signals.md
 * "STR enum export maps" and resmed_config.py ENUM_OPTIONS['MOP']. */
static int profile_name_to_mop(const char *name)
{
    if (!name) return -1;
    if (strcmp(name, "CpapProfile") == 0)       return 0;  /* CPAP      */
    if (strcmp(name, "AutoSetProfile") == 0)    return 1;  /* AutoSet   */
    if (strcmp(name, "SpontProfile") == 0)      return 3;  /* S         */
    if (strcmp(name, "STProfile") == 0)         return 4;  /* ST        */
    if (strcmp(name, "TimedProfile") == 0)      return 5;  /* T         */
    if (strcmp(name, "VAutoProfile") == 0)      return 6;  /* VAuto     */
    if (strcmp(name, "ASVProfile") == 0)        return 7;  /* ASV       */
    if (strcmp(name, "ASVAutoProfile") == 0)    return 8;  /* ASVAuto   */
    if (strcmp(name, "AutoSetForHerProfile") == 0) return 11; /* AutoSet Her */
    return -1;
}

/* Convert a raw spool value to the EDF digital value by dividing by the
 * field's "logical scale".  The AS11 stores summary metrics in the protobuf
 * spool as fixed-point integers; its firmware STR writer divides each by a
 * field-specific logical_scale before writing the EDF digital value.
 *
 * We replicate that here using integer arithmetic: the logical_scale is
 * expressed as a fraction (den / num), so the conversion is
 *   edf_digital = raw * num / den
 * Returns -1 (sentinel) unchanged when raw is -1.
 *
 * Logical scales (from as11_edf_superset.py STR_SUPERSET_METADATA):
 *   Pressure (cmH2O)     logical_scale = 2     → num=1, den=2
 *   Flow (L/s)           logical_scale = 0.2   → num=5, den=1
 *   Humidity/Temp/Power  logical_scale = 10    → num=1, den=10
 *   SpO2 (%)             logical_scale = 100   → num=1, den=100
 *   Minute Ventilation   logical_scale = 12.5  → num=2, den=25
 *   Respiratory Rate     logical_scale = 20    → num=1, den=20
 *   Tidal Volume         logical_scale = 2     → num=1, den=2
 *   Indices (AHI etc)    logical_scale = 10    → num=1, den=10
 *   Duration/enums/SAU   logical_scale = 1     → no conversion needed
 */
static int16_t spool_to_edf(int16_t raw, int num, int den)
{
    if (raw == -1) return -1;
    return (int16_t)((int32_t)raw * num / den);
}

/* Build STR data values [4-76] from a summary context and settings JSON.
 * str_values must be pre-filled with 0xFF (sentinel for "no data").
 *
 * Spool-derived stat fields [33-76] are raw fixed-point integers from the
 * protobuf Summary record.  Each is divided by its logical_scale (via
 * spool_to_edf) to produce the EDF digital value that the AS11 firmware
 * would write.  Settings fields [6-30] come from the Get RPC response
 * (settings.json) and are already in EDF digital units (e.g. cmH2O × 50). */
static void build_str_data_values(summary_ctx_t *ctx, int16_t *str_values,
                                  const cJSON *settings_json)
{
    /* Session core [4-5] — logical_scale = 1, no conversion needed */
    str_values[4] = get_scalar(ctx, SUM_F_DURATION_MIN, 0);  /* Duration */

    /* Mode [5]: derived from ActiveTherapyProfile in settings.json.
     * The AS11's own export uses the MOP setting (ActiveTherapyProfile),
     * not the SessionModeEntries from the Summary spool, which contain
     * unreliable values.  See edf_signals.md "STR enum export maps". */
    int mode_raw = -1;
    if (settings_json) {
        cJSON *sp = cJSON_GetObjectItem(settings_json, "SettingProfiles");
        cJSON *ap = sp ? cJSON_GetObjectItem(sp, "ActiveProfiles") : NULL;
        cJSON *tp_name = ap ? cJSON_GetObjectItem(ap, "TherapyProfile") : NULL;
        if (tp_name && cJSON_IsString(tp_name))
            mode_raw = profile_name_to_mop(tp_name->valuestring);
    }
    if (mode_raw >= 0 && mode_raw < (int)(sizeof(MODE_MAP) / sizeof(MODE_MAP[0]))) {
        str_values[5] = MODE_MAP[mode_raw];
    } else if (mode_raw == 11) {
        /* AutoSet Her: not in standard MODE_MAP (which has 11 entries for
         * indices 0-10).  Use the MOP enum value directly as the EDF value. */
        str_values[5] = 11;
    } else {
        str_values[5] = -1;  /* unknown — leave sentinel */
    }

    /* CPAP/AutoSet settings [6-13] and common comfort/settings [14-30]:
     * from settings.json (Get RPC response captured during post-therapy).
     * Pressures are stored in cmH2O × 50, temperatures in °C × 10.
     * Enum fields use AS11 EDF enum values (Off=1, On=2, etc.). */
    if (settings_json) {
        cJSON *sp = cJSON_GetObjectItem(settings_json, "SettingProfiles");
        cJSON *tp = sp ? cJSON_GetObjectItem(sp, "TherapyProfiles") : NULL;
        cJSON *fp = sp ? cJSON_GetObjectItem(sp, "FeatureProfiles") : NULL;

        /* Pressure fields [6-13]: cmH2O × 50 */
        if (tp) {
            cJSON *cpap = cJSON_GetObjectItem(tp, "CpapProfile");
            cJSON *autoset = cJSON_GetObjectItem(tp, "AutoSetProfile");
            cJSON *her = cJSON_GetObjectItem(tp, "AutoSetForHerProfile");
            cJSON *v;
            if (cpap) {
                if ((v = cJSON_GetObjectItem(cpap, "StartPressure")) && cJSON_IsNumber(v))
                    str_values[6] = (int16_t)(v->valuedouble * 50);
                if ((v = cJSON_GetObjectItem(cpap, "SetPressure")) && cJSON_IsNumber(v))
                    str_values[7] = (int16_t)(v->valuedouble * 50);
            }
            if (autoset) {
                if ((v = cJSON_GetObjectItem(autoset, "StartPressure")) && cJSON_IsNumber(v))
                    str_values[8] = (int16_t)(v->valuedouble * 50);
                if ((v = cJSON_GetObjectItem(autoset, "MaxPressure")) && cJSON_IsNumber(v))
                    str_values[9] = (int16_t)(v->valuedouble * 50);
                if ((v = cJSON_GetObjectItem(autoset, "MinPressure")) && cJSON_IsNumber(v))
                    str_values[10] = (int16_t)(v->valuedouble * 50);
            }
            if (her) {
                if ((v = cJSON_GetObjectItem(her, "StartPressure")) && cJSON_IsNumber(v))
                    str_values[11] = (int16_t)(v->valuedouble * 50);
                if ((v = cJSON_GetObjectItem(her, "MaxPressure")) && cJSON_IsNumber(v))
                    str_values[12] = (int16_t)(v->valuedouble * 50);
                if ((v = cJSON_GetObjectItem(her, "MinPressure")) && cJSON_IsNumber(v))
                    str_values[13] = (int16_t)(v->valuedouble * 50);
            }
        }

        /* Comfort/settings [14-30] */
        if (fp) {
            cJSON *comfort = cJSON_GetObjectItem(fp, "ComfortFeature");
            cJSON *epr = cJSON_GetObjectItem(fp, "EprFeature");
            cJSON *ramp = cJSON_GetObjectItem(fp, "AutoRampFeature");
            cJSON *smart = cJSON_GetObjectItem(fp, "SmartStartStopFeature");
            cJSON *circuit = cJSON_GetObjectItem(fp, "CircuitFeature");
            cJSON *climate = cJSON_GetObjectItem(fp, "ClimateFeature");
            cJSON *patview = cJSON_GetObjectItem(fp, "PatientViewFeature");
            cJSON *v;

            if (comfort) {
                v = cJSON_GetObjectItem(comfort, "AutoSetComfort");
                if (v && cJSON_IsString(v)) {
                    if (strcmp(v->valuestring, "On") == 0) str_values[14] = 2;
                    else if (strcmp(v->valuestring, "Plus") == 0) str_values[14] = 1;
                }
            }

            if (ramp) {
                v = cJSON_GetObjectItem(ramp, "RampEnable");
                if (v && cJSON_IsString(v)) {
                    if (strcmp(v->valuestring, "Off") == 0) str_values[15] = 1;
                    else if (strcmp(v->valuestring, "On") == 0) str_values[15] = 2;
                    else if (strcmp(v->valuestring, "Auto") == 0) str_values[15] = 3;
                }
                v = cJSON_GetObjectItem(ramp, "RampTime");
                if (v && cJSON_IsNumber(v)) str_values[16] = (int16_t)v->valuedouble;
            }

            if (epr) {
                v = cJSON_GetObjectItem(epr, "EprEnablePatientAccess");
                if (v && cJSON_IsString(v)) str_values[17] = (int16_t)on_off_to_edf(v->valuestring);
                v = cJSON_GetObjectItem(epr, "EprEnable");
                if (v && cJSON_IsString(v)) str_values[18] = (int16_t)on_off_to_edf(v->valuestring);
                v = cJSON_GetObjectItem(epr, "EprPressure");
                if (v && cJSON_IsNumber(v)) str_values[19] = (int16_t)(v->valuedouble * 50);
                v = cJSON_GetObjectItem(epr, "EprType");
                if (v && cJSON_IsString(v)) {
                    if (strcmp(v->valuestring, "RampOnly") == 0) str_values[20] = 1;
                    else if (strcmp(v->valuestring, "FullTime") == 0) str_values[20] = 2;
                }
            }

            if (smart) {
                v = cJSON_GetObjectItem(smart, "SmartStart");
                if (v && cJSON_IsString(v)) str_values[21] = (int16_t)on_off_to_edf(v->valuestring);
            }

            if (patview) {
                v = cJSON_GetObjectItem(patview, "PatientView");
                if (v && cJSON_IsString(v)) {
                    if (strcmp(v->valuestring, "Advanced") == 0) str_values[22] = 1;
                    else if (strcmp(v->valuestring, "Basic") == 0) str_values[22] = 2;
                }
            }

            if (circuit) {
                v = cJSON_GetObjectItem(circuit, "AntiBacterialFilter");
                if (v && cJSON_IsString(v)) {
                    if (strcmp(v->valuestring, "No") == 0) str_values[23] = 1;
                    else if (strcmp(v->valuestring, "Yes") == 0) str_values[23] = 2;
                }
                v = cJSON_GetObjectItem(circuit, "MaskType");
                if (v && cJSON_IsString(v)) {
                    if (strcmp(v->valuestring, "Nasal") == 0) str_values[24] = 1;
                    else if (strcmp(v->valuestring, "Pillows") == 0) str_values[24] = 2;
                    else if (strcmp(v->valuestring, "FullFace") == 0 ||
                             strcmp(v->valuestring, "Full Face") == 0) str_values[24] = 3;
                    else if (strcmp(v->valuestring, "Pediatric") == 0) str_values[24] = 4;
                }
                v = cJSON_GetObjectItem(circuit, "TubeType");
                if (v && cJSON_IsString(v)) {
                    if (strcmp(v->valuestring, "SlimLine") == 0) str_values[25] = 1;
                    else if (strcmp(v->valuestring, "Standard") == 0) str_values[25] = 2;
                    else if (strcmp(v->valuestring, "3m") == 0) str_values[25] = 3;
                    else if (strcmp(v->valuestring, "19mmNonHeated") == 0) str_values[25] = 4;
                }
            }

            if (climate) {
                v = cJSON_GetObjectItem(climate, "ClimateControl");
                if (v && cJSON_IsString(v)) {
                    if (strcmp(v->valuestring, "Auto") == 0) str_values[26] = 1;
                    else if (strcmp(v->valuestring, "Manual") == 0) str_values[26] = 2;
                }
                v = cJSON_GetObjectItem(climate, "HumidifierSettingEnable");
                if (v && cJSON_IsString(v)) str_values[27] = (int16_t)on_off_to_edf(v->valuestring);
                v = cJSON_GetObjectItem(climate, "HumidifierLevel");
                if (v && cJSON_IsNumber(v)) str_values[28] = (int16_t)v->valuedouble;
                v = cJSON_GetObjectItem(climate, "HeatedTubeSettingEnable");
                if (v && cJSON_IsString(v)) str_values[29] = (int16_t)on_off_to_edf(v->valuestring);
                v = cJSON_GetObjectItem(climate, "HeatedTubeTemperature");
                if (v && cJSON_IsNumber(v)) str_values[30] = (int16_t)(v->valuedouble * 10);
            }
        }
    }

    /* [31] HeatedTube and [32] Humidifier: enum fields from Summary spool.
     * logical_scale = 1, no conversion needed. */
    str_values[31] = get_scalar(ctx, SUM_F_TUBE_CONNECTED, -1);
    str_values[32] = get_scalar(ctx, SUM_F_HUM_CONNECTED, -1);

    /* Environment and oximetry stats [33-46]
     * Spool values are converted to EDF digital via spool_to_edf().
     * Default -1 (sentinel) when no summary data available. */
    str_values[33] = spool_to_edf(get_metric(ctx, SUM_F_BLOWER_PRESS, 3, -1), 1, 2);   /* BlowPress.95  /2  */
    str_values[34] = spool_to_edf(get_metric(ctx, SUM_F_BLOWER_PRESS, 1, -1), 1, 2);   /* BlowPress.5   /2  */
    str_values[35] = spool_to_edf(get_metric(ctx, SUM_F_RESP_FLOW, 3, -1), 5, 1);     /* Flow.95       *5  */
    str_values[36] = spool_to_edf(get_metric(ctx, SUM_F_RESP_FLOW, 1, -1), 5, 1);     /* Flow.5        *5  */
    str_values[37] = spool_to_edf(get_metric(ctx, SUM_F_BLOWER_FLOW, 2, -1), 5, 1);   /* BlowFlow.50   *5  */
    str_values[38] = spool_to_edf(get_metric(ctx, SUM_F_AMB_HUMID, 2, -1), 1, 10);    /* AmbHumidity   /10 */
    str_values[39] = spool_to_edf(get_metric(ctx, SUM_F_HUM_TEMP, 2, -1), 1, 10);     /* HumTemp       /10 */
    str_values[40] = spool_to_edf(get_metric(ctx, SUM_F_HTUBE_TEMP, 2, -1), 1, 10);   /* HTubeTemp     /10 */
    str_values[41] = spool_to_edf(get_metric(ctx, SUM_F_HTUBE_POWER, 2, -1), 1, 10);  /* HTubePow      /10 */
    str_values[42] = spool_to_edf(get_metric(ctx, SUM_F_HUM_POWER, 2, -1), 1, 10);    /* HumPow        /10 */
    str_values[43] = spool_to_edf(get_metric(ctx, SUM_F_SPO2, 2, -1), 1, 100);        /* SpO2.50       /100*/
    str_values[44] = spool_to_edf(get_metric(ctx, SUM_F_SPO2, 3, -1), 1, 100);        /* SpO2.95       /100*/
    str_values[45] = spool_to_edf(get_metric(ctx, SUM_F_SPO2, 4, -1), 1, 100);        /* SpO2.Max      /100*/
    str_values[46] = get_scalar(ctx, SUM_F_SAU, -1);                                  /* SpO2Thresh    no scale */

    /* Bilevel/ventilation summary stats [47-68] */
    str_values[47] = spool_to_edf(get_metric(ctx, SUM_F_MEAN_MASK_PRESS, 2, -1), 1, 2);  /* MaskPress.50 /2 */
    str_values[48] = spool_to_edf(get_metric(ctx, SUM_F_MEAN_MASK_PRESS, 3, -1), 1, 2);  /* MaskPress.95 /2 */
    str_values[49] = spool_to_edf(get_metric(ctx, SUM_F_MEAN_MASK_PRESS, 4, -1), 1, 2);  /* MaskPress.Max /2 */
    str_values[50] = spool_to_edf(get_metric(ctx, SUM_F_INSP_PRESS, 2, -1), 1, 2);       /* TgtIPAP.50   /2 */
    str_values[51] = spool_to_edf(get_metric(ctx, SUM_F_INSP_PRESS, 3, -1), 1, 2);       /* TgtIPAP.95   /2 */
    str_values[52] = spool_to_edf(get_metric(ctx, SUM_F_INSP_PRESS, 4, -1), 1, 2);       /* TgtIPAP.Max  /2 */
    str_values[53] = spool_to_edf(get_metric(ctx, SUM_F_EXP_PRESS, 2, -1), 1, 2);        /* TgtEPAP.50   /2 */
    str_values[54] = spool_to_edf(get_metric(ctx, SUM_F_EXP_PRESS, 3, -1), 1, 2);        /* TgtEPAP.95   /2 */
    str_values[55] = spool_to_edf(get_metric(ctx, SUM_F_EXP_PRESS, 4, -1), 1, 2);        /* TgtEPAP.Max  /2 */
    str_values[56] = spool_to_edf(get_metric(ctx, SUM_F_LEAK, 2, -1), 1, 2);             /* Leak.50      /2 */
    str_values[57] = spool_to_edf(get_metric(ctx, SUM_F_LEAK, 4, -1), 1, 2);             /* Leak.95      /2 */
    str_values[58] = spool_to_edf(get_metric(ctx, SUM_F_LEAK, 3, -1), 1, 2);             /* Leak.70      /2 */
    str_values[59] = spool_to_edf(get_metric(ctx, SUM_F_LEAK, 5, -1), 1, 2);             /* Leak.Max     /2 */
    str_values[60] = spool_to_edf(get_metric(ctx, SUM_F_MIN_VENT, 2, -1), 2, 25);        /* MinVent.50   /12.5 */
    str_values[61] = spool_to_edf(get_metric(ctx, SUM_F_MIN_VENT, 3, -1), 2, 25);        /* MinVent.95   /12.5 */
    str_values[62] = spool_to_edf(get_metric(ctx, SUM_F_MIN_VENT, 4, -1), 2, 25);        /* MinVent.Max  /12.5 */
    str_values[63] = spool_to_edf(get_metric(ctx, SUM_F_RESP_RATE, 2, -1), 1, 20);       /* RespRate.50  /20 */
    str_values[64] = spool_to_edf(get_metric(ctx, SUM_F_RESP_RATE, 3, -1), 1, 20);       /* RespRate.95  /20 */
    str_values[65] = spool_to_edf(get_metric(ctx, SUM_F_RESP_RATE, 4, -1), 1, 20);       /* RespRate.Max /20 */
    str_values[66] = spool_to_edf(get_metric(ctx, SUM_F_TIDAL_VOL, 2, -1), 1, 2);        /* TidVol.50    /2 */
    str_values[67] = spool_to_edf(get_metric(ctx, SUM_F_TIDAL_VOL, 3, -1), 1, 2);        /* TidVol.95    /2 */
    str_values[68] = spool_to_edf(get_metric(ctx, SUM_F_TIDAL_VOL, 4, -1), 1, 2);        /* TidVol.Max   /2 */

    /* Indices [69-75] — logical_scale = 10 (events/hr × 10 in spool).
     * [76] CSR — logical_scale = 1, no conversion. */
    str_values[69] = spool_to_edf(get_scalar(ctx, SUM_F_AHI, -1), 1, 10);
    str_values[70] = spool_to_edf(get_scalar(ctx, SUM_F_HI, -1), 1, 10);
    str_values[71] = spool_to_edf(get_scalar(ctx, SUM_F_AI, -1), 1, 10);
    str_values[72] = spool_to_edf(get_scalar(ctx, SUM_F_OAI, -1), 1, 10);
    str_values[73] = spool_to_edf(get_scalar(ctx, SUM_F_CAI, -1), 1, 10);
    str_values[74] = spool_to_edf(get_scalar(ctx, SUM_F_UAI, -1), 1, 10);
    str_values[75] = spool_to_edf(get_scalar(ctx, SUM_F_RIN, -1), 1, 10);
    str_values[76] = get_scalar(ctx, SUM_F_CSR, -1);
}

/* Build STR header values [0-3] from summary spool session entries.
 * Fills str_values[0-3] and mask_on_extra/mask_off_extra arrays.
 * period_start_ms is the PeriodStart from the summary spool (epoch ms).
 * Returns the MaskEvents count. */
static int build_str_mask_events(summary_ctx_t *ctx, int16_t *str_values,
                                 int16_t *mask_on_extra, int16_t *mask_off_extra,
                                 int64_t period_start_ms, int64_t clock_drift_ms)
{
    /* Summary timestamps are in AS11 time.  STR.edf is deliberately exported
     * in NTP time so its session intervals match the NTP-based EDF headers,
     * filenames, and event annotations. */
    int64_t ntp_period_start_ms = period_start_ms + clock_drift_ms;

    /* [0] Date: days from Unix epoch (noon-based NTP day of PeriodStart) */
    time_t t = (time_t)(ntp_period_start_ms / 1000);
    struct tm tm;
    localtime_r(&t, &tm);
    if (tm.tm_hour < 12) t -= 86400;
    struct tm epoch_tm = { .tm_year = 70, .tm_mon = 0, .tm_mday = 1,
                           .tm_hour = 0, .tm_min = 0, .tm_sec = 0 };
    time_t epoch_t = mktime(&epoch_tm);
    str_values[0] = (int16_t)((t - epoch_t) / 86400);

    int64_t noon_epoch_ms = (int64_t)t * 1000;

    /* [1-3] MaskOn/MaskOff/MaskEvents from session entries.
     * Each session entry has: ts = MaskOn timestamp, duration_min = session
     * duration in minutes. MaskOff = MaskOn + duration_min.
     * The AS11 writes MaskOff = MaskOn (not -1) for 0-duration sessions. */
    int mask_on_count = 0;
    int mask_off_count = 0;

    for (int i = 0; i < ctx->n_session_entries && mask_on_count < 20; i++) {
        int64_t event_ntp_ms = ctx->session_entries[i].ts + clock_drift_ms;
        int min_from_noon = (int)((event_ntp_ms - noon_epoch_ms) / 60000);

        if (mask_on_count == 0)
            str_values[1] = (int16_t)min_from_noon;
        else
            mask_on_extra[mask_on_count - 1] = (int16_t)min_from_noon;
        mask_on_count++;

        /* MaskOff = MaskOn + per-session duration (from spool, verified
         * against AS11 export). 0-duration → MaskOff = MaskOn. */
        int dur = (int)ctx->session_entries[i].duration_min;
        int off_min = min_from_noon + dur;
        if (mask_off_count == 0)
            str_values[2] = (int16_t)off_min;
        else
            mask_off_extra[mask_off_count - 1] = (int16_t)off_min;
        mask_off_count++;
    }

    /* Fallback: use PeriodStart/PeriodEnd if no session entries */
    if (mask_on_count == 0 && ctx->has_scalar[SUM_F_PERIOD_START]) {
        int64_t ps_ntp = ctx->scalars[SUM_F_PERIOD_START] + clock_drift_ms;
        int min_from_noon = (int)((ps_ntp - noon_epoch_ms) / 60000);
        str_values[1] = (int16_t)min_from_noon;
        mask_on_count = 1;
    }
    if (mask_off_count == 0 && ctx->has_scalar[SUM_F_PERIOD_END]) {
        int64_t pe_ntp = ctx->scalars[SUM_F_PERIOD_END] + clock_drift_ms;
        int min_from_noon = (int)((pe_ntp - noon_epoch_ms) / 60000);
        str_values[2] = (int16_t)min_from_noon;
        mask_off_count = 1;
    }

    str_values[3] = (int16_t)(mask_on_count > mask_off_count ? mask_on_count : mask_off_count);
    return str_values[3];
}

/* Per-day STR record for multi-record STR.edf generation. */
typedef struct {
    int16_t values[STR_DATA_COUNT];
    int16_t mask_on_extra[20];
    int16_t mask_off_extra[20];
    int64_t period_start;       /* NTP-corrected — used for data values and sorting */
    int64_t period_start_as11;  /* Raw AS11 clock — used ONLY for noon-day labelling.
                                 * Rationale (audit §5.9): AS11 sets PeriodStart to
                                 * exactly noon in its own clock.  After NTP drift
                                 * correction (~7–8 min) this lands just before noon,
                                 * causing noon_day_folder() to shift every record one
                                 * day back.  Using the raw AS11 value for day labels
                                 * keeps spool-day names aligned with STR records
                                 * while all data timestamps remain NTP-corrected. */
} str_day_record_t;

/* Build a STR record for the current day from session data (events.snt,
 * start/end times, settings).  Used when the Summary spool doesn't yet
 * contain the current (incomplete) day.
 * Fills rec->values, mask_on_extra, mask_off_extra, period_start. */
static void build_current_day_record(str_day_record_t *rec,
                                     const char *session_dir,
                                     const char *session_id,
                                     int64_t start_epoch_ms,
                                     int64_t end_epoch_ms,
                                     int64_t clock_drift_ms,
                                     const cJSON *settings_json)
{
    memset(rec->values, 0xFF, STR_DATA_COUNT * sizeof(int16_t));
    memset(rec->mask_on_extra, 0xFF, sizeof(rec->mask_on_extra));
    memset(rec->mask_off_extra, 0xFF, sizeof(rec->mask_off_extra));
    rec->period_start = start_epoch_ms;
    /* Reverse NTP correction for day labelling (see struct comment). */
    rec->period_start_as11 = start_epoch_ms - clock_drift_ms;

    /* [0] Date: days from Unix epoch (noon-based day of session start) */
    time_t start_t = (time_t)(start_epoch_ms / 1000);
    struct tm tm_start;
    localtime_r(&start_t, &tm_start);
    time_t noon_t = start_t;
    if (tm_start.tm_hour < 12) noon_t -= 86400;
    struct tm epoch_tm = { .tm_year=70, .tm_mon=0, .tm_mday=1,
                           .tm_hour=0, .tm_min=0, .tm_sec=0 };
    time_t epoch_t = mktime(&epoch_tm);
    rec->values[0] = (int16_t)((noon_t - epoch_t) / 86400);

    int64_t noon_epoch_ms = (int64_t)noon_t * 1000;

    /* [1-3] MaskOn/MaskOff/MaskEvents from events.snt.
     * events.snt contains JSON lines with EventNotification messages.
     * Each event has "reportTime" (ISO 8601 UTC string) and "event" label.
     * Prefer MaskOn/MaskOff; fall back to TherapyStart/TherapyStop only if
     * MaskOn/MaskOff are not present (e.g. very short sessions). */
    int mask_on_count = 0;
    int mask_off_count = 0;
    int16_t ts_start_min = -1;  /* TherapyStart as fallback for MaskOn */
    int16_t ts_stop_min = -1;   /* TherapyStop as fallback for MaskOff */

    char events_path[350];
    snprintf(events_path, sizeof(events_path), "%s/%s_events.snt", session_dir, session_id);
    FILE *ef = fopen(events_path, "r");
    if (ef) {
        char line[512];
        while (fgets(line, sizeof(line), ef) && (mask_on_count < 20 || mask_off_count < 20)) {
            cJSON *msg = cJSON_Parse(line);
            if (!msg) continue;
            cJSON *params = cJSON_GetObjectItem(msg, "params");
            if (params) {
                cJSON *events = cJSON_GetObjectItem(params, "events");
                if (events && cJSON_IsArray(events)) {
                    int n = cJSON_GetArraySize(events);
                    for (int i = 0; i < n; i++) {
                        cJSON *ev = cJSON_GetArrayItem(events, i);
                        if (!ev) continue;
                        cJSON *label = cJSON_GetObjectItem(ev, "event");
                        if (!label || !cJSON_IsString(label)) continue;
                        cJSON *rt = cJSON_GetObjectItem(ev, "reportTime");
                        if (!rt || !cJSON_IsString(rt)) continue;

                        /* Parse ISO 8601 UTC: "2026-06-28T15:40:46.449Z" */
                        struct tm ev_tm = {0};
                        int ms = 0;
                        sscanf(rt->valuestring, "%d-%d-%dT%d:%d:%d.%dZ",
                               &ev_tm.tm_year, &ev_tm.tm_mon, &ev_tm.tm_mday,
                               &ev_tm.tm_hour, &ev_tm.tm_min, &ev_tm.tm_sec, &ms);
                        ev_tm.tm_year -= 1900;
                        ev_tm.tm_mon -= 1;
                        /* Convert UTC broken-down time to epoch directly.
                         * Algorithm from Howard Hinnant's date library (public domain). */
                        int y = ev_tm.tm_year + 1900;
                        int m = ev_tm.tm_mon + 1;
                        int d = ev_tm.tm_mday;
                        y -= m <= 2;
                        const int era = (y >= 0 ? y : y - 399) / 400;
                        const unsigned yoe = (unsigned)(y - era * 400);
                        const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
                        const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
                        int64_t days = (int64_t)era * 146097 + (int)doe - 719468;
                        int64_t secs = days * 86400 + ev_tm.tm_hour * 3600 +
                                       ev_tm.tm_min * 60 + ev_tm.tm_sec;
                        int64_t event_ntp_ms = secs * 1000 + ms + clock_drift_ms;
                        int min_from_noon = (int)((event_ntp_ms - noon_epoch_ms) / 60000);

                        if (strcmp(label->valuestring, "MaskOn") == 0 &&
                            mask_on_count < 20) {
                            if (mask_on_count == 0)
                                rec->values[1] = (int16_t)min_from_noon;
                            else
                                rec->mask_on_extra[mask_on_count - 1] = (int16_t)min_from_noon;
                            mask_on_count++;
                        } else if (strcmp(label->valuestring, "MaskOff") == 0 &&
                                   mask_off_count < 20) {
                            if (mask_off_count == 0)
                                rec->values[2] = (int16_t)min_from_noon;
                            else
                                rec->mask_off_extra[mask_off_count - 1] = (int16_t)min_from_noon;
                            mask_off_count++;
                        } else if (strcmp(label->valuestring, "TherapyStart") == 0) {
                            ts_start_min = (int16_t)min_from_noon;
                        } else if (strcmp(label->valuestring, "TherapyStop") == 0) {
                            ts_stop_min = (int16_t)min_from_noon;
                        }
                    }
                }
            }
            cJSON_Delete(msg);
        }
        fclose(ef);
    }

    /* Fallback: use TherapyStart/TherapyStop if no MaskOn/MaskOff found,
     * then fall back to session start/end times. */
    if (mask_on_count == 0) {
        if (ts_start_min >= 0) {
            rec->values[1] = ts_start_min;
            mask_on_count = 1;
        } else {
            int start_min = tm_start.tm_hour * 60 + tm_start.tm_min - 720;
            if (start_min < 0) start_min += 1440;
            rec->values[1] = (int16_t)start_min;
            mask_on_count = 1;
        }
    }
    if (mask_off_count == 0) {
        if (ts_stop_min >= 0) {
            rec->values[2] = ts_stop_min;
            mask_off_count = 1;
        } else {
            time_t end_t = (time_t)(end_epoch_ms / 1000);
            struct tm tm_end;
            localtime_r(&end_t, &tm_end);
            int end_min = tm_end.tm_hour * 60 + tm_end.tm_min - 720;
            if (end_min < 0) end_min += 1440;
            rec->values[2] = (int16_t)end_min;
            mask_off_count = 1;
        }
    }
    rec->values[3] = (int16_t)(mask_on_count > mask_off_count ? mask_on_count : mask_off_count);

    /* [4] Duration: minutes from start to end */
    if (end_epoch_ms > start_epoch_ms)
        rec->values[4] = (int16_t)((end_epoch_ms - start_epoch_ms) / 60000);

    /* [6-30] Settings from settings_json (same as build_str_data_values) */
    /* Create a temporary ctx with no data so build_str_data_values just
     * fills settings and leaves stats as -1. */
    summary_ctx_t empty_ctx;
    memset(&empty_ctx, 0, sizeof(empty_ctx));
    build_str_data_values(&empty_ctx, rec->values, settings_json);

    ESP_LOGI(TAG, "STR.edf: synthesized current day record "
             "(MaskOn=%d MaskOff=%d Duration=%d)",
             mask_on_count, mask_off_count, rec->values[4]);
}

/* Resolve the drift for one AS11 Summary noon-day.  Session metadata stores
 * the drift measured when that session stopped; choose the session closest to
 * the Summary PeriodStart.  This lets cumulative STR.edf keep historical days
 * NTP-corrected even after the AS11 clock changes later. */
typedef struct {
    int64_t start_as11_ms;
    int64_t drift_ms;
    char as11_day[16];
} session_drift_entry_t;

/* Build an in-memory index of all session drift entries by scanning
 * .sessions/streams/ once.  Returns a malloc'd array (caller frees) or
 * NULL on failure.  *out_count receives the number of entries. */
static session_drift_entry_t *build_session_drift_index(int *out_count)
{
    *out_count = 0;
    DIR *streams_dir = opendir(SD_STREAMS_DIR);
    if (!streams_dir) return NULL;

    int cap = 0, n = 0;
    session_drift_entry_t *entries = NULL;

    struct dirent *day_ent;
    while ((day_ent = readdir(streams_dir)) != NULL) {
        if (strlen(day_ent->d_name) != 8) continue;

        char stream_day_path[300];
        snprintf(stream_day_path, sizeof(stream_day_path), "%s/%s",
                 SD_STREAMS_DIR, day_ent->d_name);
        DIR *stream_day_dir = opendir(stream_day_path);
        if (!stream_day_dir) continue;

        struct dirent *session_ent;
        while ((session_ent = readdir(stream_day_dir)) != NULL) {
            size_t name_len = strlen(session_ent->d_name);
            if (name_len < 13 ||
                strcmp(session_ent->d_name + name_len - 13, "_session.json") != 0) {
                continue;
            }

            char session_path[600];
            snprintf(session_path, sizeof(session_path), "%s/%s",
                     stream_day_path, session_ent->d_name);
            cJSON *session = read_json_file(session_path);
            if (!session) continue;

            cJSON *start_j = cJSON_GetObjectItem(session, "start_epoch_ms");
            cJSON *drift_j = cJSON_GetObjectItem(session, "clock_drift_ms");
            if (cJSON_IsNumber(start_j) && cJSON_IsNumber(drift_j)) {
                if (n >= cap) {
                    int new_cap = cap ? cap * 2 : 16;
                    session_drift_entry_t *tmp = realloc(entries,
                        new_cap * sizeof(session_drift_entry_t));
                    if (!tmp) {
                        cJSON_Delete(session);
                        continue;
                    }
                    entries = tmp;
                    cap = new_cap;
                }
                int64_t start_ntp_ms = (int64_t)start_j->valuedouble;
                int64_t drift_ms = (int64_t)drift_j->valuedouble;
                entries[n].start_as11_ms = start_ntp_ms - drift_ms;
                entries[n].drift_ms = drift_ms;
                noon_day_folder(entries[n].start_as11_ms, entries[n].as11_day,
                                sizeof(entries[n].as11_day));
                n++;
            }
            cJSON_Delete(session);
        }
        closedir(stream_day_dir);
    }
    closedir(streams_dir);

    *out_count = n;
    return entries;
}

/* Look up the best-matching drift for a given AS11 noon-day and PeriodStart
 * from the in-memory index.  Falls back to fallback_drift_ms if no match. */
static int64_t lookup_drift(const session_drift_entry_t *entries, int n_entries,
                            const char *as11_day_label,
                            int64_t period_start_ms,
                            int64_t fallback_drift_ms)
{
    int64_t resolved_drift_ms = fallback_drift_ms;
    int64_t best_distance_ms = INT64_MAX;

    for (int i = 0; i < n_entries; i++) {
        if (strcmp(entries[i].as11_day, as11_day_label) != 0) continue;
        int64_t distance_ms = llabs(entries[i].start_as11_ms - period_start_ms);
        if (distance_ms < best_distance_ms) {
            best_distance_ms = distance_ms;
            resolved_drift_ms = entries[i].drift_ms;
        }
    }
    return resolved_drift_ms;
}

/* Generate multi-record STR.edf from per-day summary spool files.
 * Scans .sessions/summaries/ for *.spool files, parses each into a
 * daily STR record, and writes them all into a single STR.edf at
 * <sdcard_dir>/STR.edf with one EDF data record per day.
 * If the current day is not covered by any spool, synthesizes a record
 * from the current session's data (events.snt, start/end times). */
static esp_err_t generate_str_edf(const char *sdcard_dir,
                                  const char *patient_id, const char *recording_id,
                                  const char *start_date,
                                  const cJSON *settings_json,
                                  const char *session_dir,
                                  const char *session_id,
                                  int64_t start_epoch_ms, int64_t end_epoch_ms,
                                  int64_t clock_drift_ms)
{
    (void)start_date;  /* computed internally from oldest record */
    /* ── Scan .sessions/summaries/ for per-day .spool files ── */
    DIR *dir = opendir(SD_SUMMARIES_DIR);
    if (!dir) {
        ESP_LOGE(TAG, "STR.edf: cannot open %s: %s", SD_SUMMARIES_DIR, strerror(errno));
        return ESP_FAIL;
    }

    /* First pass: collect all spool filenames, sort, keep newest 30.
     * readdir order is non-deterministic, so we must collect all names
     * and sort to ensure deterministic day selection. */
    char (*spool_names)[15] = NULL;
    int n_spool = 0, spool_cap = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        const char *nm = ent->d_name;
        size_t nlen = strlen(nm);
        if (nlen != 14 || strcmp(nm + 8, ".spool") != 0) continue;
        bool digits_ok = true;
        for (int i = 0; i < 8; i++) {
            if (nm[i] < '0' || nm[i] > '9') { digits_ok = false; break; }
        }
        if (!digits_ok) continue;

        if (n_spool >= spool_cap) {
            int new_cap = spool_cap ? spool_cap * 2 : 16;
            char (*tmp)[15] = realloc(spool_names, new_cap * sizeof(*spool_names));
            if (!tmp) {
                ESP_LOGE(TAG, "STR.edf: spool name array realloc failed");
                free(spool_names);
                closedir(dir);
                return ESP_ERR_NO_MEM;
            }
            spool_names = tmp;
            spool_cap = new_cap;
        }
        strncpy(spool_names[n_spool], nm, 14);
        spool_names[n_spool][14] = '\0';
        n_spool++;
    }
    closedir(dir);

    /* Sort filenames lexicographically — YYYYMMDD.spool sorts chronologically */
    for (int i = 0; i < n_spool - 1; i++) {
        for (int j = i + 1; j < n_spool; j++) {
            if (strcmp(spool_names[j], spool_names[i]) < 0) {
                char tmp[15];
                strcpy(tmp, spool_names[i]);
                strcpy(spool_names[i], spool_names[j]);
                strcpy(spool_names[j], tmp);
            }
        }
    }

    /* Keep only the newest 30 (trim from the front) */
    int start_idx = n_spool > 30 ? n_spool - 30 : 0;

    str_day_record_t *records = calloc(30, sizeof(str_day_record_t));
    summary_ctx_t *ctx = calloc(1, sizeof(summary_ctx_t));
    edf_signal_def_t *str_sigs = calloc(STR_DATA_COUNT, sizeof(edf_signal_def_t));
    if (!records || !ctx || !str_sigs) {
        ESP_LOGE(TAG, "STR.edf: malloc failed");
        free(records); free(ctx); free(str_sigs);
        free(spool_names);
        return ESP_ERR_NO_MEM;
    }

    /* Build session drift index once (avoids O(days × sessions) dir scans). */
    int n_drift_entries = 0;
    session_drift_entry_t *drift_index = build_session_drift_index(&n_drift_entries);
    ESP_LOGI(TAG, "STR.edf: drift index: %d session entries", n_drift_entries);

    /* Second pass: parse the selected spool files */
    int n_records = 0;
    for (int si = start_idx; si < n_spool; si++) {
        const char *nm = spool_names[si];

        /* Read the spool file */
        char spool_path[300];
        snprintf(spool_path, sizeof(spool_path), "%s/%s", SD_SUMMARIES_DIR, nm);
        size_t spool_len = 0;
        uint8_t *spool_data = read_bin_file(spool_path, &spool_len);
        if (!spool_data || spool_len == 0) {
            ESP_LOGW(TAG, "STR.edf: skipping %s (empty or unreadable)", nm);
            free(spool_data);
            continue;
        }

        /* Parse the spool file directly as a Summary record.
         * collect_summary_spool writes the inner record content (not
         * wrapped in a field-2 tag), so we iterate it directly. */
        memset(ctx, 0, sizeof(*ctx));
        pb_iter(spool_data, spool_len, summary_field_cb, ctx);

        int64_t period_start = 0;
        bool found_ps = false;
        if (ctx->has_scalar[SUM_F_PERIOD_START]) {
            period_start = ctx->scalars[SUM_F_PERIOD_START];
            found_ps = true;
        }
        free(spool_data);

        if (!found_ps) {
            ESP_LOGW(TAG, "STR.edf: skipping %s (no PeriodStart)", nm);
            continue;
        }

        /* Build the record */
        str_day_record_t *rec = &records[n_records];
        memset(rec->values, 0xFF, STR_DATA_COUNT * sizeof(int16_t));
        memset(rec->mask_on_extra, 0xFF, sizeof(rec->mask_on_extra));
        memset(rec->mask_off_extra, 0xFF, sizeof(rec->mask_off_extra));

        char as11_day_label[16];
        noon_day_folder(period_start, as11_day_label, sizeof(as11_day_label));
        int64_t record_drift_ms = lookup_drift(drift_index, n_drift_entries,
                                                as11_day_label,
                                                period_start,
                                                clock_drift_ms);
        build_str_mask_events(ctx, rec->values, rec->mask_on_extra,
                              rec->mask_off_extra, period_start, record_drift_ms);
        build_str_data_values(ctx, rec->values, settings_json);
        rec->period_start = period_start + record_drift_ms;
        rec->period_start_as11 = period_start;  /* raw AS11 for day labelling */
        n_records++;

        ESP_LOGI(TAG, "STR.edf: parsed %s (PeriodStart=%lld, drift=%lld, Duration=%d)",
                 nm, (long long)period_start, (long long)record_drift_ms,
                 rec->values[4]);
    }
    free(spool_names);
    free(drift_index);

    /* Check if the current day is already covered by a spool record.
     * If not, synthesize a record from the current session's data.
     *
     * Day labelling uses the raw AS11 PeriodStart (period_start_as11)
     * deliberately — see the period_start_as11 comment in str_day_record_t.
     * The current session day label is derived from its AS11-domain noon-day
     * (start_epoch_ms - clock_drift_ms) to stay in the same label domain. */
    char current_day_label[16];
    noon_day_folder(start_epoch_ms - clock_drift_ms, current_day_label,
                    sizeof(current_day_label));
    ESP_LOGI(TAG, "STR.edf: current day label=%s (start_epoch_ms=%lld)",
             current_day_label, (long long)start_epoch_ms);
    bool current_day_found = false;
    for (int i = 0; i < n_records; i++) {
        char rec_day[16];
        noon_day_folder(records[i].period_start_as11, rec_day, sizeof(rec_day));
        ESP_LOGI(TAG, "STR.edf: record[%d] day=%s (period_start=%lld)",
                 i, rec_day, (long long)records[i].period_start);
        if (strcmp(rec_day, current_day_label) == 0) {
            current_day_found = true;
            break;
        }
    }
    ESP_LOGI(TAG, "STR.edf: current_day_found=%d n_records=%d",
             current_day_found, n_records);
    if (!current_day_found && n_records < 30) {
        ESP_LOGI(TAG, "STR.edf: synthesizing current day record (day=%s)",
                 current_day_label);
        build_current_day_record(&records[n_records], session_dir,
                                 session_id,
                                 start_epoch_ms, end_epoch_ms, clock_drift_ms,
                                 settings_json);
        n_records++;
    }

    if (n_records == 0) {
        ESP_LOGW(TAG, "STR.edf: no valid summary spool files found in %s", SD_SUMMARIES_DIR);
        free(records); free(ctx); free(str_sigs);
        return ESP_FAIL;
    }

    /* Sort records by period_start (chronological) */
    for (int i = 0; i < n_records - 1; i++) {
        for (int j = i + 1; j < n_records; j++) {
            if (records[j].period_start < records[i].period_start) {
                str_day_record_t tmp = records[i];
                records[i] = records[j];
                records[j] = tmp;
            }
        }
    }

    /* STR.edf signal definitions */
    static const edf_signal_def_t str_signal_defs[STR_DATA_COUNT] = {
        /* [0-3] Session header */
        { .label="Date", .transducer="", .unit="", .phys_min=0.0, .phys_max=24836.0, .dig_min=0, .dig_max=24836, .prefilter="", .samples_per_record=1 },
        { .label="MaskOn", .transducer="", .unit="MINUTES", .phys_min=0.0, .phys_max=1440.0, .dig_min=0, .dig_max=1440, .prefilter="", .samples_per_record=20 },
        { .label="MaskOff", .transducer="", .unit="MINUTES", .phys_min=0.0, .phys_max=1440.0, .dig_min=0, .dig_max=1440, .prefilter="", .samples_per_record=20 },
        { .label="MaskEvents", .transducer="", .unit="", .phys_min=0.0, .phys_max=255.0, .dig_min=0, .dig_max=255, .prefilter="", .samples_per_record=1 },
        /* [4-5] Session core */
        { .label="Duration", .transducer="", .unit="min.", .phys_min=0.0, .phys_max=1440.0, .dig_min=0, .dig_max=1440, .prefilter="", .samples_per_record=1 },
        { .label="Mode", .transducer="", .unit="", .phys_min=0, .phys_max=16, .dig_min=0, .dig_max=16, .prefilter="", .samples_per_record=1 },
        /* [6-13] CPAP/AutoSet settings */
        { .label="S.C.StartPress", .transducer="", .unit="cmH2O", .phys_min=4.0, .phys_max=20.0, .dig_min=200, .dig_max=1000, .prefilter="", .samples_per_record=1 },
        { .label="S.C.Press", .transducer="", .unit="cmH2O", .phys_min=4.0, .phys_max=20.0, .dig_min=200, .dig_max=1000, .prefilter="", .samples_per_record=1 },
        { .label="S.A.StartPress", .transducer="", .unit="cmH2O", .phys_min=4.0, .phys_max=20.0, .dig_min=200, .dig_max=1000, .prefilter="", .samples_per_record=1 },
        { .label="S.A.MaxPress", .transducer="", .unit="cmH2O", .phys_min=4.0, .phys_max=20.0, .dig_min=200, .dig_max=1000, .prefilter="", .samples_per_record=1 },
        { .label="S.A.MinPress", .transducer="", .unit="cmH2O", .phys_min=4.0, .phys_max=20.0, .dig_min=200, .dig_max=1000, .prefilter="", .samples_per_record=1 },
        { .label="S.AFH.StartPress", .transducer="", .unit="cmH2O", .phys_min=4.0, .phys_max=20.0, .dig_min=200, .dig_max=1000, .prefilter="", .samples_per_record=1 },
        { .label="S.AFH.MaxPress", .transducer="", .unit="cmH2O", .phys_min=4.0, .phys_max=20.0, .dig_min=200, .dig_max=1000, .prefilter="", .samples_per_record=1 },
        { .label="S.AFH.MinPress", .transducer="", .unit="cmH2O", .phys_min=4.0, .phys_max=20.0, .dig_min=200, .dig_max=1000, .prefilter="", .samples_per_record=1 },
        /* [14-33] Common comfort/settings */
        { .label="S.AS.Comfort", .transducer="", .unit="", .phys_min=0, .phys_max=16, .dig_min=0, .dig_max=16, .prefilter="", .samples_per_record=1 },
        { .label="S.RampEnable", .transducer="", .unit="", .phys_min=0, .phys_max=16, .dig_min=0, .dig_max=16, .prefilter="", .samples_per_record=1 },
        { .label="S.RampTime", .transducer="", .unit="min.", .phys_min=5.0, .phys_max=45.0, .dig_min=5, .dig_max=45, .prefilter="", .samples_per_record=1 },
        { .label="S.EPR.ClinEnable", .transducer="", .unit="", .phys_min=0, .phys_max=16, .dig_min=0, .dig_max=16, .prefilter="", .samples_per_record=1 },
        { .label="S.EPR.EPREnable", .transducer="", .unit="", .phys_min=0, .phys_max=16, .dig_min=0, .dig_max=16, .prefilter="", .samples_per_record=1 },
        { .label="S.EPR.Level", .transducer="", .unit="cmH2O", .phys_min=1.0, .phys_max=3.0, .dig_min=50, .dig_max=150, .prefilter="", .samples_per_record=1 },
        { .label="S.EPR.EPRType", .transducer="", .unit="", .phys_min=0, .phys_max=16, .dig_min=0, .dig_max=16, .prefilter="", .samples_per_record=1 },
        { .label="S.SmartStart", .transducer="", .unit="", .phys_min=0, .phys_max=16, .dig_min=0, .dig_max=16, .prefilter="", .samples_per_record=1 },
        { .label="S.PtAccess", .transducer="", .unit="", .phys_min=0, .phys_max=16, .dig_min=0, .dig_max=16, .prefilter="", .samples_per_record=1 },
        { .label="S.ABFilter", .transducer="", .unit="", .phys_min=0, .phys_max=16, .dig_min=0, .dig_max=16, .prefilter="", .samples_per_record=1 },
        { .label="S.Mask", .transducer="", .unit="", .phys_min=0, .phys_max=16, .dig_min=0, .dig_max=16, .prefilter="", .samples_per_record=1 },
        { .label="S.Tube", .transducer="", .unit="", .phys_min=0, .phys_max=16, .dig_min=0, .dig_max=16, .prefilter="", .samples_per_record=1 },
        { .label="S.ClimateControl", .transducer="", .unit="", .phys_min=0, .phys_max=16, .dig_min=0, .dig_max=16, .prefilter="", .samples_per_record=1 },
        { .label="S.HumEnable", .transducer="", .unit="", .phys_min=0, .phys_max=16, .dig_min=0, .dig_max=16, .prefilter="", .samples_per_record=1 },
        { .label="S.HumLevel", .transducer="", .unit="", .phys_min=1.0, .phys_max=8.0, .dig_min=1, .dig_max=8, .prefilter="", .samples_per_record=1 },
        { .label="S.TempEnable", .transducer="", .unit="", .phys_min=0, .phys_max=16, .dig_min=0, .dig_max=16, .prefilter="", .samples_per_record=1 },
        { .label="S.Temp", .transducer="", .unit="Celsius", .phys_min=15.6, .phys_max=30.0, .dig_min=156, .dig_max=300, .prefilter="", .samples_per_record=1 },
        { .label="HeatedTube", .transducer="", .unit="", .phys_min=0, .phys_max=16, .dig_min=0, .dig_max=16, .prefilter="", .samples_per_record=1 },
        { .label="Humidifier", .transducer="", .unit="", .phys_min=0, .phys_max=16, .dig_min=0, .dig_max=16, .prefilter="", .samples_per_record=1 },
        /* [33-46] Environment and oximetry stats */
        { .label="BlowPress.95", .transducer="", .unit="cmH2O", .phys_min=-10.0, .phys_max=45.0, .dig_min=-500, .dig_max=2250, .prefilter="", .samples_per_record=1 },
        { .label="BlowPress.5", .transducer="", .unit="cmH2O", .phys_min=-10.0, .phys_max=45.0, .dig_min=-500, .dig_max=2250, .prefilter="", .samples_per_record=1 },
        { .label="Flow.95", .transducer="", .unit="L/s", .phys_min=-2.0, .phys_max=3.0, .dig_min=-1000, .dig_max=1500, .prefilter="", .samples_per_record=1 },
        { .label="Flow.5", .transducer="", .unit="L/s", .phys_min=-2.0, .phys_max=3.0, .dig_min=-1000, .dig_max=1500, .prefilter="", .samples_per_record=1 },
        { .label="BlowFlow.50", .transducer="", .unit="L/s", .phys_min=-4.0, .phys_max=4.0, .dig_min=-2000, .dig_max=2000, .prefilter="", .samples_per_record=1 },
        { .label="AmbHumidity.50", .transducer="", .unit="mg/L", .phys_min=0.0, .phys_max=100.0, .dig_min=0, .dig_max=1000, .prefilter="", .samples_per_record=1 },
        { .label="HumTemp.50", .transducer="", .unit="Celsius", .phys_min=0.0, .phys_max=100.0, .dig_min=0, .dig_max=1000, .prefilter="", .samples_per_record=1 },
        { .label="HTubeTemp.50", .transducer="", .unit="Celsius", .phys_min=0.0, .phys_max=40.0, .dig_min=0, .dig_max=400, .prefilter="", .samples_per_record=1 },
        { .label="HTubePow.50", .transducer="", .unit="%", .phys_min=0.0, .phys_max=100.0, .dig_min=0, .dig_max=1000, .prefilter="", .samples_per_record=1 },
        { .label="HumPow.50", .transducer="", .unit="%", .phys_min=0.0, .phys_max=100.0, .dig_min=0, .dig_max=1000, .prefilter="", .samples_per_record=1 },
        { .label="SpO2.50", .transducer="", .unit="%", .phys_min=0.0, .phys_max=100.0, .dig_min=0, .dig_max=100, .prefilter="", .samples_per_record=1 },
        { .label="SpO2.95", .transducer="", .unit="%", .phys_min=0.0, .phys_max=100.0, .dig_min=0, .dig_max=100, .prefilter="", .samples_per_record=1 },
        { .label="SpO2.Max", .transducer="", .unit="%", .phys_min=0.0, .phys_max=100.0, .dig_min=0, .dig_max=100, .prefilter="", .samples_per_record=1 },
        { .label="SpO2Thresh", .transducer="", .unit="min.", .phys_min=0.0, .phys_max=1440.0, .dig_min=0, .dig_max=1440, .prefilter="", .samples_per_record=1 },
        /* [47-68] Bilevel/ventilation summary stats */
        { .label="MaskPress.50", .transducer="", .unit="cmH2O", .phys_min=0.0, .phys_max=40.0, .dig_min=0, .dig_max=2000, .prefilter="", .samples_per_record=1 },
        { .label="MaskPress.95", .transducer="", .unit="cmH2O", .phys_min=0.0, .phys_max=40.0, .dig_min=0, .dig_max=2000, .prefilter="", .samples_per_record=1 },
        { .label="MaskPress.Max", .transducer="", .unit="cmH2O", .phys_min=0.0, .phys_max=40.0, .dig_min=0, .dig_max=2000, .prefilter="", .samples_per_record=1 },
        { .label="TgtIPAP.50", .transducer="", .unit="cmH2O", .phys_min=0.0, .phys_max=50.0, .dig_min=0, .dig_max=2500, .prefilter="", .samples_per_record=1 },
        { .label="TgtIPAP.95", .transducer="", .unit="cmH2O", .phys_min=0.0, .phys_max=50.0, .dig_min=0, .dig_max=2500, .prefilter="", .samples_per_record=1 },
        { .label="TgtIPAP.Max", .transducer="", .unit="cmH2O", .phys_min=0.0, .phys_max=50.0, .dig_min=0, .dig_max=2500, .prefilter="", .samples_per_record=1 },
        { .label="TgtEPAP.50", .transducer="", .unit="cmH2O", .phys_min=0.0, .phys_max=30.0, .dig_min=0, .dig_max=1500, .prefilter="", .samples_per_record=1 },
        { .label="TgtEPAP.95", .transducer="", .unit="cmH2O", .phys_min=0.0, .phys_max=30.0, .dig_min=0, .dig_max=1500, .prefilter="", .samples_per_record=1 },
        { .label="TgtEPAP.Max", .transducer="", .unit="cmH2O", .phys_min=0.0, .phys_max=30.0, .dig_min=0, .dig_max=1500, .prefilter="", .samples_per_record=1 },
        { .label="Leak.50", .transducer="", .unit="L/s", .phys_min=0.0, .phys_max=2.0, .dig_min=0, .dig_max=100, .prefilter="", .samples_per_record=1 },
        { .label="Leak.95", .transducer="", .unit="L/s", .phys_min=0.0, .phys_max=2.0, .dig_min=0, .dig_max=100, .prefilter="", .samples_per_record=1 },
        { .label="Leak.70", .transducer="", .unit="L/s", .phys_min=0.0, .phys_max=2.0, .dig_min=0, .dig_max=100, .prefilter="", .samples_per_record=1 },
        { .label="Leak.Max", .transducer="", .unit="L/s", .phys_min=0.0, .phys_max=2.0, .dig_min=0, .dig_max=100, .prefilter="", .samples_per_record=1 },
        { .label="MinVent.50", .transducer="", .unit="L/min", .phys_min=0.0, .phys_max=30.0, .dig_min=0, .dig_max=240, .prefilter="", .samples_per_record=1 },
        { .label="MinVent.95", .transducer="", .unit="L/min", .phys_min=0.0, .phys_max=30.0, .dig_min=0, .dig_max=240, .prefilter="", .samples_per_record=1 },
        { .label="MinVent.Max", .transducer="", .unit="L/min", .phys_min=0.0, .phys_max=30.0, .dig_min=0, .dig_max=240, .prefilter="", .samples_per_record=1 },
        { .label="RespRate.50", .transducer="", .unit="bpm", .phys_min=0.0, .phys_max=90.0, .dig_min=0, .dig_max=450, .prefilter="", .samples_per_record=1 },
        { .label="RespRate.95", .transducer="", .unit="bpm", .phys_min=0.0, .phys_max=90.0, .dig_min=0, .dig_max=450, .prefilter="", .samples_per_record=1 },
        { .label="RespRate.Max", .transducer="", .unit="bpm", .phys_min=0.0, .phys_max=90.0, .dig_min=0, .dig_max=450, .prefilter="", .samples_per_record=1 },
        { .label="TidVol.50", .transducer="", .unit="L", .phys_min=0.0, .phys_max=4.0, .dig_min=0, .dig_max=200, .prefilter="", .samples_per_record=1 },
        { .label="TidVol.95", .transducer="", .unit="L", .phys_min=0.0, .phys_max=4.0, .dig_min=0, .dig_max=200, .prefilter="", .samples_per_record=1 },
        { .label="TidVol.Max", .transducer="", .unit="L", .phys_min=0.0, .phys_max=4.0, .dig_min=0, .dig_max=200, .prefilter="", .samples_per_record=1 },
        /* [69-76] Indices and CSR */
        { .label="AHI", .transducer="", .unit="", .phys_min=0.0, .phys_max=240.0, .dig_min=0, .dig_max=2400, .prefilter="", .samples_per_record=1 },
        { .label="HI", .transducer="", .unit="", .phys_min=0.0, .phys_max=240.0, .dig_min=0, .dig_max=2400, .prefilter="", .samples_per_record=1 },
        { .label="AI", .transducer="", .unit="", .phys_min=0.0, .phys_max=240.0, .dig_min=0, .dig_max=2400, .prefilter="", .samples_per_record=1 },
        { .label="OAI", .transducer="", .unit="", .phys_min=0.0, .phys_max=240.0, .dig_min=0, .dig_max=2400, .prefilter="", .samples_per_record=1 },
        { .label="CAI", .transducer="", .unit="", .phys_min=0.0, .phys_max=240.0, .dig_min=0, .dig_max=2400, .prefilter="", .samples_per_record=1 },
        { .label="UAI", .transducer="", .unit="", .phys_min=0.0, .phys_max=240.0, .dig_min=0, .dig_max=2400, .prefilter="", .samples_per_record=1 },
        { .label="RIN", .transducer="", .unit="", .phys_min=0.0, .phys_max=240.0, .dig_min=0, .dig_max=2400, .prefilter="", .samples_per_record=1 },
        { .label="CSR", .transducer="", .unit="", .phys_min=0.0, .phys_max=1440.0, .dig_min=0, .dig_max=1440, .prefilter="", .samples_per_record=1 },
    };

    memcpy(str_sigs, str_signal_defs, STR_DATA_COUNT * sizeof(edf_signal_def_t));

    /* Create STR.edf at SDCARD root */
    char path[300];
    snprintf(path, sizeof(path), "%s/STR.edf", sdcard_dir);

    char tmp_path[380];
    FILE *edf = open_atomic_file(path, tmp_path, sizeof(tmp_path));
    if (!edf) {
        ESP_LOGE(TAG, "cannot create %s: %s", path, strerror(errno));
        free(records); free(ctx); free(str_sigs);
        return ESP_FAIL;
    }

    /* STR.edf uses 12.00.00 noon as start time.
     * The start date must correspond to the oldest record (records[0] after
     * sorting), not the current session — otherwise SleepHQ misaligns every
     * record by the offset between the header date and the actual first day.
     *
     * We use period_start_as11 (raw AS11 clock) for the header date to keep
     * the day label consistent with spool-day names — see §5.9 comment in
     * str_day_record_t. */
    const char *str_start_time = "12.00.00";
    char str_date[32];
    {
        time_t t = (time_t)(records[0].period_start_as11 / 1000);
        struct tm tm;
        localtime_r(&t, &tm);
        if (tm.tm_hour < 12) {
            t -= 86400;
            localtime_r(&t, &tm);
        }
        snprintf(str_date, sizeof(str_date), "%02d.%02d.%02d",
                 tm.tm_mday, tm.tm_mon + 1, tm.tm_year % 100);
    }
    ESP_LOGI(TAG, "STR.edf: header date=%s (from oldest record PeriodStart=%lld)",
             str_date, (long long)records[0].period_start);

    /* Rewrite the recording_id Startdate to match the header start_date
     * (oldest record), not the current session's noon day.  The recording_id
     * format is "Startdate DD-MMM-YYYY X X X SRN=...". */
    char fixed_recording_id[128];
    {
        /* Use AS11-domain period_start to match the header date label above. */
        time_t t = (time_t)(records[0].period_start_as11 / 1000);
        struct tm tm;
        localtime_r(&t, &tm);
        if (tm.tm_hour < 12) {
            t -= 86400;
            localtime_r(&t, &tm);
        }
        static const char *mon_names[] = {
            "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
            "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"
        };
        /* Find the tail after "Startdate DD-MMM-YYYY" in the original */
        const char *tail = recording_id;
        if (strncmp(tail, "Startdate ", 10) == 0) {
            /* Skip "Startdate " + date (11 chars for "DD-MMM-YYYY") */
            tail += 10;
            while (*tail && *tail != ' ') tail++;
        }
        snprintf(fixed_recording_id, sizeof(fixed_recording_id),
                 "Startdate %02d-%s-%04d%s",
                 tm.tm_mday, mon_names[tm.tm_mon % 12],
                 tm.tm_year + 1900, tail);
    }

    int header_bytes = edf_write_header(edf, patient_id, fixed_recording_id,
                                        str_date, str_start_time,
                                        n_records,
                                        "86400.00",
                                        "EDF", str_sigs, STR_DATA_COUNT);
    if (header_bytes < 0) {
        ESP_LOGE(TAG, "STR.edf: edf_write_header failed");
        discard_atomic_file(edf, tmp_path);
        free(records); free(ctx); free(str_sigs);
        return ESP_FAIL;
    }

    /* Write one data record per day.
     * Each record: 115 data int16 + 1 CRC int16 = 232 bytes. */
    for (int r = 0; r < n_records; r++) {
        int16_t rec_buf[115];
        int rec_pos = 0;
        for (int i = 0; i < STR_DATA_COUNT; i++) {
            int spr = str_signal_defs[i].samples_per_record;
            rec_buf[rec_pos++] = records[r].values[i];
            for (int s = 1; s < spr; s++) {
                if (i == 1 && s - 1 < 20)
                    rec_buf[rec_pos++] = records[r].mask_on_extra[s - 1];
                else if (i == 2 && s - 1 < 20)
                    rec_buf[rec_pos++] = records[r].mask_off_extra[s - 1];
                else
                    rec_buf[rec_pos++] = -1;
            }
        }
        if (rec_pos != 115) {
            ESP_LOGE(TAG, "STR.edf: internal error: rec_pos=%d != 115", rec_pos);
            discard_atomic_file(edf, tmp_path);
            free(records); free(ctx); free(str_sigs);
            return ESP_FAIL;
        }

        uint16_t crc = crc16_ccitt((uint8_t *)rec_buf, 115 * sizeof(int16_t));
        if (!write_all(edf, rec_buf, 115 * sizeof(int16_t))) {
            ESP_LOGE(TAG, "STR.edf: write failed at record %d", r);
            discard_atomic_file(edf, tmp_path);
            free(records); free(ctx); free(str_sigs);
            return ESP_FAIL;
        }
        int16_t crc_val = (int16_t)crc;
        if (!write_all(edf, &crc_val, sizeof(crc_val))) {
            ESP_LOGE(TAG, "STR.edf: CRC write failed at record %d", r);
            discard_atomic_file(edf, tmp_path);
            free(records); free(ctx); free(str_sigs);
            return ESP_FAIL;
        }
    }

    if (finalize_atomic_file(edf, tmp_path, path) != ESP_OK) {
        ESP_LOGE(TAG, "cannot finalize %s: %s", path, strerror(errno));
        free(records); free(ctx); free(str_sigs);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "STR.edf generated: %s (%d records)", path, n_records);

    free(records);
    free(ctx);
    free(str_sigs);
    return ESP_OK;
}

/* ════════════════════════════════════════════════════════════════════
 *  Section 7: EVE.edf / CSL.edf generation from events.snt stream
 * ════════════════════════════════════════════════════════════════════
 *
 * EVE.edf and CSL.edf are EDF+D annotation files containing event
 * annotations.  Each data record contains:
 *   - 62 bytes: EDF+ TAL annotation payload
 *   - 2 bytes:  little-endian CRC16 over the 62 payload bytes
 *
 * The first record is always a "Recording starts" marker.
 * Subsequent records contain one event TAL each.
 *
 * Event labels (from edf_annotations.md and live notifications):
 *   HypopneaEnd         → "Hypopnea"
 *   CentralApneaEnd     → "Central Apnea"
 *   ObstructiveApneaEnd → "Obstructive Apnea"
 *   ApneaEnd            → "Apnea"
 *   ReraEnd             → "Arousal"
 *   CsrStart / CsrEnd   → "CSR" (in CSL mode)
 *
 * All timestamps inside events.snt reportTime are in the AS11 clock domain.
 * Convert them with clock_drift_ms (NTP - AS11) before calculating annotation
 * onsets relative to the NTP-corrected EDF header start time.
 */

static const char *event_label_map_str(const char *ev_name, bool csl_mode)
{
    if (!ev_name) return NULL;
    if (csl_mode) {
        if (strcmp(ev_name, "CsrStart") == 0 || strcmp(ev_name, "CsrEnd") == 0 ||
            strcmp(ev_name, "CSRStart") == 0 || strcmp(ev_name, "CSREnd") == 0) {
            return "CSR";
        }
        return NULL;
    }
    if (strcmp(ev_name, "HypopneaEnd") == 0)          return "Hypopnea";
    if (strcmp(ev_name, "CentralApneaEnd") == 0)      return "Central Apnea";
    if (strcmp(ev_name, "ObstructiveApneaEnd") == 0)  return "Obstructive Apnea";
    if (strcmp(ev_name, "ApneaEnd") == 0)             return "Apnea";
    if (strcmp(ev_name, "ReraEnd") == 0)              return "Arousal";
    return NULL;
}

typedef struct {
    int64_t onset_sec;
    int64_t dur_sec;
    const char *label;
} snt_event_t;

static esp_err_t generate_eve_edf(const char *edf_path,
                                  const char *snt_path,
                                  int64_t session_start_ms, int64_t clock_drift_ms,
                                  const char *patient_id, const char *recording_id,
                                  const char *start_date, const char *start_time,
                                  bool csl_mode)
{
    char path[350];
    char tmp_path[380];
    strncpy(path, edf_path, sizeof(path) - 1);
    path[sizeof(path) - 1] = '\0';

    size_t capacity = 128;
    size_t count = 0;
    snt_event_t *ev_list = malloc(capacity * sizeof(snt_event_t));
    if (!ev_list) {
        ESP_LOGE(TAG, "generate_eve_edf: out of memory");
        return ESP_ERR_NO_MEM;
    }

    FILE *ef = fopen(snt_path, "r");
    if (ef) {
        char line[512];
        while (fgets(line, sizeof(line), ef)) {
            cJSON *msg = cJSON_Parse(line);
            if (!msg) continue;
            cJSON *params = cJSON_GetObjectItem(msg, "params");
            if (params) {
                cJSON *data_id = cJSON_GetObjectItem(params, "dataId");
                if (data_id && cJSON_IsString(data_id)) {
                    /* Both EVE.edf and CSL.edf source from TherapyEvents-
                     * RespiratoryEvents.  CSR events are delivered under
                     * this same dataId, so the label filter in
                     * event_label_map_str handles the EVE/CSL distinction. */
                    if (strcmp(data_id->valuestring, "TherapyEvents-RespiratoryEvents") == 0) {
                        cJSON *events = cJSON_GetObjectItem(params, "events");
                        if (events && cJSON_IsArray(events)) {
                            int n = cJSON_GetArraySize(events);
                            for (int i = 0; i < n; i++) {
                                cJSON *ev = cJSON_GetArrayItem(events, i);
                                if (!ev) continue;
                                cJSON *label_j = cJSON_GetObjectItem(ev, "event");
                                if (!label_j || !cJSON_IsString(label_j)) continue;
                                const char *label = event_label_map_str(label_j->valuestring, csl_mode);
                                if (!label) continue;
                                cJSON *rt_j = cJSON_GetObjectItem(ev, "reportTime");
                                if (!rt_j || !cJSON_IsString(rt_j)) continue;

                                int64_t as11_ts_ms = parse_iso8601_utc_ms(rt_j->valuestring);
                                if (as11_ts_ms < 0) continue;

                                int64_t event_ntp_ms = as11_ts_ms + clock_drift_ms;
                                int64_t onset_sec = (event_ntp_ms - session_start_ms) / 1000;
                                if (onset_sec < 0) onset_sec = 0;

                                int64_t dur_sec = 0;
                                cJSON *dur_j = cJSON_GetObjectItem(ev, "durationSeconds");
                                if (dur_j && cJSON_IsNumber(dur_j)) {
                                    dur_sec = dur_j->valueint;
                                    if (dur_sec < 0) dur_sec = 0;
                                }

                                if (count >= capacity) {
                                    size_t new_cap = capacity * 2;
                                    snt_event_t *tmp = realloc(ev_list, new_cap * sizeof(snt_event_t));
                                    if (tmp) {
                                        ev_list = tmp;
                                        capacity = new_cap;
                                    } else {
                                        break;
                                    }
                                }
                                ev_list[count].onset_sec = onset_sec;
                                ev_list[count].dur_sec = dur_sec;
                                ev_list[count].label = label;
                                count++;
                            }
                        }
                    }
                }
            }
            cJSON_Delete(msg);
        }
        fclose(ef);
    } else {
        ESP_LOGW(TAG, "generate_eve_edf: cannot open %s: %s", snt_path, strerror(errno));
    }

    /* Sort events by onset_sec (stable enough — ties keep insertion order).
     * events.snt lines may arrive out of order due to BLE retransmits or
     * _SNC spool replay, so we must sort for deterministic EDF output. */
    for (size_t i = 0; i + 1 < count; i++) {
        for (size_t j = i + 1; j < count; j++) {
            if (ev_list[j].onset_sec < ev_list[i].onset_sec) {
                snt_event_t tmp = ev_list[i];
                ev_list[i] = ev_list[j];
                ev_list[j] = tmp;
            }
        }
    }

    /* Deduplicate: remove events with identical onset_sec and label.
     * BLE may deliver the same event in multiple _SNC notifications. */
    size_t dedup_count = 0;
    for (size_t i = 0; i < count; i++) {
        if (dedup_count > 0 &&
            ev_list[dedup_count - 1].onset_sec == ev_list[i].onset_sec &&
            strcmp(ev_list[dedup_count - 1].label, ev_list[i].label) == 0) {
            continue;
        }
        ev_list[dedup_count++] = ev_list[i];
    }
    if (dedup_count < count) {
        ESP_LOGI(TAG, "%s: deduplicated %zu duplicate events (%zu → %zu)",
                 csl_mode ? "CSL.edf" : "EVE.edf", count - dedup_count,
                 count, dedup_count);
    }
    count = dedup_count;

    /* Total records = 1 (Recording starts) + count */
    int total_records = 1 + (int)count;

    ESP_LOGI(TAG, "%s: %d events from %s, %d total records",
             csl_mode ? "CSL.edf" : "EVE.edf", (int)count, snt_path, total_records);

    /* EVE.edf signal definitions: EDF Annotations only.
     * edf_write_header auto-appends Crc16 as the last signal,
     * giving 2 total signals (768-byte header, 64-byte records). */
    edf_signal_def_t eve_sigs[1];
    eve_sigs[0].label = "EDF Annotations";
    eve_sigs[0].transducer = "";
    eve_sigs[0].unit = "";
    eve_sigs[0].phys_min = -32768.0;
    eve_sigs[0].phys_max = 32767.0;
    eve_sigs[0].dig_min = -32768;
    eve_sigs[0].dig_max = 32767;
    eve_sigs[0].prefilter = "";
    eve_sigs[0].samples_per_record = 31;  /* 62 bytes / 2 bytes per sample */

    FILE *edf = open_atomic_file(path, tmp_path, sizeof(tmp_path));
    if (!edf) {
        ESP_LOGE(TAG, "cannot create %s: %s", path, strerror(errno));
        free(ev_list);
        return ESP_FAIL;
    }

    int header_bytes = edf_write_header(edf, patient_id, recording_id,
                                        start_date, start_time,
                                        total_records, "0.00",
                                        "EDF+D", eve_sigs, 1);
    if (header_bytes < 0) {
        ESP_LOGE(TAG, "EVE.edf: edf_write_header failed");
        discard_atomic_file(edf, tmp_path);
        free(ev_list);
        return ESP_FAIL;
    }

    /* Write data records */
    /* Record 0: "Recording starts" marker */
    {
        uint8_t payload[62];
        memset(payload, 0, 62);
        int p = 0;
        payload[p++] = '+';
        payload[p++] = '0';
        payload[p++] = 0x14;  /* TAL separator */
        payload[p++] = 0x14;  /* empty duration */
        payload[p++] = 0x00;  /* end of first TAL */
        payload[p++] = '+';
        payload[p++] = '0';
        payload[p++] = 0x15;  /* duration separator */
        payload[p++] = '0';
        payload[p++] = 0x14;  /* TAL separator */
        const char *label = "Recording starts";
        memcpy(payload + p, label, strlen(label));
        p += strlen(label);
        payload[p++] = 0x14;
        payload[p++] = 0x00;

        uint16_t crc = crc16_ccitt(payload, 62);
        uint8_t crc_bytes[2] = { (uint8_t)(crc & 0xFF), (uint8_t)(crc >> 8) };
        if (!write_all(edf, payload, 62) ||
            !write_all(edf, crc_bytes, 2)) {
            ESP_LOGE(TAG, "%s: write failed at Recording starts record",
                     csl_mode ? "CSL.edf" : "EVE.edf");
            discard_atomic_file(edf, tmp_path);
            free(ev_list);
            return ESP_FAIL;
        }
    }

    /* Subsequent records: one event per record */
    for (size_t i = 0; i < count; i++) {
        uint8_t payload[62];
        memset(payload, 0, 62);
        int p = 0;
        /* Empty timekeeping TAL */
        payload[p++] = '+';
        payload[p++] = '0';
        payload[p++] = 0x14;
        payload[p++] = 0x14;
        payload[p++] = 0x00;

        /* Event TAL: +onset\x15duration\x14label\x14\x00 */
        char onset_str[24];
        snprintf(onset_str, sizeof(onset_str), "+%lld", (long long)ev_list[i].onset_sec);
        memcpy(payload + p, onset_str, strlen(onset_str));
        p += strlen(onset_str);
        payload[p++] = 0x15;  /* duration separator */
        char dur_str[24];
        snprintf(dur_str, sizeof(dur_str), "%lld", (long long)ev_list[i].dur_sec);
        memcpy(payload + p, dur_str, strlen(dur_str));
        p += strlen(dur_str);
        payload[p++] = 0x14;
        size_t max_copy = 62 - p - 3;
        size_t label_len = strlen(ev_list[i].label);
        if (label_len > max_copy) label_len = max_copy;
        memcpy(payload + p, ev_list[i].label, label_len);
        p += label_len;
        payload[p++] = 0x14;
        payload[p++] = 0x00;

        uint16_t crc = crc16_ccitt(payload, 62);
        uint8_t crc_bytes[2] = { (uint8_t)(crc & 0xFF), (uint8_t)(crc >> 8) };
        if (!write_all(edf, payload, 62) ||
            !write_all(edf, crc_bytes, 2)) {
            ESP_LOGE(TAG, "%s: write failed at event %zu",
                     csl_mode ? "CSL.edf" : "EVE.edf", i);
            discard_atomic_file(edf, tmp_path);
            free(ev_list);
            return ESP_FAIL;
        }
    }

    free(ev_list);
    if (finalize_atomic_file(edf, tmp_path, path) != ESP_OK) {
        ESP_LOGE(TAG, "cannot finalize %s: %s", path, strerror(errno));
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "%s generated: %s (%d events)", csl_mode ? "CSL.edf" : "EVE.edf", path, (int)count);
    return ESP_OK;
}

/* ════════════════════════════════════════════════════════════════════
 *  Section 8: Identification.json + .crc generation
 * ════════════════════════════════════════════════════════════════════ */

static esp_err_t generate_identification(const char *edf_dir,
                                         const char *ident_json_path)
{
    /* Read the flat identification.json from post-therapy/ and restructure
     * it into the nested AS11 format:
     *   {"FlowGenerator":{"IdentificationProfiles":{
     *     "Product":{...},"Hardware":{...},"Software":{...}
     *   }}}
     * Also compute CRC-32 of the JSON bytes and write as 4-byte binary LE. */

    cJSON *ident = read_json_file(ident_json_path);
    if (!ident) {
        ESP_LOGW(TAG, "identification.json not found: %s", ident_json_path);
        return ESP_FAIL;
    }

    /* Helper to get a string field from the flat JSON (handles both string and number types) */
    const char *get_str(const cJSON *obj, const char *key) {
        cJSON *j = cJSON_GetObjectItem(obj, key);
        if (j && cJSON_IsString(j)) return j->valuestring;
        if (j && cJSON_IsNumber(j)) {
            static char num_buf[32];
            snprintf(num_buf, sizeof(num_buf), "%d", j->valueint);
            return num_buf;
        }
        return "";
    }

    /* Build nested Product object */
    cJSON *product = cJSON_CreateObject();
    cJSON_AddStringToObject(product, "UniversalIdentifier",
        get_str(ident, "UniversalIdentifier"));
    cJSON_AddStringToObject(product, "SerialNumber",
        get_str(ident, "SerialNumber"));
    cJSON_AddStringToObject(product, "SerialNumberVerificationCode", "");
    cJSON_AddStringToObject(product, "ProductCode",
        get_str(ident, "ProductCode"));

    /* ProductName: keep as-is (AS11 stores "AirSense 11 AutoSet" with spaces) */
    cJSON_AddStringToObject(product, "ProductName", get_str(ident, "ProductName"));

    cJSON_AddStringToObject(product, "FdaUniqueDeviceIdentifier", "");
    cJSON_AddStringToObject(product, "ProductGeographicIdentifier",
        get_str(ident, "ProductGeographicIdentifier"));

    /* Build Hardware object */
    cJSON *hardware = cJSON_CreateObject();
    cJSON_AddStringToObject(hardware, "HardwareIdentifier",
        get_str(ident, "HardwareIdentifier"));

    /* Build Software object.
     * Numeric fields use cJSON_AddNumberToObject to match AS11 format. */
    cJSON *software = cJSON_CreateObject();
    cJSON_AddStringToObject(software, "BootloaderIdentifier",
        get_str(ident, "BootloaderIdentifier"));
    cJSON_AddStringToObject(software, "ApplicationIdentifier",
        get_str(ident, "ApplicationIdentifier"));
    cJSON_AddStringToObject(software, "ConfigurationIdentifier",
        get_str(ident, "ConfigurationIdentifier"));
    {
        cJSON *v;
        v = cJSON_GetObjectItem(ident, "PlatformIdentifier");
        cJSON_AddNumberToObject(software, "PlatformIdentifier",
            v ? v->valuedouble : 0);
        v = cJSON_GetObjectItem(ident, "VariantIdentifier");
        cJSON_AddNumberToObject(software, "VariantIdentifier",
            v ? v->valuedouble : 0);
        v = cJSON_GetObjectItem(ident, "RegionIdentifier");
        cJSON_AddNumberToObject(software, "RegionIdentifier",
            v ? v->valuedouble : 0);
    }
    cJSON_AddStringToObject(software, "ProfileVariationIdentifier",
        get_str(ident, "ProfileVariantIdentifier"));
    {
        cJSON *v = cJSON_GetObjectItem(ident, "DataVersionIdentifier");
        cJSON_AddNumberToObject(software, "DataVersionIdentifier",
            v ? v->valuedouble : 0);
    }
    cJSON_AddStringToObject(software, "DataModelVersionIdentifier",
        get_str(ident, "DataModelVersionIdentifier"));

    /* Build IdentificationProfiles */
    cJSON *profiles = cJSON_CreateObject();
    cJSON_AddItemToObject(profiles, "Product", product);
    cJSON_AddItemToObject(profiles, "Hardware", hardware);
    cJSON_AddItemToObject(profiles, "Software", software);

    /* Build FlowGenerator wrapper */
    cJSON *fg = cJSON_CreateObject();
    cJSON_AddItemToObject(fg, "IdentificationProfiles", profiles);

    /* Build root: {"FlowGenerator":{"IdentificationProfiles":{...}}} */
    cJSON *root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "FlowGenerator", fg);

    /* Render as compact JSON (no whitespace) */
    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    cJSON_Delete(ident);

    if (!json_str) {
        ESP_LOGE(TAG, "generate_identification: cJSON_PrintUnformatted failed");
        return ESP_FAIL;
    }

    /* Write Identification.json to EDF dir (atomic) */
    char path[300];
    char tmp_path[380];
    size_t json_len = strlen(json_str);
    snprintf(path, sizeof(path), "%s/Identification.json", edf_dir);
    FILE *f = open_atomic_file(path, tmp_path, sizeof(tmp_path));
    if (f) {
        if (!write_all(f, json_str, json_len)) {
            ESP_LOGE(TAG, "cannot write %s: %s", path, strerror(errno));
            discard_atomic_file(f, tmp_path);
        } else if (finalize_atomic_file(f, tmp_path, path) != ESP_OK) {
            ESP_LOGE(TAG, "cannot finalize %s: %s", path, strerror(errno));
        } else {
            ESP_LOGI(TAG, "wrote %s (%u bytes)", path, (unsigned)json_len);
        }
    } else {
        ESP_LOGE(TAG, "cannot create %s: %s", path, strerror(errno));
    }

    /* Compute CRC-32 and write as 4-byte binary little-endian (atomic) */
    uint32_t crc = crc32_ieee((const uint8_t *)json_str, json_len);
    snprintf(path, sizeof(path), "%s/Identification.crc", edf_dir);
    f = open_atomic_file(path, tmp_path, sizeof(tmp_path));
    if (f) {
        uint8_t crc_bytes[4] = {
            (uint8_t)(crc & 0xFF),
            (uint8_t)((crc >> 8) & 0xFF),
            (uint8_t)((crc >> 16) & 0xFF),
            (uint8_t)((crc >> 24) & 0xFF),
        };
        if (!write_all(f, crc_bytes, 4)) {
            ESP_LOGE(TAG, "cannot write %s: %s", path, strerror(errno));
            discard_atomic_file(f, tmp_path);
        } else if (finalize_atomic_file(f, tmp_path, path) != ESP_OK) {
            ESP_LOGE(TAG, "cannot finalize %s: %s", path, strerror(errno));
        } else {
            ESP_LOGI(TAG, "wrote %s (crc32=0x%08X)", path, (unsigned)crc);
        }
    } else {
        ESP_LOGE(TAG, "cannot create %s: %s", path, strerror(errno));
    }

    free(json_str);
    return ESP_OK;
}

/* ════════════════════════════════════════════════════════════════════
 *  Section 9: Main EDF generation entry point
 * ════════════════════════════════════════════════════════════════════ */

/* Read a JSON file and return the parsed cJSON object. */
static cJSON *read_json_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(fsize + 1);
    if (!buf) { fclose(f); return NULL; }
    fread(buf, 1, fsize, f);
    buf[fsize] = '\0';
    fclose(f);
    cJSON *json = cJSON_Parse(buf);
    free(buf);
    return json;
}

/* Read a binary file into a malloc'd buffer. */
static uint8_t *read_bin_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize <= 0) { fclose(f); return NULL; }
    uint8_t *buf = malloc(fsize);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, fsize, f);
    fclose(f);
    if (rd != (size_t)fsize) { free(buf); return NULL; }
    *out_len = (size_t)fsize;
    return buf;
}

/* Format date/time strings for EDF header from epoch ms. */
static void format_edf_datetime(int64_t epoch_ms,
                                char *date_out, int date_len,
                                char *time_out, int time_len)
{
    time_t t = (time_t)(epoch_ms / 1000);
    struct tm tm;
    localtime_r(&t, &tm);
    snprintf(date_out, date_len, "%02d.%02d.%02d",
             tm.tm_mday, tm.tm_mon + 1, tm.tm_year % 100);
    snprintf(time_out, time_len, "%02d.%02d.%02d",
             tm.tm_hour, tm.tm_min, tm.tm_sec);
}

/* Format the recording ID string.
 * Format: "Startdate DD-MMM-YYYY X X X SRN=<serial> MID=<mid> VID=<vid>" */
static void format_recording_id(char *out, size_t out_len,
                                int64_t epoch_ms,
                                const cJSON *ident)
{
    time_t t = (time_t)(epoch_ms / 1000);
    struct tm tm;
    localtime_r(&t, &tm);

    static const char *month_names[] = {
        "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
        "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"
    };

    const char *srn = "";
    const char *mid = "";
    const char *vid = "";

    if (ident) {
        cJSON *j;
        j = cJSON_GetObjectItem(ident, "SerialNumber");
        if (j && cJSON_IsString(j)) srn = j->valuestring;
        j = cJSON_GetObjectItem(ident, "PlatformIdentifier");
        if (j) {
            if (cJSON_IsString(j)) mid = j->valuestring;
            else if (cJSON_IsNumber(j)) { static char mid_buf[16]; snprintf(mid_buf, sizeof(mid_buf), "%d", j->valueint); mid = mid_buf; }
        }
        j = cJSON_GetObjectItem(ident, "VariantIdentifier");
        if (j) {
            if (cJSON_IsString(j)) vid = j->valuestring;
            else if (cJSON_IsNumber(j)) { static char vid_buf[16]; snprintf(vid_buf, sizeof(vid_buf), "%d", j->valueint); vid = vid_buf; }
        }
    }

    snprintf(out, out_len,
             "Startdate %02d-%s-%04d X X X SRN=%s MID=%s VID=%s",
             tm.tm_mday, month_names[tm.tm_mon % 12],
             tm.tm_year + 1900, srn, mid, vid);
}

/* Compute the noon-based day folder (YYYYMMDD) for a session timestamp.
 * AS11 groups sessions by a day window that starts at noon, so sessions
 * before noon belong to the previous day's folder. */
static void noon_day_folder(int64_t epoch_ms, char *out, size_t out_len)
{
    time_t t = (time_t)(epoch_ms / 1000);
    struct tm tm;
    localtime_r(&t, &tm);
    if (tm.tm_hour < 12) {
        /* Before noon — belongs to previous day */
        t -= 86400;
        localtime_r(&t, &tm);
    }
    snprintf(out, out_len, "%04d%02d%02d",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
}

/* Format a session timestamp prefix: YYYYMMDD_HHMMSS */
static void session_timestamp(int64_t epoch_ms, char *out, size_t out_len)
{
    time_t t = (time_t)(epoch_ms / 1000);
    struct tm tm;
    localtime_r(&t, &tm);
    snprintf(out, out_len, "%04d%02d%02d_%02d%02d%02d",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
}

esp_err_t edf_gen_generate(const char *session_dir, const char *session_id,
                           int64_t start_epoch_ms, int64_t end_epoch_ms,
                           int64_t clock_drift_ms)
{
    if (!session_dir || !session_id) return ESP_ERR_INVALID_ARG;
    if (!sd_storage_is_ready()) {
        ESP_LOGW(TAG, "SD not ready, skipping EDF generation");
        return ESP_ERR_INVALID_STATE;
    }
    /* Reject sessions with invalid timestamps — these result from crash
     * recovery on 0-byte .snt files and would produce bogus 19691231 folders. */
    if (start_epoch_ms < 946684800000LL) {  /* < 2000-01-01T00:00:00Z */
        ESP_LOGW(TAG, "invalid start_epoch_ms=%lld, skipping EDF generation for %s",
                 (long long)start_epoch_ms, session_id);
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "=== EDF GENERATION START ===");
    ESP_LOGI(TAG, "session=%s id=%s drift=%lldms",
             session_dir, session_id, (long long)clock_drift_ms);
    ESP_LOGI(TAG, "start_epoch_ms=%lld end_epoch_ms=%lld",
             (long long)start_epoch_ms, (long long)end_epoch_ms);

    /* ── Create SDCARD export directory structure ──
     * EDF files go to /somnotrace/SDCARD/DATALOG/YYYYMMDD/
     * STR.edf goes to /somnotrace/SDCARD/STR.edf (root level)
     * Identification.json goes to /somnotrace/SDCARD/
     * This is the ResMed-compatible SD card image, fully derived from .sessions/ */
    mkdir(SD_SDCARD_DIR, 0775);
    mkdir(SD_SDCARD_DATALOG, 0775);
    mkdir(SD_SDCARD_SETTINGS, 0775);

    /* The SDCARD export uses NTP-corrected time throughout: DATALOG days,
     * filenames, EDF headers, STR session boundaries, and annotations must
     * agree so importers group sessions correctly and graphs show real time. */

    /* Create date subdirectory inside DATALOG (noon-based NTP day).
     * Layout: DATALOG/YYYYMMDD/YYYYMMDD_HHMMSS_TYPE.edf */
    char day_folder[16];
    noon_day_folder(start_epoch_ms, day_folder, sizeof(day_folder));

    char day_dir[220];
    snprintf(day_dir, sizeof(day_dir), "%s/%s", SD_SDCARD_DATALOG, day_folder);
    mkdir(day_dir, 0775);

    /* Session timestamp prefix for EVE/CSL EDF filenames (NTP TherapyStart). */
    char ts_prefix[32];
    session_timestamp(start_epoch_ms, ts_prefix, sizeof(ts_prefix));

    /* ── Load post-therapy data from .sessions/streams/YYYYMMDD/ ──
     * Files use prefix-based naming: <session_id>_ident.json, etc. */

    /* Read identification.json for EDF header fields */
    char ident_path[330];
    snprintf(ident_path, sizeof(ident_path), "%s/%s_ident.json", session_dir, session_id);
    cJSON *ident = read_json_file(ident_path);

    /* Read settings.json for STR.edf settings fields */
    char settings_path[330];
    snprintf(settings_path, sizeof(settings_path), "%s/%s_settings.json", session_dir, session_id);
    cJSON *settings = read_json_file(settings_path);
    ESP_LOGI(TAG, "settings.json: path=%s %s", settings_path,
             settings ? "loaded OK" : "FAILED to load");

    /* Path to events.snt for EVE.edf / CSL.edf generation */
    char events_snt_path[330];
    snprintf(events_snt_path, sizeof(events_snt_path), "%s/%s_events.snt", session_dir, session_id);

    /* ── Build EDF header timestamps ──
     *
     * AS11 uses two different start timestamps:
     *   - EVE/CSL: TherapyStart (session start event)
     *   - BRP/PLD/SA2: MaskOn (mask detected, ~7s after TherapyStart)
     * The .snt files capture from TherapyStart, so BRP/PLD/SA2 need
     * skip_samples to discard pre-MaskOn data.
     *
     * SDCARD EDF headers and filenames use NTP time.  The STR generator
     * applies the same correction to its AS11-derived MaskOn/MaskOff values. */

    /* TherapyStart timestamp (for EVE/CSL, NTP clock domain). */
    int64_t edf_start_ms = start_epoch_ms;
    char start_date[16], start_time[16];
    format_edf_datetime(edf_start_ms, start_date, sizeof(start_date),
                        start_time, sizeof(start_time));

    /* MaskOn timestamp (for BRP/PLD/SA2, NTP clock domain). */
    int64_t maskon_start_ms = start_epoch_ms;
    uint32_t brp_skip = 0, sa2_skip = 0, pld_skip = 0;
    uint32_t brp_max = 0, sa2_max = 0, pld_max = 0;
    {
        int64_t maskon_as11 = find_mask_on_time(events_snt_path);
        if (maskon_as11 > 0) {
            int64_t maskon_ntp = maskon_as11 + clock_drift_ms;
            if (maskon_ntp > start_epoch_ms && maskon_ntp < end_epoch_ms) {
                maskon_start_ms = maskon_ntp;
                int64_t skip_ms = maskon_ntp - start_epoch_ms;
                brp_skip = (uint32_t)(skip_ms * 25 / 1000);   /* 25 Hz */
                sa2_skip = (uint32_t)(skip_ms * 1 / 1000);     /* 1 Hz */
                pld_skip = (uint32_t)(skip_ms / 2000);         /* 0.5 Hz */
                ESP_LOGI(TAG, "MaskOn: as11=%lld ntp=%lld skip_ms=%lld "
                             "brp_skip=%u sa2_skip=%u pld_skip=%u",
                         (long long)maskon_as11, (long long)maskon_ntp,
                         (long long)skip_ms,
                         (unsigned)brp_skip, (unsigned)sa2_skip,
                         (unsigned)pld_skip);
            } else {
                ESP_LOGW(TAG, "MaskOn NTP time %lld out of session range "
                             "[%lld, %lld], using TherapyStart",
                         (long long)maskon_ntp,
                         (long long)start_epoch_ms,
                         (long long)end_epoch_ms);
            }
        } else {
            ESP_LOGI(TAG, "MaskOn not found in events.snt, using TherapyStart "
                         "for BRP/PLD/SA2");
        }

        /* MaskOff timestamp (for BRP/PLD/SA2 end truncation).
         * AS11 EDF data spans MaskOn→MaskOff, but .snt captures TherapyStart→
         * TherapyStop.  We need to truncate the tail to match MaskOff. */
        int64_t maskoff_as11 = find_mask_off_time(events_snt_path);
        if (maskoff_as11 > 0) {
            int64_t maskoff_ntp = maskoff_as11 + clock_drift_ms;
            if (maskoff_ntp > maskon_start_ms && maskoff_ntp < end_epoch_ms) {
                int64_t dur_ms = maskoff_ntp - maskon_start_ms;
                brp_max = (uint32_t)(dur_ms * 25 / 1000);   /* 25 Hz */
                sa2_max = (uint32_t)(dur_ms * 1 / 1000);     /* 1 Hz */
                pld_max = (uint32_t)(dur_ms / 2000);         /* 0.5 Hz */
                ESP_LOGI(TAG, "MaskOff: as11=%lld ntp=%lld dur_ms=%lld "
                             "brp_max=%u sa2_max=%u pld_max=%u",
                         (long long)maskoff_as11, (long long)maskoff_ntp,
                         (long long)dur_ms,
                         (unsigned)brp_max, (unsigned)sa2_max,
                         (unsigned)pld_max);
            } else {
                ESP_LOGW(TAG, "MaskOff NTP time %lld out of range "
                             "[%lld, %lld], no end truncation",
                         (long long)maskoff_ntp,
                         (long long)maskon_start_ms,
                         (long long)end_epoch_ms);
            }
        } else {
            ESP_LOGI(TAG, "MaskOff not found in events.snt, no end truncation");
        }
    }
    char maskon_date[16], maskon_time[16];
    format_edf_datetime(maskon_start_ms, maskon_date, sizeof(maskon_date),
                        maskon_time, sizeof(maskon_time));
    char maskon_ts_prefix[32];
    session_timestamp(maskon_start_ms, maskon_ts_prefix,
                      sizeof(maskon_ts_prefix));

    /* STR.edf uses the noon-based day date (sessions before noon belong
     * to the previous day's STR.edf). */
    char str_start_date[32];
    {
        time_t t = (time_t)(start_epoch_ms / 1000);
        struct tm tm;
        localtime_r(&t, &tm);
        if (tm.tm_hour < 12) {
            t -= 86400;
            localtime_r(&t, &tm);
        }
        snprintf(str_start_date, sizeof(str_start_date), "%02d.%02d.%02d",
                 tm.tm_mday, tm.tm_mon + 1, tm.tm_year % 100);
    }

    /* Recording ID: TherapyStart for EVE/CSL, MaskOn for BRP/PLD/SA2. */
    char recording_id[128];       /* TherapyStart-based (EVE/CSL) */
    format_recording_id(recording_id, sizeof(recording_id),
                        edf_start_ms, ident);
    char maskon_recording_id[128]; /* MaskOn-based (BRP/PLD/SA2) */
    format_recording_id(maskon_recording_id, sizeof(maskon_recording_id),
                        maskon_start_ms, ident);

    /* STR.edf recording_id uses the noon-based day date, not session start. */
    char str_recording_id[128];
    {
        time_t t = (time_t)(start_epoch_ms / 1000);
        struct tm tm;
        localtime_r(&t, &tm);
        if (tm.tm_hour < 12) t -= 86400;
        int64_t noon_ms = (int64_t)t * 1000;
        format_recording_id(str_recording_id, sizeof(str_recording_id),
                           noon_ms, ident);
    }

    /* Patient ID has CRC filled in by edf_write_header.
     * Initial value is the "X X X X" prefix with placeholder zeros. */
    char patient_id[81] = "X X X X 0000 0000";

    int errors = 0;

    /* ── Generate BRP.edf (25 Hz breath waveform) ── */
    {
        char snt_path[300], edf_path[350];
        snprintf(snt_path, sizeof(snt_path), "%s/%s_brp.snt", session_dir, session_id);
        snprintf(edf_path, sizeof(edf_path), "%s/%s_BRP.edf", day_dir, maskon_ts_prefix);

        edf_signal_def_t brp_sigs[] = {
            { .label = "Flow.40ms", .transducer = "",
              .unit = "L/s", .phys_min = -2.0, .phys_max = 3.0,
              .dig_min = -1000, .dig_max = 1500,
              .prefilter = "", .samples_per_record = 1500 },
            { .label = "Press.40ms", .transducer = "",
              .unit = "cmH2O", .phys_min = 0.0, .phys_max = 40.0,
              .dig_min = 0, .dig_max = 2000,
              .prefilter = "", .samples_per_record = 1500 },
        };
        if (convert_snt_to_edf(snt_path, edf_path, patient_id, maskon_recording_id,
                               maskon_date, maskon_time, brp_sigs, 2, "60.00",
                               NULL, brp_skip, brp_max) != ESP_OK) {
            errors++;
        }
    }

    /* ── Generate SA2.edf (1 Hz SpO2/pulse) ── */
    {
        char snt_path[300], edf_path[350];
        snprintf(snt_path, sizeof(snt_path), "%s/%s_sa2.snt", session_dir, session_id);
        snprintf(edf_path, sizeof(edf_path), "%s/%s_SA2.edf", day_dir, maskon_ts_prefix);

        edf_signal_def_t sa2_sigs[] = {
            { .label = "Pulse.1s", .transducer = "",
              .unit = "bpm", .phys_min = 0.0, .phys_max = 300.0,
              .dig_min = 0, .dig_max = 300,
              .prefilter = "", .samples_per_record = 60,
              .invalid_passthrough = true },
            { .label = "SpO2.1s", .transducer = "",
              .unit = "%", .phys_min = 0.0, .phys_max = 100.0,
              .dig_min = 0, .dig_max = 100,
              .prefilter = "", .samples_per_record = 60,
              .invalid_passthrough = true },
        };
        if (convert_snt_to_edf(snt_path, edf_path, patient_id, maskon_recording_id,
                               maskon_date, maskon_time, sa2_sigs, 2, "60.00",
                               NULL, sa2_skip, sa2_max) != ESP_OK) {
            errors++;
        }
    }

    /* ── Generate PLD.edf (0.5 Hz per-breath stats) ──
     *
     * BLE quantisation note (2026-07-05 reverse-engineering):
     * The AS11 sends PLD values via BLE at phys×100 (integer physical
     * units).  Some channels have coarser BLE resolution than the AS11's
     * internal SD-card EDF:
     *   - RespRate: BLE sends integer bpm; AS11 internal EDF has 0.2 bpm
     *     resolution (dig_max=450, phys_max=90).  ~20 % of samples differ
     *     by ±0.2–1.0 bpm.
     *   - MinVent: BLE sends 0.01 L/min; AS11 internal EDF has 0.125 L/min
     *     resolution.  ~24 % of samples differ by ±0.02–0.12.
     *   - MaskPress: during pressure ramp-up (first ~20 s) BLE and SD sample
     *     at slightly different moments within the 2 s window, causing
     *     ±0.02–0.16 cmH2O differences.  Converges to ≤0.04 once stable.
     * Other channels (Press, EprPress, Leak, TidVol, Snore, FlowLim) match
     * ≥94 % at offset=0.  These differences are a fundamental BLE data-path
     * limitation and cannot be fixed by firmware changes.
     * See spec/archive/edf-as11-comparison-20260629.md §3.6. */
    {
        char snt_path[300], edf_path[350];
        snprintf(snt_path, sizeof(snt_path), "%s/%s_pld.snt", session_dir, session_id);
        snprintf(edf_path, sizeof(edf_path), "%s/%s_PLD.edf", day_dir, maskon_ts_prefix);

        edf_signal_def_t pld_sigs[] = {
            { .label = "MaskPress.2s", .transducer = "",
              .unit = "cmH2O", .phys_min = 0.0, .phys_max = 40.0,
              .dig_min = 0, .dig_max = 2000,
              .prefilter = "", .samples_per_record = 30 },
            { .label = "Press.2s", .transducer = "",
              .unit = "cmH2O", .phys_min = 0.0, .phys_max = 50.0,
              .dig_min = 0, .dig_max = 2500,
              .prefilter = "", .samples_per_record = 30 },
            { .label = "EprPress.2s", .transducer = "",
              .unit = "cmH2O", .phys_min = 0.0, .phys_max = 30.0,
              .dig_min = 0, .dig_max = 1500,
              .prefilter = "", .samples_per_record = 30 },
            { .label = "Leak.2s", .transducer = "",
              .unit = "L/s", .phys_min = 0.0, .phys_max = 2.0,
              .dig_min = 0, .dig_max = 100,
              .prefilter = "", .samples_per_record = 30 },
            { .label = "RespRate.2s", .transducer = "",
              .unit = "bpm", .phys_min = 0.0, .phys_max = 90.0,
              .dig_min = 0, .dig_max = 450,
              .prefilter = "", .samples_per_record = 30 },
            { .label = "TidVol.2s", .transducer = "",
              .unit = "L", .phys_min = 0.0, .phys_max = 4.0,
              .dig_min = 0, .dig_max = 200,
              .prefilter = "", .samples_per_record = 30 },
            { .label = "MinVent.2s", .transducer = "",
              .unit = "L/min", .phys_min = 0.0, .phys_max = 30.0,
              .dig_min = 0, .dig_max = 240,
              .prefilter = "", .samples_per_record = 30 },
            { .label = "Snore.2s", .transducer = "",
              .unit = "", .phys_min = 0.0, .phys_max = 5.0,
              .dig_min = 0, .dig_max = 250,
              .prefilter = "", .samples_per_record = 30 },
            { .label = "FlowLim.2s", .transducer = "",
              .unit = "", .phys_min = 0.0, .phys_max = 1.0,
              .dig_min = 0, .dig_max = 100,
              .prefilter = "", .samples_per_record = 30,
              .invalid_passthrough = true },
        };
        /* PLD .snt has 12 channels but AS11 EDF (VID=3) only has 9.
         * Channel order in .snt: 0=MaskPress, 1=Press, 2=EprPress, 3=Leak,
         * 4=RespRate, 5=TidVol, 6=MinVent, 7=TgtVent, 8=IERatio,
         * 9=Snore, 10=FlowLim, 11=Ti
         * EDF drops TgtVent(7), IERatio(8), Ti(11). */
        static const int pld_ch_map[] = {0, 1, 2, 3, 4, 5, 6, 9, 10};
        if (convert_snt_to_edf(snt_path, edf_path, patient_id, maskon_recording_id,
                               maskon_date, maskon_time, pld_sigs, 9, "60.00",
                               pld_ch_map, pld_skip, pld_max) != ESP_OK) {
            errors++;
        }
    }

    /* ── Generate STR.edf from per-day summary spool files ──
     * STR.edf goes in the SDCARD root (not inside DATALOG/) — it is a
     * multi-day cumulative file with one record per day. */
    if (generate_str_edf(SD_SDCARD_DIR, patient_id, str_recording_id,
                         str_start_date, settings,
                         session_dir, session_id,
                         start_epoch_ms, end_epoch_ms, clock_drift_ms) != ESP_OK) {
        errors++;
    }

    /* ── Check if session is long enough for EVE/CSL ──
     * AS11 does not write EVE.edf or CSL.edf for sessions shorter than
     * one data record (60 seconds).  Match this behaviour by checking
     * the BRP .snt sample count — if it's less than one record's worth
     * (1500 samples at 25 Hz), skip EVE/CSL generation. */
    bool session_too_short = false;
    {
        char brp_snt[300];
        snprintf(brp_snt, sizeof(brp_snt), "%s/%s_brp.snt", session_dir, session_id);
        FILE *bf = fopen(brp_snt, "rb");
        if (bf) {
            snt_header_t bhdr;
            if (snt_read_header(bf, &bhdr) == ESP_OK) {
                int brp_spr = 1500;  /* BRP samples per 60s record */
                if (bhdr.sample_count < (uint32_t)brp_spr) {
                    session_too_short = true;
                    ESP_LOGI(TAG, "session %s too short (%u BRP samples < %d), "
                             "skipping EVE/CSL", session_id,
                             (unsigned)bhdr.sample_count, brp_spr);
                }
            }
            fclose(bf);
        }
    }

    /* ── Generate EVE.edf from events.snt ──
     * Both event onsets and the EDF header use NTP-corrected time. */
    if (!session_too_short) {
        char eve_path[350];
        snprintf(eve_path, sizeof(eve_path), "%s/%s_EVE.edf", day_dir, ts_prefix);
        if (generate_eve_edf(eve_path, events_snt_path,
                             start_epoch_ms, clock_drift_ms,
                             patient_id, recording_id,
                             start_date, start_time, false) != ESP_OK) {
            errors++;
        }

        /* ── Generate CSL.edf (CSR event log) from events.snt ──
         * CSL.edf contains only CSR (Cheyne-Stokes Respiration) events.
         * For sessions with no CSR events, CSL.edf contains only the
         * "Recording starts" marker record. */
        char csl_path[350];
        snprintf(csl_path, sizeof(csl_path), "%s/%s_CSL.edf", day_dir, ts_prefix);
        if (generate_eve_edf(csl_path, events_snt_path,
                             start_epoch_ms, clock_drift_ms,
                             patient_id, recording_id,
                             start_date, start_time, true) != ESP_OK) {
            errors++;
        }
    }

    /* ── Generate Identification.json + .crc ── */
    if (generate_identification(SD_SDCARD_DIR, ident_path) != ESP_OK) {
        errors++;
    }

    /* ── Copy settings to SDCARD/SETTINGS/CurrentSettings.json ──
     * The device returns {"SettingProfiles":{...}} but AS11 nests this under
     * a "FlowGenerator" wrapper: {"FlowGenerator":{"SettingProfiles":{...}}}.
     * Use a reference wrapper so the original `settings` tree is freed once
     * below (cJSON_AddItemReferenceToObject does not transfer ownership). */
    if (settings) {
        cJSON *cs_root = cJSON_CreateObject();
        cJSON_AddItemReferenceToObject(cs_root, "FlowGenerator", settings);
        char *settings_str = cJSON_PrintUnformatted(cs_root);
        cJSON_Delete(cs_root);
        if (settings_str) {
            /* cJSON drops ".0" for integer-valued doubles (e.g. 7.0 → 7),
             * but AS11 preserves it for pressure/temperature fields.
             * Post-process the string to add ".0" back for known float
             * fields so the output matches AS11 byte-for-byte. */
            static const char *const float_fields[] = {
                "StartPressure", "MaxPressure", "MinPressure",
                "SetPressure", "HeatedTubeTemperature", NULL
            };
            for (int fi = 0; float_fields[fi]; fi++) {
                char pattern[64];
                snprintf(pattern, sizeof(pattern), "\"%s\":", float_fields[fi]);
                size_t plen = strlen(pattern);
                char *p = settings_str;
                while ((p = strstr(p, pattern)) != NULL) {
                    char *val_start = p + plen;
                    /* Skip optional minus sign */
                    char *v = val_start;
                    if (*v == '-') v++;
                    /* Check if value is purely integer (all digits, no '.') */
                    char *scan = v;
                    while (*scan >= '0' && *scan <= '9') scan++;
                    if (scan > v && *scan != '.') {
                        /* Integer value — insert ".0" before the terminator */
                        size_t insert_pos = scan - settings_str;
                        size_t tail_len = strlen(scan) + 1; /* includes NUL */
                        /* Realloc to make room for 2 extra bytes */
                        size_t old_len = strlen(settings_str);
                        char *tmp = realloc(settings_str, old_len + 3);
                        if (tmp) {
                            settings_str = tmp;
                            memmove(settings_str + insert_pos + 2,
                                    settings_str + insert_pos, tail_len);
                            settings_str[insert_pos] = '.';
                            settings_str[insert_pos + 1] = '0';
                        }
                        p = settings_str + insert_pos + 2;
                    } else {
                        p = scan;
                    }
                }
            }
            size_t slen = strlen(settings_str);
            char cs_path[300];
            char cs_tmp[380];
            snprintf(cs_path, sizeof(cs_path), "%s/CurrentSettings.json", SD_SDCARD_SETTINGS);
            FILE *csf = open_atomic_file(cs_path, cs_tmp, sizeof(cs_tmp));
            if (csf) {
                if (!write_all(csf, settings_str, slen)) {
                    ESP_LOGE(TAG, "cannot write %s: %s", cs_path, strerror(errno));
                    discard_atomic_file(csf, cs_tmp);
                } else if (finalize_atomic_file(csf, cs_tmp, cs_path) != ESP_OK) {
                    ESP_LOGE(TAG, "cannot finalize %s: %s", cs_path, strerror(errno));
                } else {
                    ESP_LOGI(TAG, "wrote %s", cs_path);
                }
            } else {
                ESP_LOGE(TAG, "cannot create %s: %s", cs_path, strerror(errno));
            }
            /* Write CurrentSettings.crc (CRC-32 LE, same format as Identification.crc) */
            snprintf(cs_path, sizeof(cs_path), "%s/CurrentSettings.crc", SD_SDCARD_SETTINGS);
            csf = open_atomic_file(cs_path, cs_tmp, sizeof(cs_tmp));
            if (csf) {
                uint32_t cs_crc = crc32_ieee((const uint8_t *)settings_str, slen);
                uint8_t crc_bytes[4] = {
                    (uint8_t)(cs_crc & 0xFF),
                    (uint8_t)((cs_crc >> 8) & 0xFF),
                    (uint8_t)((cs_crc >> 16) & 0xFF),
                    (uint8_t)((cs_crc >> 24) & 0xFF),
                };
                if (!write_all(csf, crc_bytes, 4)) {
                    ESP_LOGE(TAG, "cannot write %s: %s", cs_path, strerror(errno));
                    discard_atomic_file(csf, cs_tmp);
                } else if (finalize_atomic_file(csf, cs_tmp, cs_path) != ESP_OK) {
                    ESP_LOGE(TAG, "cannot finalize %s: %s", cs_path, strerror(errno));
                } else {
                    ESP_LOGI(TAG, "wrote %s (crc32=0x%08X)", cs_path, (unsigned)cs_crc);
                }
            } else {
                ESP_LOGE(TAG, "cannot create %s: %s", cs_path, strerror(errno));
            }
            free(settings_str);
        }
    }

    /* ── Cleanup ── */
    if (ident) cJSON_Delete(ident);
    if (settings) cJSON_Delete(settings);
    /* events_data no longer used */

    ESP_LOGI(TAG, "=== EDF GENERATION DONE (%d errors) ===", errors);
    return errors > 0 ? ESP_FAIL : ESP_OK;
}
