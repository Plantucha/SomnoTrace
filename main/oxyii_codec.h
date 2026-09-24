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

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* WHY THIS IS ITS OWN UNIT: everything here is pure — bytes in, bytes out, no
 * BLE, no clock, no globals — but it lived as statics inside
 * oximeter_oxyii.c, where no host test could reach it.  #177 was a bug in
 * exactly this code (the auth timestamp packed by bit index instead of byte),
 * fixed in 14bea1b with two real captured frames posted as known-answer
 * vectors, and nothing held the fix.  scripts/oxyii_codec_test.c now links
 * this unit and asserts those frames byte for byte. */

#define OXYII_LEAD         0xA5
#define OXYII_HEADER_LEN   7     /* lead, op, ~op, flag, seq, len_lo, len_hi */

/* MD5("lepucloud") = c2a7cf50dafed885a8f8f7eac44335f3.  Shared with the
 * driver, which also XORs the ring's AUTH reply with it. */
extern const uint8_t LEPUCLOUD_MD5[16];

/* CRC-8, poly 0x07, init 0, no reflection (CRC-8/SMBUS). */
uint8_t oxyii_crc8(const uint8_t *data, int len);

/* Encode an OxyII frame into buf.  Returns the total frame length, or -1 if
 * it does not fit in bufsz. */
int oxyii_encode(uint8_t *buf, int bufsz, uint8_t op, uint8_t flag, uint8_t seq,
                 const uint8_t *payload, int payload_len);

/* Try to decode a frame from buf.  Returns the total frame length on success,
 * -1 if incomplete (need more data), -2 if invalid (bad lead, op check or
 * CRC).  *payload_len receives the frame's own length even when payload_cap
 * truncated the copy. */
int oxyii_try_decode(const uint8_t *buf, int len, uint8_t *op, uint8_t *flag,
                     uint8_t *seq, uint8_t *payload, int *payload_len,
                     int payload_cap);

/* The 16-byte OP_AUTH payload for a device serial at Unix time ts:
 *     key = LEPUCLOUD_MD5[0,2,..,14] | serial[0..3] or "0000" | ts as LE u32
 *     out = key XOR LEPUCLOUD_MD5
 * The serial is used only if it starts with a digit and has at least four
 * characters; otherwise "0000" (a NULL serial is treated as unknown). */
void oxyii_auth_payload_for(const char *serial, uint32_t ts, uint8_t out16[16]);

#ifdef __cplusplus
}
#endif
