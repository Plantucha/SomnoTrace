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

/* ── Helpers ────────────────────────────────────────────────────────── */

/* Write a raw binary buffer to a file. Returns ESP_OK on success. */
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
    ESP_LOGI(TAG, "wrote %s (%u bytes)", path, (unsigned)len);
    return ESP_OK;
}

/* Write a cJSON object to a file as formatted JSON. Returns ESP_OK on success. */
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

/* Convert epoch milliseconds to ISO-8601 UTC string.
 * Format: "2026-06-25T15:08:00.000Z" (AS11 spool fromDateTime format). */
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

/* ── Spool collection ───────────────────────────────────────────────── */

/* Pull a spool and save the raw protobuf to a file in the post-therapy dir.
 * Returns ESP_OK on success, non-fatal on failure (logs a warning). */
static esp_err_t collect_spool(const char *dir, const char *spool_type,
                               const char *filename, const char *from_dt)
{
    uint8_t *data = NULL;
    size_t len = 0;

    esp_err_t ret = as11_ble_spool_pull(spool_type, from_dt, &data, &len);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "spool pull failed for %s: %s", spool_type, esp_err_to_name(ret));
        /* Non-fatal — EDF generation will handle missing data */
        return ret;
    }

    if (data && len > 0) {
        char path[330];
        snprintf(path, sizeof(path), "%s/%s", dir, filename);
        write_bin_file(path, data, len);
    } else {
        ESP_LOGI(TAG, "spool %s is empty (no data for this session)", spool_type);
        /* Write an empty file to signal "pulled but empty" vs "not pulled" */
        char path[330];
        snprintf(path, sizeof(path), "%s/%s", dir, filename);
        FILE *f = fopen(path, "wb");
        if (f) fclose(f);
    }

    free(data);
    return ESP_OK;
}

/* ── Device identification via Get RPC ──────────────────────────────── */

/* Device identity fields needed for EDF headers and Identification.json.
 * This list matches the Python reference QUERY_VARS in capture_data.py.
 * The AS11 Get RPC accepts these as a single batch. */
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

/* Therapy settings are fetched via the "SettingProfiles" subtree target.
 * The AS11 Get RPC supports subtree targets — passing "SettingProfiles"
 * returns the entire settings tree in one call, which is far more efficient
 * and reliable than listing 60+ individual variable names (which causes
 * "Invalid Params" errors).  This matches the Python reference code in
 * capture_data.py. */
static const char *const SETTINGS_KEYS[] = {
    "SettingProfiles",
};

static esp_err_t collect_identification(const char *dir)
{
    cJSON *ident = as11_ble_get_values(
        IDENTITY_KEYS, sizeof(IDENTITY_KEYS) / sizeof(IDENTITY_KEYS[0]));
    if (!ident) {
        ESP_LOGW(TAG, "failed to get device identification");
        return ESP_FAIL;
    }

    char path[330];
    snprintf(path, sizeof(path), "%s/identification.json", dir);
    write_json_file(path, ident);
    cJSON_Delete(ident);
    return ESP_OK;
}

static esp_err_t collect_settings(const char *dir)
{
    cJSON *settings = as11_ble_get_values(
        SETTINGS_KEYS, sizeof(SETTINGS_KEYS) / sizeof(SETTINGS_KEYS[0]));
    if (!settings) {
        ESP_LOGW(TAG, "failed to get device settings");
        return ESP_FAIL;
    }

    char path[330];
    snprintf(path, sizeof(path), "%s/settings.json", dir);
    write_json_file(path, settings);
    cJSON_Delete(settings);
    return ESP_OK;
}

/* ── Main collection entry point ────────────────────────────────────── */

esp_err_t post_therapy_collect(const char *session_dir, int64_t start_epoch_ms,
                               int64_t clock_drift_ms)
{
    if (!session_dir) return ESP_ERR_INVALID_ARG;
    if (!sd_storage_is_ready()) {
        ESP_LOGW(TAG, "SD not ready, skipping post-therapy collection");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "=== POST-THERAPY COLLECTION START ===");
    ESP_LOGI(TAG, "session_dir=%s start_epoch_ms=%lld", session_dir, (long long)start_epoch_ms);

    /* Create post-therapy subfolder inside the session directory.
     * This is where all spool data and RPC-sourced data goes.
     * The stream .snt files remain in the session root directory.
     * Together, stream files + post-therapy/ form the complete data warehouse. */
    char pt_dir[300];
    snprintf(pt_dir, sizeof(pt_dir), "%s/post-therapy", session_dir);
    if (mkdir(pt_dir, 0775) != 0 && errno != EEXIST) {
        ESP_LOGE(TAG, "failed to create %s: %s", pt_dir, strerror(errno));
        return ESP_FAIL;
    }

    /* Convert session start time to ISO-8601 UTC for spool fromDateTime.
     * We subtract a small margin (60s) to ensure we capture the full session,
     * in case of clock drift between NTP and AS11. */
    char from_dt[32];
    epoch_ms_to_iso_utc(start_epoch_ms - 60000, from_dt, sizeof(from_dt));
    ESP_LOGI(TAG, "spool fromDateTime=%s", from_dt);

    int errors = 0;

    /* 1. Pull Summary spool → raw protobuf → post-therapy/summary.bin
     *    This contains session statistics and settings for STR.edf generation. */
    if (collect_spool(pt_dir, "Summary", "summary.bin", from_dt) != ESP_OK) {
        errors++;
    }

    /* 2. Pull TherapyEvents-RespiratoryEvents spool → raw protobuf →
     *    post-therapy/respiratory_events.bin
     *    This contains apnea/hypopnea event timestamps for EVE.edf generation.
     *    We also have live events in events.jsonl, but the spool is the
     *    authoritative post-session record.  Both are stored for comparison. */
    if (collect_spool(pt_dir, "TherapyEvents-RespiratoryEvents",
                      "respiratory_events.bin", from_dt) != ESP_OK) {
        errors++;
    }

    /* 3. Get device identification via RPC → post-therapy/identification.json
     *    Contains SerialNumber, MID, VID, ProductCode, etc. needed for
     *    EDF recording ID field and Identification.json for SleepHQ import. */
    if (collect_identification(pt_dir) != ESP_OK) {
        errors++;
    }

    /* 4. Get current therapy settings via RPC → post-therapy/settings.json
     *    Contains all therapy mode settings (pressures, EPR, ramp, etc.)
     *    needed for STR.edf settings rows.  These are the current values
     *    at the time of collection (immediately post-therapy). */
    if (collect_settings(pt_dir) != ESP_OK) {
        errors++;
    }

    /* Write a manifest file documenting what was collected.
     * Includes clock_drift_ms so EDF generation can apply the correct
     * time correction to spool-sourced timestamps without needing
     * to re-read session.json. */
    cJSON *manifest = cJSON_CreateObject();
    cJSON_AddStringToObject(manifest, "collection_time", from_dt);
    cJSON_AddNumberToObject(manifest, "clock_drift_ms", (double)clock_drift_ms);
    cJSON_AddNumberToObject(manifest, "errors", errors);
    char mpath[330];
    snprintf(mpath, sizeof(mpath), "%s/manifest.json", pt_dir);
    write_json_file(mpath, manifest);
    cJSON_Delete(manifest);

    ESP_LOGI(TAG, "=== POST-THERAPY COLLECTION DONE (%d errors) ===", errors);
    return errors > 0 ? ESP_FAIL : ESP_OK;
}
