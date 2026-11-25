#!/usr/bin/env bash
# Plays a ReXGlue game drawn by ReXNative (its Windows build) under Proton on the Steam Deck.
# `python -m rexnative deck` puts it beside the game's executable, the only .exe in the folder,
# with the game's files in game/; Steam runs it as a non-Steam game with NO compatibility tool
# (install.sh adds it): it starts Proton itself, with the prefix every game of the kit shares.
#   play.sh [game flags...]
# REXNATIVE_PROTON picks another Proton (default: GE-Proton, the newest, else Valve's Proton
# Experimental, else its newest numbered Proton, in any Steam library: the SD card's too).
here=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
# deck.env beside this script, if present: this Deck's own environment for Proton, one
# VAR=value a line, e.g. PROTON_LOG=1. One in the games' folder (~/Games/ReXNative/deck.env)
# goes for every game there, read first.
for env in "$(dirname "$here")/deck.env" "$here/deck.env"; do
  if [[ -f $env ]]; then
    set -a
    # shellcheck disable=SC1090
    . "$env"
    set +a
  fi
done
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
# One Proton prefix for every game (~/Games/ReXNative/prefix, beside the games' folders): 400 MB
# once, not in each game's folder. A game keeps its files, saves (userdata/) and settings in its
# own folder; REXNATIVE_PREFIX (deck.env) names another prefix. A game in another folder (the
# microSD card's ReXNative/, `deck --root sd`) uses the internal one unless one is beside it.
prefix=${REXNATIVE_PREFIX:-$(dirname "$here")/prefix}
if [[ -z ${REXNATIVE_PREFIX:-} && ! -d $prefix && -d $HOME/Games/ReXNative/prefix ]]; then
  prefix=$HOME/Games/ReXNative/prefix
fi
export STEAM_COMPAT_DATA_PATH=$prefix STEAM_COMPAT_CLIENT_INSTALL_PATH=$steam
export PROTON_USE_XALIA=0
# GE-Proton's protonfixes takes the game's id from UMU_ID, SteamAppId or SteamGameId, else the
# last number in STEAM_COMPAT_DATA_PATH, and stops the game when it finds none (GE-Proton11-12:
# IndexError, the game never starts). Steam gives a shortcut started from its Play button
# SteamAppId; one started by SteamClient.Apps.RunGame (a Decky plugin's play button) had none,
# and the shared prefix's path has no number (2026-11-25). 0: no game's fixes, as a shortcut's.
if [[ -z ${UMU_ID:-}${SteamAppId:-}${SteamGameId:-} ]]; then
  export SteamAppId=0
fi
# The same launch had no SteamDeck=1 either, which the renderer (1x on the Deck) and its HUD (the
# Deck's controls, no PC-only rows) go by: the Deck's own firmware says so (Valve's board).
if [[ -z ${SteamDeck:-} && $(cat /sys/class/dmi/id/board_vendor 2>/dev/null) == Valve ]]; then
  export SteamDeck=1
fi
mkdir -p "$STEAM_COMPAT_DATA_PATH" "$here/logs"
cd "$here"
# ReXGlue v0.11 writes logs/<name>_NNN.log beside the game (log.file_name is the name alone) and
# removes old runs itself.
# waitforexitandrun, as Steam starts Proton: GE-Proton runs its protonfixes and upscaler setup
# (PROTON_FSR4_UPGRADE) only then.
# try.flags, if present: flags for one try (python -m rexnative deck --try), on one line.
extra=()
[[ -f $here/try.flags ]] && read -r -a extra < "$here/try.flags"
# The GPU's clock kept at 1000 MHz or more while the game plays (up to the Deck's 1600), set back
# to SteamOS's automatic clock when it ends. The Deck's APU shares 15 W between CPU and GPU: when
# a game's threads burst at 3.5 GHz the automatic clock let the GPU fall to 200-450 MHz in the
# middle of frames, which took 45-70 ms. Forza Horizon (2026-11-25), 80 s in play each: 8 and 11
# frames over 45 ms (worst 58-62 ms) automatic, none (worst 42 ms) with the floor, 8.2 W both
# ways; Iron Brigade and Overlord II unchanged at the same power or less. The control Steam's own
# GPU slider uses (steamos-priv-write: pp_od_clk_voltage in manual mode). Only from SteamOS's
# automatic clock: a clock the player set in the quick menu stays. REXNATIVE_GPU_FLOOR in a
# deck.env: another floor in MHz, 0 for none. A user unit outside the game's processes waits for
# this one (Proton after the exec) and sets the automatic clock back however the game ends; the
# mark file says the manual clock was ours should the Deck have stopped before that.
gpu_floor() {
  local floor=$1 w=/usr/bin/steamos-polkit-helpers/steamos-priv-write dev= d
  local mark=${XDG_STATE_HOME:-$HOME/.local/state}/rexnative/gpu-floor
  [[ $floor =~ ^[0-9]+$ ]] && (( floor > 0 )) || return 0
  [[ -x $w ]] && command -v systemd-run >/dev/null || return 0
  for d in /sys/class/drm/card*/device; do
    grep -qs '^OD_SCLK' "$d/pp_od_clk_voltage" && { dev=$d; break; }
  done
  [[ -n $dev ]] || return 0
  local level max
  level=$(<"$dev/power_dpm_force_performance_level") || return 0
  if [[ $level != auto ]]; then
    # The player's clock, unless a game of ours set it and never set it back.
    [[ $level == manual && -f $mark ]] &&
      ! systemctl --user -q is-active 'rexnative-gpu-floor-*' 2>/dev/null || return 0
  fi
  max=$(sed -n 's/^SCLK: *[0-9]*Mhz *\([0-9]*\)Mhz.*/\1/p' "$dev/pp_od_clk_voltage")
  [[ $max =~ ^[0-9]+$ ]] || return 0
  (( floor <= max )) || floor=$max
  local back
  back="$(printf '%q' "$w") $dev/pp_od_clk_voltage r; $(printf '%q' "$w") $dev/pp_od_clk_voltage c; "
  back+="$(printf '%q' "$w") $dev/power_dpm_force_performance_level auto; rm -f $(printf '%q' "$mark")"
  mkdir -p "$(dirname "$mark")" && : > "$mark"
  if "$w" "$dev/power_dpm_force_performance_level" manual >/dev/null 2>&1 &&
     "$w" "$dev/pp_od_clk_voltage" "s 0 $floor" >/dev/null 2>&1 &&
     "$w" "$dev/pp_od_clk_voltage" "s 1 $max" >/dev/null 2>&1 &&
     "$w" "$dev/pp_od_clk_voltage" c >/dev/null 2>&1 &&
     systemd-run --user --quiet --collect --unit="rexnative-gpu-floor-$$" /bin/sh -c \
       "while kill -0 $$ 2>/dev/null; do sleep 1; done; $back" >/dev/null 2>&1; then
    return 0
  fi
  /bin/sh -c "$back" >/dev/null 2>&1
}
gpu_floor "${REXNATIVE_GPU_FLOOR:-1000}"
exec "$proton/proton" waitforexitandrun "$exe" "--log_file=$name" "${extra[@]}" "$@"
