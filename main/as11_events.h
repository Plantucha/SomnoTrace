/*
 * SomnoTrace - AS11 EventNotification classification
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
 * attribution "Based on SomnoTrace, originally created by Ilya Kruchinin".
 */
#pragma once

/*
 * WHAT AN AS11 EventNotification MEANS, and nothing else.
 *
 * A separate translation unit with no ESP-IDF dependency beyond cJSON, for the
 * same reason as as11_reconnect.c: the classifier used to be static inside
 * session_writer.c, a ~3000-line file that pulls in FreeRTOS, the SD stack and
 * the uploader, so it could not be linked by a host test. scripts/
 * as11_events_test.c therefore carried a hand-copied duplicate of the enum and
 * the parser and asserted against that.
 *
 * A test over a copy cannot fail for a change to the shipped parser: add an
 * event name here, reorder a branch, or fix a bug, and the suite still passes
 * green against the stale duplicate while reading as coverage. Distinguishing
 * "Mask Fit" and tube-drying Cooldown from real therapy is exactly the decision
 * that must not silently drift, since a misclassification writes a session that
 * never happened.
 *
 * The taxonomy and the parser now live here, and the test links this file.
 */

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Event types recognized in an AS11 EventNotification. */
typedef enum {
    AS11_EV_NONE = 0,
    AS11_EV_THERAPY_START,
    AS11_EV_THERAPY_STOP,
    AS11_EV_MASK_FIT_START,
    AS11_EV_MASK_FIT_STOP,
    AS11_EV_COOLDOWN_START,
    AS11_EV_COOLDOWN_STOP,
    AS11_EV_STANDBY_START,
} as11_event_t;

/* Classify an EventNotification message.
 *
 * Returns the first recognised event in params.events, or AS11_EV_NONE when the
 * message carries none. On AS11_EV_THERAPY_START, *out_report (when non-NULL)
 * receives that event's "reportTime" string, borrowed from msg and valid only
 * while msg lives; it is set to NULL in every other case.
 */
as11_event_t check_event_notification(const cJSON *msg, const char **out_report);

#ifdef __cplusplus
}
#endif
