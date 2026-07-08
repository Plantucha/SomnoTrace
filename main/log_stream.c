/*
 * SomnoTrace - Log stream: ring-buffered log capture with SSE delivery
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

#include "log_stream.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "cJSON.h"
#include "sd_storage.h"
#include "psram_task.h"

static const char *TAG = "log_stream";

/* ── Ring Buffer ──────────────────────────────────────────────────── */

#define RINGBUF_SIZE_INTERNAL   (8  * 1024)
#define RINGBUF_SIZE_PSRAM      (16 * 1024)
#define LOG_LINE_MAX            256

/* ── SD Persistent Logging ───────────────────────────────────────── */

#define LOG_DIR             SD_MOUNT_POINT "/.logs"
#define LOG_FILE_PREFIX     "somnotrace.log."
#define LOG_MAX_FILES       3
#define LOG_FILE_MAX_SIZE   (40 * 1024)   /* 40 KB per file */
#define LOG_TOTAL_MAX       (LOG_FILE_MAX_SIZE * LOG_MAX_FILES)  /* 120 KB */
#define WRITEBUF_SIZE       (8 * 1024)    /* PSRAM write buffer */
#define FLUSH_INTERVAL_MS   2000          /* flush at least every 2 s */
#define FLUSH_THRESHOLD     4096          /* flush when buffer reaches 4 KB */

static RingbufHandle_t s_ringbuf;
static vprintf_like_t  s_orig_vprintf;

/* Write buffer for SD persistence (separate from SSE ring buffer). */
static uint8_t *s_writebuf;          /* PSRAM buffer */
static size_t   s_writebuf_head;     /* write position (from vprintf hook) */
static size_t   s_writebuf_tail;     /* read position (from flush task) */
static SemaphoreHandle_t s_writebuf_mutex;
static TaskHandle_t s_flush_task;
static bool s_sd_ready;              /* SD card is mounted and log dir created */

/* ── WebSocket Live Tail ──────────────────────────────────────────── */
/* Single active viewer model (matches the old SSE guard).  The forwarder
 * task drains the ring buffer continuously and pushes raw text frames via
 * httpd_ws_send_frame_async — the officially supported API for sending WS
 * frames from outside a request-handler context, so this never touches
 * the shared httpd worker task. */
static httpd_handle_t   s_ws_hd = NULL;
static int              s_ws_fd = -1;
static SemaphoreHandle_t s_ws_mutex;
static TaskHandle_t     s_ws_fwd_task;

/**
 * Custom vprintf hook installed via esp_log_set_vprintf().
 *
 * 1. Forward to the original UART handler (so serial console keeps working).
 * 2. Render the formatted string into a scratch buffer.
 * 3. Push it into the ring buffer (best-effort, drop if full).
 * 4. Notify the SD flush task if enough data has accumulated.
 */

static int log_vprintf_hook(const char *fmt, va_list args)
{
    /* Always forward to UART first. */
    int ret = s_orig_vprintf(fmt, args);

    /* Render into a stack-local scratch buffer. */
    char buf[LOG_LINE_MAX];
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    if (len <= 0) {
        return ret;
    }
    if (len >= (int)sizeof(buf)) {
        len = sizeof(buf) - 1;
    }

    /* Push to SSE ring buffer (best-effort, drop if full). */
    if (s_ringbuf) {
        xRingbufferSend(s_ringbuf, buf, (size_t)len, 0);
    }

    /* Push to SD write buffer (best-effort, drop if full or mutex busy). */
    if (s_writebuf && xSemaphoreTake(s_writebuf_mutex, 0) == pdTRUE) {
        size_t avail = WRITEBUF_SIZE - (s_writebuf_head - s_writebuf_tail);
        if ((size_t)len <= avail) {
            size_t pos = s_writebuf_head % WRITEBUF_SIZE;
            size_t chunk1 = WRITEBUF_SIZE - pos;
            if ((size_t)len <= chunk1) {
                memcpy(s_writebuf + pos, buf, (size_t)len);
            } else {
                memcpy(s_writebuf + pos, buf, chunk1);
                memcpy(s_writebuf, buf + chunk1, (size_t)len - chunk1);
            }
            s_writebuf_head += (size_t)len;
        }
        xSemaphoreGive(s_writebuf_mutex);
    }

    /* Wake the SD flush task if enough data has accumulated. */
    if (s_flush_task && s_writebuf_head - s_writebuf_tail >= FLUSH_THRESHOLD) {
        xTaskNotifyGive(s_flush_task);
    }

    return ret;
}

