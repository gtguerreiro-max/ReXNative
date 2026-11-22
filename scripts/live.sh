#!/usr/bin/env bash
# scripts/live.ps1 on Linux and Proton (the Steam Deck): the same commands and output; see live.py.
exec python3 "$(dirname "$(readlink -f "$0")")/live.py" "$@"
