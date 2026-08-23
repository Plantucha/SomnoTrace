/*
 * SomnoTrace - O2 Ring (OxyII) BLE protocol codec and session
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 *
 * This file is part of SomnoTrace.
 *
 * SomnoTrace is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
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
 *
 * Clean-room OxyII BLE protocol for Wellue O2 Ring S / SleepHQ O2 Ring Pro.
 * See spec/0003-o2ring-ble-sync.md.
 */

#include "oximeter.h"
#include "sd_storage.h"
#include "as11_ble.h"
#include "psram_task.h"
#include "nvs_writer.h"
#include "oximetry_canonical.h"
#include "time_sync.h"
#include "upload_sched.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <time.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "nimble/nimble_port.h"
#include "host/ble_hs.h"
#include "host/util/util.h"

static const char *TAG = "oximeter";

/* ── Store forward declarations (oximeter_store.c) ─────────────────── */
void ox_store_ensure_dirs(void);
bool ox_store_load_paired(char *serial, size_t serial_sz,
                          char *firmware, size_t fw_sz,
                          char *name_prefix, size_t prefix_sz,
                          char *last_addr, size_t addr_sz);
void ox_store_save_paired(const char *serial, const char *firmware,
                          const char *name_prefix, const char *last_addr);
void ox_store_delete_paired(void);
int  ox_store_index_check(const char *serial, const char *name);
void ox_store_index_add(const char *serial, const char *name,
                        uint32_t bytes, bool finalised);
long ox_store_part_size(const char *name);
esp_err_t ox_store_part_append(const char *name, const uint8_t *data, size_t len);
bool ox_store_promote(const char *serial, const char *name);
void ox_store_part_remove(const char *name);

/* ── OxyII protocol constants ──────────────────────────────────────── */
#define OXYII_LEAD         0xA5
#define OXYII_HEADER_LEN   7
#define OXYII_MAX_FRAME    2048

#define OP_GET_CONFIG      0x00
#define OP_LIVE_B          0x04
#define OP_SETUP           0x10
#define OP_SET_UTC_TIME    0xC0
#define OP_GET_INFO        0xE1
#define OP_GET_BATTERY     0xE4
#define OP_GET_FILE_LIST   0xF1
#define OP_READ_FILE_START 0xF2
#define OP_READ_FILE_DATA  0xF3
#define OP_READ_FILE_END   0xF4
#define OP_AUTH            0xFF

#define MFG_OXYII          0xF34E
#define MFG_RECORDING      0x036F

/* MD5("lepucloud") = c2a7cf50dafed885a8f8f7eac44335f3 */
static const uint8_t LEPUCLOUD_MD5[16] = {
    0xc2, 0xa7, 0xcf, 0x50, 0xda, 0xfe, 0xd8, 0x85,
    0xa8, 0xf8, 0xf7, 0xea, 0xc4, 0x43, 0x35, 0xf3,
};

/* OxyII GATT UUIDs (128-bit, stored little-endian for NimBLE) */
static const ble_uuid128_t OXYII_SVC_UUID =
    BLE_UUID128_INIT(0x48, 0x12, 0xd0, 0x41, 0x29, 0x4e, 0x1b, 0x83,
                     0xf9, 0x98, 0x4b, 0xa1, 0x01, 0x00, 0xfb, 0xe8);
static const ble_uuid128_t OXYII_WRITE_UUID =
    BLE_UUID128_INIT(0x48, 0x12, 0xd0, 0x41, 0x29, 0x4e, 0x1b, 0x83,
                     0xf9, 0x98, 0x4b, 0xa1, 0x02, 0x00, 0xfb, 0xe8);
static const ble_uuid128_t OXYII_NOTIFY_UUID =
    BLE_UUID128_INIT(0x48, 0x12, 0xd0, 0x41, 0x29, 0x4e, 0x1b, 0x83,
                     0xf9, 0x98, 0x4b, 0xa1, 0x03, 0x00, 0xfb, 0xe8);

/* ── CRC8 (poly=0x07, init=0) ──────────────────────────────────────── */
static uint8_t oxyii_crc8(const uint8_t *data, int len)
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

