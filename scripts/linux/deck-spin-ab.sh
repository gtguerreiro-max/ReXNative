#!/bin/bash
# A test build on the Steam Deck (a native Linux build, or a Windows build under Proton), started
# from the installed game's Steam shortcut through proton-shim, the spin sleep (spin_pause.h) off
# and on alternated, two rounds:
#   deck-spin-ab.sh <installed dir> <test dir> <appid> <process regex> <warm> <window> [label]
# Run it on the Deck, beside deck-measure.sh. Passed on to deck-measure.sh: HEAVY, WAIT_DRAWS,
# SETTLE, PRESS (CONTROL_FILE is the test dir's control.txt, which the test dir's try.flags must
# name; Overlord II's "Press Start" does not take the control file's pad: a person presses it).
# FRESH_USERDATA=1 removes the test dir's userdata before each run, so each starts the same way.
G=$1; T=$2; APP=$3; RE=$4; warm=$5; win=$6; label=${7:-$(basename $T)}
# The test dir's kind: a native Linux build (run-linux.sh) or a Windows one.
if [ -x "$T/run-linux.sh" ]; then B="REXNATIVE_LINUX_BUILD=$T"; else B="REXNATIVE_WIN_BUILD=$T"; fi
show() { grep -E "WAIT|RESULT|^MH|^FRAMES|^GPU|BUSY|refus|NOT" /tmp/s_run.txt; grep -A2 THREADS /tmp/s_run.txt | tail -2; }
for round in 1 2; do
  for mode in off on; do
    v=1; [ $mode = off ] && v=0
    [ -n "$FRESH_USERDATA" ] && rm -rf "$T/userdata"
    echo "=== $label spin-sleep $mode ($round)"
    MH=1 MHCFG= CONTROL_FILE=$T/control.txt LOGDIR=$T EXTRA_ENV="REXNATIVE_PROTON=$HOME/Games/ReXNative-vulkan-tests/proton-shim
$B
REXNATIVE_SPIN_SLEEP=$v" $(dirname "$0")/deck-measure.sh $G "$RE" $APP $label-$mode-$round $warm $win > /tmp/s_run.txt 2>&1; show
    rm -f $T/control.txt; sleep 10
  done
done
ls $G | grep -E "deck.env|try.flags" || echo "clean: no deck.env/try.flags left"
