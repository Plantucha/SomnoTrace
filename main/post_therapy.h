/*
 * SomnoTrace - Post-therapy data collection from AS11 spools and RPC
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

/* ── Post-therapy data collection ─────────────────────────────────────
 *
 * After therapy stops, the AS11 device has internal spools containing
 * session summary data (for STR.edf) and respiratory event logs (for
 * EVE.edf).  Additionally, device identification fields (serial number,
 * model, variant) are needed for EDF headers and Identification.json.
 *
 * This module orchestrates the collection of all post-therapy data:
 *
 *   Summary spool (30-day lookback):
 *     → decoded per-day → .sessions/summaries/YYYYMMDD.spool
 *
 *   TherapyEvents-RespiratoryEvents spool:
 *     → .sessions/streams/YYYYMMDD/<prefix>_resp_events.bin
 *
 *   Device identification (Get RPC):
 *     → .sessions/streams/YYYYMMDD/<prefix>_ident.json
 *
 *   Current settings (Get RPC):
 *     → .sessions/streams/YYYYMMDD/<prefix>_settings.json
 *
 * post_therapy_collect() is blocking and should be called from a task
 * with adequate stack (8KB+).  It is invoked by stop_task after
 * session_writer_stop() has finalised the stream files.
 *
 * Parameters:
 *   session_dir    - path to the noon-day folder (e.g. ".sessions/streams/20260627")
 *   file_prefix    - session file prefix (e.g. "20260627_023000")
 *   start_epoch_ms - session start time in epoch ms (unused for lookback;
 *                    30-day fixed window is used instead)
 *   clock_drift_ms - NTP time - AS11 device time (positive = AS11 is behind).
 *                    Saved to manifest.json for use by EDF generation.
 */
esp_err_t post_therapy_collect(const char *session_dir, const char *file_prefix,
                               int64_t start_epoch_ms, int64_t clock_drift_ms);
