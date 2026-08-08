/*
 * SomnoTrace - SD card storage initialisation and FATFS mount
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

#include "sd_storage.h"

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <dirent.h>

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "ff.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "bsp_display.h"

static const char *TAG = "sd_storage";

static bool s_mounted = false;
static sdmmc_card_t *s_card = NULL;

/* ── Space policy thresholds ──────────────────────────────────────────
 * A night of raw stream data is ~3-4 MB, plus derived EDFs.  The reserve
 * keeps enough room for the session about to start plus its recovery
 * metadata; the floor is the point at which recording is refused. */
#define SD_RESERVE_BYTES   (24ULL * 1024 * 1024)   /* warn below 24 MB  */
#define SD_FLOOR_BYTES     (8ULL * 1024 * 1024)    /* refuse below 8 MB */

/* ── Arbitration state ────────────────────────────────────────────── */
static SemaphoreHandle_t s_lease_mutex = NULL;   /* guards the counters  */
static SemaphoreHandle_t s_export_sem = NULL;    /* EXPORT/DESTRUCTIVE   */
static volatile int s_recording = 0;
static volatile int s_uploading = 0;

static void lease_init_once(void)
{
    if (!s_lease_mutex) s_lease_mutex = xSemaphoreCreateMutex();
    /* Recursive: a day rebuild holds the export lease across the whole
     * transaction and calls the per-session generator inside it, which takes
     * the same lease.  A plain mutex would self-deadlock. */
    if (!s_export_sem) s_export_sem = xSemaphoreCreateRecursiveMutex();
}

esp_err_t sd_storage_init(void)
{
    ESP_LOGI(TAG, "initialising SDMMC 4-bit mode...");

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_1;
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;  /* 40 MHz max; driver auto-negotiates down if card doesn't support it */

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.clk = GPIO_NUM_16;
    slot_config.cmd = GPIO_NUM_15;
    slot_config.d0  = GPIO_NUM_17;
    slot_config.d1  = GPIO_NUM_18;
    slot_config.d2  = GPIO_NUM_13;
    slot_config.d3  = GPIO_NUM_14;
    slot_config.width = 4;
    /* Internal pull-ups are often too weak for SD cards.
     * The Waveshare board should have external pull-ups on the SD lines. */
    slot_config.flags = SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 16,
        .allocation_unit_size = 0,
    };

    sdmmc_card_t *card;
    esp_err_t ret = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot_config,
                                             &mount_config, &card);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "4-bit mount failed (%s), trying 1-bit mode", esp_err_to_name(ret));
        slot_config.width = 1;
        ret = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot_config,
                                       &mount_config, &card);
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "failed to mount SD card: %s (0x%x)", esp_err_to_name(ret), ret);
        ESP_LOGE(TAG, "check: SD card inserted? pull-ups? GPIO pins 13-18?");
        return ret;
    }

    s_mounted = true;
    s_card = card;
    lease_init_once();

    sdmmc_card_print_info(stdout, card);

    /* Create the SomnoTrace app-root and its subtrees. The parent .somnotrace/
     * must be created before its children (FATFS mkdir is non-recursive). */
    mkdir(SD_APP_DIR, 0775);
    mkdir(SD_SESSIONS_DIR, 0775);
    mkdir(SD_STREAMS_DIR, 0775);
    mkdir(SD_SUMMARIES_DIR, 0775);
    mkdir(SD_LOG_DIR, 0775);
    mkdir(SD_UPLOAD_STATE_DIR, 0775);

    /* Create ResMed-compatible export directory tree */
    mkdir(SD_SDCARD_DIR, 0775);
    mkdir(SD_SDCARD_DATALOG, 0775);
    mkdir(SD_SDCARD_SETTINGS, 0775);

    ESP_LOGI(TAG, "SD mounted at %s, directory tree ready", SD_MOUNT_POINT);

    return ESP_OK;
}

bool sd_storage_is_ready(void)
{
    return s_mounted;
}

/* ── Free space ───────────────────────────────────────────────────── */

esp_err_t sd_storage_get_free(uint64_t *free_bytes, uint64_t *total_bytes)
{
    if (!s_mounted) return ESP_ERR_INVALID_STATE;

    FATFS *fs = NULL;
    DWORD free_clst = 0;
    /* Drive "0:" — the single mounted FATFS volume. */
    if (f_getfree("0:", &free_clst, &fs) != FR_OK || !fs) {
        return ESP_FAIL;
    }
    if (total_bytes)
        *total_bytes = (uint64_t)fs->n_fatent * fs->csize * fs->ssize;
    if (free_bytes)
        *free_bytes = (uint64_t)free_clst * fs->csize * fs->ssize;
    return ESP_OK;
}

/* Reclaim derived (regenerable) output so raw capture can proceed.
 * Only SDCARD/ is eligible: it is fully rebuildable from
 * .somnotrace/sessions/, which is the source of truth. */
static uint64_t reclaim_derived_output(void)
{
    /* Deliberately conservative: report what could be reclaimed and let the
     * user act.  Automatic deletion of derived data is only safe once the
     * per-day export/upload state machine can prove a day is reproducible
     * and already uploaded, so we do not delete here. */
    uint64_t free_bytes = 0;
    sd_storage_get_free(&free_bytes, NULL);
    return free_bytes;
}

