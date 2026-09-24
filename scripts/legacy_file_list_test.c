/*
 * SomnoTrace - Host unit tests for the Gen1 CMD_INFO FileList parser
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
#include <string.h>

#include "legacy_file_list.h"

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

#define CAP 32
static char g_names[CAP][LEGACY_FILE_NAME_MAX];

/* The real list from a PO6 ring (issue #288), in the order the device sent it —
 * which is NOT chronological.  Order is preserved because the caller pulls by
 * name and nothing downstream may assume the ring sorts. */
static void test_real_ring_list_round_trips(void)
{
    legacy_file_list_stats_t st;
    int n = legacy_parse_file_list(
        "20260923031114,20260923041114,20260923012908,20260923021113",
        false, g_names, CAP, &st);

    CHECK(n == 4, "the ring's four recordings all parse");
    CHECK(strcmp(g_names[0], "20260923031114") == 0, "first name preserved");
    CHECK(strcmp(g_names[3], "20260923021113") == 0, "device order preserved");
    CHECK(st.skipped == 0 && !st.overflow && !st.source_truncated,
          "a complete list reports no shortfall");
    CHECK(legacy_file_list_complete(&st), "complete() agrees");
}

static void test_empty_and_null(void)
{
    legacy_file_list_stats_t st;
    CHECK(legacy_parse_file_list(NULL, false, g_names, CAP, &st) == 0,
          "NULL list yields nothing");
    CHECK(legacy_parse_file_list("", false, g_names, CAP, &st) == 0,
          "empty list yields nothing");
    CHECK(legacy_parse_file_list("a,b", false, NULL, CAP, &st) == 0,
          "no output array yields nothing");
    CHECK(legacy_parse_file_list("a,b", false, g_names, 0, &st) == 0,
          "zero capacity yields nothing");
    CHECK(legacy_parse_file_list("a,b", false, g_names, CAP, NULL) == 2,
          "stats are optional");
}

/* A trailing comma is how several firmwares terminate the list; it is not a
 * missing recording and must not be reported as one. */
static void test_trailing_comma_is_not_a_loss(void)
{
    legacy_file_list_stats_t st;
    int n = legacy_parse_file_list("20260923031114,20260923041114,", false,
                                   g_names, CAP, &st);
    CHECK(n == 2, "two names either side of the terminator");
    CHECK(st.skipped == 0, "the terminator is not counted as a skip");
    CHECK(legacy_file_list_complete(&st), "and the list still reads complete");
}

/* A name too long for a slot used to be dropped silently.  It is still dropped
 * — there is nowhere to put it — but it is now counted, so the caller can say
 * that the ring holds something this sync cannot fetch. */
static void test_over_long_name_is_counted_not_swallowed(void)
{
    legacy_file_list_stats_t st;
    int n = legacy_parse_file_list(
        "20260923031114,"
        "thisnameisfarlongerthanthirtytwocharacters,"
        "20260923041114",
        false, g_names, CAP, &st);

    CHECK(n == 2, "the two usable names are returned");
    CHECK(st.skipped == 1, "the unusable one is counted");
    CHECK(!legacy_file_list_complete(&st), "so the list does not read complete");
    CHECK(strcmp(g_names[1], "20260923041114") == 0,
          "and parsing continues past it");
}

/* The ring accumulates files until its own retention prunes them, so a list
 * longer than the caller's array is reachable rather than theoretical. */
static void test_more_names_than_capacity_reports_overflow(void)
{
    char list[1024];
    list[0] = '\0';
    for (int i = 0; i < 6; i++) {
        char one[40];
        snprintf(one, sizeof(one), "%s2026092301111%d", i ? "," : "", i);
        strncat(list, one, sizeof(list) - strlen(list) - 1);
    }

    legacy_file_list_stats_t st;
    int n = legacy_parse_file_list(list, false, g_names, 4, &st);

    CHECK(n == 4, "the array is filled to capacity");
    CHECK(st.overflow, "and the surplus is reported, not discarded in silence");
    CHECK(!legacy_file_list_complete(&st), "so the list does not read complete");
}

/* THE BUG THIS MODULE EXISTS FOR.  A response longer than the caller's buffer
 * is cut mid-name; the fragment left behind still satisfies every syntactic
 * test, so it would be pulled, fail, and abort the sync attempt. */
static void test_truncated_response_drops_the_fragment(void)
{
    legacy_file_list_stats_t st;
    int n = legacy_parse_file_list(
        "20260923031114,20260923041114,202609230511", /* cut mid-name */
        true, g_names, CAP, &st);

    CHECK(n == 2, "only the names that were fully received are returned");
    CHECK(st.skipped == 1, "the fragment is counted");
    CHECK(st.source_truncated, "and the truncation is reported");
    CHECK(!legacy_file_list_complete(&st), "so the list does not read complete");
    for (int i = 0; i < n; i++)
        CHECK(strcmp(g_names[i], "202609230511") != 0,
              "the fragment never reaches the caller");
}

/* ANTI-VACUITY for the test above.  If the fragment were dropped by some
 * property of the text — a length rule, a digit count — then a truncation
 * landing exactly on a comma would lose a perfectly good name, and an
 * untruncated list would lose its last one.  Both must hold. */
static void test_the_fragment_rule_keys_on_the_flag_only(void)
{
    legacy_file_list_stats_t st;

    /* Same text, NOT truncated: the final element is real and must survive. */
    int n = legacy_parse_file_list(
        "20260923031114,20260923041114,202609230511", false, g_names, CAP, &st);
    CHECK(n == 3, "an untruncated list keeps its final element");
    CHECK(strcmp(g_names[2], "202609230511") == 0,
          "even when that element is short");
    CHECK(legacy_file_list_complete(&st), "and reads complete");

    /* A truncation that happened to land on a separator still costs one name —
     * the parser cannot know the cut was clean, and guessing is what this
     * module refuses to do. */
    n = legacy_parse_file_list("20260923031114,20260923041114,", true,
                               g_names, CAP, &st);
    CHECK(n == 2, "a clean-looking truncation is still treated as lossy");
    CHECK(!legacy_file_list_complete(&st), "and is reported as such");
}

/* Names are NUL-terminated inside their slot and never run into the next one. */
static void test_names_are_bounded(void)
{
    legacy_file_list_stats_t st;
    memset(g_names, 'X', sizeof(g_names));
    int n = legacy_parse_file_list("20260923031114,20260923041114", false,
                                   g_names, CAP, &st);
    CHECK(n == 2, "two names");
    CHECK(strlen(g_names[0]) == 14, "first name is exactly its own length");
    CHECK(strlen(g_names[1]) == 14, "second likewise");
}

int main(void)
{
    test_real_ring_list_round_trips();
    test_empty_and_null();
    test_trailing_comma_is_not_a_loss();
    test_over_long_name_is_counted_not_swallowed();
    test_more_names_than_capacity_reports_overflow();
    test_truncated_response_drops_the_fragment();
    test_the_fragment_rule_keys_on_the_flag_only();
    test_names_are_bounded();

    if (g_failures) {
        printf("legacy_file_list tests FAILED (%d)\n", g_failures);
        return 1;
    }
    printf("legacy_file_list tests passed\n");
    return 0;
}
