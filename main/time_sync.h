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
 * Reads GMT offset from NVS, configures timezone, starts SNTP in poll mode
 * (1-hour re-sync interval). Tries DHCP option 42 servers first, falls back
 * to public NTP pools. */
esp_err_t time_sync_init(void);

/* Get/set GMT offset (hours, -12..+14) stored in NVS. */
int time_sync_get_gmt_offset(void);
esp_err_t time_sync_set_gmt_offset(int gmt_off);

/* Returns true if system time has been synchronised via NTP. */
bool time_sync_is_synced(void);
