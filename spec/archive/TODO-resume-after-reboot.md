# TODO: Ensure New Session Starts After Crash/Reboot When Therapy Is Active

## Status

**Not started.** The prerequisite fixes (fsync in flush_all, _brp.snt naming
fix, timezone-before-BLE, headerless recovery) have been implemented and need
to be confirmed working before this is attempted.

## Design decision: new session, not append-mode resume

The original plan considered reopening existing `.snt` files in append mode
to continue the same session after a crash. After review, **starting a new
session is the better approach** for these reasons:

1. **Honest data representation**: There IS a gap (reboot time + BLE
   reconnect). Appending would hide it, making the EDF look continuous when
   it isn't. A new session makes the gap explicit in the graphs.
2. **Simplicity**: No need to parse headers, restore sample counts, seek to
   end, or manage append mode. The existing `session_writer_start()` already
   does exactly this.
3. **Generalises correctly**: The "ESP powered on after AS11 already running
   therapy" scenario is the same code path — detect therapy active, start
   fresh session. No special-casing for crash vs. cold-start.
4. **With the fixes already implemented, this works**: The interrupted
   session's data survives (fsync), gets recovered with a reconstructed
   start time, produces its own EDF files, and uploads. The recovery session
   gets a correct local-time ID, produces EDF files (naming bug fixed), and
   uploads. Two sessions with a visible gap — which is exactly what happened.

## Problem History (Aug 7–8, 2026 incident)

On the night of Aug 7, 2026, the ESP device was recording an overnight CPAP
therapy session (`20260808_012019`, starting ~01:20 local). After ~4h39m of
flawless operation (418,675 flow samples, clean 60-second flush ticks every
minute), the device suffered an **INT_WDT reset** at approximately 06:00
local — coinciding exactly with the therapy alert window end (`win_end=360`
= 06:00).

### What happened after the crash

1. The ESP rebooted. `session_writer_recover()` found the interrupted
   session `20260808_012019` but could not read a valid `.snt` header
   because `flush_all()` only called `fflush()` (pushes stdio buffer into
   FATFS RAM) and never `fsync()` (commits FAT directory entry + cluster
   chain to the SD card). The directory entry read 0 bytes, so the recovery
   code skipped the session entirely. **4h39m of therapy data was lost** —
   the data was physically in orphaned clusters on the SD card but
   unreachable through the filesystem.

2. The ESP reconnected to the AS11, detected therapy still active, and
   started a new session (`20260807_200019` — misnamed in UTC because the
   timezone had not yet been applied; true local time was ~06:00). This
   recovery session captured ~61 minutes of therapy data (91,845 flow
   samples).

3. When therapy stopped at ~07:01, EDF generation ran but **silently
   discarded** the recovery session because the no-therapy gate in
   `edf_gen.c` probed `*_brp.snt` (v1 naming) instead of `*_flow.snt`
   (v2 naming). With 0 samples found, it concluded "no therapy delivered"
   and skipped all EDF files.

4. SleepHQ upload ran and re-uploaded the earlier evening session
   (`20260807_221802`, 22:18–01:14) but the entire overnight session was
   missing — both the crashed 4h39m portion and the recovery 61-minute
   portion.

### Net result

- **AS11 golden source**: 2 sessions totaling ~8.7 hours of therapy.
- **SleepHQ**: 1 session, ~3 hours.
- **User experience**: Personal graphs showed a huge gap; SleepHQ showed
  only the earlier session. User slept through the entire night but the
  data was gone.

### Fixes already implemented (prerequisites)

