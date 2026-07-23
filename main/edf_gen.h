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
 * This module generates EDF (European Data Format) files from session
 * data, producing output compatible with OSCAR and SleepHQ.
 *
 * Input data lives under the SomnoTrace app-root (ESP-native):
 *   /somnotrace/.somnotrace/sessions/streams/YYYYMMDD/
 *     <prefix>_brp.snt, _sa2.snt, _pld.snt, _brp_mm.snt  ← stream data
 *     <prefix>_events.snt                                 ← live events
 *     <prefix>_resp_events.bin                             ← TherapyEvents spool
 *     <prefix>_ident.json                                  ← device identity
 *     <prefix>_settings.json                               ← current settings
 *     <prefix>_session.json                                ← session metadata
 *   /somnotrace/.somnotrace/sessions/summaries/
 *     YYYYMMDD.spool                                       ← per-day Summary spool
 *
 * Output goes to SDCARD/ (ResMed-compatible SD card image, OSCAR-ready):
 *   /somnotrace/SDCARD/
 *     STR.edf                ← Multi-record daily summary (one record per day
 *                              from .somnotrace/sessions/summaries/ spool files, sorted chronologically)
 *     Identification.json    ← Device identity (nested AS11 format)
 *     Identification.crc     ← CRC-32 of Identification.json
 *     SETTINGS/
 *       CurrentSettings.json ← Latest settings snapshot
 *       CurrentSettings.crc  ← CRC-32 of CurrentSettings.json
 *     DATALOG/
 *       YYYYMMDD/            ← Noon-based day folder
 *         <prefix>_BRP.edf   ← Breath waveform 25 Hz (from brp.snt)
 *         <prefix>_PLD.edf   ← Per-breath stats 0.5 Hz (from pld.snt)
 *         <prefix>_SA2.edf   ← SpO2/pulse 1 Hz (from sa2.snt)
 *         <prefix>_EVE.edf   ← Respiratory event annotations
 *         <prefix>_CSL.edf   ← CSR event annotations
 *
 * SDCARD/ is fully derived from .somnotrace/sessions/ and can be deleted and regenerated
 * at any time without BLE access.
 *
 * This function is blocking and should be called from a task with adequate
 * stack (8KB+).  It must be called AFTER post_therapy_collect() has completed,
 * so that all spool and RPC data is available.
 *
 * Parameters:
 *   session_dir    - path to the noon-day stream folder (.somnotrace/sessions/streams/YYYYMMDD/)
 *   session_id     - session prefix (e.g. "20260627_224219" or "20260627_224219_2")
 *   start_epoch_ms - session start time in epoch ms (NTP-corrected)
 *   end_epoch_ms   - session end time in epoch ms (0 if unknown)
 *   clock_drift_ms - NTP time - AS11 device time (positive = AS11 is behind).
 *                    Applied to spool-sourced timestamps (Summary, events)
 *                    to correct for AS11 clock skew.  Stream .snt data is
 *                    already NTP-timestamped and needs no adjustment.
 */
/* Build a per-noon-day JSON summary (AHI/indices, usage, leak/pressure/EPAP/
 * resp-rate percentiles, session count) from that day's Summary spool, in
 * physical units matching STR.edf/OSCAR. On success returns ESP_OK and sets
 * *out_json to a malloc'd string (caller frees). Returns ESP_ERR_NOT_FOUND if
 * the day has no spool. noon_day is "YYYYMMDD". */
esp_err_t edf_gen_summary_json(const char *noon_day, char **out_json);

esp_err_t edf_gen_generate(const char *session_dir, const char *session_id,
                           int64_t start_epoch_ms, int64_t end_epoch_ms,
                           int64_t clock_drift_ms);
