# Getting started

How to build the SDK, prepare a game with the tool, and add the renderer to a ReXGlue project of
your own. The game's settings and keys are in [SETTINGS.md](SETTINGS.md); running it on a Steam
Deck is in [STEAM_DECK.md](STEAM_DECK.md).

> No game files, generated code or game data are in this repository. You need your own copy of
> each game.

## Requirements

- Windows 10 or 11.
- Python 3.10+ (the tool has no dependencies; the debugging scripts `snaps.py` and `resolves.py`
  need Pillow and numpy).
- Visual Studio 2022 or its Build Tools with the C++ workload.
- LLVM 20 (clang++ and lld: ReXGlue v0.11 needs clang 20 or later).
- CMake, Ninja and git.
- The Vulkan SDK (ReXGlue builds its Vulkan driver too).
- The NVIDIA DLSS SDK and DLSS 5 headers (from NVIDIA).
- To fit the renderer to a new game, the tool also needs one game it already knows (a
  *reference*), with its ReXGlue generated code and its `default.xex`: From Dust, Iron Brigade,
  Project Gotham Racing 4, or any game it fitted before.

## 1. Build the SDK

ReXGlue v0.11.0, pinned, with the patches the renderer needs ([sdk/README.md](../sdk/README.md)),
and the AMD FidelityFX SDK v2.4.0:

```powershell
scripts\build-sdk.ps1 -Llvm C:\path\to\LLVM-20\bin
```

## 2. Tell the tool where things are

Copy `ports.example.json` to `ports.json` and set the paths of your reference games. Then:

```powershell
$env:REXNATIVE_LLVM        = "C:\path\to\LLVM-20\bin"
$env:REXNATIVE_REXGLUE     = "$PWD\build\sdk-install\bin\rexglue.exe"
$env:REXNATIVE_SDK_INSTALL = "$PWD\build\sdk-install"
$env:REXNATIVE_SDK_SRC     = "$PWD\build\rexglue-sdk"
$env:REXNATIVE_FIDELITYFX  = "$PWD\build\fidelityfx-sdk"
$env:REXNATIVE_DLSS_SDK    = "C:\path\to\DLSS"
$env:REXNATIVE_DLSS_NR     = "C:\path\to\dlss_nr"
```

## 3. Prepare the game

From an archive, a folder, an Xbox Live (STFS) package or a disc image:

```powershell
python -m rexnative prepare "D:\Games\My Game.zip" --work D:\rexnative-work --title-update --dlc --project
```

This extracts the game, downloads and installs its newest title update (listed by XboxUnity,
checked against the listing) and its downloadable content (`--dlc`: the Internet Archive's Xbox
360 DLC collection, which needs an archive.org login, below), runs ReXGlue's codegen until it is clean, generates the game's
layer (`layer/report.md` says what was found, how and how sure), writes a ReXGlue project with
ReXNative and the settings HUD, builds it, finds the functions codegen missed, and builds again.
The game is then in `D:\rexnative-work\<game>\run\`: run its executable. Reports are in English;
`--lang pt` (or `REXNATIVE_LANG=pt`) writes them in Portuguese.

NVIDIA's DLSS and DLSS 5 and AMD's FidelityFX DLLs (413 MB) are kept once for every game, in
`<work>\common` (`D:\rexnative-work\common`), not in each game's `build\` and `run\`; the renderer
looks for them beside the executable first (a port that keeps them there goes on working), then in
`..\common` and `..\..\common` (`--rexnative_common_dir` names another folder). On the Steam Deck
they are in `~/Games/ReXNative/common`.

## Downloadable content

`--dlc` (or `python -m rexnative dlc <work>`, for games prepared before) takes each game's DLC
from the Internet Archive's Xbox 360 DLC collection
([msx360gcdlc](https://archive.org/details/msx360gcdlc): one RAR per game). The collection lends
its files to logged-in archive.org accounts only, so sign in once on this PC:

```powershell
pip install internetarchive
ia configure
```

`ia configure` asks for the account's e-mail and password and keeps the account's cookies in
`%USERPROFILE%\.config\internetarchive\ia.ini`, where ReXNative reads them. An account that signs
in with Google has no password: `scripts\ia-keys.ps1` opens archive.org/account/s3.php (signed in
with Google in the browser, it shows the account's S3 keys) and keeps the two keys you paste in
that ia.ini instead (`IA_ACCESS_KEY_ID` and `IA_SECRET_ACCESS_KEY` work too). A game's archives
are found by its title;
`<work>\<game>\dlc-archives.txt` names them by hand (one file name a line, or `none`). Each is
checked against the collection's MD5, its packages for the game's title are installed and the
archive deleted; `<work>\<game>\dlc.json` records them, and an archive is fetched again only when
the collection's copy changes.

## Other commands

`python -m rexnative <command> --help` gives each one's options.

| Command | What it does |
|---|---|
| `info <xex>` | title, version, XDK libraries |
| `title-update <work>` | the title updates listed, nothing downloaded |
| `dlc <work>/<game> <packages, folders or archives>` | its downloadable content, where the game finds it: `run/userdata/0000000000000000/<title id>/00000002` with its header files; `deck` takes it along |
| `dlc <work>[/<game>] [--list]` | the game's (or every game's) downloadable content from the Internet Archive's Xbox 360 DLC collection, fetched and installed; `--list` downloads nothing |
| `project <work>/<game> --build` | rebuild one game; the game's own hooks in `src/game_*.cpp` are built with it |
| `survey --work <work>` | a table of the prepared games |
| `deck <work>/<game>...` | built games onto a Steam Deck ([STEAM_DECK.md](STEAM_DECK.md)) |
| `adapt` | only the layer, for a ReXGlue project of your own (below) |
| `scene` | the scene surface from a resolve dump |
| `validate <port>` | regenerate a known port and compare |
| `art <work>/<game> <capture.png>` | the game's Steam artwork ([STEAM_DECK.md](STEAM_DECK.md#steam-artwork)) |
| `extract`, `neutral` | see their `--help` |

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

### The scene the upscalers work on

The upscalers find the game's 3D scene by themselves: the renderer learns it from the game's
resolves (the log says `ReXNative scene: learned at frame ...`;
[GAME_LAYER.md](GAME_LAYER.md#the-scene-at-run-time)). A scene with neither MSAA nor an HDR
format is not found that way: measure it once by running with `--rexnative_resolve_dump=<dir>`,
creating `<dir>/dump.request` during a 3D scene and running
`python -m rexnative scene <dir>/f<N>-resolves.jsonl`; put its result in
`results/mygame/overrides.json` and run `adapt` again.
