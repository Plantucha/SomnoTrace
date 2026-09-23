/*
 * SomnoTrace - Gen1 (legacy) CMD_INFO FileList parsing
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

#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Longest filename slot, including the terminator.  Gen1 recording names are
 * 14 characters (`YYYYMMDDhhmmss`), optionally with a `.vld` suffix. */
#define LEGACY_FILE_NAME_MAX 32

/* Why the parsed list may not be the ring's whole list.
 *
 * The Gen1 command set is FILE_OPEN / FILE_READ / FILE_CLOSE / INFO / PING /
 * CONFIG — there is no delete.  SomnoTrace marks a recording pulled in its own
 * index and skips it next time, but the file stays on the device, so the
 * FileList grows until the ring's own retention prunes it.  Every ceiling below
 * is therefore reachable on a device with more storage than the host assumed,
 * and each one used to drop recordings without saying a word. */
typedef struct {
    int  count;             /* names written to the caller's array            */
    int  skipped;           /* entries rejected: empty, over-long, or a       */
                            /* fragment left by a truncated response          */
    bool source_truncated;  /* the response itself was cut before parsing     */
    bool overflow;          /* more names present than the array can hold     */
} legacy_file_list_stats_t;

/* Split a CMD_INFO FileList (comma-separated) into `names`.
 *
 * `source_truncated` says the caller's buffer was too small for the response.
 * That matters to parsing, not just to reporting: a truncated list ends
 * mid-name, and the final element then has no comma to vouch for it.  A
 * fragment that still looks like a filename would be pulled, fail, and abort
 * the whole sync attempt — so it is dropped and counted, never guessed at.
 *
 * Returns the number of names written; `stats` (optional) explains any
 * shortfall.  Pure: no logging, no allocation, no device access. */
int legacy_parse_file_list(const char *file_list, bool source_truncated,
                           char names[][LEGACY_FILE_NAME_MAX], int max_count,
                           legacy_file_list_stats_t *stats);

/* True when the parse saw the ring's entire list — nothing truncated, nothing
 * overflowed, nothing skipped.  A false here means recordings exist on the
 * device that this sync cannot see. */
bool legacy_file_list_complete(const legacy_file_list_stats_t *stats);

#ifdef __cplusplus
}
#endif
