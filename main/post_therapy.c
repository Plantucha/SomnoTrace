/*
 * SomnoTrace - Post-therapy data collection from AS11 spools and RPC
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

#include "post_therapy.h"
#include "as11_ble.h"
#include "sd_storage.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include <errno.h>

#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "post_therapy";

/* ── Protobuf helpers (minimal, for Summary spool decoding) ─────────── */

static uint64_t pb_decode_varint(const uint8_t *buf, size_t buf_len, size_t *pos)
{
    uint64_t val = 0;
    int shift = 0;
    while (*pos < buf_len) {
        uint8_t b = buf[(*pos)++];
        val |= (uint64_t)(b & 0x7F) << shift;
        if (!(b & 0x80)) break;
        shift += 7;
    }
    return val;
}

/* Extract PeriodStart (field 2, varint) from a Summary record. */
static int64_t extract_period_start(const uint8_t *rec, size_t rec_len)
{
    size_t pos = 0;
    while (pos < rec_len) {
        uint64_t tag = pb_decode_varint(rec, rec_len, &pos);
        int field = (int)(tag >> 3);
        int wire = (int)(tag & 0x07);
        if (field == 0) break;

        if (field == 2 && wire == 0) {
            return (int64_t)pb_decode_varint(rec, rec_len, &pos);
        } else if (wire == 0) {
            pb_decode_varint(rec, rec_len, &pos);
        } else if (wire == 2) {
            size_t lpos = pos;
            uint64_t flen = pb_decode_varint(rec, rec_len, &lpos);
            pos = lpos + flen;
        } else if (wire == 1) {
            pos += 8;
        } else if (wire == 5) {
            pos += 4;
        } else {
            break;
        }
    }
    return 0;
}

/* Compute noon-based day folder (YYYYMMDD) from epoch ms.
 * Sessions before noon belong to the previous day's folder. */
