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

#pragma once

/**
 * Check the reset reason and, if a crash occurred, extract the core dump
 * summary (task name, exception PC, backtrace) and log it.
 *
 * Should be called once, early in app_main() — after log_stream_init()
 * so the output is captured in the ring buffer, but before any other
 * subsystem that might itself crash.
 *
 * The core dump is erased after its summary is logged, so the information
 * appears only once in the first boot after the crash.
 */
void crash_diag_check(void);
