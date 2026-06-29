# 0002 — EDF export

- **Status:** Draft
- **Author(s):** Ilya Kruchinin (@ilyakruchinin)
- **Created:** 2026-06-17
- **Last updated:** 2026-06-29
- **Related specs:** `0001-airsense11-ble-sync.md`, `0003-o2ring-ble-sync.md`

## 1. Summary

Mapping pulled therapy and oximetry data to EDF/EDF+ files in the format
the ResMed AirSense 11 writes to its SD card.  Output goes to
`/somnotrace/SDCARD/` (ResMed-compatible tree) for OSCAR compatibility.

## 2. Motivation / goals

- Produce byte-identical EDF files to what the AS11 firmware writes.
- Support multi-day `STR.edf` (summary) from Summary spool data.
- Include the current (in-progress) noon-day in `STR.edf`.
- Export `BRP.edf`, `PLD.edf`, `SA2.edf`, `EVE.edf` per session.

## 3. Non-goals

- Re-encoding or transcoding therapy waveforms (raw streams are passed
  through with header generation only).

## 4. Behaviour

### 4.1 Inputs

- **Summary spool** (`/somnotrace/.sessions/summaries/YYYYMMDD.spool`):
  per-day protobuf records pulled via BLE `StartSpool` with a 30-day
  lookback.  Each record contains scalars (AHI, indices, enums) and
  metric submessages (percentiles for pressure, flow, leak, etc.).
- **Settings JSON** (`<session>_settings.json`): from BLE `Get` RPC for
  `SettingProfiles`, captured during post-therapy collection.
- **Stream data** (`<session>_brp.snt`, `_pld.snt`, `_sa2.snt`): raw
  therapy waveforms recorded during the session.
- **Events** (`<session>_events.snt`): JSON-line event notifications
  (MaskOn, TherapyStop, etc.).
- **Respiratory events** (`<session>_resp_events.bin`): protobuf spool
  from `TherapyEvents-RespiratoryEvents`.

### 4.2 States / flow

1. **Post-therapy collection** (`post_therapy_collect`):
   - Pull Summary spool (30-day lookback) → per-day `.spool` files.
   - Pull respiratory events spool → `.bin` file.
   - Get device identification → `_ident.json`.
   - Get current settings → `_settings.json`.
2. **EDF generation** (`edf_gen_generate`):
   - Generate `BRP.edf`, `PLD.edf`, `SA2.edf` from stream data.
   - Generate `EVE.edf` from respiratory events.
   - Generate `STR.edf` from all available Summary spool day records.
   - Generate `Identification.json` from ident data.

### 4.3 Outputs / data formats

#### 4.3.1 Directory structure

```
/somnotrace/SDCARD/
  DATALOG/
    <YYYYMMDD>/          (noon-day folder)
      <ts>_BRP.edf       (Flow.40ms, Press.40ms)
      <ts>_PLD.edf       (9 × 0.2s channels)
      <ts>_SA2.edf       (Pulse.1s, SpO2.1s)
      <ts>_EVE.edf       (annotations + events)
      <ts>_STR.edf       (daily summary, multi-record)
  SETTINGS/
    <ts>_SETTINGS.edf    (settings snapshot)
  Identification.json
```

#### 4.3.2 STR.edf — summary record scaling

The AS11 stores summary metrics in the protobuf spool as **fixed-point
integers**.  Its firmware STR writer divides each value by a
field-specific **logical scale** before writing the EDF digital value.
The physical value is then `digital / edf_output_scale`.

Verified against live BLE data (spool vs `Get` RPC):

| Field group          | logical_scale | Conversion      | Example                    |
|----------------------|---------------|-----------------|----------------------------|
| Pressure (cmH2O)     | 2             | `raw / 2`       | 1178 → 589 → 11.78 cmH2O   |
| Flow (L/s)           | 0.2           | `raw * 5`       | 52 → 260 → 0.52 L/s        |
| Humidity/Temp/Power  | 10            | `raw / 10`      | 1160 → 116 → 11.6 mg/L     |
| SpO2 (%)             | 100           | `raw / 100`     | 97 → 0.97 → 97%            |
| Minute Ventilation   | 12.5          | `raw * 2 / 25`  | 650 → 52 → 6.5 L/min       |
| Respiratory Rate     | 20            | `raw / 20`      | 1280 → 64 → 12.8 bpm       |
| Tidal Volume         | 2             | `raw / 2`       | 50 → 25 → 0.5 L            |
| Indices (AHI etc)    | 10            | `raw / 10`      | 80 → 8 → 0.8 events/hr     |
| Duration/enums/SAU   | 1             | no conversion   | —                          |

Settings fields [6-30] come from the `Get` RPC response (settings.json)
and are already in EDF digital units (e.g. pressures × 50, temperatures
× 10).  No additional scaling is applied to those.

#### 4.3.3 Current day in STR.edf

The AS11 **does include the current in-progress noon-day** in the Summary
spool.  The PeriodStart/PeriodEnd span the full noon-to-noon window even
while the day is still in progress, and DurationMin accumulates as
sessions are added.

`collect_summary_spool()` captures and stores it as `YYYYMMDD.spool`,
where the filename is derived from the raw AS11 PeriodStart (no clock
drift correction).  This is critical: applying drift correction before
computing the noon-day label can shift the day by one when the corrected
time falls before noon (e.g. 12:00 AS11 → 11:52 NTP → previous day).

`build_current_day_record()` remains as a fallback for when the spool
pull fails entirely — it synthesizes Date, MaskOn/MaskOff, MaskEvents,
and Duration from `events.snt` and session timing, with -1 sentinels for
all other fields.

### 4.4 Error handling & edge cases

- Missing summary spool for a day → that day is omitted from STR.edf.
- Spool pull failure → `build_current_day_record()` synthesizes a
  minimal record from session events.
- Missing settings JSON → settings fields [6-30] remain -1 sentinel.
- Spool filenames use raw AS11 PeriodStart for noon-day classification
  (no drift correction) — consistent with `edf_gen.c`'s day matching.

## 5. Acceptance criteria

- [x] STR.edf stats fields match AS11 firmware output (verified via
      live BLE spool vs Get RPC comparison, 2026-06-29).
- [x] Current in-progress noon-day included in Summary spool (verified
      via FTP fetch + Python protobuf parse, 2026-06-29).
- [x] Spool filename uses raw AS11 PeriodStart (no drift correction)
      — fixed in `post_therapy.c`.
- [ ] Multi-day STR.edf opens correctly in OSCAR.
- [ ] BRP/PLD/SA2/EVE EDF files match AS11 SD card output.

## 6. Security / privacy considerations

- Handles personal medical data.  No real patient data in tests or
  fixtures.  All data stays on local SD card unless explicitly uploaded.

## 7. Open questions

- Should we also query `Summary-SpontTriggerPercentage` and
  `Summary-SpontCyclePercentage` via Get RPC?  These return
  `InvalidObject` errors and may require a different RPC method.
- TargetMinuteVentilation, IeRatio, and InspiratoryDuration metrics
  also return `InvalidObject` via Get RPC but are present in the spool.
  These fields are not yet mapped in the C STR signal definitions.
