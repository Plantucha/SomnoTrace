# SomnoTrace — EDF vs AS11 Reference Comparison (2026-06-29)

Analysis of ESP-generated EDF files against AS11 reference exports for
session `20260629_232738` (ESP) vs `20260629_233520` / `20260629_233506`
(AS11).

Clock drift: **-449 007 ms** (NTP = AS11 + drift; AS11 = NTP + 449.007 s).

---

## 1. Summary

| File | Header match | Data match | Graph correlation | Status |
|------|-------------|------------|-------------------|--------|
| SA2  | start_time -13 s, CRC1 differs | **byte-identical** | N/A | done |
| EVE  | start_time +1 s, CRC1 differs  | **byte-identical** | N/A | done |
| CSL  | start_time +1 s, CRC1 differs  | **byte-identical** | N/A | done |
| BRP  | start_time -13 s, CRC1 differs | 74.5 % bytes differ | **1.0000** | done |
| PLD  | start_time -13 s, CRC1 differs | 33.8 % bytes differ | **>0.99** | done |
| STR  | **identical** | 1 value off by 1 | N/A | done |
| Identification.json | — | **byte-identical** | — | done |
| CurrentSettings.json | structure OK | 6× `.0` formatting | — | done |

---

## 2. Byte-identical files

### SA2.edf (data)

All 242 data bytes match exactly. The `invalid_passthrough` fix works:
`-1` sentinel values for Pulse/SpO2 (no oximeter connected) are now
written verbatim instead of being scaled to `0`.

### EVE.edf and CSL.edf (data)

Both 64-byte data sections are byte-identical. Event annotations and
calibration data match AS11 exactly.

### Identification.json

Byte-for-byte identical (768 bytes).

### STR.edf (header)

All 78 signal definitions and all header fields match exactly. This
includes patient ID (CRC words), recording ID, start date/time, and
every signal label/transducer/unit/phys_min/phys_max/dig_min/dig_max/
prefilter/spr/reserved field.

---

## 3. Remaining differences

### 3.1 Header start_time (13 s therapy-start detection lag)

| File group | ESP header time | AS11 header time | Delta |
|------------|----------------|-----------------|-------|
| BRP / SA2 / PLD | 23.35.07 | 23.35.20 | -13 s |
| EVE / CSL       | 23.35.07 | 23.35.06 | +1 s  |

**Root cause:** ESP detects therapy start from the first BLE stream
notification. AS11 starts its EDF recording ~13 s later, when it
internally decides therapy is stable. The drift correction itself is
correct — both timestamps are in the AS11 clock domain. The 1 s offset
for EVE/CSL is because event annotations start at a slightly different
point than signal recording.

**CRC1 (H1) differs** as a direct consequence, since CRC1 covers header
bytes 0x19–0xFF which include the start_time field. CRC2 (H2, signal
header blocks) matches in all files.

**Possible fixes (not implemented):**
- Query AS11 for its session start time via BLE RPC after therapy stops.
- Delay session start detection to match AS11's internal threshold.
- Both add complexity for a cosmetic header difference.

### 3.2 BRP.edf — waveform capture offset (74.5 % byte difference)

The byte difference is entirely due to the 13.4 s capture-start offset
(335 samples × 0.04 s). When aligned:

| Signal | Correlation | Max error | Mean error | Lag |
|--------|------------|-----------|------------|-----|
| Flow.40ms | **1.0000** | 2 / 2500 (0.08 %) | 1.2 | 335 samples (13.40 s) |
| Press.40ms | **0.9993** | 2 / 2500 (0.08 %) | 1.2 | 335 samples (13.40 s) |

The waveforms overlay almost perfectly. Max error is 2 digital units
out of ±2500 full-scale — within rounding noise of the int16 scaling.

### 3.3 PLD.edf — waveform capture offset (33.8 % byte difference)

