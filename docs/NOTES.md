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

## Fibers

Double Dragon Neon and Dungeons & Dragons: Chronicles of Mystara use XAPI fibers. Their five fiber
functions are found by shape (the current fiber at +356 of the thread, `ERROR_ALREADY_FIBER` /
`ERROR_ALREADY_THREAD`, SwitchToFiber's register save, the start-up code CreateFiber points at)
and replaced by the SDK's host-fiber versions (`[rexcrt]` in `crt.toml`): recompiled,
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

## Recorded command buffers

Iron Brigade's static scenery was missing (yellow): the pixel history of a RenderDoc capture
showed no G-buffer draw on a building whose depth was there. Its mesh was drawn only in the
shadow and depth passes, and the push-buffer trace showed two large INDIRECT_BUFFERs into
ordinary memory: command buffers recorded on a second device and run each frame with
RunCommandBuffer, whose draws no hook saw.

RunCommandBuffer is now hooked (`overrides.json` `functions.RunCommandBuffer`), and
`renderer/src/native/cmd_replay.cpp` reads the object's chunk list and walks its PM4: register
writes, constants, shader loads, predication, and DRAW_INDX packets as native draws, with the
state the call inherits and keeps as the GPU has it. A buffer is checked whole before anything
is applied. In Iron Brigade's first mission that is 2 buffers and 343 draws a frame, about
0.5 ms of decoding. `--rexnative_cmdbuf` 0 off, 1 count and log, 2 replay (the layer's
`kDefaultCmdBuf`); `--rexnative_d3d_control` takes `cmdbuf 0|1|2`, `cmdbuf log N` and
`cmdbuf dump [tag]` (each packet of the next two buffers into the trace folder); draw-debug.txt
bits 4096 and 8192 skip the replayed draws or all others.

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
