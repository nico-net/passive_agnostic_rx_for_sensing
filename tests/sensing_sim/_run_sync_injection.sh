#!/bin/bash
# Validate the Phase 1-3 sync stack against a KNOWN injected receiver-clock error.
#
# WHY. rfsimulator runs both ends on one machine clock, so this harness natively presents almost no
# STO/CFO/SFO (measured 2026-07-27 on the 4-object scene: CFO rms 0.42 Hz, STO drift 0.12 bins/CPI,
# SFO correction withheld on 100% of CPIs by its own linearity gate for lack of any real drift). A
# sync on/off A/B therefore measures "correcting nothing changes nothing" and can neither validate
# nor condemn the estimator. `sensing_channel_set_rx_clock()` gives it something real to find; this
# script sweeps operating points and scores recovery against ground truth.
#
# Single receiver (rx1) on purpose: this is a per-receiver DSP question, needs no fusion, ~3x cheaper.
# sync_correction MUST be on (the scene confs default to it since 2026-07-27) or there are no
# estimates to score.
#
# Sizing note that matters: the SFO delay ramp is sawtooth-wrapped at WRAP samples because the CIR is
# capped at 255 taps (uint8_t). At 122.88 Msps a 0.5 ppm ramp advances ~61 samples/s, so WRAP=64
# wraps about once a second, i.e. roughly one CPI in ten contains a step and will (correctly) fail
# Phase 3's linearity gate. Expect ~90% clean, not 100%.
#
# CPI length matters just as much and was the likely culprit in the FIRST run of this sweep
# (2026-07-27, cpi_slots=128 = 64ms CPI): 0.5 ppm drift over 64ms is only ~2 samples of ramp, which
# a per-row noise floor can plausibly swamp before the linearity gate ever sees it -- the gate
# reporting 0/CPIs passed could be "correctly rejecting noise", not "broken". CPI_SLOTS defaults to
# 512 (256ms) here specifically to give the ramp more room over noise; note this INCREASES how often
# the sawtooth wrap falls inside a single CPI (a real, unavoidable trade-off, not tuned away) --
# read the wrap-crossing rate implied by WRAP/CPI_SLOTS/the injected ppm before trusting a 0%-pass
# result as evidence the gate itself is broken, rather than as noise or wrap-crossing artifacts.
#
# Usage: sudo setsid ./_run_sync_injection.sh [dur_s] [wrap_samples] [cpi_slots] > /tmp/ms/syncinj.log 2>&1
set -u
cd "$(dirname "${BASH_SOURCE[0]}")"
DUR=${1:-200}
WRAP=${2:-64}
export CPI_SLOTS=${3:-512}
ROOT=/tmp/ms/sync_inj
BW=98280000     # 273 PRB x 12 x 30 kHz occupied bandwidth
FS=122880000    # RF sample rate at 273 PRB
rm -rf "$ROOT"; mkdir -p "$ROOT"
RES=$ROOT/results.csv
echo "point,sto_samples,cfo_hz_inj,sfo_ppm_inj,cpis,cfo_hz_est,sfo_ppm_est,locked,flywheel,sto_corr,sfo_corr,pass" > "$RES"

# name : sto_us : cfo_hz : sfo_ppm
#  - "control" reproduces the current near-zero baseline and is the reference every other row is read
#    against; without it a small recovered value cannot be told from a small native impairment.
#  - CFO is kept well inside cell-search tolerance: the point is to exceed the 0.42 Hz native noise
#    floor by orders of magnitude, not to break the UE's attach.
#  - 2 ppm is a realistic cheap-TCXO worst case and doubles as the stress point.
POINTS=(
  "control:0:0:0"
  "cfo50:0:50:0"
  "sfo0p5:0:0:0.5"
  "sfo2:0:0:2.0"
  "combined:1.0:50:0.5"
)

for spec in "${POINTS[@]}"; do
  IFS=: read -r name sto cfo sfo <<< "$spec"
  OUT=$ROOT/$name
  echo ""; echo "######## $name  sto=${sto}us cfo=${cfo}Hz sfo=${sfo}ppm  ($(date +%H:%M:%S)) ########"
  for n in 2 3 4; do ip netns del oai_isac_gnb$n 2>/dev/null; ip netns del oai_isac_ue$n 2>/dev/null; done

  # Inject into the FIRST (sensing_channel) block only -- `rx_array_boresight_deg` also appears in the
  # [sensing] block further down, and patching that one would silently do nothing.
  c="_syncinj_run.conf"
  python3 - "$c" "$sto" "$cfo" "$sfo" "$WRAP" <<'PY'
import re, sys
dst, sto, cfo, sfo, wrap = sys.argv[1:6]
s = open("_scene_four_rx1.conf").read()
i = s.index("sensing_channel = {")
j = s.index("rx_array_boresight_deg", i)
j = s.index("\n", j) + 1
inject = (f"  rx_sto_us           = {sto};\n"
          f"  rx_cfo_hz           = {cfo};\n"
          f"  rx_sfo_ppm          = {sfo};\n"
          f"  rx_sfo_wrap_samples = {wrap};\n")
open(dst, "w").write(s[:j] + inject + s[j:])
PY
  grep -q "rx_sfo_ppm          = $sfo;" "$c" || { echo "FAILED to inject into $c"; exit 1; }

  UE_NB_ANT_RX=4 ./_run_mot_variant.sh "$c" "$OUT" "$DUR" >"$ROOT/$name.log" 2>&1
  rm -f "$c"

  echo "--- injected truth seen by the channel:"
  grep -m2 "SENSING_CHANNEL clk:" "$OUT/logs/ue.log" 2>/dev/null || echo "    (none -- injection did not reach the channel)"
  python3 score_sync_injection.py "$OUT" --bw-hz $BW --fs-hz $FS --csv "$name" 2>>"$RES"

  pkill -9 -f "nr-.*softmodem" 2>/dev/null; pkill -9 -f iperf3 2>/dev/null; sleep 3
done

echo ""; echo "######## SYNC INJECTION SWEEP COMPLETE ########"
column -s, -t "$RES"
