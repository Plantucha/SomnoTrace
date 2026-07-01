/*
 * SomnoTrace - SleepHQ upload backend using esp_http_client
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

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_random.h"
#include "cJSON.h"
#include "mbedtls/md5.h"

/* SD card paths — must match sd_storage.h */
#define SD_MOUNT_POINT      "/somnotrace"
#define SD_SDCARD_DIR       SD_MOUNT_POINT "/SDCARD"
#define SD_SDCARD_DATALOG   SD_SDCARD_DIR "/DATALOG"
#define SD_SDCARD_SETTINGS  SD_SDCARD_DIR "/SETTINGS"

static const char *TAG = "upload_shq";

#define SHQ_HOST        "sleephq.com"
#define SHQ_URL_BASE    "https://sleephq.com"
#define SHQ_TOKEN_URL   SHQ_URL_BASE "/oauth/token"
#define SHQ_ME_URL      SHQ_URL_BASE "/api/v1/me"
#define SHQ_IMPORTS_URL SHQ_URL_BASE "/api/v1/teams/%s/imports"
#define SHQ_FILES_URL   SHQ_URL_BASE "/api/v1/imports/%s/files"
#define SHQ_PROCESS_URL SHQ_URL_BASE "/api/v1/imports/%s/process_files"

#define SHQ_BUF_SIZE    (8 * 1024)
#define SHQ_TIMEOUT_MS  30000

/* Token cache */
static char s_token[512] = {0};
static int64_t s_token_time_s = 0;
static int s_token_expires = 0;
static char s_team_id[32] = {0};

/* ── HTTP helpers ───────────────────────────────────────────────────── */

/* Simple HTTP response buffer */
typedef struct {
    char *data;
    size_t size;
    size_t capacity;
} http_resp_t;

static esp_err_t http_resp_init(http_resp_t *r, size_t initial_cap)
{
    r->data = malloc(initial_cap);
    if (!r->data) return ESP_ERR_NO_MEM;
    r->size = 0;
    r->capacity = initial_cap;
    return ESP_OK;
}

static void http_resp_free(http_resp_t *r)
{
    if (r->data) { free(r->data); r->data = NULL; }
    r->size = 0;
    r->capacity = 0;
}

static esp_err_t http_resp_append(http_resp_t *r, const char *data, size_t len)
{
    if (r->size + len + 1 > r->capacity) {
        size_t newcap = r->capacity * 2;
        while (newcap < r->size + len + 1) newcap *= 2;
        char *nd = realloc(r->data, newcap);
        if (!nd) return ESP_ERR_NO_MEM;
        r->data = nd;
        r->capacity = newcap;
    }
    memcpy(r->data + r->size, data, len);
    r->size += len;
    r->data[r->size] = '\0';
    return ESP_OK;
}

/* Current response buffer — set before each request so the event handler
 * can append data.  The uploader is single-threaded so this is safe. */
static http_resp_t *s_current_resp = NULL;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA && s_current_resp) {
        return http_resp_append(s_current_resp, (char *)evt->data, evt->data_len);
    }
    return ESP_OK;
}

/* Perform a simple GET or POST with JSON response on a shared client. */
static esp_err_t shq_http_request(esp_http_client_handle_t client,
                                   const char *url, const char *method,
                                   const char *body, const char *content_type,
                                   http_resp_t *resp)
{
    esp_http_client_set_url(client, url);
    esp_http_client_set_method(client,
        (strcmp(method, "POST") == 0) ? HTTP_METHOD_POST : HTTP_METHOD_GET);

    if (body && content_type) {
        esp_http_client_set_header(client, "Content-Type", content_type);
        esp_http_client_set_post_field(client, body, strlen(body));
    } else {
        esp_http_client_set_post_field(client, NULL, 0);
    }

    resp->size = 0;
    if (resp->data) resp->data[0] = '\0';
    s_current_resp = resp;

    esp_err_t err = esp_http_client_perform(client);
    s_current_resp = NULL;

    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        if (status < 200 || status >= 300) {
            ESP_LOGW(TAG, "HTTP %d: %s", status, resp->data ? resp->data : "(no body)");
            err = ESP_FAIL;
        }
    } else {
        ESP_LOGE(TAG, "HTTP perform failed: %s", esp_err_to_name(err));
    }

    return err;
}

/* ── Authentication ─────────────────────────────────────────────────── */

