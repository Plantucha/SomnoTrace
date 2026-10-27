/*
 * SomnoTrace - ntfy server authentication: config model + header/query encoding
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

/* ntfy accepts three credential transports (docs.ntfy.sh/publish/#authentication):
 *   1. username + password  →  Authorization: Basic base64("user:pass")
 *   2. access token         →  Authorization: Bearer tk_…
 *   3. either of the above in the URL: ?auth=<base64 of the header value>
 * The query form exists for proxies that strip the Authorization header. */

#define ALERT_AUTH_VERSION     1

#define ALERT_AUTH_MODE_NONE   0
#define ALERT_AUTH_MODE_BASIC  1
#define ALERT_AUTH_MODE_TOKEN  2

#define ALERT_AUTH_USER_MAX    32
#define ALERT_AUTH_SECRET_MAX  64   /* password or tk_… token */

/* Persisted as its own NVS blob in the "alert" namespace.  Kept out of
 * therapy_alert_config_t on purpose: that blob is read with a fixed size, so
 * growing it would leave new fields uninitialised on upgrade and make a
 * downgrade fail the read (full reset to defaults). */
typedef struct {
    uint8_t version;                            /* ALERT_AUTH_VERSION     */
    uint8_t mode;                               /* ALERT_AUTH_MODE_*      */
    bool    via_query;                          /* ?auth= instead of hdr  */
    char    user[ALERT_AUTH_USER_MAX];
    char    secret[ALERT_AUTH_SECRET_MAX];
} alert_auth_t;

#define ALERT_AUTH_DEFAULTS { .version = ALERT_AUTH_VERSION, \
                              .mode = ALERT_AUTH_MODE_NONE }

/* True when the fields the mode needs are present. */
bool alert_auth_valid(const alert_auth_t *a);

/* "none" / "basic" / "token" (unknown → "none"). */
const char *alert_auth_mode_name(uint8_t mode);

/* Parse a mode name; -1 when unknown. */
int alert_auth_mode_from_name(const char *name);

/* Authorization header value ("Basic …" / "Bearer …").
 * Returns the length written (0 for mode none, output ""), -1 on bad args,
 * invalid config, or an output buffer that is too small. */
int alert_auth_header(const alert_auth_t *a, char *out, size_t out_len);

/* Query fragment "auth=<url-encoded base64 of the header value>", without a
 * leading '?'.  Returns the length written (0 for mode none, output ""),
 * -1 on bad args, invalid config, or a buffer that is too small. */
int alert_auth_query(const alert_auth_t *a, char *out, size_t out_len);