/* ── SD Persistent Logging ───────────────────────────────────────── */

/* Ensure the log directory exists on the SD card. */
static void ensure_log_dir(void)
{
    if (!sd_storage_is_ready()) return;
    mkdir(LOG_DIR, 0777);  /* ignore EEXIST */
    s_sd_ready = true;
}

/* Rotate log files: delete oldest, shift others up, create new .0 */
static void rotate_logs(void)
{
    char path[64];

    /* Delete the oldest file (somnotrace.log.2) */
    snprintf(path, sizeof(path), "%s%s%d", LOG_DIR, LOG_FILE_PREFIX, LOG_MAX_FILES - 1);
    remove(path);

    /* Shift: .1 -> .2, .0 -> .1 */
    for (int i = LOG_MAX_FILES - 2; i >= 0; i--) {
        char old_path[64], new_path[64];
        snprintf(old_path, sizeof(old_path), "%s%s%d", LOG_DIR, LOG_FILE_PREFIX, i);
        snprintf(new_path, sizeof(new_path), "%s%s%d", LOG_DIR, LOG_FILE_PREFIX, i + 1);
        rename(old_path, new_path);
    }
}

/* Get current log file size. */
static long log_file_size(void)
{
    char path[64];
    snprintf(path, sizeof(path), "%s%s0", LOG_DIR, LOG_FILE_PREFIX);
    struct stat st;
    if (stat(path, &st) == 0) return st.st_size;
    return 0;
}

/* Background flush task: drains write buffer to SD card. */
static void log_flush_task(void *arg)
{
    while (true) {
        /* Wait for notification or timeout (periodic flush). */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(FLUSH_INTERVAL_MS));

        if (!s_sd_ready) {
            ensure_log_dir();
            if (!s_sd_ready) continue;
        }

        size_t avail;
        xSemaphoreTake(s_writebuf_mutex, portMAX_DELAY);
        avail = s_writebuf_head - s_writebuf_tail;
        xSemaphoreGive(s_writebuf_mutex);

        if (avail == 0) continue;

        /* Check if we need to rotate before writing. */
        long cur_size = log_file_size();
        if (cur_size >= LOG_FILE_MAX_SIZE) {
            rotate_logs();
            cur_size = 0;
        }

        /* Open current log file for append. */
        char path[64];
        snprintf(path, sizeof(path), "%s%s0", LOG_DIR, LOG_FILE_PREFIX);
        FILE *f = fopen(path, "ab");
        if (!f) {
            ESP_LOGE(TAG, "flush: cannot open %s (errno %d)", path, errno);
            continue;
        }

        /* Drain available data from write buffer. */
        uint8_t tmp[LOG_LINE_MAX];
        size_t total_written = 0;

        while (true) {
            xSemaphoreTake(s_writebuf_mutex, portMAX_DELAY);
            size_t pending = s_writebuf_head - s_writebuf_tail;
            if (pending == 0) {
                xSemaphoreGive(s_writebuf_mutex);
                break;
            }
            size_t pos = s_writebuf_tail % WRITEBUF_SIZE;
            size_t chunk = WRITEBUF_SIZE - pos;
            if (chunk > pending) chunk = pending;
            if (chunk > sizeof(tmp)) chunk = sizeof(tmp);
            memcpy(tmp, s_writebuf + pos, chunk);
            s_writebuf_tail += chunk;
            xSemaphoreGive(s_writebuf_mutex);

            size_t written = fwrite(tmp, 1, chunk, f);
            total_written += written;
            if (written != chunk) break;

            /* Check rotation mid-flush. */
            if (cur_size + (long)total_written >= LOG_FILE_MAX_SIZE) {
                fclose(f);
                rotate_logs();
                cur_size = 0;
                total_written = 0;
                snprintf(path, sizeof(path), "%s%s0", LOG_DIR, LOG_FILE_PREFIX);
                f = fopen(path, "ab");
                if (!f) break;
            }
        }

        if (f) fclose(f);
    }
}

