/*
 * SomnoTrace - Session data writer for SD card storage
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

#include "session_writer.h"
#include "sd_storage.h"
#include "bsp_display.h"
#include "as11_ble.h"
#include "post_therapy.h"
#include "edf_gen.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "cJSON.h"

static const char *TAG = "session";

/* ── .snt file format ─────────────────────────────────────────────── */

#define SNT_MAGIC       0x534E5442u   /* "SNTB" */
#define SNT_VERSION     1

typedef struct __attribute__((packed)) {
    uint32_t magic;            /* 0x534E5442 "SNTB"                  */
    uint8_t  version;          /* format version (1)                  */
    uint8_t  tier;             /* 0 = L0 raw, 1 = L1 MinMax          */
    uint8_t  n_channels;       /* channels per record                 */
    uint8_t  sample_bytes;     /* 2 (int16)                           */
    uint16_t sample_hz_x10;   /* rate × 10 (250 = 25 Hz)            */
    uint16_t reserved;
    int64_t  start_epoch_ms;  /* session start (NTP clock)           */
    uint32_t sample_count;    /* records written (updated each flush) */
    uint32_t reserved2;
} snt_header_t;              /* 32 bytes */

#define SNT_HEADER_SIZE  sizeof(snt_header_t)   /* 32 bytes */

/* ── Session state ────────────────────────────────────────────────── */

#define FLUSH_INTERVAL_SEC   60
#define FLUSH_TASK_STACK     4096
#define MAX_SESSION_DIR_LEN  128

/* Per-stream file handles and counters */
typedef struct {
    FILE    *f_l0;       /* L0 interleaved raw */
    FILE    *f_l1;       /* L1 MinMax sidecar (BRP only) */
    uint32_t sample_count;   /* total samples written to file (per channel) */
} stream_files_t;

struct session_writer {
    char     dir[MAX_SESSION_DIR_LEN];
    char     session_id[16];      /* YYYYMMDD_HHMM */
    int64_t  start_time_us;       /* boot-relative us (for duration) */
    int64_t  start_epoch_ms;      /* NTP epoch ms at session start */
    int64_t  end_time_us;         /* boot-relative us (for duration) */
    int64_t  end_epoch_ms;        /* NTP epoch ms at session stop */
    bool     active;

    stream_files_t brp;     /* 25 Hz, 2 ch: flow + mask_pressure (40ms natural) */
    stream_files_t sa2;     /* 1 Hz, 2 ch: heart_rate + spo2 (decimated from 5 Hz report) */
    stream_files_t pld;     /* 0.5 Hz, 12 ch (0.5 Hz natural, decimated from 5 Hz report) */
    FILE    *f_events;
    uint32_t brp_mm_count;  /* L1 MinMax records written */

    int64_t  clock_drift_ms;   /* NTP time - AS11 device time at session stop */

    /* recent buffer: holds data between flushes */
    SemaphoreHandle_t mutex;
    TaskHandle_t flush_task_handle;  /* flush task for this session */

    /* BRP recent buffer (largest, 25 Hz × 2 ch × 60 s = 3000 samples) */
    int16_t brp_flow[1500];
    int16_t brp_press[1500];
    uint32_t brp_buf_count;     /* samples in buffer (per channel) */

    /* SA2 recent buffer (1 Hz × 2 ch × 60 s = 60 samples) */
    int16_t sa2_hr[60];
    int16_t sa2_spo2[60];
    uint32_t sa2_buf_count;
    uint8_t sa2_countdown;     /* decimation: write SA2 every 5th notification (5 Hz / 5 = 1 Hz) */

    /* PLD recent buffer (0.5 Hz × 12 ch × 60 s = 30 samples) */
    int16_t pld_buf[300][12];
    uint32_t pld_buf_count;
    uint8_t pld_countdown;     /* decimation: write PLD every 10th notification */

    /* events: simple JSON line file, flushed with data */
};

static session_writer_t *s_active = NULL;
static SemaphoreHandle_t s_active_mutex = NULL;
static bool s_therapy_stopped = false;  /* set on TherapyStop, cleared on TherapyStart */

static char s_device_addr[32] = {0};
static char s_client_id[64] = {0};

/* ── Helpers ──────────────────────────────────────────────────────── */

static void make_session_id(char *out, size_t out_len)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    snprintf(out, out_len, "%04d%02d%02d_%02d%02d",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min);
}

static void write_snt_header(FILE *f, uint8_t tier, uint16_t hz_x10,
                             uint8_t n_ch, int64_t start_epoch_ms)
{
    snt_header_t hdr = {
        .magic = SNT_MAGIC,
        .version = SNT_VERSION,
        .tier = tier,
        .n_channels = n_ch,
        .sample_bytes = 2,
        .sample_hz_x10 = hz_x10,
        .reserved = 0,
        .start_epoch_ms = start_epoch_ms,
        .sample_count = 0,
        .reserved2 = 0,
    };
    fwrite(&hdr, 1, SNT_HEADER_SIZE, f);
}

static void update_snt_header_sample_count(FILE *f, uint32_t count)
{
    if (!f) return;
    long pos = ftell(f);
    fseek(f, offsetof(snt_header_t, sample_count), SEEK_SET);
    fwrite(&count, sizeof(uint32_t), 1, f);
    fflush(f);
    fseek(f, pos, SEEK_SET);
}

/* ── Flush ────────────────────────────────────────────────────────── */

