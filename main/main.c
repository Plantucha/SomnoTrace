/*
 * SomnoTrace - application entry point
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 *
 * This file is part of SomnoTrace.
 *
 * SomnoTrace is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * SomnoTrace is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <https://www.gnu.org/licenses/>.
 *
 * ADDITIONAL TERM (GPLv3 Section 7(b)): Redistributions must preserve the
 * attribution "Based on SomnoTrace, originally created by Ilya Kruchinin
 * (https://github.com/ilyakruchinin)." See the NOTICE file for details.
 */

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bsp_power.h"
#include "bsp_display.h"
#include "net_provision.h"
#include "as11_ble.h"
#include "sd_storage.h"
#include "session_writer.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_wifi.h"
#include "ftp.h"
#include "time_sync.h"
#include "uploader.h"
#include "log_stream.h"
#include "device_settings.h"
#include "bsp_audio.h"


static const char *TAG = "somnotrace";
static volatile bool s_softap_requested = false;

static void show_status(const char *title, const char *lines[], int n)
{
    bsp_display_show_lines(title, lines, n);
    for (int i = 0; i < n; i++) {
        ESP_LOGI(TAG, "  %s", lines[i]);
    }
}

static void enter_softap(const struct netprov_config *cfg)
{
    as11_ble_disconnect();
    bsp_display_set_wifi_connected(false);
    char ap_ip[16] = "0.0.0.0";
    esp_err_t err = netprov_start_portal(cfg, ap_ip);
    if (err != ESP_OK) {
        const char *lines[] = { "SoftAP failed" };
        show_status("Error", lines, 1);
        return;
    }

    char ssid_line[48];
    snprintf(ssid_line, sizeof(ssid_line), "SSID: %s-setup", cfg->hostname);
    const char *lines[] = {
        "Wi-Fi Setup Mode",
        ssid_line,
        ap_ip,
        "Connect and configure",
    };
    show_status("Setup", lines, 4);
}

