/*
 * SomnoTrace - Minimal cJSON test shim for host unit tests
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
#include <string.h>

typedef struct cJSON {
    struct cJSON *child;
    struct cJSON *next;
    char *string;
    char *valuestring;
    int type;
} cJSON;

#define cJSON_String 16

static inline cJSON *cJSON_GetObjectItem(const cJSON *o, const char *key)
{
    if (!o) return NULL;
    for (cJSON *c = o->child; c; c = c->next) {
        if (c->string && key && strcmp(c->string, key) == 0) return c;
    }
    return NULL;
}

static inline bool cJSON_IsString(const cJSON *i)
{
    return i && i->type == cJSON_String;
}
