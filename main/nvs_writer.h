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

#pragma once

#include "esp_err.h"

/*
 * A task with a PSRAM-backed stack cannot itself perform SPI-flash writes: the
 * flash routine runs with the cache disabled on its own core, and accessing
 * its PSRAM stack mid-operation faults. (Other PSRAM-stack tasks are protected
 * by CONFIG_SPI_FLASH_AUTO_SUSPEND, but the task *executing* the write is not.)
 *
 * To let the httpd worker run on a PSRAM stack (task_caps = MALLOC_CAP_SPIRAM)
 * while its handlers still persist settings, all NVS writes are funnelled to
 * this single task, which is created with an ordinary internal-RAM stack. The
 * submitting task blocks until the write completes and receives its result, so
 * HTTP responses still reflect success/failure synchronously.
 */

/* Callback executed on the nvs_writer task. Must perform only the flash write
 * (e.g. nvs_open/set/commit); do any parsing/allocation in the caller. */
typedef esp_err_t (*nvs_writer_fn_t)(void *arg);

/* Create the writer task + queue. Idempotent. MUST be called before any task
 * with a PSRAM stack (e.g. the httpd worker) can invoke nvs_writer_run(). */
void nvs_writer_init(void);

/* Run fn(arg) on the internal-stack writer task and block until it finishes,
 * returning fn's result. If the writer is not yet initialised (early boot),
 * fn runs inline on the calling task (which at that point has an internal
 * stack), so this is always safe to call. */
esp_err_t nvs_writer_run(nvs_writer_fn_t fn, void *arg);
