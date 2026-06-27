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
#include <time.h>
#include <errno.h>

#include "esp_log.h"
#include "esp_timer.h"
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

    /* Write fixed header */
    fwrite(hdr, 1, 256, f);

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

    /* Write signal header blocks */
    fwrite(sigblock, 1, 256 * total, f);

    free(sigblock);
    return header_bytes;
}

/* Compute the patient ID CRC words after the header has been written.
 * Reads back the file to compute CRC16 over the appropriate byte ranges.
 * Then rewrites the patient ID field with the CRC values filled in.
 *
 * Patient ID format: "X X X X AAAA BBBB" where:
 *   AAAA = CRC16-CCITT-FALSE of fixed header bytes 0x19..0xFF
 *   BBBB = CRC16-CCITT-FALSE of signal header bytes 0x100..header_bytes-1
 */
static void edf_finalise_crc(FILE *f, int header_bytes)
{
    /* Read the entire header back into memory.
     * The file must have been opened with "w+b" (read+write) mode. */
    uint8_t *hdr = malloc(header_bytes);
    if (!hdr) {
        ESP_LOGE(TAG, "edf_finalise_crc: malloc(%d) failed", header_bytes);
        return;
    }

    fseek(f, 0, SEEK_SET);
    if (fread(hdr, 1, header_bytes, f) != (size_t)header_bytes) {
        ESP_LOGE(TAG, "edf_finalise_crc: fread header failed (header_bytes=%d)",
                 header_bytes);
        free(hdr);
        return;
    }

    /* First CRC: bytes 0x19..0xFF (offset 25 to 255, 231 bytes) */
    uint16_t crc1 = crc16_ccitt(hdr + 0x19, 256 - 0x19);

    /* Second CRC: bytes 0x100..header_bytes-1 (all signal header blocks) */
    uint16_t crc2 = crc16_ccitt(hdr + 256, header_bytes - 256);

    ESP_LOGI(TAG, "edf_finalise_crc: H1=%04X H2=%04X", crc1, crc2);

    /* Write the CRC values into the patient ID field at offset 0x08.
     * Format: "X X X X AAAA BBBB" (uppercase hex, left-aligned, space-padded to 80).
     * The "X X X X " prefix is already in the header; we just need to
     * write the two hex words at the correct offset. */
    char pid[81];
    snprintf(pid, sizeof(pid), "X X X X %04X %04X", crc1, crc2);
    /* Pad to 80 chars with spaces */
    int plen = strlen(pid);
    while (plen < 80) pid[plen++] = ' ';
    pid[80] = '\0';

    fseek(f, 8, SEEK_SET);  /* patient ID starts at offset 0x08 */
    fwrite(pid, 1, 80, f);
    fflush(f);

    free(hdr);
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
 */
static esp_err_t convert_snt_to_edf(const char *snt_path, const char *edf_path,
                                    const char *patient_id, const char *recording_id,
                                    const char *start_date, const char *start_time,
                                    const edf_signal_def_t *signals, int n_signals,
                                    const char *record_dur,
                                    const int *channel_map)
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

    /* Total samples (per channel) and record count.
     * EDF records are 60 seconds each.  Sessions shorter than 60 seconds
     * produce 0 data records (header-only file), matching AS11 behaviour. */
    uint32_t total_samples = hdr.sample_count;
    int total_records = total_samples / spr[0];
    if (total_samples > 0 && total_records == 0) {
        ESP_LOGI(TAG, "%s: short session (%u samples < %d spr), writing 0 records",
                 snt_path, total_samples, spr[0]);
    }
    if (total_records < 0) total_records = 0;

    ESP_LOGI(TAG, "converting %s → %s: %u samples, %d records, %d ch",
             snt_path, edf_path, total_samples, total_records, n_signals);

    /* Create EDF file and write header */
    FILE *edf = fopen(edf_path, "w+b");
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
        fclose(edf);
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
    int16_t *record_buf = malloc(record_bytes);       /* de-interleaved output */
    if (!raw || !record_buf) {
        ESP_LOGE(TAG, "malloc record buffers failed");
        free(raw); free(record_buf); free(spr); free(sig);
        fclose(edf);
        fclose(snt);
        return ESP_ERR_NO_MEM;
    }

    for (int rec = 0; rec < total_records; rec++) {
        /* Read one record's worth of interleaved samples from .snt.
         * The last record may be partial (short session) — zero-pad
         * the record buffer for any missing samples. */
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
                ESP_LOGW(TAG, "short read at record %d, stopping", rec);
                break;
            }
        }

        /* De-interleave: [ch0_s0, ch1_s0, ...] → [ch0_all, ch1_all, ...]
         * Samples beyond available data remain zero (from memset above).
         * If channel_map is provided, select only the mapped channels. */
        int avail_samples = (int)(avail / (snt_channels * sizeof(int16_t)));
        for (int ch = 0; ch < n_signals; ch++) {
            int snt_ch = channel_map ? channel_map[ch] : ch;
            for (int s = 0; s < samples_per_record; s++) {
                if (s < avail_samples) {
                    record_buf[ch * samples_per_record + s] = raw[s * snt_channels + snt_ch];
                } else {
                    record_buf[ch * samples_per_record + s] = 0;
                }
            }
        }

        /* Write de-interleaved signal data to EDF */
        fwrite(record_buf, sizeof(int16_t), record_data_samples, edf);

        /* Compute CRC16 over the signal data bytes and write as int16 */
        uint16_t crc = crc16_ccitt((uint8_t *)record_buf, record_bytes);
        int16_t crc_val = (int16_t)crc;
        fwrite(&crc_val, sizeof(int16_t), 1, edf);
    }

    free(raw);
    free(record_buf);
    free(spr);
    free(sig);

    /* Finalise CRC in patient ID */
    edf_finalise_crc(edf, header_bytes);

    fclose(edf);
    fclose(snt);
    ESP_LOGI(TAG, "EDF conversion complete: %s", edf_path);
    return ESP_OK;
}

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

