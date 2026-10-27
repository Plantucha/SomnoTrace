/*
 * SomnoTrace - ntfy server authentication: header/query encoding
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

#include "alert_auth.h"

static const char B64_ALPHABET[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* Standard base64 with padding (what ntfy expects).  Returns the number of
 * characters written, or -1 when out_len is too small (including the NUL). */
static int b64_encode(const char *in, size_t in_len, char *out, size_t out_len)
{
    const size_t need = ((in_len + 2) / 3) * 4;
    if (out_len < need + 1) return -1;

    size_t o = 0;
    for (size_t i = 0; i < in_len; i += 3) {
        const uint32_t b0 = (uint8_t)in[i];
        const uint32_t b1 = (i + 1 < in_len) ? (uint8_t)in[i + 1] : 0;
        const uint32_t b2 = (i + 2 < in_len) ? (uint8_t)in[i + 2] : 0;
        const uint32_t v = (b0 << 16) | (b1 << 8) | b2;
        out[o++] = B64_ALPHABET[(v >> 18) & 0x3F];
        out[o++] = B64_ALPHABET[(v >> 12) & 0x3F];
        out[o++] = (i + 1 < in_len) ? B64_ALPHABET[(v >> 6) & 0x3F] : '=';
        out[o++] = (i + 2 < in_len) ? B64_ALPHABET[v & 0x3F] : '=';
    }
    out[o] = '\0';
    return (int)o;
}

bool alert_auth_valid(const alert_auth_t *a)
{
    if (!a) return false;
    switch (a->mode) {
        case ALERT_AUTH_MODE_NONE:
            return true;
        case ALERT_AUTH_MODE_BASIC:
            return a->user[0] != '\0' && a->secret[0] != '\0';
        case ALERT_AUTH_MODE_TOKEN:
            return a->secret[0] != '\0';
        default:
            return false;
    }
}

const char *alert_auth_mode_name(uint8_t mode)
{
    switch (mode) {
        case ALERT_AUTH_MODE_BASIC: return "basic";
        case ALERT_AUTH_MODE_TOKEN: return "token";
        default:                    return "none";
    }
}

int alert_auth_mode_from_name(const char *name)
{
    if (!name) return -1;
    if (!strcmp(name, "none"))  return ALERT_AUTH_MODE_NONE;
    if (!strcmp(name, "basic")) return ALERT_AUTH_MODE_BASIC;
    if (!strcmp(name, "token")) return ALERT_AUTH_MODE_TOKEN;
    return -1;
}

int alert_auth_header(const alert_auth_t *a, char *out, size_t out_len)
{
    if (!a || !out || out_len == 0) return -1;
    out[0] = '\0';
    if (a->mode == ALERT_AUTH_MODE_NONE) return 0;
    if (!alert_auth_valid(a)) return -1;

    if (a->mode == ALERT_AUTH_MODE_TOKEN) {
        int n = snprintf(out, out_len, "Bearer %s", a->secret);
        return (n < 0 || (size_t)n >= out_len) ? -1 : n;
    }

    char creds[ALERT_AUTH_USER_MAX + 1 + ALERT_AUTH_SECRET_MAX + 1];
    int n = snprintf(creds, sizeof(creds), "%s:%s", a->user, a->secret);
    if (n < 0 || (size_t)n >= sizeof(creds)) return -1;

    char b64[((sizeof(creds) + 2) / 3) * 4 + 1];
    int bl = b64_encode(creds, (size_t)n, b64, sizeof(b64));
    if (bl < 0) return -1;

    n = snprintf(out, out_len, "Basic %s", b64);
    return (n < 0 || (size_t)n >= out_len) ? -1 : n;
}

int alert_auth_query(const alert_auth_t *a, char *out, size_t out_len)
{
    if (!a || !out || out_len == 0) return -1;
    out[0] = '\0';
    if (a->mode == ALERT_AUTH_MODE_NONE) return 0;
    if (!alert_auth_valid(a)) return -1;

    /* ntfy's ?auth= takes the base64 of the Authorization header value,
     * with two quirks measured against ntfy 2.34:
     *   - the value is decoded as UNPADDED base64 (Go's RawStdEncoding), so
     *     a trailing '=' makes the server answer HTTP 500;
     *   - the query is percent-decoded, so '+' must be sent as %2B or it
     *     arrives as a space and the credentials fail.
     * "Basic " + base64(user:secret) is at most 6 + 132 chars. */
    char hdr[160];
    if (alert_auth_header(a, hdr, sizeof(hdr)) <= 0) return -1;

    char b64[((sizeof(hdr) + 2) / 3) * 4 + 1];
    int bl = b64_encode(hdr, strlen(hdr), b64, sizeof(b64));
    if (bl < 0) return -1;
    while (bl > 0 && b64[bl - 1] == '=') b64[--bl] = '\0';

    /* Percent-encode everything outside the RFC 3986 unreserved set, so the
     * base64 '+', '/' and '=' survive query decoding intact. */
    size_t o = 0;
    const char *pre = "auth=";
    for (const char *p = pre; *p; p++) {
        if (o + 2 > out_len) return -1;
        out[o++] = *p;
    }
    for (const char *p = b64; *p; p++) {
        const char c = *p;
        const bool plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                           (c >= '0' && c <= '9') ||
                           c == '-' || c == '_' || c == '.' || c == '~';
        if (plain) {
            if (o + 2 > out_len) return -1;
            out[o++] = c;
        } else {
            if (o + 4 > out_len) return -1;
            static const char HEX[] = "0123456789ABCDEF";
            out[o++] = '%';
            out[o++] = HEX[((uint8_t)c >> 4) & 0xF];
            out[o++] = HEX[(uint8_t)c & 0xF];
        }
    }
    out[o] = '\0';
    return (int)o;
}
