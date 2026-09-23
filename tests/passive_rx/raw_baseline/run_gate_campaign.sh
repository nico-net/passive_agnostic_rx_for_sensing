#!/bin/bash
# Offline acceptance-gate campaign. Strictly SEQUENTIAL: one receiver at a time, and never while a
# build or another nr-uesoftmodem is running (CPU contention produces false RXDISCONT/faults).
# Every stage appends one JSON line to $SUM; nothing is judged by eye.
set -u
REPO=/home/sens/NICOLA/adaptive-rx-UL-DL
RB=$REPO/tests/passive_rx/raw_baseline
BUILD=$REPO/cmake_targets/ran_build/build
CAP=/home/sens/NICOLA/captures
STAMP=$(date -u +%Y%m%dT%H%M%SZ)
ROOT=${CAMPAIGN_ROOT:-$CAP/campaign_$STAMP}
SUM=$ROOT/summary.jsonl
mkdir -p "$ROOT"
echo "$ROOT" > /tmp/campaign_root.txt

guard() {
  while pgrep -x nr-uesoftmodem >/dev/null || pgrep -f "make -k -j10 tests|cc1plus|cc1 " >/dev/null; do sleep 10; done
}
emit() { echo "$1" >> "$SUM"; echo "$1"; }

replay() { # name capture [extra args...]
  local name=$1 cap=$2; shift 2
  guard
  local out=$ROOT/$name
  python3 "$RB/run_raw_replay.py" "$cap" --out "$out" --speed 1.0 --timeout 420 --sample-shift 2 "$@" \
    > "$out.driver.log" 2>&1
  local rc=$?
  local acq="null"
  if [ -f "$out/result.json" ]; then
    python3 "$RB/validate_raw_acquisition.py" "$out" > "$out.acquisition.json" 2> "$out.acquisition.err"
    local arc=$?
    acq=$(python3 -c "
import json,sys
try: d=json.load(open('$out.acquisition.json')); print(json.dumps(d.get('status')))
except Exception: print(json.dumps('FAIL_RAW_ACQUISITION rc=$arc: '+open('$out.acquisition.err').read().strip()[:200]))")
  fi
  emit "$(python3 -c "
import json,re
r={}
try: r=json.load(open('$out/result.json'))
except Exception as e: r={'status':'NO_RESULT','error':str(e)}
log=open('$out/receiver.log',errors='replace').read() if __import__('os').path.exists('$out/receiver.log') else ''
tr=re.findall(r'ACQ_STATE (\w+) -> (\w+)',log)
print(json.dumps({'stage':'$name','capture':'$cap','driver_rc':$rc,
  'status':r.get('status'),'eof':r.get('eof'),'faults':len(r.get('faults',[]) or []),
  'gate4_acquisition':$acq,'gate5':r.get('acceptance_gate_5'),
  'acq_transitions':[f'{a}->{b}' for a,b in tr],'final_state':tr[-1][1] if tr else None}))")"
}

oracle() {
  guard
  local out=$ROOT/gate1_manual_oracle
  local input=/tmp/agnostic-dci10-validation/replay-input.bin
  ( cd "$REPO" && timeout 60s nice -n 10 env -u ISAC_PASSIVE_REPLAY_CAPTURE -u ISAC_PASSIVE_REPLAY_UL_PROBE \
      -u ISAC_PASSIVE_REPLAY_UL_CONFIG ISAC_PASSIVE_REPLAY_INPUT="$input" ISAC_SYNC_ONLY=1 \
      LD_LIBRARY_PATH="$BUILD:${LD_LIBRARY_PATH:-}" "$BUILD/nr-uesoftmodem" --passive-rx --sa \
      -r 273 --numerology 1 --band 78 -C 3450000000 --ssb 150 \
      --ue-nb-ant-rx 4 --ue-nb-ant-tx 4 --thread-pool -1 ) > "$out.log" 2>&1
  local rc=$?
  emit "$(python3 -c "
import json,re
log=open('$out.log',errors='replace').read()
m=re.search(r'REPLAY PASS: identical DL controls=(\d+) failed=(\d+) raw UL=(\d+)',log)
v=re.findall(r'REPLAY VOID[^\n]*',log)
print(json.dumps({'stage':'gate1_manual_oracle','rc':$rc,
  'identical_dl':int(m.group(1)) if m else None,'failed':int(m.group(2)) if m else None,
  'raw_ul':int(m.group(3)) if m else None,'void':v[:3],
  'reference':{'identical_dl':45,'failed':0,'raw_ul':15}}))")"
}

# Gate 1: saved-IQ manual decoder oracle (reference = PROGRESS.md figure for THIS input: 45/0/15).
oracle
# Gate 4: autonomous broadcast acquisition on every capture that still has IQ on disk.
replay gate4_window_1      $CAP/raw_batch.vKkQwC/window_1
replay gate4_window_2      $CAP/raw_batch.vKkQwC/window_2
replay gate4_fullband_4s   $CAP/raw_fullband_4s.p67kVf
replay gate4_capture_2s    $CAP/raw_long_5min.mOLDO7/capture_2s
# Gate 5 variants on the long capture (the 3 s @ 60 s case already passed; these probe its edges).
replay gate5_early_gap_5s  $CAP/raw_5min_last120.v51DBe/capture_120s --gap-at-s 5  --gap-s 3
replay gate5_long_gap_10s  $CAP/raw_5min_last120.v51DBe/capture_120s --gap-at-s 60 --gap-s 10
replay gate5_tiny_gap_50ms $CAP/raw_5min_last120.v51DBe/capture_120s --gap-at-s 60 --gap-s 0.05
echo CAMPAIGN_DONE >> "$SUM"
echo CAMPAIGN_DONE
