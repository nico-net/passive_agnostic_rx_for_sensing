#!/bin/bash
# n1_equiv.sh: multi-PCI Task A9, N=1 equivalence suite (candidate build vs reference build, same host class). Runs on every PR and after every merge
# of adaptive-rx-UL-DL into mpci-main (plan F10), together with gate V2a. NOTHING here is run by the agent that wrote it: the orchestrator runs it when the host is free.
#
#   n1_equiv.sh unit    [--build DIR] [--dry-run]
#       module / interleave unit tests with the single cell of the test (ctest -R, candidate build dir) + the python tests of this suite and of the scorers.
#   n1_equiv.sh replay  --ref-bin DIR --cand-bin DIR [--capture DIR] [--n 2] [--dur 240] [--speed 0.25] [--out DIR] [--ref-json F] [--dry-run]
#       solo (N=1) replay of one stored capture (default Vodafone PCI 415, as run_two_cell_replay.sh with its second cell removed), arms interleaved
#       ABBA-style (r1 ref,cand; r2 cand,ref; ...), n runs per arm, shared td_measure lock per run. Verdict table: <out>/verdict_replay.txt
#   n1_equiv.sh rfsim   --ref-bin DIR --cand-bin DIR [--n 2] [--secs 420] [--out DIR] [--ref-json F] [--dry-run]
#       the 4-RX rfsim gate (dgx/rfsim_regress.sh, 106 PRB, merged-main defaults, 420 s) once per run and arm, interleaved, each under flock -x /tmp/td_measure.lock,
#       idle host required. Gate result of each run (gate_rc) is part of the comparison. Verdict table: <out>/verdict_rfsim.txt
#   n1_equiv.sh store   --bed replay|rfsim -o FILE RUNDIR...      store the parsed runs of a reference build as a reference JSON (host class recorded)
#   n1_equiv.sh verdict --bed replay|rfsim --ref PATH... --cand PATH... [--out PREFIX]    compare existing run directories / reference JSON
#
# DIR = a build directory holding nr-uesoftmodem (+ nr-softmodem, mpci_file_pub, libshm_iq.so for the arms that need them). The reference is a build of
# adaptive-rx-UL-DL (main) or mpci-main at the commit the candidate was merged from; the candidate is the build under test. Both on THIS host class
# (HOST_CLASS=<name> overrides the default "$(uname -m)-<nproc>cpu"); a reference JSON of another host class is refused (exit 3).
# Exit: 0 equivalent, 1 drift (or a failed unit test), 3 refused / insufficient data / host busy. Rules: our receivers only (pgrep -u), SIGINT stops, nice 10, no flags in the receiver.
set -uo pipefail
ROOT=$(cd "$(dirname "$0")/../../.." && pwd); HERE=$ROOT/tests/passive_rx/mpci; O=${ORCH:-/home/nicola/NICOLA/wt/rr-orchestration}
HOST_CLASS=${HOST_CLASS:-$(uname -m)-$(nproc)cpu}
PY=(python3 -W ignore)
cmd=${1:-}; [ $# -gt 0 ] && shift
DRY=0; N=2; DUR=240; SECS=420; SPEED=0.25; OUT=""; REFB=""; CANDB=""; REFJ=""; BUILD="$ROOT/cmake_targets/ran_build/build"; BED=""; STORE_O=""
CAP=${CAP415:-/home/nicola/NICOLA/ota_runs/raw_pci415_12s_b}
CFG=${CFG:-$O/ota/sd_t26ul.cfg}; ENVFILE=${ENVFILE:-$O/ota/FROZEN_20261018.env}
POS=(); VREF=(); VCAND=(); mode=""
while [ $# -gt 0 ]; do
  case $1 in
    --dry-run) DRY=1 ;; --n) N=$2; shift ;; --dur) DUR=$2; shift ;; --secs) SECS=$2; shift ;; --speed) SPEED=$2; shift ;;
    --out) OUT=$2; shift ;; --ref-bin) REFB=$2; shift ;; --cand-bin) CANDB=$2; shift ;; --ref-json) REFJ=$2; shift ;;
    --build) BUILD=$2; shift ;; --capture) CAP=$2; shift ;; --bed) BED=$2; shift ;; -o) STORE_O=$2; shift ;;
    --ref) mode=ref ;; --cand) mode=cand ;;
    -*) echo "unknown option $1" >&2; exit 2 ;;
    *) case $mode in ref) VREF+=("$1") ;; cand) VCAND+=("$1") ;; *) POS+=("$1") ;; esac ;;
  esac; shift
