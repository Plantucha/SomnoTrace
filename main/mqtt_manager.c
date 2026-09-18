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

#include "mqtt_manager.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <ctype.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "mqtt_client.h"

#include "nvs.h"
#include "nvs_flash.h"
#include "nvs_writer.h"
#include "cJSON.h"

#include "net_provision.h"
#include "bsp_power.h"
#include "bsp_display.h"
#include "therapy_alert.h"
#include "as11_ble.h"
#include "oximeter.h"
#include "session_writer.h"
#include "sd_storage.h"
#include "uploader.h"
#include "upload_sched.h"
#include "psram_task.h"

static const char *TAG = "mqtt";

#define NVS_NAMESPACE_MQTT   "mqtt"
#define NVS_KEY_ENABLED      "mqtt_en"
#define NVS_KEY_URI          "mqtt_uri"
#define NVS_KEY_USER         "mqtt_user"
#define NVS_KEY_PASS         "mqtt_pass"
#define NVS_KEY_PREFIX       "mqtt_prefix"
#define NVS_KEY_DISCOVERY    "mqtt_disc"

/* Fixed 60s telemetry timer (non-configurable for 2.4 GHz radio coexistence). */
#define TELEMETRY_INTERVAL_MS (60 * 1000)

/* Internal state */
static SemaphoreHandle_t        s_mtx = NULL;
static mqtt_config_t            s_cfg;
static mqtt_conn_state_t        s_conn_state = MQTT_STATE_DISABLED;
static esp_mqtt_client_handle_t s_client = NULL;
static TaskHandle_t             s_telemetry_task = NULL;
static char                     s_lwt_topic[64] = {0};
static char                     s_state_topic[64] = {0};
static char                     s_cmd_therapy_topic[64] = {0};
static char                     s_cmd_alert_topic[64] = {0};
static char                     s_cmd_uploader_topic[64] = {0};
static char                     s_device_id[32] = {0};
static bool                     s_net_up = false;

static bool is_network_up(void)
{
    netprov_link_t link;
    netprov_get_link(&link);
    return link.up || s_net_up;
}

/* Forward declarations */
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data);
static void publish_discovery_payloads(void);
static void do_publish_state(void);

/* ── Device Identifier & Topics ───────────────────────────────────────── */

static void init_device_id(void)
{
    if (s_device_id[0] != '\0') return;
    uint8_t mac[6] = {0};
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) != ESP_OK) {
        /* Fallback if Wi-Fi MAC read fails */
        snprintf(s_device_id, sizeof(s_device_id), "somnotrace_default");
        return;
    }
    snprintf(s_device_id, sizeof(s_device_id), "somnotrace_%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void update_topic_strings(const char *prefix)
{
    const char *p = (prefix && prefix[0] != '\0') ? prefix : MQTT_DEFAULT_PREFIX;
    snprintf(s_lwt_topic, sizeof(s_lwt_topic), "%s/status", p);
    snprintf(s_state_topic, sizeof(s_state_topic), "%s/state", p);
    snprintf(s_cmd_therapy_topic, sizeof(s_cmd_therapy_topic), "%s/cmd/therapy", p);
    snprintf(s_cmd_alert_topic, sizeof(s_cmd_alert_topic), "%s/cmd/alert", p);
    snprintf(s_cmd_uploader_topic, sizeof(s_cmd_uploader_topic), "%s/cmd/uploader", p);
}

/* ── NVS Load & Save ─────────────────────────────────────────────────── */

static esp_err_t do_load_config_nvs(void *arg)
{
    mqtt_config_t *cfg = (mqtt_config_t *)arg;
    mqtt_config_t def = MQTT_CONFIG_DEFAULTS;
    *cfg = def;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE_MQTT, NVS_READONLY, &h);
    if (err != ESP_OK) return err;

    uint8_t en = 0;
    if (nvs_get_u8(h, NVS_KEY_ENABLED, &en) == ESP_OK) cfg->enabled = (en != 0);

    size_t len = sizeof(cfg->broker_uri);
    nvs_get_str(h, NVS_KEY_URI, cfg->broker_uri, &len);

    len = sizeof(cfg->username);
    nvs_get_str(h, NVS_KEY_USER, cfg->username, &len);

    len = sizeof(cfg->password);
    nvs_get_str(h, NVS_KEY_PASS, cfg->password, &len);

    len = sizeof(cfg->topic_prefix);
    if (nvs_get_str(h, NVS_KEY_PREFIX, cfg->topic_prefix, &len) != ESP_OK || cfg->topic_prefix[0] == '\0') {
        strlcpy(cfg->topic_prefix, MQTT_DEFAULT_PREFIX, sizeof(cfg->topic_prefix));
    }

    uint8_t disc = 1;
    if (nvs_get_u8(h, NVS_KEY_DISCOVERY, &disc) == ESP_OK) cfg->discovery_en = (disc != 0);

    nvs_close(h);
    return ESP_OK;
}

