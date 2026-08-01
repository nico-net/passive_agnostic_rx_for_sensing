#!/bin/bash
# Heterogeneous passive fleet: ONE array receiver (X410 stand-in, 4 coherent channels -> AoA) plus
# ONE single-antenna receiver (B210 stand-in, range/Doppler only), at each available cell bandwidth.
#
# Answers: does an X410 + B210 pair actually fuse and localise in the FULL-PASSIVE path (never attach,
# never transmit), and what does the cell bandwidth buy?
#
#   rx1  4 antennas  -> reports azimuth        (X410 stand-in)
#   rx2  1 antenna   -> range/Doppler only     (B210 stand-in)
#
# Both arms run the SAME scene (ported verbatim into the 106 PRB confs) so bandwidth is the only
# difference. Note the base 106 PRB scene could NOT have been used: it is the 0.1 m/s target
# documented as undetectable (found in 1 of 54 CPIs, sits in the zero-Doppler notch).
#
# NOT simulable here, and the reason a 20 MHz arm is absent: rfsimulator requires every node to share
# a sample rate, and there is no 51 PRB gNB config -- so a narrowband receiver alongside a wideband
# one cannot be represented at all. Both receivers necessarily run at the cell bandwidth.
#
# Usage: sudo ./_run_bw_fleet.sh [dur_per_arm_s]
set -u
cd "$(dirname "${BASH_SOURCE[0]}")"
DUR=${1:-2400}
OUT=${OUT:-/tmp/bwfleet}
mkdir -p "$OUT"

run_arm() { # name  BW100
  local name=$1 bw=$2
  echo "############ ARM $name (BW100=$bw, ${DUR}s)  $(date +%H:%M:%S) ############"
  NUM_RX=2 RX1_NANT=4 RX2_NANT=1 CONF_TAG=".aoa" BW100="$bw" IPERF_RATE=6M \
    ./run_passive_rx.sh "$DUR" "$OUT/$name" > "$OUT/$name.log" 2>&1
  echo "  exit=$?  reports: rx1=$(wc -l < /tmp/passive_rx/passive_reports.jsonl 2>/dev/null || echo 0)" \
       "rx2=$(wc -l < /tmp/passive_rx/passive_reports_rx2.jsonl 2>/dev/null || echo 0)"
  # keep this arm's raw reports before the next arm wipes them
  cp -f /tmp/passive_rx/passive_reports.jsonl     "$OUT/${name}_rx1.jsonl" 2>/dev/null || true
  cp -f /tmp/passive_rx/passive_reports_rx2.jsonl "$OUT/${name}_rx2.jsonl" 2>/dev/null || true
  tail -25 "$OUT/$name.log"
}

run_arm bw40  0
run_arm bw100 1
echo "############ done $(date +%H:%M:%S) -> $OUT ############"
