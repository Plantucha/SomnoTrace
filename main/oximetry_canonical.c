/*
 * SomnoTrace - Canonical oximetry recording-package storage and conversion
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

#include "oximetry_canonical.h"
#include "sd_storage.h"
#include "time_sync.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_rom_crc.h"

static const char *TAG = "ox_canon";

#define OX_ROOT       SD_OXYMETRY_DIR
#define OX_DEVICES    OX_ROOT "/devices"
#define OX_INBOX      OX_ROOT "/inbox"
#define OX_STAGING    OX_ROOT "/staging"
#define OX_RECORDINGS OX_ROOT "/recordings"
#define OX_QUARANTINE OX_ROOT "/quarantine"
#define OX_STATE      OX_ROOT "/state"
#define OX_GEN        "generations"

#define OX_SOURCE_HEADER 10
#define OX_SOURCE_TRAILER 48
#define OX_TRAILER_MAGIC_OFFSET 4
#define OX_SNT_HEADER_BYTES 64
#define OX_MAX_CHANNELS 16

#define OX_STATUS_SPO2_MISSING  (1u << 0)
#define OX_STATUS_PULSE_MISSING (1u << 1)

/* SNT v3 uses a fixed 64-byte header.  All multibyte fields are little-endian
 * because the target and browser format are both explicitly little-endian. */
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t version;
    uint8_t tier;
    uint8_t timing;
    uint8_t n_channels;
    uint8_t sample_bytes;
    uint8_t flags8;
    uint16_t header_bytes;
    uint32_t period_num_us;
    uint32_t period_den;
    int64_t start_epoch_ms;
    uint32_t sample_count;
    uint32_t data_bytes;
    uint32_t data_crc32;
    uint32_t reserved0;
    uint32_t reserved1;
    uint8_t reserved[16];
} ox_snt3_header_t;

_Static_assert(sizeof(ox_snt3_header_t) == OX_SNT_HEADER_BYTES,
               "oximetry SNT v3 header must be 64 bytes");

static bool safe_component(const char *s, size_t max_len)
{
    if (!s || !s[0] || strlen(s) >= max_len) return false;
    if (strcmp(s, ".") == 0 || strcmp(s, "..") == 0) return false;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '/' || *p == '\\' || *p < 0x20) return false;
    }
    return true;
}

static esp_err_t mkdir_one(const char *path)
{
    if (mkdir(path, 0775) == 0 || errno == EEXIST) return ESP_OK;
    ESP_LOGW(TAG, "mkdir %s failed: %s", path, strerror(errno));
    return ESP_FAIL;
}

