/*
 * SomnoTrace - MQTT client and Home Assistant Auto-Discovery manager
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
#include <stdint.h>
#include "esp_err.h"

#include "therapy_alert.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Connection state ─────────────────────────────────────────────────── */

typedef enum {
    MQTT_STATE_DISABLED = 0,
    MQTT_STATE_DISCONNECTED,
    MQTT_STATE_CONNECTING,
    MQTT_STATE_CONNECTED,
} mqtt_conn_state_t;

/* ── Configuration ────────────────────────────────────────────────────── */

#define MQTT_URI_MAX        128
#define MQTT_USER_MAX       64
#define MQTT_PASS_MAX       64
#define MQTT_PREFIX_MAX     32
#define MQTT_DEFAULT_PREFIX "somnotrace"

typedef struct {
    bool enabled;
    char broker_uri[MQTT_URI_MAX];
    char username[MQTT_USER_MAX];
    char password[MQTT_PASS_MAX];
    char topic_prefix[MQTT_PREFIX_MAX];
    bool discovery_en;
} mqtt_config_t;

#define MQTT_CONFIG_DEFAULTS { \
    .enabled = false, \
    .broker_uri = "", \
    .username = "", \
    .password = "", \
    .topic_prefix = MQTT_DEFAULT_PREFIX, \
    .discovery_en = true, \
}

/* ── Lifecycle ────────────────────────────────────────────────────────── */

/* Initialise the MQTT manager subsystem.
 * Loads configuration from NVS and registers Wi-Fi/IP event handlers.
 * Call once at boot after netprov_init(). */
esp_err_t mqtt_manager_init(void);

/* Connect to the MQTT broker if enabled and configured.
 * Automatically triggered by IP_EVENT_STA_GOT_IP. */
esp_err_t mqtt_manager_start(void);

/* Disconnect from the MQTT broker. */
esp_err_t mqtt_manager_stop(void);

/* Check if MQTT integration is enabled in configuration. */
bool mqtt_manager_is_enabled(void);

/* Get current connection state. */
mqtt_conn_state_t mqtt_manager_get_state(void);

/* Human-readable connection state string ("disabled", "disconnected", etc.). */
const char *mqtt_manager_state_str(mqtt_conn_state_t st);

/* ── State publishing ─────────────────────────────────────────────────── */

/* Publish the combined JSON state payload (<prefix>/state).
 * If force is false, rate-limited by the internal timer.
 * If force is true, publishes immediately (event bypass). */
void mqtt_manager_publish_state(bool force);

/* ── Event hooks ──────────────────────────────────────────────────────── */

/* Called when therapy starts (immediate state publish). */
void mqtt_manager_on_therapy_start(void);

/* Called when therapy stops (immediate state publish). */
void mqtt_manager_on_therapy_stop(void);

/* Called when therapy alert state changes (immediate state publish). */
void mqtt_manager_on_alert_state_changed(alert_state_t new_state);

/* ── Configuration API (for Web UI / REST) ────────────────────────────── */

/* Load MQTT config from NVS into cfg. */
esp_err_t mqtt_manager_load_config(mqtt_config_t *cfg);

/* Save MQTT config to NVS (safe to call from PSRAM task via nvs_writer_run). */
esp_err_t mqtt_manager_save_config(const mqtt_config_t *cfg);

/* Get current configuration as a JSON string for REST API.
 * The password is masked ("has_password": bool). Caller frees *out_json. */
esp_err_t mqtt_manager_get_config_json(char **out_json);

/* Parse and persist MQTT configuration from JSON string. */
esp_err_t mqtt_manager_save_config_json(const char *json_str);

#ifdef __cplusplus
}
#endif
