# Keys and settings

## Keys

| Key | Pad | |
|---|---|---|
| F1 | Back + Start (the Deck: View + Menu) | the settings HUD |
| F9 | | switch between ReXNative and ReXGlue's Xenos GPU emulation (not under Proton) |
| F7 | | the upscaler on or off |
| F8 | | show the upscaler's inputs |

The keys are settings too (`bind_rexnative_hud`, `bind_renderer_switch`, `bind_upscaler`,
`bind_upscaler_debug`).

## The settings HUD

F1 (or Back + Start on a pad) opens a settings HUD over the running game; what it changes is
saved into the game's configuration file.

- **Upscaler page:** switches the upscaler, its type (automatic, DLSS or FSR), the FSR version
  and the mode (native anti-aliasing to ultra performance) while the game runs, and says what
  runs ("AMD FSR 4.1.2 Quality, 853x480 to 1280x720").
- **DLSS 5 page:** picks a look, one set of DLSS 5's settings per kind of game (Cinematic,
  Cinematic with two passes, Natural, Stylized, Cel-shaded / 2D, Strong, or Custom), with its
  intensity on top. `project` gives each game the look that suits it (`rexnative/dlss5.py`:
  cel-shaded, anime and 2D games get Cel-shaded, cartoon and painterly 3D Stylized, racing two
  passes, realistic games Cinematic), after the modding community's findings (the Cinematic
  style and two passes on old console games; cel-shaded art only sharpened; a colour strength of
  0 keeps the game's hues).

## Settings

Every renderer option is a `rexnative.*` setting, in the `[rexnative]` section of the game's
configuration file (`run/<game>.toml`) or on the command line.

- `--rexnative=false` starts the game on ReXGlue's renderer.
- `rexnative.menu_fps` limits the frame rate: 60 by default, so menus made for a console's 60 Hz
  keep their speed on a faster display; 30 for a game the console ran at 30; 0 for no limit.
- `rexnative.upscaler_depth_test`: the upscaler takes the frame's last pass of the scene whose
  draws test depth, not only write it (Diablo III, whose menus draw the hero in a pass of its own).
- The older names (`--rexnative_upscaler`, PGR4Native's `pgr4_native_*` and `pgr4_d3d_*`) still
  work as aliases.

## Inline draws

A game whose build inlined draws past the hooked D3D functions (Orcs Must Die!'s title, menus,
loading screens and HUD) shows them black. With `rexnative.inline_draws` ReXNative walks the push
buffer the game wrote between two hooked calls, as it walks a recorded command buffer, and draws
what it finds there. `rexnative.inline_draws_auto` (on by default) tries it by itself when 30
frames in a row reach the hooks with at most 2 draw calls while the game writes its push buffer,
and keeps it only if the walk finds draws (a video or a loading screen looks the same and has
none).

`rexnative.auto_handover = 30` is the other way out: after 30 frames in which ReXNative sees at
most one draw call, ReXGlue's renderer draws until the game's draw calls come back, as F9 would.
It stays off under Wine, as F9 does: under vkd3d-proton the window cannot go from one renderer's
swap chain to the other's (the Xenos driver's Present then waits for good).

How inline draws work, and how they were found: [NOTES.md](NOTES.md#inline-draws).
