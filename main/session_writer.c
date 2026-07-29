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
#include "uploader.h"

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
#include "psram_task.h"

static const char *TAG = "session";

/* ── .snt file format ─────────────────────────────────────────────── */

#define SNT_MAGIC       0x534E5442u   /* "SNTB" */
#define SNT_VERSION     2
#define SNT_MISSING     INT16_MIN      /* v2 unambiguous missing-data sentinel */

typedef struct __attribute__((packed)) {
    uint32_t magic;            /* 0x534E5442 "SNTB"                  */
    uint8_t  version;          /* format version (2)                  */
    uint8_t  tier;             /* 0 = L0 raw, 1 = L1 MinMax          */
    uint8_t  n_channels;       /* channels per record                 */
    uint8_t  sample_bytes;     /* 2 (int16)                           */
    uint16_t sample_hz_x10;   /* rate × 10 (250 = 25 Hz)            */
    uint16_t reserved;
    int64_t  start_epoch_ms;  /* session start (NTP clock)           */
    uint32_t sample_count;    /* records written (updated each flush) */
    uint32_t reserved2;
} snt_header_t;              /* 28 bytes (packed) */

#define SNT_HEADER_SIZE  sizeof(snt_header_t)   /* 28 bytes (packed) */

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
    char     dir[MAX_SESSION_DIR_LEN];   /* noon-day folder path */
    char     session_id[32];      /* YYYYMMDD_HHMMSS[_n] — file prefix */
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
    bool     clock_drift_valid; /* true when clock_drift_ms was successfully queried */

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

    /* ── Missing-packet compensation ────────────────────────────────
     * StreamData notifications include a "startTime" ISO8601 timestamp
     * for the first sample in each report.  Under normal operation the
     * gap between consecutive notifications is ~200ms (reportIntervalMs).
     * A dropped BLE notification produces a gap of ~400ms+, which we
     * detect by comparing startTime values.
     *
     * Compensation strategy (per signal type):
     *   BRP (25Hz waveform): insert SNT_MISSING sentinels — fabricated data
     *     could mimic apnea/hypopnea, so missing data must be visible.
     *   SA2 (1Hz vitals):    hold previous value — HR/SpO2 change over
     *     minutes; a 200ms-1s gap is clinically negligible.
     *   PLD (0.5Hz metrics):  hold previous value — all PLD signals are
     *     2-second summaries that change slowly; holding is a tiny error.
     *
     * See: https://github.com/ilyakruchinin/SomnoTrace/issues/20 */
    int64_t  prev_stream_ms;     /* ms-since-midnight from last startTime */
    bool     prev_stream_ms_valid;  /* false until first notification */
    int16_t  last_hr;            /* last known HR for hold-on-gap */
    int16_t  last_spo2;          /* last known SpO2 for hold-on-gap */
    int16_t  last_pld[12];       /* last known PLD row for hold-on-gap */
    bool     last_hr_valid;      /* true after first HR sample received */
    bool     last_spo2_valid;
    bool     last_pld_valid;

    /* Session-level gap statistics (logged at session stop) */
    uint32_t stream_notifications;  /* total StreamData notifications received */
    uint32_t gap_events;            /* number of gap detections (compensated) */
    uint32_t gap_missing_total;     /* total missing notifications compensated */

    /* events: simple JSON line file, flushed with data */
};

static session_writer_t *s_active = NULL;
static SemaphoreHandle_t s_active_mutex = NULL;
/* Set to true on TherapyStop, false on TherapyStart.
 * Used by the auto-start logic in session_writer_on_stream_data_raw to
 * prevent starting a new session from flow/pressure data alone when therapy
 * has been explicitly stopped.  Without this, the residual pressure decay
 * after TherapyStop could trigger a spurious auto-start. */
static bool s_therapy_stopped = false;

/* _SNC (Summary update counter) tracking — set when ValueChange
 * notification is received via the _SNC event subscription. */
static volatile bool s_snc_changed = false;
static volatile int64_t s_snc_value = -1;

static char s_device_addr[32] = {0};
static char s_client_id[64] = {0};

/* ── Helpers ──────────────────────────────────────────────────────── */

/* Compute the noon-based day folder (YYYYMMDD) from local time.
 * Sessions before noon belong to the previous day's folder. */
