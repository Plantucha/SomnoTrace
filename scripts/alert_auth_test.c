/*
 * SomnoTrace - host test for ntfy authentication encoding (alert_auth.c)
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

/* The three ntfy credential transports (docs.ntfy.sh/publish/#authentication):
 *   Basic header, Bearer header, and ?auth=<base64 of the header value>.
 * Expected strings below were generated independently (Python base64 +
 * urllib.parse.quote) so the test is not just the implementation twice. */

#include <stdio.h>
#include <string.h>

#include "alert_auth.h"

static int checks, fails;

#define CHECK(cond, msg) do { \
        checks++; \
        if (!(cond)) { fails++; printf("FAILED: %s (line %d)\n", msg, __LINE__); } \
    } while (0)

#define CHECK_STR(got, want, msg) do { \
        checks++; \
        if (strcmp((got), (want)) != 0) { \
            fails++; \
            printf("FAILED: %s (line %d)\n  got:  %s\n  want: %s\n", \
                   msg, __LINE__, (got), (want)); \
        } \
    } while (0)

static void test_none(void)
{
    alert_auth_t a = ALERT_AUTH_DEFAULTS;
    char hdr[160], q[576];

    CHECK(alert_auth_valid(&a), "none is valid");
    CHECK(alert_auth_header(&a, hdr, sizeof(hdr)) == 0, "none: header length 0");
    CHECK_STR(hdr, "", "none: empty header");
    CHECK(alert_auth_query(&a, q, sizeof(q)) == 0, "none: query length 0");
    CHECK_STR(q, "", "none: empty query");
}

static void test_basic(void)
{
    alert_auth_t a = ALERT_AUTH_DEFAULTS;
    char hdr[160], q[576];

    a.mode = ALERT_AUTH_MODE_BASIC;
    strcpy(a.user, "user");
    strcpy(a.secret, "pass");

    CHECK(alert_auth_valid(&a), "basic with user+pass is valid");
    CHECK(alert_auth_header(&a, hdr, sizeof(hdr)) > 0, "basic: header written");
    CHECK_STR(hdr, "Basic dXNlcjpwYXNz", "basic: Authorization header");
    CHECK(alert_auth_query(&a, q, sizeof(q)) > 0, "basic: query written");
    CHECK_STR(q, "auth=QmFzaWMgZFhObGNqcHdZWE56",
              "basic: ?auth= is base64 of the header value");

    /* Base64 characters that a query string would mangle must be escaped. */
    strcpy(a.user, "u");
    strcpy(a.secret, "~~~");
    CHECK(alert_auth_header(&a, hdr, sizeof(hdr)) > 0, "basic(+/): header written");
    CHECK_STR(hdr, "Basic dTp+fn4=", "basic(+/): header carries + and =");
    CHECK(alert_auth_query(&a, q, sizeof(q)) > 0, "basic(+/): query written");
    CHECK_STR(q, "auth=QmFzaWMgZFRwK2ZuND0",
              "basic(+/): padding stripped, '+' percent-encoded");
}

static void test_token(void)
{
    alert_auth_t a = ALERT_AUTH_DEFAULTS;
    char hdr[160], q[576];

    a.mode = ALERT_AUTH_MODE_TOKEN;
    strcpy(a.secret, "tk_AgQdq7mVBoFD37zQVN29RhuMzNIz2");

    CHECK(alert_auth_valid(&a), "token with secret is valid");
    CHECK(alert_auth_header(&a, hdr, sizeof(hdr)) > 0, "token: header written");
    CHECK_STR(hdr, "Bearer tk_AgQdq7mVBoFD37zQVN29RhuMzNIz2",
              "token: Bearer header");
    CHECK(alert_auth_query(&a, q, sizeof(q)) > 0, "token: query written");
    CHECK_STR(q, "auth=QmVhcmVyIHRrX0FnUWRxN21WQm9GRDM3elFWTjI5Umh1TXpOSXoy",
              "token: ?auth= is base64 of the Bearer value");
}

