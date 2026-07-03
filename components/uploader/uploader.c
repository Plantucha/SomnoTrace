/*
 * SomnoTrace - Upload orchestration task and config management
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

#include "uploader.h"
#include "uploader_state.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"
#include "esp_heap_caps.h"

/* LittleFS */
#include "esp_littlefs.h"

static const char *TAG = "uploader";

/* ── Constants ──────────────────────────────────────────────────────── */

#define UPLOAD_TASK_STACK      12288
#define UPLOAD_TASK_PRIORITY   4    /* low — below BLE notif (10) and EDF (5) */
#define UPLOAD_QUEUE_LEN       16
#define RETRY_INTERVAL_MS      30000   /* check for retries every 30s */
#define MAX_RETRY_ATTEMPTS     5

/* Exponential backoff delays (ms) for retries */
static const int retry_delays_ms[] = {30000, 60000, 120000, 300000, 600000};
#define N_RETRY_DELAYS (int)(sizeof(retry_delays_ms) / sizeof(retry_delays_ms[0]))

#define NVS_NAMESPACE  "uploader"
#define LITTLEFS_LABEL "storage"

/* ── Internal state ─────────────────────────────────────────────────── */

typedef struct {
    char day_folder[16];
} upload_event_t;

static QueueHandle_t s_queue = NULL;
static SemaphoreHandle_t s_state_mutex = NULL;
static upload_state_t *s_state = NULL;
static uploader_config_t s_config;
static bool s_initialised = false;

/* Registered backends */
#define MAX_BACKENDS 4
static const upload_backend_t *s_backends[MAX_BACKENDS];
static int s_n_backends = 0;

/* ── Backend registration ───────────────────────────────────────────── */

void uploader_register_backend(const upload_backend_t *backend)
{
    if (!backend || s_n_backends >= MAX_BACKENDS) return;
    s_backends[s_n_backends++] = backend;
    ESP_LOGI(TAG, "registered backend: %s", backend->name);
}

/* External backend declarations */
extern const upload_backend_t smb_backend;
extern const upload_backend_t sleephq_backend;

/* ── LittleFS init ──────────────────────────────────────────────────── */

static esp_err_t init_littlefs(void)
{
    esp_vfs_littlefs_conf_t conf = {
        .base_path = "/littlefs",
        .partition_label = LITTLEFS_LABEL,
        .format_if_mount_failed = true,
        .dont_mount = false,
    };

    esp_err_t ret = esp_vfs_littlefs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LittleFS mount failed: %s", esp_err_to_name(ret));
        return ret;
    }

    size_t total = 0, used = 0;
    esp_littlefs_info(LITTLEFS_LABEL, &total, &used);
    ESP_LOGI(TAG, "LittleFS mounted: total=%u used=%u", (unsigned)total, (unsigned)used);
    return ESP_OK;
}

/* ── Config load / save (NVS) ───────────────────────────────────────── */

esp_err_t uploader_load_config(uploader_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    memset(cfg, 0, sizeof(*cfg));

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "no uploader config in NVS — using defaults");
        return ESP_ERR_NVS_NOT_FOUND;
    }

    size_t len;
    len = sizeof(cfg->smb_host);
    nvs_get_str(h, "smb_host", cfg->smb_host, &len);
    len = sizeof(cfg->smb_share);
    nvs_get_str(h, "smb_share", cfg->smb_share, &len);
    len = sizeof(cfg->smb_user);
    nvs_get_str(h, "smb_user", cfg->smb_user, &len);
    len = sizeof(cfg->smb_pass);
    nvs_get_str(h, "smb_pass", cfg->smb_pass, &len);
    len = sizeof(cfg->smb_path);
    nvs_get_str(h, "smb_path", cfg->smb_path, &len);
    len = sizeof(cfg->shq_client_id);
    nvs_get_str(h, "shq_cid", cfg->shq_client_id, &len);
    len = sizeof(cfg->shq_client_secret);
    nvs_get_str(h, "shq_secret", cfg->shq_client_secret, &len);

    nvs_close(h);
    return ESP_OK;
}

