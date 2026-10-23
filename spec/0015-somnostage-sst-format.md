# 0015 — SomnoStage `.sst` sleep-stage file format (SST1)

- **Status:** Implemented
- **Author(s):** Ilya Kruchinin
- **Created:** 2026-10-23
- **Last updated:** 2026-10-23
- **Related specs:** `0002-edf-export.md`, `0003-o2ring-ble-sync.md`

## 1. Summary

This spec defines **SST1**, the binary file format SomnoTrace uses to store
SomnoStage sleep-staging results (`.sst` files, "sleep stage sidecars"). Each
`.sst` file carries one hypnogram: a fixed 64-byte little-endian header
followed by one stage byte and one confidence byte per epoch. The format is
deliberately minimal so that upload destinations (Aerivue today, BreathingRoom
and others later) can implement a parser in a few dozen lines and ingest the
files **as-is** — no conversion, resampling, or re-epoching is performed or
required.

## 2. Motivation / goals

- SomnoStage produces per-epoch sleep stages plus a per-epoch confidence
  score. The `.sst` file is the authoritative, lossless record of a scoring
  run (the Zeo-compatible CSV export is a derived, downsampled view — see
  §4.7).
- Cloud endpoints receiving `.sst` uploads need an exact byte-level
  description to parse, validate and display hypnograms without access to
  SomnoTrace internals.
- Keep the format stable and versioned so future decoders/flags can be added
  without breaking existing readers.

## 3. Non-goals

- The SomnoStage model itself (features, weights, `SSTP` model blob) is not
  covered; `model_semver` identifies it opaquely.
- The Zeo-compatible CSV export format (consumed by OSCAR) is a separate,
  lossy derivative and is described only for contrast in §4.7.
- Transport: how `.sst` files reach a server (HTTP upload API, naming on the
  wire) is defined by each endpoint's own API, not by this spec. §4.6
  documents the on-device locations for completeness.

## 4. Behaviour

### 4.1 Overall layout

All multi-byte integers are **little-endian**. The header is exactly 64 bytes
with no padding (packed wire layout — do not rely on a compiler's `struct`
layout to match).

```
+-------------------------+
| Header (64 bytes)       |
+-------------------------+
| stages[n_epochs]   u8[] |
+-------------------------+
| confidence[n_epochs] u8[]|
+-------------------------+
```

Total file size is always exactly `64 + 2 * n_epochs` bytes.

### 4.2 Header fields

| Offset | Size | Type    | Field          | Description |
|-------:|-----:|---------|----------------|-------------|
| 0      | 4    | u32     | `magic`        | `0x31545353` — the bytes `S S T 1` in file order. |
| 4      | 2    | u16     | `version`      | `1` for SST1. Readers MUST reject any other version. |
| 6      | 1    | u8      | `decode_mode`  | Decoder used: `0` argmax, `1` viterbi, `2` forward-backward. Currently always `2`. Informational — the decoded stages are already materialised in the payload. |
| 7      | 1    | u8      | `flags`        | Bitmask, see §4.3. |
| 8      | 24   | char[24]| `model_semver` | NUL-padded ASCII model version (e.g. `"1.0.0"`); `"-"` when unavailable. May be shorter than 24 bytes; treat as C string. |
| 32     | 1    | u8      | `source_type`  | `0` = O2 Ring / Wellue oximetry recording (SpO2 + pulse + motion). `1` = ResMed AirSense 11 session (SpO2 + heart rate; no motion channel). |
| 33     | 3    | u8[3]   | `reserved0`    | Zero. Readers MUST ignore; writers MUST write 0. |
| 36     | 4    | u32     | `n_epochs`     | Number of epochs `N` in the payload. `0` is legal only in INSUFFICIENT stubs (§4.4). |
| 40     | 8    | i64     | `start_ms`     | Start of epoch 0 as Unix time in **milliseconds** (UTC). |
| 48     | 4    | u32     | `epoch_sec`    | Nominal epoch duration in seconds (see §4.5 — not always 30). |
| 52     | 4    | u32     | `header_crc`   | Reserved, always `0` in SST1. Intended for a future header CRC-32; readers MUST ignore it. |
| 56     | 8    | u8[8]   | `reserved1`    | Zero. Readers MUST ignore; writers MUST write 0. |

