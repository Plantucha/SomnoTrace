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

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_netif_sntp.h"

static const char *TAG = "time_sync";

#define NVS_NAMESPACE    "cfg"
#define NVS_KEY_TZ_STR   "tz_str"
#define NVS_KEY_TZ_NAME  "tz_name"
#define NVS_KEY_NTP_SRV  "ntp_srv"
#define TZ_STR_MAX       64
#define TZ_NAME_MAX      40
#define NTP_SRV_MAX      64
#define SNTP_SYNC_MS    (3600 * 1000)   /* 1 hour */
#define NTP_INITIAL_TIMEOUT_MS  15000   /* per-attempt wait for initial sync */
#define NTP_INITIAL_ATTEMPTS    3

static bool s_synced = false;
static bool s_initial_sync_done = false;

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

esp_err_t time_sync_set_ntp_server(const char *server)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    if (server && server[0] != '\0') {
        err = nvs_set_str(h, NVS_KEY_NTP_SRV, server);
    } else {
        err = nvs_erase_key(h, NVS_KEY_NTP_SRV);
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    }
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "NTP server set to: %s",
                 (server && server[0]) ? server : "(auto)");
    }
    return err;
}

void time_sync_get_ntp_server(char *server, size_t server_len)
{
    if (!server || server_len == 0) return;
    server[0] = '\0';
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_str(h, NVS_KEY_NTP_SRV, server, &server_len);
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

    /* Check for a user-configured custom NTP server in NVS. */
    char ntp_srv[NTP_SRV_MAX];
    time_sync_get_ntp_server(ntp_srv, sizeof(ntp_srv));
    bool has_custom_ntp = (ntp_srv[0] != '\0');

    esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(
        has_custom_ntp ? ntp_srv : "pool.ntp.org");
    sntp_cfg.smooth_sync = false;
    sntp_cfg.sync_cb = sntp_sync_cb;

    if (has_custom_ntp) {
        /* Custom NTP: use it exclusively, no DHCP, no fallbacks. */
        sntp_cfg.server_from_dhcp = false;
        sntp_cfg.wait_for_sync = false;
        sntp_cfg.start = false;
        sntp_cfg.renew_servers_after_new_IP = false;
        sntp_cfg.ip_event_to_renew = 0;
        ESP_LOGI(TAG, "SNTP started — custom server: %s", ntp_srv);
    } else {
        /* Auto mode: DHCP option 42 + public NTP fallbacks. */
        sntp_cfg.server_from_dhcp = true;
        sntp_cfg.wait_for_sync = false;
        sntp_cfg.start = false;
        sntp_cfg.renew_servers_after_new_IP = true;
        sntp_cfg.ip_event_to_renew = IP_EVENT_STA_GOT_IP;
        sntp_cfg.index_of_first_server = 1;  /* slot 0 = static fallback */
    }

    esp_err_t err = esp_netif_sntp_init(&sntp_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_sntp_init failed: %s", esp_err_to_name(err));
        return err;
    }

    if (!has_custom_ntp) {
        /* Add a second fallback server */
        esp_sntp_setservername(2, "time.google.com");
    }

    /* Set periodic re-sync interval (1 hour) before starting */
    esp_sntp_set_sync_interval(SNTP_SYNC_MS);

    err = esp_netif_sntp_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_sntp_start failed: %s", esp_err_to_name(err));
        return err;
    }

    if (has_custom_ntp) {
        ESP_LOGI(TAG, "SNTP started — custom server: %s, sync every %d ms", ntp_srv, SNTP_SYNC_MS);
    } else {
        ESP_LOGI(TAG, "SNTP started — DHCP option 42 + pool.ntp.org + time.google.com, sync every %d ms", SNTP_SYNC_MS);
    }
    return ESP_OK;
}

bool time_sync_wait_initial(void)
{
    if (s_synced) {
        s_initial_sync_done = true;
        return true;
    }

    /* SNTP is already running from time_sync_init().  It retries on its own
     * (lwIP default retry timeout ~15 s).  We poll s_synced in three windows
     * of NTP_INITIAL_TIMEOUT_MS each, giving ~45 s total for the first sync. */
    for (int attempt = 1; attempt <= NTP_INITIAL_ATTEMPTS; attempt++) {
        ESP_LOGI(TAG, "waiting for initial NTP sync (attempt %d/%d)...",
                 attempt, NTP_INITIAL_ATTEMPTS);

        int waited = 0;
        while (!s_synced && waited < NTP_INITIAL_TIMEOUT_MS) {
            vTaskDelay(pdMS_TO_TICKS(500));
            waited += 500;
        }

        if (s_synced) {
            s_initial_sync_done = true;
            ESP_LOGI(TAG, "initial NTP sync succeeded on attempt %d", attempt);
            return true;
        }

        ESP_LOGW(TAG, "NTP sync attempt %d timed out after %d ms",
                 attempt, NTP_INITIAL_TIMEOUT_MS);
    }

    s_initial_sync_done = true;
    ESP_LOGE(TAG, "initial NTP sync failed after %d attempts", NTP_INITIAL_ATTEMPTS);
    return false;
}
