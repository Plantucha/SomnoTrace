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

/* ────────────────────────────────────────────────────────────────────
 *  Architecture (see spec/archive/TODO-resume-after-reboot-take3.md)
 *
 *  Two persistent workers own everything that can block:
 *
 *   sw_storage  — owns every session file and every filesystem operation:
 *                 mkdir, open, header write, sample write, event write,
 *                 header update, fsync, close, manifest write, checkpoint.
 *                 Created once at init with a static command queue.
 *
 *   sw_post     — owns the post-stop pipeline: the stop-time GetDateTime
 *                 RPC, post-therapy spool collection, EDF generation and
 *                 the upload trigger.  Created once at init; no per-stop
 *                 task allocation (a failed allocation used to leave a
 *                 session that was never finalised).
 *
 *  The BLE notification path performs NO filesystem I/O.  It appends
 *  samples to a producer-owned batch and enqueues:
 *      - lifecycle commands (open / finalize)
 *      - immutable stream batches (flow+pressure travel together)
 *      - event records (pre-formatted JSON strings)
 *
 *  Flow and pressure share one batch descriptor and one sample count so
 *  they can never drift out of lockstep.  Batches are drawn from a small
 *  per-session PSRAM pool, so a slow card produces a bounded, counted
 *  backlog instead of silent sample loss.
 *
 *  Durability ordering per commit (never claim durability early):
 *      1. write detached batch payloads
 *      2. fflush data
 *      3. update stream headers
 *      4. fsync every affected stream
 *      5. write the checkpoint slot
 *      6. fsync the checkpoint
 * ──────────────────────────────────────────────────────────────────── */

#include "session_writer.h"
#include "sd_storage.h"
#include "as11_ble.h"
#include "bsp_display.h"
#include "post_therapy.h"
#include "edf_gen.h"
#include "uploader.h"
#include "time_sync.h"
#include "therapy_alert.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_rom_crc.h"
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

#define SNT_HEADER_SIZE  sizeof(snt_header_t)

/* ── Recovery journal (.ckpt) ─────────────────────────────────────────
 * Fixed-size binary journal with two alternating slots.  A checkpoint is
 * only written after the data it describes has been fsync'd, so it can
 * never over-promise.  Recovery picks the slot with the highest sequence
 * number and a valid CRC, which makes a torn write during the checkpoint
 * update harmless (the older slot is still intact). */

#define CKPT_MAGIC      0x534E5443u   /* "SNTC" */
#define CKPT_VERSION    1
#define CKPT_SLOTS      2

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    uint32_t seq;              /* monotonically increasing               */
    uint32_t flow_count;       /* durable per-stream record counts       */
    uint32_t press_count;
    uint32_t flow_mm_count;
    uint32_t sa2_count;
    uint32_t pld_count;
    int64_t  start_epoch_ms;
    int64_t  elapsed_us;       /* monotonic since session start          */
    int64_t  last_stream_us;   /* monotonic at last StreamData received   */
    uint32_t crc32;            /* over all preceding bytes               */
} snt_ckpt_t;

#define CKPT_SLOT_SIZE  sizeof(snt_ckpt_t)

/* ── Buffer geometry ──────────────────────────────────────────────────
 * Capacities are per batch, and a session holds SW_BATCH_POOL batches, so
 * total producer headroom is capacity × pool.  That is a better use of
 * RAM than one larger buffer: it absorbs a slow card without ever letting
 * the producer block on I/O. */

#define BRP_CAP              1500   /* 60 s @ 25 Hz, per channel */
#define SA2_CAP               180   /* 180 s @ 1 Hz              */
#define PLD_CAP               300   /* 600 s @ 0.5 Hz            */
#define BRP_COMMIT_THRESHOLD 1400   /* hand off before clipping  */
#define SW_BATCH_POOL           3

/* Maximum time therapy data may sit uncommitted.  Configurable policy
 * rather than a hardcoded tick: this bounds crash loss.  30 s is the
 * deliberate starting point — tightening it increases SD activity, which
 * is still a suspect for the INT_WDT reset. */
#ifndef SW_COMMIT_INTERVAL_MS
#define SW_COMMIT_INTERVAL_MS   30000
#endif

/* Close an orphaned session when StreamData stops arriving (missed
 * TherapyStop with no following TherapyStart).  Monotonic, not wall clock. */
#ifndef SW_STALE_TIMEOUT_MS
#define SW_STALE_TIMEOUT_MS     600000   /* 10 min */
#endif

/* A StreamData discontinuity at least this long is not compensated — the
 * session is split so the gap is never rendered as continuous samples. */
#ifndef SW_SPLIT_GAP_MS
#define SW_SPLIT_GAP_MS         120000   /* 2 min */
#endif

/* Ignore a repeated TherapyStart this soon after a session started; it is
 * an echo, not a new therapy cycle. */
#define SW_START_DEBOUNCE_MS    15000

#define SW_STORAGE_QUEUE_LEN    24
#define SW_POST_QUEUE_LEN        4
#define MAX_SESSION_DIR_LEN    128

/* ── Stream batch ─────────────────────────────────────────────────────
 * Immutable once handed to the storage worker.  Flow and pressure share
 * n_brp: they are appended in lockstep and must be written in lockstep. */

typedef struct {
    int16_t  brp_flow[BRP_CAP];
    int16_t  brp_press[BRP_CAP];
    uint32_t n_brp;
    int16_t  sa2_hr[SA2_CAP];
    int16_t  sa2_spo2[SA2_CAP];
    uint32_t n_sa2;
    int16_t  pld[PLD_CAP][12];
    uint32_t n_pld;
    int64_t  elapsed_us;      /* monotonic span covered by this batch */
    int64_t  last_stream_us;
} stream_batch_t;

/* ── Storage worker commands ──────────────────────────────────────── */

typedef enum {
    SW_CMD_OPEN = 0,   /* create dir, open files, sync headers        */
    SW_CMD_BATCH,      /* write one detached batch                    */
    SW_CMD_EVENT,      /* write one pre-formatted JSON event line     */
    SW_CMD_FINALIZE,   /* final commit, close, write manifest         */
} sw_cmd_type_t;

typedef struct {
    sw_cmd_type_t     type;
    session_writer_t *s;
    stream_batch_t   *batch;        /* SW_CMD_BATCH: returned to pool  */
    char             *event_json;   /* SW_CMD_EVENT: worker frees      */
    /* SW_CMD_FINALIZE payload */
    int64_t           end_epoch_ms;
    int64_t           clock_drift_ms;
    bool              drift_valid;
    const char       *drift_source;
    int64_t           drift_measured_at_ms;
    const char       *state;        /* static string                   */
    bool              free_session; /* worker owns the struct (no sw_post) */
    SemaphoreHandle_t done;
} sw_cmd_t;

/* ── Post-stop pipeline job ───────────────────────────────────────── */

typedef struct {
    session_writer_t *s;            /* freed by sw_post when done      */
    int64_t           end_epoch_ms;
    const char       *state;        /* "completed" | "timed_out" | ... */
    bool              allow_ble;    /* false when BLE is the reason    */
} sw_post_job_t;

/* ── Per-stream file state (storage worker only) ──────────────────── */

typedef struct {
    FILE    *f_l0;
    FILE    *f_l1;
    uint32_t sample_count;   /* durable records written (per channel) */
} stream_files_t;

struct session_writer {
    /* ── identity (set by producer at create, dir/id finalised by worker) */
    char     dir[MAX_SESSION_DIR_LEN];
    char     session_id[40];
    int64_t  start_time_us;
    int64_t  start_epoch_ms;
    int64_t  end_time_us;
    int64_t  end_epoch_ms;

    /* ── producer state ──────────────────────────────────────────── */
    volatile bool     active;
    SemaphoreHandle_t fill_mutex;
    stream_batch_t   *fill;          /* current producer batch          */
    QueueHandle_t     batch_pool;    /* free stream_batch_t*            */
    stream_batch_t   *batches[SW_BATCH_POOL];

    uint8_t  sa2_countdown;
    uint8_t  pld_countdown;

    /* missing-packet compensation / hold-last-value cache */
    int64_t  prev_stream_ms;
    bool     prev_stream_ms_valid;
    int16_t  last_flow, last_press;
    bool     last_brp_valid;
    int16_t  last_hr, last_spo2;
    bool     last_hr_valid, last_spo2_valid;
    int16_t  last_pld[12];
    bool     last_pld_valid;

    /* ── stats (producer writes, worker reads at finalize) ────────── */
    uint32_t stream_notifications;
    uint32_t gap_events;
    uint32_t gap_missing_total;
    uint32_t gap_long_events;      /* uncompensated discontinuities    */
    uint64_t gap_long_ms_total;
    int64_t  gap_long_first_ms;    /* offset from session start        */
    int64_t  gap_long_last_ms;
    uint32_t brp_dropped;
    uint32_t sa2_dropped;
    uint32_t pld_dropped;
    uint32_t batch_dropped;
    uint32_t event_dropped;

    /* ── monotonic activity tracking (watchdog) ───────────────────── */
    volatile int64_t last_stream_us;

    /* ── storage worker state ─────────────────────────────────────── */
    stream_files_t flow;
    stream_files_t press;
    stream_files_t sa2;
    stream_files_t pld_f;
    FILE    *f_events;
    FILE    *f_ckpt;
    uint32_t flow_mm_count;
    uint32_t ckpt_seq;
    int64_t  committed_elapsed_us;
    int64_t  committed_last_stream_us;
    bool     files_open;
    bool     have_uncommitted;

    /* ── failure state (worker writes, anyone reads) ──────────────── */
    volatile bool storage_failed;
    volatile uint32_t io_errors;
    char     first_io_error[80];

    /* ── drift / timing metadata (filled at finalize) ─────────────── */
    int64_t  clock_drift_ms;
    bool     clock_drift_valid;
    const char *clock_drift_source;
    int64_t  clock_drift_measured_at_ms;
};

/* ── Module state ─────────────────────────────────────────────────── */

static session_writer_t *s_active = NULL;
static SemaphoreHandle_t s_active_mutex = NULL;
static QueueHandle_t     s_storage_q = NULL;
static QueueHandle_t     s_post_q = NULL;
static TaskHandle_t      s_storage_task = NULL;
static TaskHandle_t      s_post_task = NULL;
static bool              s_ready = false;

/* Set on TherapyStop, cleared on TherapyStart.  RAM-only, so it is false
 * after any reset — which is what arms mid-therapy auto-start.  The stale
 * watchdog deliberately does NOT set it (that would block auto-start on
 * reconnect). */
static bool s_therapy_stopped = false;

/* TherapyStart de-duplication: an echoed start must not rotate a healthy
 * session, but a genuine one must (a missed TherapyStop otherwise glues
 * two nights together, and StreamData keeps resetting any timeout). */
static char    s_last_start_report[32] = {0};

static volatile bool s_snc_changed = false;
static volatile int64_t s_snc_value = -1;

static char s_device_addr[32] = {0};
static char s_client_id[64] = {0};

static void sw_request_finalize(session_writer_t *s, const char *state,
                                int64_t end_epoch_ms, bool allow_ble);

/* ── Helpers ──────────────────────────────────────────────────────── */

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

static void make_session_id(char *out, size_t out_len)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    snprintf(out, out_len, "%04d%02d%02d_%02d%02d%02d",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
}

static uint32_t snt_record_bytes(const snt_header_t *h)
{
    uint32_t n = (uint32_t)h->n_channels * (uint32_t)h->sample_bytes;
    return n ? n : 2;
}

