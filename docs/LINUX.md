# Linux and Vulkan (experimental)

> [!NOTE]
> An experiment on the `vulkan-experiment` branch: can recompiled games run faster on the Steam
> Deck with Vulkan, and without Proton? Not part of `main`.

ReXNative draws with Direct3D 12, so a game drawn by it is a Windows executable and runs on the
Steam Deck under Proton (vkd3d-proton turns the D3D12 calls into Vulkan). ReXGlue itself builds
for Linux, and its own renderer, the Xenos GPU emulation, has a Vulkan driver (`gpu.driver`:
`d3d12`, `vulkan` or `any`). This page has what was measured, how a game is built and run on
Linux, on ReXGlue's renderer or on ReXNative's without Wine, and what a Vulkan backend for
ReXNative would take.

## What was measured

Tetris Splash on the Steam Deck (SteamOS 3.9, RADV, 1280x800, upscaler off), its title screen,
30 seconds after 30 seconds of warm-up. Frame times from MangoHud (every frame), the GPU's busy
share and the SoC's power from `/sys/class/drm/card0/device`, the game's CPU time from `/proc`
(100 % = one core). Every setup started from the game's Steam shortcut. The Deck's GPU clock was
fixed at 1600 MHz then (its performance overlay's manual clock): the GPU's share compares, the
absolute power does not stand for battery use; the measurements with the clock on auto are in
"ReXNative on Linux, without Wine".

| Setup | FPS | Frame time, mean / 99th pct | GPU busy | SoC power | Game's CPU |
| --- | --- | --- | --- | --- | --- |
| ReXNative (D3D12, vkd3d-proton) | 60.0 | 16.68 / 17.04 ms | 1-2 % | 5.2 W | 32 % |
| Classic renderer, D3D12 (`rexnative.classic = true`) | 60.0 | 16.67 / 17.25 ms | 9-10 % | 5.3 W | 39 % |
| Classic + `gpu.driver = "vulkan"` (refused, see below) | 60.0 | 16.67 / 17.01 ms | 0-2 % | 5.0 W | 32 % |
| Xenos D3D12 alone (`rexnative.enabled = false`) | 60.0 | 16.67 / 16.88 ms | 27-31 % | 11.5 W | 137 % |
| Xenos Vulkan alone, Proton | 60.0 | 16.66 / 16.92 ms | 91-99 % | 15.6 W | 164 % |
| Xenos Vulkan alone, **native Linux build** | 60.0 | 16.66 / 16.88 ms | 90-99 % | 15.9 W | 151 % |

- **ReXNative's renderer is the cheap one by far**: the same screen costs 1-2 % of the GPU drawn
  natively, 27-31 % on the Xenos emulation's D3D12 driver and all of it on its Vulkan driver.
  The API is not what costs: the emulation (EDRAM, render target ownership transfers) is.
- **Xenos on Vulkan costs about 3.5 times the GPU time of Xenos on D3D12** on the Deck. At 99 %
  the GPU has no room left: a heavier game would lose frames on it.
- **Without Proton** the same Vulkan build used 8 % less CPU (151 % against 164 % of a core;
  the whole system 24 % against 28 %), the same GPU and the same frame times.
- Under Proton, `rexnative.classic = true` with `gpu.driver = "vulkan"` does not switch: the
  classic renderer's frames reach the screen through ReXNative's D3D12 swap chain
  (`classic_frames.cpp`), which needs the Xenos D3D12 device. ReXNative logs "renderer switch
  ignored under Wine" and keeps drawing natively, beside an idle Vulkan device; nothing crashes.
  `rexnative.enabled = false` is the way to the Vulkan driver.
- Without ReXNative the frame limiter (`rexnative.menu_fps`) is not there either: the Xenos rows
  may draw more frames than they show.
- On Vulkan the Xenos driver logs "Texture fetch constant ... has invalid type" about 30,000
  times a second for ten seconds after the start (D3D12: about 350): logs of 90 MB.

## Building a game for Linux

The project files `python -m rexnative project` writes have an option: `-DREXNATIVE=OFF`, and
any system other than Windows, builds the game without ReXNative. Its own host code then has no
ReXNative HUD, settings or hooks, and ReXGlue's renderer draws it. The scripts in
[scripts/linux/](../scripts/linux/) do the rest, on Linux or in WSL2 (tested: Arch Linux in
WSL2, clang 23, CMake 4.4):

```bash
# The SDK (the same patched tree as for Windows), copied into the Linux file system first:
scripts/linux/build-sdk.sh ~/rexglue-sdk [VMA include dir]
# A game's work folder (prepared and written on Windows), copied and built in ~/vk-tests/<game>:
scripts/linux/build-game.sh /mnt/d/rexnative-work/<game> ~/vk-tests/<game> ~/rexglue-sdk/out/install/linux-amd64
```

`build-game.sh` leaves `<copy>/linux/` ready to copy to the Deck: the executable, the SDK's
libraries (`librex*.so`, SDL3), `lib/` with the C++ runtime it was built with, `game/`, a
settings file for the Deck's panel (`gpu.driver = "vulkan"`) and `run-linux.sh`.

Two things keep a build from a newer Linux running on the Deck, and both are handled:

- **glibc**: the Deck has 2.43. A build on 2.44 asked for `sinh` and `cosh` at their new 2.44
  version (FFmpeg's `libavutil/eval.c`, inside `librexkernel.so`). `build-sdk.sh` compiles the
  SDK's C code with [glibc-compat.h](../scripts/linux/glibc-compat.h), which binds them to the old
  version. `build-game.sh` prints the newest glibc version the build needs.
- **libstdc++**: the build needs `GLIBCXX_3.4.36`, the Deck's has up to 3.4.35. `lib/` carries
  the one it was built with, and `run-linux.sh` puts it on `LD_LIBRARY_PATH`.

The Deck needs nothing installed: SDL3 is the game's own, and the Vulkan loader, RADV, X11,
Wayland and PipeWire are SteamOS's.

WSL2's Vulkan (Dozen, over the PC's D3D12) cannot run the Xenos Vulkan driver: its render target
cache needs integer MSAA sampled images. The build gets as far as that, with every module loaded;
`--gpu.driver=null` runs the game's code without drawing.

## Running it on the Steam Deck

Started over ssh on gamescope's X display (`DISPLAY=:0`), the game runs and presents, but
gamescope keeps showing Steam: in Game Mode it shows only what Steam started. So the test starts
it from Steam:

- [proton-shim](../scripts/linux/proton-shim) stands in for Proton. An installed game's `play.sh`
  reads `deck.env` beside it; with `REXNATIVE_PROTON=<the shim's folder>` and
  `REXNATIVE_LINUX_BUILD=<the linux/ folder>` there, its Steam shortcut starts the native build.
  [deck-measure.sh](../scripts/linux/deck-measure.sh) writes that `deck.env` for one run
  (`EXTRA_ENV`) and removes it afterwards.
- Desktop Mode, or a Steam shortcut of its own (the Deck kit's `install.sh`), would do as well.

The native build, started that way: the title screen drawn right, 60 FPS, its audio stream on
PipeWire, the Deck's controls found through Steam Input ("New controller connected to user 0").

## ReXNative on Linux, without Wine

ReXNative's D3D12 code also runs as a native Linux program: its D3D12 calls go to vkd3d-proton
built as a Linux library, and its swap chain comes from DXVK Native's DXGI on the game's SDL3
window. Nothing of the renderer is rewritten; what Windows gave it, a few files give it there.

```
game (Linux ELF) -> ReXNative (D3D12) -> libvkd3d-proton-d3d12.so -> Vulkan (RADV)
                                      -> libdxvk_dxgi.so (swap chain on the SDL3 window)
                                      -> libvkd3d-utils.so.1 (D3DCompile, WineHQ vkd3d)
```

- [build-vkd3d-dxvk.sh](../scripts/linux/build-vkd3d-dxvk.sh) builds vkd3d-proton 3.1 and DXVK
  Native 3.1.1. vkd3d-proton needs one fix for that
  ([vkd3d-proton-native-adapter-parent.patch](../scripts/linux/vkd3d-proton-native-adapter-parent.patch)):
  on Linux it ignored the DXGI adapter it was given, and DXVK's DXGI, making a swap chain, asks
  the device for it (a null pointer: a crash).
- The headers are DXVK Native's (mingw-w64's, with `wchar_t` names as DXVK's DXGI has them),
  with mingw-w64 14's newer `d3d12.h`; vkd3d-proton returns structures (descriptor handles,
  resource descriptions) through a pointer as MSVC does, so they are used with
  `WIDL_EXPLICIT_AGGREGATE_RETURNS`.
- [renderer/src/compat/linux](../renderer/src/compat/linux) and
  [renderer/src/linux](../renderer/src/linux): WRL's `ComPtr`; the Win32 calls on eventfd
  (events, which vkd3d-proton's fences signal), timerfd (waitable timers), pthreads, dlopen and
  SDL3; the D3D12 and DXGI entry points; no DLSS (NVIDIA's NGX is Windows only); `D3DCompile`
  for ReXNative's own shaders from WineHQ vkd3d (HLSL to DXBC, as `d3dcompiler_47`), called with
  the Windows calling convention, which that library keeps on Linux. Arch's DXC package
  asserts on any shader, and Microsoft's own DXC release was not tried.
- `build-game.sh ... --rexnative <vkn out>` builds a game so and puts the libraries in `lib/`.

Tetris Splash and Jet Set Radio run so on the Steam Deck, started from Steam, their pictures
right. Tetris Splash's title screen (as in the first table):

| Tetris Splash, title | FPS | Frame time, mean / 99th pct | GPU busy | SoC power | Game's CPU |
| --- | --- | --- | --- | --- | --- |
| ReXNative under Proton | 60.0 | 16.68 / 17.04 ms | 1-2 % | 5.2 W | 32 % |
| ReXNative, native Linux | 60.0 | 16.67 / 17.17 ms | 2-4 % | 4.4 W | 24 % |

Jet Set Radio's heavy start (1,100-1,450 draws a frame, the same every time; see "What it
would gain" below), upscaler off, 30 seconds after 40, and Overlord II's 3D main menu (446 draws,
reached by pressing START through ReXNative's control file, `deck-measure.sh` `WAIT_DRAWS` and
`PRESS`; each build with a userdata folder of its own), 30 seconds 25 after the menu shows.
Proton and native runs alternated, from the same Steam shortcut, the GPU clock on auto:

| Run | SoC power | Game's CPU | Whole CPU | Main thread | Xenos command thread | GPU per frame |
| --- | --- | --- | --- | --- | --- | --- |
| Jet Set Radio, Proton 1 / 2 / 3 / 4 | 5.4 / 7.1 / 8.8 / 9.2 W | 145-164 % | 22-26 % | 79-101 % | 22 % | |
| Jet Set Radio, native 1 / 2 / 3 / 4 | 4.2 / 7.9 / 8.6 / 7.2 W | 107-130 % | 17-21 % | 74-101 % | 5 % | |
| Overlord II, Proton 1 / 2 / 3 | 5.6 / 6.1 / 10.8 W | 136-176 % | 24-29 % | 49-94 % | 21 % | 9.6 ms |
| Overlord II, native 1 / 2 / 3 | 6.8 / 9.0 / 5.0 W | 97-128 % | 17-19 % | 65-99 % | under 5 % | 9.4 ms |

Every run: 30 FPS (both games' own cap), the same frame times, the same GPU time.

- **Natively the game takes less CPU, every time**: a fifth to a third less of it (the whole
  system 17-21 % against 22-29 %). ReXGlue's Xenos command thread, waiting on the GPU, takes 5 %
  of a core instead of 21-22 %, and Overlord II's network thread, 25 % under Wine, is not among
  the busiest at all. Waiting through Wine's synchronization costs more than through Linux's.
- **The power, though, follows the game's main thread**, which waits for the next frame in a
  loop (on Jet Set Radio it runs 90-100 % of the time and blocks about once a frame: 32-34
  voluntary context switches a second at 30 FPS, both ways). How much of a core it spins differs from run to run
  (49-101 %) on both sides, and the SoC's power with it (4.2 W at 74 %, 7.9 W at 100 % on Jet
  Set Radio natively): on average the two come out about even (6.9 W native, 7.5-7.6 W under Proton).
  With the GPU clock fixed at 1600 MHz, Jet Set Radio's earlier runs gave 5.3-6.6 W natively
  against 9.2-9.6 W.
- So the battery's biggest lever is that loop, on Linux and under Proton alike: the next
  section makes it a real wait.
- Recording a draw costs about the same (Jet Set Radio 0.95-1.25 µs natively against
  0.94-1.09 under Proton, Overlord II 1.65-1.81 against 1.35-1.75), with a plain meson release
  build of vkd3d-proton; LTO and `-march=znver2` (the Deck's CPU) are left to try.

What is not there on Linux yet: DLSS (never on the Deck), FSR and frame generation (the
FidelityFX SDK's DX12 runtime; FSR 3.1 has a Vulkan backend, FSR 4 does not), the classic
renderer switch (the Xenos driver presents with Vulkan, not into ReXNative's D3D12 swap chain),
HDR (not tried), the hardware memory watch, and a Steam shortcut of its own (the test starts it
through an installed game's, `proton-shim`).

## Battery: the XDK's spin waits sleep

Where Jet Set Radio's main thread spends its time, from the Linux build's own sampling profiler
([renderer/src/linux/profiler.cpp](../renderer/src/linux/profiler.cpp):
`REXNATIVE_PROFILE=<thread name>,<seconds>,<delay>` sends that thread `SIGPROF` every
millisecond and writes `logs/profile.txt`; its offsets map to the link map's `sub_<address>`):
about 80 % of it in one small loop, the XDK's D3D waiting inside the game's `Swap` (ReXNative's
`kHookSwap`) for the GPU and the next frame: 32 `db16cyc`, Xenon's spin wait hint, then a look
at the device's state, over and over. ReXGlue's codegen writes `db16cyc` as `simde_mm_pause()`,
an x86 `PAUSE`: the loop keeps a core at full clock until the next frame.

[spin_pause.h](../renderer/src/spin_pause.h), which the project adds to the generated code's
precompiled header after ReXGlue's own (rexnative/project.py), makes `simde_mm_pause()` call
[rexnative_spin_pause](../renderer/src/native/spin_pause.cpp): a `PAUSE`, and once a thread's run
of pauses has gone on for 200 µs (a wait, not a short spin lock), a 100 µs sleep every 32 of
them (`nanosleep`; a high resolution waitable timer on Windows). Nothing else changes; the
generated code is compiled again (its precompiled header changed), not generated again.
`REXNATIVE_SPIN_SLEEP=0` in the environment keeps the plain pause, so one build compares both.

Jet Set Radio's heavy start, the GPU clock on auto, the sleep off and on alternated in the same
build:

| Build | Spin wait | SoC power | Game's CPU | Main thread | Frame time 99th pct / max |
| --- | --- | --- | --- | --- | --- |
| Native Linux | spins (1 / 2) | 8.2 / 8.4 W | 130 % | 101 % | 34.5 / 35.4 ms |
| Native Linux | sleeps (1 / 2) | **3.8 / 4.0 W** | 55-57 % | 22-23 % | 34.4 / 34.9 ms |
| Windows, Proton | spins (1 / 2) | 9.1 / 8.2 W | 150-159 % | 89-99 % | 35.3 / 67.7 ms |
| Windows, Proton | sleeps (1 / 2) | **4.4 / 4.5 W** | 100 % | 31 % | 34.9 / 36.8 ms |

The same 30 FPS and frame times for half the power, under Proton as natively: a change for every
game ReXNative draws, on the Steam Deck as it runs them today. Natively it ends 0.5 W lower than
under Proton (the Xenos command thread's 5 % against 23 % of a core). The Windows builds ran
under Proton from test folders (`proton-shim`'s `REXNATIVE_WIN_BUILD`; the installed games
untouched), alternated with [deck-spin-ab.sh](../scripts/linux/deck-spin-ab.sh).

Overlord II's 3D main menu (446 draws, GPU 9.4-9.7 ms a frame either way; a fresh userdata for
every run, START pressed by hand at "Press Start"):

| Build | Spin wait | SoC power | Game's CPU | Main thread | Frame time 99th pct |
| --- | --- | --- | --- | --- | --- |
| Windows, Proton | spins (1 / 2) | 10.7 / 11.3 W | 179 % | 99 % | 34.2 / 34.6 ms |
| Windows, Proton | sleeps (1 / 2) | **5.6 / 5.6 W** | 126-127 % | 34-35 % | 34.0 / 34.1 ms |
| Native Linux | spins (1 / 2) | 8.4 / 5.7 W | 113-121 % | 82-92 % | 34.6 / 34.4 ms |
| Native Linux | sleeps (1 / 2) | **4.6 / 5.2 W** | 53-57 % | 22-24 % | 34.4 / 34.6 ms |

Tetris Splash's title (60 FPS), under Proton: the same with the sleep and without (3.5-4.2 W,
60.0 FPS, 99th percentile 17.0-17.05 ms, the longest frame 17.4-17.7 ms): its main thread does
not spin there. On the PC (Windows' own D3D12), Jet Set Radio's process takes 32-34 % of a core
instead of 112-114 %, at the same 30 FPS.

One spin it does not reach: Overlord II's intro videos under Proton keep its main thread at
76-80 % (9.7-10.7 W) with the sleep or without, while the native build sleeps there. The game
waits there in many short KeDelayExecutionThread calls, and ReXGlue's Windows `SleepFor` spins
the last 250 µs of every wait for precision (`kSpinTail`), which a game that waits briefly and
often pays for in full; the POSIX `SleepFor` is a plain `nanosleep`. A patch for the SDK, not
done here.

Building it: the precompiled header changes, but Ninja does not compile the generated code again
in a build folder made before; `rexnative project --build` and `build-game.sh` remove such a
stale header first (else the build keeps the old code without a word).

### Frame latency 1, with the sleep

With the CPU no longer spinning, frame latency 1 (`--rexnative_frame_latency=1`: a frame begins
only once the one before it was shown, against 2 for the installed games) was tried on Jet Set
Radio's heavy start, 2 and 1 alternated (`AB=latency deck-spin-ab.sh`; "begin to shown" is the
pacer's own measure, from a frame's beginning, the game reading its pad, to the display showing
it: deck-measure.sh's `SHOWN` line):

| Build | Frame latency | Begin to shown | Frame time 99th pct / max | SoC power |
| --- | --- | --- | --- | --- |
| Windows, Proton | 2 (1 / 2) | 42.3 / 45.2 ms | 34.8 / 36.0-36.3 ms | 4.2 / 4.4 W |
| Windows, Proton | 1 (1 / 2) | **33.3 / 23.4 ms** | 34.8-35.1 / 35.4-36.3 ms | 4.5 / 4.5 W |
| Native Linux | 2 (1 / 2) | 41.2 / 39.9 ms | 34.3-34.4 / 34.6-35.1 ms | 3.9 / 3.8 W |
| Native Linux | 1 (1 / 2) | **33.3 / 23.1 ms** | 34.3-34.4 / 34.6-34.8 ms | 4.3 / 4.0 W |

The same 30 FPS and frame times, the picture 8 to 22 ms sooner after the pad (a fifth to a
half less); 33.3 ms where the pacer had not yet learnt to begin the frame later in its slot, 23
ms where it had. It costs 0.1-0.4 W.

Midnight Club: Los Angeles, whose races fell to 22-29 FPS at frame latency 1 before the sleep
(docs/ROADMAP.md; the installed game's settings, FSR at native resolution), driven through the
city by a person for six minutes, a Windows test build under Proton with the sleep and those
settings, the latency switched live every minute, 2, 1, 2, 1, 2, 1, through the control file
([deck-drive-latency.sh](../scripts/linux/deck-drive-latency.sh), then
[deck-drive-report.py](../scripts/linux/deck-drive-report.py): every frame with 2,500 draws or
more, from the per-frame CSV):

| Frame latency | Frames | FPS | Frame time 99th pct | Frames >= 40 ms | Begin to shown | SoC power |
| --- | --- | --- | --- | --- | --- | --- |
| 2 | 3,877 | 29.1 | 51.8 ms | 1.4 % | 45.6 ms | 7.0 W |
| 1 | 4,115 | **29.6** | 41.4 ms | 1.1 % | **33.3 ms** | 7.2 W |
| 2, no pipeline waits | 3,827 | 30.3 | 34.6 ms | 0.3 % | | |
| 1, no pipeline waits | 4,099 | 29.9 | 38.1 ms | 0.8 % | | |

At 1 it held 30 FPS through the drive, the picture 12 ms sooner, for 0.2 W. The longest frames,
at either latency, waited for pipelines to compile (up to 0.5 s: the test folder had no pipeline
cache yet); without them latency 1 has a few more late frames (0.8 % against 0.3 %), none in a
row. Left: a race, with its rival cars, and the same drive with the sleep off
(`REXNATIVE_SPIN_SLEEP=0`), to show the sleep is what changed it.

## Next: ReXNative on Vulkan (not started)

What a Vulkan backend for ReXNative's own renderer would take, starting with the core draw path
(a simple game's frame: device, swap chain, pipelines, textures, render targets, resolves,
present), without upscalers or frame generation.

What helps:

- The SDK already has most of the Vulkan pieces, from Xenia: the guest shader translator to
  SPIR-V (`pipeline/shader/spirv_translator*`, about 10,700 lines), the device, presenter, upload
  pools, descriptor set allocators and VMA (`host/vulkan`), and a whole Vulkan command processor
  to read (`vulkan/`, about 20,000 lines). The patched SDK tree builds for Linux as it is.
- The game side of ReXNative (the hooks of the guest's D3D calls, command buffer replay, the
  layers `rexnative prepare` writes) does not depend on the API.

What stands in the way:

- **The guest shaders.** ReXNative feeds the SDK's DXBC translator its own modifications and
  root signatures (`shader_translate.cpp`). The SPIR-V translator expects Xenia's Vulkan binding
  model (its descriptor sets for shared memory, constants, textures and samplers): the pipeline
  layouts have to follow `vulkan/pipeline_cache.cpp`.
- **ReXNative's own shaders**: about ten HLSL sources compiled at run time with `D3DCompile`
  (resolve, present, render target copies, upscaler input, overlay, frame generation's HUD,
  DLSS 5). On Vulkan: compiled with DXC to SPIR-V (`-spirv`, explicit `[[vk::binding]]`) at
  build time.
- **D3D12 all through the core files**: `draw.cpp` (5,800 lines), `render_targets.cpp`,
  `resolve.cpp`, `textures.cpp`, `present.cpp`, `pipeline_store.cpp`, `d3d12_device.cpp`; 49 of
  the renderer's files use D3D12 types. Resource states become image layouts and barriers,
  descriptor heaps become descriptor sets (descriptor indexing, which RADV has), root constants
  push constants. Either a thin layer both backends share, or Vulkan twins of those files.
- **Windows in the renderer**: WRL `ComPtr`, `LoadLibrary`/`GetProcAddress`, `d3dcompiler_47`,
  the HUD's ImGui D3D12 backend, `renderer/rexnative.cmake`.
- **What has no Vulkan side yet**: DLSS and DLSS 5 (NVIDIA only, never on the Deck), the
  FidelityFX SDK's DX12 upscaler and frame generation (FSR 3.1 has a Vulkan backend; FSR 4 on
  the Deck comes through vkd3d-proton today). Left out of the first step.

Effort: the core draw path to a simple game's right picture on Vulkan (Tetris Splash, Geometry
Wars) is about 8,000-12,000 new or changed lines, some three to four weeks of sessions; the
same games as on D3D12 (MSAA, EDRAM aliasing, memexport, tessellation, the per-game fixes) two
to three months more. It can be built and checked on the PC first: the same code runs on
Windows' Vulkan, with RenderDoc.

### What it would gain: a heavy game measured

Jet Set Radio's start, with no input, is the same 3D sequence of 1,100 to 1,450 draws a frame
every time (four sessions on the Deck agree): a heavy draw path, and repeatable.

**On the Deck** (under Proton, 30 seconds after 40, `deck-measure.sh` with `HEAVY=800`):

| | FPS | Draw CPU per frame | per draw | GPU busy | SoC power |
| --- | --- | --- | --- | --- | --- |
| Upscaler off | 30.0 (the game's cap) | 1.23 ms | 0.97 µs | 3-9 % | 9.6-10.6 W |
| Upscaler on (FSR native AA, the Deck's default) | 29.9 | 2.62 ms | 2.08 µs | 16-20 % | 12.0 W |

The game's busiest threads over the window: its main thread at 100 % of a core (the game's own
code: at a steady 30 FPS, waiting in a loop), the Xenos command thread 21 %, ReXNative's render
thread 4-8 %, its list thread 4 %, vkd3d-proton's queue thread 4 %. Neither the GPU nor
ReXNative's threads are anywhere near their limit.

**On the PC, the same executable with Windows' D3D12 and with vkd3d-proton**
([pc-vkd3d-ab.ps1](../scripts/linux/pc-vkd3d-ab.ps1); vkd3d-proton 3.1 and DXVK's `dxgi.dll`
taken from the Deck's GE-Proton 11-12, beside the executable of a copy of the game; Ryzen 5
5600X, RTX 5060 Ti; runs alternated, another game running on the PC meanwhile):

| Run | D3D12 | Draws a frame | Draw CPU per draw | Process CPU (% of a core) |
| --- | --- | --- | --- | --- |
| A1 | Windows' | 1,235 | 0.617 µs | 114 % |
| B1 | vkd3d-proton | 1,212 | 0.579 µs | 121 % |
| A2 | Windows' | 1,215 | 0.564 µs | 114 % |
| B2 | vkd3d-proton | 1,229 | 0.569 µs | 122 % |

vkd3d-proton records ReXNative's draws as fast as NVIDIA's own D3D12 driver: the same cost per
draw within the noise (another vkd3d-proton run gave 0.72 µs). What it adds is its queue thread,
up to about 7 % of one core in all.

So, for the games measured, a Vulkan backend would not make them faster on the Deck. The draw
path costs 1-3 ms of a 33 ms frame, vkd3d-proton's part of it is not measurable, and the GPU is
mostly idle. The frames the Deck's logs show missing their target have fewer draws than the
others; where the GPU is the limit (Overlord II's GPU time per frame: median 17 ms, 90th
percentile 33 ms, with the upscaler), Vulkan would draw the same shaders. A Vulkan backend
would be worth it to run without Proton or Wine at all, not for speed. For that alone,
ReXNative's D3D12 code on a native `libvkd3d-proton.so` (above) does it for much less work,
since vkd3d-proton is no slower than a native driver.

That shorter way is the one taken above ("ReXNative on Linux, without Wine"): vkd3d-proton's
native build, which its documentation calls development-oriented, works for the two games tried.