static void test_invalid(void)
{
    alert_auth_t a = ALERT_AUTH_DEFAULTS;
    char hdr[160], q[576];

    a.mode = ALERT_AUTH_MODE_BASIC;            /* user missing */
    strcpy(a.secret, "pass");
    CHECK(!alert_auth_valid(&a), "basic without user is invalid");
    CHECK(alert_auth_header(&a, hdr, sizeof(hdr)) < 0, "basic without user: no header");
    CHECK(alert_auth_query(&a, q, sizeof(q)) < 0, "basic without user: no query");

    a.mode = ALERT_AUTH_MODE_BASIC;            /* secret missing */
    strcpy(a.user, "user");
    a.secret[0] = '\0';
    CHECK(!alert_auth_valid(&a), "basic without secret is invalid");

    a.mode = ALERT_AUTH_MODE_TOKEN;            /* token missing */
    CHECK(!alert_auth_valid(&a), "token without secret is invalid");
    CHECK(alert_auth_header(&a, hdr, sizeof(hdr)) < 0, "token without secret: no header");

    a.mode = 99;                               /* unknown mode */
    strcpy(a.secret, "x");
    CHECK(!alert_auth_valid(&a), "unknown mode is invalid");
    CHECK_STR(alert_auth_mode_name(99), "none", "unknown mode name falls back to none");
}

static void test_modes(void)
{
    CHECK_STR(alert_auth_mode_name(ALERT_AUTH_MODE_NONE), "none", "name none");
    CHECK_STR(alert_auth_mode_name(ALERT_AUTH_MODE_BASIC), "basic", "name basic");
    CHECK_STR(alert_auth_mode_name(ALERT_AUTH_MODE_TOKEN), "token", "name token");

    CHECK(alert_auth_mode_from_name("none") == ALERT_AUTH_MODE_NONE, "parse none");
    CHECK(alert_auth_mode_from_name("basic") == ALERT_AUTH_MODE_BASIC, "parse basic");
    CHECK(alert_auth_mode_from_name("token") == ALERT_AUTH_MODE_TOKEN, "parse token");
    CHECK(alert_auth_mode_from_name("digest") < 0, "parse unknown");
    CHECK(alert_auth_mode_from_name(NULL) < 0, "parse NULL");
}

static void test_buffers(void)
{
    alert_auth_t a = ALERT_AUTH_DEFAULTS;
    char small[8];

    a.mode = ALERT_AUTH_MODE_TOKEN;
    strcpy(a.secret, "tk_AgQdq7mVBoFD37zQVN29RhuMzNIz2");
    CHECK(alert_auth_header(&a, small, sizeof(small)) < 0,
          "short header buffer is rejected, not truncated");
    CHECK(alert_auth_header(&a, NULL, 8) < 0, "NULL header buffer rejected");
    CHECK(alert_auth_query(&a, small, sizeof(small)) < 0,
          "short query buffer is rejected");
    CHECK(alert_auth_header(NULL, small, sizeof(small)) < 0, "NULL config rejected");

    /* Worst case that must still fit the firmware's 160/576-byte buffers. */
    char hdr[160], q[576];
    a.mode = ALERT_AUTH_MODE_BASIC;
    memset(a.user, 'u', sizeof(a.user) - 1);   a.user[sizeof(a.user) - 1] = '\0';
    memset(a.secret, '~', sizeof(a.secret) - 1); a.secret[sizeof(a.secret) - 1] = '\0';
    CHECK(alert_auth_header(&a, hdr, sizeof(hdr)) > 0, "max-size basic header fits");
    CHECK(alert_auth_query(&a, q, sizeof(q)) > 0, "max-size basic query fits");
    CHECK(strlen(q) < 576, "max-size query under the buffer bound");
}

int main(void)
{
    test_none();
    test_basic();
    test_token();
    test_invalid();
    test_modes();
    test_buffers();
    printf("alert_auth_test: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
