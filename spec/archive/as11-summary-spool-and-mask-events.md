# AS11 Summary Spool SessionModeEntries & Mask Event Analysis

- **Status:** Implemented
- **Author(s):** Cascade (AI assistant), Ilya Kruchinin
- **Created:** 2026-07-05
- **Last updated:** 2026-07-05
- **Related specs:** 0002-edf-export.md

## 1. Summary

Findings from comparing firmware-generated EDF files against AS11 native
exports. Covers: the true meaning of Summary spool `SessionModeEntries`
field 2 (per-session duration, not therapy mode), MaskOff calculation,
pressure stabilisation signalling via RPC events, and `_SNC` variable
tracking for Summary spool update detection.

## 2. SessionModeEntries — "mode" field is per-session duration

### Discovery

The Summary spool protobuf field 6 (`SessionModeEntries`) contains
repeated wrapper submessages, each with:

- **Sub-field 1** (varint): MaskOn timestamp (epoch ms, rounded to minute)
- **Sub-field 2** (varint): **Per-session duration in minutes** (NOT therapy mode)

The firmware originally interpreted sub-field 2 as a therapy mode value.
This produced garbage Mode values in STR.edf because the values (1, 2, 149,
444, etc.) are actually session durations in minutes.

### Verification

Compared all 18 session entries from the Jun 29 spool record and the
single entry from Jul 4 against the AS11's own STR.edf MaskOn/MaskOff
values. **100% match** across all entries:

```
MaskOff = MaskOn + duration   (when duration > 0)
MaskOff = -1 (sentinel)       (when duration == 0)
```

### Spool timestamp reference

Summary spool timestamps (PeriodStart, PeriodEnd, ClockB, session entry ts)
are in **NTP-synced UTC epoch milliseconds**, not the AS11's internal
drifting clock. Verified by matching ClockB exactly to events.snt
TherapyStop timestamps. The `clock_drift_ms` (from GetDateTime RPC) applies
only to the AS11's internal clock display, not to spool data.

## 3. MaskOff in STR.edf

### Problem

The Summary spool does not contain explicit MaskOff timestamps. Only
MaskOn timestamps and per-session durations are available.

### Solution

MaskOff is **reliably calculated** as `MaskOn + duration` from the spool's
SessionModeEntries. This matches the AS11's own STR.edf exactly.

The firmware's previous approach (`DurationMin / SessionCount` with a
`mode != 0` check) was wrong for multi-session days because it assumed
equal session durations. The fix uses the per-session duration directly
from each session entry.

### Current-day fallback

For the current (incomplete) day where no Summary spool exists yet,
MaskOff is derived from `events.snt` (live EventNotification messages).
The firmware receives `MaskOff` events via the `UsageEvents-TherapyStatusEvents`
subscription and writes them to `events.snt`. If no MaskOff event is
received (short sessions), `TherapyStop` is used as a fallback.

## 4. Pressure Stabilisation Signal

### Available events

The `SystemActivityEvents-FrequentActivityEvents` event selector
(provided by `SubscribeEvent`) includes:

- **`PressureStart`** — pressure begins ramping
- **`PressureStop`** — pressure stops (~15s after TherapyStop)
- `TherapyStarted`, `CooldownStarted`, `StandbyStarted`
- `WarmupStarted`, `RampDownStarted`, `RampDownCompleted`

### AS11 BRP recording start

The AS11 starts BRP.edf recording approximately at TherapyStart time
(~3 minutes before MaskOn). The firmware was starting recording earlier
(~7.5 minutes before TherapyStart) due to BLE connection + stream setup
overhead, capturing idle pre-therapy data.

### Fix

Subscribe to `SystemActivityEvents-FrequentActivityEvents` to receive
`PressureStart` events. Use `PressureStart` as the signal to begin
recording BRP/PLD data. Discard stream data received before
`PressureStart`. SA2 (oximetry) data is recorded from session start
since it comes from the external O2 Ring, not the AS11.

## 5. _SNC Variable for Summary Update Detection

### Background

`_SNC` (RPC variable, var_reference.tsv line 711) is a counter that
increments when the AS11 updates its Summary spool data (nor:1:/Summary.bin).
After TherapyStop, the AS11 takes a few seconds to finalise and write the
updated Summary.

### Previous approach

The firmware polled the Summary spool every 3 seconds for up to 2 minutes,
checking ClockB (field 40) against the session end time. This works but
is inefficient — each poll is a full PullSpoolFragments RPC round.

### Implemented approach

`_SNC` can be used as a `dataId` in `SubscribeEvent`. The AS11 pushes an
`EventNotification` with `dataId":"_SNC"` and `event":"ValueChange"` when
the counter increments — no polling required.

Example notification (received ~230ms after TherapyStop):
```json
{"jsonrpc":"2.0","method":"EventNotification","params":{
  "subscriptionId":3,"dataId":"_SNC",
  "events":[{"reportTime":"2026-07-02T10:46:10.230Z",
             "event":"ValueChange","value":247}]
}}
```

Implementation:
1. `SubscribeEvent` includes `"_SNC"` as a fourth dataId (alongside the
   three event-profile selectors)
2. `session_writer_on_notification` detects `dataId":"_SNC"` + `ValueChange`
   and sets a module-level flag (`s_snc_changed`) with the new value
3. `post_therapy_wait_spool_current` checks `session_writer_snc_changed()`
   every 3 seconds (just a flag check — no BLE RPC) and pulls the spool
   when the flag is set
4. Fallback: if no `_SNC` notification arrives within 2 minutes (e.g.
   subscription not accepted, BLE dropped notification), pulls the spool
   blindly on the last attempt and proceeds with available data

## 6. STR.edf Mode field

### Fix (already applied)