/* ── Frame codec ───────────────────────────────────────────────────── */
/* Encode an OxyII frame into buf.  Returns total frame length. */
static int oxyii_encode(uint8_t *buf, int bufsz, uint8_t op,
                         uint8_t flag, uint8_t seq,
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

/* Try to decode a frame from buf.  Returns total frame length on success,
 * -1 if incomplete (need more data), -2 if invalid (bad lead/crc). */
static int oxyii_try_decode(const uint8_t *buf, int len,
                             uint8_t *op, uint8_t *flag, uint8_t *seq,
                             uint8_t *payload, int *payload_len,
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

/* ── Auth payload ──────────────────────────────────────────────────── */
/* Derive session key and XOR with LEPUCLOUD_MD5 to produce auth payload. */
static void oxyii_auth_payload(uint8_t *out16)
{
    uint8_t key[16];
    /* key[0..7] = LEPUCLOUD_MD5 even-indexed bytes */
    for (int i = 0; i < 8; i++)
        key[i] = LEPUCLOUD_MD5[i * 2];
    /* key[8..11] = "0000" (default serial prefix) */
    memcpy(key + 8, "0000", 4);
    /* key[12..15] = (ts >> 0), (ts >> 1), (ts >> 2), (ts >> 3) — not LE bytes */
    time_t now = time(NULL);
    for (int i = 0; i < 4; i++)
        key[12 + i] = (now >> i) & 0xFF;
    /* auth = key XOR LEPUCLOUD_MD5 */
    for (int i = 0; i < 16; i++)
        out16[i] = key[i] ^ LEPUCLOUD_MD5[i];
}

/* ── SET_UTC_TIME payload (8 bytes) ────────────────────────────────── */
static void oxyii_time_payload(uint8_t *out8)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    out8[0] = (tm.tm_year + 1900) & 0xFF;
    out8[1] = ((tm.tm_year + 1900) >> 8) & 0xFF;
    out8[2] = tm.tm_mon + 1;
    out8[3] = tm.tm_mday;
    out8[4] = tm.tm_hour;
    out8[5] = tm.tm_min;
    out8[6] = tm.tm_sec;
    out8[7] = 0x00;
}

static int64_t oxyii_filename_epoch_ms(const char *name)
{
    if (!name || strlen(name) < 14) return 0;
    for (int i = 0; i < 14; i++)
        if (name[i] < '0' || name[i] > '9') return 0;
    struct tm tm = {0};
    int year, mon, day, hour, min, sec;
    if (sscanf(name, "%4d%2d%2d%2d%2d%2d", &year, &mon, &day,
               &hour, &min, &sec) != 6)
        return 0;
    tm.tm_year = year - 1900;
    tm.tm_mon = mon - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = min;
    tm.tm_sec = sec;
    tm.tm_isdst = -1;
    time_t t = mktime(&tm);
    return t == (time_t)-1 ? 0 : (int64_t)t * 1000;
}

/* ── READ_FILE_START payload (20 bytes) ────────────────────────────── */
static void oxyii_file_start_payload(uint8_t *out20, const char *name)
{
    memset(out20, 0, 20);
    strncpy((char *)out20, name, 16);
    /* bytes 16..19: file type = 0 */
}

/* ── READ_FILE_DATA payload (4 bytes) ──────────────────────────────── */
static void oxyii_file_data_payload(uint8_t *out4, uint32_t offset)
{
    out4[0] = offset & 0xFF;
    out4[1] = (offset >> 8) & 0xFF;
    out4[2] = (offset >> 16) & 0xFF;
    out4[3] = (offset >> 24) & 0xFF;
}

/* ── Module state ──────────────────────────────────────────────────── */
#define OX_SCAN_MAX 16

struct ox_scan_result {
    ble_addr_t addr;
    char name[32];
    int rssi;
    uint16_t mfg;
};

static SemaphoreHandle_t s_state_mtx;
static SemaphoreHandle_t s_ops_mtx;     /* serialise BLE ops (scan/pair/pull) */
static SemaphoreHandle_t s_op_sem;      /* GATT op completion */
static SemaphoreHandle_t s_conn_sem;    /* connect completion */
static SemaphoreHandle_t s_resp_sem;    /* notification response */
static SemaphoreHandle_t s_scan_done;
static volatile int s_op_status;
static volatile int s_conn_status;

static char s_status[24] = OX_STATUS_IDLE;
static char s_error[128];

/* Paired ring info (loaded from NVS at init) */
static char s_serial[32];
static char s_firmware[16];
static char s_name_prefix[16];
static char s_paired_addr[18];
static bool s_paired = false;
static bool s_presence_served = false;
static bool s_ring_present = false;
static TickType_t s_served_at;
static int s_f1_fail_count = 0;  /* consecutive F1 timeouts in this sync window */

/* Measured: END powers off ~120s after take-off IF no one connects.
 * Any GATT connection resets that timer. Pull happens inside the
 * window, so after a pull: never reconnect while the advert lasts.
 * Still advertising at pull+130s can only mean re-worn. */
#define OX_END_WINDOW_MS  130000
#define OX_WORN_PROBE_MS  60000  /* LIVE_B interval while worn; END lasts ~120s */
#define OX_F1_MAX_RETRIES 3      /* give up on F1 after N consecutive timeouts */

/* BLE connection state */
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_write_handle;
static uint16_t s_notify_handle;
static uint16_t s_cccd_handle;
static uint16_t s_svc_start, s_svc_end;
static uint8_t s_seq = 0;

/* Scan state */
static struct ox_scan_result s_scan[OX_SCAN_MAX];
static int s_scan_count;

/* Notification accumulation buffer */
static uint8_t s_resp_buf[OXYII_MAX_FRAME];
static int s_resp_len;
static uint8_t s_resp_opcode;
static uint8_t s_resp_payload[OXYII_MAX_FRAME];
static int s_resp_payload_len;

/* ── Helpers ───────────────────────────────────────────────────────── */
static void set_state(const char *st)
{
    xSemaphoreTake(s_state_mtx, portMAX_DELAY);
    strlcpy(s_status, st, sizeof(s_status));
    xSemaphoreGive(s_state_mtx);
}

static void set_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    xSemaphoreTake(s_state_mtx, portMAX_DELAY);
    vsnprintf(s_error, sizeof(s_error), fmt, ap);
    strlcpy(s_status, OX_STATUS_ERROR, sizeof(s_status));
    xSemaphoreGive(s_state_mtx);
    va_end(ap);
}

static void clear_op_sem(void)
{
    while (xSemaphoreTake(s_op_sem, 0) == pdTRUE) { }
}

static int wait_op(int timeout_ms)
{
    if (xSemaphoreTake(s_op_sem, pdMS_TO_TICKS(timeout_ms)) != pdTRUE)
        return BLE_HS_ETIMEOUT;
    return s_op_status;
}

static bool name_is_oxyii(const char *name)
{
    if (!name || !name[0]) return false;
    char up[32];
    int i;
    for (i = 0; i < 31 && name[i]; i++)
        up[i] = toupper((unsigned char)name[i]);
    up[i] = '\0';
    return strncmp(up, "S8-AW", 5) == 0 ||
           strncmp(up, "SHQO2PRO", 8) == 0;
}

static void addr_to_str(const ble_addr_t *a, char *out, size_t outsz)
{
    snprintf(out, outsz, "%02x:%02x:%02x:%02x:%02x:%02x",
             a->val[5], a->val[4], a->val[3], a->val[2], a->val[1], a->val[0]);
}

static bool parse_addr(const char *str, ble_addr_t *out)
{
    unsigned int v[6];
    int n = sscanf(str, "%x:%x:%x:%x:%x:%x",
                   &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]);
    if (n != 6) return false;
    out->val[5] = v[0]; out->val[4] = v[1]; out->val[3] = v[2];
    out->val[2] = v[3]; out->val[1] = v[4]; out->val[0] = v[5];
    /* Static random addresses have the top two bits of the most-significant
     * byte set (0xC0 mask).  O2 Ring uses a static random address. */
    out->type = (v[0] & 0xC0) == 0xC0 ? BLE_ADDR_RANDOM : BLE_ADDR_PUBLIC;
    return true;
}

/* ── GAP event handler ─────────────────────────────────────────────── */
static int gap_event(struct ble_gap_event *event, void *arg);

