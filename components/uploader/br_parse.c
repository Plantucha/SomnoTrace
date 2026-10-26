/*
 * SomnoTrace - BreathingRoom response parsing (pure C, host-testable)
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

#include "br_parse.h"

#include <string.h>
#include <stdio.h>
#include "cJSON.h"

static bool jnum(const cJSON *o, const char *key, int *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (!cJSON_IsNumber(v) || v->valueint < 0 ||
        v->valuedouble != (double)v->valueint) return false;
    *out = v->valueint;
    return true;
}

static void jstr(char *dst, size_t cap, const cJSON *o, const char *key)
{
    if (!dst || cap == 0) return;
    dst[0] = '\0';
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (cJSON_IsString(v) && v->valuestring)
        snprintf(dst, cap, "%s", v->valuestring);
}

bool br_parse_import(const char *body, br_import_result_t *out)
{
    if (!body || !out) return false;
    memset(out, 0, sizeof(*out));

    cJSON *root = cJSON_ParseWithOpts(body, NULL, true);
    if (!root) return false;
    cJSON *sess = cJSON_GetObjectItemCaseSensitive(root, "sessions");
    if (!cJSON_IsObject(root) || !cJSON_IsArray(sess) ||
        !jnum(root, "created", &out->created) ||
        !jnum(root, "skipped", &out->skipped) ||
        !jnum(root, "deferred", &out->deferred)) {
        cJSON_Delete(root);
        return false;
    }

    out->n_sessions = cJSON_GetArraySize(sess);
    cJSON_Delete(root);
    return true;
}

bool br_parse_stage_files(const char *body, br_ox_result_t *out,
                          const char *const *filenames, int n_files)
{
    return br_parse_oximetry_files(body, out, filenames, n_files);
}

bool br_parse_oximetry_files(const char *body, br_ox_result_t *out,
                              const char *const *filenames, int n_files)
{
    if (!body || !out) return false;
    memset(out, 0, sizeof(*out));

    cJSON *root = cJSON_ParseWithOpts(body, NULL, true);
    if (!root) return false;
    cJSON *results = cJSON_GetObjectItemCaseSensitive(root, "results");
    if (!cJSON_IsObject(root) || !cJSON_IsArray(results) ||
        (filenames && cJSON_GetArraySize(results) != n_files)) {
        cJSON_Delete(root);
        return false;
    }

    out->n_results = cJSON_GetArraySize(results);
    cJSON *r;
    cJSON_ArrayForEach(r, results) {
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(r, "filename");
        const cJSON *status = cJSON_GetObjectItemCaseSensitive(r, "status");
        if (!cJSON_IsObject(r) || !cJSON_IsString(name) || !name->valuestring[0] ||
            !cJSON_IsString(status)) goto invalid;
        for (cJSON *prev = results->child; prev != r; prev = prev->next) {
            const cJSON *pn = cJSON_GetObjectItemCaseSensitive(prev, "filename");
            if (strcmp(pn->valuestring, name->valuestring) == 0) goto invalid;
        }
        if (filenames) {
            bool found = false;
            for (int i = 0; i < n_files; i++)
                if (filenames[i] && strcmp(filenames[i], name->valuestring) == 0)
                    found = true;
            if (!found) goto invalid;
        }
        if (strcmp(status->valuestring, "imported") == 0 ||
            strcmp(status->valuestring, "replaced") == 0) {
            const cJSON *id = cJSON_GetObjectItemCaseSensitive(r, "id");
            if (!cJSON_IsString(id) || !id->valuestring[0] ||
                strlen(id->valuestring) >= sizeof(out->first_id)) goto invalid;
            out->n_imported++;
            if (!out->first_id[0])
                jstr(out->first_id, sizeof(out->first_id), r, "id");
        } else if (strcmp(status->valuestring, "rejected") == 0) {
            out->n_rejected++;
            if (!out->first_reason[0]) {
                jstr(out->first_reason, sizeof(out->first_reason), r, "reason");
                if (!out->first_reason[0])
                    snprintf(out->first_reason, sizeof(out->first_reason),
                             "%s", status->valuestring);
            }
        } else {
            goto invalid;
        }
    }
    cJSON_Delete(root);
    return true;

invalid:
    cJSON_Delete(root);
    memset(out, 0, sizeof(*out));
    return false;
}

bool br_parse_oximetry(const char *body, br_ox_result_t *out)
{
    return br_parse_oximetry_files(body, out, NULL, 0);
}
