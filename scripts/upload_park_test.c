/*
 * SomnoTrace - Host unit tests for the upload group parking policy
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

#include <stdio.h>

#include "upload_park.h"

/* Deliberately NOT assert(): a suite built with -DNDEBUG would elide every
 * assertion and still exit 0, which is a green run that checked nothing. */
static int g_failures;
#define CHECK(cond, msg)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, (msg));          \
            g_failures++;                                                     \
        }                                                                     \
    } while (0)

/* A synced "now" well clear of the guard, and a group at the attempt cap. */
#define NOW    (UPLOAD_CLOCK_SYNCED_S + 10u * UPLOAD_PARK_REVIVE_S)
#define CAP    UPLOAD_MAX_GROUP_ATTEMPTS

static void test_only_failed_groups_park(void)
{
    CHECK(!upload_park_active(false, 255, NOW, NOW),
          "a group that is not FAILED is never parked, whatever its counter");
    CHECK(upload_park_active(true, CAP, NOW, NOW),
          "a FAILED group at the cap, just tried, is parked");
}

static void test_below_the_cap_never_parks(void)
{
    for (int a = 0; a < CAP; a++)
        CHECK(!upload_park_active(true, (uint8_t)a, NOW, NOW),
              "fewer than UPLOAD_MAX_GROUP_ATTEMPTS failures never park");
    CHECK(upload_park_active(true, CAP, NOW, NOW),
          "the cap itself parks");
}

/* attempts saturates at UINT8_MAX in upload_sched.c; the saturated value
 * must still read as parked, not wrap back to "fresh". */
static void test_saturated_counter_stays_parked(void)
{
    CHECK(upload_park_active(true, 255, NOW, NOW),
          "a saturated attempt counter is still parked");
}

/* The revive window, stated at its exact edge on both sides. */
static void test_revive_boundary_is_exact(void)
{
    CHECK(upload_park_active(true, CAP, NOW - (UPLOAD_PARK_REVIVE_S - 1), NOW),
          "one second inside the window stays parked");
    CHECK(!upload_park_active(true, CAP, NOW - UPLOAD_PARK_REVIVE_S, NOW),
          "exactly one window later the group revives");
    CHECK(!upload_park_active(true, CAP, NOW - 2 * UPLOAD_PARK_REVIVE_S, NOW),
          "and stays revived after that");
}

/* The divergence the Python model had: a clock stepped backward leaves
 * last_try_s in the future.  The unsigned age wraps and the group revives
 * early — one send, instead of trusting a timestamp known to be wrong. */
static void test_clock_stepped_backward_revives(void)
{
    CHECK(!upload_park_active(true, CAP, NOW + 500, NOW),
          "clock stepped back 500 s revives");
    CHECK(!upload_park_active(true, CAP, NOW + 3600, NOW),
          "clock stepped back 1 h revives");
    CHECK(!upload_park_active(true, CAP, NOW + 1, NOW),
          "clock stepped back 1 s revives");
}

static void test_unsynced_clock_holds_parking(void)
{
    CHECK(upload_park_active(true, CAP, 0, 1000),
          "while the clock is unsynced a parked group stays parked");
    CHECK(upload_park_active(true, CAP, 0, UPLOAD_CLOCK_SYNCED_S - 1),
          "right up to the sync guard");
    CHECK(!upload_park_active(true, CAP, 0, UPLOAD_CLOCK_SYNCED_S),
          "and a 1970-era last_try_s revives the moment the clock is synced");
}

/* ANTI-VACUITY: a sweep over the age must reach both verdicts, so a function
 * that answered the same thing for every age cannot pass. */
static void test_both_verdicts_are_reachable(void)
{
    int saw_parked = 0, saw_free = 0;
    for (uint32_t age = 0; age <= 2 * UPLOAD_PARK_REVIVE_S; age += 600) {
        if (upload_park_active(true, CAP, NOW - age, NOW)) saw_parked = 1;
        else saw_free = 1;
    }
    CHECK(saw_parked, "the sweep reaches parked");
    CHECK(saw_free, "the sweep reaches revived");
}

int main(void)
{
    test_only_failed_groups_park();
    test_below_the_cap_never_parks();
    test_saturated_counter_stays_parked();
    test_revive_boundary_is_exact();
    test_clock_stepped_backward_revives();
    test_unsynced_clock_holds_parking();
    test_both_verdicts_are_reachable();

    if (g_failures) {
        printf("upload_park tests FAILED (%d)\n", g_failures);
        return 1;
    }
    printf("upload_park tests passed\n");
    return 0;
}