/* ── Initialisation ───────────────────────────────────────────────── */

void log_stream_init(void)
{
    /* Prefer PSRAM (larger buffer) if available, else internal RAM. */
    size_t buf_sz;
    if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) > RINGBUF_SIZE_PSRAM * 2) {
        s_ringbuf = xRingbufferCreateWithCaps(RINGBUF_SIZE_PSRAM,
                                              RINGBUF_TYPE_BYTEBUF,
                                              MALLOC_CAP_SPIRAM);
        buf_sz = RINGBUF_SIZE_PSRAM;
    } else {
        s_ringbuf = xRingbufferCreateWithCaps(RINGBUF_SIZE_INTERNAL,
                                              RINGBUF_TYPE_BYTEBUF,
                                              MALLOC_CAP_INTERNAL);
        buf_sz = RINGBUF_SIZE_INTERNAL;
    }
    if (!s_ringbuf) {
        ESP_LOGE(TAG, "failed to create log ring buffer");
        return;
    }

    /* Allocate SD write buffer in PSRAM. */
    s_writebuf = heap_caps_malloc(WRITEBUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!s_writebuf) {
        s_writebuf = heap_caps_malloc(WRITEBUF_SIZE, MALLOC_CAP_INTERNAL);
    }
    if (s_writebuf) {
        s_writebuf_mutex = xSemaphoreCreateMutex();
        if (!s_writebuf_mutex) {
            free(s_writebuf);
            s_writebuf = NULL;
            ESP_LOGW(TAG, "failed to create write buffer mutex, SD logging disabled");
        }
    } else {
        ESP_LOGW(TAG, "failed to allocate write buffer, SD logging disabled");
    }

    ESP_LOGI(TAG, "log stream init: %u-byte ring buffer (%s), %s SD logging",
             (unsigned)buf_sz,
             buf_sz == RINGBUF_SIZE_PSRAM ? "PSRAM" : "internal",
             s_writebuf ? "with" : "without");

    /* Install our hook; stash the original handler. */
    s_orig_vprintf = esp_log_set_vprintf(log_vprintf_hook);

    /* Start the SD flush task (low priority, core 0). */
    if (s_writebuf) {
        s_flush_task = psram_task_create(log_flush_task, "log_flush", 4096, NULL, 3, 0, NULL, NULL);
    }

    /* Create the WebSocket mutex (protects s_ws_hd/s_ws_fd). */
    s_ws_mutex = xSemaphoreCreateMutex();
}

/* ── WebSocket Live Tail: GET /api/logs/ws ────────────────────────── */
/* The handler runs only for the WS handshake and incoming control frames;
 * it returns immediately so the httpd worker task is never blocked.  A
 * separate forwarder task drains the ring buffer and pushes text frames
 * via httpd_ws_send_frame_async — the officially supported API for
 * sending WS frames outside a handler context. */

