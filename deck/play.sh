#!/usr/bin/env bash
# Plays a ReXGlue game drawn by ReXNative (its Windows build) under Proton on the Steam Deck.
# `python -m rexnative deck` puts it beside the game's executable, the only .exe in the folder,
# with the game's files in game/; Steam runs it as a non-Steam game with NO compatibility tool
# (install.sh adds it): it starts Proton itself, with a prefix of its own in this folder.
#   play.sh [game flags...]
# REXNATIVE_PROTON picks another Proton (default: GE-Proton, the newest, else Valve's Proton
# Experimental, else its newest numbered Proton, in any Steam library: the SD card's too).
here=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
# deck.env beside this script, if present: this Deck's own environment for Proton, one
# VAR=value a line, e.g. PROTON_LOG=1.
if [[ -f $here/deck.env ]]; then
  set -a
  # shellcheck disable=SC1091
  . "$here/deck.env"
  set +a
fi
# FSR 4 on the Deck: GE-Proton fetches AMD's amdxcffx64.dll into the prefix (once), with which the
# FSR SDK 2.4 upscaler runs FSR 4.1 (its RDNA 2 model) instead of FSR 3.1. Used when the upscaler
# is on (F1, Upscaler); PROTON_FSR4_UPGRADE=0 in deck.env leaves FSR 3.1. Valve's Proton ignores it.
export PROTON_FSR4_UPGRADE=${PROTON_FSR4_UPGRADE:-1}
steam=$HOME/.local/share/Steam
proton=${REXNATIVE_PROTON:-}
if [[ -z $proton ]]; then
  # Steam's libraries: the internal one and those libraryfolders.vdf lists (an SD card's).
  libs=("$steam")
  while IFS= read -r lib; do libs+=("$lib"); done < <(
    sed -n 's/^[[:space:]]*"path"[[:space:]]*"\(.*\)"[[:space:]]*$/\1/p' \
      "$steam/steamapps/libraryfolders.vdf" 2>/dev/null)
  candidates=()
  while IFS= read -r p; do candidates+=("$p"); done < <(
    ls -1d "$steam"/compatibilitytools.d/GE-Proton*/ 2>/dev/null | sort -V -r)
  for lib in "${libs[@]}"; do candidates+=("$lib/steamapps/common/Proton - Experimental/"); done
  while IFS= read -r p; do candidates+=("$p"); done < <(
    for lib in "${libs[@]}"; do
      for p in "$lib"/steamapps/common/Proton\ [0-9]*/; do
        [[ -d $p ]] && printf '%s\t%s\n' "$(basename "$p")" "$p"
      done
    done | sort -t$'\t' -k1,1 -V -r | cut -f2)
  for p in "${candidates[@]}"; do
    [[ -x $p/proton ]] && { proton=${p%/}; break; }
  done
fi
[[ -n $proton ]] || { echo "no Proton found (set REXNATIVE_PROTON)" >&2; exit 1; }
# The newest executable, should an older build's still be in the folder.
exe=$(ls -1t "$here"/*.exe 2>/dev/null | head -1)
[[ -n $exe ]] || { echo "no game executable in $here" >&2; exit 1; }
name=$(basename "$exe" .exe)
export STEAM_COMPAT_DATA_PATH=$here/prefix STEAM_COMPAT_CLIENT_INSTALL_PATH=$steam
export PROTON_USE_XALIA=0
mkdir -p "$STEAM_COMPAT_DATA_PATH" "$here/logs"
cd "$here"
# ReXGlue v0.11 writes logs/<name>_NNN.log beside the game (log.file_name is the name alone) and
# removes old runs itself.
# waitforexitandrun, as Steam starts Proton: GE-Proton runs its protonfixes and upscaler setup
# (PROTON_FSR4_UPGRADE) only then.
# try.flags, if present: flags for one try (python -m rexnative deck --try), on one line.
extra=()
[[ -f $here/try.flags ]] && read -r -a extra < "$here/try.flags"
exec "$proton/proton" waitforexitandrun "$exe" "--log_file=$name" "${extra[@]}" "$@"