static void noon_day_from_epoch(int64_t epoch_ms, char *out, size_t out_len)
{
    time_t t = (time_t)(epoch_ms / 1000);
    struct tm tm;
    localtime_r(&t, &tm);
    if (tm.tm_hour < 12) {
        t -= 86400;
        localtime_r(&t, &tm);
    }
    snprintf(out, out_len, "%04d%02d%02d",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
}

/* ── File helpers ───────────────────────────────────────────────────── */

static esp_err_t write_bin_file(const char *path, const uint8_t *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "failed to open %s for writing: %s", path, strerror(errno));
        return ESP_FAIL;
    }
    size_t written = fwrite(data, 1, len, f);
    fclose(f);
    if (written != len) {
        ESP_LOGE(TAG, "short write to %s: %u/%u", path, (unsigned)written, (unsigned)len);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* Atomic write: write to temp file, then rename. */
static esp_err_t write_bin_atomic(const char *path, const uint8_t *data, size_t len)
{
    char tmp_path[330];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

    if (write_bin_file(tmp_path, data, len) != ESP_OK) return ESP_FAIL;

    if (rename(tmp_path, path) != 0) {
        ESP_LOGE(TAG, "rename %s → %s failed: %s", tmp_path, path, strerror(errno));
        remove(tmp_path);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "wrote %s (%u bytes)", path, (unsigned)len);
    return ESP_OK;
}

static esp_err_t write_json_file(const char *path, const cJSON *json)
{
    char *str = cJSON_Print(json);
    if (!str) return ESP_FAIL;

    FILE *f = fopen(path, "w");
    if (!f) {
        ESP_LOGE(TAG, "failed to open %s: %s", path, strerror(errno));
        free(str);
        return ESP_FAIL;
    }
    fputs(str, f);
    fputc('\n', f);
    fclose(f);
    free(str);
    ESP_LOGI(TAG, "wrote %s", path);
    return ESP_OK;
}

static void epoch_ms_to_iso_utc(int64_t epoch_ms, char *out, size_t out_len)
{
    time_t t = (time_t)(epoch_ms / 1000);
    int ms = (int)(epoch_ms % 1000);
    struct tm tm;
    gmtime_r(&t, &tm);
    snprintf(out, out_len, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec, ms);
}

/* ── Summary spool decode & per-day storage ─────────────────────────── */

/* Pull Summary spool with 30-day lookback, decode into per-day files.
 * Each day record is written to .sessions/summaries/YYYYMMDD.spool
 * (atomic write — latest pull wins). */
static esp_err_t collect_summary_spool(int64_t clock_drift_ms)
{
    /* Fixed 30-day lookback window — no state file needed. */
    int64_t now_ms = (int64_t)time(NULL) * 1000;
    int64_t from_ms = now_ms - 30LL * 86400 * 1000;
    char from_dt[32];
    epoch_ms_to_iso_utc(from_ms, from_dt, sizeof(from_dt));
    ESP_LOGI(TAG, "Summary spool: fromDateTime=%s (30-day lookback)", from_dt);

    uint8_t *data = NULL;
    size_t len = 0;
    esp_err_t ret = as11_ble_spool_pull("Summary", from_dt, &data, &len);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Summary spool pull failed: %s", esp_err_to_name(ret));
        return ret;
    }
    if (!data || len == 0) {
        ESP_LOGI(TAG, "Summary spool is empty");
        free(data);
        return ESP_OK;
    }

    /* Iterate top-level field-2 wrappers (each contains a day record) */
    int days_written = 0;
    size_t pos = 0;
    while (pos < len) {
        uint64_t tag = pb_decode_varint(data, len, &pos);
        int field = (int)(tag >> 3);
        int wire = (int)(tag & 0x07);
        if (field == 0) break;

        if (field == 2 && wire == 2) {
            size_t lpos = pos;
            uint64_t flen = pb_decode_varint(data, len, &lpos);
            pos = lpos;
            if (pos + flen > len) break;

            const uint8_t *rec = data + pos;
            size_t rec_len = (size_t)flen;

            /* Extract PeriodStart from this day record */
            int64_t period_start = extract_period_start(rec, rec_len);
            if (period_start > 0) {
                /* Apply clock drift correction */
                period_start += clock_drift_ms;

                char day_label[16];
                noon_day_from_epoch(period_start, day_label, sizeof(day_label));

                char spool_path[300];
                snprintf(spool_path, sizeof(spool_path), "%s/%s.spool",
                         SD_SUMMARIES_DIR, day_label);
                write_bin_atomic(spool_path, rec, rec_len);
                days_written++;
            }
            pos += flen;
        } else if (wire == 2) {
            size_t lpos = pos;
            uint64_t flen = pb_decode_varint(data, len, &lpos);
            pos = lpos + flen;
        } else if (wire == 0) {
            pb_decode_varint(data, len, &pos);
        } else if (wire == 1) {
            pos += 8;
        } else if (wire == 5) {
            pos += 4;
        } else {
            break;
        }
    }

    free(data);
    ESP_LOGI(TAG, "Summary spool: wrote %d day record(s) to %s", days_written, SD_SUMMARIES_DIR);
    return ESP_OK;
}

/* ── Spool collection (TherapyEvents) ───────────────────────────────── */

static esp_err_t collect_resp_events(const char *dir, const char *prefix,
                                     const char *from_dt)
{
    uint8_t *data = NULL;
    size_t len = 0;

    esp_err_t ret = as11_ble_spool_pull("TherapyEvents-RespiratoryEvents",
                                        from_dt, &data, &len);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "resp events spool pull failed: %s", esp_err_to_name(ret));
        return ret;
    }

    if (data && len > 0) {
        char path[330];
        snprintf(path, sizeof(path), "%s/%s_resp_events.bin", dir, prefix);
        write_bin_file(path, data, len);
    } else {
        ESP_LOGI(TAG, "resp events spool is empty");
        char path[330];
        snprintf(path, sizeof(path), "%s/%s_resp_events.bin", dir, prefix);
        FILE *f = fopen(path, "wb");
        if (f) fclose(f);
    }

    free(data);
    return ESP_OK;
}

/* ── Device identification via Get RPC ──────────────────────────────── */