/* Handle notification: accumulate and try to decode. */
static void handle_notify_rx(const uint8_t *data, int len)
{
    if (s_resp_len + len > (int)sizeof(s_resp_buf)) {
        ESP_LOGW(TAG, "notify overflow: resp_len=%d + %d > %d",
                 s_resp_len, len, (int)sizeof(s_resp_buf));
        s_resp_len = 0;
    }
    memcpy(s_resp_buf + s_resp_len, data, len);
    s_resp_len += len;

    uint8_t op, flag, seq;
    int plen;
    int rc = oxyii_try_decode(s_resp_buf, s_resp_len, &op, &flag, &seq,
                               s_resp_payload, &plen, sizeof(s_resp_payload));
    if (rc > 0) {
        s_resp_opcode = op;
        s_resp_payload_len = plen;
        s_resp_len = 0;
        xSemaphoreGive(s_resp_sem);
    } else if (rc == -2) {
        ESP_LOGW(TAG, "notify decode error, resetting buffer");
        s_resp_len = 0;
    }
    /* rc == -1: incomplete, wait for more data */
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        char addr_str[18];
        addr_to_str(&event->disc.addr, addr_str, sizeof(addr_str));

        /* Parse name from adv data (with manual fallback) */
        struct ble_hs_adv_fields f;
        char name[32] = {0};
        const uint8_t *raw = event->disc.data;
        int raw_len = event->disc.length_data;

        memset(&f, 0, sizeof(f));
        if (ble_hs_adv_parse_fields(&f, raw, raw_len) != 0) {
            for (int off = 0; off + 1 < raw_len; ) {
                uint8_t ad_len = raw[off];
                if (ad_len == 0 || off + 1 + ad_len > raw_len) break;
                uint8_t ad_type = raw[off + 1];
                const uint8_t *ad_data = raw + off + 2;
                int ad_data_len = ad_len - 1;
                if (ad_type == 0x09 || ad_type == 0x08) {
                    int nl = ad_data_len < 31 ? ad_data_len : 31;
                    memcpy(name, ad_data, nl);
                    name[nl] = '\0';
                } else if (ad_type == 0xFF && ad_data_len >= 2) {
                    f.mfg_data = ad_data;
                    f.mfg_data_len = ad_data_len;
                }
                off += 1 + ad_len;
            }
        } else {
            if (f.name && f.name_len > 0) {
                int nl = f.name_len < 31 ? f.name_len : 31;
                memcpy(name, f.name, nl);
                name[nl] = '\0';
            }
        }

        uint16_t cid = 0;
        if (f.mfg_data && f.mfg_data_len >= 2)
            cid = f.mfg_data[0] | (f.mfg_data[1] << 8);

        /* Recording-mode advert (on-finger): never a pull candidate. */
        if (cid == MFG_RECORDING)
            return 0;

        bool match = name_is_oxyii(name) || cid == MFG_OXYII;
        if (!match) return 0;
        if (name[0] == '\0')
            strlcpy(name, "O2Ring", sizeof(name));

        /* Dedupe by address */
        for (int i = 0; i < s_scan_count; i++) {
            if (memcmp(&s_scan[i].addr, &event->disc.addr,
                       sizeof(ble_addr_t)) == 0) {
                s_scan[i].rssi = event->disc.rssi;
                s_scan[i].mfg = cid;
                if (name[0] && strncmp(name, "O2Ring", 6) != 0)
                    strlcpy(s_scan[i].name, name, sizeof(s_scan[i].name));
                return 0;
            }
        }
        if (s_scan_count < OX_SCAN_MAX) {
            s_scan[s_scan_count].addr = event->disc.addr;
            strlcpy(s_scan[s_scan_count].name, name,
                    sizeof(s_scan[s_scan_count].name));
            s_scan[s_scan_count].rssi = event->disc.rssi;
            s_scan[s_scan_count].mfg = cid;
            s_scan_count++;
            ESP_LOGD(TAG, "scan: '%s' rssi=%d addr=%s mfg=0x%04x",
                     name, event->disc.rssi, addr_str, cid);
        }
        return 0;
    }
    case BLE_GAP_EVENT_DISC_COMPLETE:
        xSemaphoreGive(s_scan_done);
        return 0;

    case BLE_GAP_EVENT_CONNECT:
        s_conn_handle = event->connect.conn_handle;
        s_conn_status = event->connect.status;
        xSemaphoreGive(s_conn_sem);
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnected (reason=%d)",
                 event->disconnect.reason);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        /* Unblock any waiting request */
        xSemaphoreGive(s_resp_sem);
        xSemaphoreGive(s_conn_sem);
        return 0;

    case BLE_GAP_EVENT_NOTIFY_RX: {
        if (event->notify_rx.conn_handle != s_conn_handle)
            return 0;
        int len = OS_MBUF_PKTLEN(event->notify_rx.om);
        if (len <= 0) return 0;
        uint8_t *data = malloc(len);
        if (!data) return 0;
        os_mbuf_copydata(event->notify_rx.om, 0, len, data);
        handle_notify_rx(data, len);
        free(data);
        return 0;
    }
    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU: %d", event->mtu.value);
        return 0;
    default:
        return 0;
    }
}

/* ── GATT discovery callbacks ──────────────────────────────────────── */
static int on_mtu(uint16_t conn, const struct ble_gatt_error *err,
                  uint16_t mtu, void *arg)
{
    (void)conn; (void)arg; (void)mtu;
    s_op_status = err ? err->status : 0;
    xSemaphoreGive(s_op_sem);
    return 0;
}

static int on_disc_svc(uint16_t conn, const struct ble_gatt_error *err,
                       const struct ble_gatt_svc *svc, void *arg)
{
    (void)conn; (void)arg;
    if (err && err->status == 0 && svc) {
        s_svc_start = svc->start_handle;
        s_svc_end = svc->end_handle;
    }
    if (err && (err->status == BLE_HS_EDONE || err->status != 0)) {
        s_op_status = (err->status == BLE_HS_EDONE) ? 0 : err->status;
        xSemaphoreGive(s_op_sem);
    }
    return 0;
}

static int on_disc_chr(uint16_t conn, const struct ble_gatt_error *err,
                       const struct ble_gatt_chr *chr, void *arg)
{
    (void)conn; (void)arg;
    if (err && err->status == 0 && chr) {
        if (ble_uuid_cmp(&chr->uuid.u, &OXYII_WRITE_UUID.u) == 0)
            s_write_handle = chr->val_handle;
        else if (ble_uuid_cmp(&chr->uuid.u, &OXYII_NOTIFY_UUID.u) == 0)
            s_notify_handle = chr->val_handle;
    }
    if (err && (err->status == BLE_HS_EDONE || err->status != 0)) {
        s_op_status = (err->status == BLE_HS_EDONE) ? 0 : err->status;
        xSemaphoreGive(s_op_sem);
    }
    return 0;
}

static int on_disc_dsc(uint16_t conn, const struct ble_gatt_error *err,
                       uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc,
                       void *arg)
{
    (void)conn; (void)chr_val_handle; (void)arg;
    if (err && err->status == 0 && dsc) {
        const ble_uuid16_t cccd = BLE_UUID16_INIT(0x2902);
        if (ble_uuid_cmp(&dsc->uuid.u, &cccd.u) == 0 && s_cccd_handle == 0)
            s_cccd_handle = dsc->handle;
    }
    if (err && (err->status == BLE_HS_EDONE || err->status != 0)) {
        s_op_status = (err->status == BLE_HS_EDONE) ? 0 : err->status;
        xSemaphoreGive(s_op_sem);
    }
    return 0;
}

