# Specifications

This folder holds **specification documents** that describe the behaviour the
firmware must implement. Specs are the source of truth for *what* and *why*;
the code is *how*.

These documents are part of the repository and are reviewed/versioned with the
code.

## Conventions

- One spec per file, numbered sequentially: `NNNN-short-title.md`
  (e.g. `0001-airsense11-ble-sync.md`).
- Start from [`0000-template.md`](0000-template.md).
- A spec should be implementation-agnostic where practical: describe observable
  behaviour, data formats, states, and acceptance criteria — not internal code
  structure.
- When a spec changes materially, update its **Status** and **Changelog**.

## Suggested initial specs

- `0001-airsense11-ble-sync.md` — discovering, pairing, and pulling therapy
  data from the ResMed AirSense 11 over BLE.
- `0002-edf-export.md` — mapping pulled data to EDF/EDF+ files.
- `0003-o2ring-ble-sync.md` — retrieving overnight oximetry from the Wellue /
  O2 Ring devices.
- `0004-upload-smb.md` — SMB upload behaviour, paths, retries.
- `0005-upload-sleephq.md` — SleepHQ upload/integration behaviour.
- `0006-device-provisioning.md` — Wi-Fi/credentials provisioning & config.

> These are placeholders to guide structure; create them as the design firms
> up.
