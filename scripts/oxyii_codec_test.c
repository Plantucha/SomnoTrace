/*
 * SomnoTrace - Host unit tests for the Wellue OxyII frame codec and auth payload
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

#include "oxyii_codec.h"

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

#define OP_AUTH 0xFF   /* as oximeter_oxyii.c */

/* ── The two captured OP_AUTH frames posted as known-answer vectors on #177 ──
 * Each capture begins with a 1-byte length prefix (0x18 = the 24 bytes that
 * follow), which is not part of the OxyII frame and is dropped here.  Both are
 * serial "0000".  Frame B uses this driver's lead byte (0xA5), so the encoder
 * can be checked against it end to end; frame A was captured with lead 0xAA,
 * so for it the payload and the CRC are checked but the encoder is not. */
static const uint8_t FRAME_A[24] = {             /* ts 1788096060, lead 0xAA */
    0xaa, 0xff, 0x00, 0x00, 0x00, 0x10, 0x00,
    0x00, 0x68, 0x15, 0x88, 0x72, 0x09, 0x1c, 0xb0,
    0x98, 0xc8, 0xc7, 0xda, 0xf8, 0x6d, 0xa1, 0x99,
    0xb4,
};
static const uint8_t FRAME_B[24] = {             /* ts 1788095920, lead 0xA5 */
    0xa5, 0xff, 0x00, 0x00, 0x00, 0x10, 0x00,
    0x00, 0x68, 0x15, 0x88, 0x72, 0x09, 0x1c, 0xb0,
    0x98, 0xc8, 0xc7, 0xda, 0x74, 0x6e, 0xa1, 0x99,
    0x25,
};
#define TS_A 1788096060u
#define TS_B 1788095920u
#define PAYLOAD_OF(frame) ((frame) + OXYII_HEADER_LEN)

/* An independent anchor: the published CRC-8/SMBUS check value.  If the CRC
 * only ever agreed with frames this suite built itself, a wrong polynomial
 * applied consistently on both sides would pass unnoticed. */
static void test_crc8_matches_the_published_check_value(void)
{
    const char *check = "123456789";
    CHECK(oxyii_crc8((const uint8_t *)check, 9) == 0xF4,
          "CRC-8/SMBUS(\"123456789\") must be 0xF4");
    CHECK(oxyii_crc8((const uint8_t *)"", 0) == 0x00, "empty input leaves init");
}

/* Both captured frames carry a CRC the ring accepted, over lead..payload. */
static void test_crc8_verifies_both_captured_frames(void)
{
    CHECK(oxyii_crc8(FRAME_A, 23) == FRAME_A[23], "frame A's CRC");
    CHECK(oxyii_crc8(FRAME_B, 23) == FRAME_B[23], "frame B's CRC");
}

/* THE REGRESSION FOR #177.  The payload is derived from serial and time and
 * nothing else, so a captured frame pins it exactly. */
static void test_auth_payload_matches_capture_a(void)
{
    uint8_t out[16];
    oxyii_auth_payload_for("0000", TS_A, out);
    CHECK(memcmp(out, PAYLOAD_OF(FRAME_A), 16) == 0,
          "auth payload for (\"0000\", 1788096060) differs from the capture");
}

static void test_auth_payload_matches_capture_b(void)
{
    uint8_t out[16];
    oxyii_auth_payload_for("0000", TS_B, out);
    CHECK(memcmp(out, PAYLOAD_OF(FRAME_B), 16) == 0,
          "auth payload for (\"0000\", 1788095920) differs from the capture");
}

/* End to end: derive the payload, frame it, and get the captured bytes back. */
static void test_full_auth_frame_matches_capture_b(void)
{
    uint8_t payload[16], frame[64];
    oxyii_auth_payload_for("0000", TS_B, payload);
    int n = oxyii_encode(frame, sizeof(frame), OP_AUTH, 0, 0, payload, 16);
    CHECK(n == 24, "an OP_AUTH frame is 7 + 16 + 1 bytes");
    CHECK(n == 24 && memcmp(frame, FRAME_B, 24) == 0,
          "encoded OP_AUTH frame differs from the capture");
}

/* ANTI-VACUITY.  The timestamp must actually reach the output: the two
 * captures are 140 s apart, so they differ in the low timestamp bytes and
 * nowhere else.  A derivation that ignored ts, or used only its top half,
 * could not produce two different payloads here. */
static void test_the_timestamp_reaches_the_payload(void)
{
    uint8_t a[16], b[16];
    oxyii_auth_payload_for("0000", TS_A, a);
    oxyii_auth_payload_for("0000", TS_B, b);
    CHECK(memcmp(a, b, 12) == 0, "key bytes 0..11 do not depend on ts");
    CHECK(a[12] != b[12] && a[13] != b[13], "the low timestamp bytes differ");
    CHECK(a[14] == b[14] && a[15] == b[15], "the high timestamp bytes agree");
}

/* The serial rule: used only when it starts with a digit and has >= 4 chars.
 * Checked through its effect on key[8..11], XORed back out of the payload. */
static void serial_bytes(const char *serial, uint8_t out4[4])
{
    uint8_t p[16];
    oxyii_auth_payload_for(serial, TS_A, p);
    for (int i = 0; i < 4; i++) out4[i] = p[8 + i] ^ LEPUCLOUD_MD5[8 + i];
}

