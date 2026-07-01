/*
 * SomnoTrace - Upload state tracking on LittleFS
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

#include "uploader_state.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>

#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "upload_state";

/* ── Load ───────────────────────────────────────────────────────────── */

esp_err_t uploader_state_load(upload_state_t *state)
{
    if (!state) return ESP_ERR_INVALID_ARG;
    memset(state, 0, sizeof(*state));

    FILE *f = fopen(UPLOAD_STATE_PATH, "r");
    if (!f) {
        ESP_LOGI(TAG, "no state file found — starting fresh");
        return ESP_OK;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fsize <= 0 || fsize > 64 * 1024) {
        ESP_LOGE(TAG, "state file size invalid (%ld)", fsize);
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }

    char *buf = malloc(fsize + 1);
    if (!buf) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    size_t rd = fread(buf, 1, fsize, f);
    fclose(f);
    buf[rd] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);

    if (!root) {
        ESP_LOGE(TAG, "failed to parse state JSON");
        return ESP_ERR_INVALID_STATE;
    }

    cJSON *sessions = cJSON_GetObjectItem(root, "sessions");
    if (sessions && cJSON_IsArray(sessions)) {
        int n = cJSON_GetArraySize(sessions);
        if (n > UPLOAD_MAX_SESSIONS) n = UPLOAD_MAX_SESSIONS;

        for (int i = 0; i < n; i++) {
            cJSON *sess = cJSON_GetArrayItem(sessions, i);
            if (!sess) continue;

            session_state_t *s = &state->sessions[i];
            cJSON *id = cJSON_GetObjectItem(sess, "id");
            cJSON *day = cJSON_GetObjectItem(sess, "day");
            if (id && cJSON_IsString(id))
                strlcpy(s->session_id, id->valuestring, sizeof(s->session_id));
            if (day && cJSON_IsString(day))
                strlcpy(s->day_folder, day->valuestring, sizeof(s->day_folder));

            cJSON *backends = cJSON_GetObjectItem(sess, "backends");
            if (backends && cJSON_IsArray(backends)) {
                int nb = cJSON_GetArraySize(backends);
                if (nb > UPLOAD_MAX_BACKENDS) nb = UPLOAD_MAX_BACKENDS;
                for (int j = 0; j < nb; j++) {
                    cJSON *be = cJSON_GetArrayItem(backends, j);
                    if (!be) continue;
                    backend_state_t *b = &s->backends[j];
                    cJSON *bname = cJSON_GetObjectItem(be, "name");
                    cJSON *bstatus = cJSON_GetObjectItem(be, "status");
                    cJSON *battempts = cJSON_GetObjectItem(be, "attempts");
                    cJSON *blast = cJSON_GetObjectItem(be, "last_try");
                    if (bname && cJSON_IsString(bname))
                        strlcpy(b->name, bname->valuestring, sizeof(b->name));
                    if (bstatus && cJSON_IsString(bstatus)) {
                        if (strcmp(bstatus->valuestring, "ok") == 0)
                            b->status = ST_OK;
                        else if (strcmp(bstatus->valuestring, "failed") == 0)
                            b->status = ST_FAILED;
                        else
                            b->status = ST_PENDING;
                    }
                    if (battempts && cJSON_IsNumber(battempts))
                        b->attempts = battempts->valueint;
                    if (blast && cJSON_IsNumber(blast))
                        b->last_try_ms = (int64_t)blast->valuedouble;
                }
                s->n_backends = nb;
            }
        }
        state->n_sessions = n;
    }

    cJSON_Delete(root);
    ESP_LOGI(TAG, "loaded %d sessions from state", state->n_sessions);
    return ESP_OK;
}

/* ── Save (atomic) ──────────────────────────────────────────────────── */

