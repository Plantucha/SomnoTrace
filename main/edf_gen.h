/*
 * SomnoTrace - EDF file generation from session data warehouse
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

/* ── EDF file generation ──────────────────────────────────────────────
 *
 * This module generates EDF (European Data Format) files from the session
 * data warehouse, producing output compatible with OSCAR and SleepHQ.
 *
 * The data warehouse consists of:
 *   /somnotrace/sessions/<session_id>/
 *     brp.snt, sa2.snt, pld.snt, brp_mm.snt   ← stream data (live, .snt format)
 *     events.jsonl                             ← live events
 *     session.json                             ← session metadata
 *     post-therapy/
 *       summary.bin                            ← raw Summary spool protobuf
 *       respiratory_events.bin                 ← raw TherapyEvents spool protobuf
 *       identification.json                    ← device identity (from Get RPC)
 *       settings.json                          ← current settings (from Get RPC)
 *
 * EDF output goes to a COMPLETELY SEPARATE directory:
 *   /somnotrace/EDF/<session_id>/
 *     DATALOG/
 *       STR.edf    ← Session summary + settings (from Summary spool)
 *       BRP.edf    ← Breath waveform 25 Hz (from brp.snt)
 *       PLD.edf    ← Per-breath stats 0.5 Hz (from pld.snt)
 *       SA2.edf    ← SpO2/pulse 1 Hz (from sa2.snt)
 *       EVE.edf    ← Respiratory event annotations (from respiratory_events.bin)
 *       CSL.edf    ← Cheyne-Stokes annotations (from Summary spool CSR field)
 *     Identification.json
 *     Identification.crc
 *
 * This function is blocking and should be called from a task with adequate
 * stack (8KB+).  It must be called AFTER post_therapy_collect() has completed,
 * so that all spool and RPC data is available.
 *
 * Parameters:
 *   session_dir    - path to the session directory
 *   session_id     - session identifier (e.g. "20260627_0230")
 *   start_epoch_ms - session start time in epoch ms (NTP-corrected)
 *   end_epoch_ms   - session end time in epoch ms (0 if unknown)
 *   clock_drift_ms - NTP time - AS11 device time (positive = AS11 is behind).
 *                    Applied to spool-sourced timestamps (Summary, events)
 *                    to correct for AS11 clock skew.  Stream .snt data is
 *                    already NTP-timestamped and needs no adjustment.
 */
esp_err_t edf_gen_generate(const char *session_dir, const char *session_id,
                           int64_t start_epoch_ms, int64_t end_epoch_ms,
                           int64_t clock_drift_ms);
