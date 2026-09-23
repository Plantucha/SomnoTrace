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

#include "legacy_file_list.h"

#include <string.h>

int legacy_parse_file_list(const char *file_list, bool source_truncated,
                           char names[][LEGACY_FILE_NAME_MAX], int max_count,
                           legacy_file_list_stats_t *stats)
{
    legacy_file_list_stats_t local;
    memset(&local, 0, sizeof(local));
    local.source_truncated = source_truncated;

    if (!file_list || !file_list[0] || !names || max_count <= 0) {
        if (stats) *stats = local;
        return 0;
    }

    int count = 0;
    const char *p = file_list;

    while (*p) {
        const char *comma = strchr(p, ',');
        size_t len = comma ? (size_t)(comma - p) : strlen(p);

        /* The last element of a TRUNCATED response is whatever survived the
         * cut.  It is dropped on the flag rather than on any property of the
         * text: a fragment of `20260923041114` is still a well-formed-looking
         * name, so no length or character test can tell it from a real one. */
        if (!comma && source_truncated) {
            local.skipped++;
            break;
        }

        if (count >= max_count) {
            /* Nothing after this fits either — stop, and say so. */
            local.overflow = true;
            break;
        }

        if (len == 0 || len >= LEGACY_FILE_NAME_MAX) {
            local.skipped++;
        } else {
            memcpy(names[count], p, len);
            names[count][len] = '\0';
            count++;
        }

        if (!comma) break;
        p = comma + 1;
    }

    local.count = count;
    if (stats) *stats = local;
    return count;
}

bool legacy_file_list_complete(const legacy_file_list_stats_t *stats)
{
    if (!stats) return false;
    return !stats->source_truncated && !stats->overflow && stats->skipped == 0;
}