/* ── Storage latency instrumentation ─────────────────────────────────
 * The INT_WDT root cause is still unknown and SDMMC is the leading suspect,
 * so every blocking filesystem operation is timed.  This is the evidence
 * that must exist BEFORE the commit interval is shortened: tightening it
 * increases SD activity, and if a long driver critical section is the
 * culprit, that would make the crash more frequent rather than less. */

typedef enum {
    SW_OP_OPEN = 0,
    SW_OP_WRITE,
    SW_OP_FLUSH,
    SW_OP_HEADER,
    SW_OP_FSYNC,
    SW_OP_MANIFEST,
    SW_OP_CKPT,
    SW_OP_COUNT
} sw_op_t;

static const char *const sw_op_names[SW_OP_COUNT] = {
    "open", "write", "flush", "header", "fsync", "manifest", "ckpt"
};

static struct {
    uint32_t max_us[SW_OP_COUNT];
    uint64_t total_us[SW_OP_COUNT];
    uint32_t count[SW_OP_COUNT];
    uint32_t over_100ms;
    uint32_t over_1s;
} s_lat;

/* Slow enough to be worth a line in the log on its own. */
#define SW_SLOW_OP_US   250000

static inline int64_t lat_begin(void) { return esp_timer_get_time(); }

static void lat_end(sw_op_t op, int64_t t0)
{
    int64_t dt = esp_timer_get_time() - t0;
    if (dt < 0) return;
    uint32_t us = (uint32_t)dt;
    if (us > s_lat.max_us[op]) s_lat.max_us[op] = us;
    s_lat.total_us[op] += us;
    s_lat.count[op]++;
    if (us >= 1000000) s_lat.over_1s++;
    else if (us >= 100000) s_lat.over_100ms++;
    if (us >= SW_SLOW_OP_US) {
        ESP_LOGW(TAG, "slow SD %s: %u ms (core %d)", sw_op_names[op],
                 (unsigned)(us / 1000), xPortGetCoreID());
    }
}

static void lat_report(void)
{
    ESP_LOGI(TAG, "SD latency (max/avg us): "
             "write=%u/%u flush=%u/%u header=%u/%u fsync=%u/%u "
             "ckpt=%u/%u manifest=%u/%u open=%u/%u; >100ms=%u >1s=%u",
             (unsigned)s_lat.max_us[SW_OP_WRITE],
             (unsigned)(s_lat.count[SW_OP_WRITE] ? s_lat.total_us[SW_OP_WRITE] / s_lat.count[SW_OP_WRITE] : 0),
             (unsigned)s_lat.max_us[SW_OP_FLUSH],
             (unsigned)(s_lat.count[SW_OP_FLUSH] ? s_lat.total_us[SW_OP_FLUSH] / s_lat.count[SW_OP_FLUSH] : 0),
             (unsigned)s_lat.max_us[SW_OP_HEADER],
             (unsigned)(s_lat.count[SW_OP_HEADER] ? s_lat.total_us[SW_OP_HEADER] / s_lat.count[SW_OP_HEADER] : 0),
             (unsigned)s_lat.max_us[SW_OP_FSYNC],
             (unsigned)(s_lat.count[SW_OP_FSYNC] ? s_lat.total_us[SW_OP_FSYNC] / s_lat.count[SW_OP_FSYNC] : 0),
             (unsigned)s_lat.max_us[SW_OP_CKPT],
             (unsigned)(s_lat.count[SW_OP_CKPT] ? s_lat.total_us[SW_OP_CKPT] / s_lat.count[SW_OP_CKPT] : 0),
             (unsigned)s_lat.max_us[SW_OP_MANIFEST],
             (unsigned)(s_lat.count[SW_OP_MANIFEST] ? s_lat.total_us[SW_OP_MANIFEST] / s_lat.count[SW_OP_MANIFEST] : 0),
             (unsigned)s_lat.max_us[SW_OP_OPEN],
             (unsigned)(s_lat.count[SW_OP_OPEN] ? s_lat.total_us[SW_OP_OPEN] / s_lat.count[SW_OP_OPEN] : 0),
             (unsigned)s_lat.over_100ms, (unsigned)s_lat.over_1s);
}

/* Record an I/O failure once, loudly, and surface it to the user.  Silent
 * loss becoming visible loss is most of the value of this path. */
static void io_fail(session_writer_t *s, const char *what)
{
    if (!s) return;
    s->io_errors++;
    if (!s->storage_failed) {
        s->storage_failed = true;
        snprintf(s->first_io_error, sizeof(s->first_io_error), "%s: %s",
                 what, strerror(errno));
        ESP_LOGE(TAG, "STORAGE FAILURE (%s) — errno=%d (%s)",
                 what, errno, strerror(errno));
        bsp_display_set_notice("SD write error");
    } else {
        ESP_LOGW(TAG, "storage error #%u (%s)", (unsigned)s->io_errors, what);
    }
}

static bool write_exact(session_writer_t *s, FILE *f, const void *data,
                        size_t len, const char *what)
{
    if (!f || len == 0) return f != NULL;
    int64_t t0 = lat_begin();
    size_t n = fwrite(data, 1, len, f);
    lat_end(SW_OP_WRITE, t0);
    if (n != len) {
        io_fail(s, what);
        return false;
    }
    return true;
}

/* ── Header write / repair ────────────────────────────────────────── */

static bool write_snt_header(session_writer_t *s, FILE *f, uint8_t tier,
                             uint16_t hz_x10, uint8_t n_ch,
                             int64_t start_epoch_ms)
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
    return write_exact(s, f, &hdr, SNT_HEADER_SIZE, "header write");
}

static bool update_snt_header_sample_count(session_writer_t *s, FILE *f,
                                           uint32_t count)
{
    if (!f) return true;
    int64_t th = lat_begin();
    long pos = ftell(f);
    if (fseek(f, offsetof(snt_header_t, sample_count), SEEK_SET) != 0) {
        io_fail(s, "header seek");
        return false;
    }
    bool ok = write_exact(s, f, &count, sizeof(uint32_t), "header update");
    if (fflush(f) != 0) { io_fail(s, "header flush"); ok = false; }
    lat_end(SW_OP_HEADER, th);
    if (fseek(f, pos, SEEK_SET) != 0) { io_fail(s, "header restore"); ok = false; }
    return ok;
}

/* ── Batch pool ───────────────────────────────────────────────────── */

static void batch_reset(stream_batch_t *b)
{
    b->n_brp = 0;
    b->n_sa2 = 0;
    b->n_pld = 0;
    b->elapsed_us = 0;
    b->last_stream_us = 0;
}

static bool batch_pool_create(session_writer_t *s)
{
    s->batch_pool = xQueueCreate(SW_BATCH_POOL, sizeof(stream_batch_t *));
    if (!s->batch_pool) return false;
    for (int i = 0; i < SW_BATCH_POOL; i++) {
        /* Large sample buffers live in PSRAM: they are never touched from an
         * ISR, and internal RAM is shared with Wi-Fi/BLE DMA. */
        s->batches[i] = heap_caps_calloc(1, sizeof(stream_batch_t),
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s->batches[i]) {
            s->batches[i] = calloc(1, sizeof(stream_batch_t));
        }
        if (!s->batches[i]) {
            ESP_LOGE(TAG, "batch pool alloc failed at %d", i);
            return false;
        }
        if (i > 0) {
            /* batches[0] becomes the initial fill buffer. */
            xQueueSend(s->batch_pool, &s->batches[i], 0);
        }
    }
    s->fill = s->batches[0];
    batch_reset(s->fill);
    return true;
}

static void batch_pool_destroy(session_writer_t *s)
{
    if (s->batch_pool) {
        stream_batch_t *b;
        while (xQueueReceive(s->batch_pool, &b, 0) == pdTRUE) { /* drain */ }
        vQueueDelete(s->batch_pool);
        s->batch_pool = NULL;
    }
    for (int i = 0; i < SW_BATCH_POOL; i++) {
        free(s->batches[i]);
        s->batches[i] = NULL;
    }
    s->fill = NULL;
}

/* Hand the current fill batch to the storage worker and take a fresh one.
 * Caller holds fill_mutex.  Returns false if the batch could not be handed
 * off (pool exhausted or queue full) — the caller keeps filling and the
 * loss is counted, never silent. */
static bool swap_and_enqueue_locked(session_writer_t *s)
{
    if (!s->fill || s->fill->n_brp == 0) {
        if (!s->fill || (s->fill->n_sa2 == 0 && s->fill->n_pld == 0))
            return true;   /* nothing to commit */
    }

    stream_batch_t *next = NULL;
    if (xQueueReceive(s->batch_pool, &next, 0) != pdTRUE) {
        s->batch_dropped++;
        return false;
    }

    stream_batch_t *full = s->fill;
    full->last_stream_us = s->last_stream_us;
    full->elapsed_us = esp_timer_get_time() - s->start_time_us;

    sw_cmd_t cmd = { .type = SW_CMD_BATCH, .s = s, .batch = full };
    if (xQueueSend(s_storage_q, &cmd, 0) != pdTRUE) {
        /* Put the spare back; keep filling the current batch. */
        xQueueSend(s->batch_pool, &next, 0);
        s->batch_dropped++;
        return false;
    }

    batch_reset(next);
    s->fill = next;
    return true;
}

static void producer_commit(session_writer_t *s)
{
    if (!s || !s->fill_mutex) return;
    xSemaphoreTake(s->fill_mutex, portMAX_DELAY);
    swap_and_enqueue_locked(s);
    xSemaphoreGive(s->fill_mutex);
}

/* ── Checkpoint ───────────────────────────────────────────────────── */

static void ckpt_write(session_writer_t *s)
{
    if (!s->f_ckpt) return;

    snt_ckpt_t c = {
        .magic = CKPT_MAGIC,
        .version = CKPT_VERSION,
        .reserved = 0,
        .seq = ++s->ckpt_seq,
        .flow_count = s->flow.sample_count,
        .press_count = s->press.sample_count,
        .flow_mm_count = s->flow_mm_count,
        .sa2_count = s->sa2.sample_count,
        .pld_count = s->pld_f.sample_count,
        .start_epoch_ms = s->start_epoch_ms,
        .elapsed_us = s->committed_elapsed_us,
        .last_stream_us = s->committed_last_stream_us,
        .crc32 = 0,
    };
    c.crc32 = esp_rom_crc32_le(0, (const uint8_t *)&c,
                               sizeof(c) - sizeof(uint32_t));

    int64_t t0 = lat_begin();
    long off = (long)((s->ckpt_seq % CKPT_SLOTS) * CKPT_SLOT_SIZE);
    if (fseek(s->f_ckpt, off, SEEK_SET) != 0) { io_fail(s, "ckpt seek"); return; }
    if (!write_exact(s, s->f_ckpt, &c, sizeof(c), "ckpt write")) return;
    if (fflush(s->f_ckpt) != 0) { io_fail(s, "ckpt flush"); return; }
    if (fsync(fileno(s->f_ckpt)) != 0) io_fail(s, "ckpt fsync");
    lat_end(SW_OP_CKPT, t0);
}

