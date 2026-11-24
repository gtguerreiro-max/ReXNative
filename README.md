<div align="center">

# ReXNative

**A native Direct3D 12 renderer for Xbox 360 games recompiled with the ReXGlue SDK, with DLAA,
DLSS, DLSS 5 and FSR 4, and a tool that fits it to a new game by itself.**

![Direct3D 12](https://img.shields.io/badge/Direct3D-12-0078D6?logo=windows&logoColor=white)
![DLSS](https://img.shields.io/badge/NVIDIA-DLAA%20%7C%20DLSS%20%7C%20DLSS%205-76B900?logo=nvidia&logoColor=white)
![FSR](https://img.shields.io/badge/AMD-FSR%204%20%7C%203.1-ED1C24?logo=amd&logoColor=white)
![Steam Deck](https://img.shields.io/badge/Steam%20Deck-Proton-1A9FFF?logo=steamdeck&logoColor=white)
![ReXGlue](https://img.shields.io/badge/ReXGlue-v0.11.0-555555)
![Game files](https://img.shields.io/badge/game%20files-not%20included-lightgrey)

</div>

A [ReXGlue](https://github.com/rexglue/rexglue-sdk) port runs a game's own PowerPC code
recompiled to x64 and draws it through an emulated Xbox 360 GPU. **ReXNative draws the game with
Direct3D 12 instead**: it hooks the game's calls into the XDK's Direct3D library and draws each
one natively, which leaves room for modern upscalers, MSAA, HDR and a settings menu over the game.

One renderer serves every game. What differs between games, a few dozen addresses and structure
offsets, is found in the game's own recompiled code by a tool, which can also take a game as it
comes (an archive, a disc image, an Xbox Live package) and leave a built executable that runs.

> [!IMPORTANT]
> No game files, generated code or game data are in this repository. You need your own copy of
> each game.

## Features

- **Upscaling and anti-aliasing:** NVIDIA DLAA, DLSS Super Resolution and DLSS 5 neural rendering
  on RTX GPUs; AMD FSR 4 and 3.1 on the others, the Steam Deck included.
- **Frame generation** (AMD FSR): a 30 FPS game shown at 60, switched on and off in the running
  game, with motion vectors from every draw and the game's HUD kept out of the generated frames.
- **Image:** MSAA up to 8x, anisotropic filtering, FSR 1 screen scaling, HDR10 output, the
  console's gamma ramps, frame pacing on the display.
- **A settings HUD over the running game** (F1, or Back + Start on a pad), saved into the game's
  configuration file.
- **Less shader compile stutter:** pipelines from earlier sessions are compiled again in the
  background at start-up.
- **The stock renderer one setting away:** the HUD's Renderer switches to ReXGlue's Xenos GPU
  emulation, Xenia's renderer, and back while the game runs, on the Steam Deck too, and keeps the
  choice for the next start (F9 switches for that run only): a game the native renderer does not
  draw right yet stays playable. A fault in the native path hands the game to it by itself.
- **Games as they come:** archives, disc images and Xbox Live packages; the newest title update
  and the game's DLC (`prepare --title-update --dlc`); a game's XEX DLL modules recompiled with
  it; a D3D older than every reference drawn from its push buffer alone.
- **FPS Boost:** Xenia canary's community patch that raises a game's frame rate ("60 FPS",
  "Unlock FPS"), for the exact version of its executable, put into the recompiled code when
  the game asks for it (`rexnative fps-boost <work> on`, then a build).
- **Windows and Steam Deck** (under Proton, through vkd3d-proton).

## Games

Thirty-seven games tried on ReXGlue v0.11, most with a Steam Deck result. Twenty-one play on the
PC: From Dust, Iron Brigade, Jet Set Radio, Dungeons & Dragons: Chronicles of Mystara, Super Meat
Boy, Minecraft, Castlevania: Symphony of the Night, Orcs Must Die!, Double Dragon Neon, Midnight
Club: Los Angeles, Dark Souls II, Overlord, Overlord II, SSX, Forza Horizon (whose speech and media
code are XEX DLL modules, recompiled with it), Halo: Combat Evolved Anniversary, Asura's Wrath,
Sega Rally Revo, Deus Ex: Human Revolution, Geometry Wars: Retro Evolved and Tetris Splash.
Fable II draws its world on the native renderer; most of the rest run in part.

**[The full table, with title updates, Steam Deck frame rates and known problems →](docs/GAMES.md)**

## Quick start

You need Windows 10 or 11, Python 3.10+, Visual Studio 2022 (C++), LLVM 20, CMake, Ninja, the
Vulkan SDK, NVIDIA's DLSS SDK, and one *reference* game the tool already knows
([full list](docs/GETTING_STARTED.md#requirements)).

```powershell
# 1. Build the pinned ReXGlue SDK with the renderer's patches, and the AMD FidelityFX SDK
scripts\build-sdk.ps1 -Llvm C:\path\to\LLVM-20\bin

# 2. Copy ports.example.json to ports.json, set your reference games' paths and the
#    REXNATIVE_* variables (docs/GETTING_STARTED.md)

# 3. Prepare a game: extract, title update, codegen, layer, project, build
python -m rexnative prepare "D:\Games\My Game.zip" --work D:\rexnative-work --title-update --dlc --project
```

The game is then in `D:\rexnative-work\<game>\run\`; F1 opens its settings.

**On a Steam Deck:** the **ReXNative Manager**, a Windows app (first release soon), will do all of
the above and the install for you, from your archive to the Deck's Steam library, with a mode
for the Deck itself ([docs/STEAM_DECK.md](docs/STEAM_DECK.md#the-rexnative-manager)). Until
then: `python -m rexnative deck D:\rexnative-work\<game> --host deck@<ip> --key <ssh key> --steam`
([docs/STEAM_DECK.md](docs/STEAM_DECK.md)).

## Documentation

| | |
|---|---|
| [Games](docs/GAMES.md) | what each game does, on the PC and on the Steam Deck |
| [Getting started](docs/GETTING_STARTED.md) | requirements, building, the tool's commands, adding it to a ReXGlue project |
| [Keys and settings](docs/SETTINGS.md) | the HUD, `rexnative.*` settings, inline draws |
| [Steam Deck](docs/STEAM_DECK.md) | the ReXNative Manager, installing games on the Deck, its settings, Steam artwork |
| [Debugging a game](docs/DEBUGGING.md) | a live game for experiments, RenderDoc, the Deck |
| [How it works](docs/ARCHITECTURE.md) | the hooks, how a game is matched, the repository layout |
| [The game layer](docs/GAME_LAYER.md) | what `game_layer.h` and `overrides.json` hold |
| [Engineering notes](docs/NOTES.md) | what each game needed, and how it was found |
| [The SDK](sdk/README.md) | the pinned ReXGlue SDK and its patches |
| [Roadmap](docs/ROADMAP.md) | what comes next, and what is done |

## Contributing

Game reports, fixes and new references are welcome: see [CONTRIBUTING.md](CONTRIBUTING.md). The
one hard rule is that nothing from a game (its files, generated code, layer or data) goes into
the repository. Security problems go through [SECURITY.md](SECURITY.md), not public issues.

## License and credits

MIT ([LICENSE](LICENSE)). The renderer is [pgr4-native](https://github.com/shaunsingh/pgr4-native)'s
(MIT, by [shaunsingh](https://github.com/shaunsingh), on
[safzanpirani](https://github.com/safzanpirani)'s work), made universal here, with code ported from
the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) and [Xenia](https://github.com/xenia-project/xenia)
(BSD-3-Clause); see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). NVIDIA DLSS and AMD
FidelityFX come from their SDKs, under their own licenses, and are not in this repository.

Xbox 360 is a trademark of Microsoft; this project is not affiliated with Microsoft, NVIDIA, AMD
or the games' publishers.