esp_err_t uploader_save_config(const uploader_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;

    nvs_handle_t h;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (ret != ESP_OK) return ret;

    nvs_set_str(h, "smb_host", cfg->smb_host);
    nvs_set_str(h, "smb_share", cfg->smb_share);
    nvs_set_str(h, "smb_user", cfg->smb_user);
    nvs_set_str(h, "smb_pass", cfg->smb_pass);
    nvs_set_str(h, "smb_path", cfg->smb_path);
    nvs_set_str(h, "shq_cid", cfg->shq_client_id);
    nvs_set_str(h, "shq_secret", cfg->shq_client_secret);
    nvs_commit(h);
    nvs_close(h);

    /* Update in-memory copy */
    memcpy(&s_config, cfg, sizeof(s_config));
    ESP_LOGI(TAG, "config saved to NVS");
    return ESP_OK;
}

bool uploader_is_smb_configured(void)
{
    return s_config.smb_host[0] != '\0' && s_config.smb_share[0] != '\0';
}

bool uploader_is_sleephq_configured(void)
{
    return s_config.shq_client_id[0] != '\0' && s_config.shq_client_secret[0] != '\0';
}

/* ── Config JSON for web UI ─────────────────────────────────────────── */

esp_err_t uploader_get_config_json(char **out_json)
{
    if (!out_json) return ESP_ERR_INVALID_ARG;

    cJSON *root = cJSON_CreateObject();

    cJSON *smb = cJSON_CreateObject();
    cJSON_AddStringToObject(smb, "host", s_config.smb_host);
    cJSON_AddStringToObject(smb, "share", s_config.smb_share);
    cJSON_AddStringToObject(smb, "user", s_config.smb_user);
    cJSON_AddStringToObject(smb, "path", s_config.smb_path);
    /* Mask password */
    cJSON_AddStringToObject(smb, "pass", s_config.smb_pass[0] ? "***" : "");
    cJSON_AddBoolToObject(smb, "configured", uploader_is_smb_configured());
    cJSON_AddItemToObject(root, "smb", smb);

    cJSON *shq = cJSON_CreateObject();
    cJSON_AddStringToObject(shq, "client_id", s_config.shq_client_id);
    /* Mask secret */
    cJSON_AddStringToObject(shq, "client_secret", s_config.shq_client_secret[0] ? "***" : "");
    cJSON_AddBoolToObject(shq, "configured", uploader_is_sleephq_configured());
    cJSON_AddItemToObject(root, "sleephq", shq);

    *out_json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return *out_json ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t uploader_save_config_json(const char *json_str)
{
    if (!json_str) return ESP_ERR_INVALID_ARG;

    cJSON *root = cJSON_Parse(json_str);
    if (!root) {
        ESP_LOGE(TAG, "failed to parse config JSON");
        return ESP_ERR_INVALID_STATE;
    }

    uploader_config_t cfg;
    memcpy(&cfg, &s_config, sizeof(cfg));  /* start from current */

    cJSON *smb = cJSON_GetObjectItem(root, "smb");
    if (smb) {
        cJSON *v;
        if ((v = cJSON_GetObjectItem(smb, "host")) && cJSON_IsString(v))
            strlcpy(cfg.smb_host, v->valuestring, sizeof(cfg.smb_host));
        if ((v = cJSON_GetObjectItem(smb, "share")) && cJSON_IsString(v))
            strlcpy(cfg.smb_share, v->valuestring, sizeof(cfg.smb_share));
        if ((v = cJSON_GetObjectItem(smb, "user")) && cJSON_IsString(v))
            strlcpy(cfg.smb_user, v->valuestring, sizeof(cfg.smb_user));
        if ((v = cJSON_GetObjectItem(smb, "path")) && cJSON_IsString(v))
            strlcpy(cfg.smb_path, v->valuestring, sizeof(cfg.smb_path));
        /* Only update password if not the mask string */
        if ((v = cJSON_GetObjectItem(smb, "pass")) && cJSON_IsString(v)) {
            if (strcmp(v->valuestring, "***") != 0)
                strlcpy(cfg.smb_pass, v->valuestring, sizeof(cfg.smb_pass));
        }
    }

    cJSON *shq = cJSON_GetObjectItem(root, "sleephq");
    if (shq) {
        cJSON *v;
        if ((v = cJSON_GetObjectItem(shq, "client_id")) && cJSON_IsString(v))
            strlcpy(cfg.shq_client_id, v->valuestring, sizeof(cfg.shq_client_id));
        if ((v = cJSON_GetObjectItem(shq, "client_secret")) && cJSON_IsString(v)) {
            if (strcmp(v->valuestring, "***") != 0)
                strlcpy(cfg.shq_client_secret, v->valuestring, sizeof(cfg.shq_client_secret));
        }
    }

    cJSON_Delete(root);

    esp_err_t ret = uploader_save_config(&cfg);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "config updated via web UI");
    }
    return ret;
}

