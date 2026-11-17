#!/usr/bin/env bash
# Finishes a game's install on the Steam Deck (run on the Deck, as the deck user), in the folder
# that holds this script, the game's executable and DLLs, and its files in game/:
#   install.sh "Game name"
# Makes the deck settings (<name>-deck.toml) the game's settings unless it has some, makes
# play.sh runnable and adds the game to Steam's library as a non-Steam game that runs play.sh
# with no compatibility tool, with the artwork in art/ if there is one (portrait.png, grid.png,
# hero.png, logo.png, icon.png). Steam must not be running: it rewrites its shortcuts file when
# it exits. In Game Mode Steam is started again at once after `steam -shutdown`
# (steam-launcher.service), so `python -m rexnative deck --steam` masks that unit while it runs
# this; by hand, run it from Desktop Mode with Steam closed.
set -euo pipefail
dest=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
title=${1:?usage: install.sh "Game name"}
# The newest executable, as play.sh takes it (|| true: no match is reported below, not by set -e).
exe=$(ls -1t "$dest"/*.exe 2>/dev/null | head -1 || true)
[[ -n $exe ]] || { echo "no game executable in $dest" >&2; exit 1; }
name=$(basename "$exe" .exe)
[[ -e $dest/game/default.xex || -e $dest/game/Default.xex ]] ||
  { echo "no game data: put the game's files in $dest/game" >&2; exit 1; }
# The deck settings become the game's while the game's are the ones installed last (.<name>-deck,
# a copy): changes made on the Deck (the HUD saves them) are kept.
if [[ -e $dest/$name-deck.toml ]] &&
   { [[ ! -e $dest/$name.toml ]] || cmp -s "$dest/$name.toml" "$dest/.$name-deck"; }; then
  cp "$dest/$name-deck.toml" "$dest/$name.toml"
  cp "$dest/$name-deck.toml" "$dest/.$name-deck"
fi
chmod +x "$dest/play.sh"
echo "$title installed in $dest"
if pgrep -x steam >/dev/null; then
  echo "Steam is running: not adding the shortcut (run again with Steam closed)"
  exit 0
fi
art=()
[[ -d $dest/art ]] && art=(--art-dir "$dest/art")
python3 "$dest/steam_shortcuts.py" add "$title" "$dest/play.sh" --start-dir "$dest" "${art[@]}"
