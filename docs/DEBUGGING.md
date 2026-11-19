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