esp_err_t uploader_state_save(const upload_state_t *state)
{
    if (!state) return ESP_ERR_INVALID_ARG;

    cJSON *root = cJSON_CreateObject();
    cJSON *sessions_arr = cJSON_CreateArray();

    for (int i = 0; i < state->n_sessions; i++) {
        const session_state_t *s = &state->sessions[i];
        cJSON *sess = cJSON_CreateObject();
        cJSON_AddStringToObject(sess, "id", s->session_id);
        cJSON_AddStringToObject(sess, "day", s->day_folder);

        cJSON *bes = cJSON_CreateArray();
        for (int j = 0; j < s->n_backends; j++) {
            const backend_state_t *b = &s->backends[j];
            cJSON *be = cJSON_CreateObject();
            cJSON_AddStringToObject(be, "name", b->name);
            const char *st = (b->status == ST_OK) ? "ok" :
                             (b->status == ST_FAILED) ? "failed" : "pending";
            cJSON_AddStringToObject(be, "status", st);
            cJSON_AddNumberToObject(be, "attempts", b->attempts);
            cJSON_AddNumberToObject(be, "last_try", (double)b->last_try_ms);
            cJSON_AddItemToArray(bes, be);
        }
        cJSON_AddItemToObject(sess, "backends", bes);
        cJSON_AddItemToArray(sessions_arr, sess);
    }

    cJSON_AddItemToObject(root, "sessions", sessions_arr);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!json_str) {
        ESP_LOGE(TAG, "failed to serialize state");
        return ESP_ERR_NO_MEM;
    }

    FILE *f = fopen(UPLOAD_STATE_TMP, "w");
    if (!f) {
        ESP_LOGE(TAG, "failed to open %s: %s", UPLOAD_STATE_TMP, strerror(errno));
        free(json_str);
        return ESP_FAIL;
    }

    fputs(json_str, f);
    fclose(f);
    free(json_str);

    if (rename(UPLOAD_STATE_TMP, UPLOAD_STATE_PATH) != 0) {
        ESP_LOGE(TAG, "rename failed: %s", strerror(errno));
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "saved %d sessions", state->n_sessions);
    return ESP_OK;
}

/* ── Find / create ──────────────────────────────────────────────────── */

session_state_t *uploader_state_find(upload_state_t *state, const char *session_id)
{
    if (!state || !session_id) return NULL;
    for (int i = 0; i < state->n_sessions; i++) {
        if (strcmp(state->sessions[i].session_id, session_id) == 0)
            return &state->sessions[i];
    }
    return NULL;
}

session_state_t *uploader_state_find_or_create(upload_state_t *state,
                                                const char *session_id,
                                                const char *day_folder)
{
    session_state_t *s = uploader_state_find(state, session_id);
    if (s) return s;

    if (state->n_sessions >= UPLOAD_MAX_SESSIONS) {
        ESP_LOGE(TAG, "session table full (%d)", UPLOAD_MAX_SESSIONS);
        return NULL;
    }

    s = &state->sessions[state->n_sessions++];
    memset(s, 0, sizeof(*s));
    strlcpy(s->session_id, session_id, sizeof(s->session_id));
    strlcpy(s->day_folder, day_folder, sizeof(s->day_folder));
    return s;
}

backend_state_t *uploader_state_backend_find_or_create(session_state_t *sess,
                                                        const char *backend_name)
{
    if (!sess || !backend_name) return NULL;
    for (int i = 0; i < sess->n_backends; i++) {
        if (strcmp(sess->backends[i].name, backend_name) == 0)
            return &sess->backends[i];
    }
    if (sess->n_backends >= UPLOAD_MAX_BACKENDS) return NULL;
    backend_state_t *b = &sess->backends[sess->n_backends++];
    memset(b, 0, sizeof(*b));
    strlcpy(b->name, backend_name, sizeof(b->name));
    b->status = ST_PENDING;
    return b;
}

void uploader_state_set_backend_status(upload_state_t *state,
                                        const char *session_id,
                                        const char *backend_name,
                                        upload_status_t status)
{
    session_state_t *s = uploader_state_find(state, session_id);
    if (!s) return;
    backend_state_t *b = uploader_state_backend_find_or_create(s, backend_name);
    if (!b) return;
    b->status = status;
    if (status == ST_FAILED || status == ST_OK) {
        b->attempts++;
        b->last_try_ms = (int64_t)time(NULL) * 1000;
    }
}

/* ── Get pending sessions ───────────────────────────────────────────── */

