/*
 * SomnoTrace - SleepHQ upload backend using raw esp_tls socket
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
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_random.h"
#include "esp_tls.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "mbedtls/md5.h"

/* SD card paths — must match sd_storage.h */
#define SD_MOUNT_POINT      "/somnotrace"
#define SD_SDCARD_DIR       SD_MOUNT_POINT "/SDCARD"
#define SD_SDCARD_DATALOG   SD_SDCARD_DIR "/DATALOG"
#define SD_SDCARD_SETTINGS  SD_SDCARD_DIR "/SETTINGS"

static const char *TAG = "upload_shq";

#define SHQ_HOST        "sleephq.com"
#define SHQ_PORT        443
#define SHQ_URL_BASE    "https://sleephq.com"
#define SHQ_TOKEN_PATH  "/oauth/token"
#define SHQ_ME_PATH     "/api/v1/me"
#define SHQ_IMPORTS_FMT "/api/v1/teams/%s/imports"
#define SHQ_FILES_FMT   "/api/v1/imports/%s/files"
#define SHQ_PROCESS_FMT "/api/v1/imports/%s/process_files"

#define SHQ_TIMEOUT_MS  30000
#define SHQ_READ_BUF    1024
#define SHQ_RESP_CAP    4096

/* Token cache */
static char s_token[512] = {0};
static int64_t s_token_time_s = 0;
static int s_token_expires = 0;
static char s_team_id[32] = {0};

/* ── TLS socket layer ───────────────────────────────────────────────── */

