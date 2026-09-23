/*
 * SomnoTrace - SomnoStage model init: SSTG envelope decrypt + SSTP parse
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

/* Boot-time model load: the embedded model.enc is memory-mapped flash, so
 * decryption MUST run early in app_main before any task that writes flash
 * exists (flash-cache constraint, see .ai/MODEL-CONSUME-v2.MD). The decrypted
 * SSTP payload lives in PSRAM; the model parser treats it zero-copy. */

#include "somno_ml.h"
#include "somno_ml_internal.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "mbedtls/aes.h"
#include "mbedtls/sha256.h"

static const char *TAG = "somno_ml";

/* embedded by idf_component_register(EMBED_FILES "model.enc") */
extern const uint8_t model_enc_start[] asm("_binary_model_enc_start");
extern const uint8_t model_enc_end[]   asm("_binary_model_enc_end");

/* SSTG envelope header layout (196 bytes, little-endian) */
#define SSTG_HEADER_SIZE 196
#define SSTG_OFF_VER       4
#define SSTG_OFF_HDRSIZE   6
#define SSTG_OFF_SEMVER    8
#define SSTG_OFF_PSIZE   144
#define SSTG_OFF_SHA256  148
#define SSTG_OFF_IV      180

static somno_model_t *s_model;
static char s_semver[33] = "-";
static bool s_tried;

static uint32_t rd_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint16_t rd_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

int somno_ml_init(void)
{
    if (s_tried) return s_model ? 0 : -1;
    s_tried = true;

    size_t enc_len = (size_t)(model_enc_end - model_enc_start);
    if (enc_len < SSTG_HEADER_SIZE + 32 ||
        memcmp(model_enc_start, "SSTG", 4) != 0) {
        ESP_LOGE(TAG, "model.enc: bad envelope");
        return -1;
    }
    if (rd_le16(model_enc_start + SSTG_OFF_VER) != 1 ||
        rd_le16(model_enc_start + SSTG_OFF_HDRSIZE) != SSTG_HEADER_SIZE) {
        ESP_LOGE(TAG, "model.enc: unsupported envelope version");
        return -1;
    }
    memcpy(s_semver, model_enc_start + SSTG_OFF_SEMVER, 32);
    s_semver[32] = '\0';
    uint32_t psize = rd_le32(model_enc_start + SSTG_OFF_PSIZE);
    size_t ct_len = enc_len - SSTG_HEADER_SIZE;
    /* CBC ciphertext is psize + PKCS7 pad (1..16 bytes); anything shorter or
       longer is truncated/corrupt.  (Old check assumed pad == 16, which
       falsely rejects payloads whose length isn't 16-aligned.) */
    if (ct_len % 16 != 0 || ct_len <= (size_t)psize ||
        ct_len - (size_t)psize > 16) {
        ESP_LOGE(TAG, "model.enc: bad ciphertext length (%u + pad vs %u)",
                 (unsigned)psize, (unsigned)ct_len);
        return -1;
    }

    uint8_t key[32];
    somno_ml_reconstruct_key(key);

    uint8_t *plain = heap_caps_malloc(enc_len - SSTG_HEADER_SIZE,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!plain) {
        ESP_LOGE(TAG, "model: PSRAM alloc failed");
        memset(key, 0, sizeof(key));
        return -1;
    }

    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    uint8_t iv[16];
    memcpy(iv, model_enc_start + SSTG_OFF_IV, 16);
    int rc = mbedtls_aes_setkey_dec(&aes, key, 256);
    if (rc == 0)
        rc = mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_DECRYPT,
                                   enc_len - SSTG_HEADER_SIZE, iv,
                                   model_enc_start + SSTG_HEADER_SIZE, plain);
    mbedtls_aes_free(&aes);
    memset(key, 0, sizeof(key));        /* scrub key halves product */
    memset(iv, 0, sizeof(iv));
    if (rc != 0) {
        ESP_LOGE(TAG, "model: decrypt failed (%d)", rc);
        free(plain);
        return -1;
    }

    uint8_t sha[32];
    mbedtls_sha256(plain, psize, sha, 0);
    if (memcmp(sha, model_enc_start + SSTG_OFF_SHA256, 32) != 0) {
        ESP_LOGE(TAG, "model: plaintext SHA-256 mismatch (wrong key?)");
        memset(plain, 0, enc_len - SSTG_HEADER_SIZE);
        free(plain);
        return -1;
    }

    s_model = somno_ml_model_load(plain, psize);
    if (!s_model) {
        ESP_LOGE(TAG, "model: SSTP parse failed");
        memset(plain, 0, enc_len - SSTG_HEADER_SIZE);
        free(plain);
        return -1;
    }
    ESP_LOGI(TAG, "SomnoStage model %s loaded (%u bytes payload, %d features)",
             s_semver, (unsigned)psize, somno_ml_model_n_features(s_model));
    somno_ml_worker_start();
    return 0;
}

const somno_model_t *somno_ml_get_model(void) { return s_model; }

bool somno_ml_available(void) { return s_model != NULL; }

const char *somno_ml_model_semver(void) { return s_semver; }