int uploader_state_get_pending(const upload_state_t *state,
                                const char *backend_names[], int n_backends,
                                const char *out_ids[], const char *out_days[],
                                int max_out)
{
    if (!state || !out_ids || !out_days) return 0;
    int count = 0;

    for (int i = 0; i < state->n_sessions && count < max_out; i++) {
        const session_state_t *s = &state->sessions[i];
        bool needs_work = false;

        for (int j = 0; j < s->n_backends; j++) {
            if (s->backends[j].status != ST_OK) {
                needs_work = true;
                break;
            }
        }

        /* Also check if any configured backend has no entry yet */
        if (!needs_work) {
            for (int k = 0; k < n_backends && !needs_work; k++) {
                bool found = false;
                for (int j = 0; j < s->n_backends; j++) {
                    if (strcmp(s->backends[j].name, backend_names[k]) == 0) {
                        found = true;
                        break;
                    }
                }
                if (!found) needs_work = true;
            }
        }

        if (needs_work) {
            out_ids[count] = s->session_id;
            out_days[count] = s->day_folder;
            count++;
        }
    }
    return count;
}

/* ── Prune old completed sessions ───────────────────────────────────── */

int uploader_state_prune(upload_state_t *state, int max_age_days)
{
    if (!state) return 0;
    int pruned = 0;
    int64_t now_s = time(NULL);
    int64_t cutoff_s = now_s - (int64_t)max_age_days * 86400;

    int write_idx = 0;
    for (int read_idx = 0; read_idx < state->n_sessions; read_idx++) {
        session_state_t *s = &state->sessions[read_idx];
        bool all_ok = true;

        for (int j = 0; j < s->n_backends; j++) {
            if (s->backends[j].status != ST_OK) {
                all_ok = false;
                break;
            }
        }

        if (all_ok && s->n_backends > 0) {
            /* Parse day_folder as YYYYMMDD and compare to cutoff */
            int year, month, day;
            if (sscanf(s->day_folder, "%4d%2d%2d", &year, &month, &day) == 3) {
                struct tm tm = {0};
                tm.tm_year = year - 1900;
                tm.tm_mon = month - 1;
                tm.tm_mday = day;
                int64_t sess_s = (int64_t)mktime(&tm);
                if (sess_s < cutoff_s) {
                    pruned++;
                    continue; /* skip copying this one */
                }
            }
        }

        if (write_idx != read_idx) {
            state->sessions[write_idx] = state->sessions[read_idx];
        }
        write_idx++;
    }

    state->n_sessions = write_idx;
    if (pruned > 0) {
        ESP_LOGI(TAG, "pruned %d old completed sessions", pruned);
    }
    return pruned;
}

/* ── State to JSON (for web UI) ─────────────────────────────────────── */

esp_err_t uploader_state_to_json(const upload_state_t *state, char **out_json)
{
    if (!state || !out_json) return ESP_ERR_INVALID_ARG;

    cJSON *root = cJSON_CreateObject();
    cJSON *sessions_arr = cJSON_CreateArray();

    int pending = 0, failed = 0;

    for (int i = 0; i < state->n_sessions; i++) {
        const session_state_t *s = &state->sessions[i];
        cJSON *sess = cJSON_CreateObject();
        cJSON_AddStringToObject(sess, "id", s->session_id);
        cJSON_AddStringToObject(sess, "day", s->day_folder);

        cJSON *bes = cJSON_CreateObject();
        bool has_failed = false;
        for (int j = 0; j < s->n_backends; j++) {
            const backend_state_t *b = &s->backends[j];
            const char *st = (b->status == ST_OK) ? "ok" :
                             (b->status == ST_FAILED) ? "failed" : "pending";
            cJSON_AddStringToObject(bes, b->name, st);
            cJSON_AddNumberToObject(bes, "_attempts", b->attempts);
            if (b->status == ST_FAILED) has_failed = true;
            if (b->status == ST_PENDING) pending++;
        }
        cJSON_AddItemToObject(sess, "backends", bes);
        if (has_failed) failed++;
        cJSON_AddItemToArray(sessions_arr, sess);
    }

    cJSON_AddItemToObject(root, "sessions", sessions_arr);
    cJSON_AddNumberToObject(root, "pending", pending);
    cJSON_AddNumberToObject(root, "failed", failed);
    cJSON_AddNumberToObject(root, "total", state->n_sessions);

    *out_json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    return *out_json ? ESP_OK : ESP_ERR_NO_MEM;
}
