# Keys and settings

## Keys

| Key | Pad | |
|---|---|---|
| F1 | Back + Start (the Deck: View + Menu) | the settings HUD |
| F9 | | switch between ReXNative and ReXGlue's Xenos GPU emulation (the HUD's Renderer too) |
| F7 | | the upscaler on or off |
| F8 | | show the upscaler's inputs |

The keys are settings too (`bind_rexnative_hud`, `bind_renderer_switch`, `bind_upscaler`,
`bind_upscaler_debug`).

## The settings HUD

F1 (or Back + Start on a pad) opens a settings HUD over the running game; what it changes is
saved into the game's configuration file. Up and down pick a setting, left and right (or A)
change it.

- **Renderer**, the first setting: Native (ReXNative) or Classic (ReXGlue's Xenos GPU emulation,
  Xenia's renderer), for a game the native renderer does not draw right yet. It switches at once,
  on the Steam Deck too, and the choice is kept for the next start (`rexnative.classic`). The HUD
  stays up on both, so the game can go back to Native the same way.
- **Upscaler page:** switches the upscaler, its type (automatic, DLSS or FSR), the FSR version
  and the mode (native anti-aliasing to ultra performance) while the game runs, and says what
  runs ("AMD FSR 4.1.2 Quality, 853x480 to 1280x720"). Frame generation (AMD FSR) switches there
  too while the game runs (`rexnative.frame_generation`): a generated frame before each of the
  game's, the game at half the screen's rate (30 FPS for 60 on the Steam Deck). It needs the
  upscaler and the frame pacer (`rexnative.frame_latency` above 0); the log's frame statistics
  say how many frames it generated ("frame generation: 120 of 120 frames (2 reset)").
  `rexnative.frame_generation_hudless` (on) keeps the HUD out of the generated frames;
  `rexnative.frame_generation_indicators` draws FidelityFX's reset and scene change marks.
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

- `rexnative.classic = true` (what the HUD's Renderer saves; F9 does not save) starts the game on
  ReXGlue's renderer with the HUD and the switch at hand; `--rexnative=false` starts it on
  ReXGlue's renderer alone, without them. Under Proton the switch needs the Xenos driver's frames
  in ReXNative's swap chain (its D3D12 backend on one GPU, as on the Steam Deck): without them the
  game stays on the native renderer, and `--rexnative=false` is the way to ReXGlue's.
- `rexnative.menu_fps` limits the frame rate: 60 by default, so menus made for a console's 60 Hz
  keep their speed on a faster display; 30 for a game the console ran at 30; 0 for no limit.
- `rexnative.upscaler_depth_test`: the upscaler takes the frame's last pass of the scene whose
  draws test depth, not only write it (Diablo III, whose menus draw the hero in a pass of its own).
- The older names (`--rexnative_upscaler`, PGR4Native's `pgr4_native_*` and `pgr4_d3d_*`) still
  work as aliases.

Two of ReXGlue's own settings (SDK patches 0040 and 0041), in the same file:

- `[kernel] user_name = "..."`: the profile's gamertag, up to 15 characters (default "User"):
  the name a game shows for the player, as Diablo III under P1. A hero's or a save's name is the
  game's own, typed in its keyboard prompt.
- `[ui] dialog_scale`: the size of the system dialogs (the keyboard prompt, message boxes); 0,
  the default, makes them twice as large on a 720p or 800p screen, three times at 1080p.
  They answer to the pad (D-pad or left stick, A, B for the last button) and the keyboard
  (arrows, Enter, Escape) as well as the mouse and the touch screen (SDK patch 0042).

## Inline draws

A game whose build inlined draws past the hooked D3D functions (Orcs Must Die!'s title, menus,
loading screens and HUD) shows them black. With `rexnative.inline_draws` ReXNative walks the push
buffer the game wrote between two hooked calls, as it walks a recorded command buffer, and draws
what it finds there. `rexnative.inline_draws_auto` (on by default) tries it by itself when 30
frames in a row reach the hooks with at most 2 draw calls while the game writes its push buffer,
and keeps it only if the walk finds draws (a video or a loading screen looks the same and has
none). It also probes the push buffer between hooked calls for 3 frames every 150, whatever the
draw count, and keeps the walk on when the game's own code loads shader constants there (Deus Ex:
Human Revolution's GPU-owned constants, without which its picture stays black).

`rexnative.auto_handover = 30` is the other way out: after 30 frames in which ReXNative sees at
most one draw call, ReXGlue's renderer draws until the game's draw calls come back, as F9 would.
It works under Proton too, as F9 does (the window keeps ReXNative's swap chain:
[NOTES.md](NOTES.md#the-renderer-switch-keeps-the-window)). A choice made with F9 or the HUD
turns it off for the rest of the run.

How inline draws work, and how they were found: [NOTES.md](NOTES.md#inline-draws).
