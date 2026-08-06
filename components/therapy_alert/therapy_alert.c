/*
 * SomnoTrace - Therapy-stop alert system with ntfy push and buzzer escalation
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

#include "therapy_alert.h"

#include <string.h>
#include <stdio.h>
#include <time.h>

#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_random.h"
#include "nvs_flash.h"
#include "cJSON.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "therapy_alert";

#define NVS_NAMESPACE  "alert"
#define NVS_KEY_CFG    "cfg"

/* ── Injected functions ─────────────────────────────────────────────── */
static alert_beep_fn_t     s_beep_fn  = NULL;
static alert_nvs_exec_fn_t s_nvs_exec = NULL;

void therapy_alert_set_beep_fn(alert_beep_fn_t fn)    { s_beep_fn = fn; }
void therapy_alert_set_nvs_executor(alert_nvs_exec_fn_t fn) { s_nvs_exec = fn; }

/* ── Config ─────────────────────────────────────────────────────────── */
static therapy_alert_config_t s_cfg = ALERT_DEFAULTS;
static bool s_cfg_loaded = false;

/* ── State machine ──────────────────────────────────────────────────── */
static alert_state_t s_state = ALERT_DISARMED;
static SemaphoreHandle_t s_state_mtx = NULL;

/* Timer handles (FreeRTOS task-based timers) */
static TaskHandle_t s_alert_task_h = NULL;
static bool s_task_cancel = false;

/* ── Helpers ────────────────────────────────────────────────────────── */

static bool time_in_window(int minutes_from_midnight, uint16_t start, uint16_t end)
{
    if (start == end) return true;  /* 24-hour window */
    if (start < end) {
        return minutes_from_midnight >= start && minutes_from_midnight < end;
    }
    /* Wraps midnight: e.g. 23:00 → 06:00 */
    return minutes_from_midnight >= start || minutes_from_midnight < end;
}

static int current_minutes_from_midnight(void)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    return tm.tm_hour * 60 + tm.tm_min;
}

const char *therapy_alert_state_str(alert_state_t st)
{
    switch (st) {
        case ALERT_DISARMED:  return "disarmed";
        case ALERT_ARMED:     return "armed";
        case ALERT_PENDING:   return "pending";
        case ALERT_PUSH_SENT: return "push_sent";
        case ALERT_BUZZING:   return "buzzing";
        case ALERT_ACKED:     return "acked";
        default:              return "unknown";
    }
}

alert_state_t therapy_alert_get_state(void)
{
    if (!s_state_mtx) return ALERT_DISARMED;
    xSemaphoreTake(s_state_mtx, portMAX_DELAY);
    alert_state_t st = s_state;
    xSemaphoreGive(s_state_mtx);
    return st;
}

static void set_state(alert_state_t st)
{
    xSemaphoreTake(s_state_mtx, portMAX_DELAY);
    s_state = st;
    xSemaphoreGive(s_state_mtx);
    ESP_LOGI(TAG, "state → %s", therapy_alert_state_str(st));
}

/* ── NVS config persistence ─────────────────────────────────────────── */

static esp_err_t do_save_config(void *arg)
{
    const therapy_alert_config_t *cfg = (const therapy_alert_config_t *)arg;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(h, NVS_KEY_CFG, cfg, sizeof(*cfg));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t therapy_alert_load_config(therapy_alert_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        *cfg = (therapy_alert_config_t)ALERT_DEFAULTS;
        return ESP_ERR_NVS_NOT_FOUND;
    }

    size_t required = sizeof(*cfg);
    esp_err_t err = nvs_get_blob(h, NVS_KEY_CFG, cfg, &required);
    nvs_close(h);

    if (err != ESP_OK) {
        *cfg = (therapy_alert_config_t)ALERT_DEFAULTS;
    } else {
        s_cfg_loaded = true;
    }
    return err;
}

