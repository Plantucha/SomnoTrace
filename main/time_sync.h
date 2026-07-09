/*
 * SomnoTrace - NTP time synchronisation with DHCP option 42 support
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
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 *
 * ADDITIONAL TERM (GPLv3 Section 7(b)): Redistributions must preserve the
 * attribution "Based on SomnoTrace, originally created by Ilya Kruchinin
 * (https://github.com/ilyakruchinin)." See the NOTICE file for details.
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"

/* Initialise SNTP after Wi-Fi STA connects.
 * Reads timezone from NVS (POSIX TZ string), configures timezone, starts SNTP
 * in poll mode (1-hour re-sync interval). Tries DHCP option 42 servers first,
 * falls back to public NTP pools. Defaults to UTC if no timezone is set. */
esp_err_t time_sync_init(void);

/* Get/set timezone as POSIX TZ string and IANA name, stored in NVS.
 * tz_str: e.g. "AEST-10AEDT,M10.1.0,M4.1.0/3"
 * tz_name: e.g. "Australia/Melbourne" (for UI re-selection only) */
esp_err_t time_sync_set_timezone(const char *tz_str, const char *tz_name);
void time_sync_get_timezone(char *tz_str, size_t tz_str_len);
void time_sync_get_tz_name(char *tz_name, size_t tz_name_len);

/* Get/set custom NTP server hostname, stored in NVS.
 * Pass NULL or empty string to clear (revert to auto/DHCP mode).
 * When set, time_sync_init uses this server exclusively. */
esp_err_t time_sync_set_ntp_server(const char *server);
void time_sync_get_ntp_server(char *server, size_t server_len);

/* Returns true if system time has been synchronised via NTP. */
bool time_sync_is_synced(void);

/* Block until the initial NTP sync succeeds or all attempts are exhausted.
 * Makes up to 3 attempts with 15-second timeouts. Returns true on success.
 * Must be called after time_sync_init(). Subsequent periodic re-syncs do
 * not trigger the failure path. */
bool time_sync_wait_initial(void);