static esp_err_t ensure_path_tree(void)
{
    const char *paths[] = {
        OX_ROOT, OX_DEVICES, OX_INBOX, OX_STAGING, OX_RECORDINGS,
        OX_QUARANTINE, OX_STATE,
    };
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        if (mkdir_one(paths[i]) != ESP_OK) return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t oximetry_canonical_ensure_dirs(void)
{
    if (!sd_storage_is_ready()) return ESP_ERR_INVALID_STATE;
    return ensure_path_tree();
}

static esp_err_t write_json_atomic(const char *path, cJSON *json)
{
    if (!path || !json) return ESP_ERR_INVALID_ARG;
    char tmp[OXIMETRY_CANONICAL_MAX_PATH + 16];
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp))
        return ESP_ERR_INVALID_SIZE;

    char *text = cJSON_PrintUnformatted(json);
    if (!text || strlen(text) > OXIMETRY_CANONICAL_MAX_JSON_BYTES) {
        if (text) cJSON_free(text);
        return ESP_ERR_NO_MEM;
    }

    FILE *f = fopen(tmp, "w");
    if (!f) {
        ESP_LOGW(TAG, "write_json_atomic: fopen(%s) failed: %s", tmp, strerror(errno));
        cJSON_free(text);
        return ESP_FAIL;
    }
    size_t len = strlen(text);
    bool ok = fwrite(text, 1, len, f) == len && fflush(f) == 0 &&
              fsync(fileno(f)) == 0;
    int close_rc = fclose(f);
    cJSON_free(text);
    if (!ok || close_rc != 0) {
        ESP_LOGW(TAG, "write_json_atomic: write failed (ok=%d close=%d) %s", ok, close_rc, tmp);
        unlink(tmp);
        return ESP_FAIL;
    }
    /* FATFS does not support rename-over-existing (returns EEXIST), so
     * unlink the destination first.  This is not strictly atomic on FAT
     * but is the standard workaround for this filesystem. */
    unlink(path);
    if (rename(tmp, path) != 0) {
        ESP_LOGW(TAG, "write_json_atomic: rename failed %s: %s", tmp, strerror(errno));
        unlink(tmp);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static cJSON *read_json_file(const char *path)
{
    struct stat st;
    if (!path || stat(path, &st) != 0 || st.st_size <= 0 ||
        st.st_size > OXIMETRY_CANONICAL_MAX_JSON_BYTES)
        return NULL;

    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    char *buf = heap_caps_malloc((size_t)st.st_size + 1, MALLOC_CAP_SPIRAM);
    if (!buf) buf = malloc((size_t)st.st_size + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t n = fread(buf, 1, (size_t)st.st_size, f);
    fclose(f);
    buf[n] = '\0';
    cJSON *json = (n == (size_t)st.st_size) ? cJSON_Parse(buf) : NULL;
    free(buf);
    return json;
}

static const char *basename_safe(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static bool path_join2(char *out, size_t out_size, const char *a, const char *b)
{
    if (!out || !a || !b) return false;
    size_t alen = strlen(a), blen = strlen(b);
    if (alen + 1 + blen + 1 > out_size) return false;
    memcpy(out, a, alen);
    out[alen] = '/';
    memcpy(out + alen + 1, b, blen + 1);
    return true;
}

static bool path_join3(char *out, size_t out_size, const char *a,
                       const char *b, const char *c)
{
    char tmp[OXIMETRY_CANONICAL_MAX_PATH];
    return path_join2(tmp, sizeof(tmp), a, b) &&
           path_join2(out, out_size, tmp, c);
}

static bool path_join4(char *out, size_t out_size, const char *a,
                       const char *b, const char *c, const char *d)
{
    char tmp[OXIMETRY_CANONICAL_MAX_PATH];
    return path_join3(tmp, sizeof(tmp), a, b, c) &&
           path_join2(out, out_size, tmp, d);
}

static bool day_for_epoch(int64_t epoch_ms, char out[9])
{
    time_t sec = (time_t)(epoch_ms / 1000);
    struct tm local;
    if (!localtime_r(&sec, &local)) return false;
    /* Match the existing therapy noon-day convention. */
    if (local.tm_hour < 12) {
        sec -= 24 * 60 * 60;
        if (!localtime_r(&sec, &local)) return false;
    }
    return strftime(out, 9, "%Y%m%d", &local) == 8;
}

static int64_t filename_epoch_ms(const char *name)
{
    if (!name || strlen(name) < 14) return 0;
    for (int i = 0; i < 14; i++) if (name[i] < '0' || name[i] > '9') return 0;
    int year, mon, day, hour, min, sec;
    if (sscanf(name, "%4d%2d%2d%2d%2d%2d", &year, &mon, &day,
               &hour, &min, &sec) != 6) return 0;
    struct tm tm = {0};
    tm.tm_year = year - 1900; tm.tm_mon = mon - 1; tm.tm_mday = day;
    tm.tm_hour = hour; tm.tm_min = min; tm.tm_sec = sec; tm.tm_isdst = -1;
    time_t t = mktime(&tm);
    return t == (time_t)-1 ? 0 : (int64_t)t * 1000;
}

static bool copy_file_crc(const char *src, const char *dst, uint32_t *out_crc,
                          uint64_t *out_size)
{
    FILE *in = fopen(src, "rb");
    if (!in) return false;
    FILE *out = fopen(dst, "wb");
    if (!out) {
        fclose(in);
        return false;
    }

    /* Heap-allocate the I/O buffer to avoid 4 KB of stack usage on the
     * PSRAM-backed migration task. */
    uint8_t *buf = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    if (!buf) buf = malloc(4096);
    if (!buf) { fclose(in); fclose(out); return false; }

    uint32_t crc = 0;
    uint64_t total = 0;
    bool ok = true;
    while (true) {
        size_t n = fread(buf, 1, 4096, in);
        if (n > 0) {
            if (fwrite(buf, 1, n, out) != n) {
                ok = false;
                break;
            }
            crc = esp_rom_crc32_le(crc, buf, n);
            total += n;
        }
        if (n < 4096) {
            if (ferror(in)) ok = false;
            break;
        }
    }
    if (ok && (fflush(out) != 0 || fsync(fileno(out)) != 0)) ok = false;
    fclose(in);
    if (fclose(out) != 0) ok = false;
    if (!ok) unlink(dst);
    free(buf);
    if (out_crc) *out_crc = crc;
    if (out_size) *out_size = total;
    return ok;
}

static bool file_crc_size(const char *path, uint32_t *out_crc, uint64_t *out_size)
{
    FILE *f = fopen(path, "rb"); if (!f) return false;
    uint8_t *buf = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    if (!buf) buf = malloc(4096);
    if (!buf) { fclose(f); return false; }
    uint32_t crc = 0; uint64_t total = 0; size_t n;
    while ((n = fread(buf, 1, 4096, f)) > 0) {
        crc = esp_rom_crc32_le(crc, buf, n); total += n;
    }
    bool ok = !ferror(f); fclose(f);
    free(buf);
    if (out_crc) *out_crc = crc;
    if (out_size) *out_size = total;
    return ok;
}

static bool write_snt3_format_a(const char *src, const char *dst,
                                int64_t start_ms, uint32_t *out_count,
                                uint32_t *out_crc)
{
    struct stat st;
    if (stat(src, &st) != 0 || st.st_size < OX_SOURCE_HEADER + OX_SOURCE_TRAILER)
        return false;
    uint64_t body = (uint64_t)st.st_size - OX_SOURCE_HEADER - OX_SOURCE_TRAILER;
    if (body == 0 || body % 3 != 0 || body / 3 > UINT32_MAX) return false;

    FILE *in = fopen(src, "rb");
    FILE *out = fopen(dst, "wb");
    if (!in || !out) {
        if (in) fclose(in);
        if (out) fclose(out);
        return false;
    }

    ox_snt3_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = OXIMETRY_CANONICAL_SNT_MAGIC;
    hdr.version = OXIMETRY_CANONICAL_SNT_VERSION;
    hdr.tier = 0;
    hdr.timing = 0;
    hdr.n_channels = OXIMETRY_CANONICAL_VITALS_CHANNELS;
    hdr.sample_bytes = sizeof(int16_t);
    hdr.header_bytes = OX_SNT_HEADER_BYTES;
    hdr.period_num_us = 1000000;
    hdr.period_den = 1;
    hdr.start_epoch_ms = start_ms;
    hdr.sample_count = (uint32_t)(body / 3);
    hdr.data_bytes = hdr.sample_count * hdr.n_channels * hdr.sample_bytes;
    if (fwrite(&hdr, 1, sizeof(hdr), out) != sizeof(hdr)) goto fail;

    if (fseek(in, OX_SOURCE_HEADER, SEEK_SET) != 0) goto fail;
    uint32_t crc = 0;
    for (uint32_t i = 0; i < hdr.sample_count; i++) {
        uint8_t raw[3];
        int16_t rec[OXIMETRY_CANONICAL_VITALS_CHANNELS];
        if (fread(raw, 1, sizeof(raw), in) != sizeof(raw)) goto fail;
        bool spo2_missing = raw[0] == 0 || raw[0] == 0xff;
        bool pulse_missing = raw[1] == 0 || raw[1] == 0xff;
        rec[OXIMETRY_CANONICAL_VITALS_SPO2] = spo2_missing
            ? OXIMETRY_CANONICAL_SNT_MISSING : (int16_t)raw[0] * 100;
        rec[OXIMETRY_CANONICAL_VITALS_PULSE] = pulse_missing
            ? OXIMETRY_CANONICAL_SNT_MISSING : (int16_t)raw[1] * 100;
        rec[OXIMETRY_CANONICAL_VITALS_MOTION_FLAGS] = raw[2];
        uint16_t status = (spo2_missing ? OX_STATUS_SPO2_MISSING : 0) |
                          (pulse_missing ? OX_STATUS_PULSE_MISSING : 0);
        rec[OXIMETRY_CANONICAL_VITALS_STATUS] = (int16_t)status;
        rec[OXIMETRY_CANONICAL_VITALS_SOURCE_STATUS] = (int16_t)raw[2];
        if (fwrite(rec, sizeof(rec), 1, out) != 1) goto fail;
        crc = esp_rom_crc32_le(crc, (const uint8_t *)rec, sizeof(rec));
    }

    if (fseek(in, st.st_size - OX_SOURCE_TRAILER + OX_TRAILER_MAGIC_OFFSET,
              SEEK_SET) != 0) goto fail;
    uint8_t magic[4];
    if (fread(magic, 1, sizeof(magic), in) != sizeof(magic) ||
        magic[0] != 0x48 || magic[1] != 0x12 || magic[2] != 0x5a ||
        magic[3] != 0xda)
        goto fail;

    hdr.data_crc32 = crc;
    if (fseek(out, 0, SEEK_SET) != 0 ||
        fwrite(&hdr, 1, sizeof(hdr), out) != sizeof(hdr) ||
        fflush(out) != 0 || fsync(fileno(out)) != 0)
        goto fail;
    fclose(in);
    if (fclose(out) != 0) return false;
    if (out_count) *out_count = hdr.sample_count;
    if (out_crc) *out_crc = crc;
    return true;

fail:
    fclose(in);
    fclose(out);
    unlink(dst);
    return false;
}

static bool write_vld3_export(const char *src, const char *dst, int64_t start_ms)
{
    struct stat st;
    if (stat(src, &st) != 0 || st.st_size < OX_SOURCE_HEADER + OX_SOURCE_TRAILER) return false;
    uint64_t body = (uint64_t)st.st_size - OX_SOURCE_HEADER - OX_SOURCE_TRAILER;
    uint32_t source_count = (uint32_t)(body / 3);
    uint32_t records = (source_count + 3) / 4;
    if (records == 0 || records > 65535 || 40ULL + (uint64_t)records * 5 > UINT32_MAX) return false;
    FILE *in = fopen(src, "rb"); FILE *out = fopen(dst, "wb");
    if (!in || !out) { if (in) fclose(in); if (out) fclose(out); return false; }
    uint8_t header[40] = {0};
    struct tm tm; time_t sec = (time_t)(start_ms / 1000);
    if (!localtime_r(&sec, &tm)) goto fail;
    header[0] = 3; header[1] = 0;
    uint16_t year = (uint16_t)(tm.tm_year + 1900);
    header[2] = year & 0xff; header[3] = year >> 8;
    header[4] = (uint8_t)(tm.tm_mon + 1); header[5] = (uint8_t)tm.tm_mday;
    header[6] = (uint8_t)tm.tm_hour; header[7] = (uint8_t)tm.tm_min; header[8] = (uint8_t)tm.tm_sec;
    uint32_t size = 40 + records * 5;
    header[9] = size & 0xff; header[10] = (size >> 8) & 0xff; header[11] = (size >> 16) & 0xff;
    uint16_t duration = (uint16_t)(records * 4);
    header[13] = duration & 0xff; header[14] = duration >> 8;
    if (fwrite(header, 1, sizeof(header), out) != sizeof(header) || fseek(in, OX_SOURCE_HEADER, SEEK_SET) != 0) goto fail;
    for (uint32_t i = 0; i < records; i++) {
        uint8_t raw[3], selected[3] = {0xff, 0xff, 0};
        for (int j = 0; j < 4 && (i * 4 + (uint32_t)j) < source_count; j++) {
            if (fread(raw, 1, sizeof(raw), in) != sizeof(raw)) goto fail;
            if (j == 0) memcpy(selected, raw, sizeof(selected));
        }
        uint8_t vld[5] = { selected[0], selected[1],
                           (uint8_t)((selected[0] == 0 || selected[0] == 0xff ||
                                      selected[1] == 0 || selected[1] == 0xff) ? 1 : 0),
                           selected[2], 0 };
        if (fwrite(vld, 1, sizeof(vld), out) != sizeof(vld)) goto fail;
    }
    if (fflush(out) != 0 || fsync(fileno(out)) != 0) goto fail;
    fclose(in); if (fclose(out) != 0) return false; return true;
fail:
    fclose(in); fclose(out); unlink(dst); return false;
}

static bool source_complete_format_a(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size < OX_SOURCE_HEADER + OX_SOURCE_TRAILER)
        return false;
    uint64_t body = (uint64_t)st.st_size - OX_SOURCE_HEADER - OX_SOURCE_TRAILER;
    if (body == 0 || body % 3 != 0) return false;
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    static const uint8_t format_a_header[OX_SOURCE_HEADER] =
        { 0x01, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00 };
    uint8_t header[OX_SOURCE_HEADER];
    if (fread(header, 1, sizeof(header), f) != sizeof(header) ||
        memcmp(header, format_a_header, sizeof(header)) != 0) {
        fclose(f);
        return false;
    }
    if (fseek(f, st.st_size - OX_SOURCE_TRAILER + OX_TRAILER_MAGIC_OFFSET,
              SEEK_SET) != 0) {
        fclose(f);
        return false;
    }
    uint8_t magic[4];
    bool ok = fread(magic, 1, sizeof(magic), f) == sizeof(magic) &&
              magic[0] == 0x48 && magic[1] == 0x12 && magic[2] == 0x5a &&
              magic[3] == 0xda;
    fclose(f);
    return ok;
}

static bool valid_recording_id(const char *id)
{
    return safe_component(id, OXIMETRY_CANONICAL_MAX_COMPONENT);
}

static esp_err_t build_generation_manifest(const char *path, const char *device_id,
                                           const char *recording_id,
                                           const char *source_name,
                                           int64_t start_ms, int64_t end_ms,
                                           uint32_t source_size, uint32_t source_crc,
                                           uint32_t sample_count, uint32_t data_crc)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(root, "schema", "somnotrace.oximetry.generation/1");
    cJSON_AddStringToObject(root, "recording_id", recording_id);
    cJSON_AddNumberToObject(root, "generation", 1);
    cJSON_AddStringToObject(root, "state", "ready");
    cJSON_AddStringToObject(root, "terminal_reason", "completed");

    cJSON *device = cJSON_AddObjectToObject(root, "device");
    cJSON_AddStringToObject(device, "device_key", device_id);
    cJSON_AddStringToObject(device, "driver_id", "wellue_oxyii");
    cJSON_AddStringToObject(device, "format_id", "wellue_oxyii_format_a");

    cJSON *source = cJSON_AddArrayToObject(root, "source");
    cJSON *src = cJSON_CreateObject();
    char original_name[OXIMETRY_CANONICAL_MAX_COMPONENT];
    strlcpy(original_name, source_name, sizeof(original_name));
    size_t original_len = strlen(original_name);
    if (original_len > 4 && strcmp(original_name + original_len - 4, ".bin") == 0)
        original_name[original_len - 4] = '\0';
    cJSON_AddStringToObject(src, "original_name", original_name);
    cJSON_AddStringToObject(src, "stored_path", "source/source.bin");
    cJSON_AddStringToObject(src, "format_id", "wellue_oxyii_format_a");
    cJSON_AddNumberToObject(src, "size", source_size);
    cJSON_AddNumberToObject(src, "crc32", source_crc);
    cJSON_AddItemToArray(source, src);
    cJSON *exports = cJSON_AddArrayToObject(root, "exports");
    cJSON *vld = cJSON_CreateObject();
    char export_path[OXIMETRY_CANONICAL_MAX_COMPONENT + 32];
    snprintf(export_path, sizeof(export_path), "exports/sleephq/recording.vld");
    cJSON_AddStringToObject(vld, "id", "sleephq_vld3");
    cJSON_AddStringToObject(vld, "path", export_path);
    cJSON_AddStringToObject(vld, "format_id", "wellue_vld_v3");
    cJSON_AddItemToArray(exports, vld);

    cJSON *time = cJSON_AddObjectToObject(root, "time");
    char tz_name[64] = "UTC";
    time_sync_get_tz_name(tz_name, sizeof(tz_name));
    cJSON_AddStringToObject(time, "timezone", tz_name);
    cJSON_AddStringToObject(time, "device_clock_basis", "local_wall");
    cJSON_AddNumberToObject(time, "start_epoch_ms", (double)start_ms);
    cJSON_AddNumberToObject(time, "end_epoch_ms", (double)end_ms);
    cJSON_AddStringToObject(time, "source", "device_filename");
    cJSON_AddStringToObject(time, "confidence", "provisional");

    cJSON *tracks = cJSON_AddArrayToObject(root, "tracks");
    cJSON *track = cJSON_CreateObject();
    cJSON_AddStringToObject(track, "id", "vitals");
    cJSON_AddStringToObject(track, "path", "data/vitals.snt");
    cJSON_AddStringToObject(track, "timing", "uniform");
    cJSON_AddNumberToObject(track, "period_num_us", 1000000);
    cJSON_AddNumberToObject(track, "period_den", 1);
    cJSON_AddNumberToObject(track, "sample_count", sample_count);
    cJSON_AddNumberToObject(track, "data_crc32", data_crc);
    cJSON *channels = cJSON_AddArrayToObject(track, "channels");
    const char *semantics[] = { "spo2", "pulse_rate", "motion", "sample_status", "source_status" };
    const char *units[] = { "%", "bpm", "source_index", "bitset", "bitset" };
    for (size_t i = 0; i < 5; i++) {
        cJSON *ch = cJSON_CreateObject();
        cJSON_AddStringToObject(ch, "semantic", semantics[i]);
        cJSON_AddStringToObject(ch, "storage", "i16");
        cJSON_AddNumberToObject(ch, "scale", i < 2 ? 0.01 : 1);
        cJSON_AddStringToObject(ch, "unit", units[i]);
        cJSON_AddItemToArray(channels, ch);
    }
    cJSON_AddItemToArray(tracks, track);

    cJSON *integrity = cJSON_AddObjectToObject(root, "integrity");
    cJSON_AddBoolToObject(integrity, "verified", true);
    cJSON_AddStringToObject(integrity, "source_validator", "oxyii_format_a_trailer");

    esp_err_t e = write_json_atomic(path, root);
    cJSON_Delete(root);
    return e;
}


/* Path-buffer pool for convert_format_a.  16 × 512 = 8 KB on the stack was
 * overflowing the 8 KB PSRAM task stack of ox_migrate.  Heap-allocating this
 * block from PSRAM eliminates ~8 KB of stack pressure. */
typedef struct {
    char final_dir[OXIMETRY_CANONICAL_MAX_PATH];
    char final_pointer[OXIMETRY_CANONICAL_MAX_PATH];
    char day_dir[OXIMETRY_CANONICAL_MAX_PATH];
    char stage_dir[OXIMETRY_CANONICAL_MAX_PATH];
    char stage_source_dir[OXIMETRY_CANONICAL_MAX_PATH];
    char stage_gen_dir[OXIMETRY_CANONICAL_MAX_PATH];
    char stage_data_dir[OXIMETRY_CANONICAL_MAX_PATH];
    char stage_source[OXIMETRY_CANONICAL_MAX_PATH];
    char stage_track_tmp[OXIMETRY_CANONICAL_MAX_PATH];
    char stage_track[OXIMETRY_CANONICAL_MAX_PATH];
    char stage_export_dir[OXIMETRY_CANONICAL_MAX_PATH];
    char stage_export[OXIMETRY_CANONICAL_MAX_PATH];
    char stage_export_file[OXIMETRY_CANONICAL_MAX_PATH];
    char stage_gen_manifest[OXIMETRY_CANONICAL_MAX_PATH];
    char gen_parent[OXIMETRY_CANONICAL_MAX_PATH];
    char stage_pointer[OXIMETRY_CANONICAL_MAX_PATH];
} convert_ctx_t;

esp_err_t oximetry_canonical_convert_format_a(const char *device_id,
                                               const char *recording_id,
                                               const char *source_path,
                                               int64_t start_utc_ms)
{
    if (!sd_storage_is_ready() || !safe_component(device_id, OXIMETRY_CANONICAL_MAX_COMPONENT) ||
        !valid_recording_id(recording_id) || !source_path ||
        !source_complete_format_a(source_path))
        return ESP_ERR_INVALID_ARG;
    if (oximetry_canonical_ensure_dirs() != ESP_OK) return ESP_FAIL;

    char day[9];
    if (!day_for_epoch(start_utc_ms, day)) return ESP_ERR_INVALID_ARG;

    struct stat st;
    if (stat(source_path, &st) != 0 || st.st_size > OXIMETRY_CANONICAL_MAX_SOURCE_BYTES)
        return ESP_ERR_INVALID_SIZE;
    uint32_t count = (uint32_t)(((uint64_t)st.st_size - OX_SOURCE_HEADER - OX_SOURCE_TRAILER) / 3);
    int64_t end_ms = start_utc_ms + (int64_t)(count - 1) * 1000;

    /* Heap-allocate path buffers to avoid ~8 KB of stack usage. */
    convert_ctx_t *p = heap_caps_malloc(sizeof(*p), MALLOC_CAP_SPIRAM);
    if (!p) p = malloc(sizeof(*p));
    if (!p) return ESP_ERR_NO_MEM;

    esp_err_t ret = ESP_FAIL;

    if (!path_join3(p->final_dir, sizeof(p->final_dir), OX_RECORDINGS, day, recording_id) ||
        !path_join2(p->final_pointer, sizeof(p->final_pointer), p->final_dir, "recording.json")) {
        free(p);
        return ESP_ERR_INVALID_SIZE;
    }

    cJSON *existing = read_json_file(p->final_pointer);
    if (existing) {
        cJSON_Delete(existing);
        free(p);
        return ESP_OK;
    }

    if (!path_join2(p->day_dir, sizeof(p->day_dir), OX_RECORDINGS, day) ||
        !path_join2(p->stage_dir, sizeof(p->stage_dir), OX_STAGING, recording_id) ||
        !path_join2(p->stage_source_dir, sizeof(p->stage_source_dir), p->stage_dir, "source") ||
        !path_join3(p->stage_gen_dir, sizeof(p->stage_gen_dir), p->stage_dir, OX_GEN, "1") ||
        !path_join2(p->stage_data_dir, sizeof(p->stage_data_dir), p->stage_gen_dir, "data") ||
        !path_join2(p->stage_source, sizeof(p->stage_source), p->stage_source_dir, "source.bin") ||
        !path_join2(p->stage_track_tmp, sizeof(p->stage_track_tmp), p->stage_data_dir, "vitals.snt.tmp") ||
        !path_join2(p->stage_track, sizeof(p->stage_track), p->stage_data_dir, "vitals.snt") ||
        !path_join2(p->stage_export_dir, sizeof(p->stage_export_dir), p->stage_gen_dir, "exports") ||
        !path_join3(p->stage_export, sizeof(p->stage_export), p->stage_gen_dir, "exports", "sleephq") ||
        !path_join2(p->stage_export_file, sizeof(p->stage_export_file), p->stage_export, "recording.vld") ||
        !path_join2(p->stage_gen_manifest, sizeof(p->stage_gen_manifest), p->stage_gen_dir, "manifest.json")) {
        free(p);
        return ESP_ERR_INVALID_SIZE;
    }

    mkdir_one(p->day_dir);
    mkdir_one(p->stage_dir);
    mkdir_one(p->stage_source_dir);
    if (!path_join2(p->gen_parent, sizeof(p->gen_parent), p->stage_dir, OX_GEN)) {
        free(p);
        return ESP_ERR_INVALID_SIZE;
    }
    mkdir_one(p->gen_parent);
    mkdir_one(p->stage_gen_dir);
    mkdir_one(p->stage_data_dir);
    mkdir_one(p->stage_export_dir);
    mkdir_one(p->stage_export);

    if (!path_join2(p->stage_pointer, sizeof(p->stage_pointer), p->stage_dir, "recording.json")) {
        free(p);
        return ESP_ERR_INVALID_SIZE;
    }
    cJSON *pending = cJSON_CreateObject();
    if (!pending) { free(p); return ESP_ERR_NO_MEM; }
    cJSON_AddStringToObject(pending, "schema", "somnotrace.oximetry.recording/1");
    cJSON_AddStringToObject(pending, "recording_id", recording_id);
    cJSON_AddNumberToObject(pending, "active_generation", 1);
    cJSON_AddStringToObject(pending, "state", "converting");
    cJSON_AddStringToObject(pending, "device_key", device_id);
    cJSON_AddStringToObject(pending, "input_path", source_path);
    cJSON_AddNumberToObject(pending, "start_epoch_ms", (double)start_utc_ms);
    esp_err_t pending_err = write_json_atomic(p->stage_pointer, pending);
    cJSON_Delete(pending);
    if (pending_err != ESP_OK) { free(p); return pending_err; }

    uint32_t source_crc = 0;
    uint32_t track_crc = 0;
    uint64_t source_size = 0;
    bool source_ok;
    if (strcmp(source_path, p->stage_source) == 0)
        source_ok = file_crc_size(source_path, &source_crc, &source_size);
    else
        source_ok = copy_file_crc(source_path, p->stage_source, &source_crc, &source_size);
    if (!source_ok || source_size != (uint64_t)st.st_size ||
        !write_snt3_format_a(source_path, p->stage_track_tmp, start_utc_ms, &count, &track_crc) ||
        (unlink(p->stage_track), rename(p->stage_track_tmp, p->stage_track) != 0) ||
        !write_vld3_export(source_path, p->stage_export_file, start_utc_ms) ||
        build_generation_manifest(p->stage_gen_manifest, device_id, recording_id,
                                  basename_safe(source_path), start_utc_ms, end_ms,
                                  (uint32_t)source_size, source_crc, count, track_crc) != ESP_OK) {
        ESP_LOGW(TAG, "convert %s: conversion step failed", recording_id);
        unlink(p->stage_track_tmp);
        free(p);
        return ESP_FAIL;
    }

    cJSON *pointer = cJSON_CreateObject();
    if (!pointer) { free(p); return ESP_ERR_NO_MEM; }
    cJSON_AddStringToObject(pointer, "schema", "somnotrace.oximetry.recording/1");
    cJSON_AddStringToObject(pointer, "recording_id", recording_id);
    cJSON_AddNumberToObject(pointer, "active_generation", 1);
    cJSON_AddStringToObject(pointer, "state", "ready");
    cJSON_AddStringToObject(pointer, "device_key", device_id);
    cJSON_AddStringToObject(pointer, "noon_day", day);
    cJSON_AddNumberToObject(pointer, "start_epoch_ms", (double)start_utc_ms);
    cJSON_AddNumberToObject(pointer, "end_epoch_ms", (double)end_ms);
    esp_err_t pe = write_json_atomic(p->stage_pointer, pointer);
    cJSON_Delete(pointer);
    if (pe != ESP_OK) { free(p); return pe; }

    /* The stage and final directory are on the same FAT volume.  The root
     * pointer is written before publication and is the final readiness gate. */
    if (rename(p->stage_dir, p->final_dir) != 0) {
        ESP_LOGW(TAG, "publish %s -> %s failed: %s", p->stage_dir, p->final_dir, strerror(errno));
        free(p);
        return ESP_FAIL;
    }
    ret = ESP_OK;
    free(p);
    return ret;
}

static bool valid_day_name(const char *s)
{
    if (!s || strlen(s) != 8) return false;
    for (int i = 0; i < 8; i++) if (s[i] < '0' || s[i] > '9') return false;
    return true;
}

static bool recording_ready_at(const char *dir)
{
    char pointer[OXIMETRY_CANONICAL_MAX_PATH];
    if (!path_join2(pointer, sizeof(pointer), dir, "recording.json")) return false;
    cJSON *p = read_json_file(pointer);
    if (!p) return false;
    cJSON *state = cJSON_GetObjectItem(p, "state");
    cJSON *gen = cJSON_GetObjectItem(p, "active_generation");
    bool ok = cJSON_IsString(state) && strcmp(state->valuestring, "ready") == 0 &&
              cJSON_IsNumber(gen) && gen->valueint > 0;
    if (ok) {
        char manifest[OXIMETRY_CANONICAL_MAX_PATH];
        snprintf(manifest, sizeof(manifest), "%s/%s/%d/manifest.json", dir, OX_GEN,
                 gen->valueint);
        cJSON *m = read_json_file(manifest);
        ok = m != NULL;
        if (m) cJSON_Delete(m);
    }
    cJSON_Delete(p);
    return ok;
}

esp_err_t oximetry_canonical_migrate_legacy(const char *device_id)
{
    if (!device_id || !safe_component(device_id, OXIMETRY_CANONICAL_MAX_COMPONENT) ||
        oximetry_canonical_ensure_dirs() != ESP_OK)
        return ESP_ERR_INVALID_STATE;
    char dir[OXIMETRY_CANONICAL_MAX_PATH];
    if (!path_join2(dir, sizeof(dir), OX_ROOT "/files", device_id)) return ESP_ERR_INVALID_SIZE;
    DIR *d = opendir(dir);
    if (!d) return ESP_OK;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        size_t len = strlen(e->d_name);
        if (len < 18 || strcmp(e->d_name + len - 4, ".bin") != 0) continue;
        char name[OXIMETRY_CANONICAL_MAX_COMPONENT];
        if (len - 4 >= sizeof(name)) continue;
        memcpy(name, e->d_name, len - 4); name[len - 4] = '\0';
        int64_t start = filename_epoch_ms(name);
        if (!start) continue;
        char source[OXIMETRY_CANONICAL_MAX_PATH];
        if (!path_join2(source, sizeof(source), dir, e->d_name)) continue;
        if (!source_complete_format_a(source)) {
            ESP_LOGI(TAG, "legacy migration skipped %s (not a complete OxyII Format A file; retained)", source);
            continue;
        }
        if (oximetry_canonical_convert_format_a(device_id, name, source, start) != ESP_OK)
            ESP_LOGW(TAG, "legacy migration failed %s (source retained)", source);
        vTaskDelay(1);
    }
    closedir(d);
    return ESP_OK;
}

