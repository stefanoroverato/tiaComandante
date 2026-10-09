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

## Live data: separate libraries (not compiled into tiaComandante)

The `live_data` tool loads these libraries at run time. They are built or copied as separate
files next to the server (`S7CommPlusDriver.dll`, `zlib.net.dll`, `libcrypto-3-x64.dll`,
`libssl-3-x64.dll`) and can be replaced with other compatible builds. The build also copies
their license texts there.

### S7CommPlusDriver

- Source: https://github.com/thomas-v2/S7CommPlusDriver (fork with the changes listed in
  `third_party/README.md`: https://github.com/stefanoroverato/S7CommPlusDriver), submodule
  `third_party/s7commplus`
- Copyright (C) 2023 Thomas Wiens
- License: GNU Lesser General Public License, version 3 or later. Full text:
  `third_party/s7commplus/LICENSE` (copied as `S7CommPlusDriver.LICENSE.txt`); it supplements
  the GNU GPL version 3, `third_party/licenses/GPL-3.0.txt` (copied as `GPL-3.0.txt`).
- tiaComandante uses the library only through its public interface, as a separate assembly
  loaded at run time. Its source code, including the modifications, is in the fork above.

### ZLIB.NET

- Source: `third_party/s7commplus/src/Zlib.net` (part of the S7CommPlusDriver repository)
- License: BSD-style (copied as `zlib.net.LICENSE.txt`)

```
Copyright (c) 2006-2007, ComponentAce
http://www.componentace.com
All rights reserved.

Redistribution and use in source and binary forms, with or without modification, are
permitted provided that the following conditions are met:

Redistributions of source code must retain the above copyright notice, this list of
conditions and the following disclaimer.
Redistributions in binary form must reproduce the above copyright notice, this list of
conditions and the following disclaimer in the documentation and/or other materials
provided with the distribution.
Neither the name of ComponentAce nor the names of its contributors may be used to endorse
or promote products derived from this software without specific prior written permission.
THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS
OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
OF THE POSSIBILITY OF SUCH DAMAGE.
```

### OpenSSL 3

- Source: https://www.openssl.org. By default the build copies the x64 DLLs shipped in the
  S7CommPlusDriver repository (`src/S7CommPlusDriver/OpenSSL-dll-x64`, OpenSSL 3.0.8);
  CMake option `TC_OPENSSL_DIR` selects another OpenSSL 3 build.
- License: Apache License 2.0 (same text as `LICENSE` in this repository)

## Not redistributed

The Siemens TIA Portal Openness assemblies (`Siemens.Engineering.*`) are not part of
tiaComandante: they are loaded at run time from the local TIA Portal V21 installation
and remain subject to the Siemens license terms.