/* ── Status JSON for web UI ─────────────────────────────────────────── */

esp_err_t uploader_get_status_json(char **out_json)
{
    if (!out_json) return ESP_ERR_INVALID_ARG;

    if (!s_state_mutex || !s_state) {
        *out_json = strdup("{\"sessions\":[]}");
        return *out_json ? ESP_OK : ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    esp_err_t ret = uploader_state_to_json(s_state, out_json);
    xSemaphoreGive(s_state_mutex);

    return ret;
}

/* ── Reset state ─────────────────────────────────────────────────────── */

esp_err_t uploader_reset_state(void)
{
    if (!s_state_mutex || !s_state) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    memset(s_state, 0, sizeof(*s_state));
    uploader_state_save(s_state);
    xSemaphoreGive(s_state_mutex);

    ESP_LOGI(TAG, "upload state cleared");
    return ESP_OK;
}

/* ── Upload task ────────────────────────────────────────────────────── */

static void process_day(const char *day_folder)
{
    ESP_LOGI(TAG, "processing day %s", day_folder);

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);

    /* Ensure day exists in state */
    day_state_t *day = uploader_state_find_or_create(s_state, day_folder);

    /* Dirty: if any backend was ST_OK, reset to ST_PENDING so the whole
     * day is re-uploaded with the new session data.  This happens here
     * (on the uploader task's internal stack) rather than in
     * uploader_on_day_ready() because callers may run on a PSRAM stack
     * and flash I/O would crash (cache disabled → PSRAM inaccessible). */
    if (day) {
        for (int i = 0; i < day->n_backends; i++) {
            if (day->backends[i].status == ST_OK) {
                ESP_LOGI(TAG, "  backend %s: dirtying (was OK) for re-upload", day->backends[i].name);
                day->backends[i].status = ST_PENDING;
            }
        }
    }

    /* Run each configured backend sequentially, skipping those already done */
    for (int i = 0; i < s_n_backends; i++) {
        const upload_backend_t *be = s_backends[i];
        if (!be || !be->is_configured || !be->is_configured()) {
            ESP_LOGI(TAG, "  backend %s: not configured, skipping", be ? be->name : "?");
            continue;
        }

        /* Skip backends that are still pending from a previous failed run
         * only if they were NOT dirtied above — but since dirtying resets
         * ST_OK → ST_PENDING, we only skip ST_OK (which shouldn't exist
         * after dirtying).  In practice, after dirtying all backends are
         * either ST_PENDING or ST_FAILED, so we process them all. */
        if (day) {
            backend_state_t *bs = uploader_state_backend_find_or_create(day, be->name);
            if (bs && bs->status == ST_OK) {
                ESP_LOGI(TAG, "  backend %s: already OK, skipping", be->name);
                continue;
            }
        }

        ESP_LOGI(TAG, "  backend %s: uploading...", be->name);
        upload_result_t result = be->upload_day(day_folder);

        upload_status_t st;
        switch (result) {
        case UPLOAD_OK:
            st = ST_OK;
            ESP_LOGI(TAG, "  backend %s: OK", be->name);
            break;
        case UPLOAD_NOT_CONFIGURED:
            st = ST_PENDING;
            ESP_LOGI(TAG, "  backend %s: not configured", be->name);
            break;
        default:
            st = ST_FAILED;
            ESP_LOGW(TAG, "  backend %s: FAILED", be->name);
            break;
        }

        /* Update state */
        if (day) {
            backend_state_t *bs = uploader_state_backend_find_or_create(day, be->name);
            if (bs) {
                bs->status = st;
                if (st == ST_OK || st == ST_FAILED) {
                    bs->attempts++;
                    bs->last_try_ms = (int64_t)time(NULL) * 1000;
                }
            }
        }
    }

    uploader_state_save(s_state);
    xSemaphoreGive(s_state_mutex);
}