static esp_err_t do_save_config_nvs(void *arg)
{
    const mqtt_config_t *cfg = (const mqtt_config_t *)arg;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE_MQTT, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    nvs_set_u8(h, NVS_KEY_ENABLED, cfg->enabled ? 1 : 0);
    nvs_set_str(h, NVS_KEY_URI, cfg->broker_uri);
    nvs_set_str(h, NVS_KEY_USER, cfg->username);
    nvs_set_str(h, NVS_KEY_PASS, cfg->password);
    nvs_set_str(h, NVS_KEY_PREFIX, cfg->topic_prefix);
    nvs_set_u8(h, NVS_KEY_DISCOVERY, cfg->discovery_en ? 1 : 0);

    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t mqtt_manager_load_config(mqtt_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    return do_load_config_nvs(cfg);
}

esp_err_t mqtt_manager_save_config(const mqtt_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    return nvs_writer_run(do_save_config_nvs, (void *)cfg);
}

/* ── Connection & Client Lifecycle ────────────────────────────────────── */

bool mqtt_manager_is_enabled(void)
{
    bool en = false;
    if (s_mtx) xSemaphoreTake(s_mtx, portMAX_DELAY);
    en = s_cfg.enabled;
    if (s_mtx) xSemaphoreGive(s_mtx);
    return en;
}

mqtt_conn_state_t mqtt_manager_get_state(void)
{
    mqtt_conn_state_t st;
    if (s_mtx) xSemaphoreTake(s_mtx, portMAX_DELAY);
    st = s_conn_state;
    if (s_mtx) xSemaphoreGive(s_mtx);
    return st;
}

const char *mqtt_manager_state_str(mqtt_conn_state_t st)
{
    switch (st) {
        case MQTT_STATE_DISABLED:     return "disabled";
        case MQTT_STATE_DISCONNECTED: return "disconnected";
        case MQTT_STATE_CONNECTING:   return "connecting";
        case MQTT_STATE_CONNECTED:    return "connected";
        default:                      return "unknown";
    }
}

esp_err_t mqtt_manager_start(void)
{
    if (!s_mtx) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_mtx, portMAX_DELAY);

    if (!s_cfg.enabled || s_cfg.broker_uri[0] == '\0') {
        s_conn_state = MQTT_STATE_DISABLED;
        xSemaphoreGive(s_mtx);
        return ESP_OK;
    }

    if (!is_network_up()) {
        ESP_LOGI(TAG, "network not up yet, deferring MQTT connection until Wi-Fi connects");
        s_conn_state = MQTT_STATE_DISCONNECTED;
        xSemaphoreGive(s_mtx);
        return ESP_OK;
    }

    esp_mqtt_client_handle_t old_client = s_client;
    s_client = NULL;
    s_conn_state = MQTT_STATE_CONNECTING;
    xSemaphoreGive(s_mtx);

    if (old_client) {
        ESP_LOGI(TAG, "stopping existing MQTT client before restart");
        esp_mqtt_client_stop(old_client);
        esp_mqtt_client_destroy(old_client);
    }

    init_device_id();
    update_topic_strings(s_cfg.topic_prefix);

    esp_mqtt_client_config_t mqtt_cfg = {0};
    mqtt_cfg.broker.address.uri = s_cfg.broker_uri;
    if (s_cfg.username[0] != '\0') {
        mqtt_cfg.credentials.username = s_cfg.username;
    }
    if (s_cfg.password[0] != '\0') {
        mqtt_cfg.credentials.authentication.password = s_cfg.password;
    }
    mqtt_cfg.credentials.client_id = s_device_id;
    mqtt_cfg.session.last_will.topic = s_lwt_topic;
    mqtt_cfg.session.last_will.msg = "offline";
    mqtt_cfg.session.last_will.msg_len = 7;
    mqtt_cfg.session.last_will.qos = 1;
    mqtt_cfg.session.last_will.retain = 1;
    mqtt_cfg.session.keepalive = 60;
    mqtt_cfg.task.stack_size = 8192;
    mqtt_cfg.task.priority = 5;
    mqtt_cfg.buffer.size = 2048;
    mqtt_cfg.buffer.out_size = 2048;

    if (strncmp(s_cfg.broker_uri, "mqtts://", 8) == 0 || strncmp(s_cfg.broker_uri, "ssl://", 6) == 0) {
        mqtt_cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
    }

    esp_mqtt_client_handle_t new_client = esp_mqtt_client_init(&mqtt_cfg);
    if (!new_client) {
        ESP_LOGE(TAG, "failed to initialize MQTT client handle");
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        s_conn_state = MQTT_STATE_DISCONNECTED;
        xSemaphoreGive(s_mtx);
        return ESP_FAIL;
    }

    esp_mqtt_client_register_event(new_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);

    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_client = new_client;
    xSemaphoreGive(s_mtx);

    esp_err_t ret = esp_mqtt_client_start(new_client);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_mqtt_client_start failed: %s", esp_err_to_name(ret));
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        s_conn_state = MQTT_STATE_DISCONNECTED;
        xSemaphoreGive(s_mtx);
    } else {
        ESP_LOGI(TAG, "MQTT client connecting to %s (client_id=%s)", s_cfg.broker_uri, s_device_id);
    }

    return ret;
}