static esp_err_t shq_authenticate(esp_http_client_handle_t client,
                                  const uploader_config_t *cfg)
{
    /* Check if token is still valid */
    if (s_token[0] && s_token_expires > 0) {
        int64_t now_s = time(NULL);
        int elapsed = (int)(now_s - s_token_time_s);
        if (elapsed < s_token_expires - 60) {
            return ESP_OK;  /* token still valid */
        }
    }

    ESP_LOGI(TAG, "authenticating with SleepHQ...");

    /* Build token request body */
    char body[512];
    snprintf(body, sizeof(body),
             "grant_type=password&client_id=%s&client_secret=%s&scope=read+write",
             cfg->shq_client_id, cfg->shq_client_secret);

    http_resp_t resp;
    http_resp_init(&resp, 1024);

    esp_err_t err = shq_http_request(client, SHQ_TOKEN_URL, "POST", body,
                                      "application/x-www-form-urlencoded", &resp);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "auth request failed");
        http_resp_free(&resp);
        return err;
    }

    /* Parse token response */
    cJSON *root = cJSON_Parse(resp.data);
    http_resp_free(&resp);

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

static esp_err_t shq_discover_team(esp_http_client_handle_t client)
{
    if (s_team_id[0]) return ESP_OK;  /* cached */

    ESP_LOGI(TAG, "discovering team ID...");

    http_resp_t resp;
    http_resp_init(&resp, 2048);

    esp_err_t err = shq_http_request(client, SHQ_ME_URL, "GET", NULL, NULL, &resp);
    if (err != ESP_OK) {
        http_resp_free(&resp);
        return err;
    }

    cJSON *root = cJSON_Parse(resp.data);
    http_resp_free(&resp);

    if (!root) return ESP_FAIL;

    cJSON *data = cJSON_GetObjectItem(root, "data");
    if (data) {
        cJSON *attrs = cJSON_GetObjectItem(data, "attributes");
        cJSON *team = NULL;
        if (attrs) {
            team = cJSON_GetObjectItem(attrs, "current_team_id");
        }
        if (!team) {
            team = cJSON_GetObjectItem(data, "current_team_id");
        }
        if (team) {
            if (cJSON_IsNumber(team)) {
                snprintf(s_team_id, sizeof(s_team_id), "%d", team->valueint);
            } else if (cJSON_IsString(team)) {
                strlcpy(s_team_id, team->valuestring, sizeof(s_team_id));
            }
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

static esp_err_t shq_create_import(esp_http_client_handle_t client,
                                   char *out_import_id, size_t id_len)
{
    ESP_LOGI(TAG, "creating import session...");

    char url[256];
    snprintf(url, sizeof(url), SHQ_IMPORTS_URL, s_team_id);

    http_resp_t resp;
    http_resp_init(&resp, 1024);

    esp_err_t err = shq_http_request(client, url, "POST", NULL, NULL, &resp);
    if (err != ESP_OK) {
        http_resp_free(&resp);
        return err;
    }

    cJSON *root = cJSON_Parse(resp.data);
    http_resp_free(&resp);

    if (!root) return ESP_FAIL;

    cJSON *data = cJSON_GetObjectItem(root, "data");
    if (data) {
        cJSON *attrs = cJSON_GetObjectItem(data, "attributes");
        cJSON *id = NULL;
        if (attrs) {
            id = cJSON_GetObjectItem(attrs, "id");
        }
        if (!id) {
            id = cJSON_GetObjectItem(data, "id");
        }
        if (id) {
            if (cJSON_IsNumber(id)) {
                snprintf(out_import_id, id_len, "%d", id->valueint);
            } else if (cJSON_IsString(id)) {
                strlcpy(out_import_id, id->valuestring, id_len);
            }
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

static esp_err_t shq_process_import(esp_http_client_handle_t client,
                                    const char *import_id)
{
    ESP_LOGI(TAG, "processing import %s...", import_id);

    char url[256];
    snprintf(url, sizeof(url), SHQ_PROCESS_URL, import_id);

    http_resp_t resp;
    http_resp_init(&resp, 1024);

    esp_err_t err = shq_http_request(client, url, "POST", NULL, NULL, &resp);
    http_resp_free(&resp);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "import %s processed", import_id);
    }
    return err;
}

/* ── Multipart file upload (single-pass, shared client) ───────────────
 *
 * Streams the file in a single pass: MD5 is computed on-the-fly as each
 * chunk is read from SD and written to the HTTP client.  The content_hash
 * is sent as the final multipart part, so we must know the total content
 * length up front.  We handle this by using chunked transfer encoding
 * (content-length = -1) so we don't need to pre-compute the hash.
 *
 * However, esp_http_client_open requires a content length for non-chunked
 * uploads.  For chunked, we pass 0 and the client uses Transfer-Encoding:
 * chunked.  But SleepHQ may not support chunked.  So we use a small
 * two-phase approach within a single read pass: buffer the file in PSRAM
 * if small enough, or fall back to two reads for large files.
 *
 * In practice, all SomnoTrace files are small (< 64 KB), so we read the
 * entire file into a PSRAM buffer once, compute MD5, and stream from that
 * buffer — one SD read, no seek-back. */

static upload_result_t shq_upload_file(esp_http_client_handle_t client,
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

    /* Read entire file into PSRAM buffer (single pass) */
    uint8_t *file_buf = heap_caps_malloc(file_size, MALLOC_CAP_SPIRAM);
    if (!file_buf) file_buf = malloc(file_size);
    if (!file_buf) {
        ESP_LOGE(TAG, "  cannot alloc %u bytes for %s", (unsigned)file_size, filename);
        fclose(f);
        return UPLOAD_FAILED;
    }

    size_t total_read = fread(file_buf, 1, file_size, f);
    fclose(f);
    if (total_read != file_size) {
        ESP_LOGE(TAG, "  short read for %s", filename);
        free(file_buf);
        return UPLOAD_FAILED;
    }

    /* Compute MD5(file_content + filename) */
    mbedtls_md5_context md5;
    mbedtls_md5_init(&md5);
    mbedtls_md5_starts(&md5);
    mbedtls_md5_update(&md5, file_buf, file_size);
    mbedtls_md5_update(&md5, (const unsigned char *)filename, strlen(filename));

    unsigned char md5_raw[16];
    mbedtls_md5_finish(&md5, md5_raw);
    mbedtls_md5_free(&md5);

    char md5_hex[33];
    for (int i = 0; i < 16; i++)
        snprintf(md5_hex + i * 2, 3, "%02x", md5_raw[i]);
    md5_hex[32] = '\0';

    /* Build multipart parts */
    char boundary[48];
    snprintf(boundary, sizeof(boundary), "----ESP32%08X", (unsigned)esp_random());

    char *header = malloc(512 + strlen(filename) + strlen(remote_subpath));
    if (!header) { free(file_buf); return UPLOAD_FAILED; }

    size_t header_len = snprintf(header, 512 + strlen(filename) + strlen(remote_subpath),
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"name\"\r\n\r\n"
        "%s\r\n"
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"path\"\r\n\r\n"
        "%s\r\n"
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"%s\"\r\n"
        "Content-Type: application/octet-stream\r\n\r\n",
        boundary, filename, boundary, remote_subpath,
        boundary, filename);

    char footer[512];
    size_t footer_len = snprintf(footer, sizeof(footer),
        "\r\n--%s\r\n"
        "Content-Disposition: form-data; name=\"content_hash\"\r\n\r\n"
        "%s\r\n"
        "--%s--\r\n",
        boundary, md5_hex, boundary);

    size_t total_content_len = header_len + file_size + footer_len;

    /* Upload via shared client */
    char url[256];
    snprintf(url, sizeof(url), SHQ_FILES_URL, import_id);
    esp_http_client_set_url(client, url);
    esp_http_client_set_method(client, HTTP_METHOD_POST);

    char ct_header[128];
    snprintf(ct_header, sizeof(ct_header),
             "multipart/form-data; boundary=%s", boundary);
    esp_http_client_set_header(client, "Content-Type", ct_header);

    esp_err_t err = esp_http_client_open(client, total_content_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "  http_open failed: %s", esp_err_to_name(err));
        free(header); free(file_buf);
        return UPLOAD_FAILED;
    }

    int written = esp_http_client_write(client, header, header_len);
    if (written < 0 || (size_t)written != header_len) {
        ESP_LOGE(TAG, "  failed to write multipart header");
        esp_http_client_close(client);
        free(header); free(file_buf);
        return UPLOAD_FAILED;
    }

    /* Stream file data from memory buffer */
    size_t total_written = 0;
    while (total_written < file_size) {
        size_t to_write = SHQ_BUF_SIZE;
        if (to_write > file_size - total_written)
            to_write = file_size - total_written;
        int w = esp_http_client_write(client, (char *)(file_buf + total_written), to_write);
        if (w < 0) {
            ESP_LOGE(TAG, "  write failed at offset %u", (unsigned)total_written);
            esp_http_client_close(client);
            free(header); free(file_buf);
            return UPLOAD_FAILED;
        }
        total_written += w;
    }

    /* Write footer */
    written = esp_http_client_write(client, footer, footer_len);
    if (written < 0 || (size_t)written != footer_len) {
        ESP_LOGE(TAG, "  failed to write multipart footer");
        esp_http_client_close(client);
        free(header); free(file_buf);
        return UPLOAD_FAILED;
    }

    /* Read response */
    esp_http_client_fetch_headers(client);
    http_resp_t resp;
    http_resp_init(&resp, 1024);
    s_current_resp = &resp;
    int rd = esp_http_client_read(client, resp.data, resp.capacity - 1);
    s_current_resp = NULL;
    if (rd > 0) { resp.size = rd; resp.data[rd] = '\0'; }

    int status = esp_http_client_get_status_code(client);
    esp_http_client_close(client);

    free(header); free(file_buf);
    http_resp_free(&resp);

    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "  file upload HTTP %d for %s", status, filename);
        return UPLOAD_FAILED;
    }

    ESP_LOGI(TAG, "  uploaded %s (%u bytes, hash=%s)", filename,
             (unsigned)file_size, md5_hex);
    return UPLOAD_OK;
}

/* ── Upload all files for a session ─────────────────────────────────── */

static upload_result_t shq_upload_session(const char *session_id,
                                           const char *day_folder)
{
    uploader_config_t cfg;
    uploader_load_config(&cfg);

    if (!cfg.shq_client_id[0] || !cfg.shq_client_secret[0]) {
        return UPLOAD_NOT_CONFIGURED;
    }

    ESP_LOGI(TAG, "SleepHQ upload: session %s (day=%s)", session_id, day_folder);

    /* Create one shared HTTP client with keep-alive for the entire session.
     * This reuses the TLS connection across all requests (auth, team
     * discovery, import creation, file uploads, process) — saving ~9 TLS
     * handshakes (~6-7 seconds). */
    esp_http_client_config_t config = {
        .url = SHQ_URL_BASE,
        .timeout_ms = SHQ_TIMEOUT_MS,
        .event_handler = http_event_handler,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 1024,
        .buffer_size_tx = 4096,
        .keep_alive_enable = true,
        .keep_alive_idle = 10,
        .keep_alive_interval = 5,
        .keep_alive_count = 3,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "http_client_init failed");
        return UPLOAD_FAILED;
    }

    /* Set auth headers once — they persist across requests on the same client */
    if (s_token[0]) {
        char auth_hdr[600];
        snprintf(auth_hdr, sizeof(auth_hdr), "Bearer %s", s_token);
        esp_http_client_set_header(client, "Authorization", auth_hdr);
    }
    esp_http_client_set_header(client, "Accept", "application/vnd.api+json");

    /* 1. Authenticate */
    esp_err_t err = shq_authenticate(client, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "authentication failed");
        esp_http_client_cleanup(client);
        return UPLOAD_FAILED;
    }

    /* Update auth header after authentication */
    char auth_hdr[600];
    snprintf(auth_hdr, sizeof(auth_hdr), "Bearer %s", s_token);
    esp_http_client_set_header(client, "Authorization", auth_hdr);

    /* 2. Discover team */
    err = shq_discover_team(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "team discovery failed");
        esp_http_client_cleanup(client);
        return UPLOAD_FAILED;
    }

    /* 3. Create import */
    char import_id[32] = {0};
    err = shq_create_import(client, import_id, sizeof(import_id));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "import creation failed");
        esp_http_client_cleanup(client);
        return UPLOAD_FAILED;
    }

    int files_uploaded = 0;
    int failures = 0;

    /* 4. Upload session EDF files from DATALOG/day_folder/ */
    char local_day_dir[256];
    snprintf(local_day_dir, sizeof(local_day_dir), "%s/%s", SD_SDCARD_DATALOG, day_folder);

    char remote_subpath[64];
    snprintf(remote_subpath, sizeof(remote_subpath), "/DATALOG/%s", day_folder);

    DIR *d = opendir(local_day_dir);
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (strstr(ent->d_name, ".edf") == NULL) continue;
            if (strstr(ent->d_name, session_id) == NULL) continue;

            char local_path[512];
            snprintf(local_path, sizeof(local_path), "%s/%s", local_day_dir, ent->d_name);

            if (shq_upload_file(client, import_id, local_path, remote_subpath,
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
        ESP_LOGW(TAG, "no EDF files uploaded for session %s", session_id);
        esp_http_client_cleanup(client);
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

        const char *fname = root_files[i] + 1;  /* skip leading / */

        if (shq_upload_file(client, import_id, local_path, "", fname) == UPLOAD_OK) {
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

            char remote_sub[64];
            snprintf(remote_sub, sizeof(remote_sub), "/SETTINGS");

            if (shq_upload_file(client, import_id, local_path, remote_sub,
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
    err = shq_process_import(client, import_id);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "import processing failed (files may still be queued)");
    }

    esp_http_client_cleanup(client);

    if (failures > 0) {
        ESP_LOGW(TAG, "SleepHQ upload completed with %d failures", failures);
        return UPLOAD_FAILED;
    }

    ESP_LOGI(TAG, "SleepHQ upload complete for session %s", session_id);
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
    .upload_session = shq_upload_session,
};
