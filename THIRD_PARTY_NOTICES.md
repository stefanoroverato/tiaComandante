# Third-party notices

tiaComandante is licensed under the Apache License 2.0 (see `LICENSE`). It is built
with the following third-party libraries, included as git submodules in `third_party/`
and compiled into the binaries.

## cJSON

- Source: https://github.com/DaveGamble/cJSON (fork: https://github.com/stefanoroverato/cJSON)
- License: MIT

```
Copyright (c) 2009-2017 Dave Gamble and cJSON contributors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```

## Mini-XML

- Source: https://github.com/michaelrsweet/mxml (fork: https://github.com/stefanoroverato/mxml)
- License: Apache License 2.0 (same text as `LICENSE` in this repository)

NOTICE:

```
Mini-XML

Copyright © 2003-2026 by Michael R Sweet


(Optional) Exceptions to the Apache 2.0 License:
================================================

In addition, if you combine or link compiled forms of this Software with
software that is licensed under the GPLv2 or LGPLv2 (“Combined Software”) and if
a court of competent jurisdiction determines that the patent provision (Section
3), the indemnity provision (Section 9) or other Section of the License
conflicts with the conditions of the GPLv2 or LGPLv2, you may retroactively and
prospectively choose to deem waived or otherwise exclude such Section(s) of the
License, but only in their entirety and only with respect to the Combined
Software.
```

## Not redistributed

The Siemens TIA Portal Openness assemblies (`Siemens.Engineering.*`) are not part of
tiaComandante: they are loaded at run time from the local TIA Portal V21 installation
and remain subject to the Siemens license terms.