esp_err_t mqtt_manager_stop(void)
{
    if (!s_mtx) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    esp_mqtt_client_handle_t old_client = s_client;
    s_client = NULL;
    s_conn_state = s_cfg.enabled ? MQTT_STATE_DISCONNECTED : MQTT_STATE_DISABLED;
    xSemaphoreGive(s_mtx);

    if (old_client) {
        esp_mqtt_client_stop(old_client);
        esp_mqtt_client_destroy(old_client);
    }

    return ESP_OK;
}

/* ── Event Handlers ───────────────────────────────────────────────────── */

static void handle_command_message(const char *topic, int topic_len, const char *data, int data_len)
{
    char tbuf[64] = {0};
    if (topic_len >= (int)sizeof(tbuf)) return;
    memcpy(tbuf, topic, topic_len);

    char payload[32] = {0};
    int plen = data_len < (int)(sizeof(payload) - 1) ? data_len : (int)(sizeof(payload) - 1);
    memcpy(payload, data, plen);

    /* Uppercase payload for case-insensitive comparison */
    for (int i = 0; i < plen; i++) payload[i] = (char)toupper((unsigned char)payload[i]);

    ESP_LOGI(TAG, "incoming command on '%s': '%s'", tbuf, payload);

    if (strcmp(tbuf, s_cmd_therapy_topic) == 0) {
        if (strstr(payload, "START") || strstr(payload, "ON") || strcmp(payload, "1") == 0) {
            ESP_LOGI(TAG, "cmd: Start Therapy requested");
            as11_ble_start_therapy();
        } else if (strstr(payload, "STOP") || strstr(payload, "OFF") || strcmp(payload, "0") == 0) {
            ESP_LOGI(TAG, "cmd: Stop Therapy requested");
            as11_ble_stop_therapy();
        }
        mqtt_manager_publish_state(true);
    } else if (strcmp(tbuf, s_cmd_alert_topic) == 0) {
        if (strstr(payload, "ACK") || strstr(payload, "SILENCE") || strstr(payload, "PRESS")) {
            ESP_LOGI(TAG, "cmd: Silence/Acknowledge Alert requested");
            therapy_alert_acknowledge();
            mqtt_manager_publish_state(true);
        }
    } else if (strcmp(tbuf, s_cmd_uploader_topic) == 0) {
        if (strstr(payload, "SCAN") || strstr(payload, "RETRY") || strstr(payload, "PRESS")) {
            ESP_LOGI(TAG, "cmd: Scan/Retry Uploads requested");
            upload_sched_request_scan();
            mqtt_manager_publish_state(true);
        }
    }
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch (event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT connected to broker");
            if (s_mtx) xSemaphoreTake(s_mtx, portMAX_DELAY);
            s_conn_state = MQTT_STATE_CONNECTED;
            if (s_mtx) xSemaphoreGive(s_mtx);

            /* Publish online availability (retained) */
            esp_mqtt_client_publish(s_client, s_lwt_topic, "online", 6, 1, 1);

            /* Publish discovery payloads if configured */
            if (s_cfg.discovery_en) {
                publish_discovery_payloads();
            }

            /* Subscribe to bidirectional command topics */
            esp_mqtt_client_subscribe(s_client, s_cmd_therapy_topic, 1);
            esp_mqtt_client_subscribe(s_client, s_cmd_alert_topic, 1);
            esp_mqtt_client_subscribe(s_client, s_cmd_uploader_topic, 1);

            /* Trigger initial state publish in telemetry worker task */
            if (s_telemetry_task) {
                xTaskNotifyGive(s_telemetry_task);
            }
            break;

        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "MQTT disconnected from broker");
            if (s_mtx) xSemaphoreTake(s_mtx, portMAX_DELAY);
            if (s_conn_state != MQTT_STATE_DISABLED) {
                s_conn_state = MQTT_STATE_DISCONNECTED;
            }
            if (s_mtx) xSemaphoreGive(s_mtx);
            break;

        case MQTT_EVENT_DATA:
            handle_command_message(event->topic, event->topic_len, event->data, event->data_len);
            break;

        case MQTT_EVENT_ERROR:
            ESP_LOGW(TAG, "MQTT client error event");
            if (event->error_handle) {
                if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
                    ESP_LOGE(TAG, "Transport error: esp_tls_last_esp_err=0x%x, tls_stack_err=0x%x, sock_errno=%d",
                             event->error_handle->esp_tls_last_esp_err,
                             event->error_handle->esp_tls_stack_err,
                             event->error_handle->esp_transport_sock_errno);
                } else if (event->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
                    ESP_LOGE(TAG, "Connection refused: return_code=0x%x",
                             event->error_handle->connect_return_code);
                }
            }
            break;

        default:
            break;
    }
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_net_up = true;
        ESP_LOGI(TAG, "network up, checking MQTT startup");
        if (mqtt_manager_is_enabled() && s_conn_state != MQTT_STATE_CONNECTED && s_conn_state != MQTT_STATE_CONNECTING) {
            mqtt_manager_start();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_net_up = false;
        ESP_LOGI(TAG, "network down, stopping MQTT client");
        if (s_conn_state == MQTT_STATE_CONNECTED || s_conn_state == MQTT_STATE_CONNECTING) {
            mqtt_manager_stop();
        }
    }
}