/* Generate STR.edf from Summary spool protobuf data.
 * Also generates CSL.edf (empty or from CSR field). */
static esp_err_t generate_str_edf(const char *edf_dir,
                                  const uint8_t *summary_data, size_t summary_len,
                                  const char *patient_id, const char *recording_id,
                                  const char *start_date, const char *start_time,
                                  int64_t start_epoch_ms, int64_t end_epoch_ms,
                                  const cJSON *settings_json,
                                  const char *session_dir)
{
    /* Parse the Summary protobuf to extract session statistics.
     * The top-level message has repeated field-2 wrappers, each containing
     * a session summary record.  We take the first (or most recent) record.
     *
     * Large structs are heap-allocated to keep the task stack small
     * (summary_ctx_t is ~5KB, str_sigs is ~6KB, str_values is 268B).
     * These will be placed in PSRAM by the heap allocator since they
     * exceed CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL (16KB total). */
    summary_ctx_t *ctx = calloc(1, sizeof(summary_ctx_t));
    int16_t *str_values = malloc(STR_DATA_COUNT * sizeof(int16_t));
    edf_signal_def_t *str_sigs = calloc(STR_DATA_COUNT, sizeof(edf_signal_def_t));
    if (!ctx || !str_values || !str_sigs) {
        ESP_LOGE(TAG, "STR.edf: malloc failed for ctx/values/sigs");
        free(ctx); free(str_values); free(str_sigs);
        return ESP_ERR_NO_MEM;
    }
    /* Initialise all STR values to -1 (sentinel for "no data") */
    memset(str_values, 0xFF, STR_DATA_COUNT * sizeof(int16_t));

    /* Find field-2 wrapper records */
    bool found_record = false;
    size_t pos = 0;
    while (pos < summary_len) {
        uint64_t tag = pb_decode_varint(summary_data, summary_len, &pos);
        int field = (int)(tag >> 3);
        int wire = (int)(tag & 0x07);
        if (field == 0) break;

        if (field == 2 && wire == 2) {
            /* Field-2 wrapper: contains a Summary record */
            size_t lpos = pos;
            uint64_t flen = pb_decode_varint(summary_data, summary_len, &lpos);
            pos = lpos;
            if (pos + flen > summary_len) break;

            /* Parse this record */
            pb_iter(summary_data + pos, (size_t)flen, summary_field_cb, ctx);
            found_record = true;
            pos += flen;
            /* Use the first record for now */
            break;
        } else if (wire == 2) {
            size_t lpos = pos;
            uint64_t flen = pb_decode_varint(summary_data, summary_len, &lpos);
            pos = lpos + flen;
        } else if (wire == 0) {
            pb_decode_varint(summary_data, summary_len, &pos);
        } else if (wire == 1) {
            pos += 8;
        } else if (wire == 5) {
            pos += 4;
        } else {
            break;
        }
    }

    if (!found_record) {
        ESP_LOGW(TAG, "STR.edf: no Summary record found in spool data");
        /* Still write a minimal STR.edf with default values */
    }

    /* Build the 77-field STR record for VID=3 (AutoSet).
     * Many fields come from the Summary spool; settings fields come from
     * settings.json (obtained via Get RPC).  We use settings_json as the
     * primary source for settings, falling back to Summary spool if needed.
     *
     * str_values is already heap-allocated and zeroed above. */

    /* Session header [0-3]: Date, MaskOn, MaskOff, MaskEvents
     * Date = days from Unix epoch (Jan 1, 1970) in local time (noon-based day)
     * MaskOn/MaskOff = minutes from noon, up to 20 entries each
     * MaskEvents = count of mask on/off events
     *
     * MaskOn/MaskOff events are read from events.snt (live EventNotification
     * JSON lines captured by session_writer).  Each line is a JSON object with
     * params.events[] containing {event/label, timestamp} fields. */
    {
        time_t start_t = (time_t)(start_epoch_ms / 1000);
        time_t end_t = (time_t)(end_epoch_ms / 1000);
        struct tm tm_start, tm_end;
        localtime_r(&start_t, &tm_start);
        localtime_r(&end_t, &tm_end);

        /* Date: days from Unix epoch, using noon-based day.
         * If session starts before noon, it belongs to the previous day. */
        time_t noon_t = start_t;
        if (tm_start.tm_hour < 12) {
            noon_t -= 86400;
        }
        struct tm epoch_tm = { .tm_year=70, .tm_mon=0, .tm_mday=1,
                               .tm_hour=0, .tm_min=0, .tm_sec=0 };
        time_t epoch_t = mktime(&epoch_tm);
        str_values[0] = (int16_t)((noon_t - epoch_t) / 86400);

        /* Compute noon-based epoch for minute-from-noon calculations */
        int64_t noon_epoch_ms = (int64_t)noon_t * 1000;

        /* Parse events.snt for MaskOn/MaskOff events */
        int mask_on_count = 0;
        int mask_off_count = 0;
        char events_path[350];
        snprintf(events_path, sizeof(events_path), "%s/events.snt", session_dir);
        FILE *ef = fopen(events_path, "r");
        if (ef) {
            char line[512];
            while (fgets(line, sizeof(line), ef) && mask_on_count < 20 && mask_off_count < 20) {
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
                            if (!label || !cJSON_IsString(label))
                                label = cJSON_GetObjectItem(ev, "label");
                            if (!label || !cJSON_IsString(label)) continue;
                            cJSON *ts = cJSON_GetObjectItem(ev, "timestamp");
                            if (!ts || !cJSON_IsNumber(ts)) continue;
                            int64_t ev_ms = (int64_t)ts->valuedouble;
                            int min_from_noon = (int)((ev_ms - noon_epoch_ms) / 60000);
                            if (strcmp(label->valuestring, "MaskOn") == 0 && mask_on_count < 20) {
                                str_values[1 + mask_on_count] = (int16_t)min_from_noon;
                                mask_on_count++;
                            } else if (strcmp(label->valuestring, "MaskOff") == 0 && mask_off_count < 20) {
                                str_values[21 + mask_off_count] = (int16_t)min_from_noon;
                                mask_off_count++;
                            }
                        }
                    }
                }
                cJSON_Delete(msg);
            }
            fclose(ef);
        }

        /* If no MaskOn events found, use session start as fallback */
        if (mask_on_count == 0) {
            int start_min = tm_start.tm_hour * 60 + tm_start.tm_min - 720;
            if (start_min < 0) start_min += 1440;
            str_values[1] = (int16_t)start_min;
            mask_on_count = 1;
        }
        /* If no MaskOff events found, use session end as fallback */
        if (mask_off_count == 0) {
            int end_min = tm_end.tm_hour * 60 + tm_end.tm_min - 720;
            if (end_min < 0) end_min += 1440;
            str_values[21] = (int16_t)end_min;
            mask_off_count = 1;
        }

        /* MaskEvents = max(mask_on_count, mask_off_count) */
        str_values[3] = (int16_t)(mask_on_count > mask_off_count ? mask_on_count : mask_off_count);

        ESP_LOGI(TAG, "STR.edf: MaskOn=%d MaskOff=%d MaskEvents=%d",
                 mask_on_count, mask_off_count, str_values[3]);
    }

    /* Session core [4-5] */
    str_values[4] = get_scalar(ctx, SUM_F_DURATION_MIN, 0);  /* Duration */
    int mode_raw = get_scalar(ctx, SUM_F_SESSION_MODE, 0);
    /* Apply enum export map for Mode */
    if (mode_raw >= 0 && mode_raw < (int)(sizeof(MODE_MAP) / sizeof(MODE_MAP[0]))) {
        str_values[5] = MODE_MAP[mode_raw];  /* Mode */
    } else {
        str_values[5] = mode_raw;
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

            /* [14] S.AS.Comfort: "Plus"→1, "On"→2 */
            if (comfort) {
                v = cJSON_GetObjectItem(comfort, "AutoSetComfort");
                if (v && cJSON_IsString(v)) {
                    if (strcmp(v->valuestring, "On") == 0) str_values[14] = 2;
                    else if (strcmp(v->valuestring, "Plus") == 0) str_values[14] = 1;
                }
            }

            /* [15] S.RampEnable: "Off"→1, "On"→2, "Auto"→3 */
            /* [16] S.RampTime: direct minutes */
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

            /* [17-20] EPR settings */
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

            /* [21] S.SmartStart */
            if (smart) {
                v = cJSON_GetObjectItem(smart, "SmartStart");
                if (v && cJSON_IsString(v)) str_values[21] = (int16_t)on_off_to_edf(v->valuestring);
            }

            /* [22] S.PtAccess (PatientView) */
            if (patview) {
                v = cJSON_GetObjectItem(patview, "PatientView");
                if (v && cJSON_IsString(v)) {
                    if (strcmp(v->valuestring, "Advanced") == 0) str_values[22] = 1;
                    else if (strcmp(v->valuestring, "Basic") == 0) str_values[22] = 2;
                }
            }

            /* [23] S.ABFilter, [24] S.Mask, [25] S.Tube */
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

            /* [26-30] Climate settings */
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

    /* [31] HeatedTube and [32] Humidifier: from Summary spool fields.
     * These are hardware connection status, not user settings. */
    str_values[31] = get_scalar(ctx, SUM_F_TUBE_CONNECTED, -1);   /* HeatedTube */
    str_values[32] = get_scalar(ctx, SUM_F_HUM_CONNECTED, -1);    /* Humidifier */

    /* Environment and oximetry stats [33-46] */
    str_values[33] = get_metric(ctx, SUM_F_BLOWER_PRESS, 3, 0);   /* BlowPress.95 */
    str_values[34] = get_metric(ctx, SUM_F_BLOWER_PRESS, 1, 0);   /* BlowPress.5 */
    str_values[35] = get_metric(ctx, SUM_F_RESP_FLOW, 3, 0);      /* Flow.95 */
    str_values[36] = get_metric(ctx, SUM_F_RESP_FLOW, 1, 0);      /* Flow.5 */
    str_values[37] = get_metric(ctx, SUM_F_BLOWER_FLOW, 2, 0);    /* BlowFlow.50 */
    str_values[38] = get_metric(ctx, SUM_F_AMB_HUMID, 2, 0);      /* AmbHumidity.50 */
    str_values[39] = get_metric(ctx, SUM_F_HUM_TEMP, 2, 0);       /* HumTemp.50 */
    str_values[40] = get_metric(ctx, SUM_F_HTUBE_TEMP, 2, 0);     /* HTubeTemp.50 */
    str_values[41] = get_metric(ctx, SUM_F_HTUBE_POWER, 2, 0);    /* HTubePow.50 */
    str_values[42] = get_metric(ctx, SUM_F_HUM_POWER, 2, 0);      /* HumPow.50 */
    str_values[43] = get_metric(ctx, SUM_F_SPO2, 2, 0);           /* SpO2.50 */
    str_values[44] = get_metric(ctx, SUM_F_SPO2, 3, 0);           /* SpO2.95 */
    str_values[45] = get_metric(ctx, SUM_F_SPO2, 4, 0);           /* SpO2.Max */
    str_values[46] = get_scalar(ctx, SUM_F_SAU, 0);               /* SpO2Thresh */

    /* Bilevel/ventilation summary stats [47-68] */
    str_values[47] = get_metric(ctx, SUM_F_MEAN_MASK_PRESS, 2, 0);  /* MaskPress.50 */
    str_values[48] = get_metric(ctx, SUM_F_MEAN_MASK_PRESS, 3, 0);  /* MaskPress.95 */
    str_values[49] = get_metric(ctx, SUM_F_MEAN_MASK_PRESS, 4, 0);  /* MaskPress.Max */
    str_values[50] = get_metric(ctx, SUM_F_INSP_PRESS, 2, 0);       /* TgtIPAP.50 */
    str_values[51] = get_metric(ctx, SUM_F_INSP_PRESS, 3, 0);       /* TgtIPAP.95 */
    str_values[52] = get_metric(ctx, SUM_F_INSP_PRESS, 4, 0);       /* TgtIPAP.Max */
    str_values[53] = get_metric(ctx, SUM_F_EXP_PRESS, 2, 0);        /* TgtEPAP.50 */
    str_values[54] = get_metric(ctx, SUM_F_EXP_PRESS, 3, 0);        /* TgtEPAP.95 */
    str_values[55] = get_metric(ctx, SUM_F_EXP_PRESS, 4, 0);        /* TgtEPAP.Max */
    str_values[56] = get_metric(ctx, SUM_F_LEAK, 2, 0);             /* Leak.50 */
    str_values[57] = get_metric(ctx, SUM_F_LEAK, 3, 0);             /* Leak.95 */
    str_values[58] = get_metric(ctx, SUM_F_LEAK, 4, 0);             /* Leak.70 */
    str_values[59] = get_metric(ctx, SUM_F_LEAK, 5, 0);             /* Leak.Max */
    str_values[60] = get_metric(ctx, SUM_F_MIN_VENT, 2, 0);         /* MinVent.50 */
    str_values[61] = get_metric(ctx, SUM_F_MIN_VENT, 3, 0);         /* MinVent.95 */
    str_values[62] = get_metric(ctx, SUM_F_MIN_VENT, 4, 0);         /* MinVent.Max */
    str_values[63] = get_metric(ctx, SUM_F_RESP_RATE, 2, 0);        /* RespRate.50 */
    str_values[64] = get_metric(ctx, SUM_F_RESP_RATE, 3, 0);        /* RespRate.95 */
    str_values[65] = get_metric(ctx, SUM_F_RESP_RATE, 4, 0);        /* RespRate.Max */
    str_values[66] = get_metric(ctx, SUM_F_TIDAL_VOL, 2, 0);        /* TidVol.50 */
    str_values[67] = get_metric(ctx, SUM_F_TIDAL_VOL, 3, 0);        /* TidVol.95 */
    str_values[68] = get_metric(ctx, SUM_F_TIDAL_VOL, 4, 0);        /* TidVol.Max */

    /* Indices and CSR [69-76] */
    str_values[69] = get_scalar(ctx, SUM_F_AHI, 0);    /* AHI */
    str_values[70] = get_scalar(ctx, SUM_F_HI, 0);     /* HI */
    str_values[71] = get_scalar(ctx, SUM_F_AI, 0);     /* AI */
    str_values[72] = get_scalar(ctx, SUM_F_OAI, 0);    /* OAI */
    str_values[73] = get_scalar(ctx, SUM_F_CAI, 0);    /* CAI */
    str_values[74] = get_scalar(ctx, SUM_F_UAI, 0);    /* UAI */
    str_values[75] = get_scalar(ctx, SUM_F_RIN, 0);    /* RIN */
    str_values[76] = get_scalar(ctx, SUM_F_CSR, 0);    /* CSR */

    /* Build STR.edf signal definitions with per-signal metadata matching
     * AS11 native VID=3 STR.edf (units, phys/dig ranges, samples_per_record). */
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

    /* Create STR.edf */
    char path[300];
    snprintf(path, sizeof(path), "%s/STR.edf", edf_dir);

    FILE *edf = fopen(path, "w+b");
    if (!edf) {
        ESP_LOGE(TAG, "cannot create %s: %s", path, strerror(errno));
        return ESP_FAIL;
    }

    /* Copy static signal defs into the heap-allocated str_sigs array */
    memcpy(str_sigs, str_signal_defs, STR_DATA_COUNT * sizeof(edf_signal_def_t));

    /* STR.edf uses 12.00.00 noon as start time, with the noon-based day date. */
    const char *str_start_time = "12.00.00";

    int header_bytes = edf_write_header(edf, patient_id, recording_id,
                                        start_date, str_start_time,
                                        1,  /* 1 data record */
                                        "86400.00",
                                        "EDF", str_sigs, STR_DATA_COUNT);
    if (header_bytes < 0) {
        ESP_LOGE(TAG, "STR.edf: edf_write_header failed");
        fclose(edf);
        return ESP_FAIL;
    }

    /* Write the single data record.
     * Most signals have spr=1, but MaskOn and MaskOff have spr=20.
     * Total data samples = 1 + 20 + 20 + 1 + 73 = 115 int16.
     * Plus Crc16 (spr=1) = 116 int16 total = 232 bytes. */
    int16_t rec_buf[115];
    int rec_pos = 0;
    for (int i = 0; i < STR_DATA_COUNT; i++) {
        int spr = str_signal_defs[i].samples_per_record;
        rec_buf[rec_pos++] = str_values[i];
        for (int s = 1; s < spr; s++) {
            rec_buf[rec_pos++] = -1;  /* pad unused slots with sentinel */
        }
    }
    /* rec_pos must be 115 (1+20+20+1+73) */
    if (rec_pos != 115) {
        ESP_LOGE(TAG, "STR.edf: internal error: record buffer rec_pos=%d != 115", rec_pos);
        fclose(edf);
        return ESP_FAIL;
    }

    /* Compute CRC16 over the 115 data signal values (230 bytes) */
    uint16_t crc = crc16_ccitt((uint8_t *)rec_buf, 115 * sizeof(int16_t));

    /* Write data record (115 data values + Crc16) */
    fwrite(rec_buf, sizeof(int16_t), 115, edf);
    int16_t crc_val = (int16_t)crc;
    fwrite(&crc_val, sizeof(int16_t), 1, edf);

    /* Finalise CRC in patient ID */
    edf_finalise_crc(edf, header_bytes);
    fclose(edf);

    ESP_LOGI(TAG, "STR.edf generated: %s", path);

    /* CSL.edf is generated by the caller (edf_gen_generate) from the
     * same events spool data, alongside EVE.edf. */

    free(ctx);
    free(str_values);
    free(str_sigs);
    return ESP_OK;
}

