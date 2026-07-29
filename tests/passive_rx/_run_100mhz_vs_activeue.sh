#!/usr/bin/env bash
# Long, DETACHED comparison batch (2026-07-28): full-passive 100 MHz vs attached-UE 100 MHz.
#
# WHY THIS EXISTS: every earlier full-passive-vs-attached-UE comparison in
# PHASE3_BLIND_PDCCH_LIVE_WIRING_HANDOVER.md was explicitly NOT apples-to-apples -- the full-passive
# harness ran at 20 MHz/106 PRB while the attached-UE (tests/sensing_sim) numbers were taken at
# 100 MHz/273 PRB, so bandwidth and receiver-mode were confounded. Now that tests/passive_rx has a
# working 100 MHz mode, both sides can run at the SAME bandwidth, and this batch runs them
# back-to-back on the same machine so the only remaining differences are receiver mode
# (never-attach/blind-PDCCH vs attached/RRC-configured) and scene.
#
# STILL NOT FULLY CONTROLLED, state this whenever quoting the output:
#   - Different SCENES. tests/passive_rx injects its own single slow target per receiver;
#     tests/sensing_sim's "crossing" scene has its own (different, faster, multi-)targets. The
#     TOL=15m confirmed-track precision METRIC is computed the same way on both sides, but the
#     underlying detection problems are not identical in difficulty.
#   - Different reference-signal availability. Passive: cell CSI-RS (80 ms period) + blind-PDCCH
#     PDSCH-DMRS. Attached: RRC-configured CSI-RS + own PDSCH DM-RS, no RNTI-guessing noise floor.
#   - Different cpi_slots (passive 32, sensing_sim 128 as forced by its own runners).
# So: read this as "how far apart are the two receiver modes at equal bandwidth", NOT as a
# controlled single-variable experiment.
#
# SEQUENTIAL BY DESIGN: every arm saturates several cores (gNB + 1 active UE + N receivers + iperf),
# and this session already measured cross-receiver CPI-alignment degrading measurably under CPU
# contention. Running arms concurrently would corrupt exactly the numbers being measured.
#
# Usage (detached, survives SSH disconnect):
#   setsid nohup ./_run_100mhz_vs_activeue.sh > /tmp/cmp100/batch.log 2>&1 < /dev/null &
set -u

# SIZING, measured not guessed (2026-07-28 smoke test): at 273 PRB the rfsimulator advances
# SIMULATED time far slower than wall time -- gNB + active UE + N passive receivers all pushing
# 122.88 Msps on 12 cores gave ~2 CPIs (of 2.51 s each) per 90 s of WALL time, i.e. roughly 5-6 %
# of real time. A CPI needs 32 CSI-RS occurrences x 80 ms = 2.56 s of SIMULATED time, so useful
# CPI counts need long wall-clock arms: 1800 s wall ~ 25-30 CPIs/receiver at 2 receivers, fewer at
# 3 (more processes -> slower still). This is why the arms are 30 min each and the whole batch is
# ~2 h. Do NOT shorten these expecting the 106-PRB CPI rates -- that harness runs much closer to
# real time because it is 4x lighter per sample.
DUR_PASSIVE="${DUR_PASSIVE:-1800}"   # 30 min per passive arm
DUR_ACTIVE="${DUR_ACTIVE:-900}"      # 15 min for the attached-UE capture (all arms scored offline;
                                     # sensing_sim is denser per simulated second -- pdsch_data fires
                                     # every scheduled slot, not on an 80 ms CSI-RS period)
IPERF="${IPERF:-6M}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SIMDIR="$SCRIPT_DIR/../sensing_sim"
OUT=/tmp/cmp100
RESULTS="$OUT/RESULTS.txt"
mkdir -p "$OUT"

say() { echo "[$(date +%H:%M:%S)] $*" | tee -a "$RESULTS"; }

cool() { # let the machine settle + make sure nothing is left running between arms
  sudo -n pkill -9 -f "passive_rx/ue.active.conf"  2>/dev/null || true
  pkill      -9 -f "passive_rx/ue.passive"         2>/dev/null || true
  pkill      -9 -f "passive_rx/gnb.sa.rfsim"       2>/dev/null || true
  sudo -n pkill -9 -f "sensing_sim"                2>/dev/null || true
  for n in 2 3 4; do
    sudo -n ip netns del oai_isac_gnb$n 2>/dev/null || true
    sudo -n ip netns del oai_isac_ue$n  2>/dev/null || true
  done
  sleep 20
}

: > "$RESULTS"
say "=== 100 MHz full-passive vs attached-UE batch ==="
say "passive arms ${DUR_PASSIVE}s each, attached-UE capture ${DUR_ACTIVE}s, iperf $IPERF"
say "host: $(nproc) cores, load $(cut -d' ' -f1-3 /proc/loadavg)"
say ""

# ---------------------------------------------------------------- full-passive arms (100 MHz)
# Each arm's own per-receiver report JSONLs live at hardcoded /tmp/passive_rx/*_100m.jsonl and are
# CLEARED at the start of every run_passive_rx.sh invocation (deliberate -- see that script), so
# they are copied out into this arm's directory immediately after the arm finishes.
run_passive () { # run_passive <label> <num_rx> <rx1_nant>
  local label="$1" nrx="$2" nant="$3"
  local d="$OUT/$label"
  mkdir -p "$d"
  say "--- ARM $label: full-passive 100MHz, ${nrx}rx, rx1_nant=$nant, ${DUR_PASSIVE}s ---"
  cool
  BW100=1 NUM_RX="$nrx" RX1_NANT="$nant" IPERF_RATE="$IPERF" \
    "$SCRIPT_DIR/run_passive_rx.sh" "$DUR_PASSIVE" "$d" > "$d/run.log" 2>&1
  cp /tmp/passive_rx/passive_reports*_100m.jsonl "$d/" 2>/dev/null || true
  # Headline numbers straight out of the runner's own summary (single source of truth).
  grep -E "^  score:|fused tracks:|confirmed-status|merge: (GROUPS|PAIRED)|DetectionReports:|detections carrying azimuth|blind PDCCH:" \
    "$d/run.log" 2>/dev/null | sed 's/^/    /' | tee -a "$RESULTS"
  say ""
}

run_passive passive_2rx        2 1
run_passive passive_3rx        3 1
run_passive passive_2rx_aoa    2 4

# ---------------------------------------------------------------- attached-UE arm (100 MHz)
# _run_aoa_vs_3rx.sh does ONE simultaneous 3-receiver attached-UE capture and then scores EVERY
# fusion arm (2rx/3rx, with/without AoA, gated/ungated) offline from that same capture -- exactly the
# arm structure wanted here, and its offline-scoring design means its internal arms are mutually
# controlled even though this batch's passive-vs-attached comparison is not.
say "--- ARM attached_ue: tests/sensing_sim 3-receiver capture, 100MHz, ${DUR_ACTIVE}s ---"
cool
mkdir -p "$OUT/attached_ue"
( cd "$SIMDIR" && sudo -n ./_run_aoa_vs_3rx.sh "$DUR_ACTIVE" crossing 1 ) \
  > "$OUT/attached_ue/run.log" 2>&1
grep -E "precision|tracks|reports|median|bearing" "$OUT/attached_ue/run.log" 2>/dev/null \
  | sed 's/^/    /' | tee -a "$RESULTS"
say ""

cool
say "=== BATCH COMPLETE ==="
say "full log per arm: $OUT/<arm>/run.log ; this summary: $RESULTS"