static int shq_tls_write_all(esp_tls_t *tls, const void *data, size_t len)
{
    const char *p = (const char *)data;
    size_t total = 0;
    int write_calls = 0;
    while (total < len) {
        ssize_t w = esp_tls_conn_write(tls, p + total, len - total);
        write_calls++;
        if (w < 0) {
            ESP_LOGE(TAG, "TLS write error: %d (after %u/%u bytes, %d calls)",
                     (int)w, (unsigned)total, (unsigned)len, write_calls);
            return -1;
        }
        if (w == 0) {
            if (write_calls > 100) {
                ESP_LOGE(TAG, "TLS write stuck: 0 return after %d calls", write_calls);
                return -1;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        total += w;
    }
    return (int)total;
}

/* Read a complete HTTP response from the TLS socket.
 * Returns HTTP status code (>=100) or -1 on error.
 * If body_out is non-NULL, the response body is stored in a malloc'd buffer
 * (caller must free).  If body_out is NULL, the body is drained and discarded.
 *
 * Uses buffered reads (not 1-byte-at-a-time) for compatibility with mbedTLS. */
static int shq_http_read_response(esp_tls_t *tls, char **body_out, size_t *body_len)
{
    /* Buffer for entire response (headers + body) */
    size_t buf_cap = SHQ_RESP_CAP + 1024;
    char *buf = malloc(buf_cap);
    if (!buf) return -1;

    size_t buf_len = 0;
    char *header_end = NULL;

    /* Phase 1: read until we find \r\n\r\n (end of headers) */
    while (!header_end) {
        if (buf_len >= buf_cap - 1) {
            ESP_LOGE(TAG, "response headers too large (%u)", (unsigned)buf_len);
            free(buf);
            return -1;
        }
        ssize_t n = esp_tls_conn_read(tls, buf + buf_len, buf_cap - buf_len - 1);
        if (n < 0) {
            ESP_LOGE(TAG, "TLS read error during headers: %d", (int)n);
            free(buf);
            return -1;
        }
        if (n == 0) {
            /* Connection closed before headers complete */
            ESP_LOGE(TAG, "connection closed during headers (got %u bytes)", (unsigned)buf_len);
            free(buf);
            return -1;
        }
        buf_len += n;
        buf[buf_len] = '\0';

        header_end = strstr(buf, "\r\n\r\n");
        if (header_end) header_end += 4;
    }

    /* Parse status line */
    int status = -1;
    if (strncmp(buf, "HTTP/", 5) == 0) {
        char *sp = strchr(buf, ' ');
        if (sp) status = atoi(sp + 1);
    }
    if (status < 0) {
        ESP_LOGE(TAG, "no HTTP status in response");
        free(buf);
        return -1;
    }

    /* Parse headers by temporarily null-terminating each line */
    size_t content_length = 0;
    bool chunked = false;

    char *line = buf;
    char *body_start = header_end;
    while (line < body_start - 4) {
        char *eol = strstr(line, "\r\n");
        if (!eol || eol >= body_start - 4) break;
        *eol = '\0';

        if (strncasecmp(line, "Content-Length:", 15) == 0) {
            char *v = line + 15;
            while (*v == ' ') v++;
            content_length = (size_t)atoi(v);
        }
        if (strncasecmp(line, "Transfer-Encoding:", 18) == 0) {
            if (strstr(line, "chunked")) chunked = true;
        }

        *eol = '\r';
        line = eol + 2;
    }

    /* Calculate body data already in buffer */
    size_t body_in_buf = buf_len - (body_start - buf);

    /* Phase 2: read remaining body */
    if (content_length > 0 && body_in_buf < content_length) {
        size_t remaining = content_length - body_in_buf;
        while (remaining > 0) {
            if (buf_len >= buf_cap - 1) break;
            size_t to_read = remaining < (buf_cap - buf_len - 1) ?
                             remaining : (buf_cap - buf_len - 1);
            ssize_t n = esp_tls_conn_read(tls, buf + buf_len, to_read);
            if (n < 0) {
                ESP_LOGE(TAG, "TLS read error during body: %d", (int)n);
                free(buf);
                return -1;
            }
            if (n == 0) break;
            buf_len += n;
            remaining -= n;
        }
    } else if (chunked) {
        /* Read until we see 0\r\n\r\n or connection closes */
        while (1) {
            if (buf_len >= buf_cap - 1) break;
            ssize_t n = esp_tls_conn_read(tls, buf + buf_len, buf_cap - buf_len - 1);
            if (n < 0) { free(buf); return -1; }
            if (n == 0) break;
            buf_len += n;
            buf[buf_len] = '\0';
            if (strstr(body_start, "\r\n0\r\n\r\n")) break;
        }
    } else if (content_length == 0 && !chunked) {
        /* No Content-Length, no chunked — read until connection closes */
        while (1) {
            if (buf_len >= buf_cap - 1) break;
            ssize_t n = esp_tls_conn_read(tls, buf + buf_len, buf_cap - buf_len - 1);
            if (n <= 0) break;
            buf_len += n;
        }
    }

    /* Extract body if requested */
    if (body_out) {
        size_t body_total = buf_len - (body_start - buf);
        if (body_total > SHQ_RESP_CAP) body_total = SHQ_RESP_CAP;
        char *body = malloc(SHQ_RESP_CAP);
        if (!body) { free(buf); return -1; }
        memcpy(body, body_start, body_total);
        if (body_total < SHQ_RESP_CAP) body[body_total] = '\0';
        *body_out = body;
        if (body_len) *body_len = body_total;
    }

    ESP_LOGI(TAG, "response: HTTP %d (%u bytes body)", status,
             (unsigned)(buf_len - (body_start - buf)));

    free(buf);
    return status;
}

/* ── HTTP request helpers ───────────────────────────────────────────── */

/* Send a simple GET or POST request with optional body and read the response.
 * The TLS connection stays open — caller manages it.
 * If body_out is non-NULL, response body is returned (caller frees). */
static int shq_http_request(esp_tls_t *tls, const char *method,
                            const char *path, const char *query,
                            const char *auth_token,
                            const char *body, const char *content_type,
                            char **body_out, size_t *body_len)
{
    /* Build request line + headers */
    char req[2048];
    int pos = 0;

    if (query) {
        pos += snprintf(req + pos, sizeof(req) - pos, "%s %s?%s HTTP/1.1\r\n", method, path, query);
    } else {
        pos += snprintf(req + pos, sizeof(req) - pos, "%s %s HTTP/1.1\r\n", method, path);
    }
    pos += snprintf(req + pos, sizeof(req) - pos, "Host: %s\r\n", SHQ_HOST);
    pos += snprintf(req + pos, sizeof(req) - pos, "Accept: application/vnd.api+json\r\n");
    pos += snprintf(req + pos, sizeof(req) - pos, "Connection: keep-alive\r\n");

    if (auth_token) {
        pos += snprintf(req + pos, sizeof(req) - pos, "Authorization: Bearer %s\r\n", auth_token);
    }

    if (body && content_type) {
        pos += snprintf(req + pos, sizeof(req) - pos, "Content-Type: %s\r\n", content_type);
        pos += snprintf(req + pos, sizeof(req) - pos, "Content-Length: %d\r\n", (int)strlen(body));
    }

    pos += snprintf(req + pos, sizeof(req) - pos, "\r\n");

    if (body) {
        pos += snprintf(req + pos, sizeof(req) - pos, "%s", body);
    }

    if (pos >= (int)sizeof(req) - 1) {
        ESP_LOGE(TAG, "request header too long (%d bytes)", pos);
        return -1;
    }

    ESP_LOGI(TAG, "sending %s %s (%d bytes)", method, path, pos);

    if (shq_tls_write_all(tls, req, pos) < 0) {
        ESP_LOGE(TAG, "failed to send request");
        return -1;
    }

    int status = shq_http_read_response(tls, body_out, body_len);
    if (status < 0) {
        ESP_LOGE(TAG, "failed to read response");
        return -1;
    }

    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "HTTP %d: %s", status, body_out && *body_out ? *body_out : "(no body)");
        return status;
    }

    return status;
}

