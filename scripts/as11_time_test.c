/*
 * SomnoTrace - Host unit tests for AS11 timezone and noon-day mapping
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

#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "as11_time.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int fails = 0;

static void set_tz(const char *tz)
{
    setenv("TZ", tz, 1);
    tzset();
}

static void expect_str(const char *what, const char *got, const char *want)
{
    bool ok = strcmp(got, want) == 0;
    printf("  %-54s got=%-10s want=%-10s %s\n", what, got, want, ok ? "PASS" : "FAIL");
    if (!ok) fails++;
}

static void expect_int(const char *what, long long got, long long want)
{
    bool ok = (got == want);
    printf("  %-54s got=%-10lld want=%-10lld %s\n", what, got, want, ok ? "PASS" : "FAIL");
    if (!ok) fails++;
}

/* Reset the cached offset between scenarios by forcing a fresh derivation. */
static void reset_offset(void)
{
    as11_time_set_offset(0, "reset");
}

int main(void)
{
    printf("\n=== Scenario 1: Issue #75 Vector (AS11 -05:00 vs ESP MST7MDT) ===\n");
    set_tz("MST7MDT,M3.2.0,M11.1.0");
    reset_offset();

    const int64_t PS_AUG7 = 1786122000000LL;
    as11_offset_t off = 0;
    bool derived = as11_time_offset_from_period_start(PS_AUG7, &off);
    expect_int("offset derived from PeriodStart (s)", derived ? off : -999999, -18000);

    char day[16];
    as11_time_noon_day_for_period_start(PS_AUG7, day, sizeof(day));
    expect_str("day label for the Aug 7 summary record", day, "20260807");
    as11_time_noon_day_for_period_start(PS_AUG7 - 86400000LL, day, sizeof(day));
    expect_str("previous day's record", day, "20260806");
    as11_time_noon_day_for_period_start(PS_AUG7 + 86400000LL, day, sizeof(day));
    expect_str("next day's record", day, "20260808");

    expect_int("day number for 20260807 (STR Date)",
               as11_time_day_number("20260807"), 20672);
    int64_t noon = as11_time_local_noon_epoch("20260807");
    expect_int("ESP-local noon epoch for 20260807", noon, 1786125600LL);

    printf("\n=== Scenario 2: Control (AS11 and ESP both +10:00 AEST) ===\n");
    set_tz("AEST-10AEDT,M10.1.0,M4.1.0/3");
    reset_offset();
    const int64_t PS_MEL = 1782698400000LL;
    derived = as11_time_offset_from_period_start(PS_MEL, &off);
    expect_int("offset derived (s)", derived ? off : -999999, 36000);
    as11_time_noon_day_for_period_start(PS_MEL, day, sizeof(day));
    expect_str("day label", day, "20260629");

    printf("\n=== Scenario 3: +13:00 NZDT / Pacific Disambiguation ===\n");
    set_tz("NZST-12NZDT,M9.5.0,M4.1.0/3");
    reset_offset();
    struct tm t = {0};
    t.tm_year = 126; t.tm_mon = 0; t.tm_mday = 14; t.tm_hour = 23;
    int64_t ps_nz = (int64_t)timegm(&t) * 1000;
    derived = as11_time_offset_from_period_start(ps_nz, &off);
    expect_int("offset derived (s)", derived ? off : -999999, 46800);
    as11_time_noon_day_for_period_start(ps_nz, day, sizeof(day));
    expect_str("day label", day, "20260115");

    printf("\n=== Scenario 4: Settings Authoritative Preservation (+13:00) ===\n");
    reset_offset();
    set_tz("UTC0");
    cJSON node_off = { .child = NULL, .next = NULL, .string = "TimeZoneOffset", .valuestring = "+13:00", .type = cJSON_String };
    cJSON node_tz = { .child = &node_off, .next = NULL, .string = "TimeZoneFeature", .valuestring = NULL, .type = 0 };
    cJSON node_fp = { .child = &node_tz, .next = NULL, .string = "FeatureProfiles", .valuestring = NULL, .type = 0 };
    cJSON node_sp = { .child = &node_fp, .next = NULL, .string = "SettingProfiles", .valuestring = NULL, .type = 0 };
    cJSON node_fg = { .child = &node_sp, .next = NULL, .string = "FlowGenerator", .valuestring = NULL, .type = 0 };
    bool s_ok = as11_time_offset_from_settings(&node_fg, &off);
    expect_int("settings offset parsed", s_ok ? off : -999999, 46800);
    derived = as11_time_offset_from_period_start(ps_nz, &off);
    expect_int("period_start respects settings offset anchor", derived ? off : -999999, 46800);

    printf("\n=== Scenario 5: 23-Hour Spring-Forward Transition Fallback ===\n");
    set_tz("EST5EDT,M3.2.0,M11.1.0");
    reset_offset();
    as11_time_noon_day(1772992800LL * 1000LL, day, sizeof(day));
    expect_str("Mar 8 14:00 EDT (afternoon session)", day, "20260308");
    as11_time_noon_day(1773030600LL * 1000LL, day, sizeof(day));
    expect_str("Mar 9 00:30 EDT (post-midnight on 23h transition night)", day, "20260308");
    as11_time_noon_day(1773073800LL * 1000LL, day, sizeof(day));
    expect_str("Mar 9 12:30 EDT (next noon-day)", day, "20260309");

    printf("\n=== Scenario 6: Non-Noon PeriodStart Rejection ===\n");
    reset_offset();
    derived = as11_time_offset_from_period_start(PS_AUG7 + 7 * 60 * 1000, &off);
    expect_int("derivation refused (7 min past noon)", derived ? 1 : 0, 0);

    printf("\n=== Scenario 7: Session Timestamps -> AS11 Noon-Day ===\n");
    reset_offset();
    as11_time_set_offset(-18000, "test");
    as11_time_noon_day(1786138713027LL, day, sizeof(day));
    expect_str("evening session belongs to its own day", day, "20260807");
    as11_time_noon_day(1786197600000LL, day, sizeof(day));
    expect_str("post-midnight session belongs to previous day", day, "20260807");

    printf("\n%s (%d failure%s)\n", fails ? "FAILURES" : "ALL PASS",
           fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