done

say() { echo "[n1_equiv] $*"; }
run() { # echo in dry-run, else execute
  if [ $DRY = 1 ]; then printf '  + '; printf '%q ' "$@"; echo; else "$@"; fi
}
host_free() { # refuse when one of OUR receivers / gNBs runs (another user's docker UE is not ours)
  [ $DRY = 1 ] && return 0
  if pgrep -u "$(id -un)" -x nr-uesoftmodem >/dev/null || pgrep -u "$(id -un)" -x nr-softmodem >/dev/null; then echo "a receiver of ours is running: not starting" >&2; exit 3; fi
}
need_bins() {
  [ -n "$REFB" ] || [ -n "$REFJ" ] || { echo "--ref-bin (or --ref-json) required" >&2; exit 2; }
  [ -n "$CANDB" ] || { echo "--cand-bin required" >&2; exit 2; }
  [ $DRY = 1 ] && return 0
  for b in "$CANDB" ${REFB:+"$REFB"}; do [ -x "$b/nr-uesoftmodem" ] || { echo "$b/nr-uesoftmodem missing" >&2; exit 2; }; done
}
order() { # <i> -> "ref cand" for odd runs, "cand ref" for even (ABBA drift cancellation)
  if [ $(( $1 % 2 )) = 1 ]; then echo "ref cand"; else echo "cand ref"; fi
}
bin_of() { if [ "$1" = ref ]; then echo "$REFB"; else echo "$CANDB"; fi; }
verdict() { # <bed> <outprefix> <ref args...> -- <cand args...>
  local bed=$1 pre=$2; shift 2; local refs=() cands=() side=ref a
  for a in "$@"; do if [ "$a" = -- ]; then side=cand; elif [ $side = ref ]; then refs+=("$a"); else cands+=("$a"); fi; done
  run "${PY[@]}" "$HERE/n1_equiv.py" compare --bed "$bed" --host-class "$HOST_CLASS" --out "$pre" --ref "${refs[@]}" --cand "${cands[@]}"
}
skip_ref() { [ "$1" = ref ] && [ -n "$REFJ" ] && [ -z "$REFB" ]; }

case $cmd in
unit)
  # tests matching mpci|interleave|cell (+ the A10/B6/audit modules) with the single cell of the test, then the python suites
  PAT='mpci|interleave|cell|sync_pin|admit|core_pool|core_plan|static_audit'
  say "unit level: ctest in $BUILD (-R '$PAT'), python tests"
  rc=0
  run ctest --test-dir "$BUILD" -R "$PAT" --no-tests=error --output-on-failure -j2 --timeout 600 || rc=1
  for t in test_n1_equiv.py test_percell_scorers.py test_admit_model.py; do run "${PY[@]}" "$HERE/$t" || rc=1; done
  exit $rc ;;