/* ── State Publishing ─────────────────────────────────────────────────── */

static void do_publish_state(void)
{
    if (!s_client || s_conn_state != MQTT_STATE_CONNECTED) return;

    cJSON *root = cJSON_CreateObject();
    if (!root) return;

    /* 1. in_therapy */
    bool in_therapy = bsp_display_is_therapy_active();
    cJSON_AddBoolToObject(root, "in_therapy", in_therapy);

    /* 2. alert_state */
    cJSON_AddStringToObject(root, "alert_state", therapy_alert_state_str(therapy_alert_get_state()));

    /* 3. as11_connected */
    bool as11_conn = (strcmp(as11_ble_get_status(), AS11_STATUS_PAIRED) == 0);
    cJSON_AddBoolToObject(root, "as11_connected", as11_conn);

    /* 4. oximeter_connected */
    const char *ox_st = oximeter_get_status();
    bool ox_conn = (strcmp(ox_st, OX_STATUS_PAIRED) == 0 ||
                    strcmp(ox_st, OX_STATUS_MONITORING) == 0 ||
                    strcmp(ox_st, OX_STATUS_PULLING) == 0);
    cJSON_AddBoolToObject(root, "oximeter_connected", ox_conn);

    /* 5. battery */
    bsp_battery_t batt;
    bsp_power_battery_get(&batt);
    if (batt.valid) {
        cJSON_AddNumberToObject(root, "battery_percent", batt.percent);
    } else {
        cJSON_AddNullToObject(root, "battery_percent");
    }
    cJSON_AddBoolToObject(root, "battery_charging", batt.charging);

    /* 6. wifi_rssi */
    netprov_link_t link;
    netprov_get_link(&link);
    if (link.rssi_valid) {
        cJSON_AddNumberToObject(root, "wifi_rssi", link.rssi);
    } else {
        cJSON_AddNullToObject(root, "wifi_rssi");
    }

    /* 7. therapy_duration_min */
    cJSON_AddNumberToObject(root, "therapy_duration_min", session_writer_get_duration_min());

    /* 8. uptime_s */
    uint32_t uptime_s = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS / 1000);
    cJSON_AddNumberToObject(root, "uptime_s", uptime_s);

    /* 9. upload status & pending */
    int pending = 0;
    const char *worst = "idle";
    uploader_get_summary(&pending, &worst);
    cJSON_AddStringToObject(root, "upload_status", worst);
    cJSON_AddNumberToObject(root, "upload_pending", pending);

    /* 10. SD free MB */
    uint64_t free_bytes = 0, total_bytes = 0;
    if (sd_storage_get_free(&free_bytes, &total_bytes) == ESP_OK) {
        cJSON_AddNumberToObject(root, "sd_free_mb", (double)(free_bytes / (1024 * 1024)));
    } else {
        cJSON_AddNullToObject(root, "sd_free_mb");
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return;

    /* Publish state with retain=1 so HA immediately reads state on startup */
    if (s_mtx) xSemaphoreTake(s_mtx, portMAX_DELAY);
    if (s_client && s_conn_state == MQTT_STATE_CONNECTED) {
        esp_mqtt_client_publish(s_client, s_state_topic, json, 0, 1, 1);
    }
    if (s_mtx) xSemaphoreGive(s_mtx);
    cJSON_free(json);
}

static void mqtt_telemetry_task(void *arg)
{
    (void)arg;
    while (1) {
        /* Wait for 60s telemetry tick or an immediate event notification */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(TELEMETRY_INTERVAL_MS));

        if (mqtt_manager_get_state() == MQTT_STATE_CONNECTED) {
            do_publish_state();
        }
    }
}

void mqtt_manager_publish_state(bool force)
{
    if (s_conn_state != MQTT_STATE_CONNECTED) return;
    if (force && s_telemetry_task) {
        xTaskNotifyGive(s_telemetry_task);
    }
}