esp_err_t therapy_alert_save_config_json(const char *json_str)
{
    if (!json_str) return ESP_ERR_INVALID_ARG;

    cJSON *root = cJSON_Parse(json_str);
    if (!root) return ESP_ERR_INVALID_ARG;

    therapy_alert_config_t cfg = s_cfg;
    cJSON *j;

    if ((j = cJSON_GetObjectItem(root, "enabled")))    cfg.enabled = cJSON_IsTrue(j);
    if ((j = cJSON_GetObjectItem(root, "win_start")))  cfg.win_start = (uint16_t)j->valuedouble;
    if ((j = cJSON_GetObjectItem(root, "win_end")))    cfg.win_end = (uint16_t)j->valuedouble;
    if ((j = cJSON_GetObjectItem(root, "delay1")))     cfg.delay1 = (uint16_t)j->valuedouble;
    if ((j = cJSON_GetObjectItem(root, "push_en")))    cfg.push_en = cJSON_IsTrue(j);
    if ((j = cJSON_GetObjectItem(root, "ntfy_srv")))   { const char *s = cJSON_GetStringValue(j); if (s) strlcpy(cfg.ntfy_srv, s, sizeof(cfg.ntfy_srv)); }
    if ((j = cJSON_GetObjectItem(root, "ntfy_topic"))) { const char *s = cJSON_GetStringValue(j); if (s) strlcpy(cfg.ntfy_topic, s, sizeof(cfg.ntfy_topic)); }
    if ((j = cJSON_GetObjectItem(root, "ntfy_prio")))  cfg.ntfy_prio = (uint8_t)j->valuedouble;
    if ((j = cJSON_GetObjectItem(root, "delay2")))     cfg.delay2 = (uint16_t)j->valuedouble;
    if ((j = cJSON_GetObjectItem(root, "buzz_en")))    cfg.buzz_en = cJSON_IsTrue(j);

    cJSON_Delete(root);

    /* Persist via injected NVS executor (safe from PSRAM-stack httpd). */
    esp_err_t err = s_nvs_exec ? s_nvs_exec(do_save_config, &cfg)
                               : do_save_config(&cfg);
    if (err == ESP_OK) {
        s_cfg = cfg;
        ESP_LOGI(TAG, "config saved: en=%d win=%d-%d d1=%d push=%d buzz=%d",
                 cfg.enabled, cfg.win_start, cfg.win_end, cfg.delay1,
                 cfg.push_en, cfg.buzz_en);
    }
    return err;
}

esp_err_t therapy_alert_get_config_json(char **out_json)
{
    if (!out_json) return ESP_ERR_INVALID_ARG;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "enabled", s_cfg.enabled);
    cJSON_AddNumberToObject(root, "win_start", s_cfg.win_start);
    cJSON_AddNumberToObject(root, "win_end", s_cfg.win_end);
    cJSON_AddNumberToObject(root, "delay1", s_cfg.delay1);
    cJSON_AddBoolToObject(root, "push_en", s_cfg.push_en);
    cJSON_AddStringToObject(root, "ntfy_srv", s_cfg.ntfy_srv);
    cJSON_AddStringToObject(root, "ntfy_topic", s_cfg.ntfy_topic);
    cJSON_AddNumberToObject(root, "ntfy_prio", s_cfg.ntfy_prio);
    cJSON_AddNumberToObject(root, "delay2", s_cfg.delay2);
    cJSON_AddBoolToObject(root, "buzz_en", s_cfg.buzz_en);

    cJSON *st = cJSON_AddObjectToObject(root, "state");
    cJSON_AddStringToObject(st, "alert", therapy_alert_state_str(therapy_alert_get_state()));

    *out_json = cJSON_Print(root);
    cJSON_Delete(root);
    return ESP_OK;
}

/* ── ntfy push notification ─────────────────────────────────────────── */