static int on_write_done(uint16_t conn, const struct ble_gatt_error *err,
                         struct ble_gatt_attr *attr, void *arg)
{
    (void)conn; (void)attr; (void)arg;
    s_op_status = err ? err->status : 0;
    xSemaphoreGive(s_op_sem);
    return 0;
}

/* ── Connect and discover GATT services ────────────────────────────── */
static esp_err_t do_connect_and_discover(ble_addr_t *target)
{
    s_write_handle = s_notify_handle = s_cccd_handle = 0;
    s_svc_start = s_svc_end = 0;
    s_resp_len = 0;

    /* Connect — infer own address type based on peer address type
     * (random peer requires random own address). */
    uint8_t own_addr_type;
    int rc = ble_hs_id_infer_auto(target->type, &own_addr_type);
    if (rc != 0) {
        set_error("addr infer failed: %d", rc);
        return ESP_FAIL;
    }
    clear_op_sem();
    rc = ble_gap_connect(own_addr_type, target,
                         15000, NULL, gap_event, NULL);
    if (rc != 0) { set_error("connect start failed: %d", rc); return ESP_FAIL; }
    if (xSemaphoreTake(s_conn_sem, pdMS_TO_TICKS(16000)) != pdTRUE) {
        set_error("connect timeout"); return ESP_FAIL;
    }
    if (s_conn_status != 0) {
        set_error("connect failed: %d", s_conn_status); return ESP_FAIL;
    }
    ESP_LOGI(TAG, "connected, handle=%d", s_conn_handle);

    /* MTU exchange */
    clear_op_sem();
    ble_gattc_exchange_mtu(s_conn_handle, on_mtu, NULL);
    wait_op(2000);

    /* Discover OxyII service by UUID */
    clear_op_sem();
    rc = ble_gattc_disc_svc_by_uuid(s_conn_handle, &OXYII_SVC_UUID.u,
                                     on_disc_svc, NULL);
    if (rc != 0 || wait_op(10000) != 0) {
        set_error("OxyII service not found"); return ESP_FAIL;
    }
    if (s_svc_start == 0) { set_error("service range empty"); return ESP_FAIL; }
    ESP_LOGI(TAG, "service: 0x%04x-0x%04x", s_svc_start, s_svc_end);

    /* Discover characteristics */
    clear_op_sem();
    rc = ble_gattc_disc_all_chrs(s_conn_handle, s_svc_start, s_svc_end,
                                 on_disc_chr, NULL);
    if (rc != 0 || wait_op(10000) != 0) {
        set_error("characteristic discovery failed"); return ESP_FAIL;
    }
    if (s_write_handle == 0 || s_notify_handle == 0) {
        set_error("write/notify char not found"); return ESP_FAIL;
    }
    ESP_LOGI(TAG, "write=%d notify=%d", s_write_handle, s_notify_handle);

    /* Discover CCCD for notify characteristic */
    clear_op_sem();
    rc = ble_gattc_disc_all_dscs(s_conn_handle, s_notify_handle, s_svc_end,
                                 on_disc_dsc, NULL);
    if (rc != 0 || wait_op(10000) != 0) {
        set_error("CCCD discovery failed"); return ESP_FAIL;
    }
    if (s_cccd_handle == 0) { set_error("CCCD not found"); return ESP_FAIL; }

    /* Enable notifications */
    uint8_t cccd_val[2] = { 0x01, 0x00 };
    clear_op_sem();
    rc = ble_gattc_write_flat(s_conn_handle, s_cccd_handle,
                              cccd_val, 2, on_write_done, NULL);
    if (rc != 0 || wait_op(5000) != 0) {
        set_error("enable notify failed"); return ESP_FAIL;
    }
    ESP_LOGI(TAG, "notifications enabled (cccd=%d)", s_cccd_handle);
    return ESP_OK;
}

static void do_disconnect(void)
{
    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        xSemaphoreTake(s_conn_sem, pdMS_TO_TICKS(3000));
    }
}

/* ── Protocol request/response ─────────────────────────────────────── */
static esp_err_t oxyii_request(uint8_t op, const uint8_t *payload, int plen,
                                bool expect_reply, int timeout_ms)
{
    uint8_t frame[OXYII_MAX_FRAME];
    int flen = oxyii_encode(frame, sizeof(frame), op, 0, s_seq++,
                             payload, plen);
    if (flen < 0) return ESP_FAIL;

    if (expect_reply) {
        while (xSemaphoreTake(s_resp_sem, 0) == pdTRUE) { }
        s_resp_len = 0;
    }

    /* Use write-without-response (OxyII protocol uses WRITE_CMD, not WRITE_REQ).
     * This eliminates one BLE round-trip per request, roughly doubling throughput. */
    int rc = ble_gattc_write_no_rsp_flat(s_conn_handle, s_write_handle,
                                          frame, flen);
    if (rc != 0) {
        ESP_LOGE(TAG, "write failed op=0x%02x rc=%d", op, rc);
        return ESP_FAIL;
    }

    if (!expect_reply) return ESP_OK;

    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (true) {
        TickType_t now = xTaskGetTickCount();
        if ((int32_t)(deadline - now) <= 0) {
            ESP_LOGE(TAG, "response timeout op=0x%02x", op);
            return ESP_ERR_TIMEOUT;
        }
        if (xSemaphoreTake(s_resp_sem, deadline - now) != pdTRUE) {
            ESP_LOGE(TAG, "response timeout op=0x%02x", op);
            return ESP_ERR_TIMEOUT;
        }
        if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE)
            return ESP_FAIL;
        if (s_resp_opcode == op)
            return ESP_OK;
        ESP_LOGW(TAG, "ignoring leftover notify op=0x%02x (want 0x%02x, %d B)",
                 s_resp_opcode, op, s_resp_payload_len);
    }
}

/* AUTH + SETUP only. Enough for GET_INFO / LIVE_B. Do not send F4
 * here — that closes a recording handle. Measured (SHQO2Pro 2D010003):
 * LIVE_B replies even with no AUTH; GET_INFO needs this prefix. */
static esp_err_t oxyii_session_open(void)
{
    s_seq = 0;
    uint8_t auth[16];
    oxyii_auth_payload(auth);
    if (oxyii_request(OP_AUTH, auth, 16, false, 0) != ESP_OK)
        return ESP_FAIL;
    vTaskDelay(pdMS_TO_TICKS(200));

    uint8_t setup = 0x00;
    if (oxyii_request(OP_SETUP, &setup, 1, true, 5000) != ESP_OK)
        return ESP_FAIL;
    return ESP_OK;
}

/* File-session prep. Only after LIVE_B says off-finger.
 * Documented order: SET_UTC → GET_CONFIG → F4. F4 clears a leftover
 * recording handle; without it, F1 is silently dropped (the F1 wedge). */
