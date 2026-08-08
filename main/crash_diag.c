/*
 * SomnoTrace - Crash diagnostics (reset reason + core dump analysis)
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

#include "crash_diag.h"

#include <inttypes.h>
#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"

/* Core dump APIs are only available when core dump is enabled in sdkconfig.
 * Guard with CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH so this file compiles even
 * if the feature is later disabled. */
#if defined(CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH)
#include "esp_core_dump.h"
#endif

static const char *TAG = "crash_diag";

/* Human-readable names for esp_reset_reason_t values.  Indices match the
 * enum defined in esp_system.h. */
static const char *reset_reason_str(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:   return "POWERON";
    case ESP_RST_EXT:       return "EXT_PIN";
    case ESP_RST_SW:        return "SOFTWARE";
    case ESP_RST_PANIC:     return "PANIC";
    case ESP_RST_INT_WDT:   return "INT_WDT";
    case ESP_RST_TASK_WDT:  return "TASK_WDT";
    case ESP_RST_WDT:       return "OTHER_WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT:  return "BROWNOUT";
    case ESP_RST_SDIO:      return "SDIO";
    case ESP_RST_USB:       return "USB";
    default:                return "UNKNOWN";
    }
}

#if defined(CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH)
/**
 * If a valid core dump exists on flash, read its summary and log the crash
 * information (task name, exception PC, backtrace addresses, ELF SHA256).
 *
 * The backtrace addresses are raw hex PCs.  They can be decoded later with:
 *   xtensa-esp32s3-elf-addr2line -pfiaC -e build/somnotrace.elf 0xADDR ...
 * or:
 *   espcoredump.py info_corefile -t b64 build/somnotrace.elf coredump.bin
 *
 * After logging, the core dump partition is erased so the same crash is not
 * reported on every subsequent boot.
 */
static void log_coredump_summary(void)
{
    /* Check whether a valid core dump image exists on flash. */
    if (esp_core_dump_image_check() != ESP_OK) {
        ESP_LOGW(TAG, "core dump image check failed — partition may be empty, "
                      "corrupt, or erased by bootloader (CHECK_BOOT)");
        return;
    }

    esp_core_dump_summary_t *summary = malloc(sizeof(esp_core_dump_summary_t));
    if (!summary) {
        ESP_LOGW(TAG, "core dump exists but malloc failed for summary struct");
        esp_core_dump_image_erase();
        return;
    }

    esp_err_t err = esp_core_dump_get_summary(summary);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "core dump exists but get_summary failed: 0x%x", (unsigned)err);
        free(summary);
        esp_core_dump_image_erase();
        return;
    }

    /* ── Log the crash summary ──────────────────────────────────────── */

    ESP_LOGW(TAG, "=== CRASH DETECTED (previous boot) ===");
    ESP_LOGW(TAG, "  Crashed task : %s", summary->exc_task);
    ESP_LOGW(TAG, "  Exception PC : 0x%08" PRIx32, summary->exc_pc);

    /* Backtrace (architecture-specific).  On Xtensa (ESP32-S3), the
     * exc_bt_info struct contains an array of PC values and a depth. */
    if (summary->exc_bt_info.depth > 0) {
        /* Build a single-line backtrace string for easy log parsing.
         * Max depth is typically 16 frames × 11 chars ("0xABCDEF01 ") ≈ 176. */
        char bt_line[256];
        int pos = 0;
        for (uint32_t i = 0; i < summary->exc_bt_info.depth && pos < (int)sizeof(bt_line) - 12; i++) {
            pos += snprintf(bt_line + pos, sizeof(bt_line) - pos,
                            "0x%08" PRIx32 " ", summary->exc_bt_info.bt[i]);
        }
        if (pos > 0 && bt_line[pos - 1] == ' ') bt_line[pos - 1] = '\0';
        ESP_LOGW(TAG, "  Backtrace    : %s", bt_line);
        ESP_LOGW(TAG, "  BT depth     : %" PRIu32, summary->exc_bt_info.depth);
    } else {
        ESP_LOGW(TAG, "  Backtrace    : (none available)");
    }

    /* ELF SHA256 — allows matching the crash to a specific firmware build.
     * The first 8 bytes (printed as 16 hex chars) are usually sufficient. */
    char sha_str[17];
    for (int i = 0; i < 8; i++) {
        snprintf(sha_str + i * 2, 3, "%02x", summary->app_elf_sha256[i]);
    }
    ESP_LOGW(TAG, "  ELF SHA256   : %s...", sha_str);

    ESP_LOGW(TAG, "=== END CRASH REPORT ===");

    free(summary);

    /* Erase the core dump so it is not reported again on subsequent boots. */
    esp_core_dump_image_erase();
    ESP_LOGI(TAG, "core dump partition erased");
}
#endif /* CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH */

void crash_diag_check(void)
{
    /* ── Always log the reset reason ─────────────────────────────────── */
    esp_reset_reason_t reason = esp_reset_reason();
    bool is_crash = (reason == ESP_RST_PANIC   ||
                     reason == ESP_RST_INT_WDT  ||
                     reason == ESP_RST_TASK_WDT ||
                     reason == ESP_RST_WDT);

    if (is_crash) {
        ESP_LOGW(TAG, "reset reason: %s (%d)", reset_reason_str(reason), (int)reason);
    } else {
        ESP_LOGI(TAG, "reset reason: %s (%d)", reset_reason_str(reason), (int)reason);
    }

    /* ── Log boot-time heap stats ────────────────────────────────────── */
    ESP_LOGI(TAG, "[heap] boot: internal free=%u, PSRAM free=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    /* ── Core dump analysis (only if the feature is enabled) ─────────── */
#if defined(CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH)
    if (is_crash) {
        log_coredump_summary();
    }
#endif
}
