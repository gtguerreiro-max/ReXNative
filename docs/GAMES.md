# Games

What each game does with ReXNative, on a Windows PC and on the Steam Deck. What each one needed,
and how it was found, is in [NOTES.md](NOTES.md).

**Frame generation** (AMD FSR, the HUD's Upscaler page) was tried on 2026-11-20 in Sega Rally
Revo's races, Halo's opening cutscene, Dark Souls II, Overlord, Minecraft, Orcs Must Die!, Midnight
Club, Jet Set Radio and Perfect Dark: in all of them a generated frame between every two of the
game's, none a copy of the real one outside camera cuts, with the HUD kept sharp (Jet Set Radio
draws its HUD into its scene, so there it is generated with the rest). Brothers and The Bureau find
no 3D scene yet, so neither the upscaler nor frame generation runs there (Open problems, below).

| Game | On the PC | Steam Deck |
|---|---|---|
| From Dust | plays: menus, levels, HUD, 60 FPS | 60 FPS ([FromDustDeck](https://github.com/gtguerreiro-max/FromDustDeck)) |
| Iron Brigade | plays: the intro and its video, the first mission with its scenery | its own 30 FPS ([IronBrigadeRecomp](https://github.com/gtguerreiro-max/IronBrigadeRecomp)) |
| Jet Set Radio (TU 1) | plays: menus, the story intro, the first level; frame generation in its attract mode (2026-11-20) | 30 FPS |
| Dungeons & Dragons: Chronicles of Mystara | plays: menus, chapter select, the first chapter, 60 FPS | 60 FPS |
| Super Meat Boy (TU 1) | plays: menus, the world map, level 1-1 | 60 FPS |
| Minecraft: Xbox 360 Edition (TU 80) | plays: menus, the update's world templates, a new world, and a saved one (its Load list waited for an asynchronous read until SDK patch 0036); with the upscaler, the HUD stays out of the upscaled scene (2026-11-19). The saves' thumbnails are black (it reads a resolve on the CPU at once: [NOTES](NOTES.md#renders-the-cpu-reads)). With frame generation its hotbar and prompts stay sharp since 2026-11-20 | 60 FPS (a saved world, FSR 4) |
| Castlevania: Symphony of the Night (TU 2) | plays: the menu with its text, the name entry, the prologue | 60 FPS |
| Orcs Must Die! (TU 1) | plays: title, menus, the campaign's story and its first level with the HUD, 30 FPS (its build inlined many draws past the hooks: `rexnative.inline_draws`, which turns itself on); with frame generation its HUD stays sharp since 2026-11-20 | 30 FPS |
| Double Dragon Neon | plays its first mission | 60 FPS |
| Brothers - A Tale of Two Sons | menu and opening, drawn as the stock renderer draws them; its 3D scene is not found (it draws its lighting, scene and post-processing on one surface), so no upscaler or frame generation yet | 30 FPS (ReXGlue's renderer) |
| Bionic Commando Rearmed (TU 2) | the intro at 60 FPS, the mission map | 60 FPS |
| Midnight Club: Los Angeles - Complete Edition | plays: the city, its cars (from recorded command buffers), 30 FPS; frame generation with a sharp HUD while driving (2026-11-20) | 30 FPS |
| Dark Souls II (TU 10) | plays: menus, a new game into Things Betwixt, 60 FPS (the console's 30 with `rexnative.menu_fps = 30`); its prologue video came out garbled before the video decoder's fix of 2026-11-18 (not checked here since); with frame generation its HUD stays sharp since 2026-11-20 | 30 FPS (Things Betwixt) |
| Halo: Combat Evolved Anniversary (TU 2) | plays: the menu (its ship and ring whole since SDK patch 0038), the opening cutscene textured throughout (its texture pool's GPU copies done in guest memory), the first level's cryo bay and HUD, the classic graphics (Back) and the pause menu, 30 FPS (the game's cap); frame generation in the opening cutscene (2026-11-20) | 60 FPS (the intro) |
| Spelunky (TU 1) | menus, with no text (also on ReXGlue's renderer, and in Xenia: [game-compatibility#97](https://github.com/xenia-project/game-compatibility/issues/97)) | 60 FPS, no text |
| Perfect Dark (TU 3) | menus and the Carrington Institute drawn right, frame generation there (2026-11-20); its mission levels drew broken when last tried (ReXGlue's renderer reaches the menus, to compare) | 60 FPS (menus) |
| 3D Ultra Minigolf Adventures | runs past its start-up | 60 FPS |
| Fable II | title, menus, the intro and Bowerstone with its buildings and people, on the native renderer (its default since 2026-11-18) | 30 FPS (title screen; gameplay not tried yet) |
| Overlord | plays its first level; with frame generation its minion counter stays sharp since 2026-11-20 | 30 FPS (first level) |
| Overlord II | plays its first level (a tiled G-buffer) | 30 FPS (first level) |
| Sega Rally Revo | plays, drawn from its replayed command buffers: the title (its chrome logo since 2026-11-18), menus and races, 30 FPS. The frame generation test game: its optical flow, motion vectors and ground tiles fixed on 2026-11-20 ([NOTES](NOTES.md#frame-generations-optical-flow)); dust behind the car still breaks into blocks and foliage shimmers | 30 FPS |
| Diablo III | its intro cinematics, the main menu, hero selection and creation (with the upscaler: `rexnative.upscaler_depth_test`) | 60 FPS (main menu) |
| Syndicate | its intro video, menus and first mission drawn as on ReXGlue's renderer (its colour grading LUT, resolved into a 3D texture, since 2026-11-19) | 30 FPS (intro) |
| Deus Ex: Human Revolution - Director's Cut | title and menus as the stock renderer draws them (its GPU-owned shader constants, read since 2026-11-18); a new campaign starts on disc 1 and plays past its intro (SDK patch 0039) | 30 FPS (title) |
| Geometry Wars: Retro Evolved | plays on the native renderer from its push buffer alone (its D3D 2.0.2135 is older than every reference: [NOTES](NOTES.md#a-d3d-older-than-every-reference)): menus and gameplay, 60 FPS; the upscaler does not start yet | installed; its Deck settings tested on the PC (frame rate not measured) |
| Asura's Wrath (TU 1) | plays: menus, the opening cutscene and the first episode's space battle; its save prompt answers to the pad (SDK patch 0042); with the upscaler its hero was see-through until 2026-11-19 ([NOTES](NOTES.md#the-scenes-depth-after-the-upscaled-resolve)); its texture pool's GPU copies done in guest memory since 2026-11-21 (the Deck's episode menu showed wrong text) | 30 FPS (first episode) |
| Blur (TU 2) | intro video, menus and a race's start; in the race the native renderer draws the HUD and the lights but the scenery black (ReXGlue's renderer draws it washed out), 2026-11-20 | not tried (space) |
| Forza Horizon (TU 4, its DLC) | plays on the native renderer: the intro videos, the main menu, the opening drive with its car, the traffic and the HUD (about 2000 draws, 30 FPS); its speech and media code are two XEX DLL modules, recompiled as ReXGlue's `[[modules]]` since 2026-11-19 (their title update patches needed SDK patch 0037) | not tried (space) |
| SSX (TU 3, its DLC) | plays on the native renderer: the intro video, title, menus, the World Tour's first descent, 30 FPS (it drew black while its layer gave the Resolve hook a reference row's category, 2026-11-19) | not tried (space) |
| Darwinia+ | its intros, the menu and the first level on the native renderer (since 2026-11-19: its frame's Clear, recorded in a replayed command buffer, and its BeginVertices' built-in stride); in the level the water's reflection, the terrain's grid and the cursor still differ from ReXGlue's renderer | not tried |
| LIMBO | title and the first forest on the native renderer (its grass since 2026-11-19: a resolve's depth clear on another pitch) | not measured |
| The Bureau: XCOM Declassified | title screen, its intro and the first mission's motel room on the native renderer (the outside and the fog showed through its walls until 2026-11-21: [NOTES](NOTES.md#a-clear-inside-predicated-tiling)); the lamps' hard shadows on the walls are still missing there; like Brothers (Unreal Engine 3) no 3D scene is found yet, so no upscaler or frame generation | installed (frame rate not measured) |
| Castle Crashers | menus and the name entry, drawn as ReXGlue's renderer draws them | installed; its Deck settings tested on the PC |
| Tetris Splash | menus and Marathon on the native renderer (with the upscaler, its frame drawn in two predicated tiles was cut at the middle until 2026-11-19) | installed; its Deck settings tested on the PC |
| Crazy Taxi | runs past its start-up; no 3D scene found yet (picture not checked) | not tried |
| Dust: An Elysian Tail | prepared, does not boot yet | |
| Top Spin 4 | not supported yet: `default.xex` is a loader, and both its XEX DLL modules (`Loader_DLL`, `Swing_DLL`) link their own D3D library, which the renderer's hooks (in the executable) cannot reach | |
| Project Gotham Racing 4 | the renderer's origin, with its own copy ([pgr4-native](https://github.com/shaunsingh/pgr4-native)) | |

## How these were tested

- **TU:** the title update installed (always the newest: `prepare --title-update`).
- Every result is from ReXGlue v0.11 builds of 2026-11-16 to 20.
- The Steam Deck results are from `python -m rexnative deck <work>/<game>... --steam --try 70`:
  the frame rate of the screen a game reaches by itself in 70 seconds, under GE-Proton (see
  [STEAM_DECK.md](STEAM_DECK.md)). "Installed" is a game the ReXNative Manager put on the Deck
  after its boot test and a test of its Deck settings on the PC (the Deck's resolution and FSR),
  whose frame rate on the Deck was not measured yet.

## Open problems

Perfect Dark's broken levels, Spelunky's missing text, and a render the game
reads back on the CPU (Minecraft's save thumbnails: the native renderer never writes a resolve
into guest memory). Since 2026-11-20 also: Blur's races, their scenery black on the native
renderer; Brothers and The Bureau, Unreal Engine 3 games whose one surface holds their lighting,
scene and post-processing, so the scene detection finds none (a detection that found Brothers'
scene made its colours wrong with the upscaler on, so it was left out); and in frame
generation, translucent dust and particles, which no draw gives motion vectors, break into the
optical flow's 8-pixel blocks. The plans for them are in [ROADMAP.md](ROADMAP.md).
The garbled WMV videos (Jet Set Radio, Syndicate, Dark Souls II) were the video decoder's
registers, fixed on 2026-11-18 ([NOTES.md](NOTES.md#registers-a-function-takes-from-its-caller)).

## Reporting a game

Tried a game that is not here, or got a different result? Open an issue with the **Game report**
form: the game, its title update, PC or Steam Deck, how far it got, and the log. Do not attach
game files, generated code or the game's layer (`results/`): see
[CONTRIBUTING.md](../CONTRIBUTING.md).