static void check_retries(void)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);

    int64_t now_ms = (int64_t)time(NULL) * 1000;

    for (int i = 0; i < s_state->n_days; i++) {
        day_state_t *s = &s_state->days[i];
        bool needs_retry = false;

        for (int j = 0; j < s->n_backends; j++) {
            backend_state_t *b = &s->backends[j];
            if (b->status == ST_FAILED && b->attempts < MAX_RETRY_ATTEMPTS) {
                int delay_idx = b->attempts - 1;
                if (delay_idx < 0) delay_idx = 0;
                if (delay_idx >= N_RETRY_DELAYS) delay_idx = N_RETRY_DELAYS - 1;
                int delay = retry_delays_ms[delay_idx];
                int64_t elapsed = now_ms - b->last_try_ms;
                if (elapsed >= delay) {
                    ESP_LOGI(TAG, "retry: day %s backend %s: attempt %d, elapsed %lldms (delay was %dms)",
                             s->day_folder, b->name, b->attempts + 1,
                             (long long)elapsed, delay);
                    needs_retry = true;
                    break;
                }
            }
        }

        if (needs_retry) {
            ESP_LOGI(TAG, "retry: day %s needs retry", s->day_folder);
            upload_event_t ev = {0};
            strlcpy(ev.day_folder, s->day_folder, sizeof(ev.day_folder));
            xQueueSend(s_queue, &ev, 0);
        }
    }

    xSemaphoreGive(s_state_mutex);
}

static void upload_task(void *arg)
{
    ESP_LOGI(TAG, "upload task started on core %d", xPortGetCoreID());

    /* On boot, re-queue any pending/failed days */
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    const char *days[UPLOAD_MAX_DAYS];

    const char *be_names[MAX_BACKENDS];
    int n_be = 0;
    for (int i = 0; i < s_n_backends; i++) {
        if (s_backends[i] && s_backends[i]->is_configured && s_backends[i]->is_configured()) {
            be_names[n_be++] = s_backends[i]->name;
        }
    }

    int n_pending = uploader_state_get_pending(s_state, be_names, n_be,
                                                days, UPLOAD_MAX_DAYS);
    xSemaphoreGive(s_state_mutex);

    for (int i = 0; i < n_pending; i++) {
        upload_event_t ev = {0};
        strlcpy(ev.day_folder, days[i], sizeof(ev.day_folder));
        xQueueSend(s_queue, &ev, 0);
    }

    if (n_pending > 0) {
        ESP_LOGI(TAG, "queued %d pending days from state", n_pending);
    }

    /* Main loop: wait for events, process, then periodic retry check */
    TickType_t last_retry_check = xTaskGetTickCount();

    while (true) {
        upload_event_t ev;
        TickType_t now = xTaskGetTickCount();
        TickType_t wait_ticks = pdMS_TO_TICKS(RETRY_INTERVAL_MS);

        /* Time until next retry check */
        TickType_t since_check = now - last_retry_check;
        if (since_check < pdMS_TO_TICKS(RETRY_INTERVAL_MS)) {
            wait_ticks = pdMS_TO_TICKS(RETRY_INTERVAL_MS) - since_check;
        }

        if (xQueueReceive(s_queue, &ev, wait_ticks) == pdTRUE) {
            process_day(ev.day_folder);
        }

        /* Periodic retry check */
        now = xTaskGetTickCount();
        if (now - last_retry_check >= pdMS_TO_TICKS(RETRY_INTERVAL_MS)) {
            last_retry_check = now;
            check_retries();
        }
    }

    vTaskDelete(NULL);
}