static esp_err_t oxyii_prepare_files(void)
{
    if (!time_is_usable()) {
        ESP_LOGW(TAG, "not setting ring clock: host time is unusable");
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t tp[8];
    oxyii_time_payload(tp);
    if (oxyii_request(OP_SET_UTC_TIME, tp, 8, true, 5000) != ESP_OK)
        return ESP_FAIL;

    if (oxyii_request(OP_GET_CONFIG, NULL, 0, true, 5000) != ESP_OK)
        return ESP_FAIL;

    /* F4 acks. Must consume it or the next F1 sees this frame. */
    if (oxyii_request(OP_READ_FILE_END, NULL, 0, true, 2000) != ESP_OK)
        ESP_LOGW(TAG, "F4 ack missing — continuing");
    return ESP_OK;
}

/* ── GET_INFO: extract firmware + serial ───────────────────────────── */
static esp_err_t oxyii_get_info(char *serial, size_t serial_sz,
                                 char *firmware, size_t fw_sz)
{
    if (oxyii_request(OP_GET_INFO, NULL, 0, true, 5000) != ESP_OK)
        return ESP_FAIL;

    /* Payload layout (from protocol study):
     *   bytes 9..16: firmware (8 chars, null-padded)
     *   byte 37: serial length
     *   bytes 38..: serial string */
    if (s_resp_payload_len < 38) return ESP_FAIL;

    if (firmware && fw_sz > 0) {
        int fl = s_resp_payload_len - 9 < 8 ? s_resp_payload_len - 9 : 8;
        if (fl < 0) fl = 0;
        memcpy(firmware, s_resp_payload + 9, fl);
        firmware[fl] = '\0';
        /* Trim trailing nulls */
        for (int i = fl - 1; i >= 0 && firmware[i] == 0; i--)
            firmware[i] = '\0';
    }

    if (serial && serial_sz > 0) {
        int slen = s_resp_payload[37];
        if (slen > 0 && 38 + slen <= s_resp_payload_len) {
            int n = slen < (int)serial_sz - 1 ? slen : (int)serial_sz - 1;
            memcpy(serial, s_resp_payload + 38, n);
            serial[n] = '\0';
        } else {
            serial[0] = '\0';
        }
    }
    return ESP_OK;
}

/* LIVE_B contact. Returns:
 *   1  off-finger / END window (state 0x00) — pull is allowed
 *   0  on-finger or file handle open — do not pull
 *  -1  no usable reply — do not pull (fail closed)
 *
 * README: [5] 0x00 = no finger, 0x01 = finger present, 0x03 = file open. */
static int oxyii_off_finger(void)
{
    if (oxyii_request(OP_LIVE_B, NULL, 0, true, 2000) != ESP_OK)
        return -1;
    if (s_resp_payload_len < 9) return -1;
    uint8_t state = s_resp_payload[5];
    uint8_t spo2  = s_resp_payload[6];
    uint8_t hr    = s_resp_payload[8];
    ESP_LOGI(TAG, "live_b: state=0x%02x spo2=%u hr=%u", state, spo2, hr);

    /* 0x03 = leftover file handle from the ring's own recording
     * (README "F1 wedge": F1 silently hangs until F4). Closing it is
     * safe and lets END report its true contact state. */
    if (state == 0x03) {
        ESP_LOGI(TAG, "live_b: file handle open — clearing wedge with F4");
        oxyii_request(OP_READ_FILE_END, NULL, 0, true, 2000);
        if (oxyii_request(OP_LIVE_B, NULL, 0, true, 2000) != ESP_OK)
            return -1;
        if (s_resp_payload_len < 9) return -1;
        state = s_resp_payload[5];
        spo2  = s_resp_payload[6];
        hr    = s_resp_payload[8];
        ESP_LOGI(TAG, "live_b after F4: state=0x%02x spo2=%u hr=%u",
                 state, spo2, hr);
    }

    if (state == 0x00) return 1;
    return 0;
}

/* ── GET_FILE_LIST: return count + names ───────────────────────────── */
static int oxyii_get_file_list(char names[][17], int max_count)
{
    int rc = oxyii_request(OP_GET_FILE_LIST, NULL, 0, true, 5000);
    if (rc != ESP_OK) {
        /* Spec 4.7: F1 timeout → F4, retry once on this link, then fail. */
        ESP_LOGW(TAG, "F1 timed out (rc=%d) — F4 then retry once", rc);
        oxyii_request(OP_READ_FILE_END, NULL, 0, true, 2000);
        if (oxyii_request(OP_GET_FILE_LIST, NULL, 0, true, 5000) != ESP_OK)
            return -1;
    }

    if (s_resp_payload_len < 1) return -1;
    int count = s_resp_payload[0];
    if (count > max_count) count = max_count;

    int pos = 1;
    for (int i = 0; i < count; i++) {
        if (pos + 16 > s_resp_payload_len) break;
        memcpy(names[i], s_resp_payload + pos, 16);
        names[i][16] = '\0';
        /* Trim trailing nulls */
        for (int j = 15; j >= 0 && names[i][j] == 0; j--)
            names[i][j] = '\0';
        pos += 16;
    }
    return count;
}

/* ── Pull a single file ────────────────────────────────────────────── */
static esp_err_t oxyii_pull_file(const char *name)
{
    /* READ_FILE_START */
    uint8_t start_pl[20];
    oxyii_file_start_payload(start_pl, name);
    if (oxyii_request(OP_READ_FILE_START, start_pl, 20, true, 5000) != ESP_OK)
        return ESP_FAIL;

    uint32_t file_size = 0;
    if (s_resp_payload_len >= 4)
        file_size = s_resp_payload[0] | (s_resp_payload[1] << 8) |
                    (s_resp_payload[2] << 16) | (s_resp_payload[3] << 24);
    ESP_LOGI(TAG, "pulling '%s' (%lu bytes)", name, (unsigned long)file_size);

    /* Resume from the durable partial length.  The device's reported size is
     * not completion evidence; the decoder validates the trailer later. */
    long prior = ox_store_part_size(name);
    if (prior < 0 || (file_size > 0 && (uint32_t)prior > file_size)) {
        ox_store_part_remove(name);
        prior = 0;
    }
    uint32_t offset = (uint32_t)prior;
    int empty_count = 0;
    bool transfer_complete = (file_size > 0 && offset == file_size);
    while (!transfer_complete) {
        uint8_t off_pl[4];
        oxyii_file_data_payload(off_pl, offset);
        if (oxyii_request(OP_READ_FILE_DATA, off_pl, 4, true, 10000) != ESP_OK) {
            ESP_LOGW(TAG, "file data timeout at offset=%lu", (unsigned long)offset);
            break;
        }

        /* Payload is the file data chunk */
        int chunk_len = s_resp_payload_len;
        if (chunk_len <= 0) {
            if (++empty_count > 2) break;
            continue;
        }
        empty_count = 0;

        if (file_size > 0 && (uint64_t)offset + chunk_len > file_size) {
            ESP_LOGW(TAG, "device returned data past file size at offset=%lu",
                     (unsigned long)offset);
            break;
        }
        if (ox_store_part_append(name, s_resp_payload, chunk_len) != ESP_OK) {
            ESP_LOGE(TAG, "SD write failed at offset=%lu", (unsigned long)offset);
            break;
        }
        offset += chunk_len;

        if (file_size > 0 && offset == file_size) {
            transfer_complete = true;
        }
    }

    /* READ_FILE_END */
    if (oxyii_request(OP_READ_FILE_END, NULL, 0, true, 2000) != ESP_OK)
        ESP_LOGW(TAG, "F4 ack missing after '%s'", name);

    /* Completion requires both an exact transfer and the O2 Ring S trailer. */
    if (!transfer_complete || file_size == 0) {
        ESP_LOGW(TAG, "incomplete '%s': %lu/%lu bytes; retaining .part",
                 name, (unsigned long)offset, (unsigned long)file_size);
        return ESP_FAIL;
    }
    bool finalised = ox_store_promote(s_serial, name);
    ESP_LOGI(TAG, "pulled '%s': %lu bytes, finalised=%d",
             name, (unsigned long)offset, finalised);
    if (!finalised) return ESP_FAIL;

    char source_path[640];
    snprintf(source_path, sizeof(source_path), "%s/oximetry/files/%s/%s.bin",
             SD_MOUNT_POINT, s_serial, name);
    if (oximetry_canonical_convert_format_a(s_serial, name, source_path,
                                            oxyii_filename_epoch_ms(name)) != ESP_OK) {
        ESP_LOGW(TAG, "canonical conversion pending for '%s'", name);
        return ESP_FAIL;
    }
    upload_sched_request_scan();
    return ESP_OK;
}

/* ── NVS persistence ───────────────────────────────────────────────── */
#define OX_NVS_NS "oximeter"

struct ox_nvs_arg {
    char serial[32];
    char firmware[16];
    char name_prefix[16];
    char last_addr[18];
};

static esp_err_t do_save_nvs(void *arg)
{
    struct ox_nvs_arg *a = arg;
    nvs_handle_t h;
    esp_err_t e = nvs_open(OX_NVS_NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    nvs_set_str(h, "serial", a->serial);
    nvs_set_str(h, "firmware", a->firmware);
    nvs_set_str(h, "name_prefix", a->name_prefix);
    nvs_set_str(h, "last_addr", a->last_addr);
    e = nvs_commit(h);
    nvs_close(h);
    return e;
}

static esp_err_t do_erase_nvs(void *arg)
{
    (void)arg;
    nvs_handle_t h;
    esp_err_t e = nvs_open(OX_NVS_NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    nvs_erase_key(h, "serial");
    nvs_erase_key(h, "firmware");
    nvs_erase_key(h, "name_prefix");
    nvs_erase_key(h, "last_addr");
    e = nvs_commit(h);
    nvs_close(h);
    return e;
}

static void load_paired_from_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(OX_NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t len;

    len = sizeof(s_serial);
    if (nvs_get_str(h, "serial", s_serial, &len) == ESP_OK && s_serial[0]) {
        s_paired = true;
        len = sizeof(s_firmware);
        nvs_get_str(h, "firmware", s_firmware, &len);
        len = sizeof(s_name_prefix);
        nvs_get_str(h, "name_prefix", s_name_prefix, &len);
        len = sizeof(s_paired_addr);
        nvs_get_str(h, "last_addr", s_paired_addr, &len);
    }
    nvs_close(h);

    /* Also try loading from paired.json (SD) as fallback */
    if (!s_paired) {
        char serial[32], fw[16], prefix[16], addr[18];
        if (ox_store_load_paired(serial, sizeof(serial),
                                 fw, sizeof(fw),
                                 prefix, sizeof(prefix),
                                 addr, sizeof(addr))) {
            strlcpy(s_serial, serial, sizeof(s_serial));
            strlcpy(s_firmware, fw, sizeof(s_firmware));
            strlcpy(s_name_prefix, prefix, sizeof(s_name_prefix));
            strlcpy(s_paired_addr, addr, sizeof(s_paired_addr));
            s_paired = true;
        }
    }
}

/* ── Pair task ─────────────────────────────────────────────────────── */
static void pair_task(void *arg)
{
    char *addr_str = (char *)arg;
    ble_addr_t target;

    xSemaphoreTake(s_ops_mtx, portMAX_DELAY);
    set_state(OX_STATUS_CONNECTING);

    if (!parse_addr(addr_str, &target)) {
        set_error("invalid address: %s", addr_str);
        free(addr_str);
        xSemaphoreGive(s_ops_mtx);
        vTaskDelete(NULL);
        return;
    }

    if (do_connect_and_discover(&target) != ESP_OK) {
        do_disconnect();
        free(addr_str);
        xSemaphoreGive(s_ops_mtx);
        vTaskDelete(NULL);
        return;
    }

    if (oxyii_session_open() != ESP_OK) {
        set_error("session open failed");
        do_disconnect();
        free(addr_str);
        xSemaphoreGive(s_ops_mtx);
        vTaskDelete(NULL);
        return;
    }

    char serial[32] = {0}, firmware[16] = {0};
    if (oxyii_get_info(serial, sizeof(serial), firmware, sizeof(firmware)) != ESP_OK) {
        set_error("get_info failed");
        do_disconnect();
        free(addr_str);
        xSemaphoreGive(s_ops_mtx);
        vTaskDelete(NULL);
        return;
    }

    if (serial[0] == '\0') {
        set_error("empty serial");
        do_disconnect();
        free(addr_str);
        xSemaphoreGive(s_ops_mtx);
        vTaskDelete(NULL);
        return;
    }

    /* Derive name_prefix from serial (first 4 chars) */
    char prefix[5];
    memcpy(prefix, serial, 4);
    prefix[4] = '\0';

    /* Save to NVS */
    struct ox_nvs_arg nvs_arg;
    strlcpy(nvs_arg.serial, serial, sizeof(nvs_arg.serial));
    strlcpy(nvs_arg.firmware, firmware, sizeof(nvs_arg.firmware));
    strlcpy(nvs_arg.name_prefix, prefix, sizeof(nvs_arg.name_prefix));
    strlcpy(nvs_arg.last_addr, addr_str, sizeof(nvs_arg.last_addr));
    nvs_writer_run(do_save_nvs, &nvs_arg);

    /* Save to paired.json on SD */
    ox_store_save_paired(serial, firmware, prefix, addr_str);

    /* Update in-RAM state */
    strlcpy(s_serial, serial, sizeof(s_serial));
    strlcpy(s_firmware, firmware, sizeof(s_firmware));
    strlcpy(s_name_prefix, prefix, sizeof(s_name_prefix));
    strlcpy(s_paired_addr, addr_str, sizeof(s_paired_addr));
    s_paired = true;
    s_presence_served = false;

    do_disconnect();
    set_state(OX_STATUS_PAIRED);
    ESP_LOGI(TAG, "paired: serial=%s fw=%s", serial, firmware);

    free(addr_str);
    xSemaphoreGive(s_ops_mtx);
    vTaskDelete(NULL);
}

/* ── Low-duty OxyII scan (caller holds s_ops_mtx) ──────────────────── */
static esp_err_t do_scan(int timeout_sec)
{
    s_scan_count = 0;

    struct ble_gap_disc_params dp = {
        .itvl = 160,   /* 100 ms */
        .window = 48,  /* 30 ms  — low duty; pairing scan uses 96/96 */
        .filter_policy = 0,
        .limited = 0,
        .passive = 1,  /* watch is listen-only; no scan requests */
    };

    uint8_t own_addr_type;
    int rc = ble_hs_id_infer_auto(BLE_ADDR_RANDOM, &own_addr_type);
    if (rc != 0) own_addr_type = as11_ble_get_own_addr_type();

    while (xSemaphoreTake(s_scan_done, 0) == pdTRUE) { }

    rc = ble_gap_disc(own_addr_type,
                      timeout_sec * 1000, &dp, gap_event, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "scan start failed: %d", rc);
        return ESP_FAIL;
    }

    xSemaphoreTake(s_scan_done, pdMS_TO_TICKS((timeout_sec + 2) * 1000));
    return ESP_OK;
}

/* ── Background watch: scan-only, connect only in the END window ──── */
static void pull_task(void *arg)
{
    (void)arg;

    int wait_ms = 0;
    while (!as11_ble_is_host_ready() && wait_ms < 15000) {
        vTaskDelay(pdMS_TO_TICKS(500));
        wait_ms += 500;
    }
    if (!as11_ble_is_host_ready()) {
        ESP_LOGW(TAG, "watch: host not ready, aborting");
        vTaskDelete(NULL);
        return;
    }

    ox_store_ensure_dirs();

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(15000));

        if (!s_paired || s_serial[0] == '\0')
            continue;
        if (xSemaphoreTake(s_ops_mtx, 0) != pdTRUE)
            continue;

        if (!sd_storage_is_ready()) {
            xSemaphoreGive(s_ops_mtx);
            continue;
        }

        if (do_scan(4) != ESP_OK) {
            xSemaphoreGive(s_ops_mtx);
            continue;
        }

        if (s_scan_count == 0) {
            if (s_ring_present)
                ESP_LOGI(TAG, "ring gone — next appearance is a new sync window");
            s_ring_present = false;
            if (s_presence_served)
                s_presence_served = false;
            s_f1_fail_count = 0;
            xSemaphoreGive(s_ops_mtx);
            continue;
        }

        if (!s_ring_present) {
            ESP_LOGI(TAG, "ring present: '%s' rssi=%d",
                     s_scan[0].name, s_scan[0].rssi);
            s_ring_present = true;
        }

        /* Remember last seen addr (hint; MAC can rotate on factory reset). */
        addr_to_str(&s_scan[0].addr, s_paired_addr, sizeof(s_paired_addr));

        if (s_presence_served) {
            /* Never reconnect during END — a connection resets the
             * ring's power-off timer (measured). Passive scans only.
             * Still advertising past the longest possible END window
             * (take-off + ~120s; pull ran inside it) ⇒ back on a
             * finger ⇒ clear served so the next take-off pulls. */
            if ((xTaskGetTickCount() - s_served_at) < pdMS_TO_TICKS(OX_END_WINDOW_MS)) {
                xSemaphoreGive(s_ops_mtx);
                continue;
            }
            ESP_LOGI(TAG, "watch: still advertising past END window — re-worn, resuming probes");
            s_presence_served = false;
            s_f1_fail_count = 0;
        }

        set_state(OX_STATUS_CONNECTING);

        if (do_connect_and_discover(&s_scan[0].addr) != ESP_OK) {
            ESP_LOGW(TAG, "watch: connect failed: %s", s_error);
            do_disconnect();
            set_state(OX_STATUS_PAIRED);
            xSemaphoreGive(s_ops_mtx);
            continue;
        }

        /* Probe only: AUTH+SETUP. Never F4 while we may still be recording. */
        if (oxyii_session_open() != ESP_OK) {
            ESP_LOGW(TAG, "watch: session open failed");
            do_disconnect();
            set_state(OX_STATUS_PAIRED);
            xSemaphoreGive(s_ops_mtx);
            continue;
        }

        char serial[32] = {0}, firmware[16] = {0};
        if (oxyii_get_info(serial, sizeof(serial), firmware, sizeof(firmware)) != ESP_OK
            || serial[0] == '\0' || strcmp(serial, s_serial) != 0) {
            ESP_LOGW(TAG, "watch: serial mismatch (got '%s', want '%s')",
                     serial, s_serial);
            do_disconnect();
            set_state(OX_STATUS_PAIRED);
            xSemaphoreGive(s_ops_mtx);
            continue;
        }

        /* Pull only in the documented no-contact END window
         * (LIVE_B [5] == 0x00). On-finger / unknown / F1-wedge: drop
         * the link immediately so the ring can keep recording or
         * finish countdown and sleep. Do not mark served. */
        int off = oxyii_off_finger();
        if (off != 1) {
            ESP_LOGI(TAG, "watch: not off-finger (live_b=%d) — disconnect, retry in %ds",
                     off, OX_WORN_PROBE_MS / 1000);
            do_disconnect();
            set_state(OX_STATUS_PAIRED);
            xSemaphoreGive(s_ops_mtx);
            vTaskDelay(pdMS_TO_TICKS(OX_WORN_PROBE_MS));
            continue;
        }

        if (oxyii_prepare_files() != ESP_OK) {
            ESP_LOGW(TAG, "watch: file prep failed");
            do_disconnect();
            set_state(OX_STATUS_PAIRED);
            xSemaphoreGive(s_ops_mtx);
            continue;
        }

        set_state(OX_STATUS_PULLING);

        char names[32][17];
        int count = oxyii_get_file_list(names, 32);
        if (count < 0) {
            s_f1_fail_count++;
            ESP_LOGW(TAG, "file list failed (op=0x%02x len=%d), attempt %d/%d",
                     s_resp_opcode, s_resp_payload_len,
                     s_f1_fail_count, OX_F1_MAX_RETRIES);
            do_disconnect();
            set_state(OX_STATUS_PAIRED);
            if (s_f1_fail_count >= OX_F1_MAX_RETRIES) {
                /* F1 never responds on this ring (firmware quirk).
                 * Mark served so we stop reconnecting — each connection
                 * resets the ring's power-off timer.  Next sync window
                 * (next take-off) will try again fresh. */
                s_presence_served = true;
                s_served_at = xTaskGetTickCount();
                ESP_LOGW(TAG, "F1 unreachable after %d attempts — treating as served, ring can sleep",
                         OX_F1_MAX_RETRIES);
            }
            xSemaphoreGive(s_ops_mtx);
            continue;
        }
        s_f1_fail_count = 0;
        ESP_LOGI(TAG, "file list: %d files", count);

        bool pull_ok = true;
        for (int i = 0; i < count; i++) {
            if (names[i][0] == '\0') continue;

            int idx = ox_store_index_check(s_serial, names[i]);
            if (idx == 1) {
                ESP_LOGD(TAG, "skip '%s' (already finalised)", names[i]);
                continue;
            }

            ESP_LOGI(TAG, "pulling file %d/%d: '%s'", i + 1, count, names[i]);
            if (oxyii_pull_file(names[i]) != ESP_OK) {
                ESP_LOGW(TAG, "pull failed for '%s'", names[i]);
                if (sd_storage_is_ready()) oximetry_canonical_migrate_legacy(s_serial);
                pull_ok = false;
                break;
            }
        }

        do_disconnect();
        if (pull_ok) {
            s_presence_served = true;
            s_served_at = xTaskGetTickCount();
            ESP_LOGI(TAG, "sync window served — no reconnect; ring powers off on its own");
        } else {
            ESP_LOGW(TAG, "sync incomplete — will retry this presence");
        }
        set_state(OX_STATUS_PAIRED);

        xSemaphoreGive(s_ops_mtx);
    }
}

