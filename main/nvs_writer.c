/*
 * SomnoTrace - Centralised NVS/flash writer task
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 *
 * This file is part of SomnoTrace.
 *
 * SomnoTrace is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * SomnoTrace is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
 * A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 *
 * ADDITIONAL TERM (GPLv3 Section 7(b)): Redistributions must preserve the
 * attribution "Based on SomnoTrace, originally created by Ilya Kruchinin
 * (https://github.com/ilyakruchinin)." See the NOTICE file for details.
 */

#include "nvs_writer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"

static const char *TAG = "nvs_writer";

/* Internal-RAM stack: this task performs the actual flash writes, so its own
 * stack must not be in PSRAM. NVS commits fit comfortably in 4 KB. */
#define NVS_WRITER_STACK   4096
#define NVS_WRITER_PRIO    6      /* >= httpd worker so a submitted write runs promptly */

typedef struct {
    nvs_writer_fn_t fn;
    void           *arg;
    esp_err_t       result;
} nvs_cmd_t;

static QueueHandle_t     s_cmd_q      = NULL;   /* holds nvs_cmd_t* */
static SemaphoreHandle_t s_submit_mtx = NULL;   /* serialises submitters */
static SemaphoreHandle_t s_done       = NULL;   /* writer -> submitter completion */

static void nvs_writer_task(void *arg)
{
    (void)arg;
    for (;;) {
        nvs_cmd_t *cmd = NULL;
        if (xQueueReceive(s_cmd_q, &cmd, portMAX_DELAY) == pdTRUE && cmd) {
            cmd->result = cmd->fn ? cmd->fn(cmd->arg) : ESP_ERR_INVALID_ARG;
            xSemaphoreGive(s_done);
        }
    }
}

void nvs_writer_init(void)
{
    if (s_cmd_q) return;   /* already initialised */

    s_submit_mtx = xSemaphoreCreateMutex();
    s_done       = xSemaphoreCreateBinary();
    s_cmd_q      = xQueueCreate(1, sizeof(nvs_cmd_t *));
    if (!s_submit_mtx || !s_done || !s_cmd_q) {
        ESP_LOGE(TAG, "alloc failed (mtx=%p done=%p q=%p)",
                 s_submit_mtx, s_done, s_cmd_q);
        s_cmd_q = NULL;   /* force inline fallback in nvs_writer_run() */
        return;
    }

    if (xTaskCreatePinnedToCore(nvs_writer_task, "nvs_writer", NVS_WRITER_STACK,
                                NULL, NVS_WRITER_PRIO, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "task create failed — falling back to inline writes");
        s_cmd_q = NULL;
        return;
    }
    ESP_LOGI(TAG, "nvs_writer task started (internal stack=%d)", NVS_WRITER_STACK);
}

esp_err_t nvs_writer_run(nvs_writer_fn_t fn, void *arg)
{
    if (!fn) return ESP_ERR_INVALID_ARG;

    /* Not yet initialised (early boot): the caller still has an internal-RAM
     * stack at this point, so running the write inline is safe. */
    if (!s_cmd_q) return fn(arg);

    xSemaphoreTake(s_submit_mtx, portMAX_DELAY);
    nvs_cmd_t cmd = { .fn = fn, .arg = arg, .result = ESP_FAIL };
    nvs_cmd_t *p = &cmd;
    xQueueSend(s_cmd_q, &p, portMAX_DELAY);
    xSemaphoreTake(s_done, portMAX_DELAY);
    esp_err_t r = cmd.result;
    xSemaphoreGive(s_submit_mtx);
    return r;
}
