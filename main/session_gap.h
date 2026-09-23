/*
 * SomnoTrace - StreamData gap policy: pad, split, or neither
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

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* WHY THIS IS A HEADER AND NOT TWO `if`s IN session_writer.c
 *
 * #279 fixed a dead zone: gaps of 10.0-10.1 s were padded with ~10 s of
 * sample-and-hold waveform instead of splitting the session.  The cause was
 * not the arithmetic — it was that the pad ceiling and the split threshold
 * were two numbers that NEARLY agreed, expressed in different units (a
 * notification count and a millisecond value), with nothing holding them in
 * step.  Fixing the conditions closed today's gap; it did not stop them
 * drifting apart the next time either is tuned.
 *
 * So the decision lives in one pure function that a host test can link
 * directly.  The test asserts what the conditions MEAN — that PAD and SPLIT
 * are disjoint and exhaustive, and that the boundary is exactly
 * SW_SPLIT_GAP_MS — rather than re-stating the arithmetic, which a copy would
 * only re-state wrongly.
 */

/* A StreamData discontinuity at least this long is not compensated - the
 * session is split so the gap is never rendered as continuous samples. */
#ifndef SW_SPLIT_GAP_MS
#define SW_SPLIT_GAP_MS         10000    /* 10 s */
#endif

/* Nominal StreamData notification period, and the slack subtracted before
 * counting how many went missing (so ordinary jitter rounds to zero). */
#ifndef SW_GAP_NOTIFY_MS
#define SW_GAP_NOTIFY_MS          200
#endif
#ifndef SW_GAP_NOTIFY_SLACK_MS
#define SW_GAP_NOTIFY_SLACK_MS    100
#endif

/* Below this a gap is jitter, not a lost notification. */
#ifndef SW_GAP_JITTER_MS
#define SW_GAP_JITTER_MS          280
#endif

typedef enum {
    SW_GAP_NONE = 0,   /* jitter - append normally                          */
    SW_GAP_PAD,        /* hold-last-value for `missing` notifications        */
    SW_GAP_SPLIT,      /* a true dropout - finalise and start a new session  */
} sw_gap_action_t;

/* What to do about a StreamData discontinuity of `gap_ms`.
 *
 * `missing_out` (optional) receives the number of notifications the gap
 * accounts for, which is what sizes the padding.  It is reported for every
 * action, including SPLIT, because the split path logs it.
 *
 * Pure: no state, no logging, no allocation. */
static inline sw_gap_action_t sw_gap_action(int64_t gap_ms, int *missing_out)
{
    int missing = 0;
    if (gap_ms > SW_GAP_JITTER_MS)
        missing = (int)((gap_ms - SW_GAP_NOTIFY_SLACK_MS) / SW_GAP_NOTIFY_MS);
    if (missing < 0) missing = 0;
    if (missing_out) *missing_out = missing;

    /* The threshold is tested FIRST and in milliseconds on both sides, so no
     * gap can be long enough to split and still be eligible to pad.  That
     * ordering is the invariant #279 restored and this header protects. */
    if (gap_ms >= SW_SPLIT_GAP_MS) return SW_GAP_SPLIT;
    if (missing > 0)               return SW_GAP_PAD;
    return SW_GAP_NONE;
}

#ifdef __cplusplus
}
#endif
