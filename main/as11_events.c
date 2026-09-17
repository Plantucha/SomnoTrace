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
#include "as11_events.h"

#include <string.h>

/* Check an EventNotification for therapy start/stop and lifecycle events. */
as11_event_t check_event_notification(const cJSON *msg, const char **out_report)
{
    if (out_report) *out_report = NULL;

    cJSON *params = cJSON_GetObjectItem(msg, "params");
    if (!params) return AS11_EV_NONE;

    cJSON *events = cJSON_GetObjectItem(params, "events");
    if (!events || !cJSON_IsArray(events)) return AS11_EV_NONE;

    int n = cJSON_GetArraySize(events);
    for (int i = 0; i < n; i++) {
        cJSON *ev = cJSON_GetArrayItem(events, i);
        if (!ev) continue;
        cJSON *event = cJSON_GetObjectItem(ev, "event");
        if (!event || !cJSON_IsString(event)) continue;
        const char *name = event->valuestring;

        if (strcmp(name, "TherapyStart") == 0) {
            cJSON *rt = cJSON_GetObjectItem(ev, "reportTime");
            if (out_report && rt && cJSON_IsString(rt))
                *out_report = rt->valuestring;
            return AS11_EV_THERAPY_START;
        }
        if (strcmp(name, "TherapyStop") == 0) {
            return AS11_EV_THERAPY_STOP;
        }
        if (strcmp(name, "MaskFitStart") == 0 ||
            strcmp(name, "MaskfitStarted") == 0 ||
            strcmp(name, "LearnTargetsStart") == 0) {
            return AS11_EV_MASK_FIT_START;
        }
        if (strcmp(name, "MaskFitStop") == 0 ||
            strcmp(name, "LearnTargetsStop") == 0) {
            return AS11_EV_MASK_FIT_STOP;
        }
        if (strcmp(name, "CooldownStarted") == 0) {
            return AS11_EV_COOLDOWN_START;
        }
        if (strcmp(name, "CooldownStopped") == 0) {
            return AS11_EV_COOLDOWN_STOP;
        }
        if (strcmp(name, "StandbyStarted") == 0) {
            return AS11_EV_STANDBY_START;
        }
    }
    return AS11_EV_NONE;
}