static void noon_day_folder_local(time_t t, char *out, size_t out_len)
{
    struct tm tm;
    localtime_r(&t, &tm);
    if (tm.tm_hour < 12) {
        t -= 86400;
        localtime_r(&t, &tm);
    }
    snprintf(out, out_len, "%04d%02d%02d",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
}

/* Generate session timestamp prefix: YYYYMMDD_HHMMSS */
static void make_session_id(char *out, size_t out_len)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    snprintf(out, out_len, "%04d%02d%02d_%02d%02d%02d",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
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
            int16_t fmn = INT16_MAX, fmx = INT16_MIN;
            int16_t pmn = INT16_MAX, pmx = INT16_MIN;
            for (int j = 0; j < 25; j++) {
                int16_t fv = s->brp_flow[base + j];
                int16_t pv = s->brp_press[base + j];
                if (fv != SNT_MISSING) {
                    if (fv < fmn) fmn = fv;
                    if (fv > fmx) fmx = fv;
                }
                if (pv != SNT_MISSING) {
                    if (pv < pmn) pmn = pv;
                    if (pv > pmx) pmx = pv;
                }
            }
            if (fmn == INT16_MAX) { fmn = fmx = SNT_MISSING; }
            if (pmn == INT16_MAX) { pmn = pmx = SNT_MISSING; }
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
    char path[MAX_SESSION_DIR_LEN + 48];
    snprintf(path, sizeof(path), "%s/%s_session.json", s->dir, s->session_id);

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

    cJSON_AddNumberToObject(root, "clock_drift_ms", (double)s->clock_drift_ms);
    cJSON_AddBoolToObject(root, "clock_drift_valid", s->clock_drift_valid);

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

    /* Stop any previously active session BEFORE creating a new one.
     * We must NOT hold s_active_mutex while calling session_writer_stop(),
     * because session_writer_stop() itself takes s_active_mutex — using a
     * regular (non-recursive) mutex would deadlock.
     *
     * Instead: grab s_active under the mutex, clear it, release the mutex,
     * then stop the previous session without holding the lock. */
    xSemaphoreTake(s_active_mutex, portMAX_DELAY);
    session_writer_t *prev = s_active;
    s_active = NULL;
    xSemaphoreGive(s_active_mutex);

    if (prev) {
        ESP_LOGW(TAG, "session already active, stopping previous");
        session_writer_stop(prev);
        free(prev);
    }

    session_writer_t *s = calloc(1, sizeof(session_writer_t));
    if (!s) {
        return NULL;
    }

    s->mutex = xSemaphoreCreateMutex();
    if (!s->mutex) {
        free(s);
        return NULL;
    }

    s->pld_countdown = 1;  /* first notification writes PLD immediately */
    s->sa2_countdown = 1;  /* first notification writes SA2 immediately */

    /* Missing-packet compensation: no previous startTime until first
     * notification arrives; no cached values to hold until first sample. */
    s->prev_stream_ms_valid = false;
    s->last_hr_valid = false;
    s->last_spo2_valid = false;
    s->last_pld_valid = false;
    s->stream_notifications = 0;
    s->gap_events = 0;
    s->gap_missing_total = 0;

    make_session_id(s->session_id, sizeof(s->session_id));

    /* Compute noon-day folder (YYYYMMDD) from local time.
     * Sessions before noon belong to the previous day's folder. */
    char noon_day[16];
    noon_day_folder_local(time(NULL), noon_day, sizeof(noon_day));

    /* Create the noon-day folder under .somnotrace/sessions/streams/ */
    snprintf(s->dir, sizeof(s->dir), "%s/%s", SD_STREAMS_DIR, noon_day);
    if (mkdir(s->dir, 0775) != 0 && errno != EEXIST) {
        ESP_LOGE(TAG, "failed to create noon-day dir %s: %s", s->dir, strerror(errno));
        free(s);
        return NULL;
    }

    /* Check for filename collision (DST fallback — same local timestamp
     * occurs twice). If any file with this prefix exists, append _2, _3, etc. */
    {
        char check_path[MAX_SESSION_DIR_LEN + 48];
        snprintf(check_path, sizeof(check_path), "%s/%s_brp.snt", s->dir, s->session_id);
        struct stat st;
        if (stat(check_path, &st) == 0) {
            for (int suffix = 2; suffix < 100; suffix++) {
                snprintf(check_path, sizeof(check_path), "%s/%s_%d_brp.snt",
                         s->dir, s->session_id, suffix);
                if (stat(check_path, &st) != 0) {
                    char sid_with_suffix[40];
                    snprintf(sid_with_suffix, sizeof(sid_with_suffix),
                             "%s_%d", s->session_id, suffix);
                    strlcpy(s->session_id, sid_with_suffix, sizeof(s->session_id));
                    break;
                }
            }
            if (stat(check_path, &st) == 0) {
                ESP_LOGE(TAG, "cannot find unique session prefix for %s", s->session_id);
                free(s);
                return NULL;
            }
        }
    }

    s->start_time_us = esp_timer_get_time();
    s->start_epoch_ms = (int64_t)time(NULL) * 1000;
    s->active = true;

    /* Open files and write headers.
     * Files are flat in the noon-day folder, prefixed with the session timestamp. */
    char path[MAX_SESSION_DIR_LEN + 48];
    int64_t start_epoch_ms = s->start_epoch_ms;

    snprintf(path, sizeof(path), "%s/%s_brp.snt", s->dir, s->session_id);
    s->brp.f_l0 = fopen(path, "wb");
    if (s->brp.f_l0) write_snt_header(s->brp.f_l0, 0, 250, 2, start_epoch_ms);  /* 25 Hz */

    snprintf(path, sizeof(path), "%s/%s_brp_mm.snt", s->dir, s->session_id);
    s->brp.f_l1 = fopen(path, "wb");
    if (s->brp.f_l1) write_snt_header(s->brp.f_l1, 1, 10, 4, start_epoch_ms);  /* 1 Hz */

    snprintf(path, sizeof(path), "%s/%s_sa2.snt", s->dir, s->session_id);
    s->sa2.f_l0 = fopen(path, "wb");
    if (s->sa2.f_l0) write_snt_header(s->sa2.f_l0, 0, 10, 2, start_epoch_ms);  /* 1 Hz */

    snprintf(path, sizeof(path), "%s/%s_pld.snt", s->dir, s->session_id);
    s->pld.f_l0 = fopen(path, "wb");
    if (s->pld.f_l0) write_snt_header(s->pld.f_l0, 0, 5, 12, start_epoch_ms);  /* 0.5 Hz */

    snprintf(path, sizeof(path), "%s/%s_events.snt", s->dir, s->session_id);
    s->f_events = fopen(path, "w");
    if (!s->f_events) {
        ESP_LOGW(TAG, "failed to open events file (non-fatal)");
    }

    if (!s->brp.f_l0 || !s->sa2.f_l0 || !s->pld.f_l0) {
        ESP_LOGE(TAG, "failed to open .snt files");
        /* No mutex held — safe to call session_writer_stop directly. */
        session_writer_stop(s);
        free(s);
        return NULL;
    }

    /* Publish the new session as s_active.  This is the only point where
     * s_active is set; notif_proc_task and stream_data_raw will now route
     * data to this session. */
    xSemaphoreTake(s_active_mutex, portMAX_DELAY);
    s_active = s;
    xSemaphoreGive(s_active_mutex);

    /* Start flush task */
    s->flush_task_handle = psram_task_create(flush_task, "session_flush", FLUSH_TASK_STACK, s, 5, tskNO_AFFINITY, NULL, NULL);

    ESP_LOGI(TAG, "=== SESSION STARTED: %s ===", s->session_id);
    ESP_LOGI(TAG, "dir: %s", s->dir);

    return s;
}

esp_err_t session_writer_stop(session_writer_t *s)
{
    if (!s) return ESP_OK;

    /* Clear s_active and mark session inactive BEFORE the GetDateTime RPC.
     *
     * The GetDateTime RPC blocks ~50-100ms waiting for the AS11 response,
     * which is processed by notif_proc_task.  If s_active still points to
     * this session during that window, a TherapyStart notification arriving
     * in notif_proc_task would see s->active==true and NOT create a new
     * session — the new therapy's stream data and TherapyStart event would
     * be written to the old (closing) session's .snt files, gluing two
     * sessions together.
     *
     * By clearing s_active first (under the mutex, then releasing it), any
     * TherapyStart during the RPC will correctly start a new session via
     * session_writer_start().  The old session's flush/close continues in
     * parallel on this thread, operating on the local pointer s, which is
     * safe because s->active=false prevents new data writes. */
    xSemaphoreTake(s_active_mutex, portMAX_DELAY);
    s->active = false;
    if (s_active == s) s_active = NULL;
    xSemaphoreGive(s_active_mutex);

    /* Query AS11 clock (blocks on RPC — s_active is already clear, so
     * notif_proc_task can process the response and any TherapyStart
     * without interfering with this stop). */
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

    /* No mutex needed here — s_active is already cleared and s->active=false
     * prevents concurrent writers from using this session. */
    s->end_time_us = esp_timer_get_time();
    s->end_epoch_ms = end_epoch_ms;
    if (have_drift) { s->clock_drift_ms = clock_drift_ms; s->clock_drift_valid = true; }

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

    /* Stream data quality summary: shows how many notifications were
     * received, how many gap events were detected and compensated for,
     * and the effective packet loss rate.  A loss rate of 0.00% means
     * no dropped notifications; 0.13% is typical for problematic BLE
     * links.  This helps users assess whether their data is complete. */
    if (s->stream_notifications > 0) {
        uint32_t expected = s->stream_notifications + s->gap_missing_total;
        uint32_t loss_bps = (s->gap_missing_total * 10000) / expected; /* bps = 0.01% units */
        ESP_LOGI(TAG, "stream quality: %u notifications received, "
                 "%u gap events, %u missing compensated, "
                 "loss rate %u.%02u%%",
                 (unsigned)s->stream_notifications,
                 (unsigned)s->gap_events,
                 (unsigned)s->gap_missing_total,
                 loss_bps / 100, loss_bps % 100);
    }

    /* s_active was already cleared above (before GetDateTime RPC). */

    if (s->mutex) vSemaphoreDelete(s->mutex);
    /* NOTE: s is NOT freed here — the caller (stop_task) needs to read
     * s->clock_drift_ms and s->start_epoch_ms after this function returns.
     * The caller is responsible for calling free(s). */
    s->mutex = NULL;  /* prevent double-delete */

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
 * Returns true if a therapy start/stop event is found. */
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
            out[count++] = SNT_MISSING;  /* null → missing sentinel */
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

/* Parse the time portion of an ISO8601 "startTime" string into
 * milliseconds-since-midnight.  The AS11 always uses the format
 * "YYYY-MM-DDTHH:MM:SS.mmmZ" (fixed-width, UTC).  We extract only
 * the H/M/S/ms fields — sufficient for detecting 200ms gaps between
 * consecutive StreamData notifications without a full date parser.
 *
 * Returns -1 if the string is too short or malformed. */
static int64_t parse_starttime_ms(const char *s, int len)
{
    /* Minimum: "YYYY-MM-DDTHH:MM:SS.mmmZ" = 24 chars */
    if (len < 24) return -1;
    /* Sanity: check digit positions */
    if (s[11] < '0' || s[11] > '9') return -1;
    int h   = (s[11] - '0') * 10 + (s[12] - '0');
    int m   = (s[14] - '0') * 10 + (s[15] - '0');
    int sec = (s[17] - '0') * 10 + (s[18] - '0');
    int ms  = (s[20] - '0') * 100 + (s[21] - '0') * 10 + (s[22] - '0');
    return (int64_t)h * 3600000 + m * 60000 + sec * 1000 + ms;
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
    int64_t cur_stream_ms = -1;   /* startTime parsed from this notification */

    /* Single-pass scan: walk JSON, extract all key→value pairs.
     * Handles two value types: arrays (signal data) and strings
     * (startTime).  The scanner peeks at the character after ':' to
     * decide which path to take. */
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
        if (p >= end) continue;

        /* startTime is a string value — extract for gap detection */
        if (klen == 9 && strncmp(key_start, "startTime", 9) == 0) {
            if (*p == '"') {
                p++;
                const char *str_start = p;
                while (p < end && *p != '"') p++;
                int slen = (int)(p - str_start);
                if (p < end) p++; /* skip closing quote */
                cur_stream_ms = parse_starttime_ms(str_start, slen);
            }
            continue;
        }

        /* Signal data is always a JSON array */
        if (*p != '[') continue;

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
        s = session_writer_start();
        if (s) {
            bsp_display_set_therapy_active(true);
        } else {
            /* SD not ready — show warning instead of therapy graph */
            const char *sd_lines[] = { "SD Card Error", "Cannot record session" };
            bsp_display_show_lines("Warning", sd_lines, 2);
        }
    }

    if (!s || !session_writer_is_active(s)) return;

    xSemaphoreTake(s->mutex, portMAX_DELAY);

    s->stream_notifications++;

    /* ── Missing-packet compensation ───────────────────────────────
     * Detect dropped StreamData notifications by comparing the current
     * startTime to the previous notification's startTime.  Normal gap
     * is ~200ms (reportIntervalMs).  The AS11 clock has ±3ms jitter,
     * so we use a threshold of 280ms (>1 sample interval beyond normal)
     * to flag a gap.  Each missing notification = 5 BRP samples (25Hz ×
     * 200ms) and advances the SA2/PLD decimation counters by 1.
     *
     * Compensation per signal type:
     *   BRP: insert SNT_MISSING sentinels (waveform integrity — no fabrication)
     *   SA2: hold previous HR/SpO2 (slow vitals, change over minutes)
     *   PLD: hold previous row (2s summaries, change slowly)
     *
     * The gap is measured in ms and converted to missing-notification
     * count via integer division by 200.  This is exact because the
     * AS11's startTime advances in ~200ms steps. */
    if (cur_stream_ms >= 0 && s->prev_stream_ms_valid) {
        int64_t gap = cur_stream_ms - s->prev_stream_ms;
        /* Handle midnight rollover (86400000 ms/day) */
        if (gap < 0) gap += 86400000;
        /* Normal gap ~200ms; threshold 280ms = 200 + 2×40ms sample */
        if (gap > 280) {
            int missing = (int)((gap - 100) / 200);
            if (missing > 0 && missing < 50) {
                ESP_LOGW(TAG, "StreamData gap: %lldms (%d missing notifications), "
                         "inserting compensation", (long long)gap, missing);

                s->gap_events++;
                s->gap_missing_total += missing;

                /* BRP: insert 5 SNT_MISSING sentinels per missing notification */
                for (int m = 0; m < missing; m++) {
                    uint32_t base = s->brp_buf_count;
                    if (base + 5 <= 1500) {
                        for (int j = 0; j < 5; j++) {
                            s->brp_flow[base + j] = SNT_MISSING;
                            s->brp_press[base + j] = SNT_MISSING;
                        }
                        s->brp_buf_count = base + 5;
                    }
                }

                /* SA2: advance decimation counter, hold previous value
                 * on each wrap.  The counter decrements from 5→0; each
                 * time it hits 0 we write a sample.  We simulate the
                 * missed decrements to keep the counter synchronized. */
                for (int m = 0; m < missing; m++) {
                    if (s->sa2_countdown <= 1) {
                        s->sa2_countdown = 5;
                        if (s->sa2_buf_count < 60) {
                            s->sa2_hr[s->sa2_buf_count] =
                                s->last_hr_valid ? s->last_hr : SNT_MISSING;
                            s->sa2_spo2[s->sa2_buf_count] =
                                s->last_spo2_valid ? s->last_spo2 : SNT_MISSING;
                            s->sa2_buf_count++;
                        }
                    } else {
                        s->sa2_countdown--;
                    }
                }

                /* PLD: same approach — advance decimation counter,
                 * hold previous row on each wrap. */
                for (int m = 0; m < missing; m++) {
                    if (s->pld_countdown <= 1) {
                        s->pld_countdown = 10;
                        if (s->last_pld_valid && s->pld_buf_count < 300) {
                            for (int k = 0; k < 12; k++)
                                s->pld_buf[s->pld_buf_count][k] = s->last_pld[k];
                            s->pld_buf_count++;
                        }
                    } else {
                        s->pld_countdown--;
                    }
                }
            }
        }
    }

    /* Update previous startTime for next notification's gap check */
    if (cur_stream_ms >= 0) {
        s->prev_stream_ms = cur_stream_ms;
        s->prev_stream_ms_valid = true;
    }

    /* BRP: PatientFlow + MaskPressure (25 Hz, 40ms natural interval).
     * Flow and pressure arrive together (one value per 40ms tick) and must be
     * appended in lockstep at the SAME cumulative buffer position.  Missing
     * samples in either channel use the SNT_MISSING sentinel.
     *
     * Recording starts at TherapyStart (session_writer_start) and ends at
     * TherapyStop (session_writer_stop), matching AS11 export behavior.
     * Archive analysis confirms BRP starts ~5-9s after TherapyStart and
     * ends at TherapyStop ≈ MaskOff time. */
    {
        uint32_t base = s->brp_buf_count;
        int n_pairs = flow_n > press_n ? flow_n : press_n;
        for (int j = 0; j < n_pairs && base + j < 1500; j++) {
            s->brp_flow[base + j]  = (j < flow_n)  ? flow_vals[j]  : SNT_MISSING;
            s->brp_press[base + j] = (j < press_n) ? press_vals[j] : SNT_MISSING;
        }
        uint32_t added = (uint32_t)n_pairs;
        if (base + added > 1500) added = 1500 - base;
        s->brp_buf_count = base + added;
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
        /* Cache last known values for gap hold */
        if (hr_found) { s->last_hr = hr_val; s->last_hr_valid = true; }
        if (spo2_found) { s->last_spo2 = spo2_val; s->last_spo2_valid = true; }
    } else if (spo2_found && s->sa2_buf_count > 0) {
        /* Update SpO2 for the most recent SA2 sample */
        s->sa2_spo2[s->sa2_buf_count - 1] = spo2_val;
        if (spo2_found) { s->last_spo2 = spo2_val; s->last_spo2_valid = true; }
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
            /* Cache last known PLD row for gap hold */
            for (int k = 0; k < 12; k++)
                s->last_pld[k] = s->pld_buf[s->pld_buf_count - 1][k];
            s->last_pld_valid = true;
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
    char     session_id[32];
    int64_t  start_epoch_ms;
    int64_t  end_epoch_ms;
    int64_t  clock_drift_ms;
    int64_t  stop_boot_us;  /* esp_timer_get_time() at session stop */
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
        esp_err_t ret = edf_gen_generate(a->session_dir, a->session_id,
                         a->start_epoch_ms, a->end_epoch_ms,
                         a->clock_drift_ms);
        if (ret == ESP_OK) {
            /* EDF generation complete — trigger upload.
             * Compute the noon-based day folder from session start time. */
            char day_folder[32];
            time_t t = (time_t)(a->start_epoch_ms / 1000);
            struct tm tm;
            localtime_r(&t, &tm);
            if (tm.tm_hour < 12) {
                t -= 86400;
                localtime_r(&t, &tm);
            }
            snprintf(day_folder, sizeof(day_folder), "%04d%02d%02d",
                     tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
            uploader_on_day_ready(day_folder);
        } else {
            ESP_LOGW(TAG, "edf_task: generation failed, not triggering upload");
        }
        free(a);
    } else {
        ESP_LOGE(TAG, "edf_task: NULL args");
    }
    ESP_LOGI(TAG, "edf_task: done");
    vTaskDelete(NULL);
}

/* spool_refresh_task: retries pulling the Summary spool until fresh, then
 * runs EDF generation.  Runs on core 0 at priority 5 (below notif_proc_task
 * at priority 10) so BLE notifications always preempt it.
 *
 * The retry loop (post_therapy_wait_spool_current) yields via vTaskDelay
 * and semaphore waits, allowing notif_proc_task to process BLE responses
 * for each spool pull.  EDF generation is mostly SD I/O which also yields.
 *
 * Uses the PSRAM stack allocated by stop_task. */
static void spool_refresh_task(void *arg)
{
    edf_task_args_t *a = (edf_task_args_t *)arg;
    if (!a) {
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "spool_refresh_task: waiting for spool to become current");
    bool fresh = post_therapy_wait_spool_current(a->end_epoch_ms, a->clock_drift_ms);
    int64_t elapsed_ms = (esp_timer_get_time() - a->stop_boot_us) / 1000;
    ESP_LOGI(TAG, "spool_refresh_task: spool %s after %lld ms from session stop",
             fresh ? "CURRENT" : "STALE (timeout)", (long long)elapsed_ms);

    /* Launch EDF generation on core 1. */
    ESP_LOGI(TAG, "spool_refresh_task: launching EDF generation");
    esp_err_t ret = edf_gen_generate(a->session_dir, a->session_id,
                     a->start_epoch_ms, a->end_epoch_ms,
                     a->clock_drift_ms);
    if (ret == ESP_OK) {
        char day_folder[32];
        time_t t = (time_t)(a->start_epoch_ms / 1000);
        struct tm tm;
        localtime_r(&t, &tm);
        if (tm.tm_hour < 12) {
            t -= 86400;
            localtime_r(&t, &tm);
        }
        snprintf(day_folder, sizeof(day_folder), "%04d%02d%02d",
                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
        uploader_on_day_ready(day_folder);
    } else {
        ESP_LOGW(TAG, "spool_refresh_task: EDF generation failed, not triggering upload");
    }
    free(a);

    ESP_LOGI(TAG, "spool_refresh_task: done");
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
 *                               → checks if Summary spool is current for today
 *   3a. If spool is current: edf_gen_generate() launched as a SEPARATE task
 *       on core 1 (see edf_task above).
 *   3b. If spool is stale: spool_refresh_task launched on core 0 retries
 *       the pull every 3s for up to 2 min. When fresh (or timeout), it
 *       launches edf_gen_generate() on core 1.
 *
 * Steps 1 and 2 run in stop_task (needs BLE for spool pulls).
 * Step 3 is dispatched and stop_task exits immediately, freeing the
 * high-priority slot for notif_proc_task. */
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
    char session_id[32];

    strlcpy(session_dir, s->dir, sizeof(session_dir));
    strlcpy(session_id, s->session_id, sizeof(session_id));  /* file prefix */

    /* Step 1: Finalise stream data (flush, close files, write session.json).
     * This also queries AS11 clock and sets s->clock_drift_ms. */
    session_writer_stop(s);

    /* Now capture the values that session_writer_stop() set */
    int64_t start_epoch_ms = s->start_epoch_ms;
    int64_t end_epoch_ms = s->end_epoch_ms;
    int64_t clock_drift_ms = s->clock_drift_ms;
    int64_t stop_boot_us = s->end_time_us;

    /* Free the session writer struct — we have everything we need */
    free(s);

    /* Step 2: Post-therapy data collection (spool pulls + Get RPC).
     * This pulls Summary and TherapyEvents spools from the AS11 and
     * queries device identification/settings via Get RPC.  All data
     * is saved to the post-therapy/ subfolder inside the session directory.
     * This step blocks on BLE RPC responses (semaphores), allowing
     * notif_proc_task to run between RPCs.  May take 10-30 seconds.
     * Also checks whether the current day's Summary spool is fresh. */
    bool spool_current = false;
    ESP_LOGI(TAG, "stop_task: starting post-therapy collection");
    post_therapy_collect(session_dir, session_id, start_epoch_ms, clock_drift_ms,
                         end_epoch_ms, &spool_current);

    /* Step 3: Launch EDF generation.
     *
     * If the spool is current, launch edf_gen_generate() immediately on
     * core 1 (async, non-blocking).
     *
     * If the spool is stale, launch spool_refresh_task on core 0 which
     * retries the pull every 3s for up to 2 min. When the spool becomes
     * current (or timeout), it launches edf_gen_generate() on core 1.
     * This keeps stop_task non-blocking — notif_proc_task is free to
     * process new therapy notifications. */
    edf_task_args_t *edf_args = malloc(sizeof(edf_task_args_t));
    if (edf_args) {
        strlcpy(edf_args->session_dir, session_dir, sizeof(edf_args->session_dir));
        strlcpy(edf_args->session_id, session_id, sizeof(edf_args->session_id));
        edf_args->start_epoch_ms = start_epoch_ms;
        edf_args->end_epoch_ms = end_epoch_ms;
        edf_args->clock_drift_ms = clock_drift_ms;
        edf_args->stop_boot_us = stop_boot_us;

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

            if (spool_current) {
                /* Spool is fresh — launch EDF generation immediately. */
                TaskHandle_t edf_handle = xTaskCreateStaticPinnedToCore(
                    edf_task, "edf_gen", edf_stack_size, edf_args, 5,
                    edf_stack, edf_tcb, 1);
                if (edf_handle) {
                    ESP_LOGI(TAG, "stop_task: EDF generation launched on core 1 "
                             "(spool current, stack=%u PSRAM)",
                             (unsigned)edf_stack_size);
                } else {
                    ESP_LOGE(TAG, "stop_task: xTaskCreateStaticPinnedToCore failed");
                    free(edf_stack); free(edf_tcb);
                    s_prev_edf_stack = NULL; s_prev_edf_tcb = NULL;
                    free(edf_args);
                }
            } else {
                /* Spool is stale — launch refresh task on core 0 that
                 * retries the pull every 3s for up to 2 min, then
                 * generates EDF when ready (or timeout). */
                TaskHandle_t refresh_handle = xTaskCreateStaticPinnedToCore(
                    spool_refresh_task, "spool_refresh", edf_stack_size, edf_args, 5,
                    edf_stack, edf_tcb, 0);
                if (refresh_handle) {
                    ESP_LOGI(TAG, "stop_task: spool refresh task launched on core 0 "
                             "(spool stale, stack=%u PSRAM)",
                             (unsigned)edf_stack_size);
                } else {
                    ESP_LOGE(TAG, "stop_task: xTaskCreateStaticPinnedToCore failed");
                    free(edf_stack); free(edf_tcb);
                    s_prev_edf_stack = NULL; s_prev_edf_tcb = NULL;
                    free(edf_args);
                }
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

    /* Handle EventNotification: check for TherapyStart/TherapyStop.
     *
     * Session lifecycle (confirmed from AS11 logs 2026-07-11):
     *   - TherapyStop and TherapyStart are separate EventNotifications,
     *     typically 1-5 minutes apart for short mask-off breaks.
     *   - Each creates a separate .snt session and separate EDF files.
     *   - The AS11 sends MaskOn ~7s after TherapyStart; .snt recording
     *     starts at TherapyStart, so BRP/PLD/SA2 EDF files use skip_samples
     *     to align to MaskOn (see edf_gen.c).
     *
     * Race condition note: session_writer_stop() now clears s_active BEFORE
     * the GetDateTime RPC, so a TherapyStart arriving during that ~50ms
     * window will correctly create a new session instead of writing to the
     * closing session's files (which was the primary "session gluing" cause
     * for short inter-session breaks). */
    if (strcmp(method_str, "EventNotification") == 0) {
        bool start = false, stop = false;
        check_event_notification(msg, &start, &stop);

        /* Handle stop first so that a single message containing both
         * TherapyStart + TherapyStop (quick start/stop) doesn't leave
         * the display stuck in graph mode.  After stop, s is set to NULL
         * so the start handler below will create a fresh session. */
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
                psram_task_create(stop_task, "session_stop", 8192, s, 15, tskNO_AFFINITY, NULL, NULL);
                s = NULL;
            }
        }
        if (start) {
            ESP_LOGI(TAG, ">>> THERAPY START detected");
            s_therapy_stopped = false;
            if (!s || !s->active) {
                s = session_writer_start();
            }
            if (s) {
                bsp_display_set_therapy_active(true);
                write_event(s, msg);
            } else {
                /* SD not ready — show warning instead of therapy graph */
                const char *sd_lines[] = { "SD Card Error", "Cannot record session" };
                bsp_display_show_lines("Warning", sd_lines, 2);
            }
        }
        if (start || stop) return;

        /* Check for _SNC ValueChange (Summary spool update notification).
         * Format: {"method":"EventNotification","params":{"dataId":"_SNC",
         * "events":[{"event":"ValueChange","value":247}]}} */
        cJSON *params = cJSON_GetObjectItem(msg, "params");
        if (params) {
            cJSON *data_id = cJSON_GetObjectItem(params, "dataId");
            if (data_id && cJSON_IsString(data_id) &&
                strcmp(data_id->valuestring, "_SNC") == 0) {
                cJSON *events = cJSON_GetObjectItem(params, "events");
                if (events && cJSON_IsArray(events)) {
                    cJSON *ev = cJSON_GetArrayItem(events, 0);
                    if (ev) {
                        cJSON *val = cJSON_GetObjectItem(ev, "value");
                        if (val && cJSON_IsNumber(val)) {
                            s_snc_value = (int64_t)val->valuedouble;
                            s_snc_changed = true;
                            ESP_LOGI(TAG, ">>> _SNC ValueChange: %lld",
                                     (long long)s_snc_value);
                        }
                    }
                }
                return;  /* _SNC notifications are not written to events.snt */
            }
            /* _ZLE ValueChange — write to events.snt with injected NTP
             * timestamp.  The AS11 does not include reportTime in
             * ValueChange notifications (same as _SNC), so we capture
             * the NTP time at receipt and inject it as "ntpTimeMs".
             * The EDF generator uses this to align BRP/PLD/SA2 start
             * to the AS11's _ZLE gating signal instead of MaskOn.
             * Rationale: https://github.com/ilyakruchinin/SomnoTrace/issues/20#issuecomment-4975037843 */
            if (strcmp(data_id->valuestring, "_ZLE") == 0) {
                int zle_val = -1;
                cJSON *events = cJSON_GetObjectItem(params, "events");
                if (events && cJSON_IsArray(events)) {
                    cJSON *ev = cJSON_GetArrayItem(events, 0);
                    if (ev) {
                        cJSON *val = cJSON_GetObjectItem(ev, "value");
                        if (val && cJSON_IsNumber(val))
                            zle_val = (int)val->valuedouble;
                    }
                }
                if (s && s->active) {
                    cJSON *zle_copy = cJSON_Duplicate(msg, 1);
                    if (zle_copy) {
                        cJSON *zle_params = cJSON_GetObjectItem(zle_copy, "params");
                        if (zle_params) {
                            cJSON *zle_events = cJSON_GetObjectItem(zle_params, "events");
                            if (zle_events && cJSON_IsArray(zle_events)) {
                                cJSON *zle_ev = cJSON_GetArrayItem(zle_events, 0);
                                if (zle_ev) {
                                    int64_t ntp_ms = (int64_t)time(NULL) * 1000;
                                    cJSON_AddNumberToObject(zle_ev, "ntpTimeMs",
                                                            (double)ntp_ms);
                                }
                            }
                        }
                        char *json_str = cJSON_PrintUnformatted(zle_copy);
                        if (json_str) {
                            fputs(json_str, s->f_events);
                            fputc('\n', s->f_events);
                            free(json_str);
                        }
                        cJSON_Delete(zle_copy);
                    }
                }
                ESP_LOGI(TAG, ">>> _ZLE ValueChange: %d (%s)",
                         zle_val, zle_val == 1 ? "rising edge" : "falling edge");
                return;
            }
        }

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

/* ── _SNC tracking ───────────────────────────────────────────────── */

bool session_writer_snc_changed(int64_t *out_value)
{
    if (!s_snc_changed) return false;
    s_snc_changed = false;
    if (out_value) *out_value = s_snc_value;
    return true;
}

/* ── Crash recovery ───────────────────────────────────────────────── */

void session_writer_recover(void)
{
    if (!sd_storage_is_ready()) return;

    DIR *dir = opendir(SD_STREAMS_DIR);
    if (!dir) return;

    struct dirent *ent;
    int recovered = 0;

    /* Scan noon-day folders under .somnotrace/sessions/streams/ */
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        if (strlen(ent->d_name) != 8) continue;  /* YYYYMMDD */

        char day_path[300];
        snprintf(day_path, sizeof(day_path), "%s/%s", SD_STREAMS_DIR, ent->d_name);

        DIR *day_dir = opendir(day_path);
        if (!day_dir) continue;

        struct dirent *day_ent;
        while ((day_ent = readdir(day_dir)) != NULL) {
            if (day_ent->d_name[0] == '.') continue;

            /* Look for *_brp.snt files — each indicates a session */
            const char *brp_suffix = strstr(day_ent->d_name, "_brp.snt");
            if (!brp_suffix) continue;

            /* Extract the session prefix (everything before _brp.snt) */
            size_t prefix_len = brp_suffix - day_ent->d_name;
            if (prefix_len >= 32) continue;
            char prefix[32];
            memcpy(prefix, day_ent->d_name, prefix_len);
            prefix[prefix_len] = '\0';

            /* Check if session.json already exists (session was finalised) */
            char json_path[400];
            snprintf(json_path, sizeof(json_path), "%s/%s_session.json", day_path, prefix);
            struct stat st;
            if (stat(json_path, &st) == 0) continue;

            ESP_LOGW(TAG, "found interrupted session: %s/%s — writing metadata",
                     ent->d_name, prefix);

            /* Read sample counts and start_epoch_ms from .snt headers */
            char path[400];
            uint32_t brp_samples = 0, sa2_samples = 0, pld_samples = 0, brp_mm_samples = 0;
            int64_t start_epoch_ms = 0;
            snt_header_t hdr;
            bool got_valid_header = false;

            snprintf(path, sizeof(path), "%s/%s_brp.snt", day_path, prefix);
            FILE *f = fopen(path, "rb");
            if (f) {
                if (fread(&hdr, 1, SNT_HEADER_SIZE, f) == SNT_HEADER_SIZE && hdr.magic == SNT_MAGIC) {
                    brp_samples = hdr.sample_count;
                    start_epoch_ms = hdr.start_epoch_ms;
                    got_valid_header = true;
                }
                fclose(f);
            }

            /* Skip sessions with no readable .snt header (0-byte files from
             * crash before FATFS flush).  No data was written, and defaulting
             * start_epoch_ms to 0 produces invalid 19691231 EDF folders. */
            if (!got_valid_header) {
                ESP_LOGW(TAG, "skipping interrupted session %s/%s — no valid .snt header (crash before flush?)",
                         ent->d_name, prefix);
                continue;
            }

            snprintf(path, sizeof(path), "%s/%s_brp_mm.snt", day_path, prefix);
            f = fopen(path, "rb");
            if (f) {
                if (fread(&hdr, 1, SNT_HEADER_SIZE, f) == SNT_HEADER_SIZE && hdr.magic == SNT_MAGIC)
                    brp_mm_samples = hdr.sample_count;
                fclose(f);
            }

            snprintf(path, sizeof(path), "%s/%s_sa2.snt", day_path, prefix);
            f = fopen(path, "rb");
            if (f) {
                if (fread(&hdr, 1, SNT_HEADER_SIZE, f) == SNT_HEADER_SIZE && hdr.magic == SNT_MAGIC)
                    sa2_samples = hdr.sample_count;
                fclose(f);
            }

            snprintf(path, sizeof(path), "%s/%s_pld.snt", day_path, prefix);
            f = fopen(path, "rb");
            if (f) {
                if (fread(&hdr, 1, SNT_HEADER_SIZE, f) == SNT_HEADER_SIZE && hdr.magic == SNT_MAGIC)
                    pld_samples = hdr.sample_count;
                fclose(f);
            }

            /* Write session.json manually (no cJSON — stack-limited task) */
            f = fopen(json_path, "w");
            if (f) {
                fprintf(f, "{\"id\":\"%s\",\"state\":\"interrupted\","
                           "\"start_epoch_ms\":%lld,"
                           "\"clock_drift_ms\":0,\"clock_drift_valid\":false,"
                           "\"brp_samples\":%u,\"brp_mm_samples\":%u,"
                           "\"sa2_samples\":%u,\"pld_samples\":%u",
                        prefix,
                        (long long)start_epoch_ms,
                        (unsigned)brp_samples, (unsigned)brp_mm_samples,
                        (unsigned)sa2_samples, (unsigned)pld_samples);

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
        closedir(day_dir);
    }

    closedir(dir);
    if (recovered > 0) {
        ESP_LOGI(TAG, "recovered %d interrupted session(s)", recovered);
    }
}
