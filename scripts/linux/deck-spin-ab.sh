#!/bin/bash
# A test build on the Steam Deck (a native Linux build, or a Windows build under Proton), started
# from the installed game's Steam shortcut through proton-shim, the spin sleep (spin_pause.h) off
# and on alternated, two rounds:
#   deck-spin-ab.sh <installed dir> <test dir> <appid> <process regex> <warm> <window> [label]
# Run it on the Deck, beside deck-measure.sh. Passed on to deck-measure.sh: HEAVY, WAIT_DRAWS,
# SETTLE, PRESS (CONTROL_FILE is the test dir's control.txt, which the test dir's try.flags must
# name; Overlord II's "Press Start" does not take the control file's pad: a person presses it).
# FRESH_USERDATA=1 removes the test dir's userdata before each run, so each starts the same way.
# AB=latency alternates ReXNative's frame latency 2 and 1 instead, the sleep on: the test dir's
# try.flags gets --rexnative_frame_latency=<n> for each run, and is put back as it was after.
G=$1; T=$2; APP=$3; RE=$4; warm=$5; win=$6; label=${7:-$(basename $T)}
# The test dir's kind: a native Linux build (run-linux.sh) or a Windows one.
if [ -x "$T/run-linux.sh" ]; then B="REXNATIVE_LINUX_BUILD=$T"; else B="REXNATIVE_WIN_BUILD=$T"; fi
show() { grep -E "WAIT|RESULT|^MH|^FRAMES|^SHOWN|^GPU|BUSY|refus|NOT" /tmp/s_run.txt; grep -A2 THREADS /tmp/s_run.txt | tail -2; }
modes="off on"
if [ "${AB:-spin}" = latency ]; then
  modes="2 1"
  saved=$(mktemp); cp "$T/try.flags" "$saved" 2>/dev/null || : > "$saved"
  trap 'cp "$saved" "$T/try.flags"; [ -s "$saved" ] || rm -f "$T/try.flags"; rm -f "$saved"' EXIT
fi
for round in 1 2; do
  for mode in $modes; do
    if [ "${AB:-spin}" = latency ]; then
      v=1; what="frame latency $mode"
      printf '%s --rexnative_frame_latency=%s\n' "$(tr -d '\n' < "$saved")" "$mode" > "$T/try.flags"
    else
      v=1; [ $mode = off ] && v=0; what="spin-sleep $mode"
    fi
    [ -n "$FRESH_USERDATA" ] && rm -rf "$T/userdata"
    echo "=== $label $what ($round)"
    MH=1 MHCFG= CONTROL_FILE=$T/control.txt LOGDIR=$T EXTRA_ENV="REXNATIVE_PROTON=$HOME/Games/ReXNative-vulkan-tests/proton-shim
$B
REXNATIVE_SPIN_SLEEP=$v" $(dirname "$0")/deck-measure.sh $G "$RE" $APP $label-$mode-$round $warm $win > /tmp/s_run.txt 2>&1; show
    rm -f $T/control.txt; sleep 10
  done
done
ls $G | grep -E "deck.env|try.flags" || echo "clean: no deck.env/try.flags left"
