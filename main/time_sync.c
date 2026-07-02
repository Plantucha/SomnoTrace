/*
 * SomnoTrace - NTP time synchronisation with DHCP option 42 support
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 *
 * This file is part of SomnoTrace.
 *
 * SomnoTrace is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * SomnoTrace is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 *
 * ADDITIONAL TERM (GPLv3 Section 7(b)): Redistributions must preserve the
 * attribution "Based on SomnoTrace, originally created by Ilya Kruchinin
 * (https://github.com/ilyakruchinin)." See the NOTICE file for details.
 */

#include "time_sync.h"

#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "esp_log.h"
#include "esp_sntp.h"
#include "nvs_flash.h"

#include "esp_netif_sntp.h"

static const char *TAG = "time_sync";

#define NVS_NAMESPACE    "cfg"
#define NVS_KEY_TZ_STR   "tz_str"
#define NVS_KEY_TZ_NAME  "tz_name"
#define TZ_STR_MAX       64
#define TZ_NAME_MAX      40
#define SNTP_SYNC_MS    (3600 * 1000)   /* 1 hour */

static bool s_synced = false;

static void sntp_sync_cb(struct timeval *tv)
{
    s_synced = true;
    time_t now = tv->tv_sec;
    struct tm tm_info;
    localtime_r(&now, &tm_info);
    ESP_LOGI(TAG, "NTP sync OK: %04d-%02d-%02d %02d:%02d:%02d",
             tm_info.tm_year + 1900, tm_info.tm_mon + 1, tm_info.tm_mday,
             tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec);
}

static void apply_timezone(const char *tz_str)
{
    if (!tz_str || tz_str[0] == '\0') {
        tz_str = "UTC0";
    }
    setenv("TZ", tz_str, 1);
    tzset();
    ESP_LOGI(TAG, "timezone set to %s", tz_str);
}

esp_err_t time_sync_set_timezone(const char *tz_str, const char *tz_name)
{
    if (!tz_str || tz_str[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, NVS_KEY_TZ_STR, tz_str);
    if (err == ESP_OK && tz_name && tz_name[0]) {
        nvs_set_str(h, NVS_KEY_TZ_NAME, tz_name);
    }
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) {
        apply_timezone(tz_str);
    }
    return err;
}

void time_sync_get_timezone(char *tz_str, size_t tz_str_len)
{
    if (!tz_str || tz_str_len == 0) return;
    tz_str[0] = '\0';
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_str(h, NVS_KEY_TZ_STR, tz_str, &tz_str_len);
        nvs_close(h);
    }
}

void time_sync_get_tz_name(char *tz_name, size_t tz_name_len)
{
    if (!tz_name || tz_name_len == 0) return;
    tz_name[0] = '\0';
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_str(h, NVS_KEY_TZ_NAME, tz_name, &tz_name_len);
        nvs_close(h);
    }
}

bool time_sync_is_synced(void)
{
    return s_synced;
}

esp_err_t time_sync_init(void)
{
    char tz_str[TZ_STR_MAX];
    time_sync_get_timezone(tz_str, sizeof(tz_str));
    apply_timezone(tz_str);

    /* Configure SNTP using esp_netif_sntp — this API handles DHCP option 42
     * servers automatically when CONFIG_LWIP_DHCP_GET_NTP_SRV is enabled. */
    esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    sntp_cfg.smooth_sync = false;
    sntp_cfg.server_from_dhcp = true;
    sntp_cfg.wait_for_sync = false;
    sntp_cfg.start = false;
    sntp_cfg.renew_servers_after_new_IP = true;
    sntp_cfg.ip_event_to_renew = IP_EVENT_STA_GOT_IP;
    sntp_cfg.sync_cb = sntp_sync_cb;
    sntp_cfg.index_of_first_server = 1;  /* slot 0 = static fallback */

    esp_err_t err = esp_netif_sntp_init(&sntp_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_sntp_init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Add a second fallback server */
    esp_sntp_setservername(2, "time.google.com");

    /* Set periodic re-sync interval (1 hour) before starting */
    esp_sntp_set_sync_interval(SNTP_SYNC_MS);

    err = esp_netif_sntp_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_sntp_start failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "SNTP started — DHCP option 42 + pool.ntp.org + time.google.com, sync every %d ms", SNTP_SYNC_MS);
    return ESP_OK;
}