| Fix | File | What |
|-----|------|------|
| fsync in flush_all | `main/session_writer.c` | `fsync(fileno(f))` after every `fflush()` + header update, bounding crash data loss to one 60-second flush interval |
| _brp.snt naming | `main/edf_gen.c` | No-therapy gate now probes `_flow.snt` first, falls back to `_brp.snt` |
| Timezone before BLE | `main/time_sync.c`, `main/main.c` | `time_sync_apply_saved_timezone()` called before `as11_ble_init()` so crash-recovery sessions get local-time ids |
| Headerless recovery | `main/session_writer.c` | `session_writer_recover()` reconstructs `start_epoch_ms` from session id instead of discarding |
| Coredump diagnostics | `sdkconfig.defaults`, `main/crash_diag.c` | Panic reboot delay 2s, CHECK_BOOT disabled, log on image check failure |

## What needs to be done

### Goal

Ensure that when the ESP boots (after crash or cold-start) and detects that
therapy is already active on the AS11, it **starts a new session** with a
correct local-time session ID. The interrupted session from before the crash
is preserved as its own session (thanks to the fsync + headerless recovery
fixes) and both sessions produce EDF files and upload independently.

### Current behaviour (needs verification)

The session writer currently auto-starts a new session when it receives a
`TherapyStart` notification from the AS11. However, when the ESP reboots
mid-therapy and reconnects, the AS11 does not send a new `TherapyStart` —
it just starts streaming `StreamData` notifications. The current code path
for this scenario needs to be verified:

1. **Does `session_writer_on_stream_data_raw()` start a session if none is
   active?** If not, the recovery session is never created and all post-crash
   therapy data is lost.
2. **Does `session_writer_on_notification()` handle `_ZLE ValueChange`
   rising edge** as a therapy-start trigger? The AS11 may send `_ZLE` state
   updates on reconnect that could trigger session start.

### Tasks

1. **Verify the auto-start-on-stream-data path**: Check whether
   `session_writer_on_stream_data_raw()` or `session_writer_on_notification()`
   starts a new session when `StreamData` arrives and no session is active.
   If not, add logic to do so.

2. **Verify session ID uses local time**: With the timezone-before-BLE fix,
   `time_sync_apply_saved_timezone()` is called before `as11_ble_init()`.
   Verify that the session ID generated during crash recovery uses local
   time, not UTC. (The Aug 7 incident produced `20260807_200019` instead of
   `20260808_060019` — this should now be fixed.)

3. **Verify interrupted session recovery**: With the fsync + headerless
   recovery fixes, verify that `session_writer_recover()` at boot
   successfully writes `session.json` for the interrupted session and that
   EDF generation produces files for it. The interrupted session should
   appear as its own session in the web UI and SleepHQ upload.

4. **Handle the "ESP powered on after AS11" scenario**: This is the same
   code path as crash recovery — ESP boots, BLE connects, AS11 is already
   running therapy, `StreamData` arrives without `TherapyStart`. The fix
   from task 1 covers this.

5. **Test end-to-end**: Flash firmware, start therapy, force `esp_restart()`
   mid-session, verify:
   - Interrupted session produces EDF files (data preserved via fsync).
   - Recovery session starts with correct local-time ID.
   - Recovery session produces EDF files (naming bug fixed).
   - Both sessions upload to SleepHQ.
   - Graphs show two sessions with a visible gap (honest representation).

### Files to verify/modify

- `main/session_writer.c` — verify `session_writer_on_stream_data_raw()`
  and `session_writer_on_notification()` handle the no-active-session +
  StreamData case; add auto-start logic if missing
- `main/session_writer.c` — verify `session_writer_recover()` correctly
  handles interrupted sessions with the new fsync + headerless recovery
- `main/edf_gen.c` — verify the `_flow.snt` naming fix produces EDF files
  for recovery sessions without `_ZLE`/`MaskOn` events
- `main/time_sync.c` / `main/main.c` — verify timezone is applied before
  session ID generation

### Testing approach

- **Integration test**: flash firmware, start therapy, force `esp_restart()`
  mid-session, verify both sessions produce EDF files and upload.
- **Cold-start test**: power on ESP after AS11 is already running therapy,
  verify a new session starts with correct local-time ID.
- **Edge case test**: crash with no SD card, crash with corrupt `.snt`
  files, crash near midnight (cross-day session folder).
