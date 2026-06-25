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

    stream_files_t brp;     /* 25 Hz, 2 ch: flow + mask_pressure */
    stream_files_t sa2;     /* 5 Hz, 2 ch: heart_rate + spo2 (decimated from 25 Hz) */
    stream_files_t pld;     /* 5 Hz, 12 ch (decimated from 25 Hz) */
    FILE    *f_events;
    uint32_t brp_mm_count;  /* L1 MinMax records written */

    int64_t  clock_drift_ms;   /* NTP time - AS11 device time at session stop */

    /* recent buffer: holds data between flushes */
    SemaphoreHandle_t mutex;

    /* BRP recent buffer (largest, 25 Hz × 2 ch × 60 s = 3000 samples) */
    int16_t brp_flow[1500];
    int16_t brp_press[1500];
    uint32_t brp_buf_count;     /* samples in buffer (per channel) */

    /* SA2 recent buffer (5 Hz × 2 ch × 60 s = 300 samples) */
    int16_t sa2_hr[300];
    int16_t sa2_spo2[300];
    uint32_t sa2_buf_count;

    /* PLD recent buffer (5 Hz × 12 ch × 60 s = 300 samples) */
    int16_t pld_buf[300][12];
    uint32_t pld_buf_count;

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

static void write_snt_header(FILE *f, uint8_t tier, uint16_t rate_hz,
                             uint8_t n_ch, int64_t start_epoch_ms)
{
    snt_header_t hdr = {
        .magic = SNT_MAGIC,
        .version = SNT_VERSION,
        .tier = tier,
        .n_channels = n_ch,
        .sample_bytes = 2,
        .sample_hz_x10 = rate_hz * 10,
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
            int16_t fmn = INT16_MAX, fmx = INT16_MIN;
            int16_t pmn = INT16_MAX, pmx = INT16_MIN;
            for (int j = 0; j < 25; j++) {
                int16_t fv = s->brp_flow[base + j];
                int16_t pv = s->brp_press[base + j];
                if (fv != INT16_MIN) {
                    if (fv < fmn) fmn = fv;
                    if (fv > fmx) fmx = fv;
                }
                if (pv != INT16_MIN) {
                    if (pv < pmn) pmn = pv;
                    if (pv > pmx) pmx = pv;
                }
            }
            if (fmn == INT16_MAX) { fmn = fmx = INT16_MIN; }
            if (pmn == INT16_MAX) { pmn = pmx = INT16_MIN; }
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
        vTaskDelay(pdMS_TO_TICKS(FLUSH_INTERVAL_SEC * 1000));
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
        ESP_LOGW(TAG, "session already active, stopping previous");
        session_writer_stop(s_active);
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
    if (s->brp.f_l0) write_snt_header(s->brp.f_l0, 0, 25, 2, start_epoch_ms);

    snprintf(path, sizeof(path), "%s/brp_mm.snt", s->dir);
    s->brp.f_l1 = fopen(path, "wb");
    if (s->brp.f_l1) write_snt_header(s->brp.f_l1, 1, 1, 4, start_epoch_ms);

    snprintf(path, sizeof(path), "%s/sa2.snt", s->dir);
    s->sa2.f_l0 = fopen(path, "wb");
    if (s->sa2.f_l0) write_snt_header(s->sa2.f_l0, 0, 5, 2, start_epoch_ms);

    snprintf(path, sizeof(path), "%s/pld.snt", s->dir);
    s->pld.f_l0 = fopen(path, "wb");
    if (s->pld.f_l0) write_snt_header(s->pld.f_l0, 0, 5, 12, start_epoch_ms);

    snprintf(path, sizeof(path), "%s/events.snt", s->dir);
    s->f_events = fopen(path, "w");
    if (!s->f_events) {
        ESP_LOGW(TAG, "failed to open events.snt (non-fatal)");
    }

    if (!s->brp.f_l0 || !s->sa2.f_l0 || !s->pld.f_l0) {
        ESP_LOGE(TAG, "failed to open .snt files");
        session_writer_stop(s);
        xSemaphoreGive(s_active_mutex);
        return NULL;
    }

    s_active = s;
    xSemaphoreGive(s_active_mutex);

    /* Start flush task */
    xTaskCreate(flush_task, "session_flush", FLUSH_TASK_STACK, s, 5, NULL);

    ESP_LOGI(TAG, "=== SESSION STARTED: %s ===", s->session_id);
    ESP_LOGI(TAG, "dir: %s", s->dir);

    return s;
}

esp_err_t session_writer_stop(session_writer_t *s)
{
    if (!s) return ESP_OK;

    xSemaphoreTake(s_active_mutex, portMAX_DELAY);

    s->active = false;
    s->end_time_us = esp_timer_get_time();
    s->end_epoch_ms = (int64_t)time(NULL) * 1000;

    /* Stop the AS11 data stream (no-op — stream continues between sessions) */
    as11_ble_stop_stream();

    /* Final flush */
    flush_all(s);

    /* Query AS11 clock at session stop for fresh drift measurement.
     * This works because stop_task runs in a separate context from
     * notif_proc_task, which can process the GetDateTime response. */
    int64_t as11_ms = 0;
    if (as11_ble_get_datetime(&as11_ms) == ESP_OK) {
        s->clock_drift_ms = s->end_epoch_ms - as11_ms;
        ESP_LOGI(TAG, "clock_drift_ms = %lld (stop-time, NTP=%lld AS11=%lld)",
                 (long long)s->clock_drift_ms,
                 (long long)s->end_epoch_ms,
                 (long long)as11_ms);
    } else {
        int64_t drift = 0;
        if (as11_ble_get_clock_drift(&drift) == ESP_OK) {
            s->clock_drift_ms = drift;
            ESP_LOGI(TAG, "clock_drift_ms = %lld (pre-stream fallback)",
                     (long long)s->clock_drift_ms);
        } else {
            ESP_LOGW(TAG, "clock_drift_ms unavailable");
        }
    }

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
    free(s);

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

/* Convert a float sample to int16_t with clamping.
 * AS11 sends flow in L/min, pressure in cmH2O, etc. as floats.
 * We scale by 100 to preserve 2 decimal places in int16_t. */
static int16_t float_to_s16(double val)
{
    double scaled = val * 100.0;
    if (scaled > 32767.0) return 32767;
    if (scaled < -32768.0) return -32768;
    return (int16_t)scaled;
}

/* Parse a StreamData notification and route samples to the correct stream buffers.
 * StreamData format:
 *   params.data[] = array of objects, each { "StreamName": [samples] }
 *   e.g. { "PatientFlow-100hz": [0.05, 0.03, ...], "MaskPressure-100hz": [...] }
 *
 * BRP (25 Hz): PatientFlow-100hz, MaskPressure-100hz
 * SA2 (5 Hz):  HeartRate, SpO2 (decimated: 1 sample per 200ms report)
 * PLD (5 Hz): MaskPressure-TwoSecond, InspiratoryPressure-TwoSecond,
 *   ExpiratoryPressure-TwoSecond, Leak-50hz, RespiratoryRate-50hz,
 *   TidalVolume-50hz, MinuteVentilation-50hz, SnoreIndex-50hz, FlowLimitation-50hz
 */
static void parse_stream_data(session_writer_t *s, const cJSON *msg)
{
    if (!s || !s->active) return;

    cJSON *params = cJSON_GetObjectItem(msg, "params");
    if (!params) return;

    cJSON *data = cJSON_GetObjectItem(params, "data");
    if (!data || !cJSON_IsArray(data)) return;

    int n_items = cJSON_GetArraySize(data);
    bool pld_record_started = false;
    for (int i = 0; i < n_items; i++) {
        cJSON *item = cJSON_GetArrayItem(data, i);
        if (!item || !cJSON_IsObject(item)) continue;

        cJSON *child = NULL;
        cJSON_ArrayForEach(child, item) {
            const char *name = child->string;
            if (!name || !cJSON_IsArray(child)) continue;

            int n_samples = cJSON_GetArraySize(child);
            if (n_samples <= 0) continue;

            /* Route to the correct buffer based on stream name */
            if (strcmp(name, "PatientFlow-100hz") == 0) {
                for (int j = 0; j < n_samples && s->brp_buf_count < 1500; j++) {
                    cJSON *v = cJSON_GetArrayItem(child, j);
                    if (v && cJSON_IsNumber(v)) {
                        s->brp_flow[s->brp_buf_count] = float_to_s16(v->valuedouble);
                    } else {
                        s->brp_flow[s->brp_buf_count] = INT16_MIN;
                    }
                    s->brp_buf_count++;
                }
            } else if (strcmp(name, "MaskPressure-100hz") == 0) {
                /* BRP pressure is paired with flow — fill the same index */
                for (int j = 0; j < n_samples; j++) {
                    /* Ensure brp_buf_count is at least j+1 (flow should arrive first) */
                    if (s->brp_buf_count <= j) {
                        /* Flow wasn't seen yet or fewer samples — pad with sentinel */
                        if (s->brp_buf_count < 1500) {
                            s->brp_flow[s->brp_buf_count] = INT16_MIN;
                            s->brp_buf_count++;
                        }
                    }
                    if (j < 1500) {
                        cJSON *v = cJSON_GetArrayItem(child, j);
                        if (v && cJSON_IsNumber(v))
                            s->brp_press[j] = float_to_s16(v->valuedouble);
                        else
                            s->brp_press[j] = INT16_MIN;
                    }
                }
            } else if (strcmp(name, "HeartRate") == 0) {
                /* SA2: take only the last sample (value-held by device at 25 Hz,
                 * updates at 1 Hz). One sample per 200ms report = 5 Hz. */
                if (s->sa2_buf_count < 300) {
                    cJSON *v = cJSON_GetArrayItem(child, n_samples - 1);
                    if (v && cJSON_IsNumber(v))
                        s->sa2_hr[s->sa2_buf_count] = float_to_s16(v->valuedouble);
                    else
                        s->sa2_hr[s->sa2_buf_count] = INT16_MIN;
                    s->sa2_buf_count++;
                }
            } else if (strcmp(name, "SpO2") == 0) {
                /* SA2: fill spo2 for the current sample index */
                if (s->sa2_buf_count > 0) {
                    int idx = s->sa2_buf_count - 1;
                    cJSON *v = cJSON_GetArrayItem(child, n_samples - 1);
                    if (v && cJSON_IsNumber(v))
                        s->sa2_spo2[idx] = float_to_s16(v->valuedouble);
                    else
                        s->sa2_spo2[idx] = INT16_MIN;
                } else if (s->sa2_buf_count < 300) {
                    /* SpO2 arrived before HeartRate — create the sample */
                    s->sa2_hr[s->sa2_buf_count] = INT16_MIN;
                    cJSON *v = cJSON_GetArrayItem(child, n_samples - 1);
                    if (v && cJSON_IsNumber(v))
                        s->sa2_spo2[s->sa2_buf_count] = float_to_s16(v->valuedouble);
                    else
                        s->sa2_spo2[s->sa2_buf_count] = INT16_MIN;
                    s->sa2_buf_count++;
                }
            } else {
                /* PLD channels: route to pld_buf by name index */
                static const char *pld_names[] = {
                    "MaskPressure-TwoSecond", "InspiratoryPressure-TwoSecond",
                    "ExpiratoryPressure-TwoSecond", "Leak-50hz",
                    "RespiratoryRate-50hz", "TidalVolume-50hz",
                    "MinuteVentilation-50hz", "SnoreIndex-50hz",
                    "FlowLimitation-50hz",
                };
                int ch_idx = -1;
                for (int k = 0; k < 9; k++) {
                    if (strcmp(name, pld_names[k]) == 0) { ch_idx = k; break; }
                }
                if (ch_idx >= 0) {
                    /* PLD: take only the last sample (value-held by device,
                     * updates at 0.5 Hz). One sample per 200ms report = 5 Hz. */
                    if (s->pld_buf_count < 300) {
                        if (!pld_record_started) {
                            /* First PLD channel for this notification — init record */
                            for (int k = 0; k < 12; k++)
                                s->pld_buf[s->pld_buf_count][k] = INT16_MIN;
                            pld_record_started = true;
                        }
                        cJSON *v = cJSON_GetArrayItem(child, n_samples - 1);
                        if (v && cJSON_IsNumber(v))
                            s->pld_buf[s->pld_buf_count][ch_idx] = float_to_s16(v->valuedouble);
                    }
                }
            }
        }
    }

    /* Advance PLD record count once per notification (after all channels) */
    if (pld_record_started) {
        s->pld_buf_count++;
    }

    /* Flush if buffers are getting full */
    if (s->brp_buf_count >= 1400) flush_brp(s);
    if (s->sa2_buf_count >= 280) flush_sa2(s);
    if (s->pld_buf_count >= 280) flush_pld(s);
}

/* Check if PatientFlow values are non-trivial (therapy active without event).
 * Used for reboot-mid-therapy detection.
 * Threshold: any sample with absolute value > 0.5 L/min indicates real therapy. */
static bool stream_data_has_active_flow(const cJSON *msg)
{
    cJSON *params = cJSON_GetObjectItem(msg, "params");
    if (!params) return false;
    cJSON *data = cJSON_GetObjectItem(params, "data");
    if (!data || !cJSON_IsArray(data)) return false;

    int n = cJSON_GetArraySize(data);
    for (int i = 0; i < n; i++) {
        cJSON *item = cJSON_GetArrayItem(data, i);
        if (!item) continue;
        cJSON *flow = cJSON_GetObjectItem(item, "PatientFlow-100hz");
        if (flow && cJSON_IsArray(flow)) {
            int ns = cJSON_GetArraySize(flow);
            for (int j = 0; j < ns; j++) {
                cJSON *v = cJSON_GetArrayItem(flow, j);
                if (v && cJSON_IsNumber(v) && fabs(v->valuedouble) > 0.5) {
                    return true;
                }
            }
        }
    }
    return false;
}

/* ── Fast-path StreamData parser (bypasses cJSON) ─────────────────── */

/* Find a JSON string key `"key"` in json[0..len), return position after
 * the colon following the key, or NULL if not found. */
static const char *find_json_key(const char *json, int len, const char *key)
{
    int klen = strlen(key);
    for (int i = 0; i + klen + 3 < len; i++) {
        if (json[i] == '"') {
            if (strncmp(json + i + 1, key, klen) == 0 && json[i + 1 + klen] == '"') {
                /* Expect a colon after the closing quote (with optional whitespace) */
                int j = i + 1 + klen + 1;
                while (j < len && (json[j] == ' ' || json[j] == '\t')) j++;
                if (j < len && json[j] == ':') {
                    return json + j + 1;  /* position after colon */
                }
            }
        }
    }
    return NULL;
}

/* Parse a JSON number array starting at *p (pointing at '[' or first digit).
 * Stores up to max_out doubles in out, returns count. Advances *p past the
 * closing ']'. */
static int parse_json_array(const char **p, const char *end, double *out, int max_out)
{
    const char *s = *p;
    while (s < end && *s != '[') s++;
    if (s >= end || *s != '[') { *p = s; return 0; }
    s++;  /* skip '[' */
    int count = 0;
    while (s < end && *s != ']' && count < max_out) {
        while (s < end && (*s == ' ' || *s == ',' || *s == '\t' || *s == '\n')) s++;
        if (s >= end || *s == ']') break;
        char *endptr;
        out[count] = strtod(s, &endptr);
        if (endptr == s) { s++; continue; }  /* skip unparseable */
        count++;
        s = endptr;
    }
    while (s < end && *s != ']') s++;
    if (s < end && *s == ']') s++;
    *p = s;
    return count;
}

/* Fast-path: process StreamData from raw JSON without cJSON.
 * Handles: display flow push, active-flow detection, sample routing. */
void session_writer_on_stream_data_raw(const char *json, int len)
{
    session_writer_t *s = session_writer_get_active();

    /* Push flow to display and detect active flow */
    bool has_active_flow = false;
    const char *end = json + len;

    /* Find "PatientFlow-100hz" array */
    const char *flow_key = find_json_key(json, len, "PatientFlow-100hz");
    if (flow_key) {
        const char *fp = flow_key;
        double vals[32];
        int n = parse_json_array(&fp, end, vals, 32);
        for (int j = 0; j < n; j++) {
            bsp_display_push_flow(vals[j]);
            if (fabs(vals[j]) > 0.5) has_active_flow = true;
        }
    }

    /* Edge case: auto-start session if flow is non-trivial and no session active */
    if ((!s || !session_writer_is_active(s)) && !s_therapy_stopped && has_active_flow) {
        ESP_LOGI(TAG, ">>> THERAPY detected via non-zero flow (reboot mid-therapy?)");
        bsp_display_set_therapy_active(true);
        s = session_writer_start();
    }

    if (!s || !session_writer_is_active(s)) return;

    /* Route samples to buffers — same logic as parse_stream_data but from raw JSON */
    xSemaphoreTake(s->mutex, portMAX_DELAY);

    bool pld_record_started = false;

    /* BRP: PatientFlow */
    {
        const char *kp = find_json_key(json, len, "PatientFlow-100hz");
        if (kp) {
            double vals[32];
            int n = parse_json_array(&kp, end, vals, 32);
            for (int j = 0; j < n && s->brp_buf_count < 1500; j++) {
                s->brp_flow[s->brp_buf_count] = float_to_s16(vals[j]);
                s->brp_buf_count++;
            }
        }
    }

    /* BRP: MaskPressure-100hz (paired with flow) */
    {
        const char *kp = find_json_key(json, len, "MaskPressure-100hz");
        if (kp) {
            double vals[32];
            int n = parse_json_array(&kp, end, vals, 32);
            for (int j = 0; j < n; j++) {
                if (s->brp_buf_count <= j) {
                    if (s->brp_buf_count < 1500) {
                        s->brp_flow[s->brp_buf_count] = INT16_MIN;
                        s->brp_buf_count++;
                    }
                }
                if (j < 1500) {
                    s->brp_press[j] = float_to_s16(vals[j]);
                }
            }
        }
    }

    /* SA2: HeartRate (take last sample) */
    {
        const char *kp = find_json_key(json, len, "HeartRate");
        if (kp) {
            double vals[32];
            int n = parse_json_array(&kp, end, vals, 32);
            if (n > 0 && s->sa2_buf_count < 300) {
                s->sa2_hr[s->sa2_buf_count] = float_to_s16(vals[n - 1]);
                s->sa2_buf_count++;
            }
        }
    }

    /* SA2: SpO2 (fill current index) */
    {
        const char *kp = find_json_key(json, len, "SpO2");
        if (kp) {
            double vals[32];
            int n = parse_json_array(&kp, end, vals, 32);
            if (n > 0) {
                if (s->sa2_buf_count > 0) {
                    s->sa2_spo2[s->sa2_buf_count - 1] = float_to_s16(vals[n - 1]);
                } else if (s->sa2_buf_count < 300) {
                    s->sa2_hr[s->sa2_buf_count] = INT16_MIN;
                    s->sa2_spo2[s->sa2_buf_count] = float_to_s16(vals[n - 1]);
                    s->sa2_buf_count++;
                }
            }
        }
    }

    /* PLD channels: take last sample of each */
    {
        static const char *pld_names[] = {
            "MaskPressure-TwoSecond", "InspiratoryPressure-TwoSecond",
            "ExpiratoryPressure-TwoSecond", "Leak-50hz",
            "RespiratoryRate-50hz", "TidalVolume-50hz",
            "MinuteVentilation-50hz", "SnoreIndex-50hz",
            "FlowLimitation-50hz",
        };
        for (int k = 0; k < 9; k++) {
            const char *kp = find_json_key(json, len, pld_names[k]);
            if (kp) {
                double vals[32];
                int n = parse_json_array(&kp, end, vals, 32);
                if (n > 0 && s->pld_buf_count < 300) {
                    if (!pld_record_started) {
                        for (int kk = 0; kk < 12; kk++)
                            s->pld_buf[s->pld_buf_count][kk] = INT16_MIN;
                        pld_record_started = true;
                    }
                    s->pld_buf[s->pld_buf_count][k] = float_to_s16(vals[n - 1]);
                }
            }
        }
    }

    if (pld_record_started) {
        s->pld_buf_count++;
    }

    /* Flush if buffers are getting full */
    if (s->brp_buf_count >= 1400) flush_brp(s);
    if (s->sa2_buf_count >= 280) flush_sa2(s);
    if (s->pld_buf_count >= 280) flush_pld(s);

    xSemaphoreGive(s->mutex);
}

/* Push PatientFlow samples from a StreamData notification to the display graph.
 * Called regardless of whether the session is fully started — the display
 * should show the breathing waveform as soon as therapy data arrives. */
static void stream_data_push_flow(const cJSON *msg)
{
    cJSON *params = cJSON_GetObjectItem(msg, "params");
    if (!params) return;
    cJSON *data = cJSON_GetObjectItem(params, "data");
    if (!data || !cJSON_IsArray(data)) return;

    int n = cJSON_GetArraySize(data);
    for (int i = 0; i < n; i++) {
        cJSON *item = cJSON_GetArrayItem(data, i);
        if (!item) continue;
        cJSON *flow = cJSON_GetObjectItem(item, "PatientFlow-100hz");
        if (flow && cJSON_IsArray(flow)) {
            int ns = cJSON_GetArraySize(flow);
            for (int j = 0; j < ns; j++) {
                cJSON *v = cJSON_GetArrayItem(flow, j);
                if (v && cJSON_IsNumber(v)) {
                    bsp_display_push_flow(v->valuedouble);
                }
            }
        }
    }
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

/* Session stop runs in a dedicated task so that notif_proc_task is free to
 * process the GetDateTime RPC response while session_writer_stop blocks. */
static void stop_task(void *arg)
{
    session_writer_t *s = (session_writer_t *)arg;
    if (s) {
        session_writer_stop(s);
    }
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
                 * the GetDateTime RPC response while session_writer_stop blocks. */
                xTaskCreate(stop_task, "session_stop", 8192, s, 5, NULL);
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

    /* Handle StreamData: route samples to buffers */
    if (strcmp(method_str, "StreamData") == 0) {
        /* Push flow samples to display graph regardless of session state */
        stream_data_push_flow(msg);

        /* Edge case: if no session active but flow is non-trivial,
         * therapy may have started before reboot — auto-start a session.
         * Skip if we recently saw a TherapyStop (residual flow after stop). */
        if (!s || !s->active) {
            if (!s_therapy_stopped && stream_data_has_active_flow(msg)) {
                ESP_LOGI(TAG, ">>> THERAPY detected via non-zero flow (reboot mid-therapy?)");
                bsp_display_set_therapy_active(true);
                s = session_writer_start();
            }
        }
        if (s && s->active) {
            xSemaphoreTake(s->mutex, portMAX_DELAY);
            parse_stream_data(s, msg);
            xSemaphoreGive(s->mutex);
        }
        return;
    }

    /* Other notifications: write as events if session active */
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
