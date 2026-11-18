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
| Minecraft: Xbox 360 Edition (TU 80) | plays: menus, the update's world templates, a new world | 60 FPS |
| Castlevania: Symphony of the Night (TU 2) | plays: the menu with its text, the name entry, the prologue | 60 FPS |
| Orcs Must Die! (TU 1) | plays: title, menus, the campaign's story and its first level with the HUD, 30 FPS (its build inlined many draws past the hooks: `rexnative.inline_draws`, which turns itself on) | 30 FPS |
| Double Dragon Neon | plays its first mission | 60 FPS |
| Brothers - A Tale of Two Sons | menu and opening, drawn as the stock renderer draws them | 30 FPS (ReXGlue's renderer) |
| Bionic Commando Rearmed (TU 2) | the intro at 60 FPS, the mission map | 60 FPS |
| Midnight Club: Los Angeles - Complete Edition | plays: the city, its cars (from recorded command buffers), 30 FPS | 30 FPS |
| Dark Souls II (TU 10) | plays: menus, a new game into Things Betwixt, 60 FPS (the console's 30 with `rexnative.menu_fps = 30`); the prologue video is garbled (also on ReXGlue's renderer: its WMV decoding) | 30 FPS (Things Betwixt) |
| Halo: Combat Evolved Anniversary (TU 2) | campaign menu and the opening cutscene; some surfaces dark or striped | 60 FPS (the intro) |
| Spelunky (TU 1) | menus, with no text (also on ReXGlue's renderer, and in Xenia: [game-compatibility#97](https://github.com/xenia-project/game-compatibility/issues/97)) | 60 FPS, no text |
| Perfect Dark (TU 3) | reaches its levels, but draws them broken (ReXGlue's renderer now reaches the menus, to compare) | 60 FPS (menus) |
| 3D Ultra Minigolf Adventures | runs past its start-up | 60 FPS |
| Project Gotham Racing 4 | the renderer's origin, with its own copy ([pgr4-native](https://github.com/shaunsingh/pgr4-native)) | |

## How these were tested

- **TU:** the title update installed (always the newest: `prepare --title-update`).
- Every result is from ReXGlue v0.11 builds of 2026-11-16 and 17.
- The Steam Deck results are from `python -m rexnative deck <work>/<game>... --steam --try 70`:
  the frame rate of the screen a game reaches by itself in 70 seconds, under GE-Proton (see
  [STEAM_DECK.md](STEAM_DECK.md)).

## Open problems

Perfect Dark's broken levels, Spelunky's missing text, Halo's dark and striped surfaces, and the
WMV videos of Dark Souls II and Jet Set Radio. The plans for them are in [ROADMAP.md](ROADMAP.md).

## Reporting a game

Tried a game that is not here, or got a different result? Open an issue with the **Game report**
form: the game, its title update, PC or Steam Deck, how far it got, and the log. Do not attach
game files, generated code or the game's layer (`results/`): see
[CONTRIBUTING.md](../CONTRIBUTING.md).
