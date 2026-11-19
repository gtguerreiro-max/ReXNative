# Debugging a game

## A running game for experiments

Starting a game for every experiment costs minutes (logos, intro, menus). Instead,
`scripts\live.ps1 start -Dir <game dir> -Exe <exe> -Route "<keys>"` starts it once in a 1280x720
window and drives it to the problem; then each experiment takes seconds on the running game:

- `shot`: a capture of the window; `frame`: the same from the renderer itself, with no need for
  the window in front; `log`: the game's log; `stop`;
- `debug "<mask>"`: the renderer's switches (depth, stencil, cull, blend, alpha test off, skip a
  range of draws);
- `snap`: the render targets after given shaders (`python scripts\snaps.py` turns them into
  images); `drawlog`: one frame's draws; `trace`: the guest push buffer of one frame;
- in `-Route`, `pad:A+START:200` presses the game's pad through the control file (no focus
  needed, for a layer with XInputGetState); `pad:none:600:ry=32767` holds a stick or a trigger
  meanwhile (`lx` `ly` `rx` `ry` from -32768 to 32767, `lt` `rt` from 0 to 255). A lone Start
  or Back needs about half a second: the settings HUD holds a short one back (its View + Menu).
- `live.ps1 control "peek 03FD1000 131072"`: how many bytes of guest physical memory there are
  not zero, and the first 32, in the log (whether the CPU wrote something or only the GPU did);
  `scripts\rdoc.ps1 save <capture> -Event N -Texture T -X 4` saves a texture's mip 4.
- Frame generation: with it on, each capture (`touch <live>\cap\capture.request`, eight
  frames with `--rexnative_capture_burst=8`) also writes the frame as presented, the frame
  generated before it and the HUD-less copy (`f<N>-fg-real.png`, `-fg-generated.png`,
  `-fg-hudless.png`). A generated frame equal to the real one is a copy, not an interpolation;
  one between the real frames before and after it moves about a quarter of their two-frame
  motion (phase correlation of a strip). `rexnative.frame_generation_indicators` draws
  FidelityFX's marks on the generated frames (red band: reset, blue: scene change), and the
  log's frame statistics count the frames generated and the ones prepared in reset mode.
  The generated frames are captured for eight frames in a row, so each lies between two real
  ones it can be checked against: where it departs from their mean by more than they differ
  from each other, the display flickers.

## RenderDoc

With `start -RenderDoc` (RenderDoc's folder in `REXNATIVE_RENDERDOC`), `capture` takes a
RenderDoc capture of the next frame (through RenderDoc's target control, so a Windows prompt
holding the focus does not stop it), and `scripts\rdoc.ps1` queries it with the game closed:

- `actions`: every draw, its index count and targets;
- `history -X -Y`: which draws touched a pixel and what failed;
- `inputs -Event`: a draw's textures as PNG;
- `buffer -Offset -Size`: the renderer's copy of guest memory;
- `mesh`, `disasm`, `cbuffers -Event`, `save`.

Only a rebuild needs a new start. [NOTES.md](NOTES.md#recorded-command-buffers) shows how Iron
Brigade's missing scenery was found this way.

## On the Steam Deck

`python scripts\deck_drive.py <work>\<game> <label> "<at>:<item>,..." <end> [flags]` starts an
installed game from Steam with extra flags and, at the given seconds, sends keys, takes the
renderer's captures (`cap`), sets draw-debug switches (`dbg<mask>`) and reads the APU's power
(`pwr`); the captures and the log come back to `<work>\<game>\deck`.