### 4.3 `flags` byte (offset 7)

| Bit  | Value | Name           | Meaning |
|-----:|------:|----------------|---------|
| 0    | 0x01  | `PARTIAL`      | The source recording was still being written (or truncated) when scored; the hypnogram covers only the data that existed. Firmware never uploads these; a later rescore replaces the file without this flag. Consumers may display the content but should mark it provisional/incomplete. |
| 1    | 0x02  | `MOTION_USED`  | A motion channel was fed to the scorer (set for ring sources; AS11 sessions have no motion so it is clear). Informational. |
| 2    | 0x04  | `INSUFFICIENT` | Presence-guard stub: fewer than 30 epochs contained a valid oximeter signal (device off the finger, watch not worn, corrupt source). The payload is all-`0xFF` stages / zero confidence — there is **no usable hypnogram**. Firmware never uploads these; consumers should treat the file as "no data". |
| 3–7  | —     | reserved       | Writers MUST write 0; readers MUST ignore unknown bits rather than reject the file. |

### 4.4 Payload

Immediately after the header, with no padding:

- **`stages[N]`** — `N` unsigned bytes at offset `64`, one per epoch:

  | Value | Stage |
  |------:|-------|
  | 0     | Wake |
  | 1     | Light (N1/N2) |
  | 2     | Deep (N3) |
  | 3     | REM |
  | 0xFF  | No score (only inside INSUFFICIENT stubs) |

  Values `4`–`0xFE` are reserved. A robust reader should treat any stage byte
  `> 3` as "no data" rather than rejecting the file.

- **`confidence[N]`** — `N` unsigned bytes at offset `64 + N`, one per epoch:
  the posterior probability of the assigned stage after temporal decoding,
  scaled to `0..255` (`value / 255.0` → `0.0..1.0`). `0` in stubs.

Epoch `k` covers the half-open interval
`[start_ms + k·epoch_sec·1000, start_ms + (k+1)·epoch_sec·1000)` in Unix
milliseconds (UTC).

A conforming file satisfies:

```
size == 64 + 2·n_epochs
magic == 0x31545353, version == 1
1 <= n_epochs <= 100000        (sanity ceiling; >24 h at the finest grid is corrupt)
1 <= epoch_sec <= 3600
start_ms > 0
```

Sanity limits are guidance for readers, not format constraints; SomnoTrace's
own validator additionally refuses `PARTIAL`/`INSUFFICIENT` files, files over
256 KB, and files whose payload contains no stage byte `< 4`.

### 4.5 The epoch grid — `epoch_sec` is authoritative

Epoch duration is **not fixed at 30 s**:

- AirSense 11 sessions and full-rate ring recordings (1 Hz sources):
  `epoch_sec = 30`.
- Gen1 O2 Rings sample at 0.25 Hz, so their epoch grid is 8 samples × 4 s =
  **`epoch_sec = 32`**.

Readers MUST lay epochs onto the timeline using `epoch_sec` from the header.
Assuming 30 s silently compresses a Gen1 night by ~6 % (an 8 h recording
becomes 7 h 30 m) and skews every stage transition relative to companion
signals (CPAP flow, oximetry). When aligning `.sst` stages with other
time-series, sample the stage function at each target instant
(`idx = floor((t - start_ms) / (epoch_sec·1000))`) — do not stretch or
resample the stage array itself.

### 4.6 File locations and naming (device side)

On the SomnoTrace SD card (informational for anyone pulling files directly,
e.g. over FTP):

| Source | Path |
|--------|------|
| Ring recording | `.somnotrace/oximetry/recordings/<YYYYMMDD>/<recording_id>/generations/<g>/data/stages.sst` |
| AirSense 11 session | `.somnotrace/sessions/streams/<YYYYMMDD>/<session_prefix>_stages.sst` |

