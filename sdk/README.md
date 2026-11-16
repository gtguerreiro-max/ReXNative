# The ReXGlue SDK for ReXNative

ReXNative's renderer builds against the ReXGlue SDK with a fixed set of patches: the ReXGlue
**v0.11.0** release (the commit in `rexglue.lock`), then `patches/v0.11/` in file name order.
`scripts/build-sdk.ps1` fetches, patches, builds (LLVM 20, the SDK's `win-amd64` preset) and
installs it, and checks out the AMD FidelityFX SDK at `fidelityfx.lock`; it prints the paths to
give `rexnative_add()`.

ReXGlue v0.11 reorganised the SDK: modules and GPU drivers are DLLs loaded at run time, the Xenos
GPU code is private (`src/gpu/xenos`), cvars became settings (`--scope.name`), and builds need
clang 20 or later. ReXNative's renderer stays in the game's executable: it compiles the SDK's
shader translator from the source tree (`renderer/src/native/shader_xlate.cmake`), reaches the app
through `renderer/src/host.h` (the window, input, guest memory, the XAM dialogs) and keeps its old
option names as aliases of its `rexnative.*` settings (`renderer/src/compat/rex/cvar.h`).

## The patches

Ported to v0.11 from the v0.10 series (pgr4-native's generic patches, From Dust's SDK changes,
ReXNative's own), plus what v0.11 needed for a host renderer. Each patch's subject says what it
does; in short:

| Patches | What |
|---|---|
| 0001-0004 | Codegen: the manifest left alone when its `sdk_version` is older; a spin-wait hint for `db16cyc`. Kernel: `ObCreateSymbolicLink`'s prefix compared as a plain string. Threading: `Win32Thread` priority from the cached value (pgr4-native) |
| 0005, 0006, 0009 | The host renderer's side of the Xenos driver: the gamma ramp accessors, `gpu.xenos.skip_draws` (the driver keeps fences, write-backs and interrupts but does no draws, resolves or swaps), `gpu.xenos.occlusion_query_external` (pgr4-native) |
| 0007, 0008 | Audio: the XMA loop subframe end, and a frame whose header straddles a packet (pgr4-native) |
| 0010 | Motion vectors for temporal upscalers: vertex and pixel shader modifications that run the vertex shader again with the previous frame's constants (pgr4-native) |
| 0011, 0012 | 8x host render targets get the alpha to coverage of 4x; RTV path colors clamped to the render target format's range (pgr4-native) |
| 0013 | From Dust's SDK changes: the per-draw pixel constant hook (`rex/xenos/draw_hook.h`), integer texture fetches folded into `exp_adjust` (`rex/xenos/integer_fetch.h`), `function_table_base`, `vmaxuw`, the device path module lookup, the `RtlRaiseException` log, the Vulkan stencil bit transfer fix |
| 0014 | Codegen: `setjmp_address` / `longjmp_address` may be arrays (every address a game calls them at) |
| 0015 | `gpu.xenos.present_external`: while set, the Xenos driver takes its presenter off the window (releasing its swap chain) so the native renderer presents into it, and gives it back for the stock renderer (F9) and the XAM dialogs |
| 0016 | The integer fetch without its environment toggle (the SDK builds with `-Werror`) |
| 0017 | `rex/xam/ui.h`: `XAMIsUIActive` exported, so the renderer gives the window back while an XAM dialog is up |
| 0018, 0020 | Codegen: every copy of the millicode helpers found and named apart (Jet Set Radio links them twice; v0.11.0 stopped at its pdata) |
| 0019 | The guest's gamma ramps published by the command processor (`rex::xenos::GetGammaRampView`) |

Left out, as the v0.11 port notes record: pgr4-native's Steam Deck pacing, statistics and lock
patches (0008-0020, 0032, 0042, 0043; to be ported before the Deck build), the computed-LOD and
`textures_unsigned` shader modifications (0021, 0022), the image check (0028), and the prebuilt
resolve shaders of the v0.10 series' `0002` (v0.11 has no sources to rebuild them from). Fixed
upstream since: the `XThread::Resume` and timer queue patches, the physical write fault race,
the achievement toast's repaint, the shared memory cleanup on Linux.

## Licenses

ReXGlue is BSD-3-Clause, with code from Xenia (BSD-3-Clause). Several pgr4-native patches port
code from Xenia Canary (BSD-3-Clause; each names the Canary commit and files); pgr4-native's
patches are MIT ([THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md)). The renderer's FSR 1
shaders (`renderer/src/native/shaders`) are the prebuilt bytecode of the ReXGlue v0.10 SDK's
presenter (BSD-3-Clause), built from AMD's FidelityFX FSR 1 (MIT). No game data is in any of them.

## Updating

To move to another ReXGlue commit: change `rexglue.lock`, run `scripts\build-sdk.ps1 -NoBuild`
into a fresh `-Dest`, and fix the patches that no longer apply; then regenerate them from the
patched tree (`git format-patch <lock>..HEAD`).
