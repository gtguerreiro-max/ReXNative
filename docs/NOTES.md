# Engineering notes

What each game needed, and how the problems behind it were found. The [README](../README.md)
says what ReXNative does; this is the record of why it does it that way.

## Finding a game's functions

`project --find-functions` (`rexnative/functions.py`) finds the function starts codegen missed,
ahead of time, in the loaded image (`--rexnative_dump_image`): anything pointed to, built with
`lis` + `addi`, with a prologue, or left between functions after code that does not fall through
(Super Meat Boy 3523, Jet Set Radio 4280). The XDK's register save and restore helpers are never
made functions; codegen handles their calls.

- **Jump tables** codegen read short are found from their bound check (`cmplwi`, `bgt`,
  `lis`/`addi`, `lwzx`, `bctr`) and given whole as `[[switch_tables]]` (Double Dragon Neon trapped
  on its cases 16 to 143).
- **Gotos** to labels of other functions become functions (Minecraft).
- **Imports** the runtime lacks become stubs (the Vision camera, `XUsbcam*`).
- **One function per layer address** (Perfect Dark, Bionic Commando Rearmed).
- **Tail calls** (`b`) to an address in no function, which codegen warns about and writes as a
  call with no name that does not compile (Orcs Must Die!), become hints.
- **A hint that splits a function**, whose loop then branches back into another function (codegen
  writes a `REX_FATAL` there: Brothers - A Tale of Two Sons stopped at its first frames), is
  removed and kept out (`not-starts.txt`).
- **Millicode**: Jet Set Radio links the register save and restore helpers twice; ReXGlue v0.11.0
  stopped at the first copy's pdata. `sdk/patches/v0.11` finds every copy and names them apart,
  and `prepare` drops the hints codegen rejects there.
- `project --discover N` runs the game up to N times; each function it reports missing (a thread
  entry, an indirect call) becomes a hint, then codegen and the build run again.

Project names are one word: ReXGlue's CMake glue runs a name's words together
(`jetsetradio_pch.h`) where its sources keep them apart.

## setjmp and longjmp

All the games tried have them. Without the addresses, a `longjmp` is recompiled as a register
restore and a return into the wrong frame: the four games that stopped at start-up (Jet Set
Radio's and Super Meat Boy's libjpeg error path, Minecraft, 3D Ultra Minigolf) stopped there.
The tool finds the XDK CRT's setjmp and longjmp by how they save and load f14-f31 in the
`jmp_buf`, and the incremental linker's thunks to them, and gives them to codegen
(`setjmp.toml`), so those calls become the host's setjmp and longjmp. Codegen takes several
addresses with an SDK patch (Super Meat Boy calls setjmp through two thunks).

## Registers a function takes from its caller

The projects use ReXGlue's `non_volatile_as_local`: r14-r31, f14-f31, v14-v31 and v64-v127 are
locals of each generated function, zero at its start, since the ABI says a function does not take
them from its caller. Some code does: an internal helper the 360 compiler gave register arguments
(constants in v64-v127, kept by the caller across a loop of calls), or a function codegen split
from the one it belongs to. That function reads zero. Fable II's animation decoder read its
caller's v77-v94 as zero, decoded every rotation key as zero, and the NaN quaternion that made
went to the camera: the world drew black. The XDK's video decoder (XMV: WMV3, VC-1) passes v64,
v65, v69 and v72 to its helpers the same way: Jet Set Radio's, Syndicate's and Dark Souls II's
videos came out as green and magenta bands, while every instruction passed its tests.

After each codegen, `prepare.live_in_nonvolatiles` reads the generated code for functions that
read such a local and never write it; they go to `shares.txt` and get `share_registers = true`
in the manifest (ReXGlue then keeps their registers in ctx, and their callers copy their locals
there around each direct call), and codegen runs again. `project --build` does the same for a
game prepared before (once per generated code, `shares.scanned`). A helper reached only through a
pointer is not covered: an indirect call copies nothing.

Two things went wrong with it in Minecraft, whose new worlds crashed after the batch rebuild of
2026-11-18 (a read of guest 0x438 on the "(4J) Post processing" thread):

