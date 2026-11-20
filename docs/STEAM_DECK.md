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
  `REXNATIVE_PROTON` picks another Proton.

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
