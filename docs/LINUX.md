# Linux and Vulkan (experimental)

> [!NOTE]
> An experiment on the `vulkan-experiment` branch: can recompiled games run faster on the Steam
> Deck with Vulkan, and without Proton? Not part of `main`.

ReXNative draws with Direct3D 12, so a game drawn by it is a Windows executable and runs on the
Steam Deck under Proton (vkd3d-proton turns the D3D12 calls into Vulkan). ReXGlue itself builds
for Linux, and its own renderer, the Xenos GPU emulation, has a Vulkan driver (`gpu.driver`:
`d3d12`, `vulkan` or `any`). This page has what was measured and how a game is built and run on
Linux without ReXNative.

## What was measured

Tetris Splash on the Steam Deck (SteamOS 3.9, RADV, 1280x800, upscaler off), its title screen,
30 seconds after 30 seconds of warm-up. Frame times from MangoHud (every frame), the GPU's busy
share and the SoC's power from `/sys/class/drm/card0/device`, the game's CPU time from `/proc`
(100 % = one core). Every setup started from the game's Steam shortcut.

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

What it would gain, going by the table above: ReXNative's GPU work on the Deck is already small
(1-2 % on this game), and dropping Proton saved 8 % of the game's CPU. The gain is CPU time per
draw: ReXNative's stats show about 4 µs of CPU per draw call on the Deck (its own code and
vkd3d-proton's under it), which matters in games with thousands of draws a frame and little in
simple ones. Measuring one
such game on the Deck (CPU time per frame, and how much of it is in vkd3d-proton) would say
whether it is worth that much work.

A shorter way to the Linux part alone: vkd3d-proton also builds as a native Linux library
(`--native`, `libvkd3d-proton.so`, with a swap chain on native surfaces since 2.9), so ReXNative's
D3D12 code could run on Linux without Wine once its Windows calls are replaced. Its own
documentation calls that build development-oriented, and the GPU work would stay as it is.
