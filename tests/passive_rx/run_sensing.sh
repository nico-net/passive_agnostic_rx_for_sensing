#!/bin/bash
# tests/passive_rx/run_sensing.sh -- ONE command: preflight, 30 s radio wait, passive receiver + sensing
# engine (in-process), realtime chain, monitor; supervised; verdict at the end. Spec §12.
set -uo pipefail
W=$(cd "$(dirname "$0")/../.." && pwd)
DUR=600 RXG=43 MGMT=192.168.1.140 SURVEY=$W/tests/passive_rx/ota/survey.json DEBUG= PORT=8080 RUN=/home/sens/NICOLA/sensing_runs/$(date +%Y%m%d_%H%M%S)
while [ $# -gt 0 ]; do case $1 in
  --dur) DUR=$2; shift;; --rxg) RXG=$2; shift;; --survey) SURVEY=$2; shift;; --debug) DEBUG=1;;
  --port) PORT=$2; shift;; --mgmt) MGMT=$2; shift;; --run-dir) RUN=$2; shift;; *) echo "unknown arg $1"; exit 2;; esac; shift; done
BUILD=${BUILD:-$W/cmake_targets/ran_build/build_sense}
# Core map: see the CORE MAP comment in ota/sensing_ota.conf.template (the owner of every core).
SENSE_CPUS=${SENSE_CPUS:-11,12,13} CHAIN_CPU=${CHAIN_CPU:-4} MON_CPU=${MON_CPU:-4} DISC_CPUS=${DISC_CPUS:-11,12,13}
DISC=$W/tests/passive_rx/discovery
mkdir -p "$RUN" && cd "$RUN" || exit 2
log(){ echo "[run_sensing $(date +%T)] $*" | tee -a "$RUN/launcher.log"; }

# ---- preflight (refuse on any failure) ----
pgrep -x nr-uesoftmodem >/dev/null && { log "ABORT: a receiver is already running"; exit 3; }
pgrep -f 'ninja|cmake --build|make -j' >/dev/null && { log "ABORT: a build is running (never build during a capture)"; exit 3; }
ping -c1 -W2 192.168.20.2 >/dev/null || { log "ABORT: X410 data plane 192.168.20.2 unreachable"; exit 3; }
# uhd_find_devices reports the data port as mgmt_addr, so run_arm's auto-derived MGMT is wrong: pass it.
ping -c1 -W2 "$MGMT" >/dev/null || { log "ABORT: X410 management address $MGMT unreachable (--mgmt ADDR)"; exit 3; }
timeout 20 uhd_find_devices --args "type=x4xx,addr=192.168.20.2" 2>/dev/null | grep -q x4xx || { log "ABORT: uhd_find_devices does not see the X410"; exit 3; }
grep -qv performance /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor 2>/dev/null && \
  { log "ABORT: CPU governor not 'performance' (RT budget assumes it): sudo cpupower frequency-set -g performance"; exit 3; }
[ -x "$BUILD/nr-uesoftmodem" ] || { log "ABORT: no sensing build at $BUILD"; exit 3; }
grep -qa 'SENSING_GATE open' "$BUILD/nr-uesoftmodem" || { log "ABORT: binary lacks the sensing gate (stale build?)"; exit 3; }
python3 "$W/tests/passive_rx/survey.py" "$SURVEY" --geometry "$RUN/geometry.json" \
  --apply "$W/tests/passive_rx/ota/sensing_ota.conf.template" "$RUN/ue.conf" --report-path "$RUN/reports.jsonl" \
  || { log "ABORT: survey rejected (see above)"; exit 3; }
# Dedicated-CORESET discovery hand-off: a previous run's result must never be applied (agnostic rule).
sudo rm -f /tmp/coresets_discovered.txt /tmp/passive_rx/idsweep_*.bin
if [ ! -x "$DISC/idsweep_offline_gpu" ] || [ -n "$(find "$DISC" -maxdepth 1 \( -name '*.c' -o -name '*.cu' \) -newer "$DISC/idsweep_offline_gpu")" ]; then
  log "building idsweep_offline_gpu"; bash "$DISC/build.sh" >> "$RUN/launcher.log" 2>&1 || { log "ABORT: idsweep_offline_gpu build failed"; exit 3; }
fi
log "preflight ok; run dir $RUN"

# ---- 30 s radio wait (run_arm itself waits 180 s more if it has to restart MPM) ----
log "waiting 30 s for the radio"; sleep 30

ARM_PID= CHAIN_PID= MON_PID= DISC_PID= RL=
stop_all(){ log "stopping"; sudo pkill -TERM -x nr-uesoftmodem 2>/dev/null; [ -n "$ARM_PID" ] && wait $ARM_PID 2>/dev/null
  sleep 3; kill $CHAIN_PID $MON_PID $DISC_PID 2>/dev/null; }
trap 'stop_all; [ -n "$RL" ] && type verdict >/dev/null 2>&1 && verdict; exit 130' INT TERM

# ---- receiver (+ in-process engine) through the qualified run_arm harness ----
export NR_ISAC_CPUS=$SENSE_CPUS NR_ISAC_REQUIRE_CUDA=1
[ -n "$DEBUG" ] && { mkdir -p "$RUN/debug"; export NR_ISAC_DEBUG_DIR=$RUN/debug; }
( REPO=$W BIN=$BUILD/nr-uesoftmodem ARM=sense CONF=$RUN/ue.conf DUR=$DUR TRIES=${TRIES:-3} RXG=$RXG NANT=4 MGMT=$MGMT \
  SCAN=1 PRB=273 CARRIER=3450000000 INITIALFO=0 \
  XENV="NR_ISAC_CPUS=$SENSE_CPUS NR_ISAC_REQUIRE_CUDA=1 ISAC_COREMAP_IDSWEEP=12 ${NR_ISAC_DEBUG_DIR:+NR_ISAC_DEBUG_DIR=$NR_ISAC_DEBUG_DIR}" \
  bash "$W/tests/passive_rx/captures/run_arm.sh" ) > "$RUN/run_arm.out" 2>&1 &