/* ════════════════════════════════════════════════════════════════════
 *  Section 7: EVE.edf generation from respiratory events spool
 * ════════════════════════════════════════════════════════════════════
 *
 * EVE.edf is an EDF+D annotation file containing respiratory event
 * annotations.  Each data record contains:
 *   - 62 bytes: EDF+ TAL annotation payload
 *   - 2 bytes:  little-endian CRC16 over the 62 payload bytes
 *
 * The first record is always a "Recording starts" marker.
 * Subsequent records contain one event TAL each.
 *
 * Event labels (from edf_annotations.md):
 *   HypopneaEnd     → "Hypopnea"
 *   CentralApneaEnd → "Central Apnea"
 *   ObstructiveApneaEnd → "Obstructive Apnea"
 *   ApneaEnd        → "Apnea"
 *   ReraEnd         → "Arousal"
 */

/* Event protobuf field numbers (from rpc_spools.md event family) */
#define EVT_F_TIMESTAMP    1   /* event timestamp (varint, epoch ms) */
#define EVT_F_TYPE         2   /* event type string (length-delimited) */
#define EVT_F_DURATION     3   /* event duration in seconds (varint) */

/* Context for event protobuf iteration */
typedef struct {
    int64_t timestamp_ms;
    char type[64];
    int64_t duration_sec;
} event_record_t;

