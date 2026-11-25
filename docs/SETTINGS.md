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
it. A value marked * takes effect at the next start. Its title bar shows how the game runs now
(the frames' recent mean: frames a second and ms) and the battery, its footer the pad's buttons
(L1 and R1 on the Steam Deck) for what the menu does; the game is dimmed behind it. On the Steam
Deck it knows it is there from Steam's `SteamDeck=1`, or the Deck's own firmware (Valve's board)
when a game was started without it.

- **Display page:** the renderer, the screen, the frame rate and latency, the window, and the
  **Menu size** (`rexnative.hud_scale`: Normal 100, Large 115, Larger 130 percent of its text,
  as Valve asks a Steam Deck game's text to be the player's to make larger).
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
- **Screen scaling** (`rexnative.fsr`, FSR 1 by default; `rexnative.fsr_sharpness`, 0.2): the last
  step, the finished picture (the game's resolution, its HUD and menus on it) scaled up to a
  larger window, FSR 1 (EASU, then RCAS's sharpening) or bilinear; only when the window is larger
  than the picture, and not with HDR output. The upscaler (FSR 4, FSR 3.1, DLSS) works earlier
  and on the 3D scene alone, whose depth and motion it needs: the finished picture has neither, so
  it cannot scale that. For the upscaler up to the screen's size, Resolution 2x or 3x draws the
  whole picture at that size and the upscaler works there (FSR 1 then has nothing to scale). Not
  shown on the Steam Deck, whose 1280x800 screen shows a 720p game at its own size: it never ran
  there (docked to a TV, gamescope scales the Deck's picture, with Steam's scaling filter).
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
  HUD's "Auto (1-3)" with latency 1 or 2). At 1 the game, the render thread and the GPU work one
  frame after the other, the least input delay; a heavy game then misses the display's pace (Blur on
  the Steam Deck: half its stretches slow at 1, almost none at 2). Auto tries 2 for 4 s, 5 s after
  the game settles and then every 15 s (less often each time it does not help, and at once when
  frames start missing their slot, or 4 come late within 3 s). When frames come at least 5 % sooner
  at 2, or late half as often and no slower (a game at its own 30 fps, whose stutters hardly change
  the mean), it keeps 2, tries 1 once 20 s later for about a second to confirm it, and then makes 2
  the game's setting for good: `frame_latency = 2` written in its settings file, as if picked in the
  HUD (a game that needed 2 will need it again; trying 1 now and then would only stutter), automatic
  still on. From 2 (that one, or 2 picked with automatic on) it goes to 3 where frames keep coming
  late at 2: 4 late frames within 3 s try 3 for 6 s, which it keeps with half the late frames or
  fewer and no slower; a minute later 2 is tried again, and 4 late frames there make 3 the game's
  setting for good (fewer: back to 2, trying 3 again on late frames, less often each time). Forza
  Horizon on the Steam Deck streaming its map: 1 to 10 frames over 50 ms in each 150 at 2, none at
  3, with 32 to 44 ms from a frame's start to the display (the delay before each frame takes up what
  3 adds). A setting of 3, or 1 or 2 without automatic, is left as it is. The log says each step
  ("frame latency 2 for this game (automatic: a frame every 41.7 ms at 1, 33.3 ms at 2; saved in its
  settings)", "frame latency 3 for this game (automatic: 5 frames late at 2 in 0.8 s, 0 in 6 s at 3;
  saved in its settings)"). `rexnative.debug_game_cost_us` and `rexnative.debug_frame_cost_us` make
  a fast PC's frames as heavy as a handheld's, for testing it.
- `rexnative.upscaler_depth_test`: the upscaler takes the frame's last pass of the scene whose
  draws test depth, not only write it (Diablo III, whose menus draw the hero in a pass of its own).
- `rexnative.scene_late_draws`: scene detection takes an 8-bit surface without MSAA as the scene
  when 3D draws went into it before any of its full-size resolves, not only its first (Iron
  Brigade resolves it once after its depth prepass, before the scene's draws).
- `rexnative.pixel_constant_override`: a picture tweak, not the console's emulation: pixel shader
  float constants replaced, as `<ucode hash>:c<register>.<x|y|z|w>=<value>`, several joined by
  `;`, for testing. `rexnative project` writes none now (rexnative/tweaks.py): it takes out Forza
  Horizon's `E68FE55ECD482EC5:c253.w=0.5`, which it wrote before the texel values (SDK patch
  0049) made that look the emulation's own, where the file still has that value.
- `rexnative.texel_unorm8` is gone (2026-11-24): texture fetches return what Xenos returns for
  every format, filtered ones too, with no setting (SDK patch 0049, docs/NOTES.md).
- `rexnative.pipeline_hold_min` (default 8): a pipeline the game never created before compiles
  on worker threads (50 to 170 ms on the Steam Deck). A frame that waits for at least this many
  of them, a scene's first, is held back while they compile (the previous one stays on screen);
  a frame mid-game waiting for fewer is shown without those draws, which appear a frame or a few
  later, instead of stuttering. 1 holds every such frame (before 2026-11-24);
  `rexnative.pipeline_wait = false` holds none. Pipelines compiled ahead from earlier runs (the
  prewarm) run below normal priority, so the game's own threads keep the CPU.
- `rexnative.fsr_auto` (default true) with `rexnative.fsr_version = "auto"` (the HUD's Newest):
  FSR 4 costs about the same time a frame whatever the render size, 5.1 ms on the Steam Deck in
  Diablo III's town at 1120x584 (FSR 3.1 3.1 ms; the scene's own draws 4.1 ms), a third of a
  60 fps frame. Where the GPU is busy for at least `rexnative.fsr_auto_share` (0.9) of the game's
  frame time for two windows of 120 frames in a row, after the first 600, FSR 4 gives way
  to FSR 3.1 for the rest of the run ("FSR 4 leaves the GPU no room ... FSR 3.1 for the rest of
  this run" in the log; the HUD's upscaler line shows what runs). Where FSR 3.1 leaves no room
  either, the upscaler goes off for the run and the scene keeps its own MSAA, the console's
  anti-aliasing: Iron Brigade at 60 fps (FPS Boost) on the Deck ran at 48.7 fps with FSR 3.1 (the
  GPU 17.5 ms a frame) and 58.6 with its MSAA (14.4 ms). Nothing is saved: the next run starts at
  FSR 4 again. The game's frame time is whole refreshes of the display at its own pace: a game
  that keeps itself at 30 fps on a 60 Hz display has 33.3 ms (Forza Horizon and Overlord II on the
  Deck, FSR 4 busy 19 ms of it, keep FSR 4), one that falls short of 60 (48.7 fps) has the
  16.7 ms it misses. A game held by its CPU (the GPU's time waiting for it is not counted) keeps
  FSR 4, which 3.1 would not make faster; an explicit `fsr_version` 4 or 3 is kept.
- `rexnative.battery_saver` (the HUD's Battery saver, on the Picture page under Antialiasing, which
  shows "- MSAA on the battery" while it holds the upscaler off; off by default, on for the Deck):
  on the battery (a Steam
  Deck, a laptop: GetSystemPowerStatus, which Proton reads from the system's power supply), the
  upscaler goes off and the scene keeps its own MSAA, the console's anti-aliasing; back on the
  charger, the upscaler again. The power is looked at every 120 frames, and at once when the menu
  changes the setting; the change takes effect at the next frame. Overlord II
  on the Steam Deck in the same place: the APU at 6.2 W with FSR 4, 5.9 with FSR 3.1, 4.7 with the
  MSAA (the GPU busy 63, 56 and 33 % of the time).
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
