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

static const char *TAG = "sd_storage";

static bool s_mounted = false;

esp_err_t sd_storage_init(void)
{
    ESP_LOGI(TAG, "initialising SDMMC 4-bit mode...");

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_1;
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;   /* 20 MHz — safe default */

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

    sdmmc_card_print_info(stdout, card);

    /* Create ESP-native directory tree */
    mkdir(SD_SESSIONS_DIR, 0775);
    mkdir(SD_STREAMS_DIR, 0775);
    mkdir(SD_SUMMARIES_DIR, 0775);
    mkdir(SD_MOUNT_POINT "/.logs", 0775);

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
