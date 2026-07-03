/*
 * SomnoTrace - Log stream: ring-buffered log capture with SSE delivery
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

#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialise the log stream subsystem.
 *
 * Allocates a ring buffer (PSRAM if available, otherwise internal RAM) and
 * installs a custom vprintf hook so every ESP_LOGx() call is captured.
 * Must be called once, before the HTTP server registers the log endpoints.
 */
void log_stream_init(void);

/**
 * Register the three log-related HTTP endpoints on the given server:
 *
 *   GET  /api/logs/stream   — SSE event stream (text/event-stream)
 *   GET  /api/logs/download  — plain-text download of buffered logs
 *   POST /api/logs/level     — change runtime log level (JSON body)
 */
void log_stream_register_handlers(httpd_handle_t server);

#ifdef __cplusplus
}
#endif
