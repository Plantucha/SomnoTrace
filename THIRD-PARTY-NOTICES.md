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
