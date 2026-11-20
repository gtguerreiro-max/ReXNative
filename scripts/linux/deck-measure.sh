#!/bin/bash
# Measures a game installed on the Steam Deck (python -m rexnative deck), run on the Deck itself:
#   deck-measure.sh <game dir> <process regex> <shortcut appid> <label> <warm-up s> <window s> [flags...]
# Starts the game from its Steam shortcut with the flags (try.flags), waits the warm-up, samples
# the GPU (busy %, clock, the SoC's power) and the CPU over the window, takes a gamescope
# screenshot halfway (/tmp/vk-<label>.png), stops the game and removes try.flags (and deck.env).
# Environment:
#   MH=1          MangoHud logs every frame (deck.env; the HUD shows: no_display logs nothing) and
#                 the frame rate, frame times and MangoHud's GPU load of the window are printed.
#   EXTRA_ENV     more deck.env lines, e.g. REXNATIVE_PROTON=<dir of proton-shim> and
#                 REXNATIVE_LINUX_BUILD=<the game's linux/ folder>: Steam then starts the native
#                 Linux build in place of the Proton one (proton-shim).
#   LOGDIR        the folder whose logs/ has the run's log (default: the game dir).
# It refuses when a game runs, or when try.flags or deck.env is there already.
dir=$1; exe_re=$2; appid=$3; label=$4; warm=$5; win=$6; shift 6
export XDG_RUNTIME_DIR=/run/user/1000 DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus
if pgrep -af "[S]teamLaunch AppId=" >/dev/null; then echo "BUSY: a game runs"; exit 3; fi
cd "$dir" || exit 1
[ -e try.flags ] && { echo "try.flags exists already: refusing"; exit 4; }
if [ -n "$MH" ]; then
  [ -e deck.env ] && { echo "deck.env exists already: refusing"; exit 4; }
  rm -rf /tmp/mh; mkdir -p /tmp/mh
  { echo MANGOHUD=1; echo "MANGOHUD_CONFIG=${MHCFG}output_folder=/tmp/mh,autostart_log=3,log_duration=$((warm+win-12))"; } > deck.env
  wrote_env=1
fi
if [ -n "$EXTRA_ENV" ]; then
  [ -z "$wrote_env" ] && [ -e deck.env ] && { echo "deck.env exists already: refusing"; exit 4; }
  printf '%s
' "$EXTRA_ENV" >> deck.env
  wrote_env=1
fi
printf '%s ' "$@" > try.flags; echo >> try.flags
cleanup() {
  p=$(pgrep -f "$exe_re"); [ -n "$p" ] && kill $p 2>/dev/null
  sleep 3; p=$(pgrep -f "$exe_re"); [ -n "$p" ] && kill -9 $p 2>/dev/null
  rm -f "$dir/try.flags"
  [ -n "$wrote_env" ] && rm -f "$dir/deck.env"
  [ -n "$waker" ] && kill $waker 2>/dev/null
}
trap cleanup EXIT
XA=$(ls /run/user/1000/xauth_* 2>/dev/null | head -1)
( export DISPLAY=:0 XAUTHORITY=$XA; for i in $(seq 1 $(( (warm+win)/20 + 3 ))); do xdotool key shift; sleep 20; done ) >/dev/null 2>&1 &
waker=$!
gameid=$(( (appid << 32) | 0x02000000 ))
started=$(date +%s)
DISPLAY=:0 nohup steam steam://rungameid/$gameid >/dev/null 2>&1 &
sleep "$warm"
dev=/sys/class/drm/card0/device; hw=$(ls -d $dev/hwmon/hwmon* | head -1)
read -r _ a b c d e f g h _ < /proc/stat; t0=$((a+b+c+d+e+f+g+h)); i0=$((d+e))
pids=$(pgrep -f "$exe_re" | tr '\n' ' ')
pc0=0; for p in $pids; do read -r -a st < /proc/$p/stat 2>/dev/null && pc0=$((pc0 + st[13] + st[14])); done
busy=0; freq=0; pw=0; n=0
for s in $(seq 1 "$win"); do
  sleep 1
  busy=$((busy + $(cat $dev/gpu_busy_percent))); freq=$((freq + $(cat $hw/freq1_input)/1000000))
  pw=$((pw + $(cat $hw/power1_average 2>/dev/null || cat $hw/power1_input)/1000)); n=$((n+1))
  [ $s -eq $((win/2)) ] && GAMESCOPE_WAYLAND_DISPLAY=gamescope-0 gamescopectl screenshot /tmp/vk-$label.png >/dev/null 2>&1
done
read -r _ a b c d e f g h _ < /proc/stat; t1=$((a+b+c+d+e+f+g+h)); i1=$((d+e))
pc1=0; for p in $pids; do read -r -a st < /proc/$p/stat 2>/dev/null && pc1=$((pc1 + st[13] + st[14])); done
hz=$(getconf CLK_TCK)
echo "RESULT $label gpu_busy=$((busy/n))% sclk=$((freq/n))MHz soc_power=$((pw/n))mW cpu_total=$(( 100*((t1-t0)-(i1-i0))/(t1-t0) ))% game_cpu=$(( 100*(pc1-pc0)/hz/win ))%core pids=[$pids]"
if [ -n "$MH" ]; then
  for k in $(seq 1 30); do ls /tmp/mh/*.csv >/dev/null 2>&1 && break; sleep 1; done
  sleep 1
  f=$(ls -t /tmp/mh/*.csv 2>/dev/null | grep -v summary | head -1)
  echo "MANGOHUD $f"
  [ -n "$f" ] && python3 - "$f" "$win" <<'PY'
import sys, csv
rows = list(csv.reader(open(sys.argv[1])))
i = next(k for k, r in enumerate(rows) if r and r[0] == 'fps')
hdr = rows[i]; data = [dict(zip(hdr, r)) for r in rows[i+1:] if len(r) == len(hdr)]
WIN = float(sys.argv[2]) * 1e9
end = float(data[-1]['elapsed']); data = [d for d in data if float(d['elapsed']) >= end - WIN]
ft = sorted(float(d['frametime']) for d in data)
n = len(ft); avg = sum(ft)/n
low1 = ft[int(n*0.99)] if n else 0
el = (float(data[-1]['elapsed']) - float(data[0]['elapsed'])) / 1e9 if n > 1 else 0
print("MH frames=%d over %.1fs -> %.1f FPS, frametime avg %.2f ms, 99th pct %.2f ms, max %.2f ms, gpu_load avg %.0f%%, cpu_load avg %.0f%%, gpu_power avg %.1f W" % (
  n, el, (n-1)/el if el else 0, avg, low1, ft[-1], sum(float(d['gpu_load']) for d in data)/n,
  sum(float(d['cpu_load']) for d in data)/n, sum(float(d.get('gpu_power') or 0) for d in data)/n))
PY
fi
running=no; pgrep -f "$exe_re" >/dev/null && running=yes
echo "RUNNING $running"
cd "${LOGDIR:-$dir}"
l=$(find logs -maxdepth 1 -name "*.log" -newermt "@$started" -printf "%T@ %p\n" 2>/dev/null | sort -n | tail -1 | cut -d" " -f2-)
echo "LOG $l"
[ -n "$l" ] && grep -E "frames [0-9]+ \| per frame|classic|Xenos driver|vulkan|Vulkan|driver|error|FATAL|crash" "$l" | grep -v "^\s*$" | tail -40
