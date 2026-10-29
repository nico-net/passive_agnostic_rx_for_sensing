#!/bin/bash
# mpci_replay.sh -- plan task G7 / gate V3: 2-cell replay on the DGX in ONE nr-uesoftmodem process, solo/pair interleaved, per-cell P1/P3 verdict.
#
# README
#   What:   the stored two-operator capture ota_runs/newcells_tiled/l0942_c3610x8 (4 x rx*.sc16 + capture_info.json; 122.88 MS/s, centre 3610 MHz,
#           8 s; WindTre PCI 508 + Iliad PCI 403; READ-ONLY) is published at real time by g5_fast_pub (it loops the file, a new RF epoch per
#           loop, as mpci_file_pub) into the ring "g5wide"; the receiver reads it through the shm device ("shm" with loader shlibversion "_iq" =
#           libshm_iq.so, the test-bed trick of g5_wide_replay.sh). Arms (all with --ue-nb-ant-rx 4, same cfg, same env file, same command-line shape):
#             pair     cells.json = {508, 403}; cell 0 (command-line tuning) = PAIR_FIRST (default 508): wideband path (planner -> in-process
#                      channelizer -> one cell ring per cell), ONE process, one CUDA context
#             solo508  cells.json = {508}, command line = 508     solo403  cells.json = {403}, command line = 403
#           SOLO CAVEAT (code, executables/nr-uesoftmodem.c "N = 1 keeps the direct radio path (no channelizer hop, no G6 code runs)"): with one cell
#           the receiver never builds the channelizer, so a solo arm reads the same ring through the shm device's own per-cell DDC (shift to the cell,
#           decimation to its rate). That IS the N=1 baseline of Global Constraint 5; the pair-vs-solo difference therefore includes the channelizer
#           (cost and risk), which is what P3 is meant to charge. It is NOT "only the cell count differs".
#   Tunings (receiver-side detections only; no gNB log/config is an input):
#           508: -C 3590010000 --ssb 193 -r 162 --initial-fo -4868     403: -C 3630000000 --ssb 18 -r 51 --initial-fo 2178
#           = tests/passive_rx/mpci/inventory/live_tuned_20261023.json (its _comment, PROJECT_MEMORY section 25 block 2026-10-23/24 item 4,
#           [MEASURED, DGX OTA 2026-10-23]); centre_hz / n_rb_hint / cfo_hz are asserted equal to the inventory entries when cells.json is written.
#           The survey of this very capture (PROJECT_MEMORY section 25 item 25: 508 GSCN 7895 / 3570.24 MHz cfo -4761, 403 GSCN 7933 / 3624.96 MHz
#           cfo +2225) agrees on GSCN/SSB frequency and differs in CFO by 107 / 47 Hz (inside the tracker pull-in). cells.json window = capture_info.json
#           (3610 MHz, 122.88 MS/s, 4 rx). The planner picks its own capture centre from the cells (about 3600 MHz for 508+403); the shm device
#           re-centres the ring (3610) by its DDC shift.
#   Order:  interleaved, N rounds (default 3) of the arms, round r rotated by r: pair solo508 solo403 | solo403 pair solo508 | solo508 solo403 pair.
#   Per run: flock -x /tmp/td_measure.lock, nice 10, timeout --signal=INT -k 60 DUR (SIGINT stop), pgrep -u $USER -x only (another user's docker UE
#           matches a bare pgrep), CUDA_MPS_PIPE_DIRECTORY=/tmp/nicola_nomps, ISAC_MLOCK=0, MemAvailable >= 24 GB, /usr/bin/time -v around the receiver
#           (user+sys CPU s = busy cores for P3), nvidia-smi sampler (gpu.tsv).
#   Files per run dir $OUT/NN_<arm>/: cells.json arm.txt replay.cfg pub.log rx.log time.txt gpu.tsv summary.json; $OUT/verdict.{txt,json}.
#   Scoring: mpci_replay_score.py (goal_metrics per cell, never pooled). P1 per cell vs its own solo arm (medians over the runs): DL p50 <= 1.2x,
#           DL p99 <= 1.5x, crc_q >= 95 % of solo, drops <= 4 % and <= solo + 1 point, UL locked p50 <= 1.2x when the solo arm has UL; P3 busy cores
#           (user+sys / wall) of the pair <= 1.25x the heavier solo; GPU check_bad == 0 in every run; 0 channelizer overruns (chan_overruns,
#           SHMIQ_OVERRUN, IQ_CHAN_OVERRUN) in the pair runs. A FAIL names the binding resource (channelizer / cores / discovery / gpu) from the
#           MPCI_FEED, CORE_POOL, DISC_TICKET lines (heuristic thresholds, [INFERRED]). Evidence label of the verdict:
#           [MEASURED, DGX replay c3610 l0942_c3610x8, cell PCI 508|403, n=N, @<commit>] per cell.
#   Time:   one run = DUR 240 s + start (publisher ring ~20-60 s, receiver start ~20 s) + stop ~10 s + scoring ~10-60 s = about 5.5 min; N=3 (9 runs) about 50 min.
#   usage:  mpci_replay.sh [--dry-run] [name]       env: B=<build dir> N=3 DUR=240 SPEED=1 PAIR_FIRST=508 CAP=<capture dir> ENVFILE=.. CFG=.. OUT=..
#                                                        ARMS="pair solo508 solo403" (a subset for a smoke run)
#           mpci_replay.sh --score <out dir>          re-score an existing campaign (no receiver)
#   Host rules: orchestrator only; never while a receiver of ours runs, never while a build/ctest holds the shared lock for long; no new receiver
#   env var / CLI option / -D switch (NO FLAGS): everything above is harness-side. sens6-frozen paths are not touched; the capture is never written.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../../.." && pwd); O=/home/nicola/NICOLA/wt/rr-orchestration
MP=$ROOT/tests/passive_rx/mpci; SC=$MP/mpci_replay_score.py
DRY=0
if [ "${1:-}" = --dry-run ]; then DRY=1; shift; fi
if [ "${1:-}" = --score ]; then python3 -I "$SC" score "${2:?out dir}"/*_*/ && python3 -I "$SC" verdict "$2"; exit $?; fi
name=${1:-mpci_replay}
B=${B:-$ROOT/cmake_targets/ran_build/build}; CFG=${CFG:-$O/ota/sd_t26ul.cfg}; ENVFILE=${ENVFILE:-$O/ota/MPCI_20261025.env}
CAP=${CAP:-/home/nicola/NICOLA/ota_runs/newcells_tiled/l0942_c3610x8}
INV=$MP/inventory/live_tuned_20261023.json
N=${N:-3}; DUR=${DUR:-240}; SPEED=${SPEED:-1}; PAIR_FIRST=${PAIR_FIRST:-508}; ARMS=${ARMS:-pair solo508 solo403}
[ "$PAIR_FIRST" = 508 ] && PAIR_SECOND=403 || PAIR_SECOND=508
OUT=${OUT:-$ROOT/replays/${name}_$(date +%Y%m%d_%H%M%S)}
RING=g5wide
say() { printf '%s\n' "$*"; }
pyc() { python3 -I "$SC" "$@"; }

