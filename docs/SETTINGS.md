# Keys and settings

## Keys

| Key | Pad | |
|---|---|---|
| F1 | Back + Start (the Deck: View + Menu) | the settings HUD |
| in the HUD: Page Up / Page Down, Q / E | in the HUD: LB / RB | the HUD's previous or next page |
| | in the HUD: Y (or its Snapshot button) | a debug snapshot for a report: the frame and a trace of the next frames, in `snapshots\` beside the executable ([DEBUGGING.md](DEBUGGING.md#snapshots-from-the-player)) |
| F9 | | switch between ReXNative and ReXGlue's Xenos GPU emulation (the HUD's Renderer too) |
| F7 | | the upscaler on or off |
| F8 | | show the upscaler's inputs |

The keys are settings too (`bind_rexnative_hud`, `bind_renderer_switch`, `bind_upscaler`,
`bind_upscaler_debug`).

## The settings HUD

F1 (or Back + Start on a pad) opens a settings HUD over the running game; what it changes is
saved into the game's configuration file. Its settings are on pages, a tab each: Display,
Picture, Upscaler, DLSS 5 (NVIDIA GPUs) and Developer. LB and RB (Page Up and Page Down, or Q and
E; or a click on a tab) turn the page, up and down pick a setting, left and right (or A) change
it. A value marked * takes effect at the next start.

- **Display page:** the renderer, the screen, the frame rate and latency, and the window.
- **Renderer**, the first setting: Native (ReXNative) or Classic (ReXGlue's Xenos GPU emulation,
  Xenia's renderer), for a game the native renderer does not draw right yet. It switches at once,
  on the Steam Deck too, and the choice is kept for the next start (`rexnative.classic`). The HUD
  stays up on both, so the game can go back to Native the same way. A fault in the native path
  (a layer whose offsets do not fit the game's D3D) hands the game to the classic renderer for
  the rest of the run instead of crashing.
- **Screen**: TV (console) or PC monitor (`rexnative.display_monitor`), at once, on both
  renderers. The console tells the game it is on an HDTV, and most games then write a gamma ramp
  that encodes the picture with the BT.709 camera curve (64/128/192 of 255 to 193/462/741 of
  1023). A TV shows that as meant; on a monitor or the Steam Deck's screen, whose curve is sRGB,
  the mid-tones come out about 20 % darker and the shadows about half as bright. PC monitor turns
  the signal back into light and encodes it for an sRGB screen, as a console connected to a
  monitor shows it. Captures taken as shown (`rexnative.capture_shown`) follow it.
- **Window** (not on the Steam Deck, where gamescope shows the game on the whole screen):
  Windowed, Full screen (a borderless window over the display) or Full screen (exclusive); the
  window's size when windowed (1280x720 to 2560x1440); and the display, when there are several.
  These are ReXGlue's own settings (`ui.fullscreen`, `ui.fullscreen_exclusive`,
  `ui.window_width`, `ui.window_height`, `ui.monitor`), applied at once and saved under `[ui]`.
- **Picture page:** the resolution, the anti-aliasing (MSAA, or DLSS / FSR: the upscaler's own
  switch, as F7), the screen scaling (FSR 1 or bilinear) and its sharpening, and the texture
  filtering.
- **Resolution** (`rexnative.resolution_scale`, 1 by default; not on the Steam Deck, where the
  renderer takes 1x whatever the file says and `rexnative deck` writes 1): 2x or 3x
  draws the game's screen-sized surfaces (the 3D scene, its post-processing and the front buffer)
  at two or three host pixels per guest pixel, a 1280x720 game at 2560x1440 or 3840x2160, as the
  Xbox One X (3x) and Series S (2x) run Xbox 360 games. The upscaler works at that size: DLSS's
  or FSR's native AA renders all of it, Quality at 3x renders 2560x1440 for 3840x2160. Taken at
  the next start. Forza Horizon on an RTX 5060 Ti: 4.2, 5.0 and 8.0 ms a frame at 1x, 2x and 3x
  with DLAA, 6.0, 14.4 and 30.4 ms with DLSS 5 on (a replayed trace of its intro drive). A
  picture larger than the window is shown at the window's size (bilinear), a supersampled frame.
- **Upscaler page:** the upscaler's type (automatic, DLSS or FSR), the FSR version and the mode
  (native anti-aliasing to ultra performance) while the game runs, and what runs ("AMD FSR 4.1.2
  Quality, 853x480 to 1280x720"). Frame generation (AMD FSR) switches there
  too while the game runs (`rexnative.frame_generation`): a generated frame before each of the
  game's, the game at half the screen's rate (30 FPS for 60 on the Steam Deck). It needs the
  upscaler and the frame pacer (`rexnative.frame_latency` above 0); the log's frame statistics
  say how many frames it generated ("frame generation: 120 of 120 frames (2 reset)").
  `rexnative.frame_generation_hudless` (on) keeps the HUD out of the generated frames;
  `rexnative.frame_generation_indicators` draws FidelityFX's reset and scene change marks;
  `rexnative.frame_generation_debug` its debug view (DEBUGGING.md);
  `rexnative.frame_generation_sharpness` (0.5, 0 to 1, 0 off) sharpens the generated frames
  where the scene moves, to the real frames' detail (NOTES.md).
- **DLSS 5 page:** DLSS 5 (`rexnative.dlss5`, NVIDIA RTX) runs inside the upscaler; with the
  upscaler off it brings it on as DLAA at the game's resolution. The page picks a look, one set
  of DLSS 5's settings per kind of game (Cinematic,
  Cinematic with two passes, Natural, Stylized, Cel-shaded / 2D, Strong, or Custom), with its
  intensity on top. `project` gives each game the look that suits it (`rexnative/dlss5.py`:
  cel-shaded, anime and 2D games get Cel-shaded, cartoon and painterly 3D Stylized, racing two
  passes, realistic games Cinematic), after the modding community's findings (the Cinematic
  style and two passes on old console games; cel-shaded art only sharpened; a colour strength of
  0 keeps the game's hues).
  Its speed (`rexnative.dlss5_reuse`: 0 every frame, 1 every 2nd, 2 every 3rd) runs the model
  on fewer frames: a frame between carries the last change along the scene's motion, rejected
  where the depth disagrees or where the game's picture there does not match the one it came from
  (what the camera uncovers, or the road beside a car, then shows the game's picture for a frame),
  and the model gets the motion chained back to the frame it last ran on. 1 nearly halves DLSS
  5's cost; the fastest surfaces then pulse at half the frame rate, so a racing game is better
  on every frame (NOTES.md, DLSS 5 on fewer frames). `rexnative.dlss5_colour_space = "oklab"` mixes the game's colour and the model's in
  Oklab when the colour strength is between 0 and 1 (Natural's 0.65, or Custom), keeping the
  saturation a mix in RGB loses between two hues; `rgb` (the default) as before.
  Its still hold (`rexnative.dlss5_hold`: 0 off, 0.8 gentle, 0.95 strong; 1 held until the
  picture changes) keeps the last change where the game's picture stands still, against the
  model's slow drift in still regions after the camera stops (DLSS5-Feeder's output stabiliser);
  `rexnative.dlss5_hold_tolerance` (0.04) is how much a texel's input may differ and still count
  as still. A texel that moves (the scene's motion) or whose input changed shows the model as is.
  Its motion check (`rexnative.dlss5_mv_check`) tests the scene's motion against the picture before
  the model gets it: DLSS 5 ignores its depth input (Veyra's controlled test) but follows its
  motion closely, and the rebuilt vectors are wrong on some draws (NOTES.md, Motion vectors from
  a vertex ring). Where a texel's 3x3 neighbourhood did not change
  (`rexnative.dlss5_mv_still`, 0.008), or no motion explains it at least twice as well as the
  vector (DLSS5-Feeder's Refine pipeline), the model gets no motion there.

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
- `rexnative.frame_latency` (the HUD's Frame latency) and `rexnative.frame_latency_auto` (on: the
  HUD's "Auto (1-2)" with latency 1). At 1 the game, the render thread and the GPU work one frame
  after the other, the least input delay; a heavy game then misses the display's pace (Blur on
  the Steam Deck: half its stretches slow at 1, almost none at 2). Auto tries 2 for 4 s, 5 s after
  the game settles and then every 15 s (less often each time it does not help, and at once when
  frames start missing their slot). When frames come at least 5 % sooner at 2 it keeps 2, tries 1
  once 20 s later for about a second to confirm it, and then makes 2 the game's setting for good:
  `frame_latency = 2` written in its settings file, as if picked in the HUD (a game that needed 2
  will need it again; trying 1 now and then would only stutter). A setting of 2 or 3 is left as
  it is. The log says each step ("frame latency 2 for this game (automatic: a frame every 41.7 ms
  at 1, 33.3 ms at 2; saved in its settings)"). `rexnative.debug_game_cost_us` and
  `rexnative.debug_frame_cost_us` make a fast PC's frames as heavy as a handheld's, for testing it.
- `rexnative.upscaler_depth_test`: the upscaler takes the frame's last pass of the scene whose
  draws test depth, not only write it (Diablo III, whose menus draw the hero in a pass of its own).
- `rexnative.scene_late_draws`: scene detection takes an 8-bit surface without MSAA as the scene
  when 3D draws went into it before any of its full-size resolves, not only its first (Iron
  Brigade resolves it once after its depth prepass, before the scene's draws).
- `rexnative.texel_unorm8` (default true): point-sampled 8-bit unsigned texels read as on Xenos,
  `(x + (x >> 7)) / 256` (0x80 is 0.50390625), not the PC's `x / 255`, in a shader whose
  textures all have 8 bits per component (sdk patch 0046); filtered fetches stay `x / 255`.
  Forza Horizon's motion blur tells them apart (docs/NOTES.md, "A car's contact shadow in the
  motion blur"); false for the PC's reading.
- `rexnative.pixel_constant_override`: a picture tweak, not the console's emulation: pixel shader
  float constants replaced, as `<ucode hash>:c<register>.<x|y|z|w>=<value>`, several joined by
  `;`, for testing. `rexnative project` writes none now (rexnative/tweaks.py): it takes out Forza
  Horizon's `E68FE55ECD482EC5:c253.w=0.5`, which it wrote before `texel_unorm8` made that look
  the emulation's own, where the file still has that value.
- `rexnative.pipeline_hold_min` (default 8): a pipeline the game never created before compiles
  on worker threads (50 to 170 ms on the Steam Deck). A frame that waits for at least this many
  of them, a scene's first, is held back while they compile (the previous one stays on screen);
  a frame mid-game waiting for fewer is shown without those draws, which appear a frame or a few
  later, instead of stuttering. 1 holds every such frame (before 2026-11-24);
  `rexnative.pipeline_wait = false` holds none. Pipelines compiled ahead from earlier runs (the
  prewarm) run below normal priority, so the game's own threads keep the CPU.
- The older names (`--rexnative_upscaler`, PGR4Native's `pgr4_native_*` and `pgr4_d3d_*`) still
  work as aliases.

Three of ReXGlue's own settings (SDK patches 0040, 0041 and 0048), in the same file:

- `[kernel] user_name = "..."`: the profile's gamertag, up to 15 characters (default "User"):
  the name a game shows for the player, as Diablo III under P1. A hero's or a save's name is the
  game's own, typed in its keyboard prompt.
- `[kernel] user_language = N`: the console's language, what `XGetLanguage`, `XamGetLanguage`
  (SDK patch 0048) and the user language of `ExGetXConfigSetting` tell the game: 1 English (the
  default), 2 Japanese, 3 German, 4 French, 5 Spanish, 6 Italian, 7 Korean, 8 traditional
  Chinese, 9 Portuguese, 11 Polish, 12 Russian, 13 Swedish, 14 Turkish, 15 Norwegian, 16 Dutch,
  17 simplified Chinese; 0 takes the host's. A game made in that language plays in it (Limbo's
  menus in Portuguese at 9); one that is not stays in English. The languages a game is made in
  are those of its SPA's string tables (its achievements' texts), which the ReXNative Manager
  offers as the game's Language.
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