static void event_field_cb(const pb_field_t *f, void *ud)
{
    event_record_t *evt = (event_record_t *)ud;
    if (f->field == EVT_F_TIMESTAMP && f->wire == 0) {
        evt->timestamp_ms = pb_varint_val(f);
    } else if (f->field == EVT_F_TYPE && f->wire == 2 && f->data && f->len > 0) {
        size_t len = f->len < sizeof(evt->type) - 1 ? f->len : sizeof(evt->type) - 1;
        memcpy(evt->type, f->data, len);
        evt->type[len] = '\0';
    } else if (f->field == EVT_F_DURATION && f->wire == 0) {
        evt->duration_sec = pb_varint_val(f);
    }
}

/* Map RPC event type to EDF annotation label. */
static const char *event_label_map(const char *rpc_type)
{
    if (!rpc_type) return NULL;
    if (strcmp(rpc_type, "HypopneaEnd") == 0) return "Hypopnea";
    if (strcmp(rpc_type, "CentralApneaEnd") == 0) return "Central Apnea";
    if (strcmp(rpc_type, "ObstructiveApneaEnd") == 0) return "Obstructive Apnea";
    if (strcmp(rpc_type, "ApneaEnd") == 0) return "Apnea";
    if (strcmp(rpc_type, "ReraEnd") == 0) return "Arousal";
    return NULL;  /* unknown event type — skip */
}

