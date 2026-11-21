# Debugging a game

Every tool here is in the renderer or in `scripts\`, the same for every game, and prints little:
a path, a short list, only what differs. Start a game once with `scripts\live.ps1 start`, drive
it to the problem, and ask it questions; nothing needs a restart but a rebuild.

## Which tool

| The symptom | The tool | What it answers |
|---|---|---|
| Something is drawn wrong and stays wrong | `live.ps1 capture`, then `rdoc.ps1 summary`, `history`, `inputs` | which pass and draw made the pixel, with what |
| It is wrong only now and then, or only in motion | `live.ps1 record 4`, then `live.ps1 play` | the same frames again, every time: wrong in the replay too, the data or the drawing is wrong; right in the replay, the timing or the renderer's leftover state is |
| Right in the replay, wrong in the game | `live.ps1 record 1` at the fault, `live.ps1 play`, `live.ps1 compare` | the first resolve of that frame that differs between the game and its replay: the pass the leftover state is in |
| A texture looks wrong | `live.ps1 audit` | the textures whose guest memory changed unseen, and the ones whose GPU copy is not guest memory's |
| Wrong after other screens, right when entered at once | `live.ps1 idle` | whether a resolved image left by an earlier screen is in it: dropped live, the fault goes |
| Which pass goes wrong first | `live.ps1 start -Oracle`, `live.ps1 oracle` | the first resolve that differs from the Xenos plugin's, same frame |
| Is ReXGlue's renderer right itself | `xenia.ps1 start`, `shot`, `capture` | Xenia Canary at the same spot |
| A renderer change broke a game, or fixed one | `live.ps1 play <trace> -Extra "--rexnative_trace_play_loops=2 --rexnative_trace_play_dump=true"` | the trace's frames as images, before and after, without playing |
| Which draw | `live.ps1 debug "<mask> 0 <from> <to>"`, `snap`, `drawlog` | skip a range of draws, the targets after a draw, a frame's draws as text |
| The renderer faulted | the log's `the fault was caught` line | the address read and the code's offset in the executable (its `.map`) |
| A device removed, a validation error | `--rexnative_dred`, `--rexnative_d3d12_debug` with `--gpu.d3d12.debug=true` | the driver's and the debug layer's account |

## RenderDoc

`live.ps1 start -RenderDoc` makes the renderer load RenderDoc itself
(`--rexnative_renderdoc=auto`: `REXNATIVE_RENDERDOC`, the newest `RenderDoc*` in a `Tools` folder
at the root of a drive, or RenderDoc's installed folder): no launcher, no environment, and only
the renderer's device is captured, not the Xenos plugin's. A game started from RenderDoc works as
well. Then:

- `live.ps1 capture 3` takes the next three guest frames, a capture each, whole (from one Swap
  to the next frame's last Present), named `<live>\rdoc\<game>-f<frame>.rdc` and listed in
  `captures.txt`. The key F6 (`bind_renderdoc`) does it from the keyboard
  (`rexnative.renderdoc_burst` frames), `--rexnative_renderdoc_frame=N` at guest frame N of a
  scripted run, and `--rexnative_renderdoc_on_mark` with every mark (F12, `capture.request`),
  so the renderer's own PNGs and the capture show the same moment.
- `rdoc.ps1 summary <capture>` prints the frame in a few lines: each pass with its render
  targets (EDRAM base, format, MSAA) and its draws (`d12-d340 e2210-9120`), each clear, each
  resolve with its source rectangle and destination address, the Swap. `draws -Event <pass>`
  lists a pass's draws (`d57 prim 4 n 1236 vs 6160EBE04B693B5E ps 804E2F1E4E062B9A`), and
  `find -Text <hash or address>` the events named with it. A draw's `d<index>` is its index in
  the frame, the one `draw-debug.txt`'s skip range and the draw log use, and its shader hashes are
  the ones `snap` and the shader dumps take.
- Resources are named: `tex 1A2B3000 k_DXT4_5 512x512x1 mips 10 tiled` (guest address, format,
  size), `rt @0 color k_8_8_8_8 1280x720 x2` (EDRAM base), `resolved 1954F000 ...`, `guest
  physical memory (shared memory)`.
- The older queries work from an event number: `history -X -Y` (which draws touched a pixel and
  what failed), `inputs -Event` (a draw's textures as PNG), `mesh`, `disasm`, `cbuffers`,
  `state`, `save`, `pick`, `debug` (the pixel shader stepped), `buffer -Offset -Size` (the
  renderer's copy of guest memory).

A capture replays the frame's commands one after the other, with the data the renderer meant to
give the GPU. A fault that depends on when the CPU and the GPU ran (it goes away with
`--rexnative_gpu_serialize=true` or without the render thread) is not in it: the capture looks
right while the game looks wrong. That alone says the commands are right and their timing is not.
Delete the captures after use: each holds every texture the frame read and the renderer's copy
of guest memory (3.4 GB for a frame of a Blur race, 18 s for two).

RenderDoc slows a game while it is loaded (it stands between the renderer and every D3D12 call:
a Blur race went from 33 to 60 ms a frame), so start with `-RenderDoc` to capture, not to play.
The events are recorded only in the frames captured.

`--rexnative_markers` records the same events and names without RenderDoc, for PIX and for the
debug layer's messages. [NOTES.md](NOTES.md#recorded-command-buffers) shows how Iron Brigade's
missing scenery was found with a capture.

## Frame traces: the frames again, without the game

A recompiled game has no save state (its state is on native stacks and in kernel objects), but
the renderer's side of a frame can be recorded whole, as Xenia records a GPU trace.
`live.ps1 record 4` writes the next four frames to `<live>\trace\<game>-f<frame>.rxt`: every
draw, clear, resolve and Swap as the hooks decoded it, the register writes between them, and the
guest memory the renderer read for them (vertices, indices, shader ucode, textures), each page
once per content. It needs the render thread (the default); the frames recorded run slower. Four frames of a Blur
race (9301 commands) took 2 s and 130 MB, 110 of them textures.

`live.ps1 play <trace.rxt>` starts the game's own executable (which has the renderer and the
game's layer) with `--rexnative_trace_play`: at the game's first Swap the game stops for good
and the trace's frames are drawn over and over, on one thread, the same every time. The played
trace is the live game for every other command: `frame`, `capture`, `debug`, `snap`, `drawlog`,
`control "set ..."`. So a fault seen once in a race is looked at for as long as it takes, and a
renderer change is checked in seconds:

    live.ps1 play <trace.rxt> -Extra "--rexnative_trace_play_loops=2 --rexnative_trace_play_dump=true"

plays the trace twice, captures every frame of the second time (`<live>\cap\f<index>-native.png`,
the index counted from the player's first frame) and exits; `--rexnative_trace_play_fps=0` plays
as fast as it goes (Blur's race trace: 16 s from the command to the four images, the game's
boot included; about 50 frames a second while it plays).

What a trace does not hold is what the GPU made before its first frame: the render targets, and
the resolves of earlier frames that were not written back to guest memory (the frame before's
reflection or exposure). The first time through, the first frame reads them empty; from the
second time on they are the last frame's, so look at a later time through, or record a frame or
two more than needed. A trace replays in a build whose command structures have the sizes it was
recorded with (the player says so otherwise: record it again).

A played trace also sorts a fault at once. The trace is what the game gave the renderer, and
the player starts with a renderer that has drawn nothing else. So a fault that shows in the
replay is in that data or in how the renderer draws it; one that never shows in the replay,
while the live game shows it, is either in the timing between the game, the render thread and
the GPU, or in what the renderer kept from earlier frames (a cached texture or resolved image,
a shared memory page taken as current, a pipeline). Blur's black patches (2026-11-20): four
frames recorded with the patches on the screen replayed clean, every time, so the game's draws,
registers, vertices and textures were right.

`live.ps1 compare` then finds where the two part. `record` also dumps the resolves of the
trace's first frame as the game drew it (`<live>\resolves`), and the player dumps the same frame
the third time through (`<live>\resolves-play`); `compare` sets them side by side, each resolve
against the one into the same destination, and prints only those apart (mean difference above
0.004, or `compare 0.02`), each with a PNG of both. Record one frame (`record 1`) at the fault.
The passes of a frame feed each other, so the first resolve listed is where the game's state
differs from a fresh renderer's, and when only the last one is listed the fault is in the final
pass's own inputs. Resolves of one texel (exposure, adapted over frames) differ by themselves.
Compare once the replay has settled: what a game adapts over frames (exposure) starts from
nothing in a replay, so `compare` waits for the third time through and a trace of one frame is
best compared again some seconds later (create `<live>\resolves-play\dump.request`, then
`python scripts\resolves.py <live>\resolves --against <live>\resolves-play --over 0.002`). Float
targets are compared as a display shows them: a fault in dark tones is a small number in linear
light. Blur: 64 of the frame's 67 resolves were equal, the exposure too, and the final image was
not; the pixels apart were the ones of a narrow band of one colour channel, a tenth of the
screen, which is what one wrong slice of a colour grading volume does
([NOTES.md](NOTES.md#resolved-images-that-outlive-their-memory)). More switches sort what is
left: `live.ps1 control "set rexnative_gpu_serialize true"` live (a fault that stays is not
timing), `control "set rexnative_texture_reload true"`, `live.ps1 audit` and `live.ps1 idle`.

`live.ps1 audit` checks the textures drawn with in the last second, in two lines when all is
well: first each one's guest memory against the hash taken when it was uploaded (a texture the
game rewrote without the cache seeing the write is listed), then each one read back from the GPU
against guest memory untiled now, subresource by subresource (a copy that went wrong, or
something else written over it, is listed with the levels that differ). A texture that holds
resolved data and differs from guest memory is listed apart: that is expected of a texture a
resolve wrote into, and it is where an image that outlived its memory shows (Blur's colour
grading volume: 4608 of 131072 bytes). Textures bound as a resolved image are counted only.

`live.ps1 idle` drops, in the running game, every resolved image that nothing was resolved into
for 60 frames (`idle 600` for another age) and names them in the log; the textures over their
memory are uploaded from guest memory again. A fault that goes away at that moment was a stale
image. Blur's patches went with it, 49 images dropped, one of them from the boot screens.

Two sessions working at once share the PC's one live game unless each sets
`$env:REXNATIVE_LIVE_STATE` to a folder of its own: `start`, `play` and `stop` act on the game
of that state. `play` takes the game from the trace's own path (`<game>\live\trace\<exe>-f<frame>.rxt`).

## Against a reference

`live.ps1 start -Oracle` runs the game with the Xenos plugin presenting and ReXNative drawing
beside it, both from the same guest frame. `live.ps1 oracle` dumps every resolve of the next
frame from both and prints only the ones whose mean difference is above 0.02 (`oracle 0.005` for
another threshold), each with a side by side PNG, then how many were compared and the first one
apart: that resolve's pass is where the frame went wrong (`scripts\resolves.py`).

`scripts\xenia.ps1` is the independent reference, Xenia Canary: `start <default.xex> [-RenderDoc]
[-Route ...]`, `keys`, `shot`, `capture`, `stop`, with keys posted to its window (X is Start,
Enter is A). Its captures have no ReXNative events, and it draws a frame with its own targets,
so its passes are matched by what they draw.

## A running game for experiments

Starting a game for every experiment costs minutes (logos, intro, menus). Instead,
`scripts\live.ps1 start -Dir <game dir> -Exe <exe> -Route "<keys>"` starts it once in a 1280x720
window and drives it to the problem; then each experiment takes seconds on the running game:

- `shot`: a capture of the window; `frame`: the same from the renderer itself, with no need for
  the window in front; `log`: the game's log; `stop`;
- `debug "<mask>"`: the renderer's switches (depth, stencil, cull, blend, alpha test off, skip a
  range of draws);
- `snap`: the render targets after given shaders or after a draw (`python scripts\snaps.py`
  turns them into images; a multisampled colour target is resolved first, a multisampled depth
  target is left out); `drawlog`: one frame's draws; `trace`: the guest push buffer of one frame;
- in `-Route`, `pad:A+START:200` presses the game's pad through the control file (no focus
  needed, for a layer with XInputGetState); `pad:none:600:ry=32767` holds a stick or a trigger
  meanwhile (`lx` `ly` `rx` `ry` from -32768 to 32767, `lt` `rt` from 0 to 255). A lone Start
  or Back needs about half a second: the settings HUD holds a short one back (its View + Menu).
- `live.ps1 control "peek 03FD1000 131072"`: how many bytes of guest physical memory there are
  not zero, and the first 32, in the log (whether the CPU wrote something or only the GPU did);
  `control "poke 0C93A000 3F800000"` writes dwords there (big-endian, as the guest's CPU), to
  see what the game makes of a value; `control "watch 0C93A000 read 8"` logs the guest
  functions that read or write a physical address through its three CPU views (hardware
  breakpoints; `watch off`); `scripts\rdoc.ps1 save <capture> -Event N -Texture T -X 4` saves
  a texture's mip 4.
- Without the Xenos copy (`--rexnative_capture_xenos=false`), `--rexnative_capture_burst=8`
  writes eight presented frames in a row, each from its own readback: one slot took every other
  frame, which hid a flicker that alternates frame by frame. A still scene shows a flicker best:
  any pixel that changes there is one (at a race start without throttle, only the crowd moves).
- `--rexnative_d3d_tap --rexnative_d3d_tap_check --rexnative_d3d_tap_check_range=4070-415F`: the
  push buffer decoded on its own and compared with the tracker at every draw, for those registers
  alone (vertex constants c28-c87 here), each draw that differs logged; the first one traces the
  next two frames (`rexnative-d3d-rangediff-f*.txt` in the trace folder, `# range_diff` lines).
  A mesh broken by one bone's vertices: its constants missed, this finds where.