/* ── Authentication ─────────────────────────────────────────────────── */

static esp_err_t shq_authenticate(esp_tls_t *tls, const uploader_config_t *cfg)
{
    if (s_token[0] && s_token_expires > 0) {
        int64_t now_s = time(NULL);
        int elapsed = (int)(now_s - s_token_time_s);
        if (elapsed < s_token_expires - 60) {
            return ESP_OK;
        }
    }

    ESP_LOGI(TAG, "authenticating with SleepHQ...");

    char body[512];
    snprintf(body, sizeof(body),
             "grant_type=password&client_id=%s&client_secret=%s&scope=read+write",
             cfg->shq_client_id, cfg->shq_client_secret);

    char *resp_body = NULL;
    size_t resp_len = 0;
    int status = shq_http_request(tls, "POST", SHQ_TOKEN_PATH, NULL, NULL,
                                  body, "application/x-www-form-urlencoded",
                                  &resp_body, &resp_len);
    if (status < 200) {
        ESP_LOGE(TAG, "auth request failed (status=%d)", status);
        free(resp_body);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(resp_body);
    free(resp_body);

    if (!root) {
        ESP_LOGE(TAG, "auth: failed to parse JSON");
        return ESP_FAIL;
    }

    cJSON *token = cJSON_GetObjectItem(root, "access_token");
    cJSON *expires = cJSON_GetObjectItem(root, "expires_in");

    if (!token || !cJSON_IsString(token)) {
        ESP_LOGE(TAG, "auth: no access_token in response");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    strlcpy(s_token, token->valuestring, sizeof(s_token));
    s_token_expires = (expires && cJSON_IsNumber(expires)) ? expires->valueint : 7200;
    s_token_time_s = time(NULL);

    cJSON_Delete(root);
    ESP_LOGI(TAG, "authenticated, token expires in %d s", s_token_expires);
    return ESP_OK;
}

/* ── Team discovery ─────────────────────────────────────────────────── */

static esp_err_t shq_discover_team(esp_tls_t *tls)
{
    if (s_team_id[0]) return ESP_OK;

    ESP_LOGI(TAG, "discovering team ID...");

    char *resp_body = NULL;
    size_t resp_len = 0;
    int status = shq_http_request(tls, "GET", SHQ_ME_PATH, NULL, s_token,
                                  NULL, NULL, &resp_body, &resp_len);
    if (status < 200) {
        free(resp_body);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(resp_body);
    free(resp_body);

    if (!root) return ESP_FAIL;

    cJSON *data = cJSON_GetObjectItem(root, "data");
    if (data) {
        cJSON *attrs = cJSON_GetObjectItem(data, "attributes");
        cJSON *team = NULL;
        if (attrs) team = cJSON_GetObjectItem(attrs, "current_team_id");
        if (!team) team = cJSON_GetObjectItem(data, "current_team_id");
        if (team) {
            if (cJSON_IsNumber(team))
                snprintf(s_team_id, sizeof(s_team_id), "%d", team->valueint);
            else if (cJSON_IsString(team))
                strlcpy(s_team_id, team->valuestring, sizeof(s_team_id));
        }
    }

    cJSON_Delete(root);

    if (!s_team_id[0]) {
        ESP_LOGE(TAG, "team discovery: no current_team_id found");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "team ID: %s", s_team_id);
    return ESP_OK;
}

/* ── Create import session ──────────────────────────────────────────── */

static esp_err_t shq_create_import(esp_tls_t *tls, char *out_import_id, size_t id_len)
{
    ESP_LOGI(TAG, "creating import session...");

    char path[256];
    snprintf(path, sizeof(path), SHQ_IMPORTS_FMT, s_team_id);

    char *resp_body = NULL;
    size_t resp_len = 0;
    int status = shq_http_request(tls, "POST", path, NULL, s_token,
                                  NULL, NULL, &resp_body, &resp_len);
    if (status < 200) {
        free(resp_body);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(resp_body);
    free(resp_body);

    if (!root) return ESP_FAIL;

    cJSON *data = cJSON_GetObjectItem(root, "data");
    if (data) {
        cJSON *attrs = cJSON_GetObjectItem(data, "attributes");
        cJSON *id = NULL;
        if (attrs) id = cJSON_GetObjectItem(attrs, "id");
        if (!id) id = cJSON_GetObjectItem(data, "id");
        if (id) {
            if (cJSON_IsNumber(id))
                snprintf(out_import_id, id_len, "%d", id->valueint);
            else if (cJSON_IsString(id))
                strlcpy(out_import_id, id->valuestring, id_len);
        }
    }

    cJSON_Delete(root);

    if (!out_import_id[0]) {
        ESP_LOGE(TAG, "create import: no import ID in response");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "import ID: %s", out_import_id);
    return ESP_OK;
}

/* ── Process import (finalize) ──────────────────────────────────────── */

static esp_err_t shq_process_import(esp_tls_t *tls, const char *import_id)
{
    ESP_LOGI(TAG, "processing import %s...", import_id);

    char path[256];
    snprintf(path, sizeof(path), SHQ_PROCESS_FMT, import_id);

    int status = shq_http_request(tls, "POST", path, NULL, s_token,
                                  NULL, NULL, NULL, NULL);
    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "process import HTTP %d", status);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "import %s processed", import_id);
    return ESP_OK;
}

/* ── Multipart file upload (streaming, on-the-fly MD5) ────────────────
 *
 * Streams the file directly from SD card to the TLS socket in a single
 * pass.  MD5 is computed on-the-fly as each chunk is read and sent.
 * The content_hash is sent in the multipart footer after the file data.
 * No PSRAM buffering needed — only one chunk buffer is allocated. */

static upload_result_t shq_upload_file(esp_tls_t *tls,
                                       const char *import_id,
                                       const char *local_path,
                                       const char *remote_subpath,
                                       const char *filename)
{
    FILE *f = fopen(local_path, "rb");
    if (!f) {
        ESP_LOGW(TAG, "  cannot open %s", local_path);
        return UPLOAD_FAILED;
    }

    fseek(f, 0, SEEK_END);
    size_t file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    /* Build multipart boundary */
    char boundary[48];
    snprintf(boundary, sizeof(boundary), "----ESP32%08X", (unsigned)esp_random());

    /* Calculate sizes of multipart parts (no heap alloc for dummy calc) */
    char part1[512];
    size_t part1_len = snprintf(part1, sizeof(part1),
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"name\"\r\n\r\n"
        "%s\r\n",
        boundary, filename);

    char part2[512];
    size_t part2_len = snprintf(part2, sizeof(part2),
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"path\"\r\n\r\n"
        "%s\r\n",
        boundary, remote_subpath);

    char part3[512];
    size_t part3_len = snprintf(part3, sizeof(part3),
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"%s\"\r\n"
        "Content-Type: application/octet-stream\r\n\r\n",
        boundary, filename);

    /* Footer: content_hash (32 hex chars) + closing boundary */
    char footer_hdr[256];
    size_t footer_hdr_len = snprintf(footer_hdr, sizeof(footer_hdr),
        "\r\n--%s\r\n"
        "Content-Disposition: form-data; name=\"content_hash\"\r\n\r\n",
        boundary);

    char closing[64];
    size_t closing_len = snprintf(closing, sizeof(closing),
        "\r\n--%s--\r\n",
        boundary);

    /* Total multipart body length = parts + file + footer_header + 32 (md5 hex) + closing */
    size_t total_body_len = part1_len + part2_len + part3_len + file_size
                           + footer_hdr_len + 32 + closing_len;

    /* Build HTTP request headers */
    char path[256];
    snprintf(path, sizeof(path), SHQ_FILES_FMT, import_id);

    char req_hdr[1024];
    int hdr_pos = snprintf(req_hdr, sizeof(req_hdr),
        "POST %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Authorization: Bearer %s\r\n"
        "Accept: application/vnd.api+json\r\n"
        "Content-Type: multipart/form-data; boundary=%s\r\n"
        "Content-Length: %u\r\n"
        "Connection: keep-alive\r\n"
        "\r\n",
        path, SHQ_HOST, s_token, boundary, (unsigned)total_body_len);

    if (hdr_pos <= 0 || hdr_pos >= (int)sizeof(req_hdr)) {
        ESP_LOGE(TAG, "  request header too long");
        fclose(f);
        return UPLOAD_FAILED;
    }

    /* Send HTTP headers */
    if (shq_tls_write_all(tls, req_hdr, hdr_pos) < 0) {
        ESP_LOGE(TAG, "  failed to send HTTP headers for %s", filename);
        fclose(f);
        return UPLOAD_FAILED;
    }

    /* Send multipart preamble parts */
    if (shq_tls_write_all(tls, part1, part1_len) < 0 ||
        shq_tls_write_all(tls, part2, part2_len) < 0 ||
        shq_tls_write_all(tls, part3, part3_len) < 0) {
        ESP_LOGE(TAG, "  failed to send multipart preamble for %s", filename);
        fclose(f);
        return UPLOAD_FAILED;
    }

    /* Stream file data with on-the-fly MD5 */
    uint8_t *chunk = malloc(SHQ_READ_BUF);
    if (!chunk) {
        ESP_LOGE(TAG, "  cannot alloc chunk buffer for %s", filename);
        fclose(f);
        return UPLOAD_FAILED;
    }

    mbedtls_md5_context md5;
    mbedtls_md5_init(&md5);
    mbedtls_md5_starts(&md5);

    size_t total_sent = 0;
    while (total_sent < file_size) {
        size_t to_read = SHQ_READ_BUF;
        if (to_read > file_size - total_sent)
            to_read = file_size - total_sent;

        size_t nread = fread(chunk, 1, to_read, f);
        if (nread == 0) {
            ESP_LOGE(TAG, "  short read for %s at offset %u", filename, (unsigned)total_sent);
            free(chunk);
            fclose(f);
            mbedtls_md5_free(&md5);
            return UPLOAD_FAILED;
        }

        /* Feed to MD5 */
        mbedtls_md5_update(&md5, chunk, nread);

        /* Write to TLS socket */
        if (shq_tls_write_all(tls, chunk, nread) < 0) {
            ESP_LOGE(TAG, "  TLS write failed for %s at offset %u", filename, (unsigned)total_sent);
            free(chunk);
            fclose(f);
            mbedtls_md5_free(&md5);
            return UPLOAD_FAILED;
        }

        total_sent += nread;

        /* Yield to scheduler */
        if (total_sent % (SHQ_READ_BUF * 4) == 0)
            taskYIELD();
    }

    free(chunk);
    fclose(f);

    /* Finalize MD5: append filename, compute digest */
    mbedtls_md5_update(&md5, (const unsigned char *)filename, strlen(filename));
    unsigned char md5_raw[16];
    mbedtls_md5_finish(&md5, md5_raw);
    mbedtls_md5_free(&md5);

    char md5_hex[33];
    for (int i = 0; i < 16; i++)
        snprintf(md5_hex + i * 2, 3, "%02x", md5_raw[i]);
    md5_hex[32] = '\0';

    /* Send footer: header + md5 hex + closing boundary */
    if (shq_tls_write_all(tls, footer_hdr, footer_hdr_len) < 0 ||
        shq_tls_write_all(tls, md5_hex, 32) < 0 ||
        shq_tls_write_all(tls, closing, closing_len) < 0) {
        ESP_LOGE(TAG, "  failed to send multipart footer for %s", filename);
        return UPLOAD_FAILED;
    }

    /* Read response (drain body, we only need status) */
    int status = shq_http_read_response(tls, NULL, NULL);
    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "  file upload HTTP %d for %s", status, filename);
        return UPLOAD_FAILED;
    }

    ESP_LOGI(TAG, "  uploaded %s (%u bytes, hash=%s)", filename,
             (unsigned)file_size, md5_hex);
    return UPLOAD_OK;
}

