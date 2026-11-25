#!/bin/bash
# summary.sh: the newest game log's frames over 50 ms, mean frame time (last 12 windows), step log
l=$(ls -t ~/Games/ReXNative/*/logs/*.log | head -1)
over=$(grep -o "[0-9]* frames over 50" "$l" | awk '{s+=$1} END {print s+0}')
echo "hitch lines $(grep -c 'hitches:' "$l"), frames over 50 ms $over"
grep -o "frame [0-9.]* ms, game thread" "$l" | tail -12 | awk '{f+=$2; n++} END {if (n) printf "mean frame %.2f ms (last %d windows)\n", f/n, n}'
grep -h "step gate" "$l" | tail -3 | cut -c38-200