void mqtt_manager_on_therapy_start(void)
{
    mqtt_manager_publish_state(true);
}

void mqtt_manager_on_therapy_stop(void)
{
    mqtt_manager_publish_state(true);
}

void mqtt_manager_on_alert_state_changed(alert_state_t new_state)
{
    (void)new_state;
    mqtt_manager_publish_state(true);
}

/* ── Home Assistant Discovery Payloads ────────────────────────────────── */

static void add_device_info(cJSON *root)
{
    cJSON *dev = cJSON_AddObjectToObject(root, "device");
    cJSON *ids = cJSON_AddArrayToObject(dev, "identifiers");
    cJSON_AddItemToArray(ids, cJSON_CreateString(s_device_id));
    cJSON_AddStringToObject(dev, "name", netprov_mdns_name_cached());
    cJSON_AddStringToObject(dev, "model", "ESP32-S3-Touch-LCD-1.54");
    cJSON_AddStringToObject(dev, "manufacturer", "SomnoTrace");

    const esp_app_desc_t *app_desc = esp_app_get_description();
    if (app_desc) {
        cJSON_AddStringToObject(dev, "sw_version", app_desc->version);
    }
}

static void publish_entity_discovery(const char *component, const char *object_id, cJSON *cfg)
{
    if (!cfg) return;
    if (!s_client) {
        cJSON_Delete(cfg);
        return;
    }

    /* Common availability & device */
    cJSON_AddStringToObject(cfg, "availability_topic", s_lwt_topic);
    cJSON_AddStringToObject(cfg, "payload_available", "online");
    cJSON_AddStringToObject(cfg, "payload_not_available", "offline");
    add_device_info(cfg);

    char disc_topic[128];
    snprintf(disc_topic, sizeof(disc_topic), "homeassistant/%s/%s/%s/config",
             component, s_device_id, object_id);

    char *payload = cJSON_PrintUnformatted(cfg);
    cJSON_Delete(cfg);
    if (!payload) return;

    if (s_mtx) xSemaphoreTake(s_mtx, portMAX_DELAY);
    if (s_client) {
        esp_mqtt_client_publish(s_client, disc_topic, payload, 0, 1, 1);
    }
    if (s_mtx) xSemaphoreGive(s_mtx);
    cJSON_free(payload);
}

