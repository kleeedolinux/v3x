# Third-party notices

V3X includes or links the following third-party code. Their licenses apply to those parts only;
the license of V3X itself is in `../LICENSE`.

| component | where | used by | license | upstream |
|---|---|---|---|---|
| MinHook | `third_party/minhook/` (git submodule, commit 8af6b4a) | the loader `version.dll` (`src/core.c`, `src/safe_hook.c`) and tests | BSD 2-Clause (`third_party/minhook/LICENSE.txt`) | https://github.com/TsudaKageyu/minhook |
| Hacker Disassembler Engine 64 (part of MinHook) | `third_party/minhook/src/hde/` | MinHook | BSD 2-Clause (in MinHook's `LICENSE.txt`) | - |
| Zydis 4.1 (amalgamated, includes Zycore) | `third_party/zydis/Zydis.c`, `Zydis.h` | the `devconsole` plugin (disassembly) | MIT (`third_party/zydis/LICENSE`) | https://github.com/zyantific/zydis |

Binary distributions of `version.dll` must reproduce the MinHook copyright notice, conditions and
disclaimer below. Binary distributions of `devconsole.dll` must include the Zydis notice below.

## MinHook

```
MinHook - The Minimalistic API Hooking Library for x64/x86
Copyright (C) 2009-2017 Tsuda Kageyu.
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

 1. Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.
 2. Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in the
    documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER
OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

Portions of this software are Copyright (c) 2008-2009, Vyacheslav Patkov
(Hacker Disassembler Engine), under the same 2-clause BSD terms; see
third_party/minhook/LICENSE.txt for the full text.
```

## Zydis

```
The MIT License (MIT)

Copyright (c) 2014-2024 Florian Bernd
Copyright (c) 2014-2024 Joel Höner

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

## Not included

Victoria 3 is (c) Paradox Interactive. No game files are part of this repository; files derived
from the user's own game install (e.g. `mods/v3x_lowspec_shaders/gfx/`) are generated locally and
ignored by git.