- **A false live-in.** `subfe r29,r19,r19` names r19, but its result is ~r19 + r19 + CA, the
  carry alone. The scan now skips registers named only as both sources of such an idiom (`subf`,
  `subfc`, `subfe`, `xor`, `andc`, `eqv`, `orc`, `vxor`, `vandc`, `vsubu*m`).
- **A shared function's registers lost through a call.** A shared function keeps r14-r31 in ctx.
  When it called the allocator's free (a function with locals) and free called a shared
  function, free copied its locals into ctx for that call and never put ctx's own values back:
  a function with locals skips the save and restore helpers. The shared caller came back with
  free's r25-r31. SDK patch 0034: the copy into ctx keeps ctx's values and the copy back
  restores them. Generated code from before it keeps the fault until codegen runs again.

## The upscaled pass of a scene drawn in passes

The upscaler takes the frame's last pass of the scene's surface whose draws write depth
(`Upscaler::SceneResolveDue`). Diablo III's menus draw the hero into the scene's surface with
depth, resolve it, and draw the menu (backdrop, the hero from that resolve, particles) in a second
pass that tests depth and writes none: the hero's pass was upscaled, and the hero came out
invisible with FSR and black with DLSS. `rexnative.upscaler_depth_test = true` (Diablo III's
settings) counts the draws that test depth too, so the second pass is the one upscaled.

## Fibers

Double Dragon Neon and Dungeons & Dragons: Chronicles of Mystara use XAPI fibers. Their five fiber
functions are found by shape (the current fiber at +356 of the thread, `ERROR_ALREADY_FIBER` /
`ERROR_ALREADY_THREAD`, SwitchToFiber's register save, the start-up code CreateFiber points at)
and replaced by the SDK's host-fiber versions (`[shim]` in `crt.toml`; `[rexcrt]` before v0.11): recompiled,
SwitchToFiber's stack switch cannot work (Double Dragon Neon's Play wrote into another fiber's
stack).

## Title updates

`prepare --title-update` installs the newest title update XboxUnity lists for the game's title,
media id and version before codegen (`rexnative/title_update.py`; `rexnative title-update
<work>` lists them without downloading). The package is checked against the listing (its content
id, the SHA-1 of its header, and every block of its hash tree). Its XEX patches go beside
`default.xex` (ReXGlue's loader applies `default.xexp` for codegen as for the game, so the code
is recompiled from the patched image), the rest of it is mounted as `update:`
(`run/title-update`), and what came from the old image is made again.

## Disc images

Disc games come as images of the disc (`.iso`, on its own or in an archive): `rexnative/xiso.py`
reads its XDVDFS partition (an image of the game partition or of the whole disc, XGD2 or XGD3)
and copies the game's files out, without the console's `$SystemUpdate`.

## Devices and draws

- **Two devices.** Halo: Combat Evolved Anniversary draws on two devices from two threads, which
  raced in the renderer's queue: only the device that presents is followed. A CreateDevice that
  never reads its `ppDevice` argument (r8) is not Direct3D_CreateDevice (Brothers' D3D 2.0.21250
  creates its device in a function of the presentation parameters alone): the renderer then
  follows the first device the calls use.
