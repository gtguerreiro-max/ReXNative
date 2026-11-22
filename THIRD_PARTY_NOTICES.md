# Third-party notices

What this repository contains from other projects, with their licenses.

- `renderer/` is the native renderer of [pgr4-native](https://github.com/shaunsingh/pgr4-native)
  (MIT, the pgr4-native authors), as adapted for From Dust in
  [FromDustDeck](https://github.com/gtguerreiro-max/FromDustDeck) and made game-neutral here.
- `renderer/src/native/` ports code from the ReXGlue SDK and, through it, from Xenia
  (BSD-3-Clause, below): format packing, texture layouts, resolve conversions, the shader
  translator's use. The files name the ported functions in their headers.
- `renderer/src/native/fidelityfx.cpp`, `upscaler.cpp` and `frame_generation.cpp` build with the
  AMD FidelityFX SDK's API headers (MIT); `fsr.cpp` uses the FSR 1 shaders the ReXGlue v0.10 SDK
  shipped, prebuilt (`renderer/src/native/shaders/d3d12_5_1`: BSD-3-Clause, from Xenia, which
  takes them from AMD FidelityFX, MIT), kept here since ReXGlue v0.11 has no FidelityFX. The SDK is not in this repository.
- `renderer/src/native/dlss.cpp` and `neural*.cpp` build with the NVIDIA DLSS SDK and DLSS 5
  headers, under NVIDIA's license; they are not in this repository. `neural_scene.cpp`'s
  inference reuse (`--rexnative_dlss5_reuse`) follows the idea Veyra
  (https://github.com/Likely7/Veyra-NRVideo, GPL-3.0) describes for its NR inference reuse,
  written here for a game's depth and motion; no Veyra code is in this repository. Its Oklab
  matrices are Björn Ottosson's (public domain). Its still hold (`--rexnative_dlss5_hold`) follows
  DLSS5-Feeder's output stabiliser (https://github.com/jlrouzies-fr/DLSS5-Feeder,
  `src/feed_hold12.h`; MIT, Copyright (c) 2026 Jean-Laurent ROUZIES).
- `renderer/src/compat/renderdoc_app.h` is RenderDoc's in-application API header (MIT, Baldur
  Karlsson; its license is at its top), which `renderer/src/native/debug_tools.cpp` loads
  RenderDoc through. RenderDoc itself is not in this repository.
- `renderer/src/hud/` draws with Dear ImGui (MIT, Omar Cornut), which the ReXGlue SDK provides.
- `rexnative/listing.py` is based on pgr4-native's `scripts/analysis/tu_address_map.py` (MIT).
- `sdk/patches/v0.11/` modify the ReXGlue SDK (BSD-3-Clause, below). Those whose `From:` names
  pgr4-native's author are pgr4-native's (MIT); several port code from Xenia Canary
  (BSD-3-Clause, below), each naming the Canary commit and files. `0013` is From Dust's port's
  (MIT); the others are this repository's (MIT). `scripts/vcenv.cmd` follows pgr4-native's.

## DLSS5-Feeder (https://github.com/jlrouzies-fr/DLSS5-Feeder)

The still hold in `renderer/src/native/neural_scene.cpp` follows its `src/feed_hold12.h`, and the
motion check the first rule of its Refine vector pipeline (README, Per-pixel refinement).

```
MIT License

Copyright (c) 2026 Jean-Laurent ROUZIES

Permission is hereby granted, free of charge, to any person obtaining a copy of this software
and associated documentation files (the "Software"), to deal in the Software without
restriction, including without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the
Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or
substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```

## ReXGlue SDK (https://github.com/rexglue/rexglue-sdk)

```

Copyright (c) 2026, Tom Clay <tomc@tctechstuff.com>

Portions of this software are derived from the Xenia project:
Copyright (c) 2022, Ben Vanik and Xenia project contributors
https://xenia.jp

All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its
   contributors may be used to endorse or promote products derived from
   this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

## Xenia Canary (https://github.com/xenia-canary/xenia-canary)

```
Copyright (c) 2015, Ben Vanik.
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the project nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL BEN VANIK BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