static void publish_discovery_payloads(void)
{
    char uniq[64];

    /* 1. binary_sensor / in_therapy */
    {
        cJSON *c = cJSON_CreateObject();
        snprintf(uniq, sizeof(uniq), "%s_in_therapy", s_device_id);
        cJSON_AddStringToObject(c, "name", "In Therapy");
        cJSON_AddStringToObject(c, "unique_id", uniq);
        cJSON_AddStringToObject(c, "state_topic", s_state_topic);
        cJSON_AddStringToObject(c, "value_template", "{{ value_json.in_therapy }}");
        cJSON_AddStringToObject(c, "payload_on", "true");
        cJSON_AddStringToObject(c, "payload_off", "false");
        cJSON_AddStringToObject(c, "icon", "mdi:sleep");
        publish_entity_discovery("binary_sensor", "in_therapy", c);
    }

    /* 2. sensor / alert_state */
    {
        cJSON *c = cJSON_CreateObject();
        snprintf(uniq, sizeof(uniq), "%s_alert_state", s_device_id);
        cJSON_AddStringToObject(c, "name", "Alert State");
        cJSON_AddStringToObject(c, "unique_id", uniq);
        cJSON_AddStringToObject(c, "state_topic", s_state_topic);
        cJSON_AddStringToObject(c, "value_template", "{{ value_json.alert_state }}");
        cJSON_AddStringToObject(c, "icon", "mdi:bell-alert");
        publish_entity_discovery("sensor", "alert_state", c);
    }

    /* 3. binary_sensor / as11_connected */
    {
        cJSON *c = cJSON_CreateObject();
        snprintf(uniq, sizeof(uniq), "%s_as11_connected", s_device_id);
        cJSON_AddStringToObject(c, "name", "CPAP Connected");
        cJSON_AddStringToObject(c, "unique_id", uniq);
        cJSON_AddStringToObject(c, "state_topic", s_state_topic);
        cJSON_AddStringToObject(c, "value_template", "{{ value_json.as11_connected }}");
        cJSON_AddStringToObject(c, "payload_on", "true");
        cJSON_AddStringToObject(c, "payload_off", "false");
        cJSON_AddStringToObject(c, "device_class", "connectivity");
        publish_entity_discovery("binary_sensor", "as11_connected", c);
    }

    /* 4. binary_sensor / oximeter_connected */
    {
        cJSON *c = cJSON_CreateObject();
        snprintf(uniq, sizeof(uniq), "%s_oximeter_connected", s_device_id);
        cJSON_AddStringToObject(c, "name", "Oximeter Connected");
        cJSON_AddStringToObject(c, "unique_id", uniq);
        cJSON_AddStringToObject(c, "state_topic", s_state_topic);
        cJSON_AddStringToObject(c, "value_template", "{{ value_json.oximeter_connected }}");
        cJSON_AddStringToObject(c, "payload_on", "true");
        cJSON_AddStringToObject(c, "payload_off", "false");
        cJSON_AddStringToObject(c, "device_class", "connectivity");
        publish_entity_discovery("binary_sensor", "oximeter_connected", c);
    }

    /* 5. sensor / battery_percent */
    {
        cJSON *c = cJSON_CreateObject();
        snprintf(uniq, sizeof(uniq), "%s_battery_percent", s_device_id);
        cJSON_AddStringToObject(c, "name", "Battery");
        cJSON_AddStringToObject(c, "unique_id", uniq);
        cJSON_AddStringToObject(c, "state_topic", s_state_topic);
        cJSON_AddStringToObject(c, "value_template", "{{ value_json.battery_percent }}");
        cJSON_AddStringToObject(c, "unit_of_measurement", "%");
        cJSON_AddStringToObject(c, "device_class", "battery");
        cJSON_AddStringToObject(c, "state_class", "measurement");
        publish_entity_discovery("sensor", "battery_percent", c);
    }

    /* 6. binary_sensor / battery_charging */
    {
        cJSON *c = cJSON_CreateObject();
        snprintf(uniq, sizeof(uniq), "%s_battery_charging", s_device_id);
        cJSON_AddStringToObject(c, "name", "Battery Charging");
        cJSON_AddStringToObject(c, "unique_id", uniq);
        cJSON_AddStringToObject(c, "state_topic", s_state_topic);
        cJSON_AddStringToObject(c, "value_template", "{{ value_json.battery_charging }}");
        cJSON_AddStringToObject(c, "payload_on", "true");
        cJSON_AddStringToObject(c, "payload_off", "false");
        cJSON_AddStringToObject(c, "device_class", "battery_charging");
        publish_entity_discovery("binary_sensor", "battery_charging", c);
    }

    /* 7. sensor / wifi_rssi */
    {
        cJSON *c = cJSON_CreateObject();
        snprintf(uniq, sizeof(uniq), "%s_wifi_rssi", s_device_id);
        cJSON_AddStringToObject(c, "name", "Wi-Fi RSSI");
        cJSON_AddStringToObject(c, "unique_id", uniq);
        cJSON_AddStringToObject(c, "state_topic", s_state_topic);
        cJSON_AddStringToObject(c, "value_template", "{{ value_json.wifi_rssi }}");
        cJSON_AddStringToObject(c, "unit_of_measurement", "dBm");
        cJSON_AddStringToObject(c, "device_class", "signal_strength");
        cJSON_AddStringToObject(c, "state_class", "measurement");
        cJSON_AddStringToObject(c, "entity_category", "diagnostic");
        publish_entity_discovery("sensor", "wifi_rssi", c);
    }

    /* 8. sensor / therapy_duration_min */
    {
        cJSON *c = cJSON_CreateObject();
        snprintf(uniq, sizeof(uniq), "%s_therapy_duration_min", s_device_id);
        cJSON_AddStringToObject(c, "name", "Therapy Duration");
        cJSON_AddStringToObject(c, "unique_id", uniq);
        cJSON_AddStringToObject(c, "state_topic", s_state_topic);
        cJSON_AddStringToObject(c, "value_template", "{{ value_json.therapy_duration_min }}");
        cJSON_AddStringToObject(c, "unit_of_measurement", "min");
        cJSON_AddStringToObject(c, "device_class", "duration");
        cJSON_AddStringToObject(c, "state_class", "measurement");
        cJSON_AddStringToObject(c, "icon", "mdi:timer-outline");
        publish_entity_discovery("sensor", "therapy_duration_min", c);
    }

    /* 9. sensor / uptime_s */
    {
        cJSON *c = cJSON_CreateObject();
        snprintf(uniq, sizeof(uniq), "%s_uptime_s", s_device_id);
        cJSON_AddStringToObject(c, "name", "Uptime");
        cJSON_AddStringToObject(c, "unique_id", uniq);
        cJSON_AddStringToObject(c, "state_topic", s_state_topic);
        cJSON_AddStringToObject(c, "value_template", "{{ value_json.uptime_s }}");
        cJSON_AddStringToObject(c, "unit_of_measurement", "s");
        cJSON_AddStringToObject(c, "device_class", "duration");
        cJSON_AddStringToObject(c, "state_class", "total_increasing");
        cJSON_AddStringToObject(c, "entity_category", "diagnostic");
        publish_entity_discovery("sensor", "uptime_s", c);
    }

    /* 10. sensor / upload_status */
    {
        cJSON *c = cJSON_CreateObject();
        snprintf(uniq, sizeof(uniq), "%s_upload_status", s_device_id);
        cJSON_AddStringToObject(c, "name", "Upload Status");
        cJSON_AddStringToObject(c, "unique_id", uniq);
        cJSON_AddStringToObject(c, "state_topic", s_state_topic);
        cJSON_AddStringToObject(c, "value_template", "{{ value_json.upload_status }}");
        cJSON_AddStringToObject(c, "icon", "mdi:cloud-upload");
        cJSON_AddStringToObject(c, "entity_category", "diagnostic");
        publish_entity_discovery("sensor", "upload_status", c);
    }

    /* 11. sensor / upload_pending */
    {
        cJSON *c = cJSON_CreateObject();
        snprintf(uniq, sizeof(uniq), "%s_upload_pending", s_device_id);
        cJSON_AddStringToObject(c, "name", "Upload Pending");
        cJSON_AddStringToObject(c, "unique_id", uniq);
        cJSON_AddStringToObject(c, "state_topic", s_state_topic);
        cJSON_AddStringToObject(c, "value_template", "{{ value_json.upload_pending }}");
        cJSON_AddStringToObject(c, "unit_of_measurement", "files");
        cJSON_AddStringToObject(c, "icon", "mdi:cloud-sync");
        cJSON_AddStringToObject(c, "entity_category", "diagnostic");
        publish_entity_discovery("sensor", "upload_pending", c);
    }

    /* 12. sensor / sd_free_mb */
    {
        cJSON *c = cJSON_CreateObject();
        snprintf(uniq, sizeof(uniq), "%s_sd_free_mb", s_device_id);
        cJSON_AddStringToObject(c, "name", "SD Card Free Space");
        cJSON_AddStringToObject(c, "unique_id", uniq);
        cJSON_AddStringToObject(c, "state_topic", s_state_topic);
        cJSON_AddStringToObject(c, "value_template", "{{ value_json.sd_free_mb }}");
        cJSON_AddStringToObject(c, "unit_of_measurement", "MB");
        cJSON_AddStringToObject(c, "device_class", "data_size");
        cJSON_AddStringToObject(c, "entity_category", "diagnostic");
        publish_entity_discovery("sensor", "sd_free_mb", c);
    }

    /* 13. switch / therapy */
    {
        cJSON *c = cJSON_CreateObject();
        snprintf(uniq, sizeof(uniq), "%s_therapy_switch", s_device_id);
        cJSON_AddStringToObject(c, "name", "Therapy Switch");
        cJSON_AddStringToObject(c, "unique_id", uniq);
        cJSON_AddStringToObject(c, "state_topic", s_state_topic);
        cJSON_AddStringToObject(c, "value_template", "{{ value_json.in_therapy }}");
        cJSON_AddStringToObject(c, "command_topic", s_cmd_therapy_topic);
        cJSON_AddStringToObject(c, "payload_on", "START");
        cJSON_AddStringToObject(c, "payload_off", "STOP");
        cJSON_AddStringToObject(c, "state_on", "true");
        cJSON_AddStringToObject(c, "state_off", "false");
        cJSON_AddStringToObject(c, "icon", "mdi:power");
        publish_entity_discovery("switch", "therapy", c);
    }

    /* 14. button / alert_ack */
    {
        cJSON *c = cJSON_CreateObject();
        snprintf(uniq, sizeof(uniq), "%s_alert_ack", s_device_id);
        cJSON_AddStringToObject(c, "name", "Silence Alert");
        cJSON_AddStringToObject(c, "unique_id", uniq);
        cJSON_AddStringToObject(c, "command_topic", s_cmd_alert_topic);
        cJSON_AddStringToObject(c, "payload_press", "ACK");
        cJSON_AddStringToObject(c, "icon", "mdi:bell-off");
        publish_entity_discovery("button", "alert_ack", c);
    }

    /* 15. button / upload_scan */
    {
        cJSON *c = cJSON_CreateObject();
        snprintf(uniq, sizeof(uniq), "%s_upload_scan", s_device_id);
        cJSON_AddStringToObject(c, "name", "Scan Uploads");
        cJSON_AddStringToObject(c, "unique_id", uniq);
        cJSON_AddStringToObject(c, "command_topic", s_cmd_uploader_topic);
        cJSON_AddStringToObject(c, "payload_press", "SCAN");
        cJSON_AddStringToObject(c, "icon", "mdi:cloud-refresh");
        cJSON_AddStringToObject(c, "entity_category", "diagnostic");
        publish_entity_discovery("button", "upload_scan", c);
    }
}

