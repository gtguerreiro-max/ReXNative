# Games

What each game does with ReXNative, on a Windows PC and on the Steam Deck. What each one needed,
and how it was found, is in [NOTES.md](NOTES.md).

| Game | On the PC | Steam Deck |
|---|---|---|
| From Dust | plays: menus, levels, HUD, 60 FPS | 60 FPS ([FromDustDeck](https://github.com/gtguerreiro-max/FromDustDeck)) |
| Iron Brigade | plays: the intro and its video, the first mission with its scenery | its own 30 FPS ([IronBrigadeRecomp](https://github.com/gtguerreiro-max/IronBrigadeRecomp)) |
| Jet Set Radio (TU 1) | plays: menus, the story intro, the first level | 30 FPS |
| Dungeons & Dragons: Chronicles of Mystara | plays: menus, chapter select, the first chapter, 60 FPS | 60 FPS |
| Super Meat Boy (TU 1) | plays: menus, the world map, level 1-1 | 60 FPS |
| Minecraft: Xbox 360 Edition (TU 80) | plays: menus, the update's world templates, a new world, and a saved one (its Load list waited for an asynchronous read until SDK patch 0036); with the upscaler, the HUD stays out of the upscaled scene (2026-11-19). The saves' thumbnails are black (it reads a resolve on the CPU at once: [NOTES](NOTES.md#renders-the-cpu-reads)) | 60 FPS (a saved world, FSR 4) |
| Castlevania: Symphony of the Night (TU 2) | plays: the menu with its text, the name entry, the prologue | 60 FPS |
| Orcs Must Die! (TU 1) | plays: title, menus, the campaign's story and its first level with the HUD, 30 FPS (its build inlined many draws past the hooks: `rexnative.inline_draws`, which turns itself on) | 30 FPS |
| Double Dragon Neon | plays its first mission | 60 FPS |
| Brothers - A Tale of Two Sons | menu and opening, drawn as the stock renderer draws them | 30 FPS (ReXGlue's renderer) |
| Bionic Commando Rearmed (TU 2) | the intro at 60 FPS, the mission map | 60 FPS |
| Midnight Club: Los Angeles - Complete Edition | plays: the city, its cars (from recorded command buffers), 30 FPS | 30 FPS |
| Dark Souls II (TU 10) | plays: menus, a new game into Things Betwixt, 60 FPS (the console's 30 with `rexnative.menu_fps = 30`); its prologue video came out garbled before the video decoder's fix of 2026-11-18 (not checked here since) | 30 FPS (Things Betwixt) |
| Halo: Combat Evolved Anniversary (TU 2) | plays: the menu (its ship and ring whole since SDK patch 0038), the opening cutscene textured throughout (its texture pool's GPU copies done in guest memory), the first level's cryo bay and HUD, the classic graphics (Back) and the pause menu, 30 FPS (the game's cap) | 60 FPS (the intro) |
| Spelunky (TU 1) | menus, with no text (also on ReXGlue's renderer, and in Xenia: [game-compatibility#97](https://github.com/xenia-project/game-compatibility/issues/97)) | 60 FPS, no text |
| Perfect Dark (TU 3) | reaches its levels, but draws them broken (ReXGlue's renderer now reaches the menus, to compare) | 60 FPS (menus) |
| 3D Ultra Minigolf Adventures | runs past its start-up | 60 FPS |
| Fable II | title, menus, the intro and Bowerstone with its buildings and people, on the native renderer (its default since 2026-11-18) | 30 FPS (title screen; gameplay not tried yet) |
| Overlord | plays its first level | 30 FPS (first level) |
| Overlord II | plays its first level (a tiled G-buffer) | 30 FPS (first level) |
| Sega Rally Revo | runs, drawn from its replayed command buffers: the title (its chrome logo since 2026-11-18) and the attract races | 30 FPS |
| Diablo III | its intro cinematics, the main menu, hero selection and creation (with the upscaler: `rexnative.upscaler_depth_test`) | 60 FPS (main menu) |
| Syndicate | its intro video plays | 30 FPS (intro) |
| Deus Ex: Human Revolution - Director's Cut | title and menus as the stock renderer draws them (its GPU-owned shader constants, read since 2026-11-18); a new game asks for disc 1 | 30 FPS (title) |
| Asura's Wrath (TU 1) | runs to its title screen | not tried (space) |
| Blur (TU 2) | runs, its intro video plays | not tried (space) |
| Forza Horizon (TU 4, its DLC) | plays on the native renderer: the intro videos, the main menu, the opening drive with its car, the traffic and the HUD (about 2000 draws, 30 FPS); its speech and media code are two XEX DLL modules, recompiled as ReXGlue's `[[modules]]` since 2026-11-19 (their title update patches needed SDK patch 0037) | not tried (space) |
| SSX (TU 3, its DLC) | plays on the native renderer: the intro video, title, menus, the World Tour's first descent, 30 FPS (it drew black while its layer gave the Resolve hook a reference row's category, 2026-11-19) | not tried (space) |
| Darwinia+ | its intros, the menu and the first level on the native renderer (since 2026-11-19: its frame's Clear, recorded in a replayed command buffer, and its BeginVertices' built-in stride); in the level the water's reflection, the terrain's grid and the cursor still differ from ReXGlue's renderer | not tried |
| LIMBO | title and the first forest on the native renderer (its grass since 2026-11-19: a resolve's depth clear on another pitch) | not measured |
| Top Spin 4 | not supported yet: `default.xex` is a loader, and both its XEX DLL modules (`Loader_DLL`, `Swing_DLL`) link their own D3D library, which the renderer's hooks (in the executable) cannot reach | |
| Project Gotham Racing 4 | the renderer's origin, with its own copy ([pgr4-native](https://github.com/shaunsingh/pgr4-native)) | |

## How these were tested

- **TU:** the title update installed (always the newest: `prepare --title-update`).
- Every result is from ReXGlue v0.11 builds of 2026-11-16 to 19.
- The Steam Deck results are from `python -m rexnative deck <work>/<game>... --steam --try 70`:
  the frame rate of the screen a game reaches by itself in 70 seconds, under GE-Proton (see
  [STEAM_DECK.md](STEAM_DECK.md)).

## Open problems

Perfect Dark's broken levels, Spelunky's missing text, and Deus
Ex's "insert disc 1" when a new game starts (from disc 1's own image), and a render the game
reads back on the CPU (Minecraft's save thumbnails: the native renderer never writes a resolve
into guest memory). The plans for them are in [ROADMAP.md](ROADMAP.md).
The garbled WMV videos (Jet Set Radio, Syndicate, Dark Souls II) were the video decoder's
registers, fixed on 2026-11-18 ([NOTES.md](NOTES.md#registers-a-function-takes-from-its-caller)).

## Reporting a game

Tried a game that is not here, or got a different result? Open an issue with the **Game report**
form: the game, its title update, PC or Steam Deck, how far it got, and the log. Do not attach
game files, generated code or the game's layer (`results/`): see
[CONTRIBUTING.md](../CONTRIBUTING.md).