static esp_err_t send_ntfy_push(const char *srv, const char *topic,
                                const char *title, const char *body,
                                uint8_t priority)
{
    if (!srv || !topic || !topic[0]) {
        ESP_LOGW(TAG, "ntfy: topic empty, skipping push");
        return ESP_ERR_INVALID_ARG;
    }

    char url[192];
    snprintf(url, sizeof(url), "%s/%s", srv, topic);

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return ESP_FAIL;

    char prio_hdr[8];
    snprintf(prio_hdr, sizeof(prio_hdr), "%d", priority);
    esp_http_client_set_header(client, "Title", title);
    esp_http_client_set_header(client, "Priority", prio_hdr);
    esp_http_client_set_header(client, "Tags", "warning");

    esp_http_client_set_post_field(client, body, strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "ntfy push: HTTP %d", status);
        if (status < 200 || status >= 300) err = ESP_FAIL;
    } else {
        ESP_LOGW(TAG, "ntfy push failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    return err;
}

esp_err_t therapy_alert_send_test_push(const char *json_override)
{
    therapy_alert_config_t cfg = s_cfg;

    if (json_override && json_override[0]) {
        cJSON *root = cJSON_Parse(json_override);
        if (root) {
            cJSON *j;
            if ((j = cJSON_GetObjectItem(root, "push_en")))    cfg.push_en = cJSON_IsTrue(j);
            if ((j = cJSON_GetObjectItem(root, "ntfy_srv")))   { const char *s = cJSON_GetStringValue(j); if (s) strlcpy(cfg.ntfy_srv, s, sizeof(cfg.ntfy_srv)); }
            if ((j = cJSON_GetObjectItem(root, "ntfy_topic"))) { const char *s = cJSON_GetStringValue(j); if (s) strlcpy(cfg.ntfy_topic, s, sizeof(cfg.ntfy_topic)); }
            if ((j = cJSON_GetObjectItem(root, "ntfy_prio")))  cfg.ntfy_prio = (uint8_t)j->valuedouble;
            cJSON_Delete(root);
        }
    }

    if (!cfg.push_en || !cfg.ntfy_topic[0]) {
        ESP_LOGW(TAG, "test push: push disabled or topic empty");
        return ESP_ERR_INVALID_STATE;
    }
    return send_ntfy_push(cfg.ntfy_srv, cfg.ntfy_topic,
                          "SomnoTrace test", "Test notification from SomnoTrace",
                          cfg.ntfy_prio);
}

/* ── Buzzer ─────────────────────────────────────────────────────────── */

static void run_buzzer(void)
{
    if (!s_beep_fn) {
        ESP_LOGW(TAG, "buzzer: no beep function injected");
        return;
    }
    for (int i = 0; i < 5; i++) {
        if (s_task_cancel) break;
        s_beep_fn(880, 1000, 60);
        if (s_task_cancel) break;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* ── Alert routine task ─────────────────────────────────────────────── */

static void alert_routine_task(void *arg)
{
    s_task_cancel = false;

    /* Phase 1: wait delay1 minutes, then send push (if enabled) */
    int delay1_ms = s_cfg.delay1 * 60 * 1000;
    ESP_LOGI(TAG, "alert routine: waiting %d ms before push/buzzer", delay1_ms);

    for (int waited = 0; waited < delay1_ms; waited += 1000) {
        if (s_task_cancel) goto done;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    if (s_task_cancel) goto done;

    /* Check if still in PENDING state (could have been ACKED or disarmed) */
    if (therapy_alert_get_state() != ALERT_PENDING) goto done;

    /* Phase 2: send push notification (if enabled) */
    if (s_cfg.push_en && s_cfg.ntfy_topic[0]) {
        char body[64];
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        snprintf(body, sizeof(body), "Therapy stopped at %02d:%02d",
                 tm.tm_hour, tm.tm_min);

        set_state(ALERT_PUSH_SENT);
        esp_err_t err = send_ntfy_push(s_cfg.ntfy_srv, s_cfg.ntfy_topic,
                                       "SomnoTrace Alert", body,
                                       s_cfg.ntfy_prio);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "push failed, falling through to buzzer");
        }
    }

    if (s_task_cancel) goto done;

    /* Phase 3: wait delay2 minutes before buzzer (only if both push+buzz) */
    if (s_cfg.push_en && s_cfg.buzz_en && s_cfg.delay2 > 0) {
        int delay2_ms = s_cfg.delay2 * 60 * 1000;
        ESP_LOGI(TAG, "alert routine: waiting %d ms before buzzer", delay2_ms);
        for (int waited = 0; waited < delay2_ms; waited += 1000) {
            if (s_task_cancel) goto done;
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    if (s_task_cancel) goto done;

    /* Phase 4: buzzer (if enabled) */
    if (s_cfg.buzz_en) {
        set_state(ALERT_BUZZING);
        run_buzzer();
    }

done:
    ESP_LOGI(TAG, "alert routine task exiting (state=%s)",
             therapy_alert_state_str(therapy_alert_get_state()));
    s_alert_task_h = NULL;
    vTaskDelete(NULL);
}

static void start_alert_routine(void)
{
    if (s_alert_task_h) {
        ESP_LOGW(TAG, "alert routine already running, cancelling old one");
        s_task_cancel = true;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    set_state(ALERT_PENDING);
    xTaskCreatePinnedToCore(alert_routine_task, "alert_routine", 4096,
                            NULL, 5, &s_alert_task_h, 0);
}

static void cancel_alert_routine(void)
{
    s_task_cancel = true;
    if (s_alert_task_h) {
        /* Give it a moment to notice the cancel flag */
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

/* ── Event hooks ────────────────────────────────────────────────────── */

void therapy_alert_on_therapy_start(void)
{
    if (!s_cfg.enabled) return;

    int now_min = current_minutes_from_midnight();
    if (time_in_window(now_min, s_cfg.win_start, s_cfg.win_end)) {
        cancel_alert_routine();
        set_state(ALERT_ARMED);
        ESP_LOGI(TAG, "therapy started inside window (%02d:%02d) — armed",
                 now_min / 60, now_min % 60);
    } else {
        ESP_LOGD(TAG, "therapy started outside window (%02d:%02d) — not armed",
                 now_min / 60, now_min % 60);
    }
}

void therapy_alert_on_therapy_stop(void)
{
    if (!s_cfg.enabled) return;

    alert_state_t st = therapy_alert_get_state();
    if (st == ALERT_ARMED) {
        ESP_LOGI(TAG, "therapy stop while armed — starting alert routine");
        start_alert_routine();
    } else {
        ESP_LOGD(TAG, "therapy stop while %s — ignoring", therapy_alert_state_str(st));
    }
}

void therapy_alert_on_ble_disconnect(void)
{
    if (!s_cfg.enabled) return;

    alert_state_t st = therapy_alert_get_state();
    if (st != ALERT_DISARMED) {
        ESP_LOGI(TAG, "BLE disconnect — disarming (was %s)", therapy_alert_state_str(st));
        cancel_alert_routine();
        set_state(ALERT_DISARMED);
    }
}

void therapy_alert_acknowledge(void)
{
    alert_state_t st = therapy_alert_get_state();
    if (st == ALERT_DISARMED || st == ALERT_ACKED) return;

    ESP_LOGI(TAG, "acknowledged (was %s)", therapy_alert_state_str(st));
    cancel_alert_routine();
    set_state(ALERT_ACKED);
}

/* ── Init ───────────────────────────────────────────────────────────── */

esp_err_t therapy_alert_init(void)
{
    /* Load config from NVS */
    esp_err_t err = therapy_alert_load_config(&s_cfg);
    if (err == ESP_OK) {
        s_cfg_loaded = true;
        ESP_LOGI(TAG, "config loaded: en=%d win=%d-%d d1=%d push=%d buzz=%d",
                 s_cfg.enabled, s_cfg.win_start, s_cfg.win_end,
                 s_cfg.delay1, s_cfg.push_en, s_cfg.buzz_en);
    } else {
        ESP_LOGI(TAG, "no saved config, using defaults");
    }

    s_state_mtx = xSemaphoreCreateMutex();
    if (!s_state_mtx) return ESP_ERR_NO_MEM;

    s_state = ALERT_DISARMED;
    return ESP_OK;
}