esp_err_t oximetry_canonical_migrate_all_legacy(void)
{
    if (oximetry_canonical_ensure_dirs() != ESP_OK) return ESP_ERR_INVALID_STATE;
    char files_dir[OXIMETRY_CANONICAL_MAX_PATH];
    if (snprintf(files_dir, sizeof(files_dir), "%s/files", OX_ROOT) >= (int)sizeof(files_dir))
        return ESP_ERR_INVALID_SIZE;
    DIR *d = opendir(files_dir);
    if (!d) return ESP_OK;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!safe_component(e->d_name, OXIMETRY_CANONICAL_MAX_COMPONENT)) continue;
        oximetry_canonical_migrate_legacy(e->d_name);
    }
    closedir(d);
    return ESP_OK;
}

esp_err_t oximetry_canonical_reconcile(void)
{
    if (oximetry_canonical_ensure_dirs() != ESP_OK) return ESP_ERR_INVALID_STATE;

    /* A converting pointer plus a complete source is enough to resume the
     * deterministic conversion after reboot. */
    DIR *staging = opendir(OX_STAGING);
    if (staging) {
        struct dirent *se;
        while ((se = readdir(staging)) != NULL) {
            if (!safe_component(se->d_name, OXIMETRY_CANONICAL_MAX_COMPONENT)) continue;
            char stage_dir[OXIMETRY_CANONICAL_MAX_PATH], pointer[OXIMETRY_CANONICAL_MAX_PATH];
            if (!path_join2(stage_dir, sizeof(stage_dir), OX_STAGING, se->d_name) ||
                !path_join2(pointer, sizeof(pointer), stage_dir, "recording.json")) continue;
            cJSON *p = read_json_file(pointer);
            if (!p) continue;
            cJSON *state = cJSON_GetObjectItem(p, "state");
            cJSON *device = cJSON_GetObjectItem(p, "device_key");
            cJSON *start = cJSON_GetObjectItem(p, "start_epoch_ms");
            char source[OXIMETRY_CANONICAL_MAX_PATH];
            bool can_resume = cJSON_IsString(state) && strcmp(state->valuestring, "converting") == 0 &&
                              cJSON_IsString(device) && cJSON_IsNumber(start) &&
                              path_join3(source, sizeof(source), stage_dir, "source", "source.bin");
            if (can_resume && oximetry_canonical_convert_format_a(
                    device->valuestring, se->d_name, source, (int64_t)start->valuedouble) != ESP_OK)
                ESP_LOGW(TAG, "staging conversion still pending: %s", se->d_name);
            cJSON_Delete(p);
            vTaskDelay(1);
        }
        closedir(staging);
    }

    DIR *days = opendir(OX_RECORDINGS);
    if (!days) return ESP_OK;
    struct dirent *de;
    while ((de = readdir(days)) != NULL) {
        if (!valid_day_name(de->d_name)) continue;
        char day_path[OXIMETRY_CANONICAL_MAX_PATH];
        if (!path_join2(day_path, sizeof(day_path), OX_RECORDINGS, de->d_name)) continue;
        DIR *records = opendir(day_path);
        if (!records) continue;
        struct dirent *re;
        while ((re = readdir(records)) != NULL) {
            if (!safe_component(re->d_name, OXIMETRY_CANONICAL_MAX_COMPONENT)) continue;
            char record_path[OXIMETRY_CANONICAL_MAX_PATH];
            if (!path_join2(record_path, sizeof(record_path), day_path, re->d_name)) continue;
            if (!recording_ready_at(record_path))
                ESP_LOGW(TAG, "recording is not ready: %s", record_path);
            vTaskDelay(1);
        }
        closedir(records);
    }
    closedir(days);
    return ESP_OK;
}

