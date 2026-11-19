# Steam Deck

A game built on the PC runs on the Steam Deck under Proton (D3D12 through vkd3d-proton). The
frame rates each game reaches there are in [GAMES.md](GAMES.md).

## Installing games on the Deck

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
- The Deck needs SSH with a key (`REXNATIVE_DECK` and `REXNATIVE_DECK_KEY` can stand for `--host`
  and `--key`).

## Settings on the Deck

The game's settings on the Deck are its PC ones with the Deck's screen (1280x800, full screen)
and AMD's FSR 4 as the scene's antialiasing at the game's own resolution (Native AA; GE-Proton's
FSR 4 upgrade, FSR 3.1 without it): on both Overlords it kept their 30 FPS and looked better than
the console's MSAA. Settings changed on the Deck (the HUD: View + Menu) are kept by later
installs.

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
