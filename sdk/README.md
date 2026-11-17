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
| 0021, 0023, 0024, 0026 | Steam Deck performance (pgr4-native): shader constant writes off the special register path, and an unchanged constant or range changes no binding; no XMA worker wake after an inline kick decode; a single-range shared memory request without the vector, sort and merge; the xmaframes decoder flushed instead of reopened (`thirdparty/ffmpeg-overlay/wmaprodec_xmaframes.c`) |
| 0022 | Guest physical memory in a section of its own, committed when it is created: under Wine a protection change on a view of a section that commits on demand goes through wineserver (pgr4-native) |
| 0025 | The entry XEX's image size checked against the one codegen recorded: a missing or different title update stops with an error instead of running code over another image (pgr4-native) |
| 0027 | A receive on a datagram socket that was never bound fails with `WSAEINVAL`, as on Windows and the console: under Proton it waited forever (Castlevania: Symphony of the Night and 3D Ultra Minigolf stopped at their first frames on the Steam Deck) |
| 0028, 0029 | Pixel shaders (pgr4-native): computed-LOD fetches sampled with `sample_b` instead of derivatives and `sample_d`; `Modification::pixel.textures_unsigned`, set by the renderer when every texture the shader reads is unsigned (`SpecializePixelTextures`): the signed sampling and the sign, gamma and exponent conversions go |
| 0030 | XAM gets a title workspace that follows the current heap: the one it kept a reference to was replaced when the executable asks for another workspace size, and Brothers - A Tale of Two Sons crashed in `XamAlloc` |
| 0031 | A fetch constant written again with its own value still drops its vertex buffer's residency (0021's early return had kept it: a buffer the title refilled in place was drawn with the previous draw's vertices on ReXGlue's renderer) |
| 0032 | An overlapped receive (`WSARecvFrom` with an `OVERLAPPED`) on a datagram socket never bound fails too, as 0027's blocking ones do |

Left out, as the v0.11 port notes record: pgr4-native's statistics patches (0018, 0032), and the
prebuilt resolve shaders of the v0.10 series' `0002` (v0.11 has no sources to rebuild them from).
Not needed on v0.11, which has equivalents of its own: pgr4-native's other Deck pacing and lock
patches (0009, 0011-0013, 0015-0017, 0020) and the key bind repeat (0019). Fixed upstream
since: the `XThread::Resume` and timer queue patches, the physical write fault race, the
achievement toast's repaint, the shared memory cleanup on Linux.

## Licenses

ReXGlue is BSD-3-Clause, with code from Xenia (BSD-3-Clause). Several pgr4-native patches port
code from Xenia Canary (BSD-3-Clause; each names the Canary commit and files); pgr4-native's
patches are MIT ([THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md)). The renderer's FSR 1
shaders (`renderer/src/native/shaders`) are the prebuilt bytecode of the ReXGlue v0.10 SDK's
presenter (BSD-3-Clause), built from AMD's FidelityFX FSR 1 (MIT). No game data is in any of them.

## Updating

To move to another ReXGlue commit: change `rexglue.lock`, check that commit out in a clone of
your own and apply the patches there as commits (`git am --3way sdk/patches/v0.11/*.patch`),
fixing each one that no longer applies (`git am --continue`); then write them again from that
tree (`git format-patch <lock>..HEAD`) and build it with `scripts\build-sdk.ps1 -SdkSrc <clone>`.
`build-sdk.ps1` itself applies them with `git apply` (no commits) and stops at the first that
fails: a stopped run's tree is half patched, so delete it before running again.
