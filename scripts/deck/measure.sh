#!/bin/bash
# Measures a game on the Steam Deck once per variant: started by deck_drive.py with the variant's
# flags, driven by the keys given, and <wait> seconds after its process appears, 25 s of: its CPU
# per thread, the APU's power, the GPU's busy share and clocks (sample.py), every wineserver's CPU
# (ws.sh); after the run the log's long frames, mean frame time and step gate lines (summary.sh).
#
#   bash scripts/deck/measure.sh <work>/<game> <exe name> "<at>:<key>,..." <wait s> "label:flags"...
#   bash scripts/deck/measure.sh D:/work/forza-horizon forzahorizon "42:Return,50:space,58:space" 75 \
#       "off:--runtime.yield_backoff_us=0" "on:--runtime.yield_backoff_us=200"
#
# The Deck and its key as for deck_drive.py: REXNATIVE_DECK (user@host:port), REXNATIVE_DECK_KEY.
# The tools go to ~/.cache/rexnative-tests on the Deck; `bash scripts/deck/measure.sh --clean`
# removes them. Python: $PYTHON, else python. Other tools there: states.py <exe> <s> (each busy
# thread's context switches and where it waits), survey.py ~/Games/ReXNative (every game log's
# windows below the game's frame rate).
set -u
here=$(cd "$(dirname "$0")" && pwd)
py=${PYTHON:-python}
deck=${REXNATIVE_DECK:?set REXNATIVE_DECK to user@host:port}
host=${deck%:*}
port=${deck##*:}
[ "$port" = "$deck" ] && port=22
ssh_args=(-p "$port" -o BatchMode=yes -o ConnectTimeout=10)
[ -n "${REXNATIVE_DECK_KEY:-}" ] && ssh_args+=(-i "$REXNATIVE_DECK_KEY")
D() { ssh "${ssh_args[@]}" "$host" "$@"; }

if [ "${1:-}" = "--clean" ]; then
  D 'rm -rf ~/.cache/rexnative-tests && echo removed'
  exit 0
fi
work=$1 exe=$2 keys=$3 wait=$4
shift 4
for f in sample.py states.py survey.py ws.sh summary.sh; do
  D "mkdir -p ~/.cache/rexnative-tests && cat > ~/.cache/rexnative-tests/$f" < "$here/$f"
done
for v in "$@"; do
  label=${v%%:*} flags=${v#*:}
  echo "=== $label ($flags)"
  # shellcheck disable=SC2086
  "$py" -u "$here/../deck_drive.py" "$work" "$label" "$keys" $((wait + 60)) $flags \
    > "${TMPDIR:-/tmp}/measure-$label.txt" 2>&1 &
  dd=$!
  t0=$(date +%s)
  until D "pgrep -f '^Z:.*$exe\.exe --log' >/dev/null" || [ $(($(date +%s) - t0)) -gt 180 ]; do
    sleep 5
  done
  t1=$(date +%s)
  until [ $(($(date +%s) - t1)) -ge "$wait" ]; do sleep 2; done
  D "python3 ~/.cache/rexnative-tests/sample.py $exe 25 & bash ~/.cache/rexnative-tests/ws.sh 25; wait"
  wait $dd
  D "bash ~/.cache/rexnative-tests/summary.sh"
done