store)
  [ -n "$BED" ] && [ -n "$STORE_O" ] && [ ${#POS[@]} -gt 0 ] || { echo "usage: store --bed replay|rfsim -o FILE RUNDIR..." >&2; exit 2; }
  run "${PY[@]}" "$HERE/n1_equiv.py" extract "${POS[@]}" -o "$STORE_O" --host-class "$HOST_CLASS" --label "$BED"
  exit $? ;;

verdict)
  [ -n "$BED" ] && [ ${#VREF[@]} -gt 0 ] && [ ${#VCAND[@]} -gt 0 ] || { echo "usage: verdict --bed B --ref PATH... --cand PATH... [--out PREFIX]" >&2; exit 2; }
  verdict "$BED" "${OUT:-/tmp/n1_equiv_verdict}" "${VREF[@]}" -- "${VCAND[@]}"; exit $? ;;

replay)
  need_bins
  [ -n "$OUT" ] || OUT=$ROOT/replays/n1equiv_replay_$(date +%Y%m%d_%H%M%S)
  host_free
  if [ $DRY = 1 ]; then say "(dry-run) would require: pgrep -u $(id -un) -x nr-uesoftmodem|nr-softmodem empty and flock -n $O/locks/heavy.lock free"
  else flock -n "$O/locks/heavy.lock" true || { echo "heavy.lock is held: not starting" >&2; exit 3; }; fi
  say "replay arm: $N run(s) per arm, capture $CAP, ${DUR}s at speed $SPEED, out $OUT, host class $HOST_CLASS"
  run mkdir -p "$OUT"
  src() { python3 -I -c "import json,sys;d=json.load(open(sys.argv[2]+'/capture_info.json'));print('%s=%s,%d,%d,%d,%d,%d,2'%(sys.argv[1],sys.argv[2],d['samples_per_channel'],d['first_sample_tick'],d['channels'],d['sample_rate_hz'],d['rf_channels'][0]['frequency_hz']))" "$1" "$2"; }
  replay_arm() { # <ref|cand> <run index>
    local arm=$1 i=$2 B; B=$(bin_of "$arm"); local R=$OUT/${arm}_r$i
    local PUB=$B/mpci_file_pub; [ -x "$PUB" ] || PUB=$CANDB/mpci_file_pub
    say "== $arm run $i ($B) -> $R"
    run mkdir -p "$R"
    if [ $DRY = 1 ]; then
      echo "  + write $R/cells.json: cell id pci415 only, from $HERE/inventory/live_tuned_20261023.json (receiver-side detections; no gNB log or config is an input)"
      echo "  + write $R/replay.cfg: $CFG + device shm_iq + loader shm_iq shlibpath=$B"
      echo "  + env from $ENVFILE minus ISAC_USRP_*/ISAC_UHD_*/ISAC_RX_RECOVER*/ISAC_*_CORE(S)=, CUDA_MPS_PIPE_DIRECTORY=/tmp/nicola_nomps"
      echo "  + (subshell) flock -s /tmp/td_measure.lock; nice -n 10 $PUB $SPEED $((DUR + 60)) nrcell_pci415=$CAP,<geometry from capture_info.json> > $R/pub.log &"
      echo "  + cd $R; env ISAC_MLOCK=0 LD_LIBRARY_PATH=$B:/usr/local/lib ISAC_RX_MRC_MODE=1 ISAC_SCAN_ANT=1 <env file> /usr/bin/time -v -o $R/time.txt \\"
      echo "      timeout --signal=INT -k 60 $DUR nice -n 10 $B/nr-uesoftmodem -O $R/replay.cfg --passive-rx -r 217 --numerology 1 --band 78 -C 3680010000 --ssb 1171 \\"
      echo "      --ue-rxgain 38 --ue-nb-ant-rx 4 --ue-nb-ant-tx 4 --initial-fo -4827 --ue-fo-compensation --usrp-args nrcell_pci415 --thread-pool -1 --time-sync-I 0.01 \\"
      echo "      --ntn-initial-time-drift 0 -A 90 < /dev/null > $R/run.log 2>&1 ; then SIGINT the publisher"
      return 0
    fi
    host_free
    python3 -I - "$HERE/inventory/live_tuned_20261023.json" "$R/cells.json" <<'PY'
import json, os, sys
d = json.load(open(sys.argv[1])); d['cells'] = [c for c in d['cells'] if c['id'] == 'pci415']; d.pop('_comment', None)
tmp = sys.argv[2] + '.tmp'; json.dump(d, open(tmp, 'w'), indent=1); os.rename(tmp, sys.argv[2])
PY
    { cat "$CFG"; echo 'device = { name = "shm_iq"; };'; echo "loader = { shm_iq = { shlibpath = \"$B\"; }; };"; } > "$R/replay.cfg"
    local base=() envs kv
    read -r -a envs < "$ENVFILE"
    for kv in "${envs[@]}"; do case "$kv" in ISAC_USRP_*|ISAC_UHD_*|ISAC_RX_RECOVER*|ISAC_*_CORES=*|ISAC_*_CORE=*) ;; *) base+=("$kv") ;; esac; done
    export CUDA_MPS_PIPE_DIRECTORY=/tmp/nicola_nomps CUDA_MPS_LOG_DIRECTORY=/tmp/nicola_nomps
    (
      exec 9>/tmp/td_measure.lock; flock -s 9
      nice -n 10 "$PUB" "$SPEED" "$((DUR + 60))" "$(src nrcell_pci415 "$CAP")" > "$R/pub.log" 2>&1 &
      P=$!; trap 'kill -INT $P 2>/dev/null || true' EXIT
      for _ in $(seq 1 100); do grep -q "ring=/nrcell_pci415" "$R/pub.log" 2>/dev/null && break; sleep 0.2; done
      cd "$R" || exit 1
      env ISAC_MLOCK=0 LD_LIBRARY_PATH="$B:/usr/local/lib" ISAC_RX_MRC_MODE=1 ISAC_SCAN_ANT=1 "${base[@]}" \
        /usr/bin/time -v -o "$R/time.txt" timeout --signal=INT -k 60 "$DUR" nice -n 10 "$B/nr-uesoftmodem" -O "$R/replay.cfg" --passive-rx -r 217 --numerology 1 --band 78 \
        -C 3680010000 --ssb 1171 --ue-rxgain 38 --ue-nb-ant-rx 4 --ue-nb-ant-tx 4 --initial-fo -4827 --ue-fo-compensation \
        --usrp-args nrcell_pci415 --thread-pool -1 --time-sync-I 0.01 --ntn-initial-time-drift 0 -A 90 < /dev/null > "$R/run.log" 2>&1
      echo "rc=$?" > "$R/rc"
      kill -INT $P 2>/dev/null; wait $P 2>/dev/null
    )
    for _ in $(seq 1 60); do pgrep -u "$(id -un)" -x nr-uesoftmodem >/dev/null || break; sleep 1; done
    say "$arm run $i: $(cat "$R/rc" 2>/dev/null) log lines $(wc -l < "$R/run.log" 2>/dev/null)"
  }
  RD=(); CD=()
  for i in $(seq 1 "$N"); do
    for arm in $(order "$i"); do skip_ref "$arm" || replay_arm "$arm" "$i"; done
    [ -n "$REFB" ] && RD+=("$OUT/ref_r$i"); CD+=("$OUT/cand_r$i")
  done
  [ -n "$REFJ" ] && RD+=("$REFJ")
  verdict replay "$OUT/verdict_replay" "${RD[@]}" -- "${CD[@]}"; rc=$?
  say "verdict: $OUT/verdict_replay.txt (rc $rc)"; exit $rc ;;