static void flush_brp(session_writer_t *s)
{
    if (!s->brp.f_l0 || s->brp_buf_count == 0) return;

    /* L0: interleave flow + pressure */
    for (uint32_t i = 0; i < s->brp_buf_count; i++) {
        int16_t pair[2] = { s->brp_flow[i], s->brp_press[i] };
        fwrite(pair, sizeof(int16_t), 2, s->brp.f_l0);
    }

    /* L1: 1-second MinMax (25 samples per second) */
    if (s->brp.f_l1) {
        uint32_t n_sec = s->brp_buf_count / 25;
        for (uint32_t sec = 0; sec < n_sec; sec++) {
            uint32_t base = sec * 25;
            int16_t fmn = INT16_MAX, fmx = -1;
            int16_t pmn = INT16_MAX, pmx = -1;
            for (int j = 0; j < 25; j++) {
                int16_t fv = s->brp_flow[base + j];
                int16_t pv = s->brp_press[base + j];
                if (fv != -1) {
                    if (fv < fmn) fmn = fv;
                    if (fv > fmx) fmx = fv;
                }
                if (pv != -1) {
                    if (pv < pmn) pmn = pv;
                    if (pv > pmx) pmx = pv;
                }
            }
            if (fmn == INT16_MAX) { fmn = fmx = -1; }
            if (pmn == INT16_MAX) { pmn = pmx = -1; }
            int16_t mm[4] = { fmn, fmx, pmn, pmx };
            fwrite(mm, sizeof(int16_t), 4, s->brp.f_l1);
            s->brp_mm_count++;
        }
    }

    s->brp.sample_count += s->brp_buf_count;
    s->brp_buf_count = 0;
}

static void flush_sa2(session_writer_t *s)
{
    if (!s->sa2.f_l0 || s->sa2_buf_count == 0) return;

    for (uint32_t i = 0; i < s->sa2_buf_count; i++) {
        int16_t pair[2] = { s->sa2_hr[i], s->sa2_spo2[i] };
        fwrite(pair, sizeof(int16_t), 2, s->sa2.f_l0);
    }
    s->sa2.sample_count += s->sa2_buf_count;
    s->sa2_buf_count = 0;
}

static void flush_pld(session_writer_t *s)
{
    if (!s->pld.f_l0 || s->pld_buf_count == 0) return;

    for (uint32_t i = 0; i < s->pld_buf_count; i++) {
        fwrite(s->pld_buf[i], sizeof(int16_t), 12, s->pld.f_l0);
    }
    s->pld.sample_count += s->pld_buf_count;
    s->pld_buf_count = 0;
}

static void flush_all(session_writer_t *s)
{
    if (!s) return;

    xSemaphoreTake(s->mutex, portMAX_DELAY);

    flush_brp(s);
    flush_sa2(s);
    flush_pld(s);

    /* sync all files */
    if (s->brp.f_l0) { fflush(s->brp.f_l0); update_snt_header_sample_count(s->brp.f_l0, s->brp.sample_count); }
    if (s->brp.f_l1) { fflush(s->brp.f_l1); update_snt_header_sample_count(s->brp.f_l1, s->brp_mm_count); }
    if (s->sa2.f_l0) { fflush(s->sa2.f_l0); update_snt_header_sample_count(s->sa2.f_l0, s->sa2.sample_count); }
    if (s->pld.f_l0) { fflush(s->pld.f_l0); update_snt_header_sample_count(s->pld.f_l0, s->pld.sample_count); }
    if (s->f_events) fflush(s->f_events);

    ESP_LOGI(TAG, "flush: brp=%u sa2=%u pld=%u samples",
             (unsigned)s->brp.sample_count,
             (unsigned)s->sa2.sample_count,
             (unsigned)s->pld.sample_count);

    xSemaphoreGive(s->mutex);
}

/* ── Flush task ───────────────────────────────────────────────────── */

static void flush_task(void *arg)
{
    session_writer_t *s = (session_writer_t *)arg;
    while (s && s->active) {
        /* Wait for flush interval OR until woken by stop_task */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(FLUSH_INTERVAL_SEC * 1000));
        if (s->active) {
            ESP_LOGI(TAG, "periodic flush tick");
            flush_all(s);
        }
    }
    vTaskDelete(NULL);
}

/* ── Session JSON ─────────────────────────────────────────────────── */

static void write_session_json(session_writer_t *s, const char *state)
{
    char path[MAX_SESSION_DIR_LEN + 32];
    snprintf(path, sizeof(path), "%s/session.json", s->dir);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "id", s->session_id);

    cJSON_AddNumberToObject(root, "start_epoch_ms", (double)s->start_epoch_ms);
    if (s->end_epoch_ms > 0) {
        cJSON_AddNumberToObject(root, "end_epoch_ms", (double)s->end_epoch_ms);
    }

    char iso_start[32], iso_end[32];
    struct tm tm;
    time_t start = (time_t)(s->start_epoch_ms / 1000);
    localtime_r(&start, &tm);
    strftime(iso_start, sizeof(iso_start), "%Y-%m-%dT%H:%M:%S", &tm);
    cJSON_AddStringToObject(root, "start_iso", iso_start);

    if (s->end_epoch_ms > 0) {
        time_t end = (time_t)(s->end_epoch_ms / 1000);
        localtime_r(&end, &tm);
        strftime(iso_end, sizeof(iso_end), "%Y-%m-%dT%H:%M:%S", &tm);
        cJSON_AddStringToObject(root, "end_iso", iso_end);
    }

    cJSON_AddStringToObject(root, "state", state);
    cJSON_AddNumberToObject(root, "brp_samples", (double)s->brp.sample_count);
    cJSON_AddNumberToObject(root, "brp_mm_samples", (double)s->brp_mm_count);
    cJSON_AddNumberToObject(root, "sa2_samples", (double)s->sa2.sample_count);
    cJSON_AddNumberToObject(root, "pld_samples", (double)s->pld.sample_count);

    if (s->clock_drift_ms != 0) {
        cJSON_AddNumberToObject(root, "clock_drift_ms", (double)s->clock_drift_ms);
    }

    if (s_device_addr[0]) cJSON_AddStringToObject(root, "as11_device", s_device_addr);
    if (s_client_id[0]) cJSON_AddStringToObject(root, "as11_client_id", s_client_id);

    char *json_str = cJSON_Print(root);
    cJSON_Delete(root);

    FILE *f = fopen(path, "w");
    if (f) {
        fputs(json_str, f);
        fclose(f);
        ESP_LOGI(TAG, "wrote %s (state=%s)", path, state);
    } else {
        ESP_LOGE(TAG, "failed to write %s", path);
    }
    free(json_str);
}

/* ── Public API ───────────────────────────────────────────────────── */