Same 14 s capture-start offset (7 samples × 2 s). ESP's first 7 samples
capture the pressure ramp-up (0 → 540), while AS11 started recording
after pressure was already stable (359 → 530).

| Signal | Correlation | Mean error | Notes |
|--------|------------|------------|-------|
| MaskPress.2s | 0.9947 | 5.0 | Ramp-up in ESP's first 7 samples |
| RespRate.2s | 0.9956 | 2.1 | |
| TidVol.2s | 0.9983 | 0.4 | |
| MinVent.2s | 0.9945 | 1.7 | |
| Leak.2s | 0.8574 | 2.3 | Noisy small values |
| Press / EprPress | — | 0.0 | Both reach same steady-state (570 / 520) |
| Snore / FlowLim | — | 0.0 | Sparse / invalid values, aligned |

### 3.4 STR.edf — Flow.95 off by 1

| Signal | ESP | AS11 | Delta |
|--------|-----|------|-------|
| Flow.95 [35] | 649 | 650 | -1 |
| Crc16 [77] | differs | differs | (consequence of Flow.95) |

77 of 78 signal values are identical. The single off-by-1 on `Flow.95`
is a 1-LSB rounding difference in the spool percentile computation
(`spool_to_edf(raw, 5, 1)` where raw × 5 = 649 vs AS11's 650). Not
safely fixable without the raw spool byte and knowledge of AS11's
exact rounding mode.

### 3.5 CurrentSettings.json — `.0` formatting

Structure is correct: `{"FlowGenerator":{"SettingProfiles":{...}}}`.
Content is semantically identical. 6 numeric values differ in
formatting:

| Field | ESP | AS11 |
|-------|-----|------|
| StartPressure (AutoSet) | `7` | `7.0` |
| MaxPressure (AutoSet) | `20` | `20.0` |
| MinPressure (AutoSet) | `5` | `5.0` |
| StartPressure (AutoSetForHer) | `5` | `5.0` |
| StartPressure (CPAP) | `4` | `4.0` |
| HeatedTubeTemperature | `18` | `18.0` |

cJSON drops the `.0` suffix for integer-valued doubles. AS11's
serializer preserves it. This is cosmetic — all consumers parse JSON
numbers correctly regardless of trailing `.0`.

**Possible fix (not implemented):** Post-process the JSON string to
add `.0` to integer-valued floats matching known field names. Adds
fragility for no semantic benefit.

---

## 4. Fixes applied in this session

All fixes were applied to `main/edf_gen.c`:

1. **SA2 invalid marker** — Added `invalid_passthrough` field to
   `edf_signal_def_t`. When a raw `.snt` sample equals `-1` (the
   invalid sentinel), it is written to the EDF as `-1` directly,
   bypassing physical → digital scaling. Enabled for SA2 Pulse/SpO2.
   Also applies to padding of partial records for passthrough signals.

2. **Clock drift correction** — EDF header datetime and recording ID
   now use `start_epoch_ms - clock_drift_ms` so the header timestamp
   is in the AS11 clock domain.

3. **CurrentSettings.json nesting** — The `SettingProfiles` object is
   now wrapped under a `FlowGenerator` key to match AS11's structure,
   using `cJSON_AddItemReferenceToObject` to avoid double-free.

4. **Dead code removal** — Removed `edf_finalise_crc()` (52 lines):
   no callers remain after CRC computation was moved into
   `edf_write_header()` (in-memory buffer CRC before writing to disk).

---

## 5. Recommendation

No further code changes are needed for data quality. The remaining
differences are either:

- **Cosmetic** — JSON `.0` formatting, CRC1 from 13 s time offset.
- **Inherent to capture architecture** — therapy-start detection lag
  causes a 13 s capture offset; graphs align near-perfectly when
  corrected.
- **Not safely fixable** — STR `Flow.95` 1-LSB rounding without raw
  spool data.

The generated EDF files are as close to identical to AS11 as physically
possible given the independent capture path.
