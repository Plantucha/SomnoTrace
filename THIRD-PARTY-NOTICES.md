# Third-Party Notices

SomnoTrace is an independent, clean-room implementation. It does **not** copy
source code from the projects listed below. Those projects were used only as
**reference material** to understand reverse-engineered device and
communication protocols. Protocol facts and ideas are not protected by
copyright; only their original expression (source code) is. SomnoTrace's
implementation was written independently.

These acknowledgements are provided in good faith to credit the prior
reverse-engineering work, and to record the MIT license terms of the
reference projects in case any incidental, copyrightable material is ever
determined to have been incorporated.

---

## airbreak-plus

- **Project:** airbreak-plus
- **Source:** https://github.com/m-kozlowski/airbreak-plus
- **Referenced for:** understanding of ResMed AirSense BLE/data-protocol
  behaviour (notably the `docs/` and `python/` directories).
- **License:** MIT

## o2ring-s-protocol

- **Project:** o2ring-s-protocol
- **Source:** https://github.com/nglessner/o2ring-s-protocol
- **Referenced for:** understanding of the Wellue / O2 Ring S BLE protocol.
- **License:** MIT

## Roboto Font

- **Project:** Roboto Font
- **Source:** https://github.com/google/fonts/tree/main/ofl/roboto
- **Used for:** Display UI typeface
- **License:** SIL Open Font License 1.1 (OFL)

## esp-idf-ftpServer

- **Project:** esp-idf-ftpServer
- **Source:** https://github.com/nopnop2002/esp-idf-ftpServer
- **Used for:** lightweight FTP server for Wi-Fi file transfer to/from SD card
- **License:** MIT (Copyright (c) 2021 nopnop2002, Copyright (c) 2018 LoBo)
- **Notes:** Vendored in `third_party/esp-idf-ftpServer/`. Modified to use
  `/somnotrace` as mount point, support selectable anonymous/authenticated
  login modes via `ftp_anonymous_mode` flag, and remove external event-group
  dependency.

## libsmb2

- **Project:** libsmb2
- **Source:** https://github.com/sahlberg/libsmb2 (tag `libsmb2-6.2`)
- **Used for:** SMB2/SMB3 client library for uploading EDF files to SMB shares
- **License:** LGPL-2.1 (library) / BSD-2-Clause (examples)
- **Notes:** Vendored in `third_party/libsmb2/`. ESP-IDF CMakeLists.txt
  adapted from upstream to work with IDF v5.5 build system. `include/esp/config.h`
  updated with `_U_` and `SOL_TCP` macros and `HAVE_SYS_TIME_H`.

## posix_tz_db

- **Project:** POSIX Timezone Database
- **Source:** https://github.com/nayarsystems/posix_tz_db
- **Used for:** IANA-to-POSIX TZ string mapping (e.g. `Australia/Melbourne` →
  `AEST-10AEDT,M10.1.0,M4.1.0/3`) for timezone selection in the web UI.
  The `zones.json` file is downloaded at build time and embedded into firmware
  via `target_add_binary_data`, then served to the web UI via the `/api/tz`
  endpoint. This allows timezone selection without internet connectivity
  (e.g. in SoftAP setup mode).
- **License:** MIT
- **Notes:** `zones.json` is fetched by `scripts/gen_tz_db.py` at build time
  and is git-ignored (generated artifact). The data is not modified.

## uPlot

- **Project:** uPlot
- **Source:** https://github.com/leeoniya/uPlot (tag `v1.6.32`)
- **Used for:** lightweight time-series charting library for the web UI
  (CPU/memory graphs on the Status page, session data plots). Served via
  `/uplot.js` and `/uplot.css` endpoints, embedded in firmware via
  `target_add_binary_data`.
- **License:** MIT (Copyright (c) 2022 Leon Sorokin)
- **Notes:** Vendored in `third_party/uplot/`. Only pre-built distribution
  files (`uPlot.iife.min.js` ~51 KB, `uPlot.min.css` ~1.9 KB) are included;
  no source modifications.

## ES8311 codec driver reference

- **Project:** ESP-ADF esp_codec_dev ES8311 driver
- **Source:** https://github.com/espressif/esp-adf (components/esp_codec_dev/device/es8311)
- **Referenced for:** ES8311 register initialization sequence, clock
  coefficient table, and I2S format configuration for DAC playback.
- **License:** Apache-2.0
- **Notes:** Clean-room implementation in `main/bsp_audio.c`. No source
  code was copied; only register addresses and initialization values
  (protocol facts) were used.

---

## MIT License (reference text)

The reference projects above are distributed under the MIT License. The MIT
License permits use of the material (including for commercial purposes)
provided the copyright notice and permission notice are preserved. The
canonical MIT permission notice reads:

```
MIT License

Copyright (c) <year> <copyright holders>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

> Note: When publishing, replace `<year>` and `<copyright holders>` above with
> the exact notices from each upstream `LICENSE` file, or include verbatim
> copies of each upstream license here.

---

## Interoperability / reverse-engineering note

SomnoTrace interoperates with third-party medical devices (e.g. ResMed
AirSense 11, Wellue O2 Ring) over their wireless interfaces. It is intended
for personal interoperability and data-portability purposes. The MIT-licensed
reference projects impose no commercial restrictions. Reverse engineering for
interoperability is recognised in many jurisdictions (e.g. the EU Software
Directive, US DMCA s.1201(f), and interoperability provisions of Australia's
Copyright Act 1968). This is not legal advice; obtain professional advice
before any commercial distribution.