esp_err_t oximetry_canonical_list_ready(cJSON **out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    *out = NULL;
    if (oximetry_canonical_ensure_dirs() != ESP_OK) return ESP_ERR_INVALID_STATE;
    cJSON *arr = cJSON_CreateArray();
    if (!arr) return ESP_ERR_NO_MEM;

    DIR *days = opendir(OX_RECORDINGS);
    if (!days) {
        *out = arr;
        return ESP_OK;
    }
    struct dirent *de;
    int count = 0;
    while ((de = readdir(days)) != NULL && count < OXIMETRY_CANONICAL_MAX_RECORDINGS) {
        if (!valid_day_name(de->d_name)) continue;
        char day_path[OXIMETRY_CANONICAL_MAX_PATH];
        if (!path_join2(day_path, sizeof(day_path), OX_RECORDINGS, de->d_name)) continue;
        DIR *records = opendir(day_path);
        if (!records) continue;
        struct dirent *re;
        while ((re = readdir(records)) != NULL && count < OXIMETRY_CANONICAL_MAX_RECORDINGS) {
            if (!safe_component(re->d_name, OXIMETRY_CANONICAL_MAX_COMPONENT)) continue;
            char record_path[OXIMETRY_CANONICAL_MAX_PATH];
            if (!path_join2(record_path, sizeof(record_path), day_path, re->d_name)) continue;
            if (!recording_ready_at(record_path)) continue;
            char pointer[OXIMETRY_CANONICAL_MAX_PATH];
            if (!path_join2(pointer, sizeof(pointer), record_path, "recording.json")) continue;
            cJSON *p = read_json_file(pointer);
            if (!p) continue;
            cJSON_AddItemToArray(arr, p);
            count++;
        }
        closedir(records);
    }
    closedir(days);
    *out = arr;
    return ESP_OK;
}

