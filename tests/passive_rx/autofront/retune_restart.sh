#!/bin/bash
# retune_restart.sh RUN.LOG [N_RB|auto]   (default auto)
# OTA-chain restart step (run by the orchestrator, no RF here). After run_once the log holds the SIB1 carrier
# (ISAC_ACQ_RETUNE / 'ACQ carrier MISMATCH' + KSSB_GRID). This prints the receiver args that put the grid on the cell's
# carrier, to replace "-C .. -r .. --ssb .." of the first launch:    -C <hz> -r <n_rb> --ssb <sc>
# Env PID=<old nr-uesoftmodem pid>: SIGINT it first (never SIGKILL: X410 claim hygiene) and wait up to 60 s for exit.
# Exit 0 args printed, 3 geometry already matches the carrier (do not restart), 4/5 see retune_args.py.
# Scope: n78, 30 kHz; sizes with a sample rate in get_samplerate_and_bw: 51 106 133 162 189 217 273 (auto = smallest >= carrier).
set -euo pipefail
H=$(cd "$(dirname "$0")" && pwd); LOG=$1; RB=${2:-auto}
if ! grep -aq 'ISAC_ACQ_RETUNE\|ACQ carrier MISMATCH' "$LOG"; then echo "no carrier mismatch in $LOG: keep current geometry" >&2; exit 3; fi
out=$(python3 -I "$H/../survey/retune_args.py" "$LOG" "$RB") || exit $?
echo "$out" >&2
[ -n "${PID:-}" ] && { kill -INT "$PID" 2>/dev/null || true; for _ in $(seq 60); do kill -0 "$PID" 2>/dev/null || break; sleep 1; done
  kill -0 "$PID" 2>/dev/null && { echo "old receiver $PID still alive after 60 s SIGINT" >&2; exit 6; }; }
[[ $out =~ C=([0-9]+)\ R=([0-9]+)\ SSB=(-?[0-9]+) ]] && echo "-C ${BASH_REMATCH[1]} -r ${BASH_REMATCH[2]} --ssb ${BASH_REMATCH[3]}"