/* Read the newest valid checkpoint slot.  Returns false if none. */
static bool ckpt_read_best(const char *path, snt_ckpt_t *out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    bool found = false;
    for (int i = 0; i < CKPT_SLOTS; i++) {
        snt_ckpt_t c;
        if (fseek(f, (long)(i * CKPT_SLOT_SIZE), SEEK_SET) != 0) break;
        if (fread(&c, 1, sizeof(c), f) != sizeof(c)) continue;
        if (c.magic != CKPT_MAGIC || c.version != CKPT_VERSION) continue;
        uint32_t crc = esp_rom_crc32_le(0, (const uint8_t *)&c,
                                        sizeof(c) - sizeof(uint32_t));
        if (crc != c.crc32) continue;
        if (!found || c.seq > out->seq) { *out = c; found = true; }
    }
    fclose(f);
    return found;
}

/* ── Storage worker: file lifecycle ───────────────────────────────── */

static void storage_open(session_writer_t *s)
{
    char path[MAX_SESSION_DIR_LEN + 64];
    int64_t t_open = lat_begin();

    if (mkdir(s->dir, 0775) != 0 && errno != EEXIST) {
        ESP_LOGE(TAG, "failed to create noon-day dir %s: %s",
                 s->dir, strerror(errno));
        io_fail(s, "mkdir");
        return;
    }

    /* DST fallback / same-second restart: the same local timestamp can occur
     * twice.  Resolve here (worker context) rather than on the notification
     * path, then keep the resolved id — sw_post reads it only after this
     * command has run. */
    {
        struct stat st;
        snprintf(path, sizeof(path), "%s/%s_flow.snt", s->dir, s->session_id);
        if (stat(path, &st) == 0) {
            /* Bounded so "<base>_<n>" always fits in session_id. */
            char base[32];
            strlcpy(base, s->session_id, sizeof(base));
            bool resolved = false;
            for (int n = 2; n < 100; n++) {
                snprintf(path, sizeof(path), "%s/%s_%d_flow.snt", s->dir, base, n);
                if (stat(path, &st) != 0) {
                    snprintf(s->session_id, sizeof(s->session_id), "%s_%d", base, n);
                    resolved = true;
                    break;
                }
            }
            if (!resolved) {
                ESP_LOGE(TAG, "cannot find unique session prefix for %s", base);
                io_fail(s, "session id collision");
                return;
            }
        }
    }

    int64_t start_ms = s->start_epoch_ms;
    bool ok = true;

    snprintf(path, sizeof(path), "%s/%s_flow.snt", s->dir, s->session_id);
    s->flow.f_l0 = fopen(path, "wb");
    ok = ok && s->flow.f_l0 && write_snt_header(s, s->flow.f_l0, 0, 250, 1, start_ms);

    snprintf(path, sizeof(path), "%s/%s_press.snt", s->dir, s->session_id);
    s->press.f_l0 = fopen(path, "wb");
    ok = ok && s->press.f_l0 && write_snt_header(s, s->press.f_l0, 0, 250, 1, start_ms);

    snprintf(path, sizeof(path), "%s/%s_flow_mm.snt", s->dir, s->session_id);
    s->flow.f_l1 = fopen(path, "wb");
    ok = ok && s->flow.f_l1 && write_snt_header(s, s->flow.f_l1, 1, 10, 2, start_ms);

    snprintf(path, sizeof(path), "%s/%s_sa2.snt", s->dir, s->session_id);
    s->sa2.f_l0 = fopen(path, "wb");
    ok = ok && s->sa2.f_l0 && write_snt_header(s, s->sa2.f_l0, 0, 10, 2, start_ms);

    snprintf(path, sizeof(path), "%s/%s_pld.snt", s->dir, s->session_id);
    s->pld_f.f_l0 = fopen(path, "wb");
    ok = ok && s->pld_f.f_l0 && write_snt_header(s, s->pld_f.f_l0, 0, 5, 12, start_ms);

    snprintf(path, sizeof(path), "%s/%s_events.snt", s->dir, s->session_id);
    s->f_events = fopen(path, "w");
    if (!s->f_events) ESP_LOGW(TAG, "failed to open events file (non-fatal)");

    snprintf(path, sizeof(path), "%s/%s.ckpt", s->dir, s->session_id);
    s->f_ckpt = fopen(path, "wb+");
    if (!s->f_ckpt) ESP_LOGW(TAG, "failed to open checkpoint file (non-fatal)");

    if (!ok) {
        ESP_LOGE(TAG, "failed to open/initialise .snt files for %s", s->session_id);
        io_fail(s, "session open");
        return;
    }

    /* Make the session identity durable NOW.  fflush() only pushes stdio
     * into FATFS RAM; without fsync() the directory entry still reports 0
     * bytes, which is how a reset inside the first commit interval used to
     * leave an unrecoverable session. */
    FILE *all[] = { s->flow.f_l0, s->press.f_l0, s->flow.f_l1,
                    s->sa2.f_l0, s->pld_f.f_l0, s->f_events };
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
        if (!all[i]) continue;
        if (fflush(all[i]) != 0) io_fail(s, "open flush");
        if (fsync(fileno(all[i])) != 0) io_fail(s, "open fsync");
    }

    s->files_open = true;
    s->committed_elapsed_us = 0;
    s->committed_last_stream_us = 0;
    ckpt_write(s);
    lat_end(SW_OP_OPEN, t_open);

    ESP_LOGI(TAG, "=== SESSION STARTED: %s ===", s->session_id);
    ESP_LOGI(TAG, "dir: %s", s->dir);
}

/* Write one detached batch.  No locks held: the batch is immutable. */
static void storage_write_batch(session_writer_t *s, stream_batch_t *b)
{
    if (!s->files_open) return;

    /* flow (L0) + 1 Hz MinMax sidecar (L1) */
    for (uint32_t i = 0; i < b->n_brp; i++) {
        if (!write_exact(s, s->flow.f_l0, &b->brp_flow[i], sizeof(int16_t),
                         "flow write")) return;
    }
    if (s->flow.f_l1) {
        uint32_t n_sec = b->n_brp / 25;
        for (uint32_t sec = 0; sec < n_sec; sec++) {
            uint32_t base = sec * 25;
            int16_t fmn = INT16_MAX, fmx = INT16_MIN;
            for (int j = 0; j < 25; j++) {
                int16_t fv = b->brp_flow[base + j];
                if (fv != SNT_MISSING) {
                    if (fv < fmn) fmn = fv;
                    if (fv > fmx) fmx = fv;
                }
            }
            if (fmn == INT16_MAX) { fmn = fmx = SNT_MISSING; }
            int16_t mm[2] = { fmn, fmx };
            if (!write_exact(s, s->flow.f_l1, mm, sizeof(mm), "flow_mm write"))
                return;
            s->flow_mm_count++;
        }
    }
    /* pressure — same count as flow, by construction */
    for (uint32_t i = 0; i < b->n_brp; i++) {
        if (!write_exact(s, s->press.f_l0, &b->brp_press[i], sizeof(int16_t),
                         "press write")) return;
    }
    s->flow.sample_count += b->n_brp;
    s->press.sample_count += b->n_brp;

    for (uint32_t i = 0; i < b->n_sa2; i++) {
        int16_t pair[2] = { b->sa2_hr[i], b->sa2_spo2[i] };
        if (!write_exact(s, s->sa2.f_l0, pair, sizeof(pair), "sa2 write")) return;
    }
    s->sa2.sample_count += b->n_sa2;

    for (uint32_t i = 0; i < b->n_pld; i++) {
        if (!write_exact(s, s->pld_f.f_l0, b->pld[i], sizeof(int16_t) * 12,
                         "pld write")) return;
    }
    s->pld_f.sample_count += b->n_pld;

    if (b->elapsed_us > s->committed_elapsed_us)
        s->committed_elapsed_us = b->elapsed_us;
    if (b->last_stream_us > s->committed_last_stream_us)
        s->committed_last_stream_us = b->last_stream_us;

    s->have_uncommitted = true;
}

/* Ordered durability commit: data → headers → fsync → checkpoint → fsync. */
static void storage_commit(session_writer_t *s)
{
    if (!s->files_open || !s->have_uncommitted) return;

    struct { FILE *f; uint32_t count; } items[] = {
        { s->flow.f_l0,  s->flow.sample_count  },
        { s->flow.f_l1,  s->flow_mm_count      },
        { s->press.f_l0, s->press.sample_count },
        { s->sa2.f_l0,   s->sa2.sample_count   },
        { s->pld_f.f_l0, s->pld_f.sample_count },
    };

    for (size_t i = 0; i < sizeof(items) / sizeof(items[0]); i++) {
        if (!items[i].f) continue;
        int64_t t0 = lat_begin();
        if (fflush(items[i].f) != 0) io_fail(s, "data flush");
        lat_end(SW_OP_FLUSH, t0);
    }
    for (size_t i = 0; i < sizeof(items) / sizeof(items[0]); i++) {
        if (!items[i].f) continue;
        update_snt_header_sample_count(s, items[i].f, items[i].count);
    }
    for (size_t i = 0; i < sizeof(items) / sizeof(items[0]); i++) {
        if (!items[i].f) continue;
        int64_t t0 = lat_begin();
        if (fsync(fileno(items[i].f)) != 0) io_fail(s, "data fsync");
        lat_end(SW_OP_FSYNC, t0);
    }
    if (s->f_events) {
        if (fflush(s->f_events) != 0) io_fail(s, "events flush");
        if (fsync(fileno(s->f_events)) != 0) io_fail(s, "events fsync");
    }

    /* Only now may the checkpoint claim these counts are durable. */
    ckpt_write(s);
    s->have_uncommitted = false;

    ESP_LOGI(TAG, "commit: flow=%u press=%u sa2=%u pld=%u seq=%u",
             (unsigned)s->flow.sample_count, (unsigned)s->press.sample_count,
             (unsigned)s->sa2.sample_count, (unsigned)s->pld_f.sample_count,
             (unsigned)s->ckpt_seq);

    /* Every 10th commit (~5 min at the default interval), summarise SD
     * latency so a degrading card shows up in the log before it costs a
     * night of data. */
    if ((s->ckpt_seq % 10) == 0) lat_report();
}

/* ── Manifest ─────────────────────────────────────────────────────── */