static esp_err_t generate_eve_edf(const char *edf_path,
                                  const uint8_t *events_data, size_t events_len,
                                  int64_t session_start_ms, int64_t clock_drift_ms,
                                  const char *patient_id, const char *recording_id,
                                  const char *start_date, const char *start_time)
{
    char path[350];
    strncpy(path, edf_path, sizeof(path) - 1);
    path[sizeof(path) - 1] = '\0';

    /* Parse events from the protobuf spool data.
     * The spool contains repeated event records, each a protobuf message
     * with timestamp, type, and duration fields. */
    /* For now, if we have no event data, write a minimal EVE.edf with
     * just the "Recording starts" marker. */

    /* Count events we can decode */
    int event_count = 0;
    if (events_data && events_len > 0) {
        size_t pos = 0;
        while (pos < events_len) {
            uint64_t tag = pb_decode_varint(events_data, events_len, &pos);
            int field = (int)(tag >> 3);
            int wire = (int)(tag & 0x07);
            if (field == 0) break;
            if (wire == 2) {
                size_t lpos = pos;
                uint64_t flen = pb_decode_varint(events_data, events_len, &lpos);
                pos = lpos;
                if (pos + flen > events_len) break;
                /* Check if this is an event wrapper (field 7 per rpc_spools.md) */
                /* Try to parse as event record */
                event_record_t evt = {0};
                pb_iter(events_data + pos, (size_t)flen, event_field_cb, &evt);
                if (evt.type[0] && event_label_map(evt.type)) {
                    event_count++;
                }
                pos += flen;
            } else if (wire == 0) {
                pb_decode_varint(events_data, events_len, &pos);
            } else if (wire == 1) {
                pos += 8;
            } else if (wire == 5) {
                pos += 4;
            } else {
                break;
            }
        }
    }

    /* Total records = 1 (Recording starts) + event_count */
    int total_records = 1 + event_count;

    ESP_LOGI(TAG, "EVE.edf: %d events, %d total records", event_count, total_records);

    /* EVE.edf signal definitions: EDF Annotations + Crc16 */
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

    FILE *edf = fopen(path, "w+b");
    if (!edf) {
        ESP_LOGE(TAG, "cannot create %s: %s", path, strerror(errno));
        return ESP_FAIL;
    }

    int header_bytes = edf_write_header(edf, patient_id, recording_id,
                                        start_date, start_time,
                                        total_records, "0.00",
                                        "EDF+D", eve_sigs, 1);
    if (header_bytes < 0) {
        ESP_LOGE(TAG, "EVE.edf: edf_write_header failed");
        fclose(edf);
        return ESP_FAIL;
    }

    /* Write data records */
    /* Record 0: "Recording starts" marker */
    {
        uint8_t payload[62];
        memset(payload, 0, 62);
        /* EDF+ TAL: +0\x14\x14\x00 +0\x15 0\x14 Recording starts\x14\x00... */
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

        /* CRC16 over the 62-byte payload (little-endian) */
        uint16_t crc = crc16_ccitt(payload, 62);
        fwrite(payload, 1, 62, edf);
        uint8_t crc_bytes[2] = { (uint8_t)(crc & 0xFF), (uint8_t)(crc >> 8) };
        fwrite(crc_bytes, 1, 2, edf);
    }

    /* Subsequent records: one event per record */
    if (events_data && events_len > 0) {
        size_t pos = 0;
        while (pos < events_len) {
            uint64_t tag = pb_decode_varint(events_data, events_len, &pos);
            int field = (int)(tag >> 3);
            int wire = (int)(tag & 0x07);
            if (field == 0) break;
            if (wire == 2) {
                size_t lpos = pos;
                uint64_t flen = pb_decode_varint(events_data, events_len, &lpos);
                pos = lpos;
                if (pos + flen > events_len) break;

                event_record_t evt = {0};
                pb_iter(events_data + pos, (size_t)flen, event_field_cb, &evt);
                pos += flen;

                const char *label = event_label_map(evt.type);
                if (!label) continue;

                /* Compute onset (seconds from session start).
                 * Event timestamps from the spool are in AS11 internal time.
                 * Session start is in NTP time.  Apply clock_drift_ms to
                 * convert AS11 time to NTP time before computing the offset:
                 *   ntp_event = as11_event + drift
                 *   onset = ntp_event - session_start_ntp */
                int64_t event_ntp_ms = evt.timestamp_ms + clock_drift_ms;
                int64_t onset_sec = (event_ntp_ms - session_start_ms) / 1000;
                if (onset_sec < 0) onset_sec = 0;

                /* Build TAL payload */
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
                snprintf(onset_str, sizeof(onset_str), "+%lld", (long long)onset_sec);
                memcpy(payload + p, onset_str, strlen(onset_str));
                p += strlen(onset_str);
                payload[p++] = 0x15;  /* duration separator */
                char dur_str[24];
                snprintf(dur_str, sizeof(dur_str), "%lld", (long long)evt.duration_sec);
                memcpy(payload + p, dur_str, strlen(dur_str));
                p += strlen(dur_str);
                payload[p++] = 0x14;
                memcpy(payload + p, label, strlen(label));
                p += strlen(label);
                payload[p++] = 0x14;
                payload[p++] = 0x00;

                /* CRC16 (little-endian) */
                uint16_t crc = crc16_ccitt(payload, 62);
                fwrite(payload, 1, 62, edf);
                uint8_t crc_bytes[2] = { (uint8_t)(crc & 0xFF), (uint8_t)(crc >> 8) };
                fwrite(crc_bytes, 1, 2, edf);
            } else if (wire == 0) {
                pb_decode_varint(events_data, events_len, &pos);
            } else if (wire == 1) {
                pos += 8;
            } else if (wire == 5) {
                pos += 4;
            } else {
                break;
            }
        }
    }

    /* Finalise CRC in patient ID */
    edf_finalise_crc(edf, header_bytes);
    fclose(edf);

    ESP_LOGI(TAG, "EVE.edf generated: %s (%d events)", path, event_count);
    return ESP_OK;
}

