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

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Byte layout of the 48-byte Format-A trailer, counted from its first byte. */
#define OXYII_TRAILER_BYTES            48
#define OXYII_TRAILER_MAGIC_OFFSET      4   /* 48 12 5A DA - finalisation mark          */
#define OXYII_TRAILER_COUNT_OFFSET     12   /* u32 LE: the recording's own sample count */
#define OXYII_TRAILER_INTERVAL_OFFSET  16   /* u8: seconds per sample                   */

/* Cadences above this are rejected as implausible rather than trusted. The
 * interval is a one-byte field, so a corrupted trailer can present any value;
 * a minute between samples is already far outside anything the ring records. */
#define OXYII_TRAILER_MAX_INTERVAL_S   60

typedef struct {
    uint32_t sample_count;   /* as STATED by the file, not derived from its size */
    uint8_t  interval_s;     /* seconds per sample, as stated                    */
    uint32_t period_us;      /* interval_s scaled, after validation              */
    bool     interval_valid; /* false => period_us fell back to 1 Hz             */
} oxyii_trailer_t;

/* Parse a Format-A trailer from the LAST OXYII_TRAILER_BYTES bytes of a file.
 * Returns false when the finalisation magic is absent, which is the reliable
 * "this file is complete" predicate - a file of the right size can still be
 * missing its trailer.
 *
 * The interval is validated, never trusted blindly: an absent or implausible
 * value leaves period_us at 1 Hz and clears interval_valid, so a damaged
 * trailer degrades to the previous behaviour instead of fabricating a cadence. */
bool oxyii_trailer_parse(const uint8_t *trailer, size_t len, oxyii_trailer_t *out);

#ifdef __cplusplus
}
#endif