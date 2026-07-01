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

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* ── Upload state tracking ─────────────────────────────────────────────
 *
 * Persistent state is stored as a single JSON file on LittleFS
 * (/littlefs/upload_state.json). The file is rewritten atomically
 * (write to .tmp, rename) on each state change.
 *
 * Session-level tracking: each session has per-backend status.
 * No per-file fingerprinting — a session is either fully uploaded
 * to a backend or not. */

#define UPLOAD_STATE_PATH    "/littlefs/upload_state.json"
#define UPLOAD_STATE_TMP     "/littlefs/upload_state.json.tmp"
#define UPLOAD_MAX_SESSIONS  16
#define UPLOAD_MAX_BACKENDS  4
#define UPLOAD_BACKEND_NAME_LEN 16

typedef enum {
    ST_PENDING,     /* not yet attempted or waiting for retry */
    ST_OK,          /* successfully uploaded */
    ST_FAILED,      /* attempted but failed */
} upload_status_t;

typedef struct {
    char name[UPLOAD_BACKEND_NAME_LEN];
    upload_status_t status;
    int attempts;
    int64_t last_try_ms;   /* epoch ms of last attempt */
} backend_state_t;

typedef struct {
    char session_id[32];       /* e.g. "20260629_232738" */
    char day_folder[16];       /* e.g. "20260629" */
    int n_backends;
    backend_state_t backends[UPLOAD_MAX_BACKENDS];
} session_state_t;

typedef struct {
    int n_sessions;
    session_state_t sessions[UPLOAD_MAX_SESSIONS];
} upload_state_t;

/* Load state from LittleFS. Returns ESP_OK on success.
 * If the file doesn't exist, starts with empty state. */
esp_err_t uploader_state_load(upload_state_t *state);

/* Save state to LittleFS atomically. */
esp_err_t uploader_state_save(const upload_state_t *state);

/* Find or create a session entry in the state.
 * Returns pointer to the session entry, or NULL if full. */
session_state_t *uploader_state_find_or_create(upload_state_t *state,
                                                const char *session_id,
                                                const char *day_folder);

/* Find a session entry by session_id. Returns NULL if not found. */
session_state_t *uploader_state_find(upload_state_t *state,
                                      const char *session_id);

/* Find or create a backend entry within a session. */
backend_state_t *uploader_state_backend_find_or_create(session_state_t *sess,
                                                        const char *backend_name);

/* Update a backend's status for a session. */
void uploader_state_set_backend_status(upload_state_t *state,
                                        const char *session_id,
                                        const char *backend_name,
                                        upload_status_t status);

/* Get a list of sessions that need retry (status != OK for any
 * configured backend, and attempts < max_attempts).
 * Fills out_ids and out_day_folders with up to max_out entries.
 * Returns the number of sessions needing retry. */
int uploader_state_get_pending(const upload_state_t *state,
                                const char *backend_names[], int n_backends,
                                const char *out_ids[], const char *out_days[],
                                int max_out);

/* Prune sessions older than max_age_days that have all backends OK.
 * Returns number of sessions pruned. */
int uploader_state_prune(upload_state_t *state, int max_age_days);

/* Convert state to JSON string for web UI. Caller must free(). */
esp_err_t uploader_state_to_json(const upload_state_t *state, char **out_json);
