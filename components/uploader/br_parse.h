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

#pragma once

#include <stdbool.h>

/* ── BreathingRoom API response parsing ───────────────────────────────
 *
 * Deliberately ESP-free (cJSON + libc only) so the parsers run under the
 * host test harness. Verified response shapes, live 2026-10-14:
 *
 *   POST /api/imports/upload   -> 201 {"created":N,"skipped":N,
 *                                      "deferred":N,"sessions":[...]}
 *   POST /api/oximetry/upload  -> 200 {"results":[{filename,status,id?…},
 *                                      …]}  status is imported | replaced
 *                                      | rejected (+reason) */

typedef struct {
    int created;     /* new sessions accepted for parse                    */
    int skipped;     /* sessions already present — normal, counts as OK   */
    int deferred;    /* over the 1000-session parse queue — NOT stored,
                        must be re-sent later                              */
    int n_sessions;  /* entries in sessions[]                              */
} br_import_result_t;

typedef struct {
    int  n_results;                       /* entries in results[]         */
    int  n_imported;                      /* imported + replaced          */
    int  n_rejected;
    char first_id[48];                    /* first imported/replaced id,
                                             "" when none                  */
    char first_reason[128];               /* first rejection reason,
                                             "" when none                  */
} br_ox_result_t;

/* Parse a /api/imports/upload response. true = valid response object. */
bool br_parse_import(const char *body, br_import_result_t *out);

/* Parse a /api/oximetry/upload response. true = valid response object.
 * A per-file "rejected" entry does NOT make this return false — the
 * request itself succeeded; the counts carry the detail. */
bool br_parse_oximetry(const char *body, br_ox_result_t *out);
bool br_parse_oximetry_files(const char *body, br_ox_result_t *out,
                              const char *const *filenames, int n_files);

/* POST /api/sleep/upload answers the same {results:[{filename,status,id?,
 * reason?}]} shape as the oximetry route (plus fields we ignore), so the
 * stage parser is the oximetry parser under a name that says what it reads. */
bool br_parse_stage_files(const char *body, br_ox_result_t *out,
                          const char *const *filenames, int n_files);
