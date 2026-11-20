#!/bin/bash
# A driving session on the Steam Deck: a test build (a Windows build under Proton, or a native
# Linux one) started from an installed game's Steam shortcut through proton-shim, a person
# playing; once the game's scene is heavy (a frame line with HEAVY draws or more), ReXNative's
# frame latency alternates 2 and 1 every PHASE s (default 60) through the control file, ROUNDS
# times (default 3); then it stays at 2 and the game runs on until the player quits.
#   deck-drive-latency.sh <installed dir> <test dir> <appid> <process regex>
# The test dir's try.flags must name its control.txt (--rexnative_d3d_control), frame lines
# (--rexnative_d3d_stats_frames=30) and, for deck-drive-report.py, the per-frame CSV
# (--rexnative_csv=<test dir>/logs/<name>.csv). The SoC's power, the GPU's busy % and clock are
# sampled every second into <test dir>/logs/drive-power.txt (seconds, latency, mW, %, MHz).
G=$1; T=$2; APP=$3; RE=$4
PHASE=${PHASE:-60}; ROUNDS=${ROUNDS:-3}; HEAVY=${HEAVY:-2500}
export XDG_RUNTIME_DIR=/run/user/1000 DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus
if pgrep -af "[S]teamLaunch AppId=" >/dev/null; then echo "BUSY: a game runs"; exit 3; fi
[ -e "$G/deck.env" ] && { echo "deck.env exists already: refusing"; exit 4; }
if [ -x "$T/run-linux.sh" ]; then B="REXNATIVE_LINUX_BUILD=$T"; else B="REXNATIVE_WIN_BUILD=$T"; fi
printf 'REXNATIVE_PROTON=%s\n%s\n' "$HOME/Games/ReXNative-vulkan-tests/proton-shim" "$B" > "$G/deck.env"
trap 'rm -f "$G/deck.env"' EXIT
started=$(date +%s)
DISPLAY=:0 nohup steam steam://rungameid/$(( (APP << 32) | 0x02000000 )) >/dev/null 2>&1 &
for i in $(seq 60); do pgrep -f "$RE" >/dev/null && break; sleep 2; done
pgrep -f "$RE" >/dev/null || { echo "NOT started in 120 s"; exit 5; }
# play.sh has read deck.env: gone, so the installed game starts as itself again.
sleep 10; rm -f "$G/deck.env"
log=
for i in $(seq 30); do
  log=$(find "$T/logs" -maxdepth 1 -name "*.log" -newermt "@$started" | head -1)
  [ -n "$log" ] && break; sleep 2
done
echo "LOG $log"
draws() { grep -E "ReXNative: frames [0-9]+ \| per frame: draws" "$log" | tail -1 | sed -E 's/.*draws ([0-9]+).*/\1/'; }
until [ "$(draws)" -ge "$HEAVY" ] 2>/dev/null; do
  pgrep -f "$RE" >/dev/null || { echo "QUIT before a heavy scene"; exit 0; }
  sleep 2
done
echo "HEAVY $(date +%T), $(( $(date +%s) - started )) s after the start"
dev=/sys/class/drm/card0/device; hw=$(ls -d $dev/hwmon/hwmon* | head -1)
out="$T/logs/drive-power.txt"; : > "$out"
for r in $(seq "$ROUNDS"); do
  for L in 2 1; do
    pgrep -f "$RE" >/dev/null || break 2
    echo "set rexnative_frame_latency $L" > "$T/control.txt"
    echo "PHASE $(date +%T) latency $L"
    for ((s = 0; s < PHASE; s++)); do
      echo "$(date +%s) $L $(( $(cat $hw/power1_average) / 1000 )) $(cat $dev/gpu_busy_percent) $(( $(cat $hw/freq1_input) / 1000000 ))" >> "$out"
      sleep 1
    done
  done
done
pgrep -f "$RE" >/dev/null && echo "set rexnative_frame_latency 2" > "$T/control.txt"
echo "DONE $(date +%T): frame latency 2 again; the game runs on"
