/*
 * SomnoTrace - Session upload system for SMB and SleepHQ backends
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

#include "esp_err.h"
#include <stdbool.h>

/* ── Upload backend interface ──────────────────────────────────────────
 *
 * Each backend (SMB, SleepHQ, future additions) implements this interface.
 * Backends are registered at init and called sequentially by the upload
 * orchestration task — one at a time to avoid memory contention between
 * TLS (SleepHQ) and SMB socket buffers. */

typedef enum {
    UPLOAD_OK,              /* session fully uploaded to this backend      */
    UPLOAD_FAILED,          /* upload attempted but failed                  */
    UPLOAD_NOT_CONFIGURED,  /* backend has no valid config — skipped        */
} upload_result_t;

typedef struct {
    const char *name;       /* "smb" | "sleephq" | ...                      */

    /* Check if this backend has valid configuration (config keys in NVS). */
    bool (*is_configured)(void);

    /* Upload all EDF files for one session + mandatory root files.
     *
     * Parameters:
     *   session_id  - file prefix, e.g. "20260629_232738"
     *   day_folder  - noon-based DATALOG subfolder, e.g. "20260629"
     *
     * Returns UPLOAD_OK on success, UPLOAD_FAILED on error. */
    upload_result_t (*upload_session)(const char *session_id,
                                       const char *day_folder);
} upload_backend_t;

/* ── Configuration ──────────────────────────────────────────────────── */

typedef struct {
    /* SMB server */
    char smb_host[64];       /* server IP or hostname                      */
    char smb_share[64];      /* share name (e.g. "cpap")                   */
    char smb_user[64];       /* username (empty = guest)                   */
    char smb_pass[64];       /* password (empty = guest)                   */
    char smb_path[128];      /* remote path within share (e.g. "/SomnoTrace") */

    /* SleepHQ */
    char shq_client_id[128];      /* API key (Client UID)                */
    char shq_client_secret[128];  /* Client Secret                       */
} uploader_config_t;

/* ── Public API ─────────────────────────────────────────────────────── */

/* Initialise the uploader subsystem.
 * - Mounts LittleFS on the "storage" partition
 * - Loads upload state and config from NVS
 * - Starts the persistent upload task
 * Call once at boot after nvs_flash_init() and sd_storage_init(). */
esp_err_t uploader_init(void);

/* Notify the uploader that EDF generation for a session is complete.
 * This is the primary event trigger — called after edf_gen_generate()
 * finishes successfully. Adds the session to the upload queue and wakes
 * the upload task. Safe to call from any task. */
void uploader_on_session_ready(const char *session_id, const char *day_folder);

/* Register a backend. Called during uploader_init() for built-in backends. */
void uploader_register_backend(const upload_backend_t *backend);

/* Load / save upload configuration from / to NVS. */
esp_err_t uploader_load_config(uploader_config_t *cfg);
esp_err_t uploader_save_config(const uploader_config_t *cfg);

/* Check if specific backends are configured. */
bool uploader_is_smb_configured(void);
bool uploader_is_sleephq_configured(void);

/* Get upload status as a JSON string (for web UI).
 * Caller must free() the returned string. */
esp_err_t uploader_get_status_json(char **out_json);

/* Get upload config as a JSON string (for web UI).
 * Passwords/secrets are masked. Caller must free() the returned string. */
esp_err_t uploader_get_config_json(char **out_json);

/* Save upload config from a JSON string (from web UI POST body).
 * Parses the JSON and stores values in NVS. */
esp_err_t uploader_save_config_json(const char *json_str);
