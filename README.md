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

[Português](LEIA-ME.md)

</div>

A [ReXGlue](https://github.com/rexglue/rexglue-sdk) port runs a game's own PowerPC code
recompiled to x64, and draws it through an emulation of the Xbox 360 GPU (Xenos: command
buffers, EDRAM, resolves). **ReXNative draws the game with Direct3D 12 instead.** It hooks the
game's calls into the XDK's Direct3D library (draws, clears, resolves, Swap), reads the state the
game's D3D device keeps, and draws each call natively. That leaves room for what the console
never had: modern upscalers, MSAA, HDR, a settings menu over the game.

One renderer serves every game. What differs between games, a few dozen addresses and structure
offsets, is found in the game's own recompiled code by a tool, which can also take a game as it
comes (an archive, a disc image, an Xbox Live package) and leave a built executable that runs.

> No game files, generated code or game data are in this repository. You need your own copy of
> each game.

- [Features](#features) · [Games](#games) · [Quick start](#quick-start) ·
  [How it works](#how-it-works) · [Adding it to a ReXGlue project](#adding-it-to-a-rexglue-project) ·
  [Keys and settings](#keys-and-settings) · [Steam Deck](#steam-deck) · [Debugging a game](#debugging-a-game) ·
  [Layout](#repository-layout) · [Roadmap](#roadmap) · [License](#license-and-credits)

## Features

- **Upscaling and anti-aliasing on the 3D scene:** NVIDIA DLAA, DLSS Super Resolution and DLSS 5
  neural rendering on RTX GPUs; AMD FSR 4 and 3.1 on the others, the Steam Deck included; FSR
  frame generation.
- **Image:** MSAA up to 8x, anisotropic filtering, FSR 1 screen scaling, HDR10 output, the
  console's gamma ramps, frame pacing on the display.
- **A settings HUD over the running game** (F1, or Back + Start on a pad), saved into the game's
  configuration file. Its Upscaler page switches the upscaler, its type (automatic, DLSS or FSR),
  the FSR version and the mode (native anti-aliasing to ultra performance) while the game runs,
  and says what runs ("AMD FSR 4.1.2 Quality, 853x480 to 1280x720").
- **Less shader compile stutter:** the pipelines a game created in earlier sessions are compiled
  again in the background at start-up.
- **The stock renderer one key away:** F9 switches between ReXNative and ReXGlue's Xenos GPU
  emulation while the game runs, to compare them.
- **Windows and Steam Deck** (under Proton, through vkd3d-proton).

## Games

| Game | On the PC | Steam Deck |
|---|---|---|
| From Dust | plays: menus, levels, HUD, 60 FPS | 60 FPS ([FromDustDeck](https://github.com/gtguerreiro-max/FromDustDeck)) |
| Iron Brigade | plays: the intro and its video, the first mission with its scenery | its own 30 FPS ([IronBrigadeRecomp](https://github.com/gtguerreiro-max/IronBrigadeRecomp)) |
| Jet Set Radio (TU 1) | plays: menus, the story intro, the first level | 30 FPS |
| Dungeons & Dragons: Chronicles of Mystara | plays: menus, chapter select, the first chapter, 60 FPS | 60 FPS |
| Super Meat Boy (TU 1) | plays: menus, the world map, level 1-1 | 60 FPS |
| Minecraft: Xbox 360 Edition (TU 80) | plays: menus, the update's world templates, a new world | 60 FPS |
| Castlevania: Symphony of the Night (TU 2) | plays: the menu with its text, the name entry, the prologue | 60 FPS |
| Orcs Must Die! (TU 1) | plays: title, menus, the campaign's story and its first level with the HUD, 30 FPS, with `rexnative.inline_draws = true` (its build inlined many draws past the hooks) | 30 FPS |
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

TU: the title update installed (always the newest). Every result is from ReXGlue v0.11 builds of
2026-11-16 and 17; the Steam Deck ones from `python -m rexnative deck <work>/<game>... --steam
--try 70` (the frame rate of the screen a game reaches by itself in 70 seconds, under GE-Proton;
see [Steam Deck](#steam-deck)). What each game needed is in [docs/NOTES.md](docs/NOTES.md).

## Quick start

**Requirements:** Windows 10 or 11; Python 3.10+ (the tool has no dependencies; the debugging
scripts `snaps.py` and `resolves.py` need Pillow and numpy); Visual Studio
2022 or its Build Tools with the C++ workload; LLVM 20 (clang++ and lld: ReXGlue v0.11 needs
clang 20 or later); CMake, Ninja and git; the Vulkan SDK (ReXGlue builds its Vulkan driver too);
the NVIDIA DLSS SDK and DLSS 5 headers (from NVIDIA). To fit the renderer to a new game, the tool
also needs one game it already knows (a *reference*), with its ReXGlue generated code and its
`default.xex`: From Dust, Iron Brigade, Project Gotham Racing 4, or any game it fitted before.

**1. Build the SDK.** ReXGlue v0.11.0, pinned, with the patches the renderer needs
([sdk/README.md](sdk/README.md)), and the AMD FidelityFX SDK v2.4.0:

```powershell
scripts\build-sdk.ps1 -Llvm C:\path\to\LLVM-20\bin
```

**2. Tell the tool where things are.** Copy `ports.example.json` to `ports.json` and set the
paths of your reference games. Then:

```powershell
$env:REXNATIVE_LLVM        = "C:\path\to\LLVM-20\bin"
$env:REXNATIVE_REXGLUE     = "$PWD\build\sdk-install\bin\rexglue.exe"
$env:REXNATIVE_SDK_INSTALL = "$PWD\build\sdk-install"
$env:REXNATIVE_SDK_SRC     = "$PWD\build\rexglue-sdk"
$env:REXNATIVE_FIDELITYFX  = "$PWD\build\fidelityfx-sdk"
$env:REXNATIVE_DLSS_SDK    = "C:\path\to\DLSS"
$env:REXNATIVE_DLSS_NR     = "C:\path\to\dlss_nr"
```

**3. Prepare the game.** From an archive, a folder, an Xbox Live (STFS) package or a disc image:

```powershell
python -m rexnative prepare "D:\Games\My Game.zip" --work D:\rexnative-work --title-update --project
```

This extracts the game, downloads and installs its newest title update (listed by XboxUnity,
checked against the listing), runs ReXGlue's codegen until it is clean, generates the game's
layer (`layer/report.md` says what was found, how and how sure), writes a ReXGlue project with
ReXNative and the settings HUD, builds it, finds the functions codegen missed, and builds again.
The game is then in `D:\rexnative-work\<game>\run\`: run its executable. Reports are in English;
`--lang pt` (or `REXNATIVE_LANG=pt`) writes them in Portuguese.

Other commands: `info <xex>` (title, version, XDK libraries), `title-update <work>` (the title
updates listed, nothing downloaded), `project <work>/<game> --build` (rebuild one game), `survey
--work <work>` (a table of the prepared games), `deck <work>/<game>...` (built games onto a Steam
Deck: [Steam Deck](#steam-deck)), `adapt` (only the layer, for a ReXGlue project of your own),
`scene` (the scene surface from a resolve dump), `validate <port>` (regenerate a known port and
compare), `extract`, `neutral`. `python -m rexnative <command> --help` gives each one's options.

## How it works

What changes from game to game is small:

| What | How ReXNative finds it |
|---|---|
| The addresses of the XDK D3D functions it hooks (Swap, Draw*, Clear, Resolve, push-buffer segments, shader load, about 100 more for tracing) | matches the game's ReXGlue-generated code against a reference's |
| The offsets of the fields of the game's D3D device | votes from the aligned instructions of the matched functions |
| Which Resolve function to hook and where its arguments are (LTCG builds inline the wrapper and drop arguments) | follows each argument's uses in both listings |
| Resolve's default clear colour | its code pattern: one float (XDK 6132) or a 16-byte vector (XDK 20353) |
| The pad the HUD reads | the game's XInputGetState (the XDK's three-instruction wrapper, found by its shape) |
| The scene surface the upscaler works on | learned at run time from the game's resolves; measured once (`rexnative scene`) for a scene with neither MSAA nor an HDR format |
| Engine behaviour (a second D3D device, setting defaults) | your choice, in `overrides.json` |

The tool writes all of it into a header, `game_layer.h`, and a list of hooks, `d3d_surface.inc`
([docs/GAME_LAYER.md](docs/GAME_LAYER.md)). The renderer reads nothing else about the game.

The matching works on the C++ that ReXGlue generates (every PowerPC instruction is kept there as
a comment), so it needs no disassembler. For each function of a reference it tries, in order of
confidence: the same listing; the same with every number masked; the only such one inside the
D3D library; the function a matched one calls at the same place (the XREF idea of Cxbx-Reloaded's
XbSymbolDatabase); the neighbour of a matched one (the library keeps its object files' order);
the one that calls the same matched functions; a similar listing; and a public wrapper the
compiler inlined away. The reference is chosen from the game's `default.xex`: the same D3D
library version first, then LTCG or not, then the closest XDK build; the other references fill
gaps and cross-check. Regenerating From Dust and Iron Brigade without their own layers gave the
same hooks and device offsets as the hand-made ones, and found mistakes in them
([validation](docs/NOTES.md#validating-the-adapter)).

A recompiled game also needs things ReXGlue's codegen does not find alone, which `prepare` and
`project` supply as codegen hints: function starts reached only through pointers, jump tables
read short, the addresses of setjmp and longjmp, XAPI fibers, missing imports
([docs/NOTES.md](docs/NOTES.md)).

## Adding it to a ReXGlue project

For a game you already have as a ReXGlue project, generate its layer:

```bash
python -m rexnative adapt --generated path/to/game/generated/default --xex path/to/game/default.xex --name mygame --title "My Game" --out results/mygame
```

add the renderer to the build:

```cmake
include(path/to/ReXNative/renderer/rexnative.cmake)
rexnative_add(mygame_common          # the host library (rexglue_setup_host_library)
    EXECUTABLE      mygame
    LAYER_DIR       ${CMAKE_CURRENT_SOURCE_DIR}/results/mygame
    REXGLUE_SDK_SRC path/to/rexglue-sdk
    FIDELITYFX_SDK  path/to/FidelityFX-SDK
    DLSS_SDK        path/to/DLSS
    DLSS_NR         path/to/dlss_nr)
```

and call it from the game's app (a `rex::app::TitleApp`): `rexnative::host::Configure()` in
`OnConfigure`; `rexnative::host::Attach(*this)` and `rexnative::hud::Initialize()` in `OnInit`;
`rexnative::hud::Shutdown()` and `rexnative::host::Detach()` in `OnShutdown`
(`renderer/src/host.h`, `renderer/src/hud/hud.h`). `rexnative project` writes such an app.

The upscalers find the game's 3D scene by themselves: the renderer learns it from the game's
resolves (the log says `ReXNative scene: learned at frame ...`;
[docs/GAME_LAYER.md](docs/GAME_LAYER.md#the-scene-at-run-time)). A scene with neither MSAA nor an
HDR format is not found that way: measure it once by running with
`--rexnative_resolve_dump=<dir>`, creating `<dir>/dump.request` during a 3D scene and running
`python -m rexnative scene <dir>/f<N>-resolves.jsonl`; put its result in
`results/mygame/overrides.json` and run `adapt` again.

## Keys and settings

| Key | Pad | |
|---|---|---|
| F1 | Back + Start | the settings HUD |
| F9 | | switch between ReXNative and ReXGlue's Xenos GPU emulation (not under Proton) |
| F7 | | the upscaler on or off |
| F8 | | show the upscaler's inputs |

The keys are settings too (`bind_rexnative_hud`, `bind_renderer_switch`, `bind_upscaler`,
`bind_upscaler_debug`). Every renderer option is a `rexnative.*` setting, in the `[rexnative]`
section of the game's configuration file (`run/<game>.toml`) or on the command line;
`--rexnative=false` starts the game on ReXGlue's renderer. `rexnative.menu_fps` limits the frame
rate (60 by default, so menus made for a console's 60 Hz keep their speed on a faster display;
30 for a game the console ran at 30; 0 for no limit). The older names (`--rexnative_upscaler`,
PGR4Native's `pgr4_native_*` and `pgr4_d3d_*`) still work as aliases.

A game whose build inlined draws past the hooked D3D functions (Orcs Must Die!'s title, menus,
loading screens and HUD) shows them black. With `rexnative.inline_draws = true` ReXNative walks the
push buffer the game wrote between two hooked calls, as it walks a recorded command buffer, and
draws what it finds there. `rexnative.auto_handover = 30` is the other way out: after 30 frames in
which ReXNative sees at most one draw call, ReXGlue's renderer draws until the game's draw calls
come back, as F9 would. It stays off under Wine, as F9 does: under vkd3d-proton the window cannot
go from one renderer's swap chain to the other's (the Xenos driver's Present then waits for good).

## Steam Deck

A game built on the PC runs on the Steam Deck under Proton (D3D12 through vkd3d-proton):

```powershell
python -m rexnative deck D:\rexnative-work\<game> --host deck@192.168.1.10 --key ~\.ssh\deck_key --steam --try 70
```

copies it over SSH to `~/Games/ReXNative/<game>/` on the Deck (its executable and DLLs without
NVIDIA's, its files, its title update; only what changed is sent again), with
[deck/](deck/)`play.sh` (starts it with Proton: GE-Proton, else Valve's, with a prefix of the
game's own), `install.sh` and `steam_shortcuts.py`. `--steam` adds each game to Steam's library
as a non-Steam game (Steam is closed meanwhile and started again); `--try SECONDS` starts each one
from Steam, reads its log and takes a capture after that long, into `<work>/<game>/deck/`, and
writes `deck-report.md`. The Deck needs SSH with a key (`REXNATIVE_DECK` and
`REXNATIVE_DECK_KEY` can stand for `--host` and `--key`).

The game's settings on the Deck are its PC ones with the Deck's screen (1280x800, full screen);
`<work>/<game>/deck.toml` adds or replaces settings for the Deck alone, such as `[rexnative]
enabled = false` for a game ReXGlue's renderer draws better (one renderer for the whole run: F9
and `auto_handover` stay off under Proton). On the Deck, `deck.env` beside `play.sh` gives
Proton variables (`PROTON_LOG=1`), and `REXNATIVE_PROTON` picks another Proton.

## Debugging a game

Starting a game for every experiment costs minutes (logos, intro, menus). Instead,
`scripts\live.ps1 start -Dir <game dir> -Exe <exe> -Route "<keys>"` starts it once in a 1280x720
window and drives it to the problem; then each experiment takes seconds on the running game:

- `shot`: a capture of the window; `log`: the game's log; `stop`;
- `debug "<mask>"`: the renderer's switches (depth, stencil, cull, blend, alpha test off, skip a
  range of draws);
- `snap`: the render targets after given shaders (`python scripts\snaps.py` turns them into
  images); `drawlog`: one frame's draws; `trace`: the guest push buffer of one frame.

With `start -RenderDoc` (RenderDoc's folder in `REXNATIVE_RENDERDOC`), `capture` takes a
RenderDoc capture of the next frame, and `scripts\rdoc.ps1` queries it with the game closed:
`actions` (every draw, its index count and targets), `history -X -Y` (which draws touched a pixel
and what failed), `inputs -Event` (a draw's textures as PNG), `buffer -Offset -Size` (the
renderer's copy of guest memory), `mesh`, `disasm`, `cbuffers -Event`, `save`. Only a rebuild
needs a new start. [docs/NOTES.md](docs/NOTES.md#recorded-command-buffers) shows how Iron
Brigade's missing scenery was found this way.

## Repository layout

```
renderer/src/native/                  the renderer (the same for every game)
renderer/src/rexnative_d3d_hooks.cpp  the guest D3D hooks and frame trace
renderer/src/hud/                     the settings HUD
renderer/src/host.{h,cpp}             the renderer's side of the ReXGlue app: window, input, memory
renderer/src/glue_defaults.cpp        game-side functions, for a game with nothing special
renderer/rexnative.cmake              rexnative_add()
rexnative/                            the tool (python -m rexnative)
deck/                                 a game's scripts on the Steam Deck (python -m rexnative deck)
sdk/                                  the ReXGlue SDK the renderer needs: lock and patches
scripts/build-sdk.ps1                 fetch, patch, build and install that SDK
scripts/live.ps1, snaps.py            a running game for experiments, snapshots as images
scripts/rdoc.ps1, rdoc.py             RenderDoc capture queries
docs/GAME_LAYER.md                    what game_layer.h holds
docs/NOTES.md                         what each game needed, and how it was found
```

## Roadmap

- Find the HDR pass before the game's tone mapping, for DLSS 5.
- The open problems above: Perfect Dark's broken levels, Spelunky's missing text, Halo's dark
  and striped surfaces, the WMV videos of Dark Souls II and Jet Set Radio.
- The upscalers' history started over at a camera cut in a game without a camera matrix (Jet
  Set Radio transforms its vertices on the CPU).
- Memexport: a game whose picture needs it, and no GPU wait when nothing on the CPU reads its
  results.
- Inline draws found by the tool, so a game such as Orcs Must Die! needs no setting.
- Build a game end to end from `scripts\build-sdk.ps1` on a clean machine.
- Every new game becomes a reference and makes the next one easier.

Done: the renderer built from this repository for From Dust and Iron Brigade, and the scene
surface learned at run time (2026-11-14); recorded command buffers, memexport, pipelines compiled
ahead, and thirteen more games end to end (2026-11-15 and 16); the move to ReXGlue v0.11.0 and
the Steam Deck on it (2026-11-16); Midnight Club's cars and Dark Souls II (2026-11-17).

## License and credits

MIT ([LICENSE](LICENSE)). The renderer is pgr4-native's (MIT), made universal here, with code
ported from the ReXGlue SDK and Xenia (BSD-3-Clause); see
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). NVIDIA DLSS and AMD FidelityFX come from their
SDKs, under their own licenses, and are not in this repository. Xbox 360 is a trademark of
Microsoft; this project is not affiliated with Microsoft, NVIDIA, AMD or the games' publishers.
