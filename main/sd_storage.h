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

#pragma once

#include <stdbool.h>
#include "esp_err.h"

#define SD_MOUNT_POINT   "/somnotrace"

/* ESP-native session data (raw streams, spool files, internal state) */
#define SD_SESSIONS_DIR      SD_MOUNT_POINT "/.sessions"
#define SD_STREAMS_DIR       SD_SESSIONS_DIR "/streams"
#define SD_SUMMARIES_DIR     SD_SESSIONS_DIR "/summaries"

/* ResMed-compatible export folder (self-contained, OSCAR-ready) */
#define SD_SDCARD_DIR        SD_MOUNT_POINT "/SDCARD"
#define SD_SDCARD_DATALOG    SD_SDCARD_DIR "/DATALOG"
#define SD_SDCARD_SETTINGS   SD_SDCARD_DIR "/SETTINGS"

/* Initialise SDMMC 4-bit mode and mount FATFS at /somnotrace.
 * Creates the .sessions/ and SDCARD/ directory trees if they don't exist.
 * Returns ESP_OK on success. Non-fatal — caller may continue without SD. */
esp_err_t sd_storage_init(void);

/* Returns true if the SD card is mounted and ready. */
bool sd_storage_is_ready(void);
