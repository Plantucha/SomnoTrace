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

#include "device_settings.h"
#include "bsp_display.h"

#include <string.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"
#include "esp_log.h"

static const char *TAG = "dev_settings";

#define NVS_NAMESPACE "device"
#define NVS_KEY_BRIGHTNESS   "bright"
#define NVS_KEY_LCD_THERAPY  "lcd_thr"

#define DEFAULT_BRIGHTNESS       50
#define MIN_BRIGHTNESS           10
#define MAX_BRIGHTNESS           100

static device_settings_t s_settings;

esp_err_t device_settings_load(device_settings_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->brightness = DEFAULT_BRIGHTNESS;
    cfg->lcd_therapy_mode = LCD_THERAPY_GRAPH;

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "no device settings in NVS — using defaults");
        memcpy(&s_settings, cfg, sizeof(s_settings));
        return ESP_ERR_NVS_NOT_FOUND;
    }

    uint8_t u8val;
    if (nvs_get_u8(h, NVS_KEY_BRIGHTNESS, &u8val) == ESP_OK) {
        cfg->brightness = (u8val < MIN_BRIGHTNESS) ? MIN_BRIGHTNESS :
                          (u8val > MAX_BRIGHTNESS) ? MAX_BRIGHTNESS : u8val;
    }
    if (nvs_get_u8(h, NVS_KEY_LCD_THERAPY, &u8val) == ESP_OK) {
        cfg->lcd_therapy_mode = (lcd_therapy_mode_t)u8val;
    }

    nvs_close(h);
    memcpy(&s_settings, cfg, sizeof(s_settings));
    ESP_LOGI(TAG, "loaded: brightness=%u%%, lcd_therapy=%u",
             s_settings.brightness, s_settings.lcd_therapy_mode);
    return ESP_OK;
}

esp_err_t device_settings_save(const device_settings_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;

    nvs_handle_t h;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (ret != ESP_OK) return ret;

    nvs_set_u8(h, NVS_KEY_BRIGHTNESS, cfg->brightness);
    nvs_set_u8(h, NVS_KEY_LCD_THERAPY, (uint8_t)cfg->lcd_therapy_mode);
    ret = nvs_commit(h);
    nvs_close(h);

    if (ret == ESP_OK) {
        memcpy(&s_settings, cfg, sizeof(s_settings));
        ESP_LOGI(TAG, "saved: brightness=%u%%, lcd_therapy=%u",
                 s_settings.brightness, s_settings.lcd_therapy_mode);
    }
    return ret;
}

const device_settings_t *device_settings_get(void)
{
    return &s_settings;
}

esp_err_t device_settings_set_brightness(uint8_t percent)
{
    if (percent < MIN_BRIGHTNESS) percent = MIN_BRIGHTNESS;
    if (percent > MAX_BRIGHTNESS) percent = MAX_BRIGHTNESS;

    s_settings.brightness = percent;
    bsp_display_set_brightness(percent);
    return ESP_OK;
}

esp_err_t device_settings_set_lcd_therapy_mode(lcd_therapy_mode_t mode)
{
    s_settings.lcd_therapy_mode = mode;
    return ESP_OK;
}

esp_err_t device_settings_get_json(char **out_json)
{
    if (!out_json) return ESP_ERR_INVALID_ARG;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "brightness", s_settings.brightness);
    cJSON_AddNumberToObject(root, "lcd_therapy_mode", (int)s_settings.lcd_therapy_mode);

    *out_json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return *out_json ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t device_settings_save_json(const char *json_str)
{
    if (!json_str) return ESP_ERR_INVALID_ARG;

    cJSON *root = cJSON_Parse(json_str);
    if (!root) {
        ESP_LOGE(TAG, "failed to parse settings JSON");
        return ESP_ERR_INVALID_STATE;
    }

    device_settings_t cfg;
    memcpy(&cfg, &s_settings, sizeof(cfg));

    cJSON *v;
    if ((v = cJSON_GetObjectItem(root, "brightness")) && cJSON_IsNumber(v)) {
        int val = v->valueint;
        if (val < MIN_BRIGHTNESS) val = MIN_BRIGHTNESS;
        if (val > MAX_BRIGHTNESS) val = MAX_BRIGHTNESS;
        cfg.brightness = (uint8_t)val;
    }
    if ((v = cJSON_GetObjectItem(root, "lcd_therapy_mode")) && cJSON_IsNumber(v)) {
        int val = v->valueint;
        cfg.lcd_therapy_mode = (val == LCD_THERAPY_OFF) ? LCD_THERAPY_OFF : LCD_THERAPY_GRAPH;
    }

    cJSON_Delete(root);

    /* Apply brightness immediately */
    bsp_display_set_brightness(cfg.brightness);

    return device_settings_save(&cfg);
}
