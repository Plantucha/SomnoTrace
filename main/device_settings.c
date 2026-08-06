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
#include "bsp_audio.h"
#include "nvs_writer.h"

#include <string.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"
#include "esp_log.h"

static const char *TAG = "dev_settings";

#define NVS_NAMESPACE "device"
#define NVS_KEY_BRIGHTNESS   "bright"
#define NVS_KEY_LCD_THERAPY  "lcd_thr"
#define NVS_KEY_ALERT_VOL    "alrtvol"

/* Brightness stored in tenth-percent units: 1=0.1%, 200=20.0%
 * Discrete steps: 0.1, 0.2, 0.5, 1, 2, 5, 10, 20 (roughly 2x each) */
#define DEFAULT_BRIGHTNESS       100 /* 10.0% */
#define MIN_BRIGHTNESS           1   /* 0.1% */
#define MAX_BRIGHTNESS           200 /* 20.0% */
#define DEFAULT_ALERT_VOLUME     65
#define MIN_ALERT_VOLUME         50

static device_settings_t s_settings;

esp_err_t device_settings_load(device_settings_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->brightness = DEFAULT_BRIGHTNESS;
    cfg->lcd_therapy_mode = LCD_THERAPY_GRAPH;
    cfg->alert_volume = DEFAULT_ALERT_VOLUME;
    /* Clamp stale NVS values to current valid range */

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
    if (nvs_get_u8(h, NVS_KEY_ALERT_VOL, &u8val) == ESP_OK) {
        cfg->alert_volume = (u8val < MIN_ALERT_VOLUME) ? MIN_ALERT_VOLUME : u8val;
    }

    nvs_close(h);
    memcpy(&s_settings, cfg, sizeof(s_settings));
    ESP_LOGI(TAG, "loaded: brightness=%u (%.1f%%), lcd_therapy=%u, alert_vol=%u",
             s_settings.brightness, s_settings.brightness / 10.0,
             s_settings.lcd_therapy_mode, s_settings.alert_volume);
    return ESP_OK;
}

static esp_err_t do_device_settings_save(void *arg)
{
    const device_settings_t *cfg = (const device_settings_t *)arg;
    nvs_handle_t h;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (ret != ESP_OK) return ret;

    nvs_set_u8(h, NVS_KEY_BRIGHTNESS, cfg->brightness);
    nvs_set_u8(h, NVS_KEY_LCD_THERAPY, (uint8_t)cfg->lcd_therapy_mode);
    nvs_set_u8(h, NVS_KEY_ALERT_VOL, cfg->alert_volume);
    ret = nvs_commit(h);
    nvs_close(h);
    return ret;
}

esp_err_t device_settings_save(const device_settings_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;

    /* Delegate the flash write so callers on a PSRAM stack (httpd) are safe. */
    esp_err_t ret = nvs_writer_run(do_device_settings_save, (void *)cfg);
    if (ret == ESP_OK) {
        memcpy(&s_settings, cfg, sizeof(s_settings));
        ESP_LOGI(TAG, "saved: brightness=%u (%.1f%%), lcd_therapy=%u, alert_vol=%u",
                 s_settings.brightness, s_settings.brightness / 10.0,
                 s_settings.lcd_therapy_mode, s_settings.alert_volume);
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

esp_err_t device_settings_set_alert_volume(uint8_t percent)
{
    if (percent < MIN_ALERT_VOLUME) percent = MIN_ALERT_VOLUME;
    if (percent > 100) percent = 100;
    s_settings.alert_volume = percent;
    bsp_audio_set_volume(percent);
    return ESP_OK;
}

esp_err_t device_settings_get_json(char **out_json)
{
    if (!out_json) return ESP_ERR_INVALID_ARG;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "brightness", s_settings.brightness);
    cJSON_AddNumberToObject(root, "lcd_therapy_mode", (int)s_settings.lcd_therapy_mode);
    cJSON_AddNumberToObject(root, "alert_volume", s_settings.alert_volume);

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
        if (val == LCD_THERAPY_OFF || val == LCD_THERAPY_ALWAYS_OFF) {
            cfg.lcd_therapy_mode = (lcd_therapy_mode_t)val;
        } else {
            cfg.lcd_therapy_mode = LCD_THERAPY_GRAPH;
        }
    }
    if ((v = cJSON_GetObjectItem(root, "alert_volume")) && cJSON_IsNumber(v)) {
        int val = v->valueint;
        if (val < MIN_ALERT_VOLUME) val = MIN_ALERT_VOLUME;
        if (val > 100) val = 100;
        cfg.alert_volume = (uint8_t)val;
    }

    cJSON_Delete(root);

    /* Apply brightness immediately */
    bsp_display_set_brightness(cfg.brightness);
    /* Apply alert volume immediately */
    bsp_audio_set_volume(cfg.alert_volume);

    /* Persist first so device_settings_get() returns the new mode,
     * then re-evaluate backlight based on the new mode.
     * If in SoftAP (force_on), backlight stays on regardless. */
    esp_err_t ret = device_settings_save(&cfg);
    bsp_display_apply_backlight_policy(false);

    return ret;
}
