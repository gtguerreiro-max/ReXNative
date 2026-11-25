#!/bin/bash
# ws.sh <seconds>: every wineserver's CPU (% of one core) over the seconds
hz=$(getconf CLK_TCK)
sum() { t=0; for p in $(pgrep -x wineserver); do v=$(awk '{print $14+$15}' /proc/$p/stat 2>/dev/null); t=$((t + ${v:-0})); done; echo $t; }
a=$(sum); sleep "$1"; b=$(sum)
echo "wineserver $(( (b-a) * 100 / (hz * $1) ))% of one core ($(pgrep -x wineserver | wc -l) processes)"