/* ── Public API ────────────────────────────────────────────────────── */
esp_err_t oximeter_init(void)
{
    s_state_mtx = xSemaphoreCreateMutex();
    s_ops_mtx   = xSemaphoreCreateMutex();
    s_op_sem    = xSemaphoreCreateBinary();
    s_conn_sem  = xSemaphoreCreateBinary();
    s_resp_sem  = xSemaphoreCreateBinary();
    s_scan_done = xSemaphoreCreateBinary();
    if (!s_state_mtx || !s_ops_mtx || !s_op_sem || !s_conn_sem ||
        !s_resp_sem || !s_scan_done)
        return ESP_ERR_NO_MEM;

    load_paired_from_nvs();
    ox_store_ensure_dirs();
    if (sd_storage_is_ready()) {
        oximetry_canonical_ensure_dirs();
        oximetry_canonical_reconcile();
        oximetry_canonical_migrate_all_legacy();
    }

    if (s_paired)
        set_state(OX_STATUS_PAIRED);

    TaskHandle_t h = psram_task_create(pull_task, "ox_pull", 8192, NULL, 3,
                                       tskNO_AFFINITY, NULL, NULL);
    if (!h) {
        ESP_LOGW(TAG, "failed to create pull task");
    }
    return ESP_OK;
}