/* ════════════════════════════════════════════════════════════════════
 *  Section 8: Identification.json + .crc generation
 * ════════════════════════════════════════════════════════════════════ */

/* Forward declaration — read_json_file is defined in Section 9. */
static cJSON *read_json_file(const char *path);

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

    /* ProductName: remove spaces (AS11 stores "AirSense11AutoSet" not "AirSense 11 AutoSet") */
    char pname[64];
    const char *pn_src = get_str(ident, "ProductName");
    int pni = 0;
    for (int i = 0; pn_src[i] && pni < (int)sizeof(pname) - 1; i++) {
        if (pn_src[i] != ' ') pname[pni++] = pn_src[i];
    }
    pname[pni] = '\0';
    cJSON_AddStringToObject(product, "ProductName", pname);

    cJSON_AddStringToObject(product, "FdaUniqueDeviceIdentifier", "");
    cJSON_AddStringToObject(product, "ProductGeographicIdentifier",
        get_str(ident, "ProductGeographicIdentifier"));

    /* Build Hardware object */
    cJSON *hardware = cJSON_CreateObject();
    cJSON_AddStringToObject(hardware, "HardwareIdentifier",
        get_str(ident, "HardwareIdentifier"));

    /* Build Software object */
    cJSON *software = cJSON_CreateObject();
    cJSON_AddStringToObject(software, "BootloaderIdentifier",
        get_str(ident, "BootloaderIdentifier"));
    cJSON_AddStringToObject(software, "ApplicationIdentifier",
        get_str(ident, "ApplicationIdentifier"));
    cJSON_AddStringToObject(software, "ConfigurationIdentifier",
        get_str(ident, "ConfigurationIdentifier"));
    cJSON_AddStringToObject(software, "PlatformIdentifier",
        get_str(ident, "PlatformIdentifier"));
    cJSON_AddStringToObject(software, "VariantIdentifier",
        get_str(ident, "VariantIdentifier"));
    cJSON_AddStringToObject(software, "RegionIdentifier",
        get_str(ident, "RegionIdentifier"));
    cJSON_AddStringToObject(software, "ProfileVariationIdentifier",
        get_str(ident, "ProfileVariantIdentifier"));
    cJSON_AddStringToObject(software, "DataVersionIdentifier",
        get_str(ident, "DataVersionIdentifier"));
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

    /* Write Identification.json to EDF dir */
    char path[300];
    snprintf(path, sizeof(path), "%s/Identification.json", edf_dir);
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(json_str, f);
        fclose(f);
        ESP_LOGI(TAG, "wrote %s (%u bytes)", path, (unsigned)strlen(json_str));
    }

    /* Compute CRC-32 and write as 4-byte binary little-endian */
    size_t json_len = strlen(json_str);
    uint32_t crc = crc32_ieee((const uint8_t *)json_str, json_len);
    snprintf(path, sizeof(path), "%s/Identification.crc", edf_dir);
    f = fopen(path, "wb");
    if (f) {
        uint8_t crc_bytes[4] = {
            (uint8_t)(crc & 0xFF),
            (uint8_t)((crc >> 8) & 0xFF),
            (uint8_t)((crc >> 16) & 0xFF),
            (uint8_t)((crc >> 24) & 0xFF),
        };
        fwrite(crc_bytes, 1, 4, f);
        fclose(f);
        ESP_LOGI(TAG, "wrote %s (crc32=0x%08X)", path, (unsigned)crc);
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

    ESP_LOGI(TAG, "=== EDF GENERATION START ===");
    ESP_LOGI(TAG, "session=%s id=%s drift=%lldms",
             session_dir, session_id, (long long)clock_drift_ms);

    /* ── Create EDF output directory ──
     * EDF files go to /somnotrace/EDF/<session_id>/DATALOG/
     * This is completely outside the sessions/ directory. */
    char edf_root[128];
    snprintf(edf_root, sizeof(edf_root), "%s/EDF", SD_MOUNT_POINT);
    mkdir(edf_root, 0775);

    char edf_dir[160];
    snprintf(edf_dir, sizeof(edf_dir), "%s/%s", edf_root, session_id);
    mkdir(edf_dir, 0775);

    char datalog_dir[200];
    snprintf(datalog_dir, sizeof(datalog_dir), "%s/DATALOG", edf_dir);
    mkdir(datalog_dir, 0775);

    /* Create date subdirectory inside DATALOG (noon-based day folder).
     * AS11 native layout: DATALOG/YYYYMMDD/YYYYMMDD_HHMMSS_TYPE.edf */
    char day_folder[16];
    noon_day_folder(start_epoch_ms, day_folder, sizeof(day_folder));

    char day_dir[220];
    snprintf(day_dir, sizeof(day_dir), "%s/%s", datalog_dir, day_folder);
    mkdir(day_dir, 0775);

    /* Session timestamp prefix for EDF filenames */
    char ts_prefix[32];
    session_timestamp(start_epoch_ms, ts_prefix, sizeof(ts_prefix));

    /* ── Load post-therapy data ── */
    char pt_dir[300];
    snprintf(pt_dir, sizeof(pt_dir), "%s/post-therapy", session_dir);

    /* Read identification.json for EDF header fields */
    char ident_path[330];
    snprintf(ident_path, sizeof(ident_path), "%s/identification.json", pt_dir);
    cJSON *ident = read_json_file(ident_path);

    /* Read settings.json for STR.edf settings fields */
    char settings_path[330];
    snprintf(settings_path, sizeof(settings_path), "%s/settings.json", pt_dir);
    cJSON *settings = read_json_file(settings_path);

    /* Read Summary spool data for STR.edf */
    char summary_path[330];
    snprintf(summary_path, sizeof(summary_path), "%s/summary.bin", pt_dir);
    size_t summary_len = 0;
    uint8_t *summary_data = read_bin_file(summary_path, &summary_len);

    /* Read respiratory events spool data for EVE.edf */
    char events_path[330];
    snprintf(events_path, sizeof(events_path), "%s/respiratory_events.bin", pt_dir);
    size_t events_len = 0;
    uint8_t *events_data = read_bin_file(events_path, &events_len);

    /* ── Build EDF header common fields ── */
    char start_date[16], start_time[16];
    format_edf_datetime(start_epoch_ms, start_date, sizeof(start_date),
                        start_time, sizeof(start_time));

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

    char recording_id[128];
    format_recording_id(recording_id, sizeof(recording_id),
                        start_epoch_ms, ident);

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

    /* Patient ID will have CRC filled in by edf_finalise_crc.
     * Initial value is the "X X X X" prefix with placeholder zeros. */
    char patient_id[81] = "X X X X 0000 0000";

    int errors = 0;

    /* ── Generate BRP.edf (25 Hz breath waveform) ── */
    {
        char snt_path[300], edf_path[350];
        snprintf(snt_path, sizeof(snt_path), "%s/brp.snt", session_dir);
        snprintf(edf_path, sizeof(edf_path), "%s/%s_BRP.edf", day_dir, ts_prefix);

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
        if (convert_snt_to_edf(snt_path, edf_path, patient_id, recording_id,
                               start_date, start_time, brp_sigs, 2, "60.00", NULL) != ESP_OK) {
            errors++;
        }
    }

    /* ── Generate SA2.edf (1 Hz SpO2/pulse) ── */
    {
        char snt_path[300], edf_path[350];
        snprintf(snt_path, sizeof(snt_path), "%s/sa2.snt", session_dir);
        snprintf(edf_path, sizeof(edf_path), "%s/%s_SA2.edf", day_dir, ts_prefix);

        edf_signal_def_t sa2_sigs[] = {
            { .label = "Pulse.1s", .transducer = "",
              .unit = "bpm", .phys_min = 0.0, .phys_max = 300.0,
              .dig_min = 0, .dig_max = 300,
              .prefilter = "", .samples_per_record = 60 },
            { .label = "SpO2.1s", .transducer = "",
              .unit = "%", .phys_min = 0.0, .phys_max = 100.0,
              .dig_min = 0, .dig_max = 100,
              .prefilter = "", .samples_per_record = 60 },
        };
        if (convert_snt_to_edf(snt_path, edf_path, patient_id, recording_id,
                               start_date, start_time, sa2_sigs, 2, "60.00", NULL) != ESP_OK) {
            errors++;
        }
    }

    /* ── Generate PLD.edf (0.5 Hz per-breath stats) ── */
    {
        char snt_path[300], edf_path[350];
        snprintf(snt_path, sizeof(snt_path), "%s/pld.snt", session_dir);
        snprintf(edf_path, sizeof(edf_path), "%s/%s_PLD.edf", day_dir, ts_prefix);

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
              .prefilter = "", .samples_per_record = 30 },
        };
        /* PLD .snt has 12 channels but AS11 EDF (VID=3) only has 9.
         * Channel order in .snt: 0=MaskPress, 1=Press, 2=EprPress, 3=Leak,
         * 4=RespRate, 5=TidVol, 6=MinVent, 7=TgtVent, 8=IERatio,
         * 9=Snore, 10=FlowLim, 11=Ti
         * EDF drops TgtVent(7), IERatio(8), Ti(11). */
        static const int pld_ch_map[] = {0, 1, 2, 3, 4, 5, 6, 9, 10};
        if (convert_snt_to_edf(snt_path, edf_path, patient_id, recording_id,
                               start_date, start_time, pld_sigs, 9, "60.00", pld_ch_map) != ESP_OK) {
            errors++;
        }
    }

    /* ── Generate STR.edf from Summary spool ──
     * STR.edf goes in the EDF root (not inside DATALOG/) — it is a daily
     * cumulative file, not per-session. */
    if (summary_data && summary_len > 0) {
        if (generate_str_edf(edf_dir, summary_data, summary_len,
                             patient_id, str_recording_id,
                             str_start_date, start_time,
                             start_epoch_ms, end_epoch_ms,
                             settings, session_dir) != ESP_OK) {
            errors++;
        }
    } else {
        ESP_LOGW(TAG, "STR.edf: no Summary spool data, skipping");
        errors++;
    }

    /* ── Generate EVE.edf from respiratory events spool ──
     * Event timestamps in the spool are in AS11 internal time.
     * clock_drift_ms is applied to convert them to NTP time. */
    char eve_path[350];
    snprintf(eve_path, sizeof(eve_path), "%s/%s_EVE.edf", day_dir, ts_prefix);
    if (generate_eve_edf(eve_path, events_data, events_len,
                         start_epoch_ms, clock_drift_ms,
                         patient_id, recording_id,
                         start_date, start_time) != ESP_OK) {
        errors++;
    }

    /* ── Generate CSL.edf (CSR event log) ──
     * For sessions with no CSR events, CSL.edf is byte-identical to
     * EVE.edf (contains only the "Recording starts" marker). */
    char csl_path[350];
    snprintf(csl_path, sizeof(csl_path), "%s/%s_CSL.edf", day_dir, ts_prefix);
    if (generate_eve_edf(csl_path, events_data, events_len,
                         start_epoch_ms, clock_drift_ms,
                         patient_id, recording_id,
                         start_date, start_time) != ESP_OK) {
        errors++;
    }

    /* ── Generate Identification.json + .crc ── */
    if (generate_identification(edf_dir, ident_path) != ESP_OK) {
        errors++;
    }

    /* ── Cleanup ── */
    if (ident) cJSON_Delete(ident);
    if (settings) cJSON_Delete(settings);
    free(summary_data);
    free(events_data);

    ESP_LOGI(TAG, "=== EDF GENERATION DONE (%d errors) ===", errors);
    return errors > 0 ? ESP_FAIL : ESP_OK;
}