static esp_err_t resolve_recording_internal(const char *recording_id, char *out,
                                            size_t out_size)
{
    if (!valid_recording_id(recording_id) || !out || out_size == 0) return ESP_ERR_INVALID_ARG;
    DIR *days = opendir(OX_RECORDINGS);
    if (!days) return ESP_ERR_NOT_FOUND;
    struct dirent *de;
    while ((de = readdir(days)) != NULL) {
        if (!valid_day_name(de->d_name)) continue;
        char candidate[OXIMETRY_CANONICAL_MAX_PATH];
        if (!path_join3(candidate, sizeof(candidate), OX_RECORDINGS,
                        de->d_name, recording_id)) continue;
        struct stat st;
        if (stat(candidate, &st) != 0 || !S_ISDIR(st.st_mode) || !recording_ready_at(candidate))
            continue;
        if (strlen(candidate) + 1 > out_size) {
            closedir(days);
            return ESP_ERR_INVALID_SIZE;
        }
        strcpy(out, candidate);
        closedir(days);
        return ESP_OK;
    }
    closedir(days);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t oximetry_canonical_resolve_recording(const char *recording_id,
                                               char *out_path,
                                               size_t out_path_size)
{
    if (oximetry_canonical_ensure_dirs() != ESP_OK) return ESP_ERR_INVALID_STATE;
    return resolve_recording_internal(recording_id, out_path, out_path_size);
}

esp_err_t oximetry_canonical_resolve_track(const char *recording_id,
                                           const char *track_id,
                                           char *out_path,
                                           size_t out_path_size)
{
    if (!track_id || !safe_component(track_id, 32) ||
        (strcmp(track_id, "vitals") != 0 && strcmp(track_id, "pleth") != 0 &&
         strcmp(track_id, "pleth_mm") != 0 && strcmp(track_id, "events") != 0))
        return ESP_ERR_INVALID_ARG;
    char dir[OXIMETRY_CANONICAL_MAX_PATH];
    esp_err_t e = oximetry_canonical_resolve_recording(recording_id, dir, sizeof(dir));
    if (e != ESP_OK) return e;
    char pointer[OXIMETRY_CANONICAL_MAX_PATH];
    if (!path_join2(pointer, sizeof(pointer), dir, "recording.json")) return ESP_ERR_INVALID_SIZE;
    cJSON *p = read_json_file(pointer);
    if (!p) return ESP_ERR_NOT_FOUND;
    cJSON *gen = cJSON_GetObjectItem(p, "active_generation");
    int generation = cJSON_IsNumber(gen) ? gen->valueint : 0;
    cJSON_Delete(p);
    if (generation <= 0 || generation > 100000) return ESP_ERR_NOT_FOUND;
    const char *ext = strcmp(track_id, "events") == 0 ? "jsonl" : "snt";
    if (snprintf(out_path, out_path_size, "%s/%s/%d/data/%s.%s", dir, OX_GEN,
                 generation, track_id, ext) >= (int)out_path_size)
        return ESP_ERR_INVALID_SIZE;
    struct stat st;
    return stat(out_path, &st) == 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t oximetry_canonical_get_manifest(const char *recording_id, cJSON **out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    *out = NULL;
    char dir[OXIMETRY_CANONICAL_MAX_PATH];
    esp_err_t e = oximetry_canonical_resolve_recording(recording_id, dir, sizeof(dir));
    if (e != ESP_OK) return e;
    char pointer[OXIMETRY_CANONICAL_MAX_PATH];
    if (!path_join2(pointer, sizeof(pointer), dir, "recording.json")) return ESP_ERR_INVALID_SIZE;
    cJSON *p = read_json_file(pointer);
    if (!p) return ESP_ERR_NOT_FOUND;
    cJSON *gen = cJSON_GetObjectItem(p, "active_generation");
    int generation = cJSON_IsNumber(gen) ? gen->valueint : 0;
    cJSON_Delete(p);
    if (generation <= 0) return ESP_ERR_NOT_FOUND;
    char manifest[OXIMETRY_CANONICAL_MAX_PATH];
    char gen_name[16];
    snprintf(gen_name, sizeof(gen_name), "%d", generation);
    if (!path_join4(manifest, sizeof(manifest), dir, OX_GEN, gen_name, "manifest.json"))
        return ESP_ERR_INVALID_SIZE;
    *out = read_json_file(manifest);
    return *out ? ESP_OK : ESP_ERR_NOT_FOUND;
}

char *oximetry_canonical_list_json(void)
{
    cJSON *arr = NULL;
    if (oximetry_canonical_list_ready(&arr) != ESP_OK || !arr) return NULL;
    char *s = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return s;
}

char *oximetry_canonical_manifest_json(const char *recording_id)
{
    cJSON *obj = NULL;
    if (oximetry_canonical_get_manifest(recording_id, &obj) != ESP_OK || !obj) return NULL;
    char *s = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    return s;
}