esp_err_t oximeter_scan(int timeout_sec)
{
    if (!as11_ble_is_host_ready())
        return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_ops_mtx, 0) != pdTRUE)
        return ESP_ERR_INVALID_STATE;

    s_scan_count = 0;
    set_state(OX_STATUS_SCANNING);

    struct ble_gap_disc_params dp = {
        .itvl = 96,
        .window = 96,
        .filter_policy = 0,
        .limited = 0,
        .passive = 0,
    };

    uint8_t own_addr_type;
    int rc = ble_hs_id_infer_auto(BLE_ADDR_RANDOM, &own_addr_type);
    if (rc != 0) own_addr_type = as11_ble_get_own_addr_type();

    rc = ble_gap_disc(own_addr_type,
                      timeout_sec * 1000, &dp, gap_event, NULL);
    if (rc != 0) {
        set_error("scan start failed: %d", rc);
        xSemaphoreGive(s_ops_mtx);
        return ESP_FAIL;
    }

    xSemaphoreTake(s_scan_done, pdMS_TO_TICKS((timeout_sec + 2) * 1000));

    if (s_paired)
        set_state(OX_STATUS_PAIRED);
    else
        set_state(OX_STATUS_IDLE);

    xSemaphoreGive(s_ops_mtx);
    return ESP_OK;
}

