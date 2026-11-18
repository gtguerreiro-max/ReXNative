# Contributing to ReXNative

Thanks for your interest. ReXNative is a small project kept by one person in their spare time,
so reports that are easy to reproduce and pull requests that do one thing are the most help.

## The one hard rule: nothing from a game

This repository must never hold anything taken or derived from a game:

- no game files (`.xex`, data, saves, packages, disc images), and no listings of a game's files;
- no code ReXGlue generated from a game (`generated/`), and no part of its listing;
- no game layer (`game_layer.h`, `d3d_surface.inc`, `report.md`, a game's `overrides.json`):
  they are addresses and data derived from the game, which is why `results/` and `references/`
  are in `.gitignore`;
- no hooks written for one game (a project's `src/game_*.cpp`): they stay in that game's own
  project, on your computer.

The renderer and the tool must stay game-neutral: what differs between games goes into the layer
the tool generates, or into a `rexnative.*` setting. A pull request that adds any of the above is
closed, and the content removed from history if it was pushed.

## Reporting a game

Open an issue with the **Game report** form. Say which game and title update, PC or Steam Deck,
how far it got with ReXNative and with ReXGlue's own renderer (`--rexnative=false`), and attach the
game's log. Screenshots or captures help; files from the game do not belong in an issue. The
results so far are in [docs/GAMES.md](docs/GAMES.md).

## Reporting a bug

Open an issue with the **Bug report** form: what you did, what happened, what you expected, your
GPU and driver (or the Steam Deck and its Proton), and the log. For a security problem, read
[SECURITY.md](SECURITY.md) instead.

## Pull requests

1. Open an issue first for anything bigger than a fix, so we can agree on the approach before you
   spend time on it.
2. Build it ([docs/GETTING_STARTED.md](docs/GETTING_STARTED.md)) and run at least one game the
   change touches, and one it should not (From Dust or Iron Brigade are the usual references).
   Say in the pull request what you ran, and where (PC, Steam Deck).
3. Keep the tool (`rexnative/`) to Python's standard library: it has no dependencies, and should
   keep having none. C++ follows the style of the file around it.
4. Update the documentation the change affects: a game's result in [docs/GAMES.md](docs/GAMES.md),
   what was found and why in [docs/NOTES.md](docs/NOTES.md), a new setting in
   [docs/SETTINGS.md](docs/SETTINGS.md).
5. One change per commit, its message starting with the part it touches (`renderer:`,
   `prepare:`, `deck:`, `scripts:`, `docs:`) and saying what now happens.

Changes to the ReXGlue SDK itself go in `sdk/patches/v0.11/` (see [sdk/README.md](sdk/README.md));
a fix that belongs in ReXGlue is better sent to [ReXGlue](https://github.com/rexglue/rexglue-sdk)
too.

## License

By contributing, you agree that your contribution is licensed under the [MIT License](LICENSE),
like the rest of the project. Code ported from another project keeps its license and is listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## Conduct

Everyone taking part is expected to follow the [Code of Conduct](CODE_OF_CONDUCT.md).