- **Inline draws.** Iron Brigade's black intro video was not a memexport: Scaleform draws the
  decoded frame with DrawIndexedVerticesUP (inline vertices and indices, which no hook saw; the
  tool now finds BeginIndexedVertices next to DrawVerticesUP, and after BeginVertices when
  DrawVerticesUP is missing: Brothers' 68 post-process draws a frame were unseen), into a
  `k_8_8_8_8_GAMMA` target the renderer wrote linear, half as bright. Gamma targets are now in
  the console's gamma (`--rexnative_gamma_targets`); a draw that blends into one goes through an
  sRGB view, so it blends in linear color as the console does.
- **Predicated tiles.** Double Dragon Neon's scene is four predicated tiles side by side, 480 x
  1080 each of 1920 x 1080, so host targets are as wide as the tiles reach, not the EDRAM pitch.
- **Resolve's level is never a pointer.** Fable II's Resolve mapping, voted from the reference
  game, read the level from r7, where its EndTiling passes each tile's destination point (a stack
  address): every scene resolve had level ~0x7000xxxx and was dropped, and the world drew black.
  `adapt` now keeps the public signature when the calls set the register a mapping reads the
  level or slice from to stack addresses more often than the public one (Deus Ex: Human
  Revolution's mapping had the same fault).
- **The Resolve's clear colour shows where its arguments went.** Deus Ex's LTCG Resolve dropped
  the level and the slice: the clear colour is in r8, ClearZ in r9's slot and the stencil in r10,
  and the function never reads r9 as an argument. Read with the public signature, each slice came
  from r9's leftover and moved the destination by billions of bytes. `adapt` now finds the colour
  as the argument the function tests against 0 and replaces with a vector it builds in its own
  frame (`abi.clear_color_arg`), takes the level and the slice only from the registers below
  it, and makes an argument the function writes before reading it at its entry a constant 0
  (`abi.reads_at_entry`): Deus Ex `{3,4,5,6,7,0,0,8}`, Fable II `{3,4,5,6,7,8,0,10}`.
- **DXT3A.** Fable II's terrain patches (tessellated, blended `SRC_ALPHA, ZERO`) take their alpha
  from a k_DXT3A texture fetched in the vertex shader, compared against one 4-bit step. The
  format was unsupported, the fetch read 0 and the ground drew black under the sky. It is now
  decompressed into R8 as Xenia does (each block is BC2's alpha half, a * 17): a lossy BC4 would
  move values across that step. Found with RenderDoc's pixel debugger (`rdoc.ps1 debug`): the
  interpolator holding the alpha was `1.07 * (fetch - 1/15)` with the fetch 0.
- **Resolves into k_1_5_5_5.** Fable II resolves three 256x256 mip chains a frame into 1:5:5:5
  textures, which the resolve refused. They are written like k_5_6_5's without a typed store:
  R16_UINT texel bits in the cache's B5G5R5A1 layout, then copied into the image.

## Recorded command buffers

Iron Brigade's static scenery was missing (yellow): the pixel history of a RenderDoc capture
showed no G-buffer draw on a building whose depth was there. Its mesh was drawn only in the
shadow and depth passes, and the push-buffer trace showed two large INDIRECT_BUFFERs into
ordinary memory: command buffers recorded on a second device and run each frame with
RunCommandBuffer, whose draws no hook saw.

RunCommandBuffer is now hooked (`adapt` finds it and turns the replay on when it is sure of it;
`overrides.json` `functions.RunCommandBuffer` and `defaults.cmdbuf` correct it), and
`renderer/src/native/cmd_replay.cpp` reads the object's chunk list and walks its PM4: register
writes, constants, shader loads, predication, and DRAW_INDX packets as native draws, with the
state the call inherits and keeps as the GPU has it. A buffer is checked whole before anything
is applied. In Iron Brigade's first mission that is 2 buffers and 343 draws a frame, about
0.5 ms of decoding. `--rexnative_cmdbuf` 0 off, 1 count and log, 2 replay (the layer's
`kDefaultCmdBuf`); `--rexnative_d3d_control` takes `cmdbuf 0|1|2`, `cmdbuf log N` and
`cmdbuf dump [tag]` (each packet of the next two buffers into the trace folder); draw-debug.txt
bits 4096 and 8192 skip the replayed draws or all others.

Midnight Club: Los Angeles (XDK 6995) draws its cars from 67 recorded buffers a frame, which
the replay first refused and then drew 512 rows up:

- its chunk lists live in physical memory's 0xA0000000 view (64 KB pages), not only 0xC0000000;
- its D3D writes an entry's dword count alone, leaving the high byte as the heap left it (0xCD);
- each buffer carries one setup per predicated tile under SET_BIN_MASK of the tile's bins, and
  the last, tile 2's, moves the window 512 rows up (PA_SC_WINDOW_OFFSET 0x7E000000). The replay
  draws once over the whole surface, so those predicated window writes become offset 0 and the
  union of the tiles' scissors;
- the device's bin mask that a buffer starts from moves with the XDK (12956 for 20353/20500,
  12708 for 6995, 13084 for 21xxx, and others): the generator reads it where the segment start
  writes SET_BIN_MASK_LO/HI (`constants.bin_mask_offset`) and the layer gives it as
  `kCmdBufBinMaskOffset`. Read at the wrong offset, every predicated draw was skipped.

`cmdbuf dump tag:N` dumps the next N calls (a whole frame of Midnight Club's is 67).

## Inline draws

Orcs Must Die!'s build inlined DrawIndexedVertices into its title, menus, loading screens and
HUD: its DRAW_INDX packets, with the state and the immediate vertex shaders before them, were
written straight into the push buffer, where the trace shows them as `fn=-`, and the native
renderer drew one quad a frame. With `rexnative.inline_draws` (a game's setting), every hooked
call on the presenting device, from the Swap thread, first walks the stream written since the
previous outermost hooked call returned (the device's push cursor then and now) with the
recorded-buffer walker: register writes into the tracker, shader loads, draws to the renderer.
After a Begin call that stream is vertex data and is skipped. A stream that runs into a hole
the D3D reserved is walked up to the packet the check refused. The shaders a stream loaded are
the inlined draw's: the next hooked draw reloads the bound ones, and an ImmediateVs that the
inlined code called is not left pending for it ("Press Start", drawn by DrawVerticesUP, got the
inlined draw's vertex shader and showed one letter).

A top-level BeginVertices only reserves room for vertices: its draw is held until the game has
written them, at the entry of RestorePush (EndVertices) or of the next call that flushes state.
Fable II inlines EndVertices, so its tonemap rectangle (7 textures into the 1280 x 720 target)
waited for the next flushing call, and the walk of the stream before that call drew the HUD's
inline draws first and left their registers on the tracker: the rectangle came last, with the
HUD's shaders, and the world stayed black. The held draw now goes out at the entry of any hooked
call on the walked device, before the walk (no D3D call comes between BeginVertices and
EndVertices).

Fable II's buildings and characters were missing in Bowerstone: about 1100 DrawIndexedVertices
a frame, of which the native path drew none. Its own code loads each mesh's shaders between the
hooks, a predicated IM_LOAD pair per pass (bin masks 0x15555555 for the depth pass, 0x2AAAAAAA
for the color pass), with SetVertexShader(NULL) and an older pixel shader object still bound,
and dirties mask +16 bit 17 for the D3D's SQ_PROGRAM_CNTL fields. The tracker cleared the vertex
shader for the null object (876 draws a frame "no vertex shader") and, taking bit 17 as a
rebind, reloaded the old pixel shader object (every mesh white). A shader the stream loaded now
stays until the D3D loads that shader again under its own bit (19 vertex, 20 pixel), as on the
console. The draw log (`draw-debug.txt` 16) names the reason of each skipped draw.

## Motion vectors from a vertex ring

The upscalers' motion vectors come from each scene draw's vertex shader run a second time with
the previous frame's float and vertex fetch constants of the draw it matches
(`motion_history.h`). Jet Set Radio transforms its vertices on the CPU (the Dreamcast's way) and
writes each frame's into one of two 4 MB buffers in turn, every draw at its own first vertex
(`VGT_INDX_OFFSET`). The second run read the previous frame's buffer at this frame's first
vertex: another mesh's vertices. On a still scene the motion was 314 pixels at the median (a
RenderDoc capture of the motion target), FSR followed it, and the characters lost their black
outlines and sometimes their heads, with any upscaler mode, even at the native size. Bisecting
the draws with motion off (`draw-debug.txt` 32768 with a range) found the character draws; the
pixel history showed their outline failing the depth test at another mesh's depth.

A draw now keeps its first vertex and its vertex buffers' addresses in the history. When the
matched draw used another buffer, its vertex fetch constants are moved by the difference of
the two first vertices times each binding's stride, so the second run reads the previous
frame's own vertices. A buffer the draw kept was written over this frame: no motion there.
With the camera following the skater, the median is now 4.5 pixels and 3 % of the pixels move
more than 64 (draws matched to another mesh of the same vertex count).

Those were more in the attract demo: 8 to 33 % of a frame's pixels off the camera's motion by
over 256 pixels. A log of each motion draw's match (`draw-debug.txt` 262144), tied to a capture
by the order of the draws writing the motion target, showed why: the road was matched with a
building a third of the screen away. Jet Set Radio clips its polygons on the CPU, so a mesh's
index count changes from frame to frame and the previous frame often has no draw of this one's
identity that is the same mesh; among the draws that have it, the first in order was taken. Each
draw now also keeps its first vertex (the first binding's first attribute of 3 or 4 floats, and
its first of 2: `anchor`). Among previous draws as alike, the nearest is taken; and a match whose
first vertex is further than 4 times the lower quartile of the last frame's distances (at least
128 units: the camera's move over most draws) is another mesh, so the draw moves with the camera
instead. Over ten captures taken in turn with and without (`draw-debug.txt` 65536), the pixels
off the camera's motion by over 64 pixels went from 10 to 36 % to 0 to 0.3 %. For a game whose
vertices stay in model space (Iron Brigade, PGR4) a draw's own mesh is at distance 0, and the
limit only refuses a match with another mesh's buffer.

The same counts find Jet Set Radio's camera cuts, which FindCamera cannot: its constants hold
the projection alone, so no view turns. A frame in which three quarters of at least 64 compared
matches were refused starts the upscaler's history over (`Upscaler::NoteMotionMatch`). In 480
frames of its attract demo the refused fraction stayed under a tenth in 439, at 20 to 30 % in the
fastest pans, and above 90 % once, at a cut.

## Inline draws, found at run time

`rexnative.inline_draws_auto` (on by default) watches each Swap: 30 frames in a row with at most
2 hooked draw calls while the game writes 8 KB or more of push buffer is what Orcs Must Die!'s
title and menus look like. The walk is then tried for 60 frames and kept only if it drew
something; else it goes off again (every pass-through hook runs `OnHook` while it is on) and is
tried again after 600 frames, twice as many each time. Jet Set Radio's intro video and Perfect
Dark's loading screens trip the first test and fail the second; run with the walk forced on,
none of the other 14 games had a draw between its hooked calls.

Deus Ex: Human Revolution drew its title black with 126 hooked draws a frame. In a RenderDoc
capture every vertex shader output position was 0: they read c0-c3, which the tracker held at
0. The push-buffer trace (`live.ps1 trace`) showed why: beside each hooked draw the game's own
code writes a `SET_SHADER_CONSTANTS` of c8-c15 and a `LOAD_ALU_CONSTANT` of c0-c7 from a ring
of its own, the XDK's GPU-owned constants, which never pass a hooked call or the device's
shadow. The walk applies them; so every 150 frames, whatever the draw count, the walk now runs
for 3 frames as a probe that only validates the gaps and counts the shader constant registers
they set to another value than the tracker holds, and stays on at 16 or more (Deus Ex: about
200 in 2 frames at its first probe). Sega Rally Revo's race turns it on as well, with thousands,
and draws as it did without it; Brothers' title and menu find nothing (Fable II has the walk on
already, for its inline draws).

## Memexport

Memexport draws (shaders that write to memory) run with shared memory as a UAV
(`--rexnative_memexport`). Their pages stay the GPU's until the CPU writes them, and what they
changed is written back into guest memory before a texture over it is read and at the Swap
(`--rexnative_memexport_readback`, a wait for the GPU; Xenia Canary's snapshot and diff). A byte
the game wrote after the memexport draw is kept (the GPU's write came first on the console);
copied over it, Iron Brigade's level loading lost a mesh to old texture blocks.

Iron Brigade's engine moves resources with a memexport shader that copies 64-byte blocks, about
140 times a frame. Each needed a readback that waited for the GPU (7 to 17 ms a frame on the
Steam Deck: the stutter), and its meshes sometimes came out stale (huge yellow triangles). Known
copies are now done in guest memory when the game issues them and never reach the GPU
(`--rexnative_gpu_memcpy`); a memexport draw also waits for its pipeline instead of being
skipped while it compiles (what it writes would be lost for good).

## EDRAM aliasing

- **Across MSAA:** a 4x (or 2x) depth surface over a 1x one at the same base takes its depth
  (each sample is a pixel of the 1x surface), as Iron Brigade's half-resolution particles need to
  be hidden by the scenery (`RenderTargets::AliasDepth`).
- **Across formats:** Brothers' post processing reads the 7e3 scene's EDRAM as 10:10:10:2 and
  8:8:8:8 bits (`RenderTargets::AliasColor` converts them between targets).
- **Inside a resolved image:** Brothers' colour grading table is a stacked texture of 16 slices
  that one 16x512 resolve wrote, a slice every 32 rows (`Resolver::FindContaining`: a slice or
  level inside a resolved image is copied from its rows; only the first slice was).

## The renderer switch keeps the window

The renderer switch (F9, the HUD's Renderer, `rexnative.classic`, `rexnative.auto_handover`)
gives the game to ReXGlue's Xenos renderer and back. The game window used to go with it: ReXNative
released its swap chain and ReXGlue's presenter made one. Under vkd3d-proton that never worked:
once a swap chain on the window was released, the next one's Present waited for good (Orcs Must
Die! stayed black at its title), so the switch was off under Proton and a game the native renderer
drew wrong had to be set to one renderer for the whole run.

The window now keeps ReXNative's swap chain whichever renderer draws. While ReXGlue's renderer
draws, each guest Swap has its D3D12 presenter copy the frame it would have shown (its guest
output, the gamma ramp applied) into one of three textures of ReXNative's, on ReXGlue's direct
queue, which then signals a fence of ReXNative's; ReXNative's queue waits for that value before the
frame that shows the texture (`renderer/src/native/classic_frames.h`, SDK patch 0033). The texture
is drawn as ReXNative's own front buffer is, scaled and with FSR 1, and the HUD goes over it, so
the HUD can always switch back. The two queues must be on one `ID3D12Device`. D3D12 and
vkd3d-proton give one device per adapter (`D3D12CreateDevice` returns the one made before), and
they were the same on the PC (an RTX GPU) and on the Steam Deck. Where they are not (two GPUs
picked apart), the window goes to ReXGlue's presenter as before, and under Proton the switch stays
off. An XAM dialog still goes to ReXGlue's presenter, whose overlay draws it.

## Pipelines compiled ahead

The pipelines a game creates are stored (`<cache_root>\rexnative\pipelines.bin`, plus a D3D12
pipeline library) and all of them are compiled again at start-up, in the background, most
recently used first (at most 4096); none is forgotten (`--rexnative_pipeline_prewarm_sessions`
and `--rexnative_pipeline_keep_sessions` limit them to the last N sessions). Only a pipeline the
game never created before still holds a frame while it compiles (50 to 170 ms on the Steam
Deck). A stored pipeline's use used to be marked on another record (the records were sorted
apart from their hashes), so pipelines in use could be forgotten.

## Validating the adapter

Each hand-made port's layer was regenerated without using that port, and compared with the one
made by hand (`python -m rexnative validate <port>`):

| | Hooks | Device offsets | Hook list (`d3d_surface.inc`) |
|---|---|---|---|
| Iron Brigade from From Dust | 24 of 25 the same, +1 found | 30 of 30 | 82 of 83, +4 |
| From Dust from PGR4 (another XDK) | 24 of 25 the same | 30 of 30 (11 had moved) | 104 of 104, +37 |

The differences were, on inspection, mostly mistakes in the hand-made layers. Iron Brigade passed
0 for Resolve's `slice` argument, which its inner Resolve does read. Its hook list had one false
match. From Dust read PGR4's default clear colour address. From Dust's scene surface was still
PGR4's, so the upscaler never saw its scene. The `slice` and the clear colour are fixed in the
ports' sources. The one hook the tool did not find in From Dust was inlined by the game; the one
it added to Iron Brigade is its CreateDevice.