/* ── JSON Config API (REST) ──────────────────────────────────────────── */

esp_err_t mqtt_manager_get_config_json(char **out_json)
{
    if (!out_json) return ESP_ERR_INVALID_ARG;

    if (s_mtx) xSemaphoreTake(s_mtx, portMAX_DELAY);
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        if (s_mtx) xSemaphoreGive(s_mtx);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddBoolToObject(root, "enabled", s_cfg.enabled);
    cJSON_AddStringToObject(root, "broker_uri", s_cfg.broker_uri);
    cJSON_AddStringToObject(root, "username", s_cfg.username);
    cJSON_AddBoolToObject(root, "has_password", s_cfg.password[0] != '\0');
    cJSON_AddStringToObject(root, "topic_prefix", s_cfg.topic_prefix);
    cJSON_AddBoolToObject(root, "discovery_en", s_cfg.discovery_en);
    cJSON_AddStringToObject(root, "state", mqtt_manager_state_str(s_conn_state));
    if (s_mtx) xSemaphoreGive(s_mtx);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json_str) return ESP_ERR_NO_MEM;

    *out_json = json_str;
    return ESP_OK;
}

esp_err_t mqtt_manager_save_config_json(const char *json_str)
{
    if (!json_str) return ESP_ERR_INVALID_ARG;

    cJSON *root = cJSON_Parse(json_str);
    if (!root) return ESP_ERR_INVALID_ARG;

    mqtt_config_t new_cfg;
    if (s_mtx) xSemaphoreTake(s_mtx, portMAX_DELAY);
    new_cfg = s_cfg;
    if (s_mtx) xSemaphoreGive(s_mtx);

    cJSON *item = cJSON_GetObjectItem(root, "enabled");
    if (item && cJSON_IsBool(item)) new_cfg.enabled = cJSON_IsTrue(item);

    item = cJSON_GetObjectItem(root, "broker_uri");
    if (item && cJSON_IsString(item)) {
        strlcpy(new_cfg.broker_uri, item->valuestring, sizeof(new_cfg.broker_uri));
    }

    item = cJSON_GetObjectItem(root, "username");
    if (item && cJSON_IsString(item)) {
        strlcpy(new_cfg.username, item->valuestring, sizeof(new_cfg.username));
    }

    item = cJSON_GetObjectItem(root, "password");
    if (item && cJSON_IsString(item)) {
        strlcpy(new_cfg.password, item->valuestring, sizeof(new_cfg.password));
    }

    item = cJSON_GetObjectItem(root, "topic_prefix");
    if (item && cJSON_IsString(item) && item->valuestring[0] != '\0') {
        strlcpy(new_cfg.topic_prefix, item->valuestring, sizeof(new_cfg.topic_prefix));
    }

    item = cJSON_GetObjectItem(root, "discovery_en");
    if (item && cJSON_IsBool(item)) new_cfg.discovery_en = cJSON_IsTrue(item);

    cJSON_Delete(root);

    /* Persist to NVS */
    esp_err_t err = mqtt_manager_save_config(&new_cfg);
    if (err != ESP_OK) return err;

    if (s_mtx) xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_cfg = new_cfg;
    update_topic_strings(s_cfg.topic_prefix);
    if (s_mtx) xSemaphoreGive(s_mtx);

    /* Restart client with new configuration */
    if (s_cfg.enabled && is_network_up()) {
        s_net_up = true;
        mqtt_manager_start();
    } else {
        mqtt_manager_stop();
    }

    return ESP_OK;
}

