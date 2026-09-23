#!/bin/bash
# tests/passive_rx/run_sensing.sh -- ONE command: preflight, 30 s radio wait, passive receiver + sensing
# engine (in-process), realtime chain, monitor; supervised; verdict at the end. Spec §12.
set -uo pipefail
W=$(cd "$(dirname "$0")/../.." && pwd)
DUR=600 RXG=43 SURVEY=$W/tests/passive_rx/ota/survey.json DEBUG= PORT=8080 RUN=/home/sens/NICOLA/sensing_runs/$(date +%Y%m%d_%H%M%S)
while [ $# -gt 0 ]; do case $1 in
  --dur) DUR=$2; shift;; --rxg) RXG=$2; shift;; --survey) SURVEY=$2; shift;; --debug) DEBUG=1;;
  --port) PORT=$2; shift;; --run-dir) RUN=$2; shift;; *) echo "unknown arg $1"; exit 2;; esac; shift; done
BUILD=${BUILD:-$W/cmake_targets/ran_build/build_sense}
SENSE_CPUS=${SENSE_CPUS:-3,12,13} CHAIN_CPU=${CHAIN_CPU:-11} MON_CPU=${MON_CPU:-10}
mkdir -p "$RUN" && cd "$RUN" || exit 2
log(){ echo "[run_sensing $(date +%T)] $*" | tee -a "$RUN/launcher.log"; }

# ---- preflight (refuse on any failure) ----
pgrep -x nr-uesoftmodem >/dev/null && { log "ABORT: a receiver is already running"; exit 3; }
pgrep -f 'ninja|cmake --build|make -j' >/dev/null && { log "ABORT: a build is running (never build during a capture)"; exit 3; }
ping -c1 -W2 192.168.20.2 >/dev/null || { log "ABORT: X410 data plane 192.168.20.2 unreachable"; exit 3; }
timeout 20 uhd_find_devices --args "type=x4xx,addr=192.168.20.2" 2>/dev/null | grep -q x4xx || { log "ABORT: uhd_find_devices does not see the X410"; exit 3; }
[ -x "$BUILD/nr-uesoftmodem" ] || { log "ABORT: no sensing build at $BUILD"; exit 3; }
strings "$BUILD/nr-uesoftmodem" | grep -q 'SENSING_GATE open' || { log "ABORT: binary lacks the sensing gate (stale build?)"; exit 3; }
python3 "$W/tests/passive_rx/survey.py" "$SURVEY" --geometry "$RUN/geometry.json" \
  --apply "$W/tests/passive_rx/ota/sensing_ota.conf.template" "$RUN/ue.conf" --report-path "$RUN/reports.jsonl" \
  || { log "ABORT: survey rejected (see above)"; exit 3; }
log "preflight ok; run dir $RUN"

# ---- 30 s radio wait (run_arm itself waits 180 s more if it has to restart MPM) ----
log "waiting 30 s for the radio"; sleep 30

# ---- receiver (+ in-process engine) through the qualified run_arm harness ----
export NR_ISAC_CPUS=$SENSE_CPUS NR_ISAC_REQUIRE_CUDA=1
[ -n "$DEBUG" ] && { mkdir -p "$RUN/debug"; export NR_ISAC_DEBUG_DIR=$RUN/debug; }
( REPO=$W BIN=$BUILD/nr-uesoftmodem ARM=sense CONF=$RUN/ue.conf DUR=$DUR TRIES=${TRIES:-3} RXG=$RXG NANT=4 \
  SCAN=1 PRB=273 CARRIER=3450000000 INITIALFO=0 \
  XENV="NR_ISAC_CPUS=$SENSE_CPUS NR_ISAC_REQUIRE_CUDA=1 ${NR_ISAC_DEBUG_DIR:+NR_ISAC_DEBUG_DIR=$NR_ISAC_DEBUG_DIR}" \
  bash "$W/tests/passive_rx/captures/run_arm.sh" ) > "$RUN/run_arm.out" 2>&1 &
