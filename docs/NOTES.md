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
  on its cases 16 to 143). A table with no such check (its index from masks, or checked with `ble`)
  that sits right after its `bctr` is read up to the first of its cases (The Bureau's Bink decoder:
  11 cases read as 8, a trap on the BinkAsy2 thread when its first video played). Codegen reads
  such a table as at most 512 entries (`max_jump_table_entries`). The search runs again on every
  new generated tree, and on a reused one when its rules change (`tables.scanned`), whenever the
  loaded image (`run/image.bin`) is there. Games prepared before a rule was added get it on their
  next build: Crazy Taxi's 699 streamed sound names (82229474, bounded only by its caller) trapped
  on sound 673 when ARCADE was chosen, and Spelunky's 534-entry table had the same cut.
- **Tables whose index is gone at the `bctr`**: Burnout Crash's `crash.dll` builds a table's
  address as `li r27,lo` + `addis r27,r27,hi` and loads with the index first (`rlwinm r3,r3,2,0,29;
  lwzx r3,r3,r27; mtctr r3; bctr`), so no register holds the index at the jump. ReXGlue's detector
  knows only `lis` + `addi`; it took each of these `bctr`s for a tail call through CTR and emitted
  none of the cases (137 such switches in that module; the game stopped at once on "Call to invalid
  or unregistered function at guest address 0x8A601E1C", a case of a 6-way switch). Its `switch`
  reads the index register at the `bctr`, so a `[[switch_tables]]` entry alone cannot work: the
  entry switches on the register the target was loaded into (r3), and a `[[midasm_hook]]` at the
  `bctr` (`rexnative_case_<bctr>`, written into `src/cases_<binary>.cpp` and built into that
  binary's own target) turns that target back into its index first. The register is the
  compiler's temporary for `mtctr`, dead after the jump. The listing leaves `cr0` out (`cmplwi
  r3,5`, `bgt 0x...`), which the bound check now reads as cr0.
- **XEX DLL modules** get the same search, over their own image: `--rexnative_dump_image` loads
  each module first (`LoadUserModule` without its entry point, in that run only) and writes
  `run/image.<module>.bin` beside `run/image.bin`; its tables go to `switch_tables.<module>.toml`,
  in the module's own `includes` (an entry outside a binary's image is an error).
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

A review found two more gaps in the scan, fixed with it:

- **A register that passes through a function.** A shared function's register has to reach it
  through every function between it and the one that sets it, and a function with locals that
  names no such register copies nothing of it into ctx. Halo: Combat Evolved Anniversary's
  sub_833D0F68 sets v92 and calls sub_833D08B8, which names no v92 and calls the shared
  sub_833D7BB8, which reads it (stale). The scan now works out, over the direct calls, which
  registers each function takes from its caller (its own reads, and its shared callees' registers
  it has no local for) to the end, in one pass. shares.txt keeps each function's whole list.
- **The copy back counted as a write.** `r28 = ctx.r28;` after a call into a shared function
  hid a register the function reads before it sets it; it no longer counts.

Codegen also runs again whenever the codegen tool is not the one that generated the code
(`codegen.ok` holds a hash of rexglue.exe): left to the build, the SDK's own codegen step would
write the code again on its own, without `drop_outside_functions`.

Still open (no case in the games prepared so far): a conditional branch or a jump table into a
shared function copies nothing either, as an indirect call does not.

**A game that reads the saved registers off the stack.** With `non_volatile_as_local` codegen
drops the calls to the prologues' save and restore helpers (`bl __savegprlr_N` and its
`__restgprlr_N`), so the registers they save, and the return address (`stw r12,-8(r1)` in the
helper), never reach the guest stack. Burnout Crash runs a garbage collector (default.xex's
allocator, from an allocation in `crash.dll`) that walks every thread's stack frames by their
return addresses and marks the object pointers each frame holds, saved registers included: it
read the stale words there (0x32A8DBE6) as an object and faulted in its mark phase, about 40 s in,
on either renderer. Such a game gets `non_volatile_as_local = false` in a `codegen-options.txt` in
its work folder (`prepare.options`, lines `name = value` over the OPTIONS above); the manifest
takes it for the entrypoint and every module, and generated code made with other options than
the game's is generated again (`_options_differ`, from the flags its `_init.cpp` records).

## The upscaled pass of a scene drawn in passes

The upscaler takes the frame's last pass of the scene's surface whose draws write depth
(`Upscaler::SceneResolveDue`). Diablo III's menus draw the hero into the scene's surface with
depth, resolve it, and draw the menu (backdrop, the hero from that resolve, particles) in a second
pass that tests depth and writes none: the hero's pass was upscaled, and the hero came out
invisible with FSR and black with DLSS. `rexnative.upscaler_depth_test = true` (Diablo III's
settings) counts the draws that test depth too, so the second pass is the one upscaled.

Overlord II lights its scene in deferred passes: its geometry writes depth into a G-buffer of
three colour targets (two predicated tiles of 1280x368), and the lighting passes after it test
depth without writing it. The last pass writing depth was the G-buffer's, so its first target was
upscaled on its own; the lighting then took that FSR output with the other targets as rendered,
and on the Steam Deck (FSR Native AA by default) the menu's rocks came out white and speckled,
"wet". A scene draw writing depth into two or more colour targets now marks the scene as deferred
(logged once), and from then on depth-tested draws count as `upscaler_depth_test` makes them.
A pass with a G-buffer draw is never the one upscaled, nor weighed against the others: in
gameplay its 220 draws left the light pass (39 draws) under the quarter rule above, so the
G-buffer pass was upscaled again, at its first resolve, the normals' target, and the lit scene
came out white. Now the light pass is upscaled, in the menu and in gameplay.

FSR 3 still ghosted there (FSR 4 on the Deck hid it): the motion target FSR read was zero at
every pixel. The G-buffer passes wrote it (100% of the pixels by their end), and then the game
restored the scene's depth from a texture with a full-screen quad whose pixel shader writes
depth; that draw writes depth, so it wrote its motion too, none, over the whole screen. A draw
whose pixel shader writes depth no longer writes motion (its vertices say nothing of what it
covers). A RenderDoc capture with the camera turning: the motion at FSR's input went from 0% to
100% of the pixels, and the character's double and the smearing while turning went.

What is left for FSR 3 is what has no motion: particles, glows, blended effects. FSR's reactive
mask (`--rexnative_fsr_reactive`, on) is made as AMD's sample makes it: the scene is copied as it
is before the first blended draw that writes no depth in the pass to be upscaled
(`Upscaler::SnapshotOpaque`; in a deferred scene the lights are blended too, so the first such
draw that reads none of the G-buffer pass's resolves), and at the upscale FidelityFX's
GenerateReactiveMask compares that copy with the finished scene (tone mapped, threshold 0.2,
0.9 where above). In Overlord II's first level FSR 3's debug view shows the snowflakes in the
mask and nothing else. FSR only: DLSS needs no mask.

## A scene drawn after a depth prepass

With the upscaler on (the Steam Deck's default), FSR did nothing in Iron Brigade. It draws a depth
prepass into the scene's surface (8-bit, no MSAA, 1280x646), resolves that surface once after a
full-screen pass, and only then draws the scene's colour (about 800 draws, two targets), and its
lighting and transparent passes. Scene detection takes an 8-bit surface without MSAA as the scene
when 16 depth-tested colour draws went into it before its first resolve (`SceneNoteDraw`), so it
never learned one. `rexnative.scene_late_draws = true` (Iron Brigade's settings) counts the draws
before every full-size resolve of the surface. Then the upscaler took the prepass, the pass with
the most draws writing depth, and the scene's colour and lighting were drawn after it at the
output's size: green and yellow sparkles over the deck and streaks of light. With
`rexnative.upscaler_depth_test = true` it takes the scene's colour pass. In FSR's Quality mode
(853x431) a faint copy of the scene, offset, still shows: the lighting drawn at the output's size
reads that pass's other targets and depth, resolved at the render size. Native AA (the Deck's
default) is clean.

## Predicated tiles side by side

Tetris Splash draws its 1280x720 frame with 2x MSAA in two tiles of 640x720, side by side, into a
surface of pitch 640, and resolves the left tile and then the right one (`rect=640,0-1280,720`).
Scene detection counted only resolves that start at x 0, so it learned a scene 640 wide; with
the upscaler on (the Steam Deck's default) the left tile was upscaled from the scene's left half
and the right tile copied nothing: the intro's 3D logo was cut at the middle of the screen. A
resolve from the top that continues a seen surface past its pitch now widens the scene
(`SceneNoteResolve`), and for a scene wider than its pitch each tile of its whole height copies
its part of the upscaled output (`Upscaler::IsSceneResolve`).

## A clear inside predicated tiling

The GPU runs the calls between BeginTiling and EndTiling once for each tile, every tile at the
EDRAM base, while the host surface holds all the tiles side by side. A ClearF without a
rectangle clears the bound surface's own size (the descriptor at surface+36), a tile's, so the
EDRAM below the tile's rows is never cleared. The Bureau (2026-11-21) clears the depth of its
tiled G-buffer's 1280x256 surfaces as the tiling's last call, then, after EndTiling, restores
only the top 256 rows of the 720 from the resolved depth: the rows below still hold its depth
prepass in the EDRAM. The renderer cleared the whole host depth, the rows below the first tile
read as far, and the translucent draws there (fog, the windows, the sky outside, a light's
billboard over the player's head) showed through the walls. Such a clear now clears the tile's
rows at once and the rest only before the next draw, clear or resolve of the same tiling
(NativeRenderer::IssueTileClearRest); at EndTiling the rest is dropped. The device's tile count
stays set after EndTiling, so the hooks follow the bracket themselves (TilingCall::kInside).
Found from the draw log (the restore's rectangle ended at 0.3555 of the height, 256 rows), a
snapshot of the depth after it (rows 256-720 at 0) and a push-buffer trace (the clear's three
bin-masked quads before EndTiling).

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

## FPS Boost

Xenia canary's community keeps patches for many games ([game-patches](https://github.com/xenia-canary/game-patches):
`patches/<title id> - <name>.patch.toml`), among them "60 FPS" and "Unlock FPS" ones that change
how a game paces itself, as FPS Boost does on the newer Xboxes. Each file is for one executable,
named by Xenia canary's module hash: XXH3-64 over the module's code pages, its import thunks
rewritten `sc 2; blr; nop; nop` as Xenia canary loads them (`UserModule::CalculateHash`; the
first and last code page descriptors, in the module heap's 64 KB pages).

ReXGlue applies such a file beside a module (`game-data/default.xex.patch.toml`, SDK patch
0047) once the module is loaded, before codegen reads it and before the game runs, when its
title id and one of its hashes are the module's: a code patch is in the recompiled code, a data
patch in the game's memory. Every load logs `Module default: title 58410A95, module hash
CF37AF559A37B77C` and each patch it applies.

`rexnative fps-boost <work>/<game> [on|off]` (`rexnative/fps_boost.py`) finds the game's file
for its executable's hash (the repository's file list is kept a day, each file by its blob hash,
in the user's cache folder) and the best frame rate patch in it: 60 FPS first (the Deck's screen
and the frame limit), then an unlocked rate, then another rate above 30; not one that shows,
locks or lowers it. The hash comes from ReXGlue's log, or before a build with patch 0047 from
`run/image.bin` (dumped with no patch file applied): `xxh3_64` there is the hash's long-input
path in Python. The choice (`fps-boost.json`) is applied by the next build: before codegen the
file is written with that patch alone, enabled (or removed), and codegen runs again when it
changed (`codegen.ok` records the file's hash), so a game never runs patched data over code
recompiled without the patch. `deck` removes a patch file the game no longer has from the Deck.

Of the games tried (2026-11-24): Iron Brigade (TU 6), Fable II (GOTY, TU 1), Midnight Club: Los
Angeles Complete Edition and Blur (TU 2) have a "60 FPS" patch for the version ReXNative
installs; Asura's Wrath (TU 1), The Bureau (TU 1) and Brothers an "Unlock FPS" one. SSX's is
for its first release (ReXNative installs TU 3); Crazy Taxi, Peggle 2, Forza Horizon, Minecraft
and Perfect Dark have patches, none for the frame rate.

## A game's own code hooks

A fix that belongs to one executable lives in `rexnative/title_hooks/<title id>-<module hash>/`
(`rexnative/title_hooks.py`): `hooks.toml` holds ReXGlue `[[midasm_hook]]` entries (a host
function called at a guest address with the registers named) and `hooks.cpp` the functions. The
hash is the one FPS Boost uses (the code as loaded, title update applied), so another title
update or another build of the game gets none. Before codegen they are written to
`<work>/title_hooks.toml` (the manifest includes it) and `<work>/src/title_hooks.cpp` (the
game's CMakeLists builds it in), and codegen runs again when they changed; another executable's
are taken out. The hash is known after a game's first build: the next one has its hooks. The
functions can count the game's Swaps (`RexNativeTitleSwaps`) and run one of their own at each
(`RexNativeSetTitleSwapHook`, on the thread that calls Swap).

### Forza Horizon: one simulation step a frame

Forza Horizon (title update 4, module `336A44DC03EC8782`) steps its simulation once per raise
of an event its vblank callback raises every second vblank, whatever the render thread does; the
render thread draws the newest step finished. A frame over two vblanks lets the simulation run
ahead: one frame shows two steps of motion and the next none, at a flat 30 fps (the frame times
on the display stay even). The hooks (`4D5309C9-336A44DC03EC8782`) count the steps begun
(0x8241DC44, the main loop storing the step's time) and, at the callback's decision
(0x824ABC50, `r11` not 0 raises), hold the raise while the steps are two ahead of the Swaps:
the callback's vblank count stays, so the next vblank raises it, at most 50 ms later. After
Pinyon Shift's step backpressure (issue #381; THIRD_PARTY_NOTICES.md), which raises a held step
at the next swap instead. `rexnative.step_gate` (default true) turns it off;
`rexnative.step_log` logs every 10 s the frames that showed 0, 1, 2 or more steps. On the Steam
Deck, driving from the start (2026-11-24): in the first 10 s of the drive, 9 frames with no step
and 13 with two of 297 without the gate, 2 and 3 of 298 with it; steady driving had one step in
every frame either way, each frame two refreshes after the last.

The render thread waits for those steps by polling: when no step newer than the one it drew
last (`2380(r31)`) is done, its main loop calls `SleepEx(0, TRUE)` and looks again, about 12,000
times a second, a whole core of the Steam Deck's four (`runtime.wait_stats` with SDK patch 0060
named the loop: "yields from 82B2A85C < 82616CD4"). A third hook (0x82616CD0, before that call)
sleeps `rexnative.render_poll_us` (300 us) first, on a futex in the process under Wine
(`rex::platform::SleepOnWord`, SDK patch 0058; elsewhere nothing changes). On the Deck, driving
(two runs each): the render thread 97 -> 58 % of a core, the APU 12.3 -> 7.4 W on average (the
CPU's cores 3,450 -> 2,600 MHz), frames over 50 ms 64 and 52 -> 27 and 21 in each run, frames
with 0 or 2 steps fewer, 33.3 ms a frame either way. 1000 us gave the same (what is left is the
thread's own work). SDK patch 0066 does it for any game (`runtime.yield_backoff_us`: a thread that
yields 16 times in a row sleeps 200 us at each further yield): Forza with only that, 8.2 to 8.5 W
and its render thread at 62 %; the hook stays, a little better for this game.

## A frame held on the display

`rexnative.present_interval` (the HUD's Frame rate) 2 or 3 shows each frame for that many
refreshes by presenting its image again. The frame pacer let the next frame begin once the
frame's last present was shown: with frame latency 1 that is at the end of the hold, and a game
that waits for its own vblanks (most 30 fps ones: Forza Horizon) then reached its Swap after
the second vblank it waits for, and waited for two more: 50 ms frames at 2, not 33. A held frame
is now shown at its first present, and the next one begins from there (`frame_pacer.cpp`).

## Downloadable content

`prepare --dlc` and `rexnative dlc <work>` fetch a game's DLC from the DLC sources the user sets
(`$REXNATIVE_DLC_SOURCES` or `--dlc-source`: folders or Internet Archive items; none come with
the tool since 2026-11-24, `rexnative/dlc.py`): an archive per game, its paths
`<title id>/00000002/<package>`. The archive is matched by the game's title (roman numerals as
digits; "1", "Complete Edition" and "Director's Cut" left out), and archive.org's listing of it
(or its first 64 KB, where the first file's header is) says which titles it holds before
anything is downloaded: an archive of other titles only is left out, and one whose packages are
installed already is not fetched. The download goes on from a part a stopped run left, is
checked against the collection's MD5, and its packages of the game's title are installed as
`rexnative dlc` installs local ones. The collection's files need an archive.org login: the
cookies `ia configure` keeps (or S3 keys), which are never logged and go to archive.org over
HTTPS only (a redirect elsewhere gets none). A connection that stops keeps its part for the
next run; one archive's failure is reported and the others go on.

## Saves: asynchronous file IO

On the console a content package's STFS driver queues every request: an overlapped `ReadFile`
or `WriteFile` on a save or DLC file always answers `ERROR_IO_PENDING`, and the result comes
from the overlapped later. Titles count on it. Minecraft's storage state machine
(`sub_82A15CF0`, sub-state 22 in `sub_82A20F68`) reads `_MinecraftSaveInfo` with an overlapped
`ReadFile` and only goes on from a pending read: when the read succeeded at once, its Load tab
loaded forever as soon as a save existed (new worlds were fine, there was nothing to read). SDK
patch 0036 follows Xenia canary's rule from hardware tests (976c3578b1): a file under
`\Device\Content\` is pending after a request that succeeded or stopped at the end of the file;
elsewhere only an unbuffered one (`FILE_NO_INTERMEDIATE_BUFFERING`) that succeeded is, and for a
write one that does not extend the file. The request is still done at once, with the I/O status
block written and the event set before the call returns.

How it was found, worth repeating for a game that waits forever: the log showed the last XAM
call (`XamContentCreateEx` of the save info, completed) and nothing after it, with the classic
renderer too. A `src/game_*.cpp` of the work folder (built with the game, never committed) hooked
the game's import wrappers, the small `sub_` that tail-calls `__imp__<name>` (a scan of
`generated/` for `__imp__<name>(` names it), and then the state machine the caller's `LR` led to,
logging its state fields on each change: the read that should have been pending showed.

Open: the save's thumbnail (`__thumbnail.png`, 64x64) is black in the world list.

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
- **A stride built into BeginVertices.** Darwinia+'s LTCG BeginVertices has no stride
  argument: it multiplies the vertex count by 44 (`mulli r10,r5,44`) and never reads r6, which
  holds whatever its caller left there (often 4). The renderer took count x r6 bytes as the
  vertices the call reserved: a few bytes of each quad, the rest stale shared memory, and the
  intro's quads were drawn as long wedges across the screen. `adapt` now finds such a stride
  (`abi.constant_stride`: the stride register unread before the first call, the count or a copy
  of it multiplied by an immediate) and the layer gives it as `REXNATIVE_BEGIN_VERTICES_STRIDE`.
  Overlord's BeginVertices has one too (`mr r24,r5`, `mulli r9,r24,20`); its layer predates this.
- **DXT3A.** Fable II's terrain patches (tessellated, blended `SRC_ALPHA, ZERO`) take their alpha
  from a k_DXT3A texture fetched in the vertex shader, compared against one 4-bit step. The
  format was unsupported, the fetch read 0 and the ground drew black under the sky. It is now
  decompressed into R8 as Xenia does (each block is BC2's alpha half, a * 17): a lossy BC4 would
  move values across that step. Found with RenderDoc's pixel debugger (`rdoc.ps1 debug`): the
  interpolator holding the alpha was `1.07 * (fetch - 1/15)` with the fetch 0.
- **Resolves into k_1_5_5_5.** Fable II resolves three 256x256 mip chains a frame into 1:5:5:5
  textures, which the resolve refused. They are written like k_5_6_5's without a typed store:
  R16_UINT texel bits in the cache's B5G5R5A1 layout, then copied into the image.
- **Clears of the -32..32 formats.** Sega Rally Revo's title showed its logo as a diagonal
  trail of pale copies, where the console shows it chrome with a moving reflection. In a
  RenderDoc capture the logo was right until the motion blur, which reads a k_16_16 velocity
  target the game clears to "no motion" at the start of each frame. k_16_16 and
  k_16_16_16_16 hold -32..32 over the host's snorm -1..1 (the pixel shader writes them through a
  color_exp_bias 5 lower), but their clears stored the colour unscaled: 32 times as far. The
  clears now store c / 32 as well.

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

Blur draws its cars, its track and its scenery from about 20 recorded buffers a frame (1500 to
2000 draws): without the replay its races showed the HUD and the tail lights on black. `adapt`
turns the replay on only for a RunCommandBuffer found with high confidence; Blur's was medium
(found next to a matched function) at the address Iron Brigade's port has it, the same D3D
build. A medium one that every reference port that has the function agrees with now counts.

Darwinia+ records its frame's Clear in a buffer it replays each frame. The D3D's Clear is a
quad (DRAW_INDX_2, RB_MODECONTROL kColorDepth) that the replay dropped with the other internal
draws: nothing cleared the scene, each frame was drawn over the last (trails, then a white
screen within a minute) and its depth was never cleared. The quad now becomes a clear of what
the registers bind (`DecodeRegisterClear`, `rexnative.cmdbuf_clears`): its three vertices are
x, y, z and the RGBA color (21 dwords at vertex fetch 0), RB_COLOR_MASK and RB_DEPTHCONTROL say
what it writes. The D3D clears the 1x 1280x720 surface as 4x MSAA of 640x360, another view of
the same EDRAM tiles: the color reaches the 1x target as an EDRAM fill, the depth through
`ClearDepthAliases`. Only in replayed buffers: the main stream's gaps hold no hooked clear's
quad, and Halo's internal quads there are others.

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

Sega Rally Revo's trees flickered with frame generation: in the generated frames they stood
where they were not. Its trees are drawn four to a draw, each one's matrix in its own registers
(c7-c9, c23-c25, c39-c41, c55-c57), and the batches are filled anew every frame: in a RenderDoc
capture the tree in a draw's fourth slot had been in another draw's first the frame before. The
draw matched was the right batch by order, but held other trees: their motion was 365 pixels on
a view that turned by 17. When the copy matched has its own registers (those that differ between
two copies of the previous frame) far from the draw's, in 3 components or more (more than 0.5
apart and a quarter of their size), it holds other objects, and the draw moves with the camera:
its own registers kept, the frame's (the same in both copies: the camera's, its time) at their
previous values. The trees' motion is then the ground's (0.1 to 0.2 pixels in a slow pan), and
the camera fallback for a draw with no match copies only the registers it shares with the draw
it is taken from, not that draw's own transform.

That test took every wheel in Forza Horizon for another object. A car's four wheels are copies of
one mesh, each with its world-view-projection (c0-c3), world matrix (c4-c7) and the world
matrix's inverse (c8-c10); spinning, a wheel's projected rows change by more than 1 in a frame and
its inverse's translation (minus the turned position, 2100 from the origin) by hundreds. All 4
wheels, their tyres and treads, 93 draws a frame of the 2026-11-23 gameplay trace, took the
camera's motion: DLSS took their history from where the camera's motion put it, a doubled rim
(the user's report; the side views in the attract loop after the title are a video, not drawn,
so they show nothing of it). A rotation keeps the sum of the squares of a matrix's registers, which another
object's position changes, so a draw whose blocks of 4 registers with components apart each kept
their norm, within a fifth of how far they moved, is the same object turned, if a register of
its position stayed (components all beyond 1, each changed by less than 5 %: the wheel's c7 =
1712 163 1239) and none of its registers is, bit for bit, another of the copy matched (a batch
refilled in another order keeps the norm of the whole run). Registers that stayed are left out
of the norms, but not a row with its translation (the inverse's axle row: the car turning by 2
degrees moved its 1987 by 73). In the trace the wheels then all match their own draw of the
frame before (`kDebugLogMotion`: refusal 5 and the turn of the block that moved most; 3 for a
copy refused). In Sega Rally Revo's race trace the trees are as before; 7 draws of a car,
matched in order with a position kept, now take their own motion instead of the camera's.

With frame generation's optical flow back (above), Sega Rally Revo's generated frames still
smeared the road around the car: a copy of the bumper's lettering below the bumper, the ground
in blocks. The frame generation captures now write the scene's motion vectors as well
(`f<N>-fg-motion.raw`, DEBUGGING.md); warping the frame before by them and comparing with the
frame showed the road in bands, rows of tens of pixels whose motion was another's: (-56, -27)
pixels, then (+41, 0), then (-60, -30) down one column, and up to 2000 pixels in places. The motion
log (`draw-debug.txt` 262144) gave the reason: the ground is drawn in tiles, a few index counts
(its levels of detail) all of one index buffer and of the same constants (the tiles' vertices are
in world space), each tile's vertices in a buffer of its own. Draws of one identity, they were
told apart by order alone, and once two draws earlier in the frame came or went every tile took
the one two places before it, its neighbour, and read that tile's vertices for its previous
position: 33 of 1500 draws a frame. A tile that came into view or changed its level of detail took
another tile's vertices too. Now among draws of one identity the one with this draw's vertex
buffer (its first binding's base and first vertex) comes first; and in a game whose matches
nearly all keep their buffer (90 %, Sega Rally 98 %), a draw whose match has another buffer moves
with the camera instead: exact for the tiles, and for a draw that writes its vertices anew every
frame while it stands still. The first vertex (the anchor) is now also read from positions of 4
half floats or 4 16-bit integers, Sega Rally's formats (its ground, its small objects), so that
draws as alike are told apart by it there too; where both first vertices are known and apart,
their distance decides instead of the buffer, and a small object that writes its vertices anew
keeps its own motion. The ground tiles all start at the same corner, so for them the buffer
decides. Matched draws with another buffer went from 32.5 a frame to 0.2.

Frame generation hides much of this: where the game's motion vectors and the two frames disagree,
its optical flow takes over. Captures taken minutes apart differ too much in what they show to
compare, so the draw-debug bits can apply in even frames only (1048576): the rules are then on
in one frame and off in the next of the same capture, and each generated frame of the one is
compared with the mean of the two around it of the other (flicker as DEBUGGING.md measures
it; an A/A run gave -0.06 points, 14 of 24 frames better). The buffer rules left the share of flickering pixels where it was on average
but lowered it in 17 and 18 of 24 frames; the anchors of 16-bit components lowered it by 0.37
points (18 of 24). A camera for the frame's first draw of a shader, taken from the previous
frame's draws of the shader, removed the ground without motion that a RenderDoc capture showed
around the car (a new tile, the first of its shader, keeps its own constants), but raised the
flicker by 0.9 points (better in 6 of 22): left out.

That flicker measure favours a generated frame that is the mean of the two around it, which is
what a surface without motion gives, so the ground without motion stayed and the captures kept
showing it: in most frames of a race 10 to 30 % of the pixels, the ground around and below the
car, had no motion at all, and the generated frames there had a copy of the number plate below
the bumper, or the plate twice. The motion log showed the draws: the ground nearest the car is
drawn first among its shader's, in tiles whose index counts change from frame to frame (no draw
of their identity in the previous frame), and the camera fallback had no draw of the shader
matched before them to take the camera's registers from. Such a draw now gets its previous
constants as a copy of its own in upload memory of their own, and the first draw of its shader
matched later in the frame writes the camera's registers into that copy, as the fallback would
have (MotionHistory::pendable): the GPU reads the copy only once the list is submitted, and the
list submitted before (every 256 draws) leaves its pending copies as they were. With this on in
odd frames only (draw-debug 2097152 with 1048576), pixels without motion went from 6.5 % to
0.1 % of the frames, the plate's copies were in the frames without it (5152, 5156, 5158 of
one capture) and not in those with it, and the flicker measure moved by -0.1 points (9 of 17).
Warping the frame before by the new vectors left more error than no motion in that zone (13.8
against 9.0): the dust cloud behind the car, which no draw gives motion vectors and which moves
with the car, covers much of it.

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

Not before the probe, though. Sega Rally's attract demo once drew a wall of stretched texture
across the sky for its first seconds. In a RenderDoc capture the wall was one skinned mesh (a
giraffe): its indices and its vertices were right, but every vertex weighted to bone 0 went
thousands of units away. Bone 0's first row is c28, which held a value from earlier draws: the
game's own code writes its bone palettes into the push buffer, and the probe that turned the walk
on came 7 s after the demo began. Two probes in a row that each find changed constants now keep
the walk too (Sega Rally's menus: 5 a probe), and a kept walk is learned per game:
`<cache>/rexnative/inline_draws.txt` (beside the pipeline cache, `userdata/cache/rexnative`
without a cache root) turns it on from the first Swap of the next runs. Delete the file to have
the game probed again.

The walk on from the start, the wall still came back now and then, in the menus and the race's
opening flyby. `--rexnative_d3d_tap_check_range=4070-415F` (the tap's decode of the push buffer
against the tracker, vertex constants c28-c87 alone; the first difference traces the next two
frames) caught it: the stream held bone matrices where the tracker held an older draw's values.
Sega Rally inlines EndVertices: after a BeginVertices its own code writes the rest of that call
(the draw packet) and then, before the next hooked call, the next draw's bones. The walk skipped
every gap after a BeginVertices as vertex data, but this game's vertices come from a ring of
their own (fetch 31 points outside the push buffer): such a gap is now walked, all but the
BeginVertices draw itself (the renderer queued it already; the first auto-index draw of its
vertex count). Before, 67 frames of a run had wrong bones; after, none.

## Resolves from the registers

SSX drew black on the native renderer with its intro running at 30 FPS: the presenter found no
resolved image for its front buffer, and the frame's draw log had no resolve at all. The trace
had 29 a frame, each written inside `823518A8`, the public `Resolve` wrapper the layer hooks.
Its row in `d3d_surface.inc` said otherwise: `fn_826CE9C0, kOther`, Iron Brigade's row for its
own wrapper, matched to the same address by region before the hooked functions were added, and
the hooks take a function's class from its row. `adapt` now gives a hooked function that is not
a draw (Swap, Resolve, Clear, the queries, the tiling bracket, the push buffer's) its own row;
a draw function takes it only from a row less certain than its own match (Minecraft's
`DrawIndexedVertices`, a medium match, is a masked match of Iron Brigade's `fn_826D6468`, and
Minecraft draws well with that row). Across the 26 layers, only SSX's Resolve was hit; with it
fixed in place SSX plays: menus, the 3D globe, the first descent.

On the way, the renderer learnt to copy a resolve it finds in the push buffer between hooked
calls (`rexnative.inline_resolves`, with the walk of `rexnative.inline_draws`): an engine that
writes its own, or a D3D path the layer does not hook. `DecodeRegisterResolve` reads it as
ReXGlue's `GetResolveInfo` does: the rectangle from the three vertices of vertex fetch 0, moved
by the window offset, inside the scissor, 8-pixel aligned; the copy from `RB_COPY_CONTROL` and
`RB_COPY_DEST_*`; the clear values from `RB_COLOR_CLEAR(_LO)` and `RB_DEPTH_CLEAR` in the source
surface's format. Each source pixel goes to the texel with its own coordinates from
`RB_COPY_DEST_BASE`, which the guest moves down whole rows of tiles for a destination point
below the rectangle (SSX's three shadow cascades in one 1024x3072 texture): the resolver writes
such a copy into the image whose memory holds that address. The destination's height is the
pitch register's plus the rectangle's first row (the XDKs up to 20353 write the level's height
minus the destination row; 21119, Forza Horizon's, writes the whole height): a copy that fits the
image already at its base takes that image's size, which the first tile, at row 0, gave it. The walk's probe keeps the walk on
when it finds a resolve a frame in the gaps (a first probe at frame 30 instead of 150 caught
SSX's start-up writing 234 constants and kept the walk on for good: left at 150).
`rexnative.resolve_check_registers` (with the tap) decodes every hooked resolve both ways: on
SSX, 82% agree exactly; the rest differ only in the right edge, cut by a 4x MSAA surface of half
the pitch or a scissor that the tap's register view holds at that point and ReXGlue's own decode
rarely sees (its "outside the surface pitch" error came twice in three minutes).

## XEX DLL modules

Forza Horizon keeps its speech and media code in two XEX DLL modules beside `default.xex`
(`SpeechFacade_default.xex` at 0x89000000, `XMediaFacade_default.xex` at 0x88000000), which it
loads at start-up. ReXGlue recompiles such modules itself: a `[[modules]]` entry in the manifest
(its file, the guest path the game loads it by, its own output folder) becomes a host DLL beside
the executable (`dll_targets.cmake`) that the generated `init.cpp` registers. `prepare` now
writes one for every `.xex` under the game's folder whose module flags say DLL, with the
entrypoint's register options; the hints and the shared functions go to the binary whose image
holds their address (its title update patch can make the image larger), and the non-volatile
live-in scan reads the modules' generated code too (63 functions of `XMediaFacade`).

A module need not be named `.xex`: Renegade Ops loads `FragmentBundle_F.dll` (at 0x98000000)
and three more `*Fragments_F.dll` at start-up, XEX2 files with the DLL flag. With `.xex` only,
they were left out of the manifest and its main thread aborted at once ("Call to invalid or
unregistered function at guest address 0x9800F238"). `.dll` files with an XEX2 header and the DLL
flag are modules too since 2026-11-24 (Halo: Anniversary ships four Waves audio ones it does not
load at start-up; they become modules at its next prepare).

FragmentBundle_F.dll then loaded, but its functions were never registered: "BaseHeap::AllocFixed
invalid base alignment: base=98019000". The runtime allocates every binary's function table in
the 0x80000000 heap (64 KiB pages), by default right after the image; this module's image is at
0x98000000, in the 4 KiB heap above. `prepare` gives a module outside 0x80000000-0x8FFFFFFF a
`function_table_base` of its own (`_module_table_bases`): after the images and tables of the
game's binaries in that heap, each table counted at its largest ((image size + 64 KiB) * 2).
Modules inside that heap keep codegen's choice.

Their title update patches then failed: "XEX patch image key does not match the base XEX". A
patch's header delta is a list of entries (a copy, a zero fill or LZX data) up to an empty one;
ReXGlue passed the applier the first entry's length only. The main executable's patch is one LZX
entry; a module's starts with three copies, so its security info, and the key in it, stayed
unpatched. SDK patch 0037 passes the descriptor's size, as Xenia does. Forza Horizon then plays
on the native renderer, after three more fixes found in its opening drive:

- Its minimap came out as blue stripes: a 64x64 texture fetched as `k_8_8_8_8_AS_16_16_16_16`,
  which the guest D3D resolves into as `k_8_8_8_8` (the trace's `RB_COPY_DEST_INFO`), and the
  resolver's table of those formats lacked it.
- Its car was not drawn (the stock renderer drew it). About 108 recorded command buffers a frame
  were rejected: the XDK 21119 D3D keeps a buffer's first chunk node inside the buffer object
  (object + 0xB8, on the 4 KiB heap), and the replay took only nodes in the physical views. A
  node on committed readable pages of any heap is taken now (`BaseHeap::QueryRangeAccess`).
- Those buffers hold 26 resolves a frame (the 64x64 textures of the car's materials: its rear
  glass showed a grid without them), which the replay dropped: it copies them from the registers
  now, as the walk does (`rexnative.inline_resolves`). Midnight Club's buffers hold none.

Top Spin 4 is another case: its `default.xex` has no D3D at all, and both of its modules link
their own D3D library. The renderer's hooks are in the executable; reaching functions in a module
needs the hooks in that module's DLL.

## Renders the CPU reads

Resolves of at most 256 texels are written back by default (`rexnative.resolve_writeback_tiny`,
one to three frames late as the experimental writeback): Blur averages its scene's luminance
down to 1x1 targets the GPU never samples, and with zeros there its races came out white.

Halo: Anniversary lights its remastered scenes in 64x64 tiles (18 by 11 at 1152x648): each tile
is a quad whose pixel shader adds the lights its constants list. The CPU picks those lights from
the scene's depth halved down to 18x11, one texel per tile, a resolve it reads back. Without the
writeback every tile got "no light" (c8 = 0,0,0,1 in the push buffer itself, so the renderer drew
exactly what it was given) and the hangar's floor was dark where Xenia Canary's is lit and
shadowed. With the limit at 256 texels the floor is lit; the frames right after a camera cut
still use the previous shot's tiles, one to three frames, until the writeback catches up. (Xenia's
report, game-compatibility#552, needs `readback_resolve = "full"` for the same reason. The
experimental writeback by size or unused destination also lit it, but its late writes of large
resolves scrambled textures that had reused the memory.)

The native renderer never writes a resolve into guest memory (the Xenos plugin skips its own
copies with `gpu.xenos.skip_draws`), so a game that reads a render on the CPU reads zeros:
Minecraft's save thumbnails are black. `rexnative.resolve_writeback` (experimental, off) reads a
resolve's texels back after its frame and writes them into guest memory as the console's resolve
does (tiled, endian-swapped), with the texture cache's watches on those pages triggered first;
by size, or for any destination no texture and no front buffer has used. On a save, Minecraft
halves its scene on the GPU (256, 128, then 64 square) and reads the 64x64 resolve at once: the
guest's fence passes when the Xenos plugin reaches it, before the renderer has even recorded the
frame, so a copy made one to three frames later is too late (it read the previous save's
writeback once, which looked like success). Writing late into memory the game may have reused
meanwhile is a risk as well, so it stays off. What would work is a sync point: the guest's wait
for its fence held until the renderer has done that resolve and written it back.

## Occlusion queries in batches

With `rexnative.occlusion` off (every game's default) the SDK's command processor answers the
game's occlusion queries: it still sees their EVENT_WRITE_ZPD packets under
`gpu.xenos.skip_draws`, and writes `gpu.xenos.query_occlusion_fake_sample_count` samples. It
wrote 0 to a report without D3D's 0xFFFFFEED sentinel (a BEGIN) and the count to one with it
(an END). But the Xenos's ZPD is four free-running sample counters that every event dumps, and
D3D subtracts two reports; in a QueryBatch (BEGIN, BEGIN, BEGIN, END, END, END) only the last END
holds the sentinel, so every other query of the batch read 0 samples. Halo: Anniversary's
remastered renderer surveys its objects that way before their material pass: a second after its
menu came up the ship and the ring behind it kept only their G-buffer pass (normals and depth)
and showed the colour the scene's target was cleared to, flat grey with their lights on top.
SDK patch 0038 gives every report a running counter advanced by the fake count at each event,
as Xenia canary's fake mode since its commit 3d233a5 (its compatibility report,
[game-compatibility#552](https://github.com/xenia-canary/game-compatibility/issues/552), had
the clue: "remastered mode object culling is separately handled through batched surveys").

`rexnative.occlusion` on, which counts the queries on the host, never reached the renderer in a
game that follows Swap's device (`kFollowSwapDeviceOnly`): QueryIssue and QueryGetData take the
query, not the device, and the query was taken for another device. They now go by the device
the thread last drew with. Halo stops during its intro with it on all the same (it seems to wait
for a result within the frame, which the host gives only after it).

## Resolves into a 3D texture

Syndicate's menus came out black: its last pass looks every pixel up in a 32x32x32
k_2_10_10_10 colour grading LUT that it builds with resolves, one depth slice each, and the
native resolver refused a 3D destination ("3D destination unsupported"), so the LUT was zeros
(guest memory holds none of it on ReXGlue's renderer either: it lives in the GPU's copy). A 3D
texture's slices interleave in its tiled memory, so a resolve into slice z and the texture cache
both name that slice by base + z x (row pitch x rows of a slice) (texture_layout.h
Slice3DKeyStrideBytes): a key for the resolved image, which stays on the GPU (no writeback, no
dump); the cache copies each slice's image into its 3D texture at that z. The menus and the
first mission show since as on ReXGlue's renderer. They looked darker at first, but the title
screen's background is animated: sixteen window captures alternating between the two renderers
in one session (`live.ps1 control "renderer stock"`) averaged as bright, and RenderDoc showed the
last pass's inputs (LUT slices, exposure, noise) equal in both. Compare animated screens that
way, not by one capture each.

## Resolved images that outlive their memory

Blur's races had black patches over walls, road and trees: in most runs that went through the
menus slowly, never in a replay of the same frames, and once there they stayed whatever was
switched afterwards. A resolve's image stands for the guest memory it was resolved into, and the
texture cache looks images up by address: a texture at an image's address binds it, and a
texture inside an image's memory takes rows of it (`Resolver::FindContaining`: a stack of shadow
cascades in one texture). Images were kept for good. Blur's boot screens resolve a 1280x720
target into 0F0D6000 and the game frees that texture around frame 400; in a race its 32x32x32
colour grading texture is loaded by the CPU into 0F408000, inside the old image's memory, with
the key address of slice 11 at the start of a row of the old image's tiles. After every upload
the cache copied a 32x32 block of the boot screen's image over that slice ("it copies even over
fresh guest data"), and the last pass mapped one band of one colour channel, a tenth of the
screen, to near black. Where the game put the texture changed from run to run (another run had it
at 0F420000 and looked right), which made the fault look like a timing race.

The console's memory holds what was written last, a resolve or the CPU. So each image's memory
now has a write watch (`Resolver::WatchDestination`, `--rexnative_resolve_watch`): when the game
writes there the image stands for nothing until it is resolved into again, and the textures
over that memory are uploaded from it. Blur drops the boot image at frame 409 and some forty
menu images when it loads a race (the log names each). The renderer's own writes there (the
writeback, the memexport readback) go through the physical view and trip no watch.

How it was found, with the tools of [DEBUGGING.md](DEBUGGING.md): a frame trace recorded with
the patches on the screen replayed clean, so the game's data was right; the patches stayed with
the GPU serialized live, so it was not timing; the texture audit found every texture from guest
memory equal to it, hash and GPU copy; the resolves of the same frame, the game's against the
settled replay's, were equal but for the final image (64 of 67, the exposure too), and the pixels
apart were one band of one channel; the audit, extended to textures that hold resolved data,
listed the grading volume alone; and `live.ps1 idle`, dropping the idle images in the running
game, took the patches away at once. A replay of that frame with an image put at 0F0D6000 first
(`--rexnative_trace_play_stale=0F0D6000,1280,720`) has the volume wrong with
`--rexnative_resolve_watch=false` and right without it: the test of the fix.

Two wrong turns on the way, for next time. The first comparison of resolves measured float
targets in linear light with a threshold that a fault in dark tones stays under, and compared a
replay whose exposure had not settled. And a run that came out right with the fix off was the
game putting the texture elsewhere, not the fix failing: one run each way proves nothing about a
fault that depends on where memory falls.

## Frame generation's HUD-less copy

Frame generation looked right by its counts (every frame with a generated one before it, 30 FPS
shown as 60) while showing each frame twice: captures of the generated frames
(DEBUGGING.md) were the real ones to the pixel. FidelityFX takes the HUD from where the frame
and its HUD-less copy differ, and copies the frame there. The copy was made at the first blended
draw into the front buffer's surface after an opaque one (PGR4's HUD). Sega Rally Revo composites
twice, the second time after a blended draw, so the copy held the first composite and differed
from the frame in 98.5% of its pixels: all HUD to FidelityFX. The copy is now made again after a
later composite (1% differs: the HUD), and every 15th frame it is compared with the frame on the
CPU; when more than half differs, the blended passes before it are learned by their pixel
shaders and the copy waits for them. With it, the generated frames lie between the real ones (a
quarter of the two-frame motion) and the HUD stays sharp. Syndicate's first mission flashes its
colours each frame, which FidelityFX's scene change detection takes for cuts (a copy, not an
interpolation): right for such frames.

Frame generation tried on more games (2026-11-20) found two more ways to lose the HUD.
Overlord's minion counter came out with its letters scrambled ("1/30", "V0/0") in every generated
frame: the copy held it. Its main menu and pause screen darken the view with a full-screen quad
of the HUD sprites' pixel shader, the measurement learned that shader as a full-screen pass, and
in the game the copy then waited for the first HUD sprite. A learned pass is now its pixel shader
with its first texture's size (the counter's 260x90 is no full-screen overlay's), and the counter
stays sharp after the menus. Dark Souls II and Minecraft resolve their front buffer from the
scene's surface, where they go on drawing after the upscaled resolve (post-processing, then the
HUD), so they had no copy at all, a rule made for Jet Set Radio (its HUD drawn into the scene
before the scene's resolve, where an early resolve would run the upscaler): their hotbars, item
slots and button prompts came out doubled and half transparent as the view turned. After the
upscaled resolve those draws go into the upscaler's full-size target, which an early resolve
copies as it is, so from there their draws are watched as another game's into its front buffer;
Minecraft's flicker measure fell from 2.3 to 0.6 % and both HUDs stay sharp.

Sega Rally Revo's scenery still flickered while driving, which the captures hardly showed: the
generated frames were right where they were (warped by the game's motion vectors the frames
before and after them, they matched within 2 levels), but softer. FidelityFX samples both
frames between their pixels and blends the two, and its frames had 0.76 of the real frames'
detail (the luma Laplacian; 0.79 in Minecraft, Overlord, Midnight Club and Perfect Dark), half of
their local detail energy: leaves, grass, tyre walls and stone pulsed between sharp and soft at
30 Hz as the view moved. The HUD pass (CompositeHud) now runs for every generated frame, from
FidelityFX's into the presented one, and sharpens it with FSR 1's RCAS by
--rexnative_frame_generation_sharpness times the pixel's motion in pixels (up to 1: a still
pixel is the real one's, as sharp). Captures alternating the strength in one race: 0.4 gave 0.90
of the detail, 0.5 0.96, 0.6 1.08, 0.7 1.25 with halos; the local detail energy's mismatch
(|log2 E_G / E_R|) fell from 1.04 without to 0.64 at 0.5, the default.

What is left are the motions no vector holds. The car's shadow is no draw of its own: the ground
shader darkens the ground from a 912x912 shadow map, so the shadow's pixels carry the ground's
motion while the shadow moves with the car, and FidelityFX, which judges the game's vectors by
the direction of the colours it samples (NormalizedDot3, blind to brightness), takes them; behind
the car, where the ground comes out from under it, the generated frame shows lit ground in the
shadow. The dust, blended without motion vectors, moves by the optical flow's 8-pixel blocks.

## Frame generation's optical flow

With the HUD-less copy right, the generated frames still smeared what moved without motion
vectors of its own (mud thrown up, dust, thin fence posts in Sega Rally Revo): where the game's
motion vectors disagree with the two frames, FidelityFX's frame generation takes its own optical
flow instead, and FidelityFX's debug view (`rexnative.frame_generation_debug`) showed that flow
flat grey, no motion at all, frame after frame. Swapping the inputs found the cause in the
provider (FSR 3.1.6, the frame generation DLL 4.0.2): with a HUD-less copy (HUDLessColor) that
is another resource than the frame (presentColor), the optical flow stays at zero; with the copy
left out, or the same resource given for both, it follows the picture. So FidelityFX now takes
the HUD-less copy as its frame and no HUD-less input, and generates between HUD-less copies; a
pass of ours (FrameGeneration::CompositeHud) then puts the frame's own pixels over the generated
one where the frame and its copy differ, and one pixel around (the HUD: 5 to 7 % of a race's
pixels). That is FidelityFX's own way of keeping the HUD (the frame's pixels where the two
differ), done after it. With the flow, the debug view shows it moving with the picture and the
generated frames keep mud and posts in place; dust clouds behind the car, which no draw gives
motion vectors (blended draws do not write them), still break into blocks of the flow's 8-pixel
grid.

Captures misled the first measurements: a capture held the guest thread for half a second for
the Xenos plugin's copy, FidelityFX's scene change detection saw the jump, and for the next four
frames (its scene change history) it copied the real frames instead of generating, so the first
frames of every capture burst looked worst and the optical flow read zero there. With frame
generation on, captures no longer hold the guest thread (DEBUGGING.md).

## The scene's depth after the upscaled resolve

With the upscaler on (the Steam Deck's default), Asura's Wrath's hero looked see-through, his
far arm and his insides over the near ones, while the stock renderer and the native one without
the upscaler drew him solid. The game draws him twice a frame: in the scene's pass, dark, writing
depth, and after its colour resolve again, as additive lighting layers that test the depth with
"greater or equal" without writing it, so only his front surfaces take the light. A RenderDoc
capture of each (`rdoc.ps1 history` on the depth target at a pixel of his arm) showed the
difference: in ReXGlue's renderer the layers behind failed against the scene's depth (0.0059),
in the native one the depth held 0 and every layer passed. The scene's draws go into the
upscaler's own targets (RenderTargets::OverrideScene); after the upscaled resolve the surface
takes its full-size targets again (Upscaler::EnterPostScene), and the full-size depth target
never had the scene's depth. EnterPostScene now copies it there (RenderTargets::CopyDepth,
AliasDepth's pass) unless the resolve cleared it. The scene drew that depth with the upscaler's
sub-pixel jitter and the draws after it have none, so a redrawn surface lands up to half a pixel's
slope on either side of its own depth; those draws (testing depth without writing it) take a
slope's worth of depth bias toward passing (draw.cpp), so that a surface is not lost on part
of its slopes.

Both copies were for a 1x full-size target alone. Marble Blast Ultra draws its scene in 2x MSAA
(two predicated tiles side by side), then the marble, its ring and the HUD over it after the
upscaled resolve: with the upscaler on (the Steam Deck's settings), the full-size MSAA target
never had the scene, and the level was black around the marble. The copies now write each
pixel's samples alike into an MSAA target.

The same game also showed, on the Steam Deck only, a few seconds of rainbow-speckled noise at
the start of its first gameplay (a photo of the Deck), then a clean picture. Its scene is drawn
in 2_10_10_10_FLOAT (7e3) and resolved as 2_10_10_10: the post-processing reads the 7e3 bits
as unorm and decodes them itself (the trick of Unreal Engine 3 on the console, see EDRAM
aliasing). The resolve's target holds those bits (RenderTargets::AliasColor), and that is what
the upscaler took for colors: a log-like encoding, with FSR set up for an 8-bit scene. FSR 3.1
on the PC got away with it; FSR 4 on the Deck (its INT8 model) did not while the game's
exposure was still high. The upscaler now takes the scene the game drew (RenderTargets::
ConvertedFrom: the float target the resolve's one was converted from), in HDR, and converts
its output to the bits the resolve reads (RenderTargets::ConvertColor, AliasColor's pass).
The logs of The Bureau, Brothers, Midnight Club: Los Angeles, Syndicate, SSX and Forza Horizon
show the same aliasing of a float surface; where the upscaled resolve is one of them, it takes
this path too.

## A D3D older than every reference

Geometry Wars: Retro Evolved (2005) links D3D 2.0.2135, older than any reference's (Castlevania:
Symphony of the Night's 4314 is the next). Its device keeps the push pointer at +0 (the segment's
end at +8) and the dirty masks from +16, its shader objects have another layout (the ucode at +40,
its size at +600), and a texture's fetch constant is at +16, not +28: the layer, mapped from PGR4's,
kept the reference's offsets, and the native renderer crashed reading the first vertex shader
object (`GuestStateTracker::UpdateShaders`). The Gestor's fix step stopped there. The game plays on
ReXGlue's Xenos plugin, so the push buffer holds everything the GPU needs: `adapt` now makes the
layer of an XDK older than 4314 stream-only (`hook_args.h` `kStreamOnly`, or overrides.json
`"stream_only"`). The renderer reads nothing from the device but its push pointer; every hook but
Swap, CreateDevice and the push buffer's own runs the guest only, so the walk (`inline_draws`)
sees every packet between Swaps and draws, resolves and clears from them, the shaders from their
`IM_LOAD`s. Swap's front buffer fetch constant is read at the offset its own code hands `VdSwap`
(`constants.texture_fetch_offset`). Two things the walk needed: its bin select starts at all ones
(this D3D's segment start writes `SET_BIN_SELECT` 0xFFFFFFFFFFFFFFFF and its draws run under
`SET_BIN_MASK` 0xFFFFFFFF00000000: the later XDKs' low-word select dropped all 16 draws a frame), and
the Clear quads in the gaps are cleared (no hook clears: the menu drew over a white surface). The
menus and the gameplay then draw as on the Xenos plugin, 60 FPS. The upscaler did not start there
(no `upscaler:` line with it asked for): not looked into.

A layer that does not fit its game's D3D no longer ends the game either. The hooks' native work
(the state tracker's reads, the renderer's draw, clear, resolve and Swap entries, the walk) runs
under a structured exception handler (`native/guard.h`); an access violation there logs "the
native path read a bad guest address", and only Swap reaches the renderer after it: it hands the
game to the Xenos plugin for the rest of the run, as F9 does (without marking the device's dirty
masks, which that layer would write at the wrong place). Geometry Wars built with its old,
PGR4-mapped layer crashed at its first draw; with the guard it played its menus and a game on the
Xenos plugin. The plugin's frames now log a line of their own in the native stats' form
(`ReXNative: frames N | per frame: draws 0.0 (the Xenos plugin draws the game), frame F ms`), so a
game on the classic renderer, from its start or handed over, still shows that it runs (the
Gestor's boot test counts those lines).

Marble Blast Ultra (D3D 2.0.2135 too) booted with no frame at all: PGR4's Swap matched nothing
there, and without the Swap hook the renderer never sees a frame. `adapt` now takes, when no
reference finds Swap, the one function that calls the VdSwap import (`swap_from_vdswap`). It then
showed a black screen: it draws its 1280x720 2x MSAA scene in two predicated tiles of 672x720
side by side (the EDRAM pitch 720, the second tile at x 608, window offset -608), and each tile
writes its own RB_COPY_DEST_BASE under its bin mask, moved by its origin's tiled offset (x 608:
+0x13000). The walk runs every bin and merges the tiles (window offset 0, the scissors' union),
so the last base, the second tile's, took both copies, 0x13000 bytes past the front buffer.
With `kStreamOnly` the walk now merges RB_COPY_DEST_BASE as well (the lowest, the origin tile's);
a draw whose merged scissor reaches past the pitch widens the host surface
(`RenderTargets::NoteTiledWidth`, as BeginTiling's clear does for a hooked game), a register
resolve may copy that far, and a tile's clear-after-copy waits for the other tiles' copies (on the
host surface the first tile's clear wiped the columns 608-672 the second copies: a black band),
issued after them as copy command 3 (kNull, the clears only). Title and menus then draw right.

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

Halo: Anniversary's engine moves its streamed textures with a memexport copy: the same shader
as Iron Brigade's (64 bytes a vertex, 8 x k_16_16_16_16, at element 8 x (index - c1.x)), which
compacts its texture pool: thousands of moves while a level loads, many overlapping (a block
moved 8 KB down). On the GPU three things went wrong, each fixed in turn while finding the
last. The readback kept a byte that differed from the snapshot for one the guest wrote after
the draw, but shared memory may not hold the guest's last bytes yet (pages the GPU owns are
copied again only once a guest write faults on them): Prepare now copies the destinations from
guest memory when the game issues the draw, and a byte stays the guest's only where it differs
from that copy too. The Xenos runs vertices in order, so a move overlapping itself reads what
the earlier vertices wrote; a PC GPU runs them in any order: such a draw (non-indexed, exporting
over a vertex buffer it reads) goes out in chunks of at most (distance / bytes a vertex)
vertices, in order, with a UAV barrier between them (MemexportChunk). And the textures still
came out as colored noise or zeros, differently each run, until the copy itself was done in
guest memory when the game issues it, as Iron Brigade's: `--rexnative_gpu_memcpy` now knows
both shaders (kGpuMemcpyShaders: the hash and the constant of the first element). The opening
cutscene is the same from run to run since, with no readback left to wait for.

Asura's Wrath (Unreal Engine 3) has the same copy again (its first element at c7.x): its texture
pool's moves, about 260 draws in a second while a menu or an episode loads, a few of them over
their own source closer than one vertex's chunk ("too closely to split: one draw", so they raced
on the GPU), and a Swap that waited 9 ms for each readback. On the Steam Deck its episode menu,
whose text and pictures arrive by these moves, showed wrong text (2026-11-21). The third entry
of kGpuMemcpyShaders does them in guest memory; the opening cutscene's textures then match guest
memory (`live.ps1 audit`: 0 stale, 0 different) with no memexport readback left.

## EDRAM aliasing

- **Across MSAA:** a 4x (or 2x) depth surface over a 1x one at the same base takes its depth
  (each sample is a pixel of the 1x surface), as Iron Brigade's half-resolution particles need to
  be hidden by the scenery (`RenderTargets::AliasDepth`).
- **Clears across pitches:** LIMBO draws its scene as 4x MSAA in four predicated tiles of
  320 x 720 and resolves each with a depth clear, then draws its foreground (the grass) in a 1x
  pass at pitch 1280 over the same EDRAM tiles, testing a depth only its own prepass writes. The
  clear reached the 4x target alone, the 1x one kept the title screen's depth and the grass
  failed the test. A resolve's depth clear now reaches every depth target on the tiles it
  clears, on any pitch when it covers whole tile rows (`RenderTargets::ClearDepthAliases`).
- **Across formats:** Brothers' post processing reads the 7e3 scene's EDRAM as 10:10:10:2 and
  8:8:8:8 bits (`RenderTargets::AliasColor` converts them between targets).
- **Color across MSAA:** Blur draws its HDR scene into a 1280x720 1x 7e3 surface and resolves
  the same EDRAM as 640x360 4x MSAA: the resolve's average of each pixel's four samples is a
  half-size copy, for its exposure and its depth of field. The 4x target was another, empty:
  the exposure was measured on black and the depth of field blended black into the scene (the
  race white, then dark smudges once it showed). An MSAA color target over a 1x one at its base
  (twice its pitch for 4x) now takes the 1x pixels as its samples (`RenderTargets::AliasColor`,
  as `AliasDepth` does for depth); the half-size copies match the Xenos plugin's (47 dB).
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
game never created before still has to compile while the game plays (50 to 170 ms on the Steam
Deck). A frame used to be held back for every such pipeline (the previous frame stayed on screen
while it waited, up to 0.5 s): Overlord II's levels, played for the first time on the Deck,
stuttered at each new effect or material. Since 2026-11-24 a frame is held only when it waits
for at least `pipeline_hold_min` (8) at once, a scene's first frame (180 at a race's
start); mid-game a frame waiting for one or two is shown without those draws, which come
back a frame or a few later, as Dolphin's and RPCS3's asynchronous shaders do. Draws that write
guest memory (memexport) still wait for theirs. The prewarm's compiles run below normal priority
(a draw's at normal), so they do not take the CPU from the game's threads. A stored pipeline's use used to be marked on another record (the records were sorted
apart from their hashes), so pipelines in use could be forgotten.

## Upload memory remembered, and the hitch line

Vertex data, constants and texture uploads go through pools of 16 MB UPLOAD-heap blocks
(`renderer/src/native/upload_pool.h`), recycled once the GPU is done with them. A pool started
with 4 (12 for constants) and grew during play: a block takes 2 to 16 ms to create on the PC,
more on the Steam Deck, and the games' logs showed it at every session (Sega Rally Revo about 12
blocks in its first 600 frames, Burnout CRASH! 5 in a tenth of a second, Darwinia up to 24). Each
pool now remembers the blocks this game needed at its peak in `<cache root>
exnativeupload_blocks.txt` (`textures`, `vertices`, `constants`; written as soon as it grows, at most 32
blocks reserved), and the next run creates them at start-up, during its loading. A request larger
than a block got a block of its own, released after its frame and made again the next time (Limbo
and The Bureau upload 41 MB at once): the largest such block, up to 128 MB, is now kept for the
next.

What stops a frame mid-game is logged with the frame statistics, every 150 frames, when anything
did: `ReXNative hitches: N new pipelines, N frames held (ms waiting), N shown without some draws;
N shaders translated (ms, longest ms); N upload blocks created (ms); N frames over 50 ms (worst
ms)`. A Steam Deck's log then says which of them a stutter was.

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

## Spin waits

The XDK's D3D waits for the GPU and for the next frame in loops of `db16cyc`, Xenon's spin wait
hint, and a look at the device's state (Jet Set Radio's main thread spent about 80 % of its time
in one, inside `Swap`). ReXGlue's codegen writes `db16cyc` as `simde_mm_pause()`, an x86 `PAUSE`,
so a 30 FPS game kept a core at full clock between its frames, and on the Steam Deck, whose CPU
and GPU share one power budget, the GPU paid for it too.

[spin_pause.h](../renderer/src/spin_pause.h), which the project adds to the generated code's
precompiled header after ReXGlue's own (rexnative/project.py), makes `simde_mm_pause()` call
[rexnative_spin_pause](../renderer/src/native/spin_pause.cpp): the `PAUSE`, and once a thread's
pauses have gone on for 200 µs with no gap (a wait, not a short spin lock), a 100 µs sleep every
32 of them (a high resolution waitable timer; `nanosleep` on Linux). The loop still checks its
condition often and wakes within about 100 µs of it. `REXNATIVE_SPIN_SLEEP=0` in the environment
keeps the plain pause, to compare. A game gets it when built again: its generated code compiles
again (not generated again); `rexnative project --build` removes a build folder's older
precompiled header first, which Ninja would otherwise keep.

On the Steam Deck under Proton, the sleep off and on alternated: Jet Set Radio's heavy start
8.2-9.1 W -> 4.4-4.5 W (main thread 89-99 % -> 31 % of a core), Overlord II's 3D menu
10.7-11.3 W -> 5.6 W, the same 30 FPS and frame times; Tetris Splash's 60 FPS title unchanged
(it does not spin there). On the PC, Jet Set Radio's process 112-114 % of a core -> 32-34 %.

With the CPU no longer taking the power, frame latency 1 holds where it did not:
Midnight Club driving its city 29.6 FPS at 1 against 29.1 at 2, the picture 33 ms after the frame
began against 46 ms (it fell to 22-29 FPS at 1 in its races before); Jet Set Radio 23-33 ms
against 40-45 ms, the same frames. The Deck's installs now use 1 (rexnative/deck.py).

Not reached: Overlord II's intro videos under Proton keep its main thread at 76-80 %: the game
waits there in many short KeDelayExecutionThread calls, and ReXGlue's Windows `SleepFor` spins
the last 250 µs of every wait (`kSpinTail`).

## SDR white in HDR output

With HDR10 output the game's 1.0, its white (and the HUD), goes out at a paper white, and only what
it renders above that reaches past it. Blur on a 456-nit Windows display looked far brighter and
harder than in Xenia Canary on the same desktop, though the frame it composes was the same: in
RenderDoc captures of both at the race start the composite pass's output averaged 76.9 and 75.7
(of 255), and Xenia's presenter applied the same gamma table the log shows (64/128/192 -> 193/462/741
of 1023). The difference was all in the presentation:

- **Paper white.** 400 nits, gamescope's level for SDR content, put the game's white near the
  display's peak, where Windows shows SDR windows (Xenia's) at the display's SDR content brightness
  (Settings > Display > HDR; 240 nits there). `--rexnative_hdr_paper_white=0` (the default) takes
  that level on Windows, read with
  `DisplayConfigGetDeviceInfo(DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL)` (thousandths of
  80 nits); Wine has no such query and 400 stays.
- **The curve.** DWM turns SDR windows into light with the piecewise sRGB curve; a 2.2 power, which
  the blit used, gives the shadows a third or less of that light near black (0.05: 0.0014 against
  0.0039), the hard, flat look. `--rexnative_hdr_sdr_curve=auto` takes the sRGB curve where Windows
  reports its SDR brightness and keeps the 2.2 power under Wine.
- **Switching HDR while the game runs.** The colour space was chosen once, with the swapchain, so
  turning Windows' HDR off left an HDR10 signal on an SDR desktop. `D3D12Device::FollowDisplayMode`
  looks at the window's display about once a second and sets the swapchain up again when its HDR
  mode changes (and takes a new SDR brightness).

The log's `HDR10 output` line gives the levels.

## DLSS 5 on fewer frames

`--rexnative_dlss5_reuse` runs the model on one frame in two or three and carries its last change
along the scene's motion between. Tested on Blur (RTX 5060 Ti, the Cinematic two-pass look,
1280x720, the model at 1024x576): the model's GPU time went from 3.54 ms a frame to 1.85 ms (one
in two) and 1.29 ms (one in three). Frame traces of a race replayed with each setting (the same
frames, compared texel by texel) showed two things.

A carried change smeared a car onto the road beside it. The depth test lets a surface through
where it touches another at nearly the same depth, and a carried change has no model output to
clamp it to: the road at the car's rear wheel took the car's change and its blue underglow, a
stain on every carried frame (a blue excess of 4.5 there, 8.8 with one frame in three, against
0.2 with the model on every frame), growing over consecutive carried frames. The carry now also
tests the game's own picture: the last frame's input where the change comes from against this
frame's 3x3 input range, the change fading out from 0.02 to 0.08 outside it (0.6 and 1.4 since).
The last frame's input is kept for this and the motion check alike. Reading the chained motion
at the nearest texel instead of a filtered one, and the carried change with a Catmull-Rom read
instead of a bilinear one, changed little on their own.

The carried frames still show less change: at 85 mph, 15 against 19 (mean texel difference from
the game's picture), a pulse at half the frame rate where the surfaces move fastest (the wall
beside the car, the road under the camera). There the carry is rejected and the game's picture
shows: by the depth or the screen's edge without the picture test (15.4), and also where motion
blur changes the input's colour with it (14.9). Carrying the change where the picture does not
match brings the stain back, so a racing game keeps the model on every frame; slower scenes lose
less. An 8-frame trace looped every 8 frames is not a fair test of this setting: whether the
model runs on the frame after the loop's cut decided between two results (27 or 42), so the
comparison used a 60-frame trace, the first 20 frames of the last time through left out.

## A stencil reference for each face

Xenos has a stencil reference for back faces (`RB_STENCILREFMASK_BF`, with
`RB_DEPTHCONTROL.backface_enable`); D3D12 had one for both, so a two-sided draw drawn without
culling wrote the front's with its back faces too (Xenia's D3D12 backend the same; its Vulkan
backend sets both). D3D12 options 14 (`IndependentFrontAndBackStencilRefMaskSupported`) gives
each face its own (`OMSetFrontAndBackStencilRef`): the renderer sets the back's where the driver
has it (`--rexnative_independent_stencil_ref`, on), and otherwise keeps one for both, the back's
only when only back faces are drawn. The masks stay one for both faces (the pipeline's
`D3D12_DEPTH_STENCIL_DESC`). Forza Horizon's foliage is such a draw (front faces 0x81, back
faces 0, nothing culled; the draw log's `sr=` and `sb=`).

It does not change Forza Horizon's picture, though it was first taken for the cause of the sharp
ground under its cars: a trace of the intro drive replayed with it on and off draws the same
pixels (live shots of two runs differ by where the cars are, which looked like a change).

## A car's contact shadow in the motion blur

Forza Horizon's motion blur copies the depth and stencil to an 8888 texture (a depth resolve of
the 4x MSAA D24FS8 scene) and its mask pass (pixel shader E68FE55ECD482EC5, 640x360) reads it back
point-sampled: the depth's 20e4 bits from A, R and G (multiplied by 255, 65280 and 15.9375 =
255/16), the stencil from B (the fetch constant swizzles the stencil byte, R in memory, there).
Alpha (sharp) is `B != 0 && B < c253.w`, `c253.w` = 0.501968, about 32897/65536: the cars (3 the
player's, 0x0F and others the AI's) stay sharp, 0 (the road) and 0x81 (foliage) blur, and 0x80
sits a hair (7e-6) under the threshold with the PC's `x / 255`. Each car's contact shadow is a
slab around it (indices 1-4 the corners at the ground, 6-9 the same 20 cm higher, from vertex
shader constants: four sides and a top, 4.8 x 2.8 m around the Viper, no culling, depth test GE,
two-sided stencil off): an `EQUAL 0` draw inverts bit 7 (write mask 0x80), so the ground under
its top, the whole footprint, becomes 0x80; an `EQUAL 0x81` draw takes foliage to 0x80; the shadow
is drawn where it is 0x80; only the cars (`REPLACE` 3) and one `REPLACE` 0 draw write stencil
before the copy. The marking is the same on any correct rasteriser (the top is 20 cm in front of
the ground), and footage of the Xbox 360 (the intro drive, the Viper from the front) has the soft
shadow, so the console marks it too: yet that ground is blurred there, sharp in ReXNative's and
Xenia's emulation (the same frame in oracle mode: 44.8 dB under the bumper), as gravel around the
car with a straight edge. So on the console a point-sampled texel 0x80 reads at or above 0.501968,
above 128/255. Xenia canary had found the same (Forza Horizon and Forza Horizon 2's "sharp
rectangle around each car", its PR #1158 then #1177, 0c843ef, August 2026): the threshold is
32897 / 65536 exactly, the value of 128/255 rounded to 16 fractional bits, so Xenos returns
fixed-point texels in Q16 and the host's float 128/255 falls just short of it. ReXNative first
read point-sampled 8-bit texels as `(x + (x >> 7)) / 256` (sdk patch 0046, `--rexnative_texel_unorm8`),
which passes the threshold too but moves every texel by up to half a level (0x7F read 0.49609, so
`floor(v * 255)` gave 126). Since 2026-11-24 (sdk patch 0049) the translator does what canary does
for every fetch of a fixed-point format, filtered ones included: unsigned components rounded to
16 fractional bits (0x80 is 0.5019684), integer fetches scaled by each component's width,
unsigned-biased ones read as signed offset binary (128 is 0). The two traces of the intro drive,
stock, replayed: within 59-61 dB of 0046's pictures, the contact shadow the same (0.4-0.6 levels
apart there, 12-17 from `x / 255`), the rest moved only along the blur's edges, where Q16 sits
nearer the exact value. "Sharp in Xenia too" above was ReXGlue's own copy of Xenia (January 2026,
the oracle), not canary, whose reference build (`scripts/xenia.ps1`, 4 November) has the
rounding but was never run on Forza: compare a look with canary itself, not only the oracle.
Ruled out on the way (2026-11-23):
- hierarchical stencil, as far as `RB_HIZCONTROL`'s undocumented bits can be read from the
  values (the draw log's `hz=`): 0x13 for the whole main pass, these draws included (read as
  hierarchical Z on and written, the GE direction); the bits that look like the hierarchical
  stencil's are only set in the screen-space passes after the first depth copy (draws 275-287);
- the per-face stencil state (0044: the slab has two-sided stencil off);
- anything resetting 0x80 before the copy.
Xenia shows the same kind of thing in Forza Motorsport 4, the same engine: "square shadows" around
the cars (xenia-canary/game-patches#398, worked around there by turning shadow maps off). Until
this, the game's settings carried a threshold tweak (`rexnative.pixel_constant_override` =
`E68FE55ECD482EC5:c253.w=0.5`, rexnative/tweaks.py); `rexnative project` takes it out again.

## Primitive reset in 32-bit indices

Forza Horizon's wheels drew their rims as grey discs streaked with thin triangles and their tyres
on jagged dark blocks (the depth prepass' copy showed it already, Xenia's plugin drew the spokes).
Their index buffers are 32-bit triangle strips with primitive reset, `VGT_MULTI_PRIM_IB_RESET_INDX`
0x00FFFFFF, and the strips' cuts stored as 0x00FFFFFF: Xenos compares the low 24 bits of an
index with it, the host's strip cut all 32 bits with 0xFFFFFFFF. The renderer binds shared
memory as the index buffer (the vertex shader swaps the indices), so the host read the marker as
a vertex (16777215 once swapped, outside the mesh) and the strip ran on through it. Where the
guest and the host would cut apart, the draw now binds a copy (`UploadReset32`, in Prepare like a
fan's indices): the marker 0xFFFFFFFF, every other index its low 24 bits in guest endian, as the
primitive processor's `ReplaceResetIndex32To24` does for the plugin. Its scan of every 32-bit
strip with reset did not change the intro drive's draw CPU time (1.1 to 1.4 ms for 1000 draws).

## The control file, whole

`--rexnative_d3d_control` names a file the renderer reads and removes once per frame. Scripts used
to append to it (`Add-Content`, `cat >>`, `open(..., 'a')`), which first creates the file and
then writes into it: seen in between, the file was read empty and removed, and its commands were
lost without a word. A route driving Dark Souls II on the Steam Deck (written over SSH) lost half
its presses; on Windows, a script appending 1500 commands as fast as it could lost 39 and failed
68 more (the file being removed under it). Both ends changed:
- the writers (`live.ps1`'s Send-Control, `live.py`'s `control`) write the commands aside and move
  the file in place whole; one the game has not taken yet is waited for (3 s, then added to);
- the renderer (native/control_file.h, PollControl and the trace player) first renames the file to
  `<file>.taken`, which fails while a writer has it open on Windows and leaves a later append a
  file of its own, then takes it only once its last line is ended (a writer on Linux writes on
  into the renamed file; after a second it is taken as it is).
Against the old reader, emulated, the new writers lost none of 1500; against the new reader, the
old appends lost none of 3000 (the 62 that failed, failed in the writer, as errors). In Forza
Horizon, 80 `keys` presses 50 ms apart and 300 `Add-Content` lines in a row all arrived, in order.

## What Xenia canary and Edge had (2026-11-24)

A comparison with Xenia canary (canary_experimental 2da2519) and its fork Xenia Edge (c47487a9),
area by area (textures, render targets, shader arithmetic, primitives, the rest), brought over
what both renderers lacked and canary had tested:
- SDK patches 0049-0055: texel values as Xenos returns them (above), canary's shader fixes (the
  scalar operand beside a three-source vector op, signed memexport's rounding bias, vertex words
  past the buffer read 0, stacked layers clamped, a locked mip's unnormalized grid), PM4 0x5D and
  0x59 (their fences were dropped with the rest of the buffer), the texture layout fixes (packed
  level-0 tails, 2D array and volume mips, exact tiled 3D bounds, a base page beside a separate
  mip page, `force_bc_w_to_max`), MIN and MAX blending's source factor, the classic texture
  cache's watch races, transcendental results rounded to 21 mantissa bits, and 64bpp clear values
  in the order D3D writes them.
- ReXNative: a draw whose result is not drawn again waits for its pipeline instead of being
  skipped (memexport, a draw an occlusion query counts, a surface at most 160 pixels wide: an eye
  adaptation history, a lookup table); a vertex fetch constant of a texture's type is read as one;
  CONSTANT_ALPHA blend factors; an A8 resolve takes the source's alpha; rectangle lists clamp
  their depth instead of clipping; triangle fans restart after a primitive reset index;
  k_10_11_11 and k_11_11_10 textures (Spelunky's 384x384) and the 8-bit format aliases.
The two Forza Horizon traces, stock, stayed within 87-90 dB of the build before the last of these
and kept the contact shadow. An A/B of the Manager's test scenes of 13 games (the build before
against this one, built on its own SDK copy) found no regression once each scene was replayed long
enough: a scene replayed right after the rebuild (2 loops at no frame limit) showed black intro
videos, Midnight Club without its sky and Fable II with a dark frame, all draws skipped while
their new pipelines compiled (every shader's translation changed); with 6 loops each was back
(Iron Brigade's intro 71 dB from the old build, Midnight Club 52 dB, Fable II 48 dB, Blur as near
its reference as before). What did move: Midnight Club's MAX-blended glints on a truck, now
weighted by their alpha (MIN/MAX factors; not checked against the console), and edge pixels in
Fable II from the texel values (0.5 levels on average). Spelunky's missing text is unchanged:
its k_10_11_11 texture is not the cause. Compare a scene only after its pipelines are built. Edge no longer has a DXBC translator (its shaders go SPIR-V to DXIL
since July 2026), so its "results as the Xenos gives" work (exact RCP/RSQ, vertex math rounded
toward zero, centroid interpolation) would have to be written for DXBC anew. Left for later, most
useful first: resolves the CPU reads (Minecraft's save thumbnails), occlusion queries that wait
for a result the game blocks on (Perfect Dark's lens flares, Halo's intro), tessellated strips
and fans, 3D textures fetched as 2D, the remaining texture formats (CTX1, YUV, DXT3A_AS_1_1_1_1).
