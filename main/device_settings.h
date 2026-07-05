/*
 * SomnoTrace - Device hardware settings (brightness, LCD therapy mode)
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 *
 * This file is part of SomnoTrace.
 *
 * SomnoTrace is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
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

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/* LCD behaviour during therapy */
typedef enum {
    LCD_THERAPY_GRAPH = 0,   /* Live flow graph (default) */
    LCD_THERAPY_OFF   = 1,   /* Backlight completely off */
} lcd_therapy_mode_t;

typedef struct {
    uint8_t brightness;        /* 10-100 percent */
    lcd_therapy_mode_t lcd_therapy_mode;
} device_settings_t;

/* Load settings from NVS. Returns ESP_OK if loaded, ESP_ERR_NVS_NOT_FOUND
 * if no settings stored (defaults are filled in). */
esp_err_t device_settings_load(device_settings_t *cfg);

/* Save settings to NVS. */
esp_err_t device_settings_save(const device_settings_t *cfg);

/* Get current in-memory settings (loaded at boot). */
const device_settings_t *device_settings_get(void);

/* Set brightness immediately (applies to hardware + updates in-memory copy).
 * Does NOT persist to NVS — call device_settings_save() for that. */
esp_err_t device_settings_set_brightness(uint8_t percent);

/* Set LCD therapy mode (updates in-memory copy only).
 * Call device_settings_save() to persist. */
esp_err_t device_settings_set_lcd_therapy_mode(lcd_therapy_mode_t mode);

/* Get settings as JSON string for web UI. Caller must free(). */
esp_err_t device_settings_get_json(char **out_json);

/* Save settings from JSON string (from web UI POST body).
 * Also applies brightness immediately. */
esp_err_t device_settings_save_json(const char *json_str);