bool sd_storage_reserve_for_recording(void)
{
    if (!s_mounted) return false;

    uint64_t free_bytes = 0, total = 0;
    if (sd_storage_get_free(&free_bytes, &total) != ESP_OK) {
        /* Cannot tell — do not block therapy recording on a failed query. */
        ESP_LOGW(TAG, "free-space query failed; allowing recording");
        return true;
    }

    if (free_bytes >= SD_RESERVE_BYTES) return true;

    ESP_LOGW(TAG, "low free space: %llu KB free of %llu KB",
             (unsigned long long)(free_bytes / 1024),
             (unsigned long long)(total / 1024));

    if (free_bytes < SD_FLOOR_BYTES) {
        free_bytes = reclaim_derived_output();
        if (free_bytes < SD_FLOOR_BYTES) {
            ESP_LOGE(TAG, "below hard floor (%llu KB) — refusing to record",
                     (unsigned long long)(free_bytes / 1024));
            bsp_display_set_notice("SD full");
            return false;
        }
    }

    bsp_display_set_notice("SD nearly full");
    return true;
}

/* ── Arbitration ──────────────────────────────────────────────────── */

void sd_storage_recording_begin(void)
{
    lease_init_once();
    if (!s_lease_mutex) { s_recording++; return; }
    xSemaphoreTake(s_lease_mutex, portMAX_DELAY);
    s_recording++;
    xSemaphoreGive(s_lease_mutex);
}

void sd_storage_recording_end(void)
{
    if (!s_lease_mutex) { if (s_recording > 0) s_recording--; return; }
    xSemaphoreTake(s_lease_mutex, portMAX_DELAY);
    if (s_recording > 0) s_recording--;
    xSemaphoreGive(s_lease_mutex);
}

bool sd_storage_recording_active(void)
{
    return s_recording > 0;
}

bool sd_storage_lease_acquire(sd_lease_t role, uint32_t timeout_ms)
{
    lease_init_once();
    if (!s_export_sem || !s_lease_mutex) return true;   /* pre-init: allow */

    TickType_t wait = pdMS_TO_TICKS(timeout_ms);

    switch (role) {
    case SD_LEASE_DESTRUCTIVE:
        /* Never destroy data while it is being produced or consumed. */
        if (s_recording > 0) {
            ESP_LOGW(TAG, "destructive op refused: recording in progress");
            return false;
        }
        if (s_uploading > 0) {
            ESP_LOGW(TAG, "destructive op refused: upload in progress");
            return false;
        }
        if (xSemaphoreTakeRecursive(s_export_sem, wait) != pdTRUE) {
            ESP_LOGW(TAG, "destructive op refused: export in progress");
            return false;
        }
        if (s_recording > 0) {   /* re-check after acquiring */
            xSemaphoreGiveRecursive(s_export_sem);
            ESP_LOGW(TAG, "destructive op refused: recording started");
            return false;
        }
        return true;

    case SD_LEASE_EXPORT:
        /* Serialised against other exports and destructive work, but
         * permitted during recording: the previous session still needs
         * exporting while the next one records. */
        if (xSemaphoreTakeRecursive(s_export_sem, wait) != pdTRUE) {
            ESP_LOGW(TAG, "export lease busy");
            return false;
        }
        return true;

    case SD_LEASE_UPLOAD:
        /* Held for the duration of the upload so a day can never be read
         * while it is being replaced by a rebuild. */
        if (xSemaphoreTakeRecursive(s_export_sem, wait) != pdTRUE) {
            ESP_LOGW(TAG, "upload lease busy (export in progress)");
            return false;
        }
        xSemaphoreTake(s_lease_mutex, portMAX_DELAY);
        s_uploading++;
        xSemaphoreGive(s_lease_mutex);
        return true;
    }
    return false;
}

void sd_storage_lease_release(sd_lease_t role)
{
    if (!s_export_sem || !s_lease_mutex) return;

    switch (role) {
    case SD_LEASE_EXPORT:
    case SD_LEASE_DESTRUCTIVE:
        xSemaphoreGiveRecursive(s_export_sem);
        break;
    case SD_LEASE_UPLOAD:
        xSemaphoreTake(s_lease_mutex, portMAX_DELAY);
        if (s_uploading > 0) s_uploading--;
        xSemaphoreGive(s_lease_mutex);
        xSemaphoreGiveRecursive(s_export_sem);
        break;
    }
}

esp_err_t sd_storage_format(void)
{
    if (!s_mounted || !s_card) {
        ESP_LOGE(TAG, "format: SD card not mounted");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGW(TAG, "format: formatting SD card — ALL DATA WILL BE LOST");

    /* esp_vfs_fat_sdcard_format() unmounts, formats (f_mkfs), and remounts
     * the filesystem at the same mount point.  The card handle remains valid. */
    esp_err_t ret = esp_vfs_fat_sdcard_format(SD_MOUNT_POINT, s_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "format: esp_vfs_fat_sdcard_format failed: %s", esp_err_to_name(ret));
        s_mounted = false;
        return ret;
    }

    /* Recreate the SomnoTrace directory tree (format wipes everything). */
    mkdir(SD_APP_DIR, 0775);
    mkdir(SD_SESSIONS_DIR, 0775);
    mkdir(SD_STREAMS_DIR, 0775);
    mkdir(SD_SUMMARIES_DIR, 0775);
    mkdir(SD_LOG_DIR, 0775);
    mkdir(SD_UPLOAD_STATE_DIR, 0775);
    mkdir(SD_SDCARD_DIR, 0775);
    mkdir(SD_SDCARD_DATALOG, 0775);
    mkdir(SD_SDCARD_SETTINGS, 0775);

    ESP_LOGI(TAG, "format: SD card formatted and directory tree recreated");
    return ESP_OK;
}
