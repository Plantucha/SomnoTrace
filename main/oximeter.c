/*
 * SomnoTrace - Oximeter multi-driver dispatcher
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
 *
 * The dispatcher loads the paired driver type from NVS at init and routes
 * public API calls to the appropriate driver (OxyII for Gen2, Legacy for
 * Gen1).  Both drivers are compiled in; only one is active at a time.
 * The scan function runs both drivers' scans and merges results so the
 * user can see all compatible rings in a single scan.
 */

#include "oximeter.h"
#include "oximeter_internal.h"

#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "nvs_writer.h"

#include <string.h>
#include <strings.h>

static const char *TAG = "oximeter";

#define OX_NVS_NS "oximeter"

static const ox_driver_ops_t *s_active = &oxyii_driver_ops;
static ox_driver_t s_driver_type = OX_DRIVER_OXYII;

/* Forward declarations for store functions */
bool ox_store_load_paired(char *serial, size_t serial_sz,
                          char *firmware, size_t fw_sz,
                          char *name_prefix, size_t prefix_sz,
                          char *last_addr, size_t addr_sz,
                          char *driver, size_t driver_sz,
                          char *ble_name, size_t ble_name_sz);

static void load_driver_type(void)
{
    /* Try NVS first */
    nvs_handle_t h;
    nvs_writer_lock();
    if (nvs_open(OX_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t drv;
        if (nvs_get_u8(h, "driver", &drv) == ESP_OK && drv <= OX_DRIVER_LEGACY)
            s_driver_type = (ox_driver_t)drv;
        nvs_close(h);
    }
    nvs_writer_unlock();

    /* Fall back to paired.json on SD */
    if (s_driver_type == OX_DRIVER_OXYII) {
        char drv[16] = {0};
        if (ox_store_load_paired(NULL, 0, NULL, 0, NULL, 0, NULL, 0,
                                 drv, sizeof(drv), NULL, 0)) {
            if (strcmp(drv, "wellue_legacy") == 0)
                s_driver_type = OX_DRIVER_LEGACY;
        }
    }

    s_active = (s_driver_type == OX_DRIVER_LEGACY)
        ? &legacy_driver_ops
        : &oxyii_driver_ops;

    ESP_LOGI(TAG, "active driver: %s",
             s_driver_type == OX_DRIVER_LEGACY ? "legacy" : "oxyii");
}

/* ── Public API — delegates to active driver ──────────────────────── */

esp_err_t oximeter_init(void)
{
    load_driver_type();
    oxyii_driver_ops.init();
    legacy_driver_ops.init();
    return ESP_OK;
}

/* Scan runs both drivers sequentially because each owns its GAP callback and
 * result buffer.  Results are merged by address after both scans complete. */
esp_err_t oximeter_scan(int timeout_sec)
{
    int each = timeout_sec > 1 ? timeout_sec / 2 : 1;
    esp_err_t oxyii = oxyii_driver_ops.scan(each);
    esp_err_t legacy = legacy_driver_ops.scan(each);
    return oxyii == ESP_OK || legacy == ESP_OK ? ESP_OK : ESP_FAIL;
}

static bool explicit_oxyii_name(const char *name)
{
    return name && (strncasecmp(name, "S8-AW", 5) == 0 ||
                    strncasecmp(name, "SHQO2PRO", 8) == 0);
}

cJSON *oximeter_get_scan_results(void)
{
    cJSON *merged = cJSON_CreateArray();
    cJSON *oxyii = oxyii_driver_ops.get_scan_results();
    cJSON *legacy = legacy_driver_ops.get_scan_results();
    if (!merged || !oxyii || !legacy) {
        if (merged) cJSON_Delete(merged);
        if (oxyii) cJSON_Delete(oxyii);
        if (legacy) cJSON_Delete(legacy);
        return cJSON_CreateArray();
    }
    cJSON *item;
    cJSON_ArrayForEach(item, oxyii) {
        cJSON *copy = cJSON_Duplicate(item, true);
        if (copy) cJSON_AddItemToArray(merged, copy);
    }
    cJSON_ArrayForEach(item, legacy) {
        cJSON *addr = cJSON_GetObjectItem(item, "addr");
        cJSON *name = cJSON_GetObjectItem(item, "name");
        int duplicate = -1;
        for (int i = 0; i < cJSON_GetArraySize(merged); i++) {
            cJSON *candidate = cJSON_GetArrayItem(merged, i);
            cJSON *candidate_addr = cJSON_GetObjectItem(candidate, "addr");
            if (cJSON_IsString(addr) && cJSON_IsString(candidate_addr) &&
                strcmp(addr->valuestring, candidate_addr->valuestring) == 0) {
                duplicate = i;
                break;
            }
        }
        if (duplicate >= 0 && !(cJSON_IsString(name) && explicit_oxyii_name(name->valuestring))) {
            cJSON *copy = cJSON_Duplicate(item, true);
            if (copy) cJSON_ReplaceItemInArray(merged, duplicate, copy);
        } else if (duplicate < 0) {
            cJSON *copy = cJSON_Duplicate(item, true);
            if (copy) cJSON_AddItemToArray(merged, copy);
        }
    }
    cJSON_Delete(oxyii);
    cJSON_Delete(legacy);
    return merged;
}

esp_err_t oximeter_pair(const char *addr_str, ox_driver_t driver)
{
    /* If pairing with a different driver type, switch active driver */
    if (driver != s_driver_type) {
        /* Forget the current driver's state */
        s_active->forget();

        /* Switch driver */
        s_driver_type = driver;
        s_active = (driver == OX_DRIVER_LEGACY)
            ? &legacy_driver_ops
            : &oxyii_driver_ops;

        /* Initialize the new driver if not already done */
        s_active->init();

        ESP_LOGI(TAG, "switched to driver: %s",
                 driver == OX_DRIVER_LEGACY ? "legacy" : "oxyii");
    }

    return s_active->pair(addr_str);
}

esp_err_t oximeter_forget(void)
{
    s_active->forget();
    return ESP_OK;
}

const char *oximeter_get_status(void)
{
    return s_active->get_status();
}

const char *oximeter_get_error(void)
{
    return s_active->get_error();
}

bool oximeter_is_paired(void)
{
    return s_active->is_paired();
}

cJSON *oximeter_get_paired_info(void)
{
    return s_active->get_paired_info();
}

ox_driver_t oximeter_get_driver(void)
{
    return s_driver_type;
}

ox_probe_mode_t oximeter_get_probe_mode(void)
{
    return s_active->get_probe_mode();
}

esp_err_t oximeter_set_probe_mode(ox_probe_mode_t mode)
{
    return s_active->set_probe_mode(mode);
}