/* ── Upload all files for a session ─────────────────────────────────── */

static upload_result_t shq_upload_day(const char *day_folder)
{
    uploader_config_t cfg;
    uploader_load_config(&cfg);

    if (!cfg.shq_client_id[0] || !cfg.shq_client_secret[0]) {
        return UPLOAD_NOT_CONFIGURED;
    }

    ESP_LOGI(TAG, "SleepHQ upload: day %s", day_folder);

    /* Open a single TLS connection for the entire session.
     * All API calls and file uploads reuse this one connection —
     * only one TLS handshake for the whole session. */
    esp_tls_cfg_t tls_cfg = {
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = SHQ_TIMEOUT_MS,
    };

    esp_tls_t *tls = esp_tls_init();
    if (!tls) {
        ESP_LOGE(TAG, "esp_tls_init failed");
        return UPLOAD_FAILED;
    }

    char url[128];
    snprintf(url, sizeof(url), "https://%s", SHQ_HOST);

    int ret = esp_tls_conn_http_new_sync(url, &tls_cfg, tls);
    if (ret != 1) {
        ESP_LOGE(TAG, "TLS connect to %s failed (ret=%d)", SHQ_HOST, ret);
        esp_tls_conn_destroy(tls);
        return UPLOAD_FAILED;
    }

    ESP_LOGI(TAG, "TLS connected to %s", SHQ_HOST);

    /* 1. Authenticate */
    esp_err_t err = shq_authenticate(tls, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "authentication failed");
        esp_tls_conn_destroy(tls);
        return UPLOAD_FAILED;
    }

    /* 2. Discover team */
    err = shq_discover_team(tls);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "team discovery failed");
        esp_tls_conn_destroy(tls);
        return UPLOAD_FAILED;
    }

    /* 3. Create import */
    char import_id[32] = {0};
    err = shq_create_import(tls, import_id, sizeof(import_id));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "import creation failed");
        esp_tls_conn_destroy(tls);
        return UPLOAD_FAILED;
    }

    int files_uploaded = 0;
    int failures = 0;

    /* 4. Upload all EDF files from DATALOG/day_folder/ */
    char local_day_dir[256];
    snprintf(local_day_dir, sizeof(local_day_dir), "%s/%s", SD_SDCARD_DATALOG, day_folder);

    char remote_subpath[64];
    snprintf(remote_subpath, sizeof(remote_subpath), "/DATALOG/%s", day_folder);

    DIR *d = opendir(local_day_dir);
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (strstr(ent->d_name, ".edf") == NULL) continue;

            char local_path[512];
            snprintf(local_path, sizeof(local_path), "%s/%s", local_day_dir, ent->d_name);

            if (shq_upload_file(tls, import_id, local_path, remote_subpath,
                                ent->d_name) == UPLOAD_OK) {
                files_uploaded++;
            } else {
                failures++;
            }
        }
        closedir(d);
    } else {
        ESP_LOGW(TAG, "  cannot open %s", local_day_dir);
    }

    if (files_uploaded == 0) {
        ESP_LOGW(TAG, "no EDF files uploaded for day %s", day_folder);
        esp_tls_conn_destroy(tls);
        return UPLOAD_FAILED;
    }

    /* 5. Upload mandatory root files */
    const char *root_files[] = {
        "/STR.edf",
        "/Identification.json",
        "/Identification.crc",
        NULL
    };

    for (int i = 0; root_files[i]; i++) {
        char local_path[300];
        snprintf(local_path, sizeof(local_path), "%s%s", SD_SDCARD_DIR, root_files[i]);

        struct stat st;
        if (stat(local_path, &st) != 0) continue;

        const char *fname = root_files[i] + 1;

        if (shq_upload_file(tls, import_id, local_path, "", fname) == UPLOAD_OK) {
            files_uploaded++;
        } else {
            failures++;
        }
    }

    /* Upload settings files */
    d = opendir(SD_SDCARD_SETTINGS);
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (ent->d_name[0] == '.') continue;

            char local_path[400];
            snprintf(local_path, sizeof(local_path), "%s/%s", SD_SDCARD_SETTINGS, ent->d_name);

            if (shq_upload_file(tls, import_id, local_path, "/SETTINGS",
                                ent->d_name) == UPLOAD_OK) {
                files_uploaded++;
            } else {
                failures++;
            }
        }
        closedir(d);
    }

    ESP_LOGI(TAG, "uploaded %d files (%d failures)", files_uploaded, failures);

    /* 6. Process import (finalize) */
    err = shq_process_import(tls, import_id);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "import processing failed (files may still be queued)");
    }

    esp_tls_conn_destroy(tls);

    if (failures > 0) {
        ESP_LOGW(TAG, "SleepHQ upload completed with %d failures", failures);
        return UPLOAD_FAILED;
    }

    ESP_LOGI(TAG, "SleepHQ upload complete for day %s", day_folder);
    return UPLOAD_OK;
}

/* ── Backend interface ──────────────────────────────────────────────── */

static bool shq_is_configured(void)
{
    return uploader_is_sleephq_configured();
}

const upload_backend_t sleephq_backend = {
    .name = "sleephq",
    .is_configured = shq_is_configured,
    .upload_day = shq_upload_day,
};
