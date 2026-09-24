/*
 * SomnoTrace - Wellue OxyII frame codec and auth payload
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

#include "oxyii_codec.h"

#include <string.h>

/* MD5("lepucloud") = c2a7cf50dafed885a8f8f7eac44335f3 */
const uint8_t LEPUCLOUD_MD5[16] = {
    0xc2, 0xa7, 0xcf, 0x50, 0xda, 0xfe, 0xd8, 0x85,
    0xa8, 0xf8, 0xf7, 0xea, 0xc4, 0x43, 0x35, 0xf3,
};

/* ── CRC8 (poly=0x07, init=0) ───────────────────────────────────────── */
uint8_t oxyii_crc8(const uint8_t *data, int len)
{
    uint8_t crc = 0;
    for (int i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 0x80) crc = (crc << 1) ^ 0x07;
            else            crc <<= 1;
        }
    }
    return crc;
}

/* ── Frame codec ────────────────────────────────────────────────────── */
int oxyii_encode(uint8_t *buf, int bufsz, uint8_t op, uint8_t flag, uint8_t seq,
                 const uint8_t *payload, int payload_len)
{
    int total = OXYII_HEADER_LEN + payload_len + 1;
    if (total > bufsz) return -1;

    buf[0] = OXYII_LEAD;
    buf[1] = op;
    buf[2] = ~op;
    buf[3] = flag;
    buf[4] = seq;
    buf[5] = payload_len & 0xFF;
    buf[6] = (payload_len >> 8) & 0xFF;
    if (payload && payload_len > 0)
        memcpy(buf + 7, payload, payload_len);
    buf[total - 1] = oxyii_crc8(buf, total - 1);
    return total;
}

int oxyii_try_decode(const uint8_t *buf, int len, uint8_t *op, uint8_t *flag,
                     uint8_t *seq, uint8_t *payload, int *payload_len,
                     int payload_cap)
{
    if (len < OXYII_HEADER_LEN) return -1;
    if (buf[0] != OXYII_LEAD) return -2;
    if ((uint8_t)(~buf[1]) != buf[2]) return -2;

    int plen = buf[5] | (buf[6] << 8);
    int total = OXYII_HEADER_LEN + plen + 1;
    if (len < total) return -1;

    if (oxyii_crc8(buf, total - 1) != buf[total - 1]) return -2;

    if (op)   *op = buf[1];
    if (flag) *flag = buf[3];
    if (seq)  *seq = buf[4];
    if (payload && payload_cap > 0) {
        int n = plen < payload_cap ? plen : payload_cap;
        memcpy(payload, buf + 7, n);
    }
    if (payload_len) *payload_len = plen;
    return total;
}

/* ── Auth payload ───────────────────────────────────────────────────── */
void oxyii_auth_payload_for(const char *serial, uint32_t ts, uint8_t out16[16])
{
    uint8_t key[16];
    /* key[0..7] = LEPUCLOUD_MD5 even-indexed bytes */
    for (int i = 0; i < 8; i++)
        key[i] = LEPUCLOUD_MD5[i * 2];
    /* key[8..11] = first 4 chars of device SN if known, else "0000" */
    if (serial && serial[0] >= '0' && serial[0] <= '9' && strlen(serial) >= 4) {
        memcpy(key + 8, serial, 4);
    } else {
        memcpy(key + 8, "0000", 4);
    }
    /* key[12..15] = little-endian uint32 Unix timestamp.  BYTE shifts, not bit
     * shifts — `ts >> i` was #177 and made the ring reject every key. */
    for (int i = 0; i < 4; i++)
        key[12 + i] = (uint8_t)((ts >> (i * 8)) & 0xFF);
    /* auth = key XOR LEPUCLOUD_MD5 */
    for (int i = 0; i < 16; i++)
        out16[i] = key[i] ^ LEPUCLOUD_MD5[i];
}
