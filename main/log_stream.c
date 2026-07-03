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
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "cJSON.h"

static const char *TAG = "log_stream";

/* ── Ring Buffer ──────────────────────────────────────────────────── */

#define RINGBUF_SIZE_INTERNAL   (8  * 1024)
#define RINGBUF_SIZE_PSRAM      (16 * 1024)
#define LOG_LINE_MAX            256

static RingbufHandle_t s_ringbuf;
static vprintf_like_t  s_orig_vprintf;

/**
 * Custom vprintf hook installed via esp_log_set_vprintf().
 *
 * 1. Forward to the original UART handler (so serial console keeps working).
 * 2. Render the formatted string into a scratch buffer.
 * 3. Push it into the ring buffer (best-effort, drop if full).
 * 4. Notify an SSE consumer task if one is waiting.
 */
static TaskHandle_t s_sse_task;

static int log_vprintf_hook(const char *fmt, va_list args)
{
    /* Always forward to UART first. */
    int ret = s_orig_vprintf(fmt, args);

    if (!s_ringbuf) {
        return ret;
    }

    /* Render into a stack-local scratch buffer. */
    char buf[LOG_LINE_MAX];
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    if (len <= 0) {
        return ret;
    }
    if (len >= (int)sizeof(buf)) {
        len = sizeof(buf) - 1;
    }

    /* Best-effort write — drop if the buffer is full.  Use the ISR-safe
     * variant because ESP_LOGx can occasionally fire from ISR-adjacent
     * contexts (timer callbacks, BLE notification handlers, etc.).
     * In non-ISR context this still works fine. */
    xRingbufferSend(s_ringbuf, buf, (size_t)len, 0);

    /* Wake the SSE sender if it is blocked waiting for data. */
    TaskHandle_t t = s_sse_task;
    if (t) {
        xTaskNotifyGive(t);
    }

    return ret;
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

    ESP_LOGI(TAG, "log stream init: %u-byte ring buffer (%s)",
             (unsigned)buf_sz,
             buf_sz == RINGBUF_SIZE_PSRAM ? "PSRAM" : "internal");

    /* Install our hook; stash the original handler. */
    s_orig_vprintf = esp_log_set_vprintf(log_vprintf_hook);
}

/* ── SSE Endpoint: GET /api/logs/stream ───────────────────────────── */

static esp_err_t logs_stream_handler(httpd_req_t *req)
{
    /* Only one SSE viewer at a time. */
    if (s_sse_task) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "Another log viewer is already connected.");
        return ESP_OK;
    }

    /* Claim the slot. */
    s_sse_task = xTaskGetCurrentTaskHandle();

    /* SSE headers. */
    httpd_resp_set_type(req, "text/event-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Connection", "keep-alive");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    /* Send a comment line so the browser knows the stream is live. */
    httpd_resp_send_chunk(req, ": connected\n\n", -1);

    /* Drain any buffered data first, then enter the live loop. */
    while (true) {
        size_t item_sz = 0;
        void *item = xRingbufferReceiveUpTo(s_ringbuf, &item_sz,
                                            pdMS_TO_TICKS(2000),
                                            LOG_LINE_MAX);
        if (!item) {
            /* No data for 2 s — send an SSE comment as a keep-alive. */
            esp_err_t err = httpd_resp_send_chunk(req, ": keepalive\n\n", -1);
            if (err != ESP_OK) {
                break;  /* Client disconnected. */
            }
            continue;
        }

        /* Build the SSE frame: "data: <text>\n\n"
         * Log lines may contain embedded newlines; we need to split them
         * into separate "data:" lines per SSE spec. */
        const char *p   = (const char *)item;
        const char *end = p + item_sz;

        while (p < end) {
            const char *nl = memchr(p, '\n', (size_t)(end - p));
            size_t line_len = nl ? (size_t)(nl - p) : (size_t)(end - p);

            /* Skip empty trailing newlines to avoid blank events. */
            if (line_len == 0) {
                p = nl ? nl + 1 : end;
                continue;
            }

            /* "data: " prefix */
            esp_err_t err = httpd_resp_send_chunk(req, "data: ", 6);
            if (err != ESP_OK) { goto disconnected; }

            /* The line content itself. */
            err = httpd_resp_send_chunk(req, p, (ssize_t)line_len);
            if (err != ESP_OK) { goto disconnected; }

            /* End of this SSE data field + blank line = dispatch event. */
            err = httpd_resp_send_chunk(req, "\n\n", 2);
            if (err != ESP_OK) { goto disconnected; }

            p = nl ? nl + 1 : end;
        }

        vRingbufferReturnItem(s_ringbuf, item);
        continue;

    disconnected:
        vRingbufferReturnItem(s_ringbuf, item);
        break;
    }

    /* Clean up: release the slot. */
    s_sse_task = NULL;
    ESP_LOGI(TAG, "SSE client disconnected");
    return ESP_OK;
}

/* ── Download Endpoint: GET /api/logs/download ────────────────────── */

static esp_err_t logs_download_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Content-Disposition",
                       "attachment; filename=\"somnotrace-logs.txt\"");

    /* Drain whatever is currently in the ring buffer. */
    bool got_any = false;
    while (true) {
        size_t item_sz = 0;
        void *item = xRingbufferReceiveUpTo(s_ringbuf, &item_sz, 0,
                                            LOG_LINE_MAX);
        if (!item) break;

        httpd_resp_send_chunk(req, (const char *)item, (ssize_t)item_sz);
        vRingbufferReturnItem(s_ringbuf, item);
        got_any = true;
    }

    if (!got_any) {
        httpd_resp_send_chunk(req, "(no log data in buffer)\n", -1);
    }

    /* End chunked response. */
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
    httpd_uri_t stream = {
        .uri     = "/api/logs/stream",
        .method  = HTTP_GET,
        .handler = logs_stream_handler,
    };
    httpd_register_uri_handler(server, &stream);

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

    ESP_LOGI(TAG, "registered /api/logs/{stream,download,level}");
}
