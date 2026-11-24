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
| 0033 | The D3D12 presenter's guest output for a host renderer that presents the title itself (`rex/xenos/draw_hook.h`, `CopyGuestOutput`): copied into the host renderer's texture on the driver's queue, which signals the host renderer's fence. ReXNative shows ReXGlue's frames through its own swap chain while ReXGlue's renderer draws, so the window never changes hands (under vkd3d-proton it could not) |
| 0034 | Codegen: a function with its nonvolatiles in locals that calls a register-sharing function puts ctx's own values back after the call (a shared caller further up keeps its registers there; Minecraft's new worlds crashed) |
| 0035 | 0033's copies keep the guest output they read until they are done: a front buffer of another size released the image a copy still read |
| 0036 | Asynchronous reads and writes return STATUS_PENDING as the console does (Xenia canary's rule: always in a content package, otherwise only unbuffered ones that succeeded); the request is still done at once. Minecraft's Load tab waited forever for a pending read of its save list |
| 0037 | A XEX header patch runs its whole delta list (the descriptor's size, as Xenia), not only the first entry: a DLL module's title update patch starts with copies, and Forza Horizon's two modules failed the key check ("image key does not match", status 9) |
| 0038 | A fake occlusion result (`query_occlusion_fake_sample_count`) is a running sample counter written to every EVENT_WRITE_ZPD report, BEGIN or END, as Xenia canary's fake mode: writing 0 where D3D left no 0xFFFFFEED read every query of a QueryBatch but the last as 0 samples, and Halo: Combat Evolved Anniversary culled its menu's ship and ring |
| 0039 | `XamSwapDisc` sets its completion event when the title asks for the disc that is running (the execution info's `disc_number`), as Xenia canary does: the stub never set it, and Deus Ex: Human Revolution Director's Cut waited on its "insert disc 1" screen as a new game started. Another disc is logged and left to the title's prompt |
| 0040 | `kernel.user_name` names the signed-in profile (its gamertag, up to 15 characters); it was always "User", which Diablo III shows under P1 |
| 0041 | The system dialogs (keyboard, message boxes) scale with the window, `ui.dialog_scale` (0: once per 360 lines, 2x on the Steam Deck's 800), and a prompt stays centred: Diablo III's hero name prompt was a small box on the Deck |
| 0042 | The system dialogs answer to the pad and the keyboard (D-pad or left stick to move, A or Enter to press, B or Escape for the last button), read past the title's input block (`IInputSystem::GetStateForUI`, last in the interface); a message box's text wraps at about 45 characters, and its answer is logged. Asura's Wrath's "saved data does not exist" box could only be touched on the Steam Deck |
| 0043 | Pixel shaders: `kSysFlagColor#GammaSrgbView`, the PWL gamma color decoded with the sRGB curve, for a k_8_8_8_8_GAMMA target the host writes through an sRGB view so it blends in linear color: the view's encode then stores the PWL value the console's EDRAM holds. Linear color written as it was came out as the sRGB curve's value, about three times the PWL one near black (Iron Brigade's title screen had grey blacks) |
| 0044 | D3D12: a two-sided draw's back faces take `RB_STENCILREFMASK_BF`'s stencil reference (`OMSetFrontAndBackStencilRef`, where the device has D3D12 options 14), as the Vulkan backend already does; one reference for both faces otherwise. Xenos keeps one for each face; with one for both, a two-sided draw without culling wrote the front's with its back faces (Forza Horizon's foliage, though its picture does not change: docs/NOTES.md) |
| 0045 | Input: `rex_hid_set_pad_overlay` (C linkage, found with `GetProcAddress`) sets a function called on each read of a user's merged pad state, after the deadzones, so `GetState`, the keystrokes made from it and `GetStateForUI` all see what it adds. ReXNative's control file `pad` presses through it: a title screen waiting for its START with `XInputGetKeystroke` (Diablo III's) takes it; the renderer's `XInputGetState` hook alone did not reach the keystrokes |
| 0046 | Pixel shaders: `Modification::pixel.textures_unorm8`, set by ReXNative where every texture a shader reads is unsigned with 8 bits per component: point-sampled fetches read texels as `(x + (x >> 7)) / 256` (0x80 is 0.50390625), not `x / 255`; filtered fetches unchanged. Forza Horizon's motion blur keeps its stencil 0x80 sharp below 0.501968 and the Xbox 360 blurs it (docs/NOTES.md) |
| 0047 | Xenia canary's game patches: a file beside a module in its format (`game:\default.xex.patch.toml`, from its game-patches repository) is applied once the module is loaded, its enabled patches only, when its title id and one of its hashes are the module's (Xenia canary's module hash: XXH3-64 over the code pages, the import thunks rewritten `sc 2; blr; nop; nop`). Codegen and the title load the module the same way, so a code patch is in the recompiled code and a data patch in the title's memory. Every load logs the module hash. ReXNative's FPS Boost (`rexnative/fps_boost.py`) |
| 0048 | `XamGetLanguage` returns the console's language (`kernel.user_language`), as `XGetLanguage` does: the stub returned 0, so a title that asks XAM directly (Diablo III, Geometry Wars 3) stayed in English whatever language was set. The ReXNative Manager's per-game Language sets `kernel.user_language` |

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

`scripts\update-sdk.ps1` moves the SDK to a newer ReXGlue commit, by default the newest nightly
prerelease: ReXGlue tags `nightly-YYYYMMDD-<commit>` on each day its `development` branch moves,
and its releases come from that branch. The script checks that commit out in a clone of its own,
applies the patches there as commits (`git am --3way`) and drops the ones the commit already has
(taken upstream). It then builds and installs the SDK with ReXGlue's unit tests (`build-sdk.ps1
-Tests`), with `-Games` builds and starts games against it, and with `-Promote` writes
`rexglue.lock` and the patches again for a commit here. `-Check` only says whether a newer
nightly exists and whether the patches apply. A patch that conflicts stops the script in the
middle of `git am`: fix the files in the clone, `git am --continue`, and run it again. The
script's header has the details; `<Dest>\update-status.txt` says what a run did.

To move by hand instead: change `rexglue.lock`, check that commit out in a clone of your own and
apply the patches there as commits (`git am --3way sdk/patches/v0.11/*.patch`), fixing each one
that no longer applies (`git am --continue`); then write them again from that tree (`git
format-patch <lock>..HEAD`) and build it with `scripts\build-sdk.ps1 -SdkSrc <clone>`.
`build-sdk.ps1` itself applies them with `git apply` (no commits) and stops at the first that
fails: a stopped run's tree is half patched, so delete it before running again.