cJSON *oximeter_get_scan_results(void)
{
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < s_scan_count; i++) {
        cJSON *e = cJSON_CreateObject();
        char addr_str[18];
        addr_to_str(&s_scan[i].addr, addr_str, sizeof(addr_str));
        cJSON_AddStringToObject(e, "addr", addr_str);
        cJSON_AddStringToObject(e, "name", s_scan[i].name);
        cJSON_AddNumberToObject(e, "rssi", s_scan[i].rssi);
        cJSON_AddItemToArray(arr, e);
    }
    return arr;
}

esp_err_t oximeter_pair(const char *addr_str)
{
    if (!as11_ble_is_host_ready())
        return ESP_ERR_INVALID_STATE;

    char *addr_copy = strdup(addr_str);
    if (!addr_copy) return ESP_ERR_NO_MEM;

    TaskHandle_t h = psram_task_create(pair_task, "ox_pair", 8192, addr_copy, 5,
                                       tskNO_AFFINITY, NULL, NULL);
    if (!h) {
        free(addr_copy);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t oximeter_forget(void)
{
    s_paired = false;
    s_presence_served = false;
    s_serial[0] = '\0';
    s_firmware[0] = '\0';
    s_name_prefix[0] = '\0';
    s_paired_addr[0] = '\0';

    nvs_writer_run(do_erase_nvs, NULL);
    ox_store_delete_paired();

    set_state(OX_STATUS_IDLE);
    return ESP_OK;
}

const char *oximeter_get_status(void)
{
    return s_status;
}

const char *oximeter_get_error(void)
{
    return s_error;
}

bool oximeter_is_paired(void)
{
    return s_paired;
}

cJSON *oximeter_get_paired_info(void)
{
    if (!s_paired) return NULL;
    cJSON *info = cJSON_CreateObject();
    cJSON_AddStringToObject(info, "serial", s_serial);
    if (s_firmware[0]) cJSON_AddStringToObject(info, "firmware", s_firmware);
    if (s_paired_addr[0]) cJSON_AddStringToObject(info, "addr", s_paired_addr);
    return info;
}