ARM_PID=$!
# run_arm writes captures under its BASE and retries (TRIES, SIB1 ADAPT) into a NEW dir each time:
# the newest run.log newer than ue.conf is the live try, re-resolved every supervision tick.
newest_rl(){ local r; r=$(ls -1t /home/sens/NICOLA/captures/sense_*/run.log 2>/dev/null | head -1)
  [ -n "$r" ] && [ "$r" -nt "$RUN/ue.conf" ] && echo "$r"; }
for _ in $(seq 1 600); do RL=$(newest_rl); [ -n "$RL" ] && break; sleep 1; done
[ -n "${RL:-}" ] || { log "ABORT: receiver never produced a run.log"; kill $ARM_PID; stop_all; exit 4; }
TRY=1

start_discovery(){ kill $DISC_PID 2>/dev/null; sudo rm -f /tmp/coresets_discovered.txt /tmp/passive_rx/idsweep_*.bin
  N=10 DISC_CPUS=$DISC_CPUS OUT=$RUN bash "$DISC/discover_live.sh" >> "$RUN/discovery.log" 2>&1 &
  DISC_PID=$!; DISC_T0=$(date +%s); DISC_WARNED=; }
start_tail(){
taskset -c $CHAIN_CPU python3 "$W/openair1/PHY/NR_UE_ISAC/tools/realtime_chain.py" --follow "$RUN/reports.jsonl" \
  --geometry "$RUN/geometry.json" --out "$RUN/tracks.jsonl" --status "$RUN/status.jsonl" \
  ${DEBUG:+--debug-dir "$RUN/debug/chain"} > "$RUN/chain.out" 2>&1 &
CHAIN_PID=$!
taskset -c $MON_CPU python3 "$W/tests/passive_rx/monitor/monitor.py" --port "$PORT" --reports "$RUN/reports.jsonl" \
  --tracks "$RUN/tracks.jsonl" --status "$RUN/status.jsonl" --geometry "$RUN/geometry.json" --log "$RL" > "$RUN/monitor.out" 2>&1 &
MON_PID=$!
}
ln -sfn "$(dirname "$RL")" "$RUN/capture"; log "receiver log $RL"
start_discovery; start_tail
log "monitor: ssh -L $PORT:localhost:$PORT sens6  then  http://localhost:$PORT/"

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
new_drops = ("abi_rejections", "sessionless_ul_rejections", "rejected_submissions", "nonviable_rows",
             "consume_failures", "ul_sessions_capped")
for k in new_drops: v[k] = last.get(k)
dl = open(f"{run}/discovery.log", errors="replace").read() if os.path.exists(f"{run}/discovery.log") else ""
v["discovery"] = [l for l in dl.splitlines() if "HANDOFF" in l or "SUMMARY" in l or l.startswith("DISCOVERY")]
v["discovered_coreset_applied"] = log.count("from decode-free discovery") + log.count("is already a verified bank entry")
arm_ok = bool(v["arm_verdict"]) and "verdict=VALID" in v["arm_verdict"]
v["valid"] = bool(v["gate_opened"] and v["cpis"] > 0 and not v["cfo_mislock"] and arm_ok
                  and not (v["dropped_cpis"] or v["discarded_pending_rows"] or v["dropped_submissions"])
                  and not any(v[k] for k in new_drops))
json.dump(v, open(f"{run}/run_verdict.json", "w"), indent=1); print(json.dumps(v))
EOF
}

# ---- supervision: death AND live-but-silent ----
T0=$(date +%s); LAST_SIZE=0; LAST_GROWTH=$T0
while kill -0 $ARM_PID 2>/dev/null; do
  sleep 10; now=$(date +%s)
  r=$(newest_rl); if [ -n "$r" ] && [ "$r" != "$RL" ]; then
    # run_arm retried: the previous try's reports must not be scored with this one's.
    [ -f "$RUN/reports.jsonl" ] && mv "$RUN/reports.jsonl" "$RUN/reports.try$TRY.jsonl"
    TRY=$((TRY+1)); RL=$r; LAST_SIZE=0; LAST_GROWTH=$now
    ln -sfn "$(dirname "$RL")" "$RUN/capture"; log "run_arm try $TRY: receiver log $RL"
    kill $CHAIN_PID $MON_PID 2>/dev/null; start_discovery; start_tail
  fi
  [ -z "$DISC_WARNED" ] && [ $((now - DISC_T0)) -gt 120 ] && ! grep -qE "HANDOFF|SUMMARY" "$RUN/discovery.log" 2>/dev/null \
    && { log "WARN: no discovery HANDOFF/SUMMARY within 120 s (see discovery.log)"; DISC_WARNED=1; }
  size=$(stat -c%s "$RL" 2>/dev/null || echo 0)
  [ "$size" -gt "$LAST_SIZE" ] && { LAST_SIZE=$size; LAST_GROWTH=$now; }
  [ $((now - LAST_GROWTH)) -gt 60 ] && log "WARN: receiver log silent for $((now - LAST_GROWTH)) s"
  kill -0 $CHAIN_PID 2>/dev/null || log "WARN: realtime_chain died (see chain.out)"
  kill -0 $MON_PID 2>/dev/null || log "WARN: monitor died (see monitor.out)"
done
sleep 3; kill $CHAIN_PID $MON_PID $DISC_PID 2>/dev/null
verdict; log "done: $RUN"