static void test_serial_selection(void)
{
    uint8_t k[4];
    serial_bytes("2407500563", k);
    CHECK(memcmp(k, "2407", 4) == 0, "a numeric serial contributes its first 4");
    serial_bytes("S8AW2100", k);
    CHECK(memcmp(k, "0000", 4) == 0, "a serial starting with a letter -> 0000");
    serial_bytes("123", k);
    CHECK(memcmp(k, "0000", 4) == 0, "a serial shorter than 4 -> 0000");
    serial_bytes("", k);
    CHECK(memcmp(k, "0000", 4) == 0, "an empty serial -> 0000");
    serial_bytes(NULL, k);
    CHECK(memcmp(k, "0000", 4) == 0, "an unknown (NULL) serial -> 0000");
}

static void test_encode_decode_round_trip(void)
{
    const int lens[] = { 0, 1, 16, 17, 255, 256, 300 };
    for (unsigned t = 0; t < sizeof(lens) / sizeof(lens[0]); t++) {
        uint8_t payload[300], frame[400], back[300];
        for (int i = 0; i < lens[t]; i++) payload[i] = (uint8_t)(i * 37 + t);
        int n = oxyii_encode(frame, sizeof(frame), 0xF3, 0x01, (uint8_t)t,
                             payload, lens[t]);
        CHECK(n == OXYII_HEADER_LEN + lens[t] + 1, "encoded length");
        uint8_t op = 0, flag = 0, seq = 0;
        int plen = -1;
        int r = oxyii_try_decode(frame, n, &op, &flag, &seq, back, &plen,
                                 (int)sizeof(back));
        CHECK(r == n, "decode consumes exactly the encoded frame");
        CHECK(op == 0xF3 && flag == 0x01 && seq == (uint8_t)t, "header fields");
        CHECK(plen == lens[t], "payload length");
        CHECK(lens[t] == 0 || memcmp(back, payload, lens[t]) == 0, "payload bytes");
    }
}

/* Each way a frame can be wrong has its own verdict, and a corrupted frame is
 * never reported as good. */
static void test_decode_rejects_bad_frames(void)
{
    uint8_t f[24];

    memcpy(f, FRAME_B, 24);
    CHECK(oxyii_try_decode(f, 24, NULL, NULL, NULL, NULL, NULL, 0) == 24,
          "the untouched capture decodes");

    memcpy(f, FRAME_B, 24); f[0] = 0x5A;
    CHECK(oxyii_try_decode(f, 24, NULL, NULL, NULL, NULL, NULL, 0) == -2,
          "a wrong lead byte is invalid");

    memcpy(f, FRAME_B, 24); f[2] ^= 0x01;
    CHECK(oxyii_try_decode(f, 24, NULL, NULL, NULL, NULL, NULL, 0) == -2,
          "an op that fails its complement check is invalid");

    memcpy(f, FRAME_B, 24); f[10] ^= 0x01;
    CHECK(oxyii_try_decode(f, 24, NULL, NULL, NULL, NULL, NULL, 0) == -2,
          "a flipped payload bit fails the CRC");

    memcpy(f, FRAME_B, 24);
    CHECK(oxyii_try_decode(f, 23, NULL, NULL, NULL, NULL, NULL, 0) == -1,
          "a frame one byte short is incomplete, not invalid");
    CHECK(oxyii_try_decode(f, 6, NULL, NULL, NULL, NULL, NULL, 0) == -1,
          "a partial header is incomplete");
}

/* A caller with a small buffer still learns the frame's true payload length,
 * and gets exactly as much as fits. */
static void test_decode_reports_true_length_when_capped(void)
{
    uint8_t out[4] = { 0 };
    int plen = -1;
    int r = oxyii_try_decode(FRAME_B, 24, NULL, NULL, NULL, out, &plen, 4);
    CHECK(r == 24, "a capped copy still decodes the whole frame");
    CHECK(plen == 16, "payload_len reports the frame's length, not the cap");
    CHECK(memcmp(out, PAYLOAD_OF(FRAME_B), 4) == 0, "the first 4 bytes are copied");
}

static void test_encode_refuses_a_buffer_too_small(void)
{
    uint8_t payload[16] = { 0 }, frame[23];
    CHECK(oxyii_encode(frame, (int)sizeof(frame), OP_AUTH, 0, 0, payload, 16) == -1,
          "a 24-byte frame does not fit in 23");
}

int main(void)
{
    test_crc8_matches_the_published_check_value();
    test_crc8_verifies_both_captured_frames();
    test_auth_payload_matches_capture_a();
    test_auth_payload_matches_capture_b();
    test_full_auth_frame_matches_capture_b();
    test_the_timestamp_reaches_the_payload();
    test_serial_selection();
    test_encode_decode_round_trip();
    test_decode_rejects_bad_frames();
    test_decode_reports_true_length_when_capped();
    test_encode_refuses_a_buffer_too_small();

    if (g_failures) {
        printf("oxyii_codec tests FAILED (%d)\n", g_failures);
        return 1;
    }
    printf("oxyii_codec tests passed\n");
    return 0;
}