Mode is derived from `ActiveTherapyProfile` in settings.json (via
`profile_name_to_mop()` → `MODE_MAP[]`), not from SessionModeEntries.
This matches the AS11's own export behavior, which uses the active
therapy profile (MOP setting) for the Mode field.

## 7. Acceptance criteria

- [x] Mode derived from settings.json ActiveTherapyProfile
- [x] MaskOff calculated as MaskOn + per-session duration from spool
- [x] SessionModeEntries sub-field 2 documented as duration_min
- [x] SystemActivityEvents-FrequentActivityEvents subscription added
- [x] BRP recording starts at PressureStart, not at stream start
- [x] _SNC polling for Summary update detection
- [x] Fallback timeout retained (2 min)

## 8. Changelog

- 2026-07-05: Initial document. Captures findings from AS11 data stream
  investigation and code fixes.
- 2026-07-05: Added section 9 (unresolved items) and clarified _SNC polling.
- 2026-07-05: Switched _SNC from Get RPC polling to SubscribeEvent push
  notifications (confirmed by other developer's capture of _SNC
  ValueChange EventNotification).

## 9. Unresolved items — to revisit

These items remain unreliable or unverified after the current fixes.
They should be revisited when more data or AS11 protocol knowledge is
available.

### 9.1 BRP start timestamp offset

The firmware starts BLE stream subscriptions at reconnect time, before
`PressureStart` arrives. BRP/PLD recording is now gated on
`pressure_started`, but there is still a small latency between the AS11
internally recording `PressureStart` and the firmware receiving the
`EventNotification` (~1-2 seconds due to BLE notification dispatch).
This means the first BRP sample timestamp may be off by 1-2 seconds
compared to the AS11's own BRP.edf.

**Impact:** Low — BRP data itself is correct, only the absolute start
timestamp shifts by a few seconds.

**Possible fix:** None practical without deeper AS11 internal timing
knowledge. The AS11 does not expose its internal recording start time
via RPC.

### 9.2 Current-day MaskOff (live, before spool pull)

For the current (incomplete) day where no Summary spool has been pulled
yet, MaskOff is derived from `events.snt` live event notifications.
The firmware maps `MaskOff` → MaskOff and `TherapyStop` → MaskOff
(fallback). If the AS11 doesn't send a `MaskOff` event (very short
sessions), `TherapyStop` is used, which may differ by a few seconds
from the AS11's own MaskOff value.

**Impact:** Low — the current-day STR record is overwritten when the
Summary spool is pulled after therapy stop (which uses the verified
`MaskOn + duration_min` calculation). The live STR record is only
visible briefly between therapy stop and EDF regeneration.

### 9.3 STR.edf Date (noon boundary timezone)

The noon-day boundary uses `localtime_r` (firmware's configured
timezone). If the firmware's timezone differs from the AS11's
configured timezone, the day boundary could shift, causing a session
to appear on the wrong day in STR.edf.

**Impact:** Config-dependent. If both devices use the same timezone,
this is correct.

**Possible fix:** Read the AS11's timezone setting via `Get` RPC and
use it for noon-day calculations. Low priority since timezone is
typically set correctly on both devices.

### 9.4 _SNC subscription acceptance not verified

The `_SNC` variable is used as a `dataId` in `SubscribeEvent`, confirmed
by another developer's capture of `_SNC` `ValueChange` push notifications.
However, we have not yet verified that the AS11 accepts `"_SNC"` as a
dataId in the same `SubscribeEvent` call alongside the three event-profile
selectors. If the AS11 rejects it (returns `valid: false` for `"_SNC"`),
the firmware will not receive push notifications and will fall back to the
2-minute timeout + blind spool pull.

**Impact:** Low — the 2-minute fallback ensures the spool is pulled
regardless. The push notification just makes it faster (~230ms vs 2 min).

**To verify:** Check the `SubscribeEvent` response for the `valid` flag
associated with `"_SNC"`. Currently the firmware logs the response but
does not parse per-selector `valid` flags.

### 9.5 PressureStart event timing vs AS11 BRP start

The AS11's own BRP.edf recording starts at a specific internal trigger
(presumably `PressureStart`). The firmware now gates on the same event,
but the AS11 may start recording a fixed number of samples before or
after the `PressureStart` notification is dispatched. This has not been
verified by comparing exact first-sample timestamps.

**Impact:** Low — at most a few samples difference at the start of
BRP.edf.

**To verify:** Compare first BRP sample timestamp from firmware-generated
EDF vs AS11-exported EDF for the same session.

### 9.6 PressureStop event not used for recording stop

Currently, BRP/PLD recording stops when `TherapyStop` is received (via
`session_writer_stop`). The `PressureStop` event arrives ~15 seconds
after `TherapyStop`. The AS11 may stop BRP recording at `PressureStop`
rather than `TherapyStop`, meaning the firmware could be recording
~15 seconds of extra data at the end of the session.

**Impact:** Low — extra data at the end is harmless (EDF generation
trims to session duration). But if the AS11 stops at `PressureStop`,
the firmware's BRP.edf could have ~15 seconds more data than the AS11's.

**To verify:** Compare last BRP sample timestamp from firmware-generated
EDF vs AS11-exported EDF.

### 9.7 Events.snt for current day — PressureStart/Stop not written

`PressureStart` and `PressureStop` events are detected by
`check_event_notification` and used to set/clear `s_pressure_started`,
but they are not explicitly written to `events.snt` (they fall through
to the "Other events" path at line 1245 of session_writer.c, which does
write them if a session is active). This should be fine, but it's worth
verifying that these events are actually logged for debugging purposes.

**Impact:** None for EDF generation — PressureStart/Stop are not used
in STR.edf or EVE.edf. Only useful for post-hoc debugging.
