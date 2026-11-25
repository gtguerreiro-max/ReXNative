# Steam Deck

A game built on the PC runs on the Steam Deck under Proton (D3D12 through vkd3d-proton). The
frame rates each game reaches there are in [GAMES.md](GAMES.md).

## The ReXNative Manager

The easiest way there will be the **ReXNative Manager**, a Windows app that takes a game from
your own archive to the Deck. It has no public release yet; the first one is close. It runs the
steps below for you:

- **Prepare, build and test** each game with this repository's tool (`prepare --title-update
  --dlc --project`), then a boot test on the PC that compares the native renderer with ReXGlue's
  classic one at the same moments, and a second run with the Deck's settings (its resolution
  and FSR). A game the native renderer cannot draw yet goes on the classic renderer instead of
  failing.
- **Install on the Deck** over Wi-Fi, on its internal storage or the microSD card, with the
  Steam shortcut and artwork (`rexnative deck` below); see what is installed and the free space,
  and delete a game with a backup of its saves.
- **Keep games up to date:** every build records what it was made with (`run/rexnative-build.json`),
  and the app lists the ReXNative fixes a game on the Deck does not have yet, to rebuild and
  send again.
- **Deck mode:** a shortcut in the Deck's Steam library opens the app in Game Mode, made for its
  screen and controller: the installed games, updates, quick settings (renderer, upscaler, FPS
  limit), problem reports, and requests to install a game from the PC.

Until it is out, `python -m rexnative deck` does the installing from the command line.

## Installing games on the Deck by hand

```powershell
python -m rexnative deck D:\rexnative-work\<game> --host deck@192.168.1.10 --key ~\.ssh\deck_key --steam --try 70
```

copies it over SSH to `~/Games/ReXNative/<game>/` on the Deck (its executable and DLLs without
NVIDIA's, its files, its title update; only what changed is sent again), with
[deck/](../deck/)`play.sh` (starts it with Proton: GE-Proton, else Valve's, with one prefix every
game shares, `~/Games/ReXNative/prefix`; AMD's FidelityFX DLLs are in `~/Games/ReXNative/common`,
once for all), `install.sh` and `steam_shortcuts.py`.

- `--steam` adds each game to Steam's library as a non-Steam game (Steam is closed meanwhile and
  started again).
- `--try SECONDS` starts each one from Steam, reads its log and takes a capture after that long,
  into `<work>/<game>/deck/`, and writes `deck-report.md`.
- `--root sd` puts the games in `ReXNative/` on the microSD card instead (`--root <folder>` in
  any other): they use the internal Proton prefix all the same, and the FidelityFX DLLs go to
  the card's `ReXNative/common`. The shortcut's name is the title's own (its SPA, from the
  build's image dump) where the archive's is a scene release name.
- A game removed from the Deck with a file `<root>/.removed/<game>` (the ReXNative Manager
  writes it when it deletes a game) is left out: a batch that sends a list of games again does
  not put back, without its shortcut, what the user removed. `--reinstall` sends it and drops the file.
- The copy stops before it starts when the Deck's file system has no room for it. One that
  breaks off (Wi-Fi, the Deck asleep) says how far it got and why; `deck` run again sends only
  what is missing.
- The Deck needs SSH with a key (`REXNATIVE_DECK` and `REXNATIVE_DECK_KEY` can stand for `--host`
  and `--key`).

## Settings on the Deck

The game's settings on the Deck are its PC ones with the Deck's screen (1280x800, full screen)
and AMD's FSR 4 as the scene's antialiasing at the game's own resolution (Native AA; GE-Proton's
FSR 4 upgrade, FSR 3.1 without it): on both Overlords it kept their 30 FPS and looked better than
the console's MSAA. Settings changed on the Deck (the HUD: View + Menu) are kept by later
installs.

Frame generation (AMD FSR, the HUD's Upscaler page) turns on and off while the game runs: the
game at 30 FPS, the screen at 60. In Sega Rally Revo's attract races (1,500 to 3,600 draws a
frame) the Deck kept 33.3 ms a frame with it on, every frame with a generated one before it, the
HUD sharp (2026-11-19). The fixes of 2026-11-20 (its optical flow, which FSR 3.1.6 had left at
zero, the motion vectors of Sega Rally's ground, the HUD kept out in more games: GAMES.md) were
tried on the PC; a game built before them needs building and sending again to get them.
It adds input delay: a frame waits for the next one, which the generated frame goes between.

- `<work>/<game>/deck.toml` adds or replaces settings for the Deck alone, such as
  `[rexnative] classic = true` for a game ReXGlue's renderer draws better: it starts on that
  renderer, and the HUD's Renderer (View + Menu) switches to the native one and back while it
  runs, as F9 and `auto_handover` do on the PC.
- On the Deck, `deck.env` beside `play.sh` gives Proton variables (`PROTON_LOG=1`), and
  `REXNATIVE_PROTON` picks another Proton. One in the games' folder (`~/Games/ReXNative/deck.env`)
  goes for every game there.

### The GPU's clock while a game plays

`play.sh` keeps the GPU at 1000 MHz or more while the game plays (up to the Deck's 1600) and
sets SteamOS's automatic clock back when it ends, however it ends. It uses the control Steam's
own GPU slider uses (`steamos-priv-write`, `pp_od_clk_voltage` in manual mode). The floor is the
game's setting `[rexnative] gpu_floor` (MHz, 0 for the automatic clock): the settings menu's
**GPU clock** (Display page: Steady or Automatic) and the ReXNative Manager's GPU clock switch for
the game both write it. A watcher outside the game's processes (a user unit,
`play.sh --gpu-floor-watch`) reads it each second while the game runs and sets the clock at once
when it changes. A clock set in Steam's quick menu goes first, for as long as it is set; the
automatic clock found in the floor's place (back from a suspend, or the quick menu's GPU clock
turned off) gets the floor again. Steam applies a game's performance profile a few seconds after
it starts from Play and when it ends, and with its GPU clock on automatic it then "latches" the
quick menu's slider (1600 MHz unless moved: "Latching manual GPU clock" in
`~/.local/share/Steam/logs/steamui_steamos.txt`) through steamos-manager. That write fails on
the automatic clock, but in the floor's manual mode it took and locked the GPU at 1600 MHz for the
rest of the game and after it (ReXNative debd573 to e5646c0). The watcher now tells it from a
player's clock (Steam logs "Setting manual GPU clock" for that) and puts the floor back, sets the
automatic clock at the end, and a game starting sets one left over back to automatic. The
Deck's APU shares 15 W between CPU and GPU: when a game's threads burst at 3.5 GHz the
automatic clock let the GPU fall to 200-450 MHz in the middle of frames, which took 45-70 ms.
Measured 2026-11-25, 80 s in play for each:

| | Forza Horizon (30 fps) | Iron Brigade (60 fps) | Overlord II (30 fps) |
|---|---|---|---|
| Automatic | 8 and 11 frames over 45 ms, worst 58-62 ms, 8.1-8.2 W | none, 8.2 W | none, 6.7 W |
| GPU floor 1000 MHz | none, worst 42 ms, 8.2 W | none, 8.1 W | none, 6.5 W |
| CPU boost off | none, worst 39 ms, 7.7 W | none, 9.2 W | none, 7.1 W |
| `lavd` scheduler | 16 frames over 45 ms, 8.3 W | | |

The boost off also took Forza's frames away, but cost the lighter games power (their CPU ran
at a higher clock without it). Without the setting in the game's file, `REXNATIVE_GPU_FLOOR` in
a `deck.env` says the floor (MHz, 0 none; the Manager's switch wrote it there before the menu had
the setting), else 1000. Installing a game also gives the games installed beside it the kit's
`play.sh`.

The frame latency with the floor (Forza Horizon driving, 115 s each, 2026-11-25): at 1, 1 frame
late (50 ms); at 2, 228, its frames ready 4.5 ms before their refresh against 8 at 1 (the game's
threads and the GPU working at once share the 15 W, and the GPU's clock drops). With the GPU fixed
at 1600 MHz (Steam's latch above), 1 at 1 and 36 at 2. Auto stays at 1 there, as it should; a 2
picked by hand under the latch felt better than it is.

## Steam artwork

`python -m rexnative art <work>/<game> <capture.png>` makes the game's Steam artwork (library
capsule, wide capsule, banner, icon) from one of its captures, best its title screen
(`live.ps1 frame` or `shot`, or a `deck/try-*.png` of `--try`), into `<work>/<game>/deck/art/`,
which the next `deck` sends and gives Steam with the shortcut.

## Experiments on the Deck

`python scripts\deck_drive.py <work>\<game> <label> "<at>:<item>,..." <end> [flags]` starts an
installed game from Steam with extra flags and, at the given seconds, sends keys, takes the
renderer's captures (`cap`), sets draw-debug switches (`dbg<mask>`) and reads the APU's power
(`pwr`); the captures and the log come back to `<work>\<game>\deck`. More on debugging:
[DEBUGGING.md](DEBUGGING.md).

## Preparing and building on the Deck itself

The tool also runs on Linux, to make a game's Windows build on the Deck with no PC (the
ReXNative Deck app sets this up). Measured on a Deck: codegen about twice the PC's time with
jemalloc, a build 2.6 times; the build runs under Proton like one made on the PC.

- `project --build` configures with the CMake toolchain file `$REXNATIVE_TOOLCHAIN_FILE` (clang
  for `x86_64-pc-windows-msvc`, lld, the MSVC CRT and the Windows SDK) instead of `vcenv.cmd`;
  `$REXNATIVE_REXGLUE` is ReXGlue's codegen built for Linux, from the same commit as the SDK
  (its generated code matches the Windows tool's function for function).
- The runs a prepare makes (the image dump, `--discover`) start the game through
  `$REXNATIVE_RUNNER`, a command that runs a Windows program (Proton, with a prefix of its own).
- `refs-pack <folder>` writes each known port's profile and listings (reference packs, about
  25 MB for three ports) and a `ports.json` naming them: with it as `--ports`, `adapt` and
  `prepare` work without the reference games' sources or generated code, with the same result.
  The packs are derived from the games: keep them to yourself.
- `deck --local` installs on the machine it runs on (no SSH); the game's files are hard links to
  the work folder's, so a game the Deck prepared does not take its space twice. Steam must be
  closed to add a shortcut this way (`install.sh` says so and leaves it).