static void ws_forwarder_task(void *arg)
{
    (void)arg;
    /* Accumulate lines into a single frame buffer to reduce the number
     * of socket writes.  Send when we hit a threshold or after a timeout. */
    char *frame_buf = heap_caps_malloc(LOG_LINE_MAX * 16, MALLOC_CAP_SPIRAM);
    if (!frame_buf) {
        ESP_LOGE(TAG, "ws_fwd: failed to allocate frame buffer");
        vTaskDelete(NULL);
        return;
    }
    size_t frame_cap = LOG_LINE_MAX * 16;
    size_t frame_pos = 0;

    while (true) {
        /* Snapshot the current WS connection under the mutex. */
        httpd_handle_t hd = NULL;
        int fd = -1;
        if (xSemaphoreTake(s_ws_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            hd = s_ws_hd;
            fd = s_ws_fd;
            xSemaphoreGive(s_ws_mutex);
        }

        if (!hd || fd < 0) {
            /* No active viewer — sleep and re-check. */
            vTaskDelay(pdMS_TO_TICKS(200));
            frame_pos = 0;
            continue;
        }

        /* Try to drain available ring-buffer data (non-blocking). */
        size_t item_sz = 0;
        void *item = xRingbufferReceiveUpTo(s_ringbuf, &item_sz, 0, LOG_LINE_MAX);
        if (item) {
            /* Append to frame buffer; flush if near capacity. */
            size_t copy_len = item_sz;
            if (frame_pos + copy_len > frame_cap - 1) {
                copy_len = frame_cap - 1 - frame_pos;
            }
            memcpy(frame_buf + frame_pos, item, copy_len);
            frame_pos += copy_len;
            vRingbufferReturnItem(s_ringbuf, item);

            /* Send immediately if we have a decent batch. */
            if (frame_pos >= 256) {
                httpd_ws_frame_t ws_pkt = {
                    .final = true,
                    .type = HTTPD_WS_TYPE_TEXT,
                    .payload = (uint8_t *)frame_buf,
                    .len = frame_pos,
                };
                esp_err_t err = httpd_ws_send_frame_async(hd, fd, &ws_pkt);
                if (err != ESP_OK) {
                    /* Client likely disconnected — clear the slot. */
                    if (xSemaphoreTake(s_ws_mutex, 0) == pdTRUE) {
                        if (s_ws_fd == fd) { s_ws_hd = NULL; s_ws_fd = -1; }
                        xSemaphoreGive(s_ws_mutex);
                    }
                }
                frame_pos = 0;
            }
        } else {
            /* No data available — send any pending fragment, then wait. */
            if (frame_pos > 0) {
                httpd_ws_frame_t ws_pkt = {
                    .final = true,
                    .type = HTTPD_WS_TYPE_TEXT,
                    .payload = (uint8_t *)frame_buf,
                    .len = frame_pos,
                };
                httpd_ws_send_frame_async(hd, fd, &ws_pkt);
                frame_pos = 0;
            }
            /* Brief sleep so we don't busy-spin on an empty ring buffer. */
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
}

static esp_err_t logs_ws_handler(httpd_req_t *req)
{
    /* Handle WS handshake — the httpd framework does the actual upgrade
     * automatically when .is_websocket = true.  We only get called for
     * the initial request and for incoming WS control frames. */
    if (req->method == HTTP_GET) {
        /* WS handshake: store the handle + fd for the forwarder task. */
        if (xSemaphoreTake(s_ws_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
            /* If a previous viewer was connected, let it go. */
            s_ws_hd = req->handle;
            s_ws_fd = httpd_req_to_sockfd(req);
            xSemaphoreGive(s_ws_mutex);
            ESP_LOGI(TAG, "ws: client connected (fd=%d)", s_ws_fd);
        }

        /* Start the forwarder task if not already running. */
        if (!s_ws_fwd_task) {
            s_ws_fwd_task = psram_task_create(ws_forwarder_task, "ws_fwd", 4096,
                                               NULL, 3, 0, NULL, NULL);
        }
        return ESP_OK;
    }

    /* For incoming WS frames (client may send close/ping). */
    httpd_ws_frame_t ws_pkt;
    uint8_t buf[32] = {0};
    memset(&ws_pkt, 0, sizeof(ws_pkt));
    ws_pkt.payload = buf;
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;

    if (httpd_ws_recv_frame(req, &ws_pkt, sizeof(buf)) != ESP_OK) {
        return ESP_FAIL;
    }

    /* If the client sent a close frame, clear the slot. */
    if (ws_pkt.type == HTTPD_WS_TYPE_CLOSE) {
        if (xSemaphoreTake(s_ws_mutex, 0) == pdTRUE) {
            s_ws_hd = NULL;
            s_ws_fd = -1;
            xSemaphoreGive(s_ws_mutex);
            ESP_LOGI(TAG, "ws: client disconnected");
        }
    }

    return ESP_OK;
}

/* ── Polling Endpoint: GET /api/logs/recent ───────────────────────── */
/* Replaces the old SSE /api/logs/stream endpoint.  Each request drains
 * available ring-buffer data and returns it as a JSON array, then closes
 * immediately — no long-lived connection, no blocking of the single httpd
 * worker task.  The frontend polls this every 1-2 seconds.
 *
 * Optional query parameter: ?since=<cursor>  — returns only lines produced
 * after the given cursor value (monotonic counter).  Omit or pass 0 to get
 * all currently-buffered lines.  Response includes the new cursor value so
 * the client can pass it on the next poll.
 *
 * Response format:
 *   {"lines":["line1","line2",...],"cursor":N}
 */

static esp_err_t logs_recent_handler(httpd_req_t *req)
{
    /* Parse optional ?since=<cursor> query parameter. */
    int since = 0;
    char query[32] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char val[16] = {0};
        if (httpd_query_key_value(query, "since", val, sizeof(val)) == ESP_OK) {
            since = atoi(val);
            if (since < 0) since = 0;
        }
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    /* Build the entire JSON response in a single buffer to avoid
     * hundreds of tiny chunked sends (each one a socket write that
     * generates ENOTCONN spam if the client has disconnected). */
    char *buf = heap_caps_malloc(8192, MALLOC_CAP_SPIRAM);
    if (!buf) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    size_t cap = 8192;
    size_t pos = 0;

    pos += snprintf(buf + pos, cap - pos, "{\"lines\":[");

    int local_cursor = since;
    int chunks_sent = 0;

    /* Drain available ring-buffer data, non-blocking. */
    while (true) {
        size_t item_sz = 0;
        void *item = xRingbufferReceiveUpTo(s_ringbuf, &item_sz, 0, LOG_LINE_MAX);
        if (!item) break;

        local_cursor++;

        /* Split chunk into individual lines on '\n' and emit as JSON strings. */
        const char *p   = (const char *)item;
        const char *end = p + item_sz;

        while (p < end) {
            const char *nl = memchr(p, '\n', (size_t)(end - p));
            size_t line_len = nl ? (size_t)(nl - p) : (size_t)(end - p);

            /* Strip trailing CR from CRLF lines. */
            if (line_len > 0 && p[line_len - 1] == '\r') {
                line_len--;
            }

            if (line_len > 0) {
                if (chunks_sent > 0) {
                    if (pos + 1 >= cap) goto buf_full;
                    buf[pos++] = ',';
                }

                if (pos + 1 >= cap) goto buf_full;
                buf[pos++] = '"';

                /* Escape JSON-special characters. */
                for (size_t i = 0; i < line_len; i++) {
                    char c = p[i];
                    if (c == '\\' || c == '"') {
                        if (pos + 2 >= cap) goto buf_full;
                        buf[pos++] = '\\';
                        buf[pos++] = c;
                    } else if ((unsigned char)c < 0x20) {
                        if (pos + 6 >= cap) goto buf_full;
                        pos += snprintf(buf + pos, cap - pos, "\\u%04x", (unsigned char)c);
                    } else {
                        if (pos + 1 >= cap) goto buf_full;
                        buf[pos++] = c;
                    }
                }

                if (pos + 1 >= cap) goto buf_full;
                buf[pos++] = '"';
                chunks_sent++;
            }

            p = nl ? nl + 1 : end;
        }

        vRingbufferReturnItem(s_ringbuf, item);
    }

buf_full:
    pos += snprintf(buf + pos, cap - pos, "],\"cursor\":%d}", local_cursor);

    httpd_resp_send(req, buf, pos);
    free(buf);
    return ESP_OK;
}

/* ── History Endpoint: GET /api/logs/history ─────────────────────── */

static esp_err_t logs_history_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

    bool got_any = false;

    /* Read log files from oldest (.2) to newest (.0). */
    if (s_sd_ready || sd_storage_is_ready()) {
        for (int i = LOG_MAX_FILES - 1; i >= 0; i--) {
            char path[64];
            snprintf(path, sizeof(path), "%s%s%d", LOG_DIR, LOG_FILE_PREFIX, i);
            FILE *f = fopen(path, "rb");
            if (!f) continue;

            char chunk[1024];
            size_t n;
            while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) {
                if (httpd_resp_send_chunk(req, chunk, (ssize_t)n) != ESP_OK) {
                    fclose(f);
                    goto hist_done;
                }
                got_any = true;
            }
            fclose(f);
        }
    }

    /* Also include current write buffer contents (not yet flushed to SD). */
    if (s_writebuf && s_writebuf_mutex) {
        xSemaphoreTake(s_writebuf_mutex, portMAX_DELAY);
        size_t pending = s_writebuf_head - s_writebuf_tail;
        if (pending > 0) {
            uint8_t *tmp = malloc(pending);
            if (tmp) {
                size_t pos = s_writebuf_tail % WRITEBUF_SIZE;
                size_t chunk1 = WRITEBUF_SIZE - pos;
                if (pending <= chunk1) {
                    memcpy(tmp, s_writebuf + pos, pending);
                } else {
                    memcpy(tmp, s_writebuf + pos, chunk1);
                    memcpy(tmp + chunk1, s_writebuf, pending - chunk1);
                }
                httpd_resp_send_chunk(req, (const char *)tmp, (ssize_t)pending);
                free(tmp);
                got_any = true;
            }
        }
        xSemaphoreGive(s_writebuf_mutex);
    }

    if (!got_any) {
        httpd_resp_send_chunk(req, "(no log history available)\n", -1);
    }

hist_done:
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* ── Download Endpoint: GET /api/logs/download ────────────────────── */

static esp_err_t logs_download_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Content-Disposition",
                       "attachment; filename=\"somnotrace-logs.txt\"");

    bool got_any = false;

    /* Include SD history files (oldest to newest). */
    if (s_sd_ready || sd_storage_is_ready()) {
        for (int i = LOG_MAX_FILES - 1; i >= 0; i--) {
            char path[64];
            snprintf(path, sizeof(path), "%s%s%d", LOG_DIR, LOG_FILE_PREFIX, i);
            FILE *f = fopen(path, "rb");
            if (!f) continue;

            char chunk[1024];
            size_t n;
            while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) {
                if (httpd_resp_send_chunk(req, chunk, (ssize_t)n) != ESP_OK) {
                    fclose(f);
                    goto dl_done;
                }
                got_any = true;
            }
            fclose(f);
        }
    }

    /* Include current write buffer (not yet flushed). */
    if (s_writebuf && s_writebuf_mutex) {
        xSemaphoreTake(s_writebuf_mutex, portMAX_DELAY);
        size_t pending = s_writebuf_head - s_writebuf_tail;
        if (pending > 0) {
            uint8_t *tmp = malloc(pending);
            if (tmp) {
                size_t pos = s_writebuf_tail % WRITEBUF_SIZE;
                size_t chunk1 = WRITEBUF_SIZE - pos;
                if (pending <= chunk1) {
                    memcpy(tmp, s_writebuf + pos, pending);
                } else {
                    memcpy(tmp, s_writebuf + pos, chunk1);
                    memcpy(tmp + chunk1, s_writebuf, pending - chunk1);
                }
                httpd_resp_send_chunk(req, (const char *)tmp, (ssize_t)pending);
                free(tmp);
                got_any = true;
            }
        }
        xSemaphoreGive(s_writebuf_mutex);
    }

    if (!got_any) {
        httpd_resp_send_chunk(req, "(no log data available)\n", -1);
    }

dl_done:
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* ── Log Level Endpoint: POST /api/logs/level ─────────────────────── */

static const char *level_to_str(esp_log_level_t level)
{
    switch (level) {
        case ESP_LOG_NONE:    return "none";
        case ESP_LOG_ERROR:   return "error";
        case ESP_LOG_WARN:    return "warn";
        case ESP_LOG_INFO:    return "info";
        case ESP_LOG_DEBUG:   return "debug";
        case ESP_LOG_VERBOSE: return "verbose";
        default:              return "unknown";
    }
}

static esp_log_level_t str_to_level(const char *s)
{
    if (strcmp(s, "error")   == 0) return ESP_LOG_ERROR;
    if (strcmp(s, "warn")    == 0) return ESP_LOG_WARN;
    if (strcmp(s, "info")    == 0) return ESP_LOG_INFO;
    if (strcmp(s, "debug")   == 0) return ESP_LOG_DEBUG;
    if (strcmp(s, "verbose") == 0) return ESP_LOG_VERBOSE;
    if (strcmp(s, "none")    == 0) return ESP_LOG_NONE;
    return (esp_log_level_t)-1;
}

/* Cached current global level so we can report it back to the UI.
 * ESP-IDF doesn't expose a getter for the global default. */
static esp_log_level_t s_current_level = ESP_LOG_INFO;

static esp_err_t logs_level_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        /* Return current level as JSON. */
        char resp[64];
        snprintf(resp, sizeof(resp), "{\"level\":\"%s\"}", level_to_str(s_current_level));
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, resp);
        return ESP_OK;
    }

    /* POST — set the global log level. */
    char body[64];
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
        return ESP_FAIL;
    }
    body[len] = '\0';

    cJSON *json = cJSON_Parse(body);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
        return ESP_FAIL;
    }

    cJSON *lvl_item = cJSON_GetObjectItem(json, "level");
    if (!cJSON_IsString(lvl_item)) {
        cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing 'level' string");
        return ESP_FAIL;
    }

    esp_log_level_t new_level = str_to_level(lvl_item->valuestring);
    cJSON_Delete(json);

    if ((int)new_level < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "invalid level — use: none, error, warn, info, debug, verbose");
        return ESP_FAIL;
    }

    /* Apply globally (wildcard "*" sets the default for all tags). */
    esp_log_level_set("*", new_level);
    s_current_level = new_level;

    ESP_LOGI(TAG, "global log level changed to %s", level_to_str(new_level));

    char resp[64];
    snprintf(resp, sizeof(resp), "{\"level\":\"%s\"}", level_to_str(new_level));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