static void write_manifest(session_writer_t *s, const char *state)
{
    char path[MAX_SESSION_DIR_LEN + 64];
    char tmp[MAX_SESSION_DIR_LEN + 72];
    snprintf(path, sizeof(path), "%s/%s_session.json", s->dir, s->session_id);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "id", s->session_id);
    cJSON_AddNumberToObject(root, "start_epoch_ms", (double)s->start_epoch_ms);
    if (s->end_epoch_ms > 0)
        cJSON_AddNumberToObject(root, "end_epoch_ms", (double)s->end_epoch_ms);

    char iso[32];
    struct tm tm;
    time_t start = (time_t)(s->start_epoch_ms / 1000);
    localtime_r(&start, &tm);
    strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%S", &tm);
    cJSON_AddStringToObject(root, "start_iso", iso);
    if (s->end_epoch_ms > 0) {
        time_t end = (time_t)(s->end_epoch_ms / 1000);
        localtime_r(&end, &tm);
        strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%S", &tm);
        cJSON_AddStringToObject(root, "end_iso", iso);
    }

    cJSON_AddStringToObject(root, "state", state);
    cJSON_AddNumberToObject(root, "fmt", 2);

    cJSON_AddNumberToObject(root, "brp_samples", (double)s->flow.sample_count);
    cJSON_AddNumberToObject(root, "brp_mm_samples", (double)s->flow_mm_count);
    cJSON_AddNumberToObject(root, "press_samples", (double)s->press.sample_count);
    cJSON_AddNumberToObject(root, "sa2_samples", (double)s->sa2.sample_count);
    cJSON_AddNumberToObject(root, "pld_samples", (double)s->pld_f.sample_count);

    cJSON_AddNumberToObject(root, "stream_notifications", (double)s->stream_notifications);
    cJSON_AddNumberToObject(root, "gap_events", (double)s->gap_events);
    cJSON_AddNumberToObject(root, "gap_missing", (double)s->gap_missing_total);

    /* Uncompensated discontinuities: recorded honestly rather than padded. */
    if (s->gap_long_events) {
        cJSON_AddNumberToObject(root, "gap_long_events", (double)s->gap_long_events);
        cJSON_AddNumberToObject(root, "gap_long_ms", (double)s->gap_long_ms_total);
        cJSON_AddNumberToObject(root, "gap_long_first_offset_ms", (double)s->gap_long_first_ms);
        cJSON_AddNumberToObject(root, "gap_long_last_offset_ms", (double)s->gap_long_last_ms);
    }

    /* Silent loss becoming visible loss. */
    if (s->brp_dropped || s->sa2_dropped || s->pld_dropped ||
        s->batch_dropped || s->event_dropped) {
        cJSON_AddNumberToObject(root, "brp_dropped", (double)s->brp_dropped);
        cJSON_AddNumberToObject(root, "sa2_dropped", (double)s->sa2_dropped);
        cJSON_AddNumberToObject(root, "pld_dropped", (double)s->pld_dropped);
        cJSON_AddNumberToObject(root, "batch_dropped", (double)s->batch_dropped);
        cJSON_AddNumberToObject(root, "event_dropped", (double)s->event_dropped);
    }
    if (s->io_errors) {
        cJSON_AddNumberToObject(root, "io_errors", (double)s->io_errors);
        cJSON_AddStringToObject(root, "io_error_first", s->first_io_error);
    }

    cJSON_AddNumberToObject(root, "clock_drift_ms", (double)s->clock_drift_ms);
    cJSON_AddBoolToObject(root, "clock_drift_valid", s->clock_drift_valid);
    cJSON_AddStringToObject(root, "clock_drift_source",
                            s->clock_drift_source ? s->clock_drift_source : "none");
    if (s->clock_drift_measured_at_ms > 0)
        cJSON_AddNumberToObject(root, "clock_drift_measured_at_ms",
                                (double)s->clock_drift_measured_at_ms);
    /* A measured drift and an estimate must never look alike to a consumer. */
    cJSON_AddBoolToObject(root, "clock_drift_usable",
                          s->clock_drift_valid ||
                          (s->clock_drift_source &&
                           strcmp(s->clock_drift_source, "none") != 0));

    cJSON_AddNumberToObject(root, "end_source_checkpoint_seq", (double)s->ckpt_seq);

    const char *src_str = "none";
    switch (time_source_get()) {
        case TIME_SRC_NTP:        src_str = "ntp"; break;
        case TIME_SRC_AS11_DRIFT: src_str = "as11_drift"; break;
        default:                  src_str = "none"; break;
    }
    cJSON_AddStringToObject(root, "time_source", src_str);

    if (s_device_addr[0]) cJSON_AddStringToObject(root, "as11_device", s_device_addr);
    if (s_client_id[0]) cJSON_AddStringToObject(root, "as11_client_id", s_client_id);

    char *json_str = cJSON_Print(root);
    cJSON_Delete(root);
    if (!json_str) return;

    /* temp → checked write → fflush → fsync → rename.  A crash must never
     * leave a half-written manifest that recovery would trust. */
    bool ok = false;
    int64_t t_man = lat_begin();
    size_t len = strlen(json_str);
    FILE *f = fopen(tmp, "w");
    if (f) {
        ok = (fwrite(json_str, 1, len, f) == len);
        if (ok && fflush(f) != 0) ok = false;
        if (ok && fsync(fileno(f)) != 0) ok = false;
        if (fclose(f) != 0) ok = false;
    }
    if (ok) {
        unlink(path);                       /* FATFS cannot rename-over */
        if (rename(tmp, path) != 0) ok = false;
    }
    lat_end(SW_OP_MANIFEST, t_man);
    if (!ok) {
        unlink(tmp);
        io_fail(s, "manifest write");
        ESP_LOGE(TAG, "failed to write %s", path);
    } else {
        ESP_LOGI(TAG, "wrote %s (state=%s)", path, state);
    }
    free(json_str);
}

static void storage_finalize(session_writer_t *s, const sw_cmd_t *cmd)
{
    s->end_epoch_ms = cmd->end_epoch_ms;
    s->end_time_us = esp_timer_get_time();
    s->clock_drift_ms = cmd->clock_drift_ms;
    s->clock_drift_valid = cmd->drift_valid;
    s->clock_drift_source = cmd->drift_source;
    s->clock_drift_measured_at_ms = cmd->drift_measured_at_ms;

    storage_commit(s);

    if (s->flow.f_l0)  { fclose(s->flow.f_l0);  s->flow.f_l0 = NULL; }
    if (s->flow.f_l1)  { fclose(s->flow.f_l1);  s->flow.f_l1 = NULL; }
    if (s->press.f_l0) { fclose(s->press.f_l0); s->press.f_l0 = NULL; }
    if (s->sa2.f_l0)   { fclose(s->sa2.f_l0);   s->sa2.f_l0 = NULL; }
    if (s->pld_f.f_l0) { fclose(s->pld_f.f_l0); s->pld_f.f_l0 = NULL; }
    if (s->f_events)   { fclose(s->f_events);   s->f_events = NULL; }
    if (s->f_ckpt)     { fclose(s->f_ckpt);     s->f_ckpt = NULL; }
    s->files_open = false;

    /* Never claim "completed" when storage failed — that is the difference
     * between a trustworthy manifest and a plausible-looking lie. */
    const char *state = cmd->state;
    if (s->storage_failed) state = "storage_failed";
    write_manifest(s, state);

    ESP_LOGI(TAG, "=== SESSION STOPPED: %s (%s) ===", s->session_id, state);
    ESP_LOGI(TAG, "flow=%u press=%u sa2=%u pld=%u total samples",
             (unsigned)s->flow.sample_count, (unsigned)s->press.sample_count,
             (unsigned)s->sa2.sample_count, (unsigned)s->pld_f.sample_count);

    if (s->stream_notifications > 0) {
        uint32_t expected = s->stream_notifications + s->gap_missing_total;
        uint32_t loss_bps = expected ? (s->gap_missing_total * 10000) / expected : 0;
        ESP_LOGI(TAG, "stream quality: %u notifications received, "
                 "%u gap events, %u missing compensated, loss rate %u.%02u%%",
                 (unsigned)s->stream_notifications, (unsigned)s->gap_events,
                 (unsigned)s->gap_missing_total, loss_bps / 100, loss_bps % 100);
    }
    if (s->gap_long_events) {
        ESP_LOGW(TAG, "uncompensated discontinuities: %u events, %llu ms total",
                 (unsigned)s->gap_long_events,
                 (unsigned long long)s->gap_long_ms_total);
    }
    if (s->brp_dropped || s->sa2_dropped || s->pld_dropped ||
        s->batch_dropped || s->event_dropped) {
        ESP_LOGW(TAG, "dropped: brp=%u sa2=%u pld=%u batches=%u events=%u",
                 (unsigned)s->brp_dropped, (unsigned)s->sa2_dropped,
                 (unsigned)s->pld_dropped, (unsigned)s->batch_dropped,
                 (unsigned)s->event_dropped);
    }

    lat_report();

    batch_pool_destroy(s);
    if (s->fill_mutex) { vSemaphoreDelete(s->fill_mutex); s->fill_mutex = NULL; }
}

/* ── Storage worker task ──────────────────────────────────────────── */

static void sw_storage_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "storage worker started on core %d", xPortGetCoreID());

    int64_t last_commit_us = esp_timer_get_time();

    while (1) {
        sw_cmd_t cmd;
        if (xQueueReceive(s_storage_q, &cmd, pdMS_TO_TICKS(500)) == pdTRUE) {
            switch (cmd.type) {
            case SW_CMD_OPEN:
                storage_open(cmd.s);
                last_commit_us = esp_timer_get_time();
                break;

            case SW_CMD_BATCH:
                storage_write_batch(cmd.s, cmd.batch);
                /* Return the buffer to the producer pool immediately. */
                if (cmd.s->batch_pool)
                    xQueueSend(cmd.s->batch_pool, &cmd.batch, 0);
                break;

            case SW_CMD_EVENT:
                if (cmd.s->f_events && cmd.event_json) {
                    if (fputs(cmd.event_json, cmd.s->f_events) < 0 ||
                        fputc('\n', cmd.s->f_events) < 0) {
                        io_fail(cmd.s, "event write");
                    }
                }
                free(cmd.event_json);
                break;

            case SW_CMD_FINALIZE: {
                session_writer_t *fs = cmd.s;
                bool own = cmd.free_session;
                storage_finalize(fs, &cmd);
                if (cmd.done) xSemaphoreGive(cmd.done);
                /* Normally sw_post owns the struct and frees it after the
                 * export pipeline; only the fallback path hands it to us. */
                if (own) free(fs);
                break;
            }
            }
            /* Fall through to the periodic check rather than `continue`, so a
             * continuously busy queue cannot starve the commit interval or the
             * stale-session watchdog. */
        }

        /* Periodic: bound uncommitted time, and close orphaned sessions. */
        session_writer_t *s = s_active;
        int64_t now = esp_timer_get_time();

        if (s && s->active) {
            if ((now - last_commit_us) / 1000 >= SW_COMMIT_INTERVAL_MS) {
                producer_commit(s);          /* swap producer buffer  */
                last_commit_us = now;
                /* The batch lands on the queue; drain it before committing so
                 * the checkpoint covers it. */
                sw_cmd_t pending;
                while (xQueuePeek(s_storage_q, &pending, 0) == pdTRUE &&
                       pending.type == SW_CMD_BATCH) {
                    if (xQueueReceive(s_storage_q, &pending, 0) != pdTRUE) break;
                    storage_write_batch(pending.s, pending.batch);
                    if (pending.s->batch_pool)
                        xQueueSend(pending.s->batch_pool, &pending.batch, 0);
                }
                storage_commit(s);
            }

            /* Stale-session watchdog: monotonic, so a wall-clock step or an
             * AS11 time-of-day jump cannot defeat it. */
            int64_t idle_ms = (now - s->last_stream_us) / 1000;
            if (s->last_stream_us > 0 && idle_ms >= SW_STALE_TIMEOUT_MS) {
                ESP_LOGW(TAG, "no StreamData for %lld ms — closing orphaned session",
                         (long long)idle_ms);
                sw_request_finalize(s, "timed_out",
                                    (int64_t)time(NULL) * 1000, false);
            }
        } else if (s == NULL) {
            last_commit_us = now;
        }
    }
}

/* ── Post-stop pipeline task ──────────────────────────────────────── */