`<g>` is the recording's active generation (from `recording.json`).
`<session_prefix>` is the session timestamp prefix shared by the session's
other files, in `YYYYMMDD_HHMMSS` form
(e.g. `20261023_014432` → `20261023_014432_stages.sst`).

Files are written atomically (`.tmp` sibling + rename); readers scanning a
live filesystem may encounter `*.sst.tmp` files and should ignore them.

When uploaded to a cloud endpoint, the file is sent as
`<unit_id>_stages.sst`, where `<unit_id>` is the ring `recording_id` or
`as11_<session_prefix>` for CPAP sessions — matching the `unit_id` used by
SomnoTrace's stage-upload tracking. Aerivue rejects files not following this
shape; consult each endpoint's API for its own naming rules.

### 4.7 Fragmentation — multiple `.sst` per night

A ring wear break (toilet trip, adjustment) splits one night into several
recordings. SomnoTrace scores fragments of the same night (gaps < 90 min) in
a single decode pass but writes **one `.sst` per recording**, each covering
only its own `[start_ms, start_ms + n·epoch_sec·1000)` span. Consumers should
expect several `.sst` files per night with contiguous or short-gap time
ranges and stitch them on the time axis — never assume one file equals one
night. AS11 sessions likewise produce one `.sst` each, so a night with
interrupted/restarted therapy can also yield several files.

### 4.8 Relationship to the Zeo CSV export

For OSCAR compatibility SomnoTrace also emits a Zeo-format CSV derived from
the `.sst`: stages are mapped to Zeo codes (Wake→1, REM→2, Light→3, Deep→4),
re-gridded to a strict 30 s raster by midpoint sampling, and summarised into
ZQ-style statistics. Confidence is dropped and Gen1 32 s epochs are
resampled. The CSV is a **lossy presentation export** — the `.sst` is the
canonical record and is what cloud endpoints receive.

### 4.9 Versioning

`version` is a hard version: a reader that does not understand the version
MUST refuse the file. Unknown `flags` bits and non-zero reserved fields do
not make a file invalid — ignore them. New payload columns or a CRC would
arrive as `version = 2`+ and are out of scope here.

## 5. Acceptance criteria

A third-party `.sst` parser is conformant when it:

- [ ] Rejects files whose size is not exactly `64 + 2·n_epochs`, whose magic
      or version differ, and honours the §4.4 sanity limits.
- [ ] Interprets all multi-byte fields as little-endian.
- [ ] Maps stage bytes `0–3` to Wake/Light/Deep/REM and treats any other
      value as "no data".
- [ ] Converts confidence bytes with `c / 255.0`.
- [ ] Places epochs on the timeline using `epoch_sec` (correct results for
      both 30 s and 32 s files).
- [ ] Treats `INSUFFICIENT` files as "no hypnogram" (stub, payload all 0xFF)
      and `PARTIAL` files as provisional-but-displayable.
- [ ] Ignores reserved header bytes and unknown flag bits without failing.
- [ ] Can stitch multiple same-night `.sst` files from ring fragments on a
      common time axis.

## 6. Security / privacy considerations

`.sst` files contain inferred sleep-stage timelines — personal health data.
They carry no identifiers (no device serial, user name, or raw physiology),
but the timing pattern is still sensitive and is covered by the same
handling rules as the rest of the recording: encrypted transport on upload,
access-controlled storage, and no real patient data in tests or fixtures.
`model_semver` leaks only the public model version string.

## 7. Open questions

- `header_crc` (offset 52) is reserved for a header integrity check; whether
  to populate it is deferred until a second format revision.
- If a future model emits per-class probabilities or additional payload
  columns, that would be `version = 2` with a different payload layout —
  flagged here so readers version-gate now.

## 8. Changelog

- 2026-10-23: Initial version — documents the format as implemented in
  `somno_ml_worker.c` / `somno_ml.h` (SST1) and validated by the Aerivue
  upload integration.