/* ── Handler Registration ─────────────────────────────────────────── */

void log_stream_register_handlers(httpd_handle_t server)
{
    httpd_uri_t recent = {
        .uri     = "/api/logs/recent",
        .method  = HTTP_GET,
        .handler = logs_recent_handler,
    };
    httpd_register_uri_handler(server, &recent);

    httpd_uri_t download = {
        .uri     = "/api/logs/download",
        .method  = HTTP_GET,
        .handler = logs_download_handler,
    };
    httpd_register_uri_handler(server, &download);

    httpd_uri_t level_get = {
        .uri     = "/api/logs/level",
        .method  = HTTP_GET,
        .handler = logs_level_handler,
    };
    httpd_register_uri_handler(server, &level_get);

    httpd_uri_t level_post = {
        .uri     = "/api/logs/level",
        .method  = HTTP_POST,
        .handler = logs_level_handler,
    };
    httpd_register_uri_handler(server, &level_post);

    httpd_uri_t history = {
        .uri     = "/api/logs/history",
        .method  = HTTP_GET,
        .handler = logs_history_handler,
    };
    httpd_register_uri_handler(server, &history);

    httpd_uri_t ws = {
        .uri        = "/api/logs/ws",
        .method     = HTTP_GET,
        .handler    = logs_ws_handler,
        .is_websocket = true,
    };
    httpd_register_uri_handler(server, &ws);

    ESP_LOGI(TAG, "registered /api/logs/{recent,ws,download,history,level}");
}