static void sw_post_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "post worker started on core %d", xPortGetCoreID());

    while (1) {
        sw_post_job_t job;
        if (xQueueReceive(s_post_q, &job, portMAX_DELAY) != pdTRUE) continue;
        session_writer_t *s = job.s;
        if (!s) continue;

        /* 1. Establish drift with honest provenance.
         *
         * The stop-time GetDateTime RPC is the authoritative source, but it
         * is unavailable exactly when the link dropped — which is the common
         * case for a timed-out session.  Never block on it there. */
        int64_t drift_ms = 0;
        bool drift_valid = false;
        const char *drift_source = "none";
        int64_t drift_at = job.end_epoch_ms;

        if (job.allow_ble) {
            int64_t as11_ms = 0;
            if (as11_ble_get_datetime(&as11_ms) == ESP_OK) {
                drift_ms = job.end_epoch_ms - as11_ms;
                drift_valid = true;
                drift_source = "measured_stop";
                ESP_LOGI(TAG, "clock_drift_ms = %lld (stop-time)", (long long)drift_ms);
            } else if (as11_ble_get_clock_drift(&drift_ms) == ESP_OK) {
                drift_valid = true;
                drift_source = "measured_prestream";
                ESP_LOGI(TAG, "clock_drift_ms = %lld (pre-stream)", (long long)drift_ms);
            }
        }
        if (!drift_valid) {
            time_drift_snapshot_t snap;
            if (time_sync_get_drift_snapshot(&snap) && snap.available) {
                drift_ms = snap.drift_ms;
                drift_at = snap.measured_at_ms;
                drift_source = snap.source;   /* "nvs" | "sd" */
                ESP_LOGW(TAG, "clock_drift_ms = %lld (estimate from %s)",
                         (long long)drift_ms, drift_source);
            } else {
                ESP_LOGW(TAG, "clock_drift_ms unavailable");
            }
        }

        /* 2. Hand the drift to the storage worker and wait for the files to
         * be finalised, so the manifest is complete before anything reads it. */
        SemaphoreHandle_t done = xSemaphoreCreateBinary();
        sw_cmd_t fin = {
            .type = SW_CMD_FINALIZE,
            .s = s,
            .end_epoch_ms = job.end_epoch_ms,
            .clock_drift_ms = drift_ms,
            .drift_valid = drift_valid,
            .drift_source = drift_source,
            .drift_measured_at_ms = drift_at,
            .state = job.state,
            .done = done,
        };
        xQueueSend(s_storage_q, &fin, portMAX_DELAY);
        if (done) {
            xSemaphoreTake(done, pdMS_TO_TICKS(60000));
            vSemaphoreDelete(done);
        }

        /* 3. Persist drift only when the clock was NTP-authoritative, else a
         * degraded-mode session would feed its own estimate back in. */
        if (drift_valid && time_source_get() == TIME_SRC_NTP) {
            time_sync_save_drift(drift_ms, job.end_epoch_ms);
        }

        char session_dir[MAX_SESSION_DIR_LEN];
        char session_id[40];
        strlcpy(session_dir, s->dir, sizeof(session_dir));
        strlcpy(session_id, s->session_id, sizeof(session_id));
        int64_t start_epoch_ms = s->start_epoch_ms;
        int64_t end_epoch_ms = s->end_epoch_ms;
        int64_t stop_boot_us = s->end_time_us;
        bool storage_failed = s->storage_failed;

        free(s);
        s = NULL;

        if (storage_failed) {
            ESP_LOGE(TAG, "post: storage failed for %s — skipping export",
                     session_id);
            continue;
        }

        /* 4. Post-therapy collection (BLE spool pulls + Get RPC). */
        bool spool_current = false;
        if (job.allow_ble) {
            ESP_LOGI(TAG, "post: starting post-therapy collection");
            post_therapy_collect(session_dir, session_id, start_epoch_ms,
                                 drift_ms, end_epoch_ms, &spool_current);
            if (!spool_current) {
                ESP_LOGI(TAG, "post: waiting for Summary spool to become current");
                bool fresh = post_therapy_wait_spool_current(end_epoch_ms, drift_ms);
                int64_t elapsed_ms = (esp_timer_get_time() - stop_boot_us) / 1000;
                ESP_LOGI(TAG, "post: spool %s after %lld ms from stop",
                         fresh ? "CURRENT" : "STALE (timeout)",
                         (long long)elapsed_ms);
            }
        } else {
            ESP_LOGW(TAG, "post: BLE unavailable, skipping spool collection for %s",
                     session_id);
        }

        /* 5. EDF generation + upload trigger.  Runs here rather than in a
         * per-stop task: one persistent worker means no allocation can fail
         * at the exact moment a session needs exporting. */
        esp_err_t ret = edf_gen_generate(session_dir, session_id,
                                        start_epoch_ms, end_epoch_ms, drift_ms);
        if (ret == ESP_OK) {
            char day_folder[32];
            time_t t = (time_t)(start_epoch_ms / 1000);
            struct tm tm;
            localtime_r(&t, &tm);
            if (tm.tm_hour < 12) { t -= 86400; localtime_r(&t, &tm); }
            snprintf(day_folder, sizeof(day_folder), "%04d%02d%02d",
                     tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
            uploader_on_day_ready(day_folder);
        } else {
            ESP_LOGW(TAG, "post: EDF generation failed, not triggering upload");
        }

        UBaseType_t free_bytes = uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t);
        if (free_bytes < 2048) {
            ESP_LOGW(TAG, "post worker: LOW STACK — %u bytes free at peak",
                     (unsigned)free_bytes);
        } else {
            ESP_LOGI(TAG, "post worker: peak stack headroom %u bytes",
                     (unsigned)free_bytes);
        }
    }
}

/* ── Public API ───────────────────────────────────────────────────── */

esp_err_t session_writer_init(void)
{
    if (s_ready) return ESP_OK;

    s_active_mutex = xSemaphoreCreateMutex();
    if (!s_active_mutex) return ESP_ERR_NO_MEM;

    s_storage_q = xQueueCreate(SW_STORAGE_QUEUE_LEN, sizeof(sw_cmd_t));
    s_post_q = xQueueCreate(SW_POST_QUEUE_LEN, sizeof(sw_post_job_t));
    if (!s_storage_q || !s_post_q) {
        ESP_LOGE(TAG, "session writer queue alloc failed");
        return ESP_ERR_NO_MEM;
    }

    /* Persistent workers, created once.  If either cannot be created the
     * caller disables recording visibly, rather than discovering the
     * problem at TherapyStop when the night is already lost. */
    s_storage_task = psram_task_create(sw_storage_task, "sw_storage", 8192,
                                       NULL, 8, tskNO_AFFINITY, NULL, NULL);
    if (!s_storage_task) {
        ESP_LOGE(TAG, "storage worker creation failed");
        return ESP_ERR_NO_MEM;
    }
    s_post_task = psram_task_create(sw_post_task, "sw_post", 16384,
                                    NULL, 5, 1, NULL, NULL);
    if (!s_post_task) {
        ESP_LOGE(TAG, "post worker creation failed");
        return ESP_ERR_NO_MEM;
    }

    s_ready = true;
    ESP_LOGI(TAG, "session writer initialised (commit interval %d ms, "
             "stale timeout %d ms)", SW_COMMIT_INTERVAL_MS, SW_STALE_TIMEOUT_MS);
    return ESP_OK;
}

session_writer_t *session_writer_start(void)
{
    if (!s_ready) {
        ESP_LOGE(TAG, "cannot start session: writer not initialised");
        return NULL;
    }
    if (!sd_storage_is_ready()) {
        ESP_LOGE(TAG, "cannot start session: SD not ready");
        return NULL;
    }
    if (!sd_storage_reserve_for_recording()) {
        ESP_LOGE(TAG, "cannot start session: insufficient free space");
        return NULL;
    }

    /* Rotate any previous session through the normal stop pipeline. */
    xSemaphoreTake(s_active_mutex, portMAX_DELAY);
    session_writer_t *prev = s_active;
    s_active = NULL;
    xSemaphoreGive(s_active_mutex);
    if (prev) {
        ESP_LOGW(TAG, "session already active, rotating");
        prev->active = false;
        sw_request_finalize(prev, "rotated", (int64_t)time(NULL) * 1000, true);
    }

    session_writer_t *s = calloc(1, sizeof(session_writer_t));
    if (!s) return NULL;

    s->fill_mutex = xSemaphoreCreateMutex();
    if (!s->fill_mutex) { free(s); return NULL; }
    if (!batch_pool_create(s)) {
        batch_pool_destroy(s);
        vSemaphoreDelete(s->fill_mutex);
        free(s);
        return NULL;
    }

    s->pld_countdown = 1;
    s->sa2_countdown = 1;
    s->clock_drift_source = "none";
    s->start_time_us = esp_timer_get_time();
    s->start_epoch_ms = (int64_t)time(NULL) * 1000;
    s->last_stream_us = s->start_time_us;

    make_session_id(s->session_id, sizeof(s->session_id));
    char noon_day[16];
    noon_day_folder_local(time(NULL), noon_day, sizeof(noon_day));
    snprintf(s->dir, sizeof(s->dir), "%s/%s", SD_STREAMS_DIR, noon_day);

    s->active = true;
    /* Declare the recording so destructive maintenance actions are refused
     * for its duration (raw capture outranks derived output). */
    sd_storage_recording_begin();

    /* Publish before the files exist on purpose.  The producer can start
     * filling immediately while the worker creates and syncs the headers,
     * so the notifications that arrive during file setup are buffered
     * rather than dropped — mid-therapy auto-start is detected from the
     * very first StreamData notification. */
    xSemaphoreTake(s_active_mutex, portMAX_DELAY);
    s_active = s;
    xSemaphoreGive(s_active_mutex);

    sw_cmd_t cmd = { .type = SW_CMD_OPEN, .s = s };
    if (xQueueSend(s_storage_q, &cmd, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGE(TAG, "failed to enqueue session open");
        xSemaphoreTake(s_active_mutex, portMAX_DELAY);
        if (s_active == s) s_active = NULL;
        xSemaphoreGive(s_active_mutex);
        s->active = false;
        sd_storage_recording_end();
        batch_pool_destroy(s);
        vSemaphoreDelete(s->fill_mutex);
        free(s);
        return NULL;
    }

    return s;
}

/* Request finalisation.  Never performs I/O or BLE work in the caller's
 * context: the storage worker closes the files and the post worker runs the
 * export pipeline. */
static void sw_request_finalize(session_writer_t *s, const char *state,
                                int64_t end_epoch_ms, bool allow_ble)
{
    if (!s) return;

    /* Clear s_active first so a TherapyStart arriving during the stop
     * pipeline creates a fresh session instead of writing into this one. */
    xSemaphoreTake(s_active_mutex, portMAX_DELAY);
    bool was_active = s->active;
    s->active = false;
    if (s_active == s) s_active = NULL;
    xSemaphoreGive(s_active_mutex);
    if (was_active) sd_storage_recording_end();

    /* Hand off whatever the producer still holds, before FINALIZE is queued;
     * the queue is FIFO, so the batch is written first. */
    producer_commit(s);

    sw_post_job_t job = {
        .s = s,
        .end_epoch_ms = end_epoch_ms,
        .state = state,
        .allow_ble = allow_ble,
    };
    if (xQueueSend(s_post_q, &job, pdMS_TO_TICKS(1000)) != pdTRUE) {
        /* Post queue full: finalise the files anyway so the raw data is never
         * left unfinished, and let the day rebuild handle the export. */
        ESP_LOGE(TAG, "post queue full — finalising files without export");
        sw_cmd_t fin = {
            .type = SW_CMD_FINALIZE, .s = s,
            .end_epoch_ms = end_epoch_ms,
            .drift_source = "none",
            .state = state,
            .free_session = true,   /* nobody downstream owns it now */
        };
        /* The stale-session watchdog runs ON the storage worker.  Queueing to
         * our own queue with portMAX_DELAY would self-deadlock if it were
         * full, so finalise inline when we are already that task. */
        if (xTaskGetCurrentTaskHandle() == s_storage_task) {
            storage_finalize(s, &fin);
            free(s);
        } else if (xQueueSend(s_storage_q, &fin, pdMS_TO_TICKS(5000)) != pdTRUE) {
            ESP_LOGE(TAG, "storage queue full — session %s left unfinalised "
                     "(recovery will repair it on next boot)", s->session_id);
        }
    }
}

esp_err_t session_writer_stop(session_writer_t *s)
{
    if (!s) return ESP_OK;
    sw_request_finalize(s, "completed", (int64_t)time(NULL) * 1000, true);
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

bool session_writer_snc_changed(int64_t *out_value)
{
    if (!s_snc_changed) return false;
    s_snc_changed = false;
    if (out_value) *out_value = s_snc_value;
    return true;
}

/* ── Event queueing (no I/O on the notification path) ─────────────── */

static void queue_event_json(session_writer_t *s, char *json_str)
{
    if (!s || !json_str) { free(json_str); return; }
    sw_cmd_t cmd = { .type = SW_CMD_EVENT, .s = s, .event_json = json_str };
    if (xQueueSend(s_storage_q, &cmd, 0) != pdTRUE) {
        s->event_dropped++;
        free(json_str);
    }
}

static void write_event(session_writer_t *s, const cJSON *msg)
{
    if (!s || !s->active) return;
    queue_event_json(s, cJSON_PrintUnformatted(msg));
}

/* ── Notification parsing ─────────────────────────────────────────── */

/* Check an EventNotification for therapy start/stop events. */
static bool check_event_notification(const cJSON *msg, bool *out_start,
                                     bool *out_stop, const char **out_report)
{
    *out_start = false;
    *out_stop = false;
    if (out_report) *out_report = NULL;

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
            cJSON *rt = cJSON_GetObjectItem(ev, "reportTime");
            if (out_report && rt && cJSON_IsString(rt))
                *out_report = rt->valuestring;
        } else if (strcmp(event->valuestring, "TherapyStop") == 0) {
            *out_stop = true;
        }
    }
    return *out_start || *out_stop;
}

