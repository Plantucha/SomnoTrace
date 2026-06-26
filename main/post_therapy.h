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
 * This module orchestrates the collection of all post-therapy data and
 * saves it to a "post-therapy" subfolder inside the session directory:
 *
 *   /somnotrace/sessions/<session_id>/
 *     brp.snt, sa2.snt, pld.snt, brp_mm.snt   ← stream data (live)
 *     events.jsonl                             ← live events
 *     session.json                             ← session metadata
 *     post-therapy/                            ← THIS MODULE writes here
 *       summary.bin                            ← raw Summary spool protobuf
 *       respiratory_events.bin                 ← raw TherapyEvents spool protobuf
 *       identification.json                    ← device identity (from Get RPC)
 *       settings.json                          ← current settings (from Get RPC)
 *
 * The combination of stream .snt files + post-therapy/ data constitutes
 * the complete "data warehouse" for the session.  EDF generation (edf_gen)
 * runs as a separate step AFTER this collection completes, writing to
 * /somnotrace/EDF/ (outside the sessions directory).
 *
 * post_therapy_collect() is blocking and should be called from a task
 * with adequate stack (8KB+).  It is invoked by stop_task after
 * session_writer_stop() has finalised the stream files.
 *
 * Parameters:
 *   session_dir    - path to the session directory (e.g. "/somnotrace/sessions/20260627_0230")
 *   start_epoch_ms - session start time in epoch ms (for spool fromDateTime)
 *   clock_drift_ms - NTP time - AS11 device time (positive = AS11 is behind).
 *                    Saved to manifest.json for use by EDF generation.
 */
esp_err_t post_therapy_collect(const char *session_dir, int64_t start_epoch_ms,
                               int64_t clock_drift_ms);