rfsim)
  need_bins
  [ -n "$OUT" ] || OUT=/tmp/n1_equiv_rfsim_$(date +%Y%m%d_%H%M%S)
  host_free
  say "rfsim arm: $N run(s) per arm of ${SECS}s (4 RX, 106 PRB, merged-main defaults), out $OUT, host class $HOST_CLASS, idle host under flock -x /tmp/td_measure.lock"
  run mkdir -p "$OUT"
  RD=(); CD=()
  for i in $(seq 1 "$N"); do
    for arm in $(order "$i"); do
      skip_ref "$arm" && continue
      B=$(bin_of "$arm"); R=$OUT/${arm}_r$i
      say "== $arm run $i ($B) -> $R"
      # one rfsim_regress run; BUILD selects the gNB and receiver binaries of the arm, its gate result is kept as gate_rc
      if [ $DRY = 1 ]; then
        echo "  + env BUILD=$B GATE_SECS=$SECS OUT=$R flock -x /tmp/td_measure.lock $ROOT/tests/passive_rx/dgx/rfsim_regress.sh 1 > $R.regress.log 2>&1 ; echo \$? > $R/gate_rc"
      else
        host_free
        mkdir -p "$R"
        env BUILD="$B" GATE_SECS="$SECS" OUT="$R" flock -x /tmp/td_measure.lock "$ROOT/tests/passive_rx/dgx/rfsim_regress.sh" 1 > "$R.regress.log" 2>&1
        echo $? > "$R/gate_rc"
        say "$arm run $i: gate_rc $(cat "$R/gate_rc")"
      fi
    done
    [ -n "$REFB" ] && RD+=("$OUT/ref_r$i"); CD+=("$OUT/cand_r$i")
  done
  [ -n "$REFJ" ] && RD+=("$REFJ")
  verdict rfsim "$OUT/verdict_rfsim" "${RD[@]}" -- "${CD[@]}"; rc=$?
  say "verdict: $OUT/verdict_rfsim.txt (rc $rc)"; exit $rc ;;

*)
  sed -n '2,22p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
esac