/* ── Fast-path StreamData parser (bypasses cJSON) ─────────────────── */

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
            out[count++] = SNT_MISSING;
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

static int match_key(const char *key, int klen)
{
    for (int i = 0; i < N_KEYS; i++) {
        if (s_keys[i].len != klen) continue;
        if (strncmp(key, s_keys[i].name, klen) == 0) return s_keys[i].id;
    }
    return -1;
}

static int64_t parse_starttime_ms(const char *s, int len)
{
    if (len < 24) return -1;
    if (s[11] < '0' || s[11] > '9') return -1;
    int h   = (s[11] - '0') * 10 + (s[12] - '0');
    int m   = (s[14] - '0') * 10 + (s[15] - '0');
    int sec = (s[17] - '0') * 10 + (s[18] - '0');
    int ms  = (s[20] - '0') * 100 + (s[21] - '0') * 10 + (s[22] - '0');
    return (int64_t)h * 3600000 + m * 60000 + sec * 1000 + ms;
}

void session_writer_on_stream_data_raw(const char *json, int len)
{
    session_writer_t *s = session_writer_get_active();

    static bool s_first_logged = false;
    if (!s_first_logged) {
        s_first_logged = true;
        ESP_LOGI(TAG, "first StreamData (%d bytes): %.*s", len,
                 len > 512 ? 512 : len, json);
    }

    const char *p = json;
    const char *end = json + len;

    int16_t flow_vals[32], press_vals[32];
    int flow_n = 0, press_n = 0;
    int16_t hr_val = -1, spo2_val = -1;
    bool hr_found = false, spo2_found = false;
    int16_t pld_vals[12];
    bool pld_found[12] = {false};
    for (int i = 0; i < 12; i++) pld_vals[i] = -1;

    int64_t cur_stream_ms = -1;
    {
        const char *st = strstr(json, "\"startTime\"");
        if (st) {
            const char *q = strchr(st + 11, '"');
            if (q) {
                q++;
                const char *qe = strchr(q, '"');
                if (qe) cur_stream_ms = parse_starttime_ms(q, (int)(qe - q));
            }
        }
    }

    while (p < end) {
        while (p < end && *p != '"') p++;
        if (p >= end) break;
        p++;
        const char *key_start = p;
        while (p < end && *p != '"') p++;
        if (p >= end) break;
        int klen = (int)(p - key_start);
        p++;

        while (p < end && (*p == ' ' || *p == ':')) p++;
        if (p >= end) break;
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

    bool has_therapy_pressure = false;
    for (int j = 0; j < press_n; j++) {
        if (press_vals[j] > 200) has_therapy_pressure = true;
    }

    /* Edge case: auto-start session if flow and pressure indicate active
     * therapy (reboot mid-therapy, or ESP powered on after the AS11). */
    if ((!s || !session_writer_is_active(s)) && !s_therapy_stopped
        && has_active_flow && has_therapy_pressure) {
        ESP_LOGI(TAG, ">>> THERAPY detected via non-zero flow (reboot mid-therapy?)");
        bsp_display_set_therapy_active(true);
        s = session_writer_start();
        if (!s) {
            ESP_LOGW(TAG, "session_writer_start() failed — "
                     "graph active but NOT recording to SD");
        }
    }

    if (!s || !session_writer_is_active(s)) return;

    s->last_stream_us = esp_timer_get_time();

    xSemaphoreTake(s->fill_mutex, portMAX_DELAY);

    s->stream_notifications++;
    stream_batch_t *b = s->fill;

    /* ── Missing-packet compensation ───────────────────────────────
     * Short gaps hold the previous value (visually indistinguishable and
     * matching AS11 conventions).  Long gaps are NOT padded: minutes of
     * fabricated data would be worse than an honest boundary, so they are
     * counted and — beyond SW_SPLIT_GAP_MS — the session is split. */
    bool want_split = false;
    if (cur_stream_ms >= 0 && s->prev_stream_ms_valid) {
        int64_t gap = cur_stream_ms - s->prev_stream_ms;
        if (gap < 0) gap += 86400000;
        if (gap > 280) {
            int missing = (int)((gap - 100) / 200);
            if (missing > 0 && missing < 50) {
                ESP_LOGW(TAG, "StreamData gap: %lldms (%d missing notifications), "
                         "inserting compensation", (long long)gap, missing);
                s->gap_events++;
                s->gap_missing_total += missing;

                for (int m = 0; m < missing; m++) {
                    uint32_t base = b->n_brp;
                    if (base + 5 <= BRP_CAP && s->last_brp_valid) {
                        for (int j = 0; j < 5; j++) {
                            b->brp_flow[base + j] = s->last_flow;
                            b->brp_press[base + j] = s->last_press;
                        }
                        b->n_brp = base + 5;
                    } else if (s->last_brp_valid) {
                        s->brp_dropped += 5;
                    }
                }
                for (int m = 0; m < missing; m++) {
                    if (s->sa2_countdown <= 1) {
                        s->sa2_countdown = 5;
                        if (b->n_sa2 < SA2_CAP) {
                            b->sa2_hr[b->n_sa2] = s->last_hr_valid ? s->last_hr : SNT_MISSING;
                            b->sa2_spo2[b->n_sa2] = s->last_spo2_valid ? s->last_spo2 : SNT_MISSING;
                            b->n_sa2++;
                        } else {
                            s->sa2_dropped++;
                        }
                    } else {
                        s->sa2_countdown--;
                    }
                }
                for (int m = 0; m < missing; m++) {
                    if (s->pld_countdown <= 1) {
                        s->pld_countdown = 10;
                        if (s->last_pld_valid && b->n_pld < PLD_CAP) {
                            for (int k = 0; k < 12; k++)
                                b->pld[b->n_pld][k] = s->last_pld[k];
                            b->n_pld++;
                        } else if (s->last_pld_valid) {
                            s->pld_dropped++;
                        }
                    } else {
                        s->pld_countdown--;
                    }
                }
            } else if (missing >= 50) {
                /* Uncompensated discontinuity — record it honestly. */
                int64_t offset_ms = s->start_epoch_ms > 0
                    ? (esp_timer_get_time() - s->start_time_us) / 1000 : 0;
                s->gap_long_events++;
                s->gap_long_ms_total += (uint64_t)gap;
                if (s->gap_long_first_ms == 0) s->gap_long_first_ms = offset_ms;
                s->gap_long_last_ms = offset_ms;
                ESP_LOGW(TAG, "long StreamData gap: %lld ms (%d notifications) — "
                         "not compensated", (long long)gap, missing);
                if (gap >= SW_SPLIT_GAP_MS) want_split = true;
            }
        }
    }

    if (cur_stream_ms >= 0) {
        s->prev_stream_ms = cur_stream_ms;
        s->prev_stream_ms_valid = true;
    }

    if (want_split) {
        /* Split rather than emit a continuous-looking timeline across a
         * multi-minute hole.  .snt assumes uniform sampling, so a gap this
         * large cannot be represented honestly inside one session. */
        xSemaphoreGive(s->fill_mutex);
        ESP_LOGW(TAG, "splitting session at long gap");
        sw_request_finalize(s, "split", (int64_t)time(NULL) * 1000, true);
        s = session_writer_start();
        if (!s) return;
        xSemaphoreTake(s->fill_mutex, portMAX_DELAY);
        b = s->fill;
        s->prev_stream_ms = cur_stream_ms;
        s->prev_stream_ms_valid = true;
    }

    /* BRP: flow + pressure appended in lockstep at the same buffer index. */
    {
        uint32_t base = b->n_brp;
        int n_pairs = flow_n > press_n ? flow_n : press_n;
        int room = (int)(BRP_CAP - base);
        int added = n_pairs;
        if (added > room) {
            s->brp_dropped += (uint32_t)(added - room);
            added = room;
        }
        for (int j = 0; j < added; j++) {
            b->brp_flow[base + j]  = (j < flow_n)  ? flow_vals[j]  : SNT_MISSING;
            b->brp_press[base + j] = (j < press_n) ? press_vals[j] : SNT_MISSING;
        }
        b->n_brp = base + (uint32_t)added;
        if (added > 0) {
            s->last_flow = b->brp_flow[b->n_brp - 1];
            s->last_press = b->brp_press[b->n_brp - 1];
            s->last_brp_valid = true;
        }
    }

    /* SA2: 1 Hz, decimated from the 5 Hz report. */
    if (--s->sa2_countdown == 0) {
        s->sa2_countdown = 5;
        if (b->n_sa2 < SA2_CAP) {
            b->sa2_hr[b->n_sa2] = hr_found ? hr_val : -1;
            b->sa2_spo2[b->n_sa2] = spo2_found ? spo2_val : -1;
            b->n_sa2++;
        } else {
            s->sa2_dropped++;
        }
        if (hr_found) { s->last_hr = hr_val; s->last_hr_valid = true; }
        if (spo2_found) { s->last_spo2 = spo2_val; s->last_spo2_valid = true; }
    } else if (spo2_found && b->n_sa2 > 0) {
        b->sa2_spo2[b->n_sa2 - 1] = spo2_val;
        s->last_spo2 = spo2_val;
        s->last_spo2_valid = true;
    }

    /* PLD: 12 channels at 0.5 Hz, decimated from the 5 Hz report. */
    if (--s->pld_countdown == 0) {
        s->pld_countdown = 10;
        bool any_found = false;
        for (int k = 0; k < 12; k++) {
            if (pld_found[k]) { any_found = true; break; }
        }
        if (any_found) {
            if (b->n_pld < PLD_CAP) {
                for (int k = 0; k < 12; k++)
                    b->pld[b->n_pld][k] = pld_found[k] ? pld_vals[k] : -1;
                b->n_pld++;
                for (int k = 0; k < 12; k++)
                    s->last_pld[k] = b->pld[b->n_pld - 1][k];
                s->last_pld_valid = true;
            } else {
                s->pld_dropped++;
            }
        }
    }

    /* Hand off to the storage worker before anything can be clipped.  No
     * file I/O happens here — only a pointer swap. */
    if (b->n_brp >= BRP_COMMIT_THRESHOLD ||
        b->n_sa2 >= SA2_CAP - 5 ||
        b->n_pld >= PLD_CAP - 20) {
        swap_and_enqueue_locked(s);
    }

    xSemaphoreGive(s->fill_mutex);
}

/* ── Notification dispatch ────────────────────────────────────────── */

void session_writer_on_notification(session_writer_t *s, const cJSON *msg)
{
    if (!msg) return;

    cJSON *method = cJSON_GetObjectItem(msg, "method");
    const char *method_str = method ? method->valuestring : NULL;
    if (!method_str) return;

    ESP_LOGD(TAG, "notification: %s", method_str);

    if (strcmp(method_str, "EventNotification") == 0) {
        bool start = false, stop = false;
        const char *report = NULL;
        check_event_notification(msg, &start, &stop, &report);

        if (stop) {
            ESP_LOGI(TAG, ">>> THERAPY STOP detected");
            s_therapy_stopped = true;
            bsp_display_set_therapy_active(false);
            therapy_alert_on_therapy_stop();
            if (s && s->active) {
                write_event(s, msg);
                sw_request_finalize(s, "completed",
                                    (int64_t)time(NULL) * 1000, true);
                s = NULL;
            }
        }
        if (start) {
            ESP_LOGI(TAG, ">>> THERAPY START detected");
            s_therapy_stopped = false;
            therapy_alert_on_therapy_start();
            bsp_display_set_therapy_active(true);

            int64_t now_us = esp_timer_get_time();
            bool duplicate = false;
            if (report && report[0] && strcmp(report, s_last_start_report) == 0) {
                duplicate = true;
            } else if (s && s->active &&
                       (now_us - s->start_time_us) / 1000 < SW_START_DEBOUNCE_MS) {
                duplicate = true;
            }
            if (report && report[0])
                strlcpy(s_last_start_report, report, sizeof(s_last_start_report));

            if (s && s->active && !duplicate) {
                /* A genuine TherapyStart while a session is still open means
                 * the TherapyStop was missed (BLE dropout across the end of
                 * therapy).  Rotate: without this the sessions glue together,
                 * and an inactivity watchdog never fires because the new
                 * therapy's StreamData keeps resetting it. */
                ESP_LOGW(TAG, "TherapyStart while session active — "
                         "missed TherapyStop, rotating session");
                sw_request_finalize(s, "rotated",
                                    (int64_t)time(NULL) * 1000, true);
                s = NULL;
            } else if (duplicate && s && s->active) {
                ESP_LOGI(TAG, "duplicate TherapyStart ignored (echo)");
            }

            if (!s || !s->active) {
                s = session_writer_start();
            }
            if (s) {
                write_event(s, msg);
            } else {
                ESP_LOGW(TAG, "session_writer_start() failed — "
                         "graph active but NOT recording to SD");
            }
        }
        if (start || stop) return;

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
                return;
            }
            /* _ZLE ValueChange — the AS11 omits reportTime here, so capture
             * the NTP time at receipt and inject it.  The pair (AS11 event +
             * ntpTimeMs) is also what lets crash recovery derive a
             * session-local drift instead of trusting a stale estimate. */
            if (data_id && cJSON_IsString(data_id) &&
                strcmp(data_id->valuestring, "_ZLE") == 0) {
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
                        cJSON *zp = cJSON_GetObjectItem(zle_copy, "params");
                        if (zp) {
                            cJSON *ze = cJSON_GetObjectItem(zp, "events");
                            if (ze && cJSON_IsArray(ze)) {
                                cJSON *zev = cJSON_GetArrayItem(ze, 0);
                                if (zev) {
                                    int64_t ntp_ms = (int64_t)time(NULL) * 1000;
                                    cJSON_AddNumberToObject(zev, "ntpTimeMs",
                                                            (double)ntp_ms);
                                }
                            }
                        }
                        queue_event_json(s, cJSON_PrintUnformatted(zle_copy));
                        cJSON_Delete(zle_copy);
                    }
                }
                ESP_LOGI(TAG, ">>> _ZLE ValueChange: %d (%s)",
                         zle_val, zle_val == 1 ? "rising edge" : "falling edge");
                return;
            }
        }

        if (s && s->active) write_event(s, msg);
        return;
    }

    if (s && s->active) write_event(s, msg);
}

