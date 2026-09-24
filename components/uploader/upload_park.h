/*
 * SomnoTrace - Upload group parking policy
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
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* WHY THIS IS A HEADER: the park decision is a pure function, so the host
 * test (scripts/upload_park_test.c) links this exact code instead of a
 * re-implementation that can drift from it.  The Python contract test reads
 * the constants below from this file for the same reason. */

/* Groups that fail this many times are parked: they no longer count toward
 * "pending", so one permanently bad file cannot keep its day on the upload
 * list forever.  The status stays UG_FAILED for the UI.  Parking is
 * time-limited: the check itself goes stale after UPLOAD_PARK_REVIVE_S, so a
 * transient server outage cannot strand a day, while a truly bad file costs
 * at most one send per window (last_try_s refreshes on every try, which
 * self-paces the retries).  A parked group also revives immediately when the
 * reconcile sees its file set change (status/attempts reset to pending) or
 * the day is invalidated. */
#define UPLOAD_MAX_GROUP_ATTEMPTS    5
#define UPLOAD_PARK_REVIVE_S     86400u   /* parked groups retry once a day */

/* Wall clock below this is unsynced (before ~Nov 2023). */
#define UPLOAD_CLOCK_SYNCED_S    1700000000u

/* A group is parked once it has failed UPLOAD_MAX_GROUP_ATTEMPTS times —
 * until the parking goes stale, after which it gets one retry per window.
 * The wall-clock guard skips the staleness test while time is unsynced; a
 * 1970-era last_try_s (failure before NTP sync) reads as ancient and revives
 * immediately, which is the desired behaviour anyway.
 *
 * The age is unsigned on purpose: if the clock steps backward (AS11-derived
 * time later corrected by NTP) last_try_s lies in the future, the age wraps
 * huge, and the group revives early.  That costs one send; staying parked on
 * a timestamp that is known to be wrong would be the worse failure. */
static inline bool upload_park_active(bool failed, uint8_t attempts,
                                      uint32_t last_try_s, uint32_t now_s)
{
    if (!failed || attempts < UPLOAD_MAX_GROUP_ATTEMPTS) return false;
    if (now_s < UPLOAD_CLOCK_SYNCED_S) return true;
    return (uint32_t)(now_s - last_try_s) < UPLOAD_PARK_REVIVE_S;
}

#ifdef __cplusplus
}
#endif