void app_main(void)
{
    const esp_app_desc_t *app_desc = esp_app_get_description();
    ESP_LOGI(TAG, "SomnoTrace %s (IDF %s) starting up",
             app_desc ? app_desc->version : "unknown",
             app_desc ? app_desc->idf_ver : "?");

    /* 1. Power latch — must be first or device powers off on button release. */
    bsp_power_hold();

    /* 1b. Start log capture early so the ring buffer catches boot messages. */
    log_stream_init();

    /* 2. Start button monitors. */
    bsp_power_start_button_monitor(5000);   /* PWR 5 s = power off */
    bsp_power_start_boot_monitor(&s_softap_requested, 5000);
    bsp_power_start_plus_monitor();         /* PLUS double-click = stop therapy */

    /* 3. Initialise display. */
    if (bsp_display_init() != ESP_OK) {
        ESP_LOGE(TAG, "display init failed");
    }

    const char *boot_lines[] = { "Booting..." };
    show_status("SomnoTrace", boot_lines, 1);

    /* Initial battery reading for the status indicator */
    bsp_power_battery_monitor_start();

    /* 4. Initialise networking stack (includes NVS init). */
    ESP_ERROR_CHECK(netprov_init());

    /* 4a. Load device settings (brightness, LCD therapy mode) and apply.
     * Must be after netprov_init() which calls nvs_flash_init(). */
    device_settings_t dev_cfg;
    device_settings_load(&dev_cfg);
    bsp_display_set_brightness(dev_cfg.brightness);

    /* 4b. Initialise BLE (AirSense 11 pairing). Non-fatal on failure. */
    if (as11_ble_init() != ESP_OK) {
        ESP_LOGE(TAG, "BLE init failed; CPAP pairing unavailable");
    }
    ESP_LOGI(TAG, "[heap] after BLE init: internal free=%u min=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));

    /* 4c. Initialise SD card storage and session writer. Non-fatal. */
    esp_err_t sd_ret = sd_storage_init();
    if (sd_ret != ESP_OK) {
        ESP_LOGE(TAG, "SD card init failed; session storage unavailable");
        /* Distinguish "no card" from "card present but mount/format error".
         * ESP_ERR_NOT_FOUND = SDMMC host couldn't probe a card (none inserted).
         * Other errors (e.g. ESP_ERR_INVALID_STATE, FR_NO_FILESYSTEM) mean
         * the card is physically present but unusable. */
        const char *sd_title, *sd_lines[2];
        int sd_nlines;
        if (sd_ret == ESP_ERR_NOT_FOUND) {
            sd_title = "Warning";
            sd_lines[0] = "Insert SD Card";
            sd_lines[1] = "Power off, insert card,";
            sd_nlines = 2;
        } else {
            sd_title = "SD Card Error";
            sd_lines[0] = "Card mount failed";
            sd_lines[1] = "Check or reformat card";
            sd_nlines = 2;
        }
        show_status(sd_title, sd_lines, sd_nlines);
        /* Hold the warning for 3 seconds before continuing boot */
        vTaskDelay(pdMS_TO_TICKS(3000));
    } else {
        session_writer_init();
        session_writer_recover();
    }

    /* 5. Load config from NVS. */
    struct netprov_config cfg;
    bool has_creds = netprov_load_config(&cfg);

    /* 6. If BOOT was held at boot, force SoftAP regardless. */
    if (s_softap_requested) {
        ESP_LOGW(TAG, "BOOT long-press detected: forcing SoftAP");
        enter_softap(&cfg);
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    /* 7. Try to connect to configured Wi-Fi. */
    char ip[16] = "0.0.0.0";
    esp_err_t err = ESP_FAIL;
    if (has_creds) {
        const char *lines[] = { "Connecting to Wi-Fi..." };
        show_status("SomnoTrace", lines, 1);
        err = netprov_try_connect(&cfg, ip, 15000);
    }

    bool in_softap = false;
    uint32_t softap_start_ticks = 0;

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Wi-Fi connected, IP=%s", ip);
        ESP_LOGI(TAG, "[heap] after WiFi: internal free=%u min=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
        bsp_display_set_wifi_connected(true);
        netprov_start_connected_server(ip);
        time_sync_init();

        /* ── Initial NTP sync with failure handling ─── */
        bool ntp_ok = time_sync_wait_initial();
        if (!ntp_ok) {
            ESP_LOGE(TAG, "initial NTP sync failed — alarm + reboot");

            /* Show failure message on screen */
            const char *fail_lines[] = {
                "NTP Sync Failed",
                "Rebooting...",
            };
            show_status("Error", fail_lines, 2);

            /* Sound audible alarm: 5 beeps of 1s on / 1s off, loud */
            if (bsp_audio_init() == ESP_OK) {
                for (int i = 0; i < 5; i++) {
                    bsp_audio_beep(880, 1000, 60);
                    vTaskDelay(pdMS_TO_TICKS(1000));
                }
            } else {
                ESP_LOGE(TAG, "audio init failed — silent reboot");
            }

            /* Hard reboot */
            vTaskDelay(pdMS_TO_TICKS(500));
            esp_restart();
        }

        if (sd_storage_is_ready()) {
            /* Configure FTP server from uploader config (NVS) */
            uploader_config_t upcfg;
            if (uploader_load_config(&upcfg) == ESP_OK && upcfg.ftp_enabled) {
                ftp_anonymous_mode = upcfg.ftp_anonymous;
                if (upcfg.ftp_anonymous) {
                    strlcpy(ftp_user, "anonymous", sizeof(ftp_user));
                    strlcpy(ftp_pass, "anonymous@", sizeof(ftp_pass));
                } else {
                    strlcpy(ftp_user, upcfg.ftp_user, sizeof(ftp_user));
                    strlcpy(ftp_pass, upcfg.ftp_pass, sizeof(ftp_pass));
                }
                ftp_server_start();
                ESP_LOGI(TAG, "FTP server started (%s mode)",
                         upcfg.ftp_anonymous ? "anonymous" : "authenticated");
            } else {
                ESP_LOGI(TAG, "FTP server disabled in config");
            }
        }

        /* Initialise upload system (needs Wi-Fi + NVS + SD card). */
        uploader_init();

        char ip_line[32];
        snprintf(ip_line, sizeof(ip_line), "IP: %s", ip);

        netprov_link_t link;
        netprov_get_link(&link);
        const char *lines[] = {
            link.ssid[0] ? link.ssid : "Wi-Fi Connected",
            ip_line,
        };
        show_status("SomnoTrace", lines, 2);
    } else {
        ESP_LOGW(TAG, "Wi-Fi connect failed, entering SoftAP");
        enter_softap(&cfg);
        in_softap = true;
        softap_start_ticks = xTaskGetTickCount();
    }

    int refresh_counter = 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (s_softap_requested && !in_softap) {
            ESP_LOGW(TAG, "BOOT long-press detected at runtime: entering SoftAP");
            in_softap = true;
            enter_softap(&cfg);
            softap_start_ticks = xTaskGetTickCount();
        }
        if (in_softap) {
            /* SoftAP idle timeout */
            if ((xTaskGetTickCount() - softap_start_ticks) * portTICK_PERIOD_MS
                 > 10 * 60 * 1000) {
                ESP_LOGW(TAG, "SoftAP 10-minute idle timeout: rebooting to retry connection");
                esp_restart();
            }
            /* Update battery indicator in SoftAP mode too */
            if (++refresh_counter >= 3) {
                refresh_counter = 0;
                bsp_battery_t batt;
                bsp_power_battery_get(&batt);
                if (batt.valid) {
                    bsp_display_set_battery(batt.percent, batt.charging);
                }
            }
        } else {
            /* Connected mode: refresh status display every 3 s.
             * Skipped during therapy (graph mode owns the display). */
            if (++refresh_counter >= 3 && !bsp_display_is_therapy_active()) {
                refresh_counter = 0;

                /* Use live link state instead of boot-time assumption. */
                netprov_link_t link;
                netprov_get_link(&link);

                if (!link.up) {
                    const char *lines[] = {
                        "Wi-Fi Disconnected",
                        "Reconnecting...",
                    };
                    bsp_display_show_lines("SomnoTrace", lines, 2);
                } else {
                    char ip_line[32];
                    snprintf(ip_line, sizeof(ip_line), "IP: %s", link.ip);

                    const char *ssid_str = link.ssid[0] ? link.ssid : "Wi-Fi Connected";
                    if (sd_storage_is_ready()) {
                        const char *lines[] = {
                            ssid_str,
                            ip_line,
                        };
                        bsp_display_show_lines("SomnoTrace", lines, 2);
                    } else {
                        const char *lines[] = {
                            ssid_str,
                            ip_line,
                            "SD Card Error",
                        };
                        bsp_display_show_lines("SomnoTrace", lines, 3);
                    }
                }

                /* Update battery indicator */
                bsp_battery_t batt;
                bsp_power_battery_get(&batt);
                if (batt.valid) {
                    bsp_display_set_battery(batt.percent, batt.charging);
                }
            }
        }
    }
}