/* ════════════════════════════════════════════════════════════════════
 *  Crash recovery
 *
 *  Physical records are authoritative, not the header count: the
 *  buffer-full path used to write records without updating the header, so
 *  a crash can leave a stale-LOW header with complete, readable records
 *  beyond it.  Taking min(header, physical) would deliberately discard
 *  them — and because both convert_snt_to_edf() and the "no therapy"
 *  suppression read the header count, a stale-low header does not just
 *  under-report: it truncates the export or drops the session entirely.
 * ════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t records;
    int64_t  start_epoch_ms;
    bool     valid;
    bool     repaired;
} snt_scan_t;

/* Validate geometry, count complete records from the file size, and repair
 * the header in both directions. */
static snt_scan_t scan_and_repair_snt(const char *path, uint8_t want_ch,
                                      uint16_t want_hz)
{
    snt_scan_t r = {0};
    FILE *f = fopen(path, "r+b");
    if (!f) return r;

    snt_header_t hdr;
    if (fread(&hdr, 1, SNT_HEADER_SIZE, f) != SNT_HEADER_SIZE ||
        hdr.magic != SNT_MAGIC) {
        fclose(f);
        return r;
    }
    /* Reject implausible geometry rather than computing nonsense from it. */
    if (hdr.version == 0 || hdr.version > SNT_VERSION ||
        hdr.n_channels == 0 || hdr.n_channels > 16 ||
        hdr.sample_bytes != 2 || hdr.sample_hz_x10 == 0) {
        ESP_LOGW(TAG, "recover: %s has implausible geometry "
                 "(v=%u ch=%u sb=%u hz10=%u)", path,
                 hdr.version, hdr.n_channels, hdr.sample_bytes,
                 hdr.sample_hz_x10);
        fclose(f);
        return r;
    }
    if (want_ch && hdr.n_channels != want_ch) {
        ESP_LOGW(TAG, "recover: %s channel mismatch (%u != %u)",
                 path, hdr.n_channels, want_ch);
    }
    if (want_hz && hdr.sample_hz_x10 != want_hz) {
        ESP_LOGW(TAG, "recover: %s rate mismatch (%u != %u)",
                 path, hdr.sample_hz_x10, want_hz);
    }

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return r; }
    long size = ftell(f);
    if (size < (long)SNT_HEADER_SIZE) { fclose(f); return r; }

    uint32_t rec_bytes = snt_record_bytes(&hdr);
    uint64_t payload = (uint64_t)size - SNT_HEADER_SIZE;
    uint32_t physical = (uint32_t)(payload / rec_bytes);
    uint64_t partial = payload % rec_bytes;
    if (partial) {
        ESP_LOGW(TAG, "recover: %s has a %llu-byte partial trailing record "
                 "(ignored)", path, (unsigned long long)partial);
    }

    r.records = physical;
    r.start_epoch_ms = hdr.start_epoch_ms;
    r.valid = true;

    if (hdr.sample_count != physical) {
        ESP_LOGW(TAG, "recover: %s header says %u records, %u are present — "
                 "repairing header", path,
                 (unsigned)hdr.sample_count, (unsigned)physical);
        if (fseek(f, offsetof(snt_header_t, sample_count), SEEK_SET) == 0 &&
            fwrite(&physical, sizeof(uint32_t), 1, f) == 1 &&
            fflush(f) == 0) {
            fsync(fileno(f));
            r.repaired = true;
        } else {
            ESP_LOGE(TAG, "recover: header repair failed for %s", path);
        }
    }

    fclose(f);
    return r;
}

/* Parse "YYYY-MM-DDTHH:MM:SS[.mmm]Z" to epoch ms.
 *
 * reportTime is UTC in the AS11 clock domain, so it must NOT go through
 * mktime() (which would apply the local timezone offset and put the derived
 * drift hours out).  Uses the same civil-from-days algorithm as
 * edf_gen.c:parse_iso8601_utc_ms() so both agree exactly. */
static int64_t parse_iso8601_utc_ms(const char *iso_str)
{
    if (!iso_str) return -1;
    int year = 0, mon = 0, mday = 0, hour = 0, min = 0, sec = 0, ms = 0;
    int n = sscanf(iso_str, "%d-%d-%dT%d:%d:%d.%dZ",
                   &year, &mon, &mday, &hour, &min, &sec, &ms);
    if (n < 6) {
        n = sscanf(iso_str, "%d-%d-%dT%d:%d:%dZ",
                   &year, &mon, &mday, &hour, &min, &sec);
        if (n < 6) return -1;
        ms = 0;
    }
    /* Howard Hinnant's days-from-civil algorithm (public domain). */
    int y = year;
    int m = mon;
    int d = mday;
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = (int64_t)era * 146097 + (int)doe - 719468;
    int64_t secs = days * 86400 + hour * 3600 + min * 60 + sec;
    return secs * 1000 + ms;
}

/* Derive a session-local drift from a durable event carrying both the AS11
 * reportTime and the NTP time captured at receipt.  Preferred over any
 * persisted estimate because it belongs to *this* session. */
static bool drift_from_events(const char *events_path, int64_t *out_drift,
                              int64_t *out_at)
{
    FILE *f = fopen(events_path, "r");
    if (!f) return false;

    char line[1024];
    bool found = false;
    while (fgets(line, sizeof(line), f)) {
        if (!strstr(line, "ntpTimeMs")) continue;
        cJSON *root = cJSON_Parse(line);
        if (!root) continue;
        cJSON *params = cJSON_GetObjectItem(root, "params");
        cJSON *events = params ? cJSON_GetObjectItem(params, "events") : NULL;
        cJSON *ev = (events && cJSON_IsArray(events)) ?
                     cJSON_GetArrayItem(events, 0) : NULL;
        cJSON *ntp = ev ? cJSON_GetObjectItem(ev, "ntpTimeMs") : NULL;
        cJSON *rt = ev ? cJSON_GetObjectItem(ev, "reportTime") : NULL;
        if (ntp && cJSON_IsNumber(ntp) && rt && cJSON_IsString(rt)) {
            int64_t as11_ms = parse_iso8601_utc_ms(rt->valuestring);
            if (as11_ms > 0) {
                int64_t ntp_ms = (int64_t)ntp->valuedouble;
                int64_t drift = ntp_ms - as11_ms;
                /* Sanity: a plausible AS11 drift is minutes, not days.  A
                 * wild value means the pair is not trustworthy, so fall
                 * through to the persisted estimate instead. */
                if (drift > -86400000LL && drift < 86400000LL) {
                    *out_drift = drift;
                    *out_at = ntp_ms;
                    found = true;
                }
            }
        }
        cJSON_Delete(root);
        if (found) break;
    }
    fclose(f);
    return found;
}