- Sound: every `rexnative.d3d_stats_frames` frames the log has a `ReXNative audio:` line, the
  SDK's mix over those frames: streams open, frames queued, underruns (periods a stream had
  nothing queued: gaps and clicks), the latency from the game's Submit to the device and the
  output channels (a stereo device gets the 5.1 mix folded, LFE left out). The PC's Sega Rally race:
  0 underruns, 42 ms.
- Timing races with the render thread (a bug that `--rexnative_render_thread=false` or
  `--rexnative_gpu_serialize=true` hides): `--rexnative_render_thread_lockstep=true` makes each
  Swap wait until the render thread recorded the frame (the guest never writes ahead), and
  `live.ps1 control "set rexnative_texture_reload true"` uploads every cached texture again
  from guest memory at the next frame (a texture whose memory changed unnoticed). Blur's black
  patches kept both: not the guest writing ahead, not stale textures (they were a resolved
  image that outlived its memory, NOTES.md).
- `--rexnative_coherency_check`: each vertex and index range a draw reads is compared with guest
  memory, and a stale page is logged (slow). A mesh broken in whole 4 KB pages of its vertex
  buffer is a stale page; one broken by the vertices of one bone is a constant (NOTES.md,
  "Inline draws, found at run time").
- Frame generation: with it on, each capture (`touch <live>\cap\capture.request`) also writes,
  for eight frames in a row, the frame as presented, the frame generated before it and the
  HUD-less copy (`f<N>-fg-real.png`, `-fg-generated.png`, `-fg-hudless.png`), off the render
  thread. With frame generation on a capture does not hold the guest thread for the Xenos
  plugin's copy (`--rexnative_capture_xenos`): the half-second hold made FidelityFX see a scene
  change, and the next four generated frames, the ones the capture takes, were copies. A generated frame equal to the real one is a copy, not an interpolation;
  one between the real frames before and after it moves about a quarter of their two-frame
  motion (phase correlation of a strip). `rexnative.frame_generation_indicators` draws
  FidelityFX's marks on the generated frames (red band: reset, blue: scene change), and the
  log's frame statistics count the frames generated and the ones prepared in reset mode.
  The generated frames are captured for eight frames in a row, so each lies between two real
  ones it can be checked against: where it departs from their mean by more than they differ
  from each other, the display flickers (a moving high-contrast edge departs from that mean by
  itself, so compare runs with alike motion). `rexnative.frame_generation_debug` (live) puts
  FidelityFX's debug view in the generated frames: top row the game's motion vectors (grey: none),
  their depth priority and the optical flow; bottom row the disocclusion mask (yellow: seen in
  both frames, red: only in the previous one, green: only in this one, black: in neither), the
  frame interpolated from and the HUD-less frame. A flat grey optical flow while the view moves is
  the fault NOTES.md's "Frame generation's optical flow" tells of. `--rexnative_frame_generation_check`
  turns on FidelityFX's checks of its inputs, its messages in the log. The captures also write
  the scene's motion vectors, the frame generation's input (`f<N>-fg-motion.raw`: "RXMV", width
  and height as uint32, then rows of half-float pairs in UV, current to previous): warping the
  HUD-less frame before by them gives this one where they are right, and the error map shows
  where they are not (a band of the road whose draw read another mesh's vertices, NOTES.md).

## On the Steam Deck

`python scripts\deck_drive.py <work>\<game> <label> "<at>:<item>,..." <end> [flags]` starts an
installed game from Steam with extra flags and, at the given seconds, sends keys, takes the
renderer's captures (`cap`), sets draw-debug switches (`dbg<mask>`) and reads the APU's power
(`pwr`); the captures and the log come back to `<work>\<game>\deck`.