ARM_PID=$!
# run_arm writes captures under its BASE; find this run's newest run.log once it appears
for _ in $(seq 1 600); do RL=$(ls -1t /home/sens/NICOLA/captures/sense_*/run.log 2>/dev/null | head -1); \
  [ -n "$RL" ] && [ "$RL" -nt "$RUN/ue.conf" ] && break; sleep 1; done
[ -n "${RL:-}" ] || { log "ABORT: receiver never produced a run.log"; kill $ARM_PID; exit 4; }
ln -sfn "$(dirname "$RL")" "$RUN/capture"; log "receiver log $RL"

# ---- realtime chain + monitor ----
taskset -c $CHAIN_CPU python3 "$W/openair1/PHY/NR_UE_ISAC/tools/realtime_chain.py" --follow "$RUN/reports.jsonl" \
  --geometry "$RUN/geometry.json" --out "$RUN/tracks.jsonl" --status "$RUN/status.jsonl" \
  ${DEBUG:+--debug-dir "$RUN/debug/chain"} > "$RUN/chain.out" 2>&1 &
CHAIN_PID=$!
taskset -c $MON_CPU python3 "$W/tests/passive_rx/monitor/monitor.py" --port "$PORT" --reports "$RUN/reports.jsonl" \
  --tracks "$RUN/tracks.jsonl" --status "$RUN/status.jsonl" --geometry "$RUN/geometry.json" --log "$RL" > "$RUN/monitor.out" 2>&1 &
MON_PID=$!
log "monitor: ssh -L $PORT:localhost:$PORT sens6  then  http://localhost:$PORT/"

stop_all(){ log "stopping"; sudo pkill -TERM -x nr-uesoftmodem 2>/dev/null; wait $ARM_PID 2>/dev/null
  sleep 3; kill $CHAIN_PID $MON_PID 2>/dev/null; }
trap 'stop_all; verdict; exit 130' INT TERM

verdict(){ python3 - "$RUN" "$RL" <<'EOF'
import json, os, re, sys
run, rl = sys.argv[1], sys.argv[2]
log = open(rl, errors="replace").read() if os.path.exists(rl) else ""
reps = [json.loads(l) for l in open(f"{run}/reports.jsonl")] if os.path.exists(f"{run}/reports.jsonl") else []
last = reps[-1] if reps else {}
v = {"cpis": len(reps), "gate_opened": "SENSING_GATE open" in log,
     "cuda": bool(re.search(r"CUDA.*(enabled|warm)", log)), "cfo_mislock": os.path.exists(os.path.join(os.path.dirname(rl), "cfo_mislock")),
     "dropped_cpis": last.get("dropped_cpis"), "discarded_pending_rows": last.get("discarded_pending_rows"),
     "dropped_submissions": last.get("dropped_submissions"),
     "arm_verdict": open(os.path.join(os.path.dirname(rl), "verdict.txt")).read().strip() if os.path.exists(os.path.join(os.path.dirname(rl), "verdict.txt")) else None}
v["valid"] = bool(v["gate_opened"] and v["cpis"] > 0 and not v["cfo_mislock"]
                  and not (v["dropped_cpis"] or v["discarded_pending_rows"] or v["dropped_submissions"]))
json.dump(v, open(f"{run}/run_verdict.json", "w"), indent=1); print(json.dumps(v))
EOF
}

# ---- supervision: death AND live-but-silent ----
T0=$(date +%s); LAST_SIZE=0; LAST_GROWTH=$T0
while kill -0 $ARM_PID 2>/dev/null; do
  sleep 10; now=$(date +%s); size=$(stat -c%s "$RL" 2>/dev/null || echo 0)
  [ "$size" -gt "$LAST_SIZE" ] && { LAST_SIZE=$size; LAST_GROWTH=$now; }
  [ $((now - LAST_GROWTH)) -gt 60 ] && log "WARN: receiver log silent for $((now - LAST_GROWTH)) s"
  kill -0 $CHAIN_PID 2>/dev/null || log "WARN: realtime_chain died (see chain.out)"
  kill -0 $MON_PID 2>/dev/null || log "WARN: monitor died (see monitor.out)"
done
sleep 3; kill $CHAIN_PID $MON_PID 2>/dev/null
verdict; log "done: $RUN"