/* Read an existing manifest and decide whether the session is already
 * final.  Presence alone must not suppress recovery: a crash can leave a
 * truncated or half-written file. */
static bool manifest_is_final(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > 16384) { fclose(f); return false; }

    char *buf = malloc(size + 1);
    if (!buf) { fclose(f); return false; }
    size_t rd = fread(buf, 1, size, f);
    fclose(f);
    buf[rd] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        ESP_LOGW(TAG, "recover: %s is not valid JSON — treating as unfinished",
                 path);
        return false;
    }
    cJSON *state = cJSON_GetObjectItem(root, "state");
    bool final = false;
    if (state && cJSON_IsString(state)) {
        const char *v = state->valuestring;
        final = (strcmp(v, "completed") == 0 || strcmp(v, "interrupted") == 0 ||
                 strcmp(v, "timed_out") == 0 || strcmp(v, "rotated") == 0 ||
                 strcmp(v, "split") == 0 || strcmp(v, "storage_failed") == 0);
    }
    cJSON_Delete(root);
    return final;
}

void session_writer_recover(void)
{
    if (!sd_storage_is_ready()) return;

    DIR *dir = opendir(SD_STREAMS_DIR);
    if (!dir) return;

    struct dirent *ent;
    int recovered = 0;

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

            const char *flow_suffix = strstr(day_ent->d_name, "_flow.snt");
            const char *brp_suffix  = strstr(day_ent->d_name, "_brp.snt");
            const char *suffix = flow_suffix ? flow_suffix : brp_suffix;
            if (!suffix) continue;
            bool is_v2 = (flow_suffix != NULL);

            size_t prefix_len = suffix - day_ent->d_name;
            if (prefix_len >= 40) continue;
            char prefix[40];
            memcpy(prefix, day_ent->d_name, prefix_len);
            prefix[prefix_len] = '\0';

            /* Leftover temp manifest from a crash mid-rename. */
            char tmp_path[420];
            snprintf(tmp_path, sizeof(tmp_path), "%s/%s_session.json.tmp",
                     day_path, prefix);
            if (unlink(tmp_path) == 0) {
                ESP_LOGW(TAG, "recover: removed stale temp manifest for %s", prefix);
            }

            char json_path[420];
            snprintf(json_path, sizeof(json_path), "%s/%s_session.json",
                     day_path, prefix);
            if (manifest_is_final(json_path)) continue;

            ESP_LOGW(TAG, "found interrupted session: %s/%s — recovering",
                     ent->d_name, prefix);

            char path[420];
            int64_t start_epoch_ms = 0;
            snt_scan_t flow_s = {0}, press_s = {0}, mm_s = {0},
                       sa2_s = {0}, pld_s = {0};

            if (is_v2) {
                snprintf(path, sizeof(path), "%s/%s_flow.snt", day_path, prefix);
                flow_s = scan_and_repair_snt(path, 1, 250);
                snprintf(path, sizeof(path), "%s/%s_press.snt", day_path, prefix);
                press_s = scan_and_repair_snt(path, 1, 250);
                snprintf(path, sizeof(path), "%s/%s_flow_mm.snt", day_path, prefix);
                mm_s = scan_and_repair_snt(path, 2, 10);
            } else {
                snprintf(path, sizeof(path), "%s/%s_brp.snt", day_path, prefix);
                flow_s = scan_and_repair_snt(path, 0, 0);
                snprintf(path, sizeof(path), "%s/%s_brp_mm.snt", day_path, prefix);
                mm_s = scan_and_repair_snt(path, 0, 0);
            }
            snprintf(path, sizeof(path), "%s/%s_sa2.snt", day_path, prefix);
            sa2_s = scan_and_repair_snt(path, 2, 10);
            snprintf(path, sizeof(path), "%s/%s_pld.snt", day_path, prefix);
            pld_s = scan_and_repair_snt(path, 12, 5);

            if (flow_s.valid) start_epoch_ms = flow_s.start_epoch_ms;

            /* v2 pair: the exported lockstep length is the shorter of the
             * two, and a mismatch is worth saying out loud. */
            uint32_t brp_records = flow_s.records;
            if (is_v2 && press_s.valid) {
                if (press_s.records != flow_s.records) {
                    ESP_LOGW(TAG, "recover: flow/press count mismatch "
                             "(%u vs %u) — using the smaller",
                             (unsigned)flow_s.records, (unsigned)press_s.records);
                }
                brp_records = flow_s.records < press_s.records
                            ? flow_s.records : press_s.records;
            }

            /* Checkpoint: newest valid slot. */
            snt_ckpt_t ck;
            bool have_ckpt = false;
            snprintf(path, sizeof(path), "%s/%s.ckpt", day_path, prefix);
            memset(&ck, 0, sizeof(ck));
            have_ckpt = ckpt_read_best(path, &ck);
            if (have_ckpt && start_epoch_ms <= 0)
                start_epoch_ms = ck.start_epoch_ms;

            /* Last resort: reconstruct the start from the session id so the
             * day bucketing stays correct instead of defaulting to epoch 0
             * and producing a 19691231 folder. */
            if (start_epoch_ms <= 0) {
                struct tm tm_id = {0};
                int y, mo, d, h, mi, sec;
                if (sscanf(prefix, "%4d%2d%2d_%2d%2d%2d",
                           &y, &mo, &d, &h, &mi, &sec) == 6) {
                    tm_id.tm_year = y - 1900; tm_id.tm_mon = mo - 1;
                    tm_id.tm_mday = d; tm_id.tm_hour = h;
                    tm_id.tm_min = mi; tm_id.tm_sec = sec;
                    tm_id.tm_isdst = -1;
                    time_t t_id = mktime(&tm_id);
                    if (t_id > 0) start_epoch_ms = (int64_t)t_id * 1000;
                }
                if (start_epoch_ms <= 0) {
                    ESP_LOGW(TAG, "skipping %s/%s — no valid header, no "
                             "checkpoint, unparseable id", ent->d_name, prefix);
                    continue;
                }
                ESP_LOGW(TAG, "recover: start time reconstructed from id for %s",
                         prefix);
            }

            /* End time, with provenance. */
            int64_t end_epoch_ms = 0;
            const char *end_source = "none";
            if (have_ckpt && ck.elapsed_us > 0) {
                end_epoch_ms = start_epoch_ms + ck.elapsed_us / 1000;
                end_source = "checkpoint";
                if (brp_records > ck.flow_count) {
                    /* Records exist beyond the checkpoint: recover them and
                     * extend the estimate by their duration rather than
                     * discarding the tail or pretending it was observed. */
                    uint32_t extra = brp_records - ck.flow_count;
                    end_epoch_ms += (int64_t)extra * 1000 / 25;
                    end_source = "checkpoint_extrapolated";
                    ESP_LOGW(TAG, "recover: %u records past checkpoint — "
                             "end extrapolated", (unsigned)extra);
                }
            } else if (brp_records > 0) {
                end_epoch_ms = start_epoch_ms + (int64_t)brp_records * 1000 / 25;
                end_source = "sample_estimate";
            }

            /* Drift: session-local first, persisted snapshot as fallback. */
            int64_t drift_ms = 0, drift_at = 0;
            const char *drift_source = "none";
            bool drift_usable = false;
            snprintf(path, sizeof(path), "%s/%s_events.snt", day_path, prefix);
            if (drift_from_events(path, &drift_ms, &drift_at)) {
                drift_source = "session_events";
                drift_usable = true;
                ESP_LOGI(TAG, "recover: drift %lld ms from session events",
                         (long long)drift_ms);
            } else {
                time_drift_snapshot_t snap;
                if (time_sync_get_drift_snapshot(&snap) && snap.available) {
                    drift_ms = snap.drift_ms;
                    drift_at = snap.measured_at_ms;
                    drift_source = snap.source;
                    drift_usable = true;
                    ESP_LOGW(TAG, "recover: drift %lld ms estimated from %s",
                             (long long)drift_ms, drift_source);
                }
            }

            /* Write the manifest atomically. */
            char tmp[440];
            snprintf(tmp, sizeof(tmp), "%s.tmp", json_path);
            FILE *f = fopen(tmp, "w");
            if (!f) {
                ESP_LOGE(TAG, "recover: cannot create %s", tmp);
                continue;
            }

            char iso_start[32] = {0};
            {
                time_t st = (time_t)(start_epoch_ms / 1000);
                struct tm tm;
                localtime_r(&st, &tm);
                strftime(iso_start, sizeof(iso_start), "%Y-%m-%dT%H:%M:%S", &tm);
            }

            fprintf(f, "{\"id\":\"%s\",\"state\":\"interrupted\","
                       "\"start_epoch_ms\":%lld",
                    prefix, (long long)start_epoch_ms);
            if (end_epoch_ms > 0)
                fprintf(f, ",\"end_epoch_ms\":%lld", (long long)end_epoch_ms);
            fprintf(f, ",\"end_source\":\"%s\"", end_source);
            fprintf(f, ",\"clock_drift_ms\":%lld,\"clock_drift_valid\":false",
                    (long long)drift_ms);
            fprintf(f, ",\"clock_drift_source\":\"%s\"", drift_source);
            fprintf(f, ",\"clock_drift_usable\":%s", drift_usable ? "true" : "false");
            if (drift_at > 0)
                fprintf(f, ",\"clock_drift_measured_at_ms\":%lld", (long long)drift_at);
            fprintf(f, ",\"brp_samples\":%u,\"brp_mm_samples\":%u,"
                       "\"sa2_samples\":%u,\"pld_samples\":%u",
                    (unsigned)brp_records, (unsigned)mm_s.records,
                    (unsigned)sa2_s.records, (unsigned)pld_s.records);
            if (is_v2)
                fprintf(f, ",\"fmt\":2,\"press_samples\":%u",
                        (unsigned)press_s.records);
            if (have_ckpt)
                fprintf(f, ",\"checkpoint_seq\":%u", (unsigned)ck.seq);
            if (iso_start[0])
                fprintf(f, ",\"start_iso\":\"%s\"", iso_start);
            if (s_device_addr[0])
                fprintf(f, ",\"as11_device\":\"%s\"", s_device_addr);
            if (s_client_id[0])
                fprintf(f, ",\"as11_client_id\":\"%s\"", s_client_id);
            fprintf(f, "}\n");

            bool ok = (fflush(f) == 0) && (fsync(fileno(f)) == 0);
            if (fclose(f) != 0) ok = false;
            if (ok) {
                unlink(json_path);
                ok = (rename(tmp, json_path) == 0);
            }
            if (!ok) {
                unlink(tmp);
                ESP_LOGE(TAG, "recover: failed to write %s", json_path);
                continue;
            }

            ESP_LOGI(TAG, "recovered %s: brp=%u end_source=%s drift=%s",
                     prefix, (unsigned)brp_records, end_source, drift_source);
            recovered++;
        }
        closedir(day_dir);
    }

    closedir(dir);
    if (recovered > 0) {
        ESP_LOGI(TAG, "recovered %d interrupted session(s)", recovered);
        ESP_LOGI(TAG, "use the Web UI 'rebuild day' action to export them");
    }
}