static const char *const IDENTITY_KEYS[] = {
    "UniversalIdentifier",
    "SerialNumber",
    "ProductCode",
    "ProductName",
    "ProductGeographicIdentifier",
    "HardwareIdentifier",
    "BootloaderIdentifier",
    "ApplicationIdentifier",
    "ConfigurationIdentifier",
    "PlatformIdentifier",
    "VariantIdentifier",
    "RegionIdentifier",
    "ProfileVariantIdentifier",
    "DataVersionIdentifier",
    "DataModelVersionIdentifier",
};

static const char *const SETTINGS_KEYS[] = {
    "SettingProfiles",
};

static esp_err_t collect_identification(const char *dir, const char *prefix)
{
    cJSON *ident = as11_ble_get_values(
        IDENTITY_KEYS, sizeof(IDENTITY_KEYS) / sizeof(IDENTITY_KEYS[0]));
    if (!ident) {
        ESP_LOGW(TAG, "failed to get device identification");
        return ESP_FAIL;
    }

    char path[330];
    snprintf(path, sizeof(path), "%s/%s_ident.json", dir, prefix);
    write_json_file(path, ident);
    cJSON_Delete(ident);
    return ESP_OK;
}

static esp_err_t collect_settings(const char *dir, const char *prefix)
{
    cJSON *settings = as11_ble_get_values(
        SETTINGS_KEYS, sizeof(SETTINGS_KEYS) / sizeof(SETTINGS_KEYS[0]));
    if (!settings) {
        ESP_LOGW(TAG, "failed to get device settings");
        return ESP_FAIL;
    }

    char path[330];
    snprintf(path, sizeof(path), "%s/%s_settings.json", dir, prefix);
    write_json_file(path, settings);
    cJSON_Delete(settings);
    return ESP_OK;
}

/* ── Main collection entry point ────────────────────────────────────── */

esp_err_t post_therapy_collect(const char *session_dir, const char *file_prefix,
                               int64_t start_epoch_ms, int64_t clock_drift_ms)
{
    if (!session_dir || !file_prefix) return ESP_ERR_INVALID_ARG;
    if (!sd_storage_is_ready()) {
        ESP_LOGW(TAG, "SD not ready, skipping post-therapy collection");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "=== POST-THERAPY COLLECTION START ===");
    ESP_LOGI(TAG, "session_dir=%s prefix=%s", session_dir, file_prefix);

    /* TherapyEvents spool uses session start time (with small margin). */
    char from_dt[32];
    epoch_ms_to_iso_utc(start_epoch_ms - 60000, from_dt, sizeof(from_dt));

    int errors = 0;

    /* 1. Pull Summary spool with 30-day lookback → per-day .spool files */
    if (collect_summary_spool(clock_drift_ms) != ESP_OK) {
        errors++;
    }

    /* 2. Pull TherapyEvents-RespiratoryEvents spool → <prefix>_resp_events.bin */
    if (collect_resp_events(session_dir, file_prefix, from_dt) != ESP_OK) {
        errors++;
    }

    /* 3. Get device identification → <prefix>_ident.json */
    if (collect_identification(session_dir, file_prefix) != ESP_OK) {
        errors++;
    }

    /* 4. Get current settings → <prefix>_settings.json */
    if (collect_settings(session_dir, file_prefix) != ESP_OK) {
        errors++;
    }

    /* Write manifest with clock_drift_ms for EDF generation */
    cJSON *manifest = cJSON_CreateObject();
    cJSON_AddStringToObject(manifest, "collection_time", from_dt);
    cJSON_AddNumberToObject(manifest, "clock_drift_ms", (double)clock_drift_ms);
    cJSON_AddNumberToObject(manifest, "errors", errors);
    char mpath[330];
    snprintf(mpath, sizeof(mpath), "%s/%s_manifest.json", session_dir, file_prefix);
    write_json_file(mpath, manifest);
    cJSON_Delete(manifest);

    ESP_LOGI(TAG, "=== POST-THERAPY COLLECTION DONE (%d errors) ===", errors);
    return errors > 0 ? ESP_FAIL : ESP_OK;
}
