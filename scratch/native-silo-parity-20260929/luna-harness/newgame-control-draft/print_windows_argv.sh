#!/bin/bash
# Inert planner: prints the exact Silo argv for a cloned runtime; never launches.
set -euo pipefail
mode="${PARITY_START_MODE:-newgame}"
rt="${PARITY_RUNTIME:?Set PARITY_RUNTIME to the isolated cloned runtime}"
winexe="$rt/bin/AnomalyDX11.exe"
case "$mode" in
  newgame)
    # Deliberately omit both -load and -start. The cloned ui_main_menu overlay
    # selects rookie_village and calls the normal faction form's OnStartGame().
    printf '%q ' "$winexe" -fsltx "Z:${rt//\//\\}\\fsgame.ltx" -dbg -nointro -skiplogo
    printf '\n'
    ;;
  load)
    name="${PARITY_LOAD_NAME:?Set PARITY_LOAD_NAME to a save stem that exists in this clone}"
    # This is the source/log-evidenced ALife load route; bare -load starts too early.
    printf '%q ' "$winexe" -fsltx "Z:${rt//\//\\}\\fsgame.ltx" -dbg -nointro -skiplogo -start "server($name/single/alife/load) client(localhost)"
    printf '\n'
    ;;
  *) echo 'PARITY_START_MODE must be newgame or load' >&2; exit 2 ;;
esac
