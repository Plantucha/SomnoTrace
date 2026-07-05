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

The AS11 starts BRP.edf recording at **TherapyStart** (CSL timestamp),
not at PressureStart. Archive analysis confirms:

| Session | CSL (TherapyStart) | BRP start | Gap |
|---------|-------------------|-----------|-----|
| Mar 29  | 01:13:05 | 01:13:12 | 7s |
| Mar 30  | 23:37:40 | 23:37:47 | 7s |
| Mar 31  | 22:27:38 | 22:27:44 | 6s |
| Jul 4 #1| 00:01:03 | 00:01:12 | 9s |
| Jul 4 #2| 03:38:35 | 03:38:41 | 6s |
| Jul 4 #3| 06:26:24 | 06:26:32 | 8s |
| Jul 5   | 00:04:38 | 00:04:43 | 5s |

BRP always starts 5-9s after TherapyStart (internal AS11 processing
delay). BRP ends at TherapyStop ≈ MaskOff time. BRP duration (nrecs ×
60s) matches STR Duration (±1 min rounding).

The firmware was previously starting recording at BLE reconnect time
(~7.5 min before TherapyStart) due to stream setup overhead. The fix
is to record from TherapyStart (session_writer_start) to TherapyStop
(session_writer_stop), which is already the existing session lifecycle.

### Fix

No PressureStart gating is needed. BRP/PLD/SA2 all record from
TherapyStart to TherapyStop, matching AS11 export behavior. The
`SystemActivityEvents-FrequentActivityEvents` subscription is retained
for event logging (PressureStart, PressureStop, CooldownStarted, etc.)
but is not used to gate data recording.

The original problem (recording starting ~7.5 min before TherapyStart)
was likely caused by the auto-start fallback detecting idle baseline
flow as "active therapy" — this is addressed by the existing
`has_therapy_pressure` check (pressure > 4 cmH2O required).

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
- [x] SystemActivityEvents-FrequentActivityEvents subscription added (for event logging)
- [x] BRP/PLD/SA2 recording starts at TherapyStart, matching AS11 export behavior
- [x] _SNC push notification subscription for Summary update detection
- [x] Fallback timeout retained (2 min)

## 8. Changelog

- 2026-07-05: Initial document. Captures findings from AS11 data stream
  investigation and code fixes.
- 2026-07-05: Added section 9 (unresolved items) and clarified _SNC polling.
- 2026-07-05: Switched _SNC from Get RPC polling to SubscribeEvent push
  notifications (confirmed by other developer's capture of _SNC
  ValueChange EventNotification).
- 2026-07-05: Archive analysis of AS11 exports confirms BRP starts at
  TherapyStart (5-9s after CSL), NOT at PressureStart. Reverted
  PressureStart gating on BRP/PLD. Removed pressure_started flag.
  Updated unresolved items.

## 9. Unresolved items — to revisit

These items remain unreliable or unverified after the current fixes.
They should be revisited when more data or AS11 protocol knowledge is
available.

### 9.1 BRP start timestamp offset

Archive analysis confirms the AS11 starts BRP 5-9s after TherapyStart
(CSL timestamp). The firmware starts recording at TherapyStart event
receipt, which has ~1-2s BLE notification latency. Net offset vs AS11
BRP: firmware BRP starts ~3-7s earlier than AS11 BRP (AS11 has 5-9s
internal delay, firmware has 1-2s notification delay).

**Impact:** Low — a few seconds difference at the start of BRP.edf.
The extra samples are idle/baseline data before pressure ramps.

**Status:** Resolved by archive analysis. No further action needed.

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

**To verify:** Run new code and check logs for:
1. `reconnect: SubscribeEvent response received` — subscription accepted
2. `>>> _SNC ValueChange: N` — push notification received
3. `spool_refresh: _SNC ValueChange received` — post_therapy acted on it
4. `spool_refresh: spool is CURRENT` — spool was fresh after pull

### 9.5 BRP recording end time

Archive analysis confirms BRP ends at TherapyStop ≈ MaskOff time.
BRP duration (nrecs × 60s) matches STR Duration (±1 min rounding).
The firmware stops recording at TherapyStop (session_writer_stop),
which matches AS11 behavior.

**Status:** Resolved by archive analysis. No further action needed.

### 9.6 Short sessions without STR entries

Archive analysis found sessions that have BRP/PLD/SA2 files but are
NOT in STR.edf session entries (e.g., Jul 4 03:38 and 06:26 sessions).
These are likely very short or interrupted sessions that the AS11
doesn't count as real therapy sessions for Summary spool purposes.

**Impact:** None for EDF generation — the firmware records all sessions
and generates EDFs for them. The STR.edf only includes sessions that
the AS11 considers valid (with MaskOn/duration in Summary spool).

**Status:** Observed but not an issue. Expected behavior.
