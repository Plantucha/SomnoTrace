/*
 * SomnoTrace - Host unit tests for the StreamData gap policy
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

#include "session_gap.h"

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

/* The sweep bound: comfortably past the split threshold, so the whole
 * interesting range is covered by every exhaustive property below. */
#define SWEEP_MAX_MS (SW_SPLIT_GAP_MS * 2)

/* THE REGRESSION FOR #279.  Padding fabricates sample-and-hold waveform, so a
 * gap long enough to split must never also be eligible to pad — otherwise a
 * radio dropout is rendered as ~10 s of continuous breathing, which scores as
 * physiology in OSCAR / SleepHQ.  This is the property, not the arithmetic:
 * it stays true however the notification period is retuned. */
static void test_no_gap_is_both_padded_and_long_enough_to_split(void)
{
    for (int64_t gap = 0; gap <= SWEEP_MAX_MS; gap++) {
        sw_gap_action_t a = sw_gap_action(gap, NULL);
        if (gap >= SW_SPLIT_GAP_MS)
            CHECK(a == SW_GAP_SPLIT,
                  "a gap at or past the split threshold must split");
        if (a == SW_GAP_PAD)
            CHECK(gap < SW_SPLIT_GAP_MS, "padding never reaches the threshold");
    }
}

/* No dead zone in the other direction either: every gap gets exactly one
 * verdict, so none can fall between the branches and be silently ignored. */
static void test_the_three_actions_are_exhaustive_and_disjoint(void)
{
    for (int64_t gap = 0; gap <= SWEEP_MAX_MS; gap++) {
        sw_gap_action_t a = sw_gap_action(gap, NULL);
        int is_none = (a == SW_GAP_NONE);
        int is_pad = (a == SW_GAP_PAD);
        int is_split = (a == SW_GAP_SPLIT);
        CHECK(is_none + is_pad + is_split == 1,
              "exactly one action applies to any gap");
    }
}

/* The boundary itself, stated once so a retune has to come through here. */
static void test_the_boundary_is_the_threshold_exactly(void)
{
    int missing = 0;
    CHECK(sw_gap_action(SW_SPLIT_GAP_MS - 1, &missing) == SW_GAP_PAD,
          "one millisecond below the threshold still pads");
    CHECK(missing > 0, "and that padding is sized by a real notification count");
    CHECK(sw_gap_action(SW_SPLIT_GAP_MS, &missing) == SW_GAP_SPLIT,
          "the threshold itself splits");
    CHECK(sw_gap_action(SW_SPLIT_GAP_MS + 1, &missing) == SW_GAP_SPLIT,
          "and everything past it splits");
}

/* The 100 ms dead zone #279 closed, named explicitly: with a 10 s threshold
 * these are the gaps that used to be padded with ~10 s of held waveform. */
static void test_the_reported_dead_zone_splits(void)
{
    for (int64_t gap = SW_SPLIT_GAP_MS; gap <= SW_SPLIT_GAP_MS + 100; gap++)
        CHECK(sw_gap_action(gap, NULL) == SW_GAP_SPLIT,
              "the 10.0-10.1 s band splits rather than padding");
}

/* Padding only ever happens when there is something to pad. */
static void test_pad_implies_a_positive_missing_count(void)
{
    for (int64_t gap = 0; gap <= SWEEP_MAX_MS; gap++) {
        int missing = -1;
        sw_gap_action_t a = sw_gap_action(gap, &missing);
        CHECK(missing >= 0, "the missing count is never negative");
        if (a == SW_GAP_PAD) CHECK(missing > 0, "padding needs a count");
        if (a == SW_GAP_NONE) CHECK(missing == 0, "jitter pads nothing");
    }
}

/* Ordinary jitter is not a lost packet. */
static void test_jitter_is_not_a_gap(void)
{
    int missing = 0;
    CHECK(sw_gap_action(0, &missing) == SW_GAP_NONE, "no gap at all");
    CHECK(missing == 0, "and nothing missing");
    CHECK(sw_gap_action(SW_GAP_JITTER_MS, &missing) == SW_GAP_NONE,
          "a gap at the jitter bound is still jitter");
    CHECK(missing == 0, "and still nothing missing");
}

/* A longer gap never accounts for fewer notifications - the property that
 * makes the count usable for sizing a buffer. */
static void test_missing_count_is_monotonic(void)
{
    int prev = 0;
    for (int64_t gap = 0; gap <= SWEEP_MAX_MS; gap++) {
        int missing = 0;
        (void)sw_gap_action(gap, &missing);
        CHECK(missing >= prev, "the missing count never decreases with the gap");
        prev = missing;
    }
}

/* A negative gap cannot arise from the caller (it normalises midnight
 * rollover first), but a pure function should not produce a negative
 * allocation size if one ever did. */
static void test_a_negative_gap_is_inert(void)
{
    int missing = -1;
    CHECK(sw_gap_action(-5000, &missing) == SW_GAP_NONE,
          "a negative gap does nothing");
    CHECK(missing == 0, "and asks for no padding");
}

/* ANTI-VACUITY.  Every property above is a universal over a sweep, and a
 * function that answered SPLIT to everything would satisfy several of them.
 * Assert the sweep actually visits all three verdicts. */
static void test_the_sweep_exercises_every_action(void)
{
    int saw_none = 0, saw_pad = 0, saw_split = 0;
    for (int64_t gap = 0; gap <= SWEEP_MAX_MS; gap++) {
        switch (sw_gap_action(gap, NULL)) {
        case SW_GAP_NONE:  saw_none = 1;  break;
        case SW_GAP_PAD:   saw_pad = 1;   break;
        case SW_GAP_SPLIT: saw_split = 1; break;
        }
    }
    CHECK(saw_none, "the sweep reaches NONE");
    CHECK(saw_pad, "the sweep reaches PAD");
    CHECK(saw_split, "the sweep reaches SPLIT");
}

int main(void)
{
    test_no_gap_is_both_padded_and_long_enough_to_split();
    test_the_three_actions_are_exhaustive_and_disjoint();
    test_the_boundary_is_the_threshold_exactly();
    test_the_reported_dead_zone_splits();
    test_pad_implies_a_positive_missing_count();
    test_jitter_is_not_a_gap();
    test_missing_count_is_monotonic();
    test_a_negative_gap_is_inert();
    test_the_sweep_exercises_every_action();

    if (g_failures) {
        printf("session_gap tests FAILED (%d)\n", g_failures);
        return 1;
    }
    printf("session_gap tests passed\n");
    return 0;
}