esp_err_t session_writer_init(void)
{
    s_active_mutex = xSemaphoreCreateMutex();
    if (!s_active_mutex) return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "session writer initialised");
    return ESP_OK;
}

session_writer_t *session_writer_start(void)
{
    if (!sd_storage_is_ready()) {
        ESP_LOGE(TAG, "cannot start session: SD not ready");
        return NULL;
    }

    xSemaphoreTake(s_active_mutex, portMAX_DELAY);
    if (s_active) {
        session_writer_t *prev = s_active;
        ESP_LOGW(TAG, "session already active, stopping previous");
        session_writer_stop(prev);
        free(prev);
    }

    session_writer_t *s = calloc(1, sizeof(session_writer_t));
    if (!s) {
        xSemaphoreGive(s_active_mutex);
        return NULL;
    }

    s->mutex = xSemaphoreCreateMutex();
    if (!s->mutex) {
        free(s);
        xSemaphoreGive(s_active_mutex);
        return NULL;
    }

    s->pld_countdown = 1;  /* first notification writes PLD immediately */
    s->sa2_countdown = 1;  /* first notification writes SA2 immediately */

    make_session_id(s->session_id, sizeof(s->session_id));

    /* Try to create the session directory. If it already exists (same-minute
     * restart), append a suffix to keep session data separate. */
    snprintf(s->dir, sizeof(s->dir), "%s/%s", SD_SESSIONS_DIR, s->session_id);
    if (mkdir(s->dir, 0775) != 0) {
        if (errno == EEXIST) {
            /* Directory exists — try with _2, _3, etc. */
            for (int suffix = 2; suffix < 100; suffix++) {
                snprintf(s->dir, sizeof(s->dir), "%s/%s_%d",
                         SD_SESSIONS_DIR, s->session_id, suffix);
                if (mkdir(s->dir, 0775) == 0) break;
            }
            /* Update session_id to include the suffix for session.json */
            char sid_with_suffix[64];
            snprintf(sid_with_suffix, sizeof(sid_with_suffix),
                     "%s_%d", s->session_id, 2);
            /* Check if we found a valid dir */
            struct stat st;
            if (stat(s->dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
                ESP_LOGE(TAG, "failed to create session dir for %s", s->session_id);
                free(s);
                xSemaphoreGive(s_active_mutex);
                return NULL;
            }
            /* Update session_id to match the directory name */
            char *last_slash = strrchr(s->dir, '/');
            if (last_slash) {
                strncpy(s->session_id, last_slash + 1, sizeof(s->session_id) - 1);
            }
        } else {
            ESP_LOGE(TAG, "failed to create session dir %s", s->dir);
            free(s);
            xSemaphoreGive(s_active_mutex);
            return NULL;
        }
    }

    s->start_time_us = esp_timer_get_time();
    s->start_epoch_ms = (int64_t)time(NULL) * 1000;
    s->active = true;

    /* Open files and write headers */
    char path[MAX_SESSION_DIR_LEN + 32];
    int64_t start_epoch_ms = s->start_epoch_ms;

    snprintf(path, sizeof(path), "%s/brp.snt", s->dir);
    s->brp.f_l0 = fopen(path, "wb");
    if (s->brp.f_l0) write_snt_header(s->brp.f_l0, 0, 250, 2, start_epoch_ms);  /* 25 Hz */

    snprintf(path, sizeof(path), "%s/brp_mm.snt", s->dir);
    s->brp.f_l1 = fopen(path, "wb");
    if (s->brp.f_l1) write_snt_header(s->brp.f_l1, 1, 10, 4, start_epoch_ms);  /* 1 Hz */

    snprintf(path, sizeof(path), "%s/sa2.snt", s->dir);
    s->sa2.f_l0 = fopen(path, "wb");
    if (s->sa2.f_l0) write_snt_header(s->sa2.f_l0, 0, 10, 2, start_epoch_ms);  /* 1 Hz */

    snprintf(path, sizeof(path), "%s/pld.snt", s->dir);
    s->pld.f_l0 = fopen(path, "wb");
    if (s->pld.f_l0) write_snt_header(s->pld.f_l0, 0, 5, 12, start_epoch_ms);  /* 0.5 Hz */

    snprintf(path, sizeof(path), "%s/events.snt", s->dir);
    s->f_events = fopen(path, "w");
    if (!s->f_events) {
        ESP_LOGW(TAG, "failed to open events.snt (non-fatal)");
    }

    if (!s->brp.f_l0 || !s->sa2.f_l0 || !s->pld.f_l0) {
        ESP_LOGE(TAG, "failed to open .snt files");
        session_writer_stop(s);
        free(s);
        xSemaphoreGive(s_active_mutex);
        return NULL;
    }

    s_active = s;
    xSemaphoreGive(s_active_mutex);

    /* Start flush task */
    xTaskCreate(flush_task, "session_flush", FLUSH_TASK_STACK, s, 5, &s->flush_task_handle);

    ESP_LOGI(TAG, "=== SESSION STARTED: %s ===", s->session_id);
    ESP_LOGI(TAG, "dir: %s", s->dir);

    return s;
}

esp_err_t session_writer_stop(session_writer_t *s)
{
    if (!s) return ESP_OK;

    /* Query AS11 clock BEFORE taking s_active_mutex.
     * This blocks on the RPC response, which is processed by notif_proc_task.
     * If we held s_active_mutex here, a TherapyStart in notif_proc_task would
     * block on the mutex and be unable to process the GetDateTime response. */
    int64_t end_epoch_ms = (int64_t)time(NULL) * 1000;
    int64_t as11_ms = 0;
    int64_t clock_drift_ms = 0;
    bool have_drift = false;

    if (as11_ble_get_datetime(&as11_ms) == ESP_OK) {
        clock_drift_ms = end_epoch_ms - as11_ms;
        have_drift = true;
        ESP_LOGI(TAG, "clock_drift_ms = %lld (stop-time, NTP=%lld AS11=%lld)",
                 (long long)clock_drift_ms,
                 (long long)end_epoch_ms,
                 (long long)as11_ms);
    } else {
        int64_t drift = 0;
        if (as11_ble_get_clock_drift(&drift) == ESP_OK) {
            clock_drift_ms = drift;
            have_drift = true;
            ESP_LOGI(TAG, "clock_drift_ms = %lld (pre-stream fallback)",
                     (long long)clock_drift_ms);
        } else {
            ESP_LOGW(TAG, "clock_drift_ms unavailable");
        }
    }

    xSemaphoreTake(s_active_mutex, portMAX_DELAY);

    s->active = false;
    s->end_time_us = esp_timer_get_time();
    s->end_epoch_ms = end_epoch_ms;
    if (have_drift) s->clock_drift_ms = clock_drift_ms;

    /* Signal flush_task to exit BEFORE doing any cleanup.
     * flush_task might be sleeping or about to call flush_all.
     * By setting s->active=false and sending notification first,
     * flush_task will see active==false and exit without touching files. */
    if (s->flush_task_handle) {
        xTaskNotifyGive(s->flush_task_handle);
        int wait = 0;
        while (eTaskGetState(s->flush_task_handle) != eDeleted && wait < 100) {
            vTaskDelay(pdMS_TO_TICKS(10));
            wait++;
        }
        if (wait >= 100) {
            ESP_LOGW(TAG, "flush_task did not exit in 1s, proceeding anyway");
        }
        s->flush_task_handle = NULL;
    }

    /* Final flush */
    flush_all(s);

    /* Close data files first to free file descriptors */
    if (s->brp.f_l0) { fclose(s->brp.f_l0); s->brp.f_l0 = NULL; }
    if (s->brp.f_l1) { fclose(s->brp.f_l1); s->brp.f_l1 = NULL; }
    if (s->sa2.f_l0) { fclose(s->sa2.f_l0); s->sa2.f_l0 = NULL; }
    if (s->pld.f_l0) { fclose(s->pld.f_l0); s->pld.f_l0 = NULL; }
    if (s->f_events) { fclose(s->f_events); s->f_events = NULL; }

    /* Write session.json (after closing data files to avoid FD exhaustion) */
    write_session_json(s, "completed");

    ESP_LOGI(TAG, "=== SESSION STOPPED: %s ===", s->session_id);
    ESP_LOGI(TAG, "brp=%u sa2=%u pld=%u total samples",
             (unsigned)s->brp.sample_count,
             (unsigned)s->sa2.sample_count,
             (unsigned)s->pld.sample_count);

    if (s_active == s) s_active = NULL;

    if (s->mutex) vSemaphoreDelete(s->mutex);
    /* NOTE: s is NOT freed here — the caller (stop_task) needs to read
     * s->clock_drift_ms and s->start_epoch_ms after this function returns.
     * The caller is responsible for calling free(s). */
    s->mutex = NULL;  /* prevent double-delete */

    xSemaphoreGive(s_active_mutex);
    return ESP_OK;
}

bool session_writer_is_active(const session_writer_t *s)
{
    return s && s->active;
}

session_writer_t *session_writer_get_active(void)
{
    return s_active;
}

void session_writer_set_device_info(const char *addr, const char *client_id)
{
    if (addr) strlcpy(s_device_addr, addr, sizeof(s_device_addr));
    if (client_id) strlcpy(s_client_id, client_id, sizeof(s_client_id));
}

/* ── Notification parsing ─────────────────────────────────────────── */

/* Check an EventNotification for therapy start/stop events.
 * The AS11 sends EventNotification with params.dataId="UsageEvents-TherapyStatusEvents"
 * and params.events[] containing objects with an "event" field.
 * Returns true if a therapy start event is found. */
static bool check_event_notification(const cJSON *msg, bool *out_start, bool *out_stop)
{
    *out_start = false;
    *out_stop = false;

    cJSON *params = cJSON_GetObjectItem(msg, "params");
    if (!params) return false;

    cJSON *events = cJSON_GetObjectItem(params, "events");
    if (!events || !cJSON_IsArray(events)) return false;

    int n = cJSON_GetArraySize(events);
    for (int i = 0; i < n; i++) {
        cJSON *ev = cJSON_GetArrayItem(events, i);
        if (!ev) continue;
        cJSON *event = cJSON_GetObjectItem(ev, "event");
        if (!event || !cJSON_IsString(event)) continue;

        if (strcmp(event->valuestring, "TherapyStart") == 0) {
            *out_start = true;
        } else if (strcmp(event->valuestring, "TherapyStop") == 0) {
            *out_stop = true;
        }
    }
    return *out_start || *out_stop;
}

/* ── Fast-path StreamData parser (bypasses cJSON) ─────────────────── */

/* Stream key identifiers — index into results arrays.
 * Order matches the pld_names layout for PLD channels (0-11). */
enum {
    KEY_PATIENT_FLOW = 0,
    KEY_MASK_PRESSURE,
    KEY_MASK_PRESSURE_2S,
    KEY_INSP_PRESSURE_2S,
    KEY_EXPR_PRESSURE_2S,
    KEY_LEAK,
    KEY_RR2,
    KEY_TD2,
    KEY_MV2,
    KEY_TGT,
    KEY_IE2,
    KEY_SNI,
    KEY_FFL,
    KEY_INT,
    KEY_HEART_RATE,
    KEY_SPO2,
    KEY_COUNT
};

/* Key lookup table — name, pre-computed length, key id.
 * Sorted by length for bucket-matching in the single-pass scanner. */
typedef struct { const char *name; int len; int id; } key_entry_t;
static const key_entry_t s_keys[] = {
    {"SpO2",                          4,  KEY_SPO2},
    {"Leak",                          4,  KEY_LEAK},
    {"_RR2",                          4,  KEY_RR2},
    {"_TD2",                          4,  KEY_TD2},
    {"_MV2",                          4,  KEY_MV2},
    {"_TGT",                          4,  KEY_TGT},
    {"_IE2",                          4,  KEY_IE2},
    {"HeartRate",                     9,  KEY_HEART_RATE},
    {"SnoreIndex",                   10,  KEY_SNI},
    {"PatientFlow",                  11,  KEY_PATIENT_FLOW},
    {"MaskPressure",                 12,  KEY_MASK_PRESSURE},
    {"FlowLimitation",               14,  KEY_FFL},
    {"InspiratoryDuration",          19,  KEY_INT},
    {"MaskPressure-TwoSecond",       22,  KEY_MASK_PRESSURE_2S},
    {"ExpiratoryPressure-TwoSecond", 28,  KEY_EXPR_PRESSURE_2S},
    {"InspiratoryPressure-TwoSecond",29,  KEY_INSP_PRESSURE_2S},
};
#define N_KEYS (int)(sizeof(s_keys) / sizeof(s_keys[0]))

/* Parse a JSON number array, scaling values by 100 into int16_t.
 * null values become -1. No strtod, no double arithmetic.
 * Returns count of values parsed. Advances *p past closing ']'. */
static int parse_scaled_array(const char **p, const char *end, int16_t *out, int max_out)
{
    const char *s = *p;
    while (s < end && *s != '[') s++;
    if (s >= end || *s != '[') { *p = s; return 0; }
    s++;
    int count = 0;
    while (s < end && *s != ']' && count < max_out) {
        while (s < end && (*s == ' ' || *s == ',' || *s == '\t' || *s == '\n')) s++;
        if (s >= end || *s == ']') break;

        if (s + 3 < end && s[0] == 'n' && s[1] == 'u' && s[2] == 'l' && s[3] == 'l') {
            out[count++] = -1;  /* null → -1 sentinel (matches AS11/OSCAR convention) */
            s += 4;
            continue;
        }

        bool neg = false;
        if (s < end && (*s == '-' || *s == '+')) { neg = (*s == '-'); s++; }

        long ip = 0;
        while (s < end && *s >= '0' && *s <= '9') { ip = ip * 10 + (*s - '0'); s++; }

        int frac = 0, fd = 0;
        if (s < end && *s == '.') {
            s++;
            while (s < end && *s >= '0' && *s <= '9') {
                if (fd < 2) { frac = frac * 10 + (*s - '0'); fd++; }
                s++;
            }
        }

        long scaled = ip * 100;
        if (fd == 1) scaled += frac * 10;
        else if (fd == 2) scaled += frac;
        if (neg) scaled = -scaled;
        if (scaled > 32767) scaled = 32767;
        if (scaled < -32768) scaled = -32768;
        out[count++] = (int16_t)scaled;
    }
    while (s < end && *s != ']') s++;
    if (s < end && *s == ']') s++;
    *p = s;
    return count;
}

/* Match a key of known length against the key table.
 * Returns key id or -1 if no match. Keys are grouped by length
 * in s_keys, so we only compare against same-length entries. */
static int match_key(const char *key, int klen)
{
    for (int i = 0; i < N_KEYS; i++) {
        if (s_keys[i].len != klen) continue;
        if (strncmp(key, s_keys[i].name, klen) == 0) return s_keys[i].id;
    }
    return -1;
}

/* Fast-path: process StreamData from raw JSON in a single linear pass.
 *
 * StartStream sends short tags (_RFL, _MKP, _MKF, etc.) but the AS11
 * normalizes them to long names in StreamData (per rpc_streams.md):
 *   _RFL → PatientFlow, _MKP → MaskPressure
 *   _MKF → MaskPressure-TwoSecond, _MKI → InspiratoryPressure-TwoSecond, etc.
 *   _RR2, _TD2, _MV2, _TGT, _IE2 have no long name — echoed as-is.
 *   _HRT → HeartRate, _SAO → SpO2
 *
 * Single-pass: walks the JSON once, matching keys by length-bucketed
 * comparison. Values are parsed directly to scaled int16_t (×100),
 * avoiding strtod and double arithmetic entirely. */
void session_writer_on_stream_data_raw(const char *json, int len)
{
    session_writer_t *s = session_writer_get_active();

    /* One-time log of raw StreamData for key-name verification */
    static bool s_first_logged = false;
    if (!s_first_logged) {
        s_first_logged = true;
        int log_len = len < 400 ? len : 400;
        ESP_LOGI(TAG, "StreamData first notification (first %d bytes): %.*s",
                 log_len, log_len, json);
    }

    const char *end = json + len;
    const char *p = json;

    /* Parsed results — filled during single-pass scan */
    int16_t flow_vals[32];  int flow_n = 0;
    int16_t press_vals[32]; int press_n = 0;
    int16_t pld_vals[12] = {0}; bool pld_found[12] = {false};
    int16_t hr_val = 0;     bool hr_found = false;
    int16_t spo2_val = 0;   bool spo2_found = false;

    /* Single-pass scan: walk JSON, extract all key→array pairs */
    while (p < end) {
        while (p < end && *p != '"') p++;
        if (p >= end) break;
        p++; /* skip opening quote */
        const char *key_start = p;
        while (p < end && *p != '"') p++;
        if (p >= end) break;
        int klen = p - key_start;
        p++; /* skip closing quote */

        while (p < end && (*p == ' ' || *p == '\t')) p++;
        if (p >= end || *p != ':') continue;
        p++;
        while (p < end && (*p == ' ' || *p == '\t')) p++;
        if (p >= end || *p != '[') continue;

        int id = match_key(key_start, klen);
        if (id < 0) continue;

        int16_t vals[32];
        int n = parse_scaled_array(&p, end, vals, 32);
        if (n <= 0) continue;

        switch (id) {
        case KEY_PATIENT_FLOW:
            memcpy(flow_vals, vals, n * sizeof(int16_t));
            flow_n = n;
            break;
        case KEY_MASK_PRESSURE:
            memcpy(press_vals, vals, n * sizeof(int16_t));
            press_n = n;
            break;
        case KEY_HEART_RATE:
            hr_val = vals[n - 1];
            hr_found = true;
            break;
        case KEY_SPO2:
            spo2_val = vals[n - 1];
            spo2_found = true;
            break;
        default:
            if (id >= KEY_MASK_PRESSURE_2S && id <= KEY_INT) {
                int pld_idx = id - KEY_MASK_PRESSURE_2S;
                pld_vals[pld_idx] = vals[n - 1];
                pld_found[pld_idx] = true;
            }
            break;
        }
    }

    /* PatientFlow: push to display, detect active flow */
    bool has_active_flow = false;
    for (int j = 0; j < flow_n; j++) {
        bsp_display_push_flow(flow_vals[j] / 100.0f);
        if (abs(flow_vals[j]) > 50) has_active_flow = true;
    }

    /* Mask pressure: during therapy, pressure is always >= 4 cmH2O (400).
     * At idle, the AS11 sends a baseline flow (~0.5 L/s) and low pressure
     * (~0.4 cmH2O). Require both flow AND pressure to avoid false triggers. */
    bool has_therapy_pressure = false;
    for (int j = 0; j < press_n; j++) {
        if (press_vals[j] > 200) has_therapy_pressure = true;
    }

    /* Edge case: auto-start session if flow and pressure indicate active therapy */
    if ((!s || !session_writer_is_active(s)) && !s_therapy_stopped && has_active_flow && has_therapy_pressure) {
        ESP_LOGI(TAG, ">>> THERAPY detected via non-zero flow (reboot mid-therapy?)");
        bsp_display_set_therapy_active(true);
        s = session_writer_start();
    }

    if (!s || !session_writer_is_active(s)) return;

    xSemaphoreTake(s->mutex, portMAX_DELAY);

    /* BRP: PatientFlow + MaskPressure (25 Hz, 40ms natural interval) */
    for (int j = 0; j < flow_n && s->brp_buf_count < 1500; j++) {
        s->brp_flow[s->brp_buf_count] = flow_vals[j];
        s->brp_buf_count++;
    }

    for (int j = 0; j < press_n; j++) {
        if (s->brp_buf_count <= j) {
            if (s->brp_buf_count < 1500) {
                s->brp_flow[s->brp_buf_count] = -1;  /* missing flow → -1 sentinel */
                s->brp_buf_count++;
            }
        }
        if (j < 1500) {
            s->brp_press[j] = press_vals[j];
        }
    }

    /* SA2: HeartRate + SpO2 (1 Hz, decimated from 5 Hz notifications).
     * Take every 5th notification to get 1 Hz. */
    if (--s->sa2_countdown == 0) {
        s->sa2_countdown = 5;
        if (s->sa2_buf_count < 60) {
            s->sa2_hr[s->sa2_buf_count] = hr_found ? hr_val : -1;
            s->sa2_spo2[s->sa2_buf_count] = spo2_found ? spo2_val : -1;
            s->sa2_buf_count++;
        }
    } else if (spo2_found && s->sa2_buf_count > 0) {
        /* Update SpO2 for the most recent SA2 sample */
        s->sa2_spo2[s->sa2_buf_count - 1] = spo2_val;
    }

    /* PLD: 12 channels, all 0.5 Hz (2s natural interval).
     * Decimate to 0.5 Hz — write every 10th notification (5 Hz / 10 = 0.5 Hz).
     * Unsupported signals are absent from StreamData — channels stay -1. */
    if (--s->pld_countdown == 0) {
        s->pld_countdown = 10;
        bool any_found = false;
        for (int k = 0; k < 12; k++) {
            if (pld_found[k]) { any_found = true; break; }
        }
        if (any_found && s->pld_buf_count < 300) {
            for (int k = 0; k < 12; k++)
                s->pld_buf[s->pld_buf_count][k] = pld_found[k] ? pld_vals[k] : -1;
            s->pld_buf_count++;
        }
    }

    /* Flush if buffers are getting full */
    if (s->brp_buf_count >= 1400) flush_brp(s);
    if (s->sa2_buf_count >= 55) flush_sa2(s);
    if (s->pld_buf_count >= 280) flush_pld(s);

    xSemaphoreGive(s->mutex);
}

/* Write an event as a JSON line to events.snt */
static void write_event(session_writer_t *s, const cJSON *msg)
{
    if (!s || !s->f_events) return;

    char *json_str = cJSON_PrintUnformatted(msg);
    if (json_str) {
        fputs(json_str, s->f_events);
        fputc('\n', s->f_events);
        free(json_str);
    }
}

/* ── EDF generation task ──────────────────────────────────────────────
 *
 * EDF generation is pure CPU + SD-card I/O — no BLE interaction needed.
 * It runs in a dedicated task pinned to core 1 (the core not running the
 * NimBLE host / notif_proc_task) at low priority (5, below notif_proc's 10)
 * so it never blocks or delays stream notification processing.
 *
 * If a new therapy session starts while EDF generation is still running,
 * stream data continues to flow through notif_proc_task on core 0 without
 * any interference.  The EDF task simply reads from the completed session's
 * files and writes to /somnotrace/EDF/ — no shared state with the live
 * session writer.
 *
 * The task allocates its own stack (16KB) and self-deletes when done. */
typedef struct {
    char     session_dir[MAX_SESSION_DIR_LEN];
    char     session_id[16];
    int64_t  start_epoch_ms;
    int64_t  end_epoch_ms;
    int64_t  clock_drift_ms;
} edf_task_args_t;

/* Cleanup pointers for the previous edf_task's PSRAM stack and TCB.
 * edf_task cannot free its own stack (it's running on it), so we save
 * the pointers here and free them the next time stop_task runs. */
static StackType_t *s_prev_edf_stack = NULL;
static StaticTask_t *s_prev_edf_tcb = NULL;

static void edf_task(void *arg)
{
    ESP_LOGI(TAG, "edf_task: started on core %d", xPortGetCoreID());
    edf_task_args_t *a = (edf_task_args_t *)arg;
    if (a) {
        edf_gen_generate(a->session_dir, a->session_id,
                         a->start_epoch_ms, a->end_epoch_ms,
                         a->clock_drift_ms);
        free(a);
    } else {
        ESP_LOGE(TAG, "edf_task: NULL args");
    }
    ESP_LOGI(TAG, "edf_task: done");
    vTaskDelete(NULL);
}

/* Session stop runs in a dedicated task so that notif_proc_task is free to
 * process the GetDateTime RPC response while session_writer_stop blocks.
 *
 * After session_writer_stop() finalises the stream .snt files, this task
 * runs the post-therapy data collection pipeline:
 *
 *   1. session_writer_stop()  — final flush, close .snt files, write session.json
 *   2. post_therapy_collect() — pull Summary + TherapyEvents spools + Get RPC
 *                               → save to post-therapy/ subfolder
 *   3. edf_gen_generate()     — launched as a SEPARATE task on core 1
 *                               (see edf_task above) so it does NOT block
 *                               notif_proc_task from processing stream
 *                               notifications if a new therapy starts.
 *
 * Steps 1 and 2 run in stop_task (needs BLE for spool pulls).
 * Step 3 is dispatched to edf_task and stop_task exits immediately,
 * freeing the high-priority slot for notif_proc_task. */
static void stop_task(void *arg)
{
    session_writer_t *s = (session_writer_t *)arg;
    if (!s) {
        vTaskDelete(NULL);
        return;
    }

    /* Capture session metadata before session_writer_stop() frees s.
     * NOTE: clock_drift_ms is set INSIDE session_writer_stop() (from the
     * GetDateTime RPC), so we capture it AFTER the call.  session_writer_stop()
     * does not free s — we do it here after reading the drift. */
    char session_dir[MAX_SESSION_DIR_LEN];
    char session_id[16];

    strlcpy(session_dir, s->dir, sizeof(session_dir));
    strlcpy(session_id, s->session_id, sizeof(session_id));

    /* Step 1: Finalise stream data (flush, close files, write session.json).
     * This also queries AS11 clock and sets s->clock_drift_ms. */
    session_writer_stop(s);

    /* Now capture the values that session_writer_stop() set */
    int64_t start_epoch_ms = s->start_epoch_ms;
    int64_t end_epoch_ms = s->end_epoch_ms;
    int64_t clock_drift_ms = s->clock_drift_ms;

    /* Free the session writer struct — we have everything we need */
    free(s);

    /* Step 2: Post-therapy data collection (spool pulls + Get RPC).
     * This pulls Summary and TherapyEvents spools from the AS11 and
     * queries device identification/settings via Get RPC.  All data
     * is saved to the post-therapy/ subfolder inside the session directory.
     * This step blocks on BLE RPC responses (semaphores), allowing
     * notif_proc_task to run between RPCs.  May take 10-30 seconds. */
    ESP_LOGI(TAG, "stop_task: starting post-therapy collection");
    post_therapy_collect(session_dir, start_epoch_ms, clock_drift_ms);

    /* Step 3: Launch EDF generation on core 1 (async, non-blocking).
     * EDF generation is CPU/SD-bound only — no BLE needed.  Running it
     * on core 1 at low priority ensures notif_proc_task on core 0 can
     * continue processing stream notifications without any interference. */
    edf_task_args_t *edf_args = malloc(sizeof(edf_task_args_t));
    if (edf_args) {
        strlcpy(edf_args->session_dir, session_dir, sizeof(edf_args->session_dir));
        strlcpy(edf_args->session_id, session_id, sizeof(edf_args->session_id));
        edf_args->start_epoch_ms = start_epoch_ms;
        edf_args->end_epoch_ms = end_epoch_ms;
        edf_args->clock_drift_ms = clock_drift_ms;

        /* Free the previous edf_task's PSRAM stack and TCB.
         * The previous task has completed and self-deleted by now
         * (it ran during the previous session's stop, which was
         * at least seconds ago).  We can't free our own stack, but
         * we can free the previous one. */
        if (s_prev_edf_stack) {
            free(s_prev_edf_stack);
            s_prev_edf_stack = NULL;
        }
        if (s_prev_edf_tcb) {
            free(s_prev_edf_tcb);
            s_prev_edf_tcb = NULL;
        }

        /* Allocate task stack from PSRAM (8MB) instead of internal SRAM (~270KB).
         * Internal SRAM is shared with Wi-Fi/BLE DMA buffers and gets fragmented
         * over time, causing xTaskCreatePinnedToCore to fail even with 8MB total
         * free heap.  PSRAM stacks are supported on ESP32-S3 with
         * CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM=y.
         *
         * The StaticTask_t (TCB) must be in internal RAM (FreeRTOS requirement).
         * The stack and TCB are freed by the NEXT stop_task invocation. */
        const uint32_t edf_stack_size = 10240;
        StackType_t *edf_stack = heap_caps_malloc(edf_stack_size, MALLOC_CAP_SPIRAM);
        StaticTask_t *edf_tcb = heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL);

        if (edf_stack && edf_tcb) {
            s_prev_edf_stack = edf_stack;
            s_prev_edf_tcb = edf_tcb;
            TaskHandle_t edf_handle = xTaskCreateStaticPinnedToCore(
                edf_task, "edf_gen", edf_stack_size, edf_args, 5,
                edf_stack, edf_tcb, 1);
            if (edf_handle) {
                ESP_LOGI(TAG, "stop_task: EDF generation launched on core 1 "
                         "(stack=%u PSRAM, internal_free=%u)",
                         (unsigned)edf_stack_size,
                         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
            } else {
                ESP_LOGE(TAG, "stop_task: xTaskCreateStaticPinnedToCore failed");
                free(edf_stack); free(edf_tcb);
                s_prev_edf_stack = NULL; s_prev_edf_tcb = NULL;
                free(edf_args);
            }
        } else {
            ESP_LOGE(TAG, "stop_task: malloc failed for edf stack/tcb "
                     "(PSRAM free=%u, internal free=%u)",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
            free(edf_stack); free(edf_tcb); free(edf_args);
        }
    } else {
        ESP_LOGE(TAG, "stop_task: failed to allocate edf_task args, EDF skipped");
    }

    /* stop_task exits — notif_proc_task is now free to process any
     * new therapy notifications without competition from this task. */
    vTaskDelete(NULL);
}

void session_writer_on_notification(session_writer_t *s, const cJSON *msg)
{
    if (!msg) return;

    cJSON *method = cJSON_GetObjectItem(msg, "method");
    const char *method_str = method ? method->valuestring : NULL;

    if (!method_str) return;

    ESP_LOGD(TAG, "notification: %s", method_str);

    /* Handle EventNotification: check for TherapyStart/TherapyStop */
    if (strcmp(method_str, "EventNotification") == 0) {
        bool start = false, stop = false;
        check_event_notification(msg, &start, &stop);

        /* Handle stop first so that a single message containing both
         * TherapyStart + TherapyStop (quick start/stop) doesn't leave
         * the display stuck in graph mode. */
        if (stop) {
            ESP_LOGI(TAG, ">>> THERAPY STOP detected");
            s_therapy_stopped = true;
            bsp_display_set_therapy_active(false);
            if (s && s->active) {
                write_event(s, msg);
                /* Run stop in a separate task so notif_proc_task can process
                 * the GetDateTime RPC response while session_writer_stop blocks.
                 * Stack 8KB: stop_task does session finalisation + post-therapy
                 * spool pulls (BLE I/O, not CPU-heavy).  EDF generation runs
                 * in a separate task on core 1 (see edf_task). */
                xTaskCreate(stop_task, "session_stop", 8192, s, 15, NULL);
                s = NULL;
            }
        }
        if (start) {
            ESP_LOGI(TAG, ">>> THERAPY START detected");
            s_therapy_stopped = false;
            bsp_display_set_therapy_active(true);
            if (!s || !s->active) {
                s = session_writer_start();
            }
            if (s) write_event(s, msg);
        }
        if (start || stop) return;

        /* Other events (MaskOn, MaskOff, etc.) — write if session active */
        if (s && s->active) write_event(s, msg);
        return;
    }

    /* StreamData is handled by the fast-path (session_writer_on_stream_data_raw)
     * in as11_ble.c before cJSON parse. If we reach here, it's a non-StreamData
     * notification — write as event if session active. */
    if (s && s->active) {
        write_event(s, msg);
    }
}

/* ── Crash recovery ───────────────────────────────────────────────── */

void session_writer_recover(void)
{
    if (!sd_storage_is_ready()) return;

    DIR *dir = opendir(SD_SESSIONS_DIR);
    if (!dir) return;

    struct dirent *ent;
    int recovered = 0;

    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') continue;

        char session_path[300];
        snprintf(session_path, sizeof(session_path), "%s/%s", SD_SESSIONS_DIR, ent->d_name);

        char json_path[330];
        snprintf(json_path, sizeof(json_path), "%s/session.json", session_path);

        struct stat st;
        if (stat(json_path, &st) == 0) {
            /* session.json exists — already finalised */
            continue;
        }

        ESP_LOGW(TAG, "found interrupted session: %s — writing metadata", ent->d_name);

        /* Write session.json directly without allocating a full session_writer_t
         * (which is ~7KB and would overflow the stack) */
        char path[330];

        /* Read sample counts and start_epoch_ms from .snt headers */
        uint32_t brp_samples = 0, sa2_samples = 0, pld_samples = 0, brp_mm_samples = 0;
        int64_t start_epoch_ms = 0;
        snt_header_t hdr;

        snprintf(path, sizeof(path), "%s/brp.snt", session_path);
        FILE *f = fopen(path, "rb");
        if (f) {
            if (fread(&hdr, 1, SNT_HEADER_SIZE, f) == SNT_HEADER_SIZE && hdr.magic == SNT_MAGIC) {
                brp_samples = hdr.sample_count;
                start_epoch_ms = hdr.start_epoch_ms;
            }
            fclose(f);
        }

        snprintf(path, sizeof(path), "%s/brp_mm.snt", session_path);
        f = fopen(path, "rb");
        if (f) {
            if (fread(&hdr, 1, SNT_HEADER_SIZE, f) == SNT_HEADER_SIZE && hdr.magic == SNT_MAGIC)
                brp_mm_samples = hdr.sample_count;
            fclose(f);
        }

        snprintf(path, sizeof(path), "%s/sa2.snt", session_path);
        f = fopen(path, "rb");
        if (f) {
            if (fread(&hdr, 1, SNT_HEADER_SIZE, f) == SNT_HEADER_SIZE && hdr.magic == SNT_MAGIC)
                sa2_samples = hdr.sample_count;
            fclose(f);
        }

        snprintf(path, sizeof(path), "%s/pld.snt", session_path);
        f = fopen(path, "rb");
        if (f) {
            if (fread(&hdr, 1, SNT_HEADER_SIZE, f) == SNT_HEADER_SIZE && hdr.magic == SNT_MAGIC)
                pld_samples = hdr.sample_count;
            fclose(f);
        }

        /* Write JSON manually to avoid cJSON heap usage on the stack-limited task */
        f = fopen(json_path, "w");
        if (f) {
            fprintf(f, "{\"id\":\"%s\",\"state\":\"interrupted\","
                       "\"start_epoch_ms\":%lld,"
                       "\"brp_samples\":%u,\"brp_mm_samples\":%u,"
                       "\"sa2_samples\":%u,\"pld_samples\":%u",
                    ent->d_name,
                    (long long)start_epoch_ms,
                    (unsigned)brp_samples, (unsigned)brp_mm_samples,
                    (unsigned)sa2_samples, (unsigned)pld_samples);

            /* Add start_iso if we have a valid epoch */
            if (start_epoch_ms > 0) {
                time_t start = (time_t)(start_epoch_ms / 1000);
                struct tm tm;
                localtime_r(&start, &tm);
                char iso_start[32];
                strftime(iso_start, sizeof(iso_start), "%Y-%m-%dT%H:%M:%S", &tm);
                fprintf(f, ",\"start_iso\":\"%s\"", iso_start);
            }

            if (s_device_addr[0]) fprintf(f, ",\"as11_device\":\"%s\"", s_device_addr);
            if (s_client_id[0]) fprintf(f, ",\"as11_client_id\":\"%s\"", s_client_id);
            fprintf(f, "}\n");
            fclose(f);
            ESP_LOGI(TAG, "wrote %s (state=interrupted)", json_path);
        }

        recovered++;
    }

    closedir(dir);
    if (recovered > 0) {
        ESP_LOGI(TAG, "recovered %d interrupted session(s)", recovered);
    }
}
