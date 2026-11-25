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
# The GPU's clock kept at 1000 MHz or more while the game plays (up to the Deck's 1600), set back
# to SteamOS's automatic clock when it ends. The Deck's APU shares 15 W between CPU and GPU: when
# a game's threads burst at 3.5 GHz the automatic clock let the GPU fall to 200-450 MHz in the
# middle of frames, which took 45-70 ms. Forza Horizon (2026-11-25), 80 s in play each: 8 and 11
# frames over 45 ms (worst 58-62 ms) automatic, none (worst 42 ms) with the floor, 8.2 W both
# ways; Iron Brigade and Overlord II unchanged at the same power or less. The control Steam's own
# GPU slider uses (steamos-priv-write: pp_od_clk_voltage in manual mode).
# The floor is the game's setting [rexnative] gpu_floor (MHz, 0 for SteamOS's automatic clock),
# which its settings menu (Display, GPU clock) and the ReXNative Manager's GPU clock switch write;
# without it REXNATIVE_GPU_FLOOR (a deck.env's), else 1000. A watcher outside the game's
# processes (a user unit: play.sh --gpu-floor-watch) reads it each second while the game runs
# (Proton, after the exec) and sets the clock at once when it changes. A clock the player set in
# Steam's quick menu goes first, for as long as it is set; SteamOS's automatic clock found in the
# floor's place (back from a suspend, or the quick menu's) gets the floor again. When the game
# ends, however it ends, the automatic clock comes back if the floor was ours; the mark file says
# so should the Deck have stopped first (the next game sets the automatic clock back then).
# Steam's latch. Steam applies the game's performance profile a few seconds after it starts and
# when it ends; with its GPU clock on automatic it writes "Setting auto GPU clock" and then
# "Latching manual GPU clock: N MHz" in its log, N being the quick menu's slider (1600 unless the
# player moved it), which steamos-manager writes as a fixed clock N-N. On SteamOS's automatic
# clock that write fails (the GPU takes no manual clock then); in the floor's manual mode it took,
# and locked the GPU at 1600 MHz for the rest of the game and after it (2026-11-25: the player's
# Forza Horizon, started from Steam's Play; games started by RunGame get no profile). So a fixed
# clock equal to the one Steam last latched, its choice being automatic, is not the player's: the
# watcher puts the floor back (not twice within 5 s: something else would be fighting it), sets the
# automatic clock at the end, and a game starting finds one left that way and sets it back. A
# player's quick menu clock is logged "Setting manual GPU clock" and goes first as before.
gpu_w=/usr/bin/steamos-polkit-helpers/steamos-priv-write
gpu_mark=${XDG_STATE_HOME:-$HOME/.local/state}/rexnative/gpu-floor
gpu_dev=
gpu_find() {
  local d
  for d in /sys/class/drm/card*/device; do
    grep -qs '^OD_SCLK' "$d/pp_od_clk_voltage" && { gpu_dev=$d; return 0; }
  done
  return 1
}
gpu_set() { "$gpu_w" "$gpu_dev/$1" "$2" >/dev/null 2>&1; }
gpu_auto() {
  gpu_set pp_od_clk_voltage r
  gpu_set pp_od_clk_voltage c
  gpu_set power_dpm_force_performance_level auto
  rm -f "$gpu_mark"
}
# The clock now: gpu_level (auto, manual...), gpu_s0 and gpu_s1 (the OD_SCLK range set, MHz),
# gpu_min and gpu_max (the range the GPU allows). Bash alone: no process a second.
gpu_read() {
  local line sec= re_od='^([01]):[[:space:]]*([0-9]+)Mhz' re_range='^SCLK:[[:space:]]*([0-9]+)Mhz[[:space:]]*([0-9]+)Mhz'
  gpu_level= gpu_s0= gpu_s1=
  read -r gpu_level < "$gpu_dev/power_dpm_force_performance_level" 2>/dev/null
  while IFS= read -r line; do
    case $line in
      OD_SCLK:*) sec=sclk ;;
      OD_*) sec= ;;
      *)
        if [[ $sec == sclk && $line =~ $re_od ]]; then
          if [[ ${BASH_REMATCH[1]} == 0 ]]; then gpu_s0=${BASH_REMATCH[2]}; else gpu_s1=${BASH_REMATCH[2]}; fi
        elif [[ $line =~ $re_range ]]; then
          gpu_min=${BASH_REMATCH[1]} gpu_max=${BASH_REMATCH[2]}
        fi ;;
    esac
  done < "$gpu_dev/pp_od_clk_voltage"
}
# gpu_latch: the clock Steam latched last (above), when that is its last word on the GPU clock.
gpu_steam_latch() {
  local log=$HOME/.local/share/Steam/logs/steamui_steamos.txt line re='Latching manual GPU clock: ([0-9]+)'
  gpu_latch=
  [[ -r $log ]] || return 1
  line=$(tac "$log" 2>/dev/null | grep -m1 -E 'Setting (auto|manual) GPU clock|Latching manual GPU clock: [0-9]+')
  [[ $line =~ $re ]] && gpu_latch=${BASH_REMATCH[1]}
  [[ -n $gpu_latch ]]
}
# Whether the clock now is Steam's latch: a fixed clock equal to it.
gpu_latched() {
  [[ $gpu_level == manual && -n $gpu_s0 && $gpu_s0 == "$gpu_s1" ]] && gpu_steam_latch &&
    [[ $gpu_s0 == "$gpu_latch" ]]
}
# gpu_want: the floor the game's settings file ($1) asks for, within the GPU's range (0: none).
gpu_wanted() {
  local line table= v= re_table='^[[:space:]]*\[[[:space:]]*"?([A-Za-z0-9_.-]+)"?[[:space:]]*\]'
  local re_key='^[[:space:]]*gpu_floor[[:space:]]*=[[:space:]]*"?([0-9]+)'
  if [[ -f $1 ]]; then
    while IFS= read -r line || [[ -n $line ]]; do
      if [[ $line =~ $re_table ]]; then
        table=${BASH_REMATCH[1]}
      elif [[ $table == rexnative && $line =~ $re_key ]]; then
        v=${BASH_REMATCH[1]}
        break
      fi
    done < "$1"
  fi
  [[ $v =~ ^[0-9]+$ ]] || v=${REXNATIVE_GPU_FLOOR:-1000}
  [[ $v =~ ^[0-9]+$ ]] || v=1000
  v=$((10#$v))
  if (( v > 0 )); then
    (( v < gpu_min )) && v=$gpu_min
    (( v > gpu_max )) && v=$gpu_max
  fi
  gpu_want=$v
}
# play.sh --gpu-floor-watch <the game's pid> <its settings file>: the watcher.
gpu_watch() {
  local pid=$1 file=$2 last=-1 failed=0 ever=0 back=-100
  held=0 floor=0  # globals: the trap reads them
  gpu_find || return 0
  gpu_read
  [[ $gpu_max =~ ^[0-9]+$ ]] || return 0
  trap '(( held )) && gpu_auto; exit 0' TERM INT HUP
  while kill -0 "$pid" 2>/dev/null; do
    gpu_wanted "$file"
    (( gpu_want != last )) && failed=0
    last=$gpu_want
    gpu_read
    if (( held )); then
      if [[ $gpu_level != manual || $gpu_s0 != "$floor" || $gpu_s1 != "$gpu_max" ]]; then
        if (( SECONDS - back >= 5 )) && gpu_latched; then
          # Steam's latch (above): the floor back.
          back=$SECONDS
          gpu_set pp_od_clk_voltage "s 0 $floor" && gpu_set pp_od_clk_voltage "s 1 $gpu_max" &&
            gpu_set pp_od_clk_voltage c
          gpu_read
        else
          # Not ours any more: the quick menu's clock, or the automatic one (a suspend's).
          held=0
          rm -f "$gpu_mark"
        fi
      elif (( gpu_want == 0 )); then
        gpu_auto
        held=0
        gpu_read
      elif (( gpu_want != floor )); then
        gpu_set pp_od_clk_voltage "s 0 $gpu_want" && gpu_set pp_od_clk_voltage c && floor=$gpu_want
      fi
    fi
    if (( !held && !failed && gpu_want > 0 )) && [[ $gpu_level == auto ]]; then
      mkdir -p "$(dirname "$gpu_mark")" && : > "$gpu_mark"
      if gpu_set power_dpm_force_performance_level manual &&
         gpu_set pp_od_clk_voltage "s 0 $gpu_want" &&
         gpu_set pp_od_clk_voltage "s 1 $gpu_max" &&
         gpu_set pp_od_clk_voltage c; then
        held=1 ever=1 floor=$gpu_want
      else
        gpu_auto
        failed=1
      fi
    fi
    sleep 1
  done
  # Ours, or Steam's latch over ours (it latches as the game ends too).
  gpu_read
  if (( held )) || { (( ever )) && gpu_latched; }; then gpu_auto; fi
  return 0
}
# The watcher for this game, started before Proton.
gpu_floor_start() {
  [[ -x $gpu_w ]] && command -v systemd-run >/dev/null && gpu_find || return 0
  gpu_read
  # A floor of ours that a game which never ended left (no watcher keeps it), or Steam's latch
  # over one (above): automatic again.
  if [[ $gpu_level == manual ]] && ! systemctl --user -q is-active 'rexnative-gpu-floor-*' 2>/dev/null &&
     { [[ -f $gpu_mark ]] || gpu_latched; }; then
    gpu_auto
  fi
  local env=()
  [[ -n ${REXNATIVE_GPU_FLOOR:-} ]] && env=(--setenv="REXNATIVE_GPU_FLOOR=$REXNATIVE_GPU_FLOOR")
  systemd-run --user --quiet --collect --unit="rexnative-gpu-floor-$$" "${env[@]}" \
    /bin/bash "$(readlink -f "$0")" --gpu-floor-watch "$$" "$1" >/dev/null 2>&1 || true
}
if [[ ${1:-} == --gpu-floor-watch ]]; then
  gpu_watch "$2" "$3"
  exit 0
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
gpu_floor_start "$here/$name.toml"
exec "$proton/proton" waitforexitandrun "$exe" "--log_file=$name" "${extra[@]}" "$@"
