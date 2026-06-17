# SomnoTrace

> An open-source ESP32-S3 bridge that pulls sleep-therapy and oximetry data
> over Bluetooth Low Energy, writes standard EDF files, and uploads them to
> SMB and/or SleepHQ.

Created and architected by **Ilya Kruchinin**
([@ilyakruchinin](https://github.com/ilyakruchinin)).

> Spiritual successor to [CPAP-AutoSync](https://github.com/ilyakruchinin/CPAP-AutoSync),
> moving from a software-only sync to a dedicated hardware BLE bridge.

## What it does

SomnoTrace is firmware for an ESP32-S3 device that:

- **Connects to a ResMed AirSense 11 over BLE**, pulls therapy data, and
  writes it as **EDF** (European Data Format) files.
- **Connects to a Wellue O2 Ring S / SleepHQ O2 Ring Pro over BLE** and
  retrieves the night's oximetry data in the morning.
- **Uploads** the resulting files to an **SMB** share and/or to **SleepHQ**.

> Status: early development. Architecture and module layout are being
> established.

## Hardware

- ESP32-S3 (Wi-Fi + BLE 5).
- Target devices: ResMed AirSense 11, Wellue O2 Ring S / O2 Ring Pro.

## License

SomnoTrace is licensed under the **GNU General Public License v3.0** with an
additional **attribution requirement** under GPLv3 Section 7(b).

- Full license text: [`LICENSE`](LICENSE)
- Copyright + Section 7(b) attribution term: [`NOTICE`](NOTICE)

In short:

- You are free to use, study, modify, and redistribute this software.
- If you distribute it (including inside a hardware product), you must release
  your complete corresponding **source code** under the GPLv3.
- Any redistribution or derivative work **must keep the attribution**:
  *"Based on SomnoTrace, originally created by Ilya Kruchinin
  (https://github.com/ilyakruchinin)."*

### Commercial licensing

The GPLv3 keeps the public version copyleft. Separately, the copyright holder
can offer the software under **alternative commercial terms** (e.g. for a
company wishing to ship it in a closed-source product). Contributions are
licensed to the maintainer under the project CLA precisely so this remains
possible. Commercial enquiries: via
[github.com/ilyakruchinin](https://github.com/ilyakruchinin).

## Contributing

Contributions are welcome — see [`CONTRIBUTING.md`](CONTRIBUTING.md).

- A **Contributor License Agreement** is required before your first merge
  (individual: [`CLA/individual-cla.md`](CLA/individual-cla.md); corporate:
  [`CLA/corporate-cla.md`](CLA/corporate-cla.md)). Signing is automated on your
  first pull request.
- SomnoTrace is a **clean-room** implementation: study protocols, but **do not
  copy source code** from other projects.

## Acknowledgements

Protocol understanding was informed by independent, MIT-licensed
reverse-engineering projects (no code copied). See
[`THIRD-PARTY-NOTICES.md`](THIRD-PARTY-NOTICES.md):

- [airbreak-plus](https://github.com/m-kozlowski/airbreak-plus) (ResMed AirSense)
- [o2ring-s-protocol](https://github.com/nglessner/o2ring-s-protocol) (Wellue O2 Ring)

## Disclaimer

SomnoTrace is an independent project and is **not affiliated with or endorsed
by** ResMed, Wellue/Viatom, or SleepHQ. It is intended for personal
data-portability and interoperability. It is **not a medical device** and must
not be used for diagnosis or treatment decisions. Use at your own risk.
