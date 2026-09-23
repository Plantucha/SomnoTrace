/*
 * SomnoTrace - Wellue OxyII "Format A" trailer decoding
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

#include "oxyii_trailer.h"

bool oxyii_trailer_parse(const uint8_t *trailer, size_t len, oxyii_trailer_t *out)
{
    if (!trailer || !out || len < OXYII_TRAILER_BYTES) return false;

    const uint8_t *m = trailer + OXYII_TRAILER_MAGIC_OFFSET;
    if (m[0] != 0x48 || m[1] != 0x12 || m[2] != 0x5a || m[3] != 0xda)
        return false;

    oxyii_trailer_t t;
    const uint8_t *c = trailer + OXYII_TRAILER_COUNT_OFFSET;
    t.sample_count = (uint32_t)c[0] | ((uint32_t)c[1] << 8) |
                     ((uint32_t)c[2] << 16) | ((uint32_t)c[3] << 24);
    t.interval_s = trailer[OXYII_TRAILER_INTERVAL_OFFSET];

    t.interval_valid = t.interval_s > 0 && t.interval_s <= OXYII_TRAILER_MAX_INTERVAL_S;
    t.period_us = t.interval_valid ? (uint32_t)t.interval_s * 1000000u : 1000000u;

    *out = t;
    return true;
}