[ -f "$CAP/capture_info.json" ] || { echo "no capture at $CAP" >&2; exit 2; }
[ -f "$ENVFILE" ] || { echo "no env file $ENVFILE" >&2; exit 2; }
PUBSRC=$(pyc src "$RING" "$CAP")
read -r -a envs < "$ENVFILE"; base=()
for kv in "${envs[@]}"; do case "$kv" in ISAC_USRP_*|ISAC_UHD_*|ISAC_RX_RECOVER*|ISAC_*_CORES=*|ISAC_*_CORE=*) ;; *) base+=("$kv") ;; esac; done
read -r -a SEQ <<< "$(pyc order "$N" $ARMS)"
say "# mpci_replay: OUT=$OUT  capture=$CAP  B=$B  N=$N DUR=$DUR SPEED=$SPEED pair cell0=$PAIR_FIRST  env=$ENVFILE (${#base[@]} vars after the radio/core filter)"
say "# sequence (${#SEQ[@]} runs): ${SEQ[*]}"
[ $DRY = 1 ] && say "# DRY RUN: nothing is started, nothing is written; the python helpers below are read-only"

run_one() { # <index> <arm>
  local idx=$1 arm=$2 pcis R
  case $arm in pair) pcis="$PAIR_FIRST $PAIR_SECOND" ;; solo508) pcis=508 ;; solo403) pcis=403 ;; *) echo "bad arm $arm" >&2; exit 2 ;; esac
  R=$OUT/$(printf '%02d' "$idx")_$arm
  local cli; cli=$(pyc cli "${pcis%% *}")
  say ""; say "## run $idx/${#SEQ[@]} arm=$arm cells=[$pcis] dir=$R"
  say "+ python3 -I mpci_replay_score.py cells $INV $CAP $pcis   > $R/cells.json   # window from capture_info.json; started cell first"
  if [ $DRY = 1 ]; then pyc cells "$INV" "$CAP" $pcis | sed 's/^/#   /'; else mkdir -p "$R"; pyc cells "$INV" "$CAP" $pcis > "$R/cells.json"; echo "$arm" > "$R/arm.txt"; fi
  local dev='device = { name = "shm"; };' ldr="loader = { shm = { shlibpath = \"$B\"; shlibversion = \"_iq\"; }; shm_iq = { shlibpath = \"$B\"; }; };"
  say "+ { cat $CFG; echo '$dev'; echo '$ldr'; } > $R/replay.cfg"
  [ $DRY = 1 ] || { cat "$CFG"; echo "$dev"; echo "$ldr"; } > "$R/replay.cfg"
  local nrb ssb centre fo
  centre=$(awk '{print $2}' <<< "$cli"); ssb=$(awk '{print $4}' <<< "$cli"); nrb=$(awk '{print $6}' <<< "$cli"); fo=$(awk '{print $8}' <<< "$cli")
  local rxcmd=(env ISAC_MLOCK=0 "LD_LIBRARY_PATH=$B:/usr/local/lib" "${base[@]}"
    /usr/bin/time -v -o "$R/time.txt" timeout --signal=INT -k 60 "$DUR" nice -n 10 "$B/nr-uesoftmodem" -O "$R/replay.cfg" --passive-rx -r "$nrb" --numerology 1 --band 78
    -C "$centre" --ssb "$ssb" --ue-rxgain 38 --ue-nb-ant-rx 4 --ue-nb-ant-tx 4 --initial-fo "$fo" --ue-fo-compensation
    --usrp-args "$RING" --thread-pool -1 --time-sync-I 0.01 --ntn-initial-time-drift 0 -A 90)
  local pubcmd=(nice -n 10 "$B/g5_fast_pub" "$SPEED" "$((DUR + 90))" "$PUBSRC")
  if [ $DRY = 1 ]; then
    say "+ exec 9>/tmp/td_measure.lock; flock -x 9          # held until this run has stopped"
    say "+ [ \"\$(pgrep -u \$(id -un) -x nr-uesoftmodem)\" ] && exit 3 ; pgrep -u \$(id -un) -x g5_fast_pub && exit 3 ; MemAvailable >= 24 GB else exit 4"
    say "+ CUDA_MPS_PIPE_DIRECTORY=/tmp/nicola_nomps CUDA_MPS_LOG_DIRECTORY=/tmp/nicola_nomps ${pubcmd[*]} > $R/pub.log 2>&1 &   # waits for 'MPCI_PUB ring=' (<= 120 s)"
    say "+ ( while sleep 10; do nvidia-smi --query-gpu=utilization.gpu,clocks.sm,power.draw --format=csv,noheader; done ) > $R/gpu.tsv &"
    local shown="${rxcmd[*]}"
    [ "$idx" -gt 1 ] && shown="${shown//${base[*]}/<the ${#base[@]} ENVFILE vars of run 1>}"
    say "+ cd $R && $shown < /dev/null > $R/rx.log 2>&1      # SIGINT stop after DUR via timeout"
    say "+ kill -INT <publisher>; wait; release the lock"
    say "+ python3 -I mpci_replay_score.py score $R     # summary.json + one line"
    return 0
  fi
  (
    exec 9>/tmp/td_measure.lock; flock -x 9
    if pgrep -u "$(id -un)" -x nr-uesoftmodem >/dev/null || pgrep -u "$(id -un)" -x g5_fast_pub >/dev/null; then echo "a receiver/publisher of ours is running: not starting" >&2; exit 3; fi
    [ "$(awk '/MemAvailable/{print int($2/1048576)}' /proc/meminfo)" -ge 24 ] || { echo "MemAvailable < 24 GB: not starting" >&2; exit 4; }
    export CUDA_MPS_PIPE_DIRECTORY=/tmp/nicola_nomps CUDA_MPS_LOG_DIRECTORY=/tmp/nicola_nomps
    "${pubcmd[@]}" 9>&- > "$R/pub.log" 2>&1 &
    PUB=$!
    trap 'kill -INT $PUB 2>/dev/null || true' EXIT
    ok=0; for _ in $(seq 1 600); do grep -q "^MPCI_PUB ring=" "$R/pub.log" 2>/dev/null && { ok=1; break; }; sleep 0.2; done
    [ $ok = 1 ] || { echo "publisher not ready after 120 s" >&2; exit 5; }
    cd "$R"; set +e
    ( while sleep 10; do nvidia-smi --query-gpu=utilization.gpu,clocks.sm,power.draw --format=csv,noheader; done ) > "$R/gpu.tsv" 2>/dev/null 9>&- &
    SMI=$!
    "${rxcmd[@]}" < /dev/null 9>&- > "$R/rx.log" 2>&1
    rc=$?
    kill $SMI 2>/dev/null; kill -INT $PUB 2>/dev/null; wait $PUB 2>/dev/null
    echo "rc=$rc" > "$R/rc.txt"; echo "rc=$rc run=$R"
  )
  pyc score "$R" || true
}

i=0
for arm in "${SEQ[@]}"; do i=$((i + 1)); run_one "$i" "$arm"; done
if [ $DRY = 1 ]; then say ""; say "+ python3 -I mpci_replay_score.py verdict $OUT   # verdict.txt / verdict.json"; exit 0; fi
pyc verdict "$OUT"
say "verdict: $OUT/verdict.txt"