/* ── Initialization ───────────────────────────────────────────────────── */

esp_err_t mqtt_manager_init(void)
{
    if (!s_mtx) s_mtx = xSemaphoreCreateMutex();
    if (!s_mtx) return ESP_ERR_NO_MEM;

    init_device_id();

    /* Load NVS configuration */
    mqtt_config_t cfg;
    if (mqtt_manager_load_config(&cfg) == ESP_OK) {
        s_cfg = cfg;
    } else {
        s_cfg = (mqtt_config_t)MQTT_CONFIG_DEFAULTS;
    }
    update_topic_strings(s_cfg.topic_prefix);

    /* Create background telemetry worker task in PSRAM */
    if (!s_telemetry_task) {
        s_telemetry_task = psram_task_create(mqtt_telemetry_task,
                                             "mqtt_pub",
                                             6144,
                                             NULL,
                                             3,
                                             tskNO_AFFINITY,
                                             NULL,
                                             NULL);
    }

    /* Register IP & Wi-Fi event handlers to auto-connect when network is ready */
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, NULL);
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, on_wifi_event, NULL);

    /* If Wi-Fi is already up, record network state and start if enabled */
    if (is_network_up()) {
        s_net_up = true;
        if (s_cfg.enabled) {
            mqtt_manager_start();
        }
    }

    ESP_LOGI(TAG, "MQTT manager initialized (enabled=%d, broker=%s, prefix=%s)",
             s_cfg.enabled, s_cfg.broker_uri[0] ? s_cfg.broker_uri : "(none)", s_cfg.topic_prefix);
    return ESP_OK;
}
