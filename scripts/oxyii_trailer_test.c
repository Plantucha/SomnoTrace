/*
 * SomnoTrace - Host unit tests for the Wellue OxyII Format-A trailer decoder
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

#include "oxyii_trailer.h"

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

/* A trailer carrying the finalisation magic, a stated sample count and a
 * stated seconds-per-sample interval. Everything else is left zero. */
static void make_trailer(uint8_t t[OXYII_TRAILER_BYTES], uint32_t count,
                         uint8_t interval)
{
    memset(t, 0, OXYII_TRAILER_BYTES);
    t[OXYII_TRAILER_MAGIC_OFFSET + 0] = 0x48;
    t[OXYII_TRAILER_MAGIC_OFFSET + 1] = 0x12;
    t[OXYII_TRAILER_MAGIC_OFFSET + 2] = 0x5a;
    t[OXYII_TRAILER_MAGIC_OFFSET + 3] = 0xda;
    t[OXYII_TRAILER_COUNT_OFFSET + 0] = (uint8_t)(count & 0xffu);
    t[OXYII_TRAILER_COUNT_OFFSET + 1] = (uint8_t)((count >> 8) & 0xffu);
    t[OXYII_TRAILER_COUNT_OFFSET + 2] = (uint8_t)((count >> 16) & 0xffu);
    t[OXYII_TRAILER_COUNT_OFFSET + 3] = (uint8_t)((count >> 24) & 0xffu);
    t[OXYII_TRAILER_INTERVAL_OFFSET] = interval;
}

/* An unfinalised file is not merely incomplete - its trailer bytes are
 * whatever the flash last held. Refusing on the magic is the only safe test. */
static void test_rejects_without_magic(void)
{
    uint8_t t[OXYII_TRAILER_BYTES];
    oxyii_trailer_t out;
    make_trailer(t, 3600, 1);
    for (int i = 0; i < 4; i++) {
        uint8_t saved = t[OXYII_TRAILER_MAGIC_OFFSET + i];
        t[OXYII_TRAILER_MAGIC_OFFSET + i] = (uint8_t)(saved ^ 0xffu);
        CHECK(!oxyii_trailer_parse(t, sizeof(t), &out),
              "a corrupted magic byte must not parse");
        t[OXYII_TRAILER_MAGIC_OFFSET + i] = saved;
    }
    CHECK(oxyii_trailer_parse(t, sizeof(t), &out),
          "restoring the magic must parse again");
}

static void test_rejects_short_buffer(void)
{
    uint8_t t[OXYII_TRAILER_BYTES];
    oxyii_trailer_t out;
    make_trailer(t, 3600, 1);
    CHECK(!oxyii_trailer_parse(t, OXYII_TRAILER_BYTES - 1, &out),
          "one byte short of a trailer must not parse");
    CHECK(!oxyii_trailer_parse(NULL, sizeof(t), &out), "NULL trailer must not parse");
    CHECK(!oxyii_trailer_parse(t, sizeof(t), NULL), "NULL out must not parse");
    CHECK(oxyii_trailer_parse(t, OXYII_TRAILER_BYTES, &out),
          "exactly a trailer's worth of bytes must parse");
}

/* THE REGRESSION THAT MOTIVATES THE u32 READ. A session longer than 65 535
 * samples truncates silently under a 16-bit read: 70 000 would come back as
 * 4 464, and nothing downstream could tell. */
static void test_sample_count_is_32_bit(void)
{
    uint8_t t[OXYII_TRAILER_BYTES];
    oxyii_trailer_t out;
    const uint32_t big = 70000u;
    make_trailer(t, big, 1);
    CHECK(oxyii_trailer_parse(t, sizeof(t), &out), "wide count must parse");
    CHECK(out.sample_count == big, "count above 65535 must not truncate");
    CHECK(out.sample_count != (big & 0xffffu), "a 16-bit read would have wrapped");

    make_trailer(t, 0x01020304u, 1);
    CHECK(oxyii_trailer_parse(t, sizeof(t), &out), "count must parse");
    CHECK(out.sample_count == 0x01020304u, "count must be little-endian across 4 bytes");
}

/* The cadence is a device setting, not a constant. Every plausible value must
 * scale exactly, because the period lands in both the binary header and the
 * manifest and a wrong one skews every timestamp in the recording. */
static void test_interval_scales_exactly(void)
{
    uint8_t t[OXYII_TRAILER_BYTES];
    oxyii_trailer_t out;
    for (unsigned iv = 1; iv <= OXYII_TRAILER_MAX_INTERVAL_S; iv++) {
        make_trailer(t, 1800, (uint8_t)iv);
        CHECK(oxyii_trailer_parse(t, sizeof(t), &out), "valid interval must parse");
        CHECK(out.interval_valid, "an in-range interval must be accepted");
        CHECK(out.period_us == iv * 1000000u, "period must be interval x 1e6");
        CHECK(out.interval_s == iv, "interval must round-trip");
    }
}

/* Degrade, never fabricate: an absent or absurd interval falls back to the
 * 1 Hz that every observed firmware writes, and SAYS it did so. */
static void test_implausible_interval_degrades_to_1hz(void)
{
    uint8_t t[OXYII_TRAILER_BYTES];
    oxyii_trailer_t out;
    const uint8_t bad[] = { 0, OXYII_TRAILER_MAX_INTERVAL_S + 1, 200, 255 };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        make_trailer(t, 1800, bad[i]);
        CHECK(oxyii_trailer_parse(t, sizeof(t), &out),
              "an implausible interval must still parse the rest");
        CHECK(!out.interval_valid, "an implausible interval must be flagged");
        CHECK(out.period_us == 1000000u, "fallback must be exactly 1 Hz");
    }
    make_trailer(t, 1800, OXYII_TRAILER_MAX_INTERVAL_S);
    CHECK(oxyii_trailer_parse(t, sizeof(t), &out), "boundary must parse");
    CHECK(out.interval_valid, "the maximum interval itself must be accepted");
}

/* Today every device writes 1 s. That is exactly why the constant was easy to
 * hardcode, so pin it: the common case must not regress while the rest is
 * generalised. */
static void test_one_hz_is_unchanged(void)
{
    uint8_t t[OXYII_TRAILER_BYTES];
    oxyii_trailer_t out;
    make_trailer(t, 28800, 1);
    CHECK(oxyii_trailer_parse(t, sizeof(t), &out), "1 Hz must parse");
    CHECK(out.period_us == 1000000u, "1 Hz must stay 1 Hz");
    CHECK(out.sample_count == 28800u, "an eight-hour night must round-trip");
    CHECK(out.interval_valid, "1 s is a valid interval");
}

int main(void)
{
    test_rejects_without_magic();
    test_rejects_short_buffer();
    test_sample_count_is_32_bit();
    test_interval_scales_exactly();
    test_implausible_interval_degrades_to_1hz();
    test_one_hz_is_unchanged();

    if (g_failures) {
        printf("oxyii_trailer tests FAILED (%d)\n", g_failures);
        return 1;
    }
    printf("oxyii_trailer tests passed\n");
    return 0;
}