/* ── Public API ─────────────────────────────────────────────────────── */

esp_err_t uploader_init(void)
{
    if (s_initialised) return ESP_OK;

    /* 1. Mount LittleFS */
    esp_err_t ret = init_littlefs();
    if (ret != ESP_OK) return ret;

    /* 2. Allocate upload state in PSRAM to keep internal RAM free for
     *    DMA-critical WiFi/BLE buffers. */
    s_state = heap_caps_calloc(1, sizeof(upload_state_t), MALLOC_CAP_SPIRAM);
    if (!s_state) {
        /* Fall back to regular malloc if PSRAM unavailable */
        s_state = calloc(1, sizeof(upload_state_t));
    }
    if (!s_state) {
        ESP_LOGE(TAG, "failed to allocate upload state (%u bytes)",
                 (unsigned)sizeof(upload_state_t));
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "upload state allocated: %u bytes (PSRAM=%s)",
             (unsigned)sizeof(upload_state_t),
             heap_caps_check_integrity(MALLOC_CAP_SPIRAM, false) ? "yes" : "no");

    /* 3. Create state mutex */
    s_state_mutex = xSemaphoreCreateMutex();
    if (!s_state_mutex) return ESP_ERR_NO_MEM;

    /* 4. Load config from NVS */
    uploader_load_config(&s_config);

    /* 5. Load upload state from LittleFS */
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    uploader_state_load(s_state);
    uploader_state_prune(s_state, 30);
    xSemaphoreGive(s_state_mutex);

    /* 6. Register backends */
    uploader_register_backend(&smb_backend);
    uploader_register_backend(&sleephq_backend);

    /* 7. Create event queue */
    s_queue = xQueueCreate(UPLOAD_QUEUE_LEN, sizeof(upload_event_t));
    if (!s_queue) return ESP_ERR_NO_MEM;

    /* 8. Start upload task on core 0 (same as BLE, but low priority) */
    BaseType_t xret = xTaskCreatePinnedToCore(
        upload_task, "uploader", UPLOAD_TASK_STACK, NULL,
        UPLOAD_TASK_PRIORITY, NULL, 0);

    if (xret != pdPASS) {
        ESP_LOGE(TAG, "failed to create upload task");
        return ESP_ERR_NO_MEM;
    }

    s_initialised = true;
    ESP_LOGI(TAG, "uploader initialised (%d backends, %d days in state)",
             s_n_backends, s_state->n_days);

    /* Log heap diagnostics for debugging memory issues */
    ESP_LOGI(TAG, "heap: internal free=%u min=%u | PSRAM free=%u min=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));

    return ESP_OK;
}

void uploader_on_day_ready(const char *day_folder)
{
    if (!s_initialised || !day_folder) return;

    ESP_LOGI(TAG, "day ready for upload: %s", day_folder);

    /* Queue the upload event — dirtying and state save happen in
     * process_day() on the uploader task (internal stack).  Do NOT
     * do flash I/O here: callers may run on a PSRAM stack, and flash
     * operations disable the cache, making PSRAM inaccessible. */
    upload_event_t ev = {0};
    strlcpy(ev.day_folder, day_folder, sizeof(ev.day_folder));
    xQueueSend(s_queue, &ev, portMAX_DELAY);
